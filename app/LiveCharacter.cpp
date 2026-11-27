#include "LiveCharacter.hpp"

#include <lunar/net/Motion.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <utility>

namespace {

const char* stateName(lunar::Enum::HumanoidStateType state)
{
    using S = lunar::Enum::HumanoidStateType;
    switch (state) {
    case S::FallingDown: return "FallingDown";
    case S::Ragdoll: return "Ragdoll";
    case S::GettingUp: return "GettingUp";
    case S::Jumping: return "Jumping";
    case S::Swimming: return "Swimming";
    case S::Freefall: return "Freefall";
    case S::Flying: return "Flying";
    case S::Landed: return "Landed";
    case S::Running: return "Running";
    case S::RunningNoPhysics: return "RunningNoPhysics";
    case S::StrafingNoPhysics: return "StrafingNoPhysics";
    case S::Climbing: return "Climbing";
    case S::Seated: return "Seated";
    case S::PlatformStanding: return "PlatformStanding";
    case S::Dead: return "Dead";
    case S::Physics: return "Physics";
    case S::None: return "None";
    }
    return "?";
}

} // namespace

LiveCharacter::LiveCharacter(std::shared_ptr<lunar::net::Replica> replica, lunar::avatar::AnimationSystem& animation)
    : replica_(std::move(replica)), animation_(animation), sync_(replica_)
{
}

LiveCharacter::~LiveCharacter()
{
    replica_->simulatedHere = nullptr;
    controller_.reset(); // before the world it steps in
}

void LiveCharacter::attach(lunar::ClientSession& client)
{
    lunar::FrameHooks hooks = client.hooks();
    hooks.physics = [this](double dt) { step(dt); };
    client.setHooks(std::move(hooks));
    // The scripts' workspace:Raycast and friends (and the prompts' line of
    // sight) cast in the world we keep of the Workspace.
    ensureWorld();
    client.client().setRaycastHook(
        [this](const lunar::DataModel&, const lunar::RaycastQuery& query) -> std::optional<lunar::RaycastHit> {
            if (!world_)
                return std::nullopt;
            lunar::physics::RaycastParams params;
            params.filterType = query.filterIsInclude ? lunar::Enum::RaycastFilterType::Include
                                                      : lunar::Enum::RaycastFilterType::Exclude;
            for (const lunar::InstancePtr& instance : query.filter)
                params.filterDescendantsInstances.push_back(instance.get());
            params.ignoreWater = query.ignoreWater;
            params.respectCanCollide = query.respectCanCollide;
            params.collisionGroup = query.collisionGroup;
            const auto hit = world_->raycast(query.origin, query.direction, params);
            if (!hit || !hit->instance)
                return std::nullopt;
            return lunar::RaycastHit{.instance = hit->instance->shared_from_this(),
                                     .position = hit->position,
                                     .normal = hit->normal,
                                     .material = hit->material,
                                     .distance = hit->distance};
        });
}

void LiveCharacter::setMoveVector(lunar::Vector3 direction, bool relativeToCamera)
{
    move_ = direction;
    moveRelative_ = relativeToCamera;
}

void LiveCharacter::setJump(bool held)
{
    jump_ = held;
}

void LiveCharacter::setFacing(std::optional<lunar::Vector3> direction)
{
    facing_ = direction;
}

// Takes over the character the server gave our player, once its Humanoid
// and root part are in (with streaming the parts come after the model). The
// world is made the first time: every part of the Workspace, held where the
// network puts it except our character's.
void LiveCharacter::adopt(lunar::Model& character)
{
    controller_.reset();
    humanoidHold_.reset();
    auto* humanoid = dynamic_cast<lunar::Humanoid*>(character.findFirstChildOfClass("Humanoid"));
    const lunar::BasePart* root = humanoid ? humanoid->rootPart() : nullptr;
    lunar::Workspace* workspace = replica_->game()->workspace();
    if (!root || !workspace || !character.isDescendantOf(workspace))
        return;
    ensureWorld();
    if (!world_)
        return;
    std::weak_ptr<lunar::Model> weak = std::static_pointer_cast<lunar::Model>(character.shared_from_this());
    simulateOnly(character);
    world_->refresh();
    humanoidHold_ = humanoid->shared_from_this(); // the controller holds a reference to it
    controller_ = std::make_unique<lunar::physics::HumanoidController>(*world_, *humanoid);
    character_ = std::move(weak);
    spawn_ = root->position();
    waitingForFloor_ = 0.0;
    rootId_.reset();
    serverSamples_ = 0;
    serverQuiet_ = 0.0;
    sync_.ownsRoot = true;
    tracks_.reset();
    sync_.sendPhysicsNow();
}

// The world we keep of the Workspace, made when the client starts whether
// or not a character of ours comes: the scripts' raycasts cast in it, and a
// script moving a part carries what's welded to it [live: Midnight Racing's
// menu gives us no character and flies its camera on a part welded to the
// one it tweens; with no world the camera stayed behind]. Until our
// character comes, only the parts the server hands us are ours to simulate.
void LiveCharacter::ensureWorld()
{
    if (world_)
        return;
    lunar::Workspace* workspace = replica_->game()->workspace();
    if (!workspace)
        return;
    world_ = std::make_unique<lunar::physics::World>(*workspace);
    world_->setSimulationFilter([this](const lunar::BasePart& part) { return simulates(part); });
    unionShapes_ = std::make_unique<UnionShapes>();
    unionShapes_->attach(*world_);
    world_->onStep = [this](float dt) {
        if (controller_ && controlling_)
            controller_->step(dt);
    };
    replica_->simulatedHere = [this](const lunar::BasePart& part) {
        const auto it = owned_.find(&part);
        return it != owned_.end() && it->second.lock().get() == &part;
    };
}

// What our World simulates: our character's parts and the parts the server
// hands us; the rest is held where the network puts it.
void LiveCharacter::simulateOnly(lunar::Model& character)
{
    simulatedCharacter_ = std::static_pointer_cast<lunar::Model>(character.shared_from_this());
    world_->setSimulationFilter([this](const lunar::BasePart& part) { return simulates(part); });
}

bool LiveCharacter::simulates(const lunar::BasePart& part) const
{
    if (const auto it = owned_.find(&part); it != owned_.end() && it->second.lock().get() == &part)
        return true;
    const auto ours = simulatedCharacter_.lock();
    return ours && part.isDescendantOf(ours.get());
}

namespace {

// Where the joints and constraints of a part live: the Model it is in, or
// the part itself outside one (as ReplicatedMotion's assemblies).
lunar::Instance* jointScope(lunar::BasePart& part)
{
    for (lunar::Instance* node = part.parent(); node; node = node->parent())
        if (dynamic_cast<lunar::Model*>(node))
            return dynamic_cast<lunar::Workspace*>(node) ? static_cast<lunar::Instance*>(&part) : node;
    return &part;
}

lunar::BasePart* attachmentPart(const lunar::Attachment* attachment)
{
    const lunar::Instance* node = attachment;
    while (dynamic_cast<const lunar::Attachment*>(node))
        node = node->parent();
    return const_cast<lunar::BasePart*>(dynamic_cast<const lunar::BasePart*>(node));
}

// The two parts a joint or constraint holds together, while it's enabled.
std::pair<lunar::BasePart*, lunar::BasePart*> heldTogether(const lunar::Instance& instance)
{
    if (const auto* joint = dynamic_cast<const lunar::JointInstance*>(&instance); joint && joint->enabled())
        return {joint->part0(), joint->part1()};
    if (const auto* weld = dynamic_cast<const lunar::WeldConstraint*>(&instance); weld && weld->enabled())
        return {weld->part0(), weld->part1()};
    if (const auto* constraint = dynamic_cast<const lunar::Constraint*>(&instance);
        constraint && constraint->enabled() && constraint->attachment0() && constraint->attachment1())
        return {attachmentPart(constraint->attachment0()), attachmentPart(constraint->attachment1())};
    return {};
}

} // namespace

// The server hands a client the parts it should simulate by setting their
// NetworkOwnerV3 to the client's peer id [capture: the game gives a room's side
// door (DoorNormal.Door) to the player who opens it, Anchored false in the
// same packet, and leaves the swing to that client]. The genuine client then
// simulates the door's hinge servo itself. Lunarstone held it where the
// server's motion put it, and the server, waiting on its owner, sent a few
// frames of its own swing and then nothing for up to 2.5 s mid-swing
// [capture]: the door stopped short or ran on past its hinge's limit until
// the server's next frames. The door's knob, on its own hinge, kept the
// server's NetworkOwnerV3 while its frames came and stopped with the door's:
// GUESS: ownership goes by mechanism (assemblies held together by joints and
// constraints), so what's held to an owned part comes with it.
void LiveCharacter::refreshOwned(double dt)
{
    ownedAge_ += dt;
    if (replica_->ownershipChanges() == ownershipSeen_ && ownedAge_ < kOwnedRefresh)
        return;
    ownershipSeen_ = replica_->ownershipChanges();
    ownedAge_ = 0.0;
    const lunar::Model* ours = character();
    // In the order found (the replica's ids, then the joints from them), so
    // the world rebuilds the same assemblies in the same order every run.
    std::vector<lunar::BasePart*> found;
    std::unordered_map<const lunar::BasePart*, std::weak_ptr<lunar::Instance>> now;
    auto add = [&](lunar::BasePart* part) {
        if (!part || (ours && part->isDescendantOf(ours)) || now.contains(part))
            return;
        now.emplace(part, part->weak_from_this());
        found.push_back(part);
    };
    for (const lunar::net::InstanceId id : replica_->partsOwnedHere())
        add(dynamic_cast<lunar::BasePart*>(replica_->find(id).get()));
    constexpr std::size_t kMostParts = 512; // a mechanism, not a map
    for (std::size_t i = 0; i < found.size() && now.size() < kMostParts; ++i) {
        lunar::BasePart* part = found[i];
        if (part->anchored())
            continue; // an anchored part holds what's on it in place: nothing to carry along
        lunar::Instance* scope = jointScope(*part);
        auto visit = [&](const lunar::Instance& instance) {
            const auto [a, b] = heldTogether(instance);
            if (a == part && b && !b->anchored())
                add(b);
            else if (b == part && a && !a->anchored())
                add(a);
        };
        visit(*scope);
        for (const lunar::InstancePtr& descendant : scope->descendants())
            visit(*descendant);
    }
    for (const std::weak_ptr<lunar::Instance>& weak : ownedOrder_)
        if (const auto alive = std::dynamic_pointer_cast<lunar::BasePart>(weak.lock());
            alive && !now.contains(alive.get()))
            world_->refilter(*alive);
    for (lunar::BasePart* part : found)
        if (!owned_.contains(part))
            world_->refilter(*part);
    owned_ = std::move(now);
    ownedOrder_.clear();
    for (lunar::BasePart* part : found)
        ownedOrder_.push_back(part->weak_from_this());
}

// Whether there's something to stand on below the root part (not our own
// character), within a fall that wouldn't hurt.
bool LiveCharacter::groundLoaded() const
{
    const lunar::Humanoid* h = humanoid();
    const lunar::BasePart* root = h ? h->rootPart() : nullptr;
    lunar::Model* model = character();
    if (!root || !model)
        return false;
    lunar::physics::RaycastParams params;
    params.filterDescendantsInstances.push_back(model);
    return world_->raycast(root->position(), {0.0f, -50.0f, 0.0f}, params).has_value();
}

void LiveCharacter::step(double dt)
{
    auto* players = dynamic_cast<lunar::Players*>(replica_->game()->findFirstChildOfClass("Players"));
    lunar::Player* local = players ? players->localPlayer() : nullptr;
    lunar::Model* current = local ? local->character() : nullptr;
    // A new character (a respawn), one whose parts are still streaming in, or none.
    if (current != character() || (current && !controller_)
        || (controller_ && !controller_->humanoid().isDescendantOf(current))) {
        controller_.reset();
        humanoidHold_.reset();
        character_.reset();
        if (current)
            adopt(*current);
    }
    ensureWorld();
    if (!world_)
        return;
    if (unionShapes_)
        unionShapes_->update(*world_);
    if (!controller_) {
        // None of ours to move; what scripts add is picked up all the same.
        world_->refresh();
        return;
    }
    lunar::Humanoid& humanoid = controller_->humanoid();
    // The server moving our root itself (it took the network ownership: it
    // sends the root's motion, which the replica plays back): we only
    // follow, like any part someone else owns, until it stops. The game's server
    // takes the character over for a ragdoll (anchoring its root) and gives
    // it back; kept, the character stayed where the server left it, its
    // Humanoid stuck getting up [live]. Once a second goes by with no motion
    // from the server and the root unanchored, it's ours again.
    // GUESS: the client's own hand-over wasn't traced (the server never sent
    // our root's motion while we owned it [live]), nor how it learns of the
    // hand-back: a server that owns the root and lets it rest sends nothing
    // either, and would get our motion for it until it moved it again.
    if (const lunar::BasePart* root = humanoid.rootPart()) {
        if (!rootId_)
            rootId_ = replica_->idOf(root);
        const std::size_t samples = rootId_ ? replica_->motion().receivedFor(*rootId_) : 0;
        if (sync_.ownsRoot && samples > serverSamples_) {
            sync_.ownsRoot = false;
            simulatedCharacter_.reset();
            world_->setSimulationFilter([this](const lunar::BasePart& part) { return simulates(part); });
        } else if (!sync_.ownsRoot) {
            serverQuiet_ = samples > serverSamples_ ? 0.0 : serverQuiet_ + dt;
            if (serverQuiet_ > kServerLetGo && !root->anchored()) {
                sync_.ownsRoot = true;
                if (lunar::Model* model = character())
                    simulateOnly(*model);
                world_->refresh();
                sync_.sendPhysicsNow();
            }
        }
        serverSamples_ = samples;
    }
    refreshOwned(dt);
    if (!sync_.ownsRoot) {
        // The server moves it, but the keys still reach MoveDirection, as the
        // client's ControlModule calls Humanoid:Move whoever owns the root:
        // The game hides you in a wardrobe that way (teleported in, anchored)
        // and lets you out when you walk.
        controller_->move(move_, moveRelative_);
        controller_->publishMoveDirection();
        if (!owned_.empty()) {
            // What else we own still moves.
            controlling_ = false;
            world_->advance(static_cast<float>(dt));
            controlling_ = true;
        }
        return;
    }
    if (waitingForFloor_) {
        world_->refresh();
        *waitingForFloor_ += dt;
        if (!groundLoaded() && *waitingForFloor_ < kFloorWait)
            return;
        waitingForFloor_.reset();
    }
    controller_->move(move_, moveRelative_);
    controller_->setFacing(facing_);
    humanoid.setJump(jump_);
    // Touches as the step's collisions see them, where the scripts put our
    // parts, and where the step leaves them: a part teleported into another
    // and pushed back out by the step touched it all the same, as the sUNC
    // test game's server waits for [replay: sUNC sets its part on the one
    // the server listens to each other frame; after the step they're apart].
    // GUESS: the client's own detection runs in each physics step.
    if (lunar::Model* model = character())
        noteTouches(*model);
    world_->advance(static_cast<float>(dt));
    if (lunar::Model* model = character())
        noteTouches(*model);
    // Fallen out of the world (Workspace.FallenPartsDestroyHeight took its
    // parts) or otherwise headless: the Humanoid dies, which the server
    // hears as its state and answers with a new character.
    // GUESS: the client's own Humanoid dies of NoNeck once those parts are
    // gone; the controller can't step without a root part, so it's done here.
    if (!humanoid.rootPart() && humanoid.getState() != lunar::Enum::HumanoidStateType::Dead)
        controller_->changeState(lunar::Enum::HumanoidStateType::Dead);
}

std::vector<lunar::net::CharacterSync::Outgoing> LiveCharacter::take(std::uint64_t nowMs)
{
    lunar::Model* model = character();
    if (!model || !controller_)
        return {};
    const float dt = lastTakeMs_ ? static_cast<float>(nowMs - *lastTakeMs_) / 1000.0f : 0.0f;
    lastTakeMs_ = nowMs;
    std::vector<std::vector<lunar::Value>> updates;
    if (lunar::avatar::CharacterAnimation* entry = animation_.find(*model); entry && entry->animator)
        for (const lunar::avatar::TrackUpdate& update : tracks_.collect(*entry->animator, dt))
            updates.push_back(update.arguments());
    auto out = sync_.frame(*model, nowMs, updates, ownedRoots());
    if (auto touches = takeTouches())
        out.push_back({std::move(*touches), lunar::net::Reliability::ReliableOrdered});
    return out;
}

// The roots of the assemblies the server gave us besides our character, in
// the order found: what CharacterSync sends the motion of. GUESS: the root
// our world picks is the one the server's would (it is for a lone part).
std::vector<const lunar::BasePart*> LiveCharacter::ownedRoots()
{
    std::vector<const lunar::BasePart*> roots;
    const lunar::Model* ours = character();
    for (const std::weak_ptr<lunar::Instance>& weak : ownedOrder_) {
        const auto part = std::dynamic_pointer_cast<lunar::BasePart>(weak.lock());
        if (!part)
            continue;
        const lunar::physics::Assembly* assembly = world_ ? world_->assemblyOf(part.get()) : nullptr;
        const lunar::BasePart* root = assembly && assembly->root ? assembly->root : part.get();
        if ((ours && root->isDescendantOf(ours)) || std::ranges::contains(roots, root))
            continue;
        roots.push_back(root);
    }
    return roots;
}

// The server sees our parts touch nothing unless we say so: the client
// owning an assembly reports its touches (ID_TOUCHES), and the server fires
// Touched and TouchEnded from them [code: Workspace::processTouchEvent
// 0x1046d0924 queues them, PhysicsTouchSerializer::send 0x10558f07e sends].
// Ours are our character's and the parts the server gave us besides. Only
// pairs with a part the server listens to (a TouchTransmitter it sent under
// it) go out. The game opens its side room doors from Hidden.Touched; the sUNC
// test game waits for a part it hands the client to touch one [live].
// The client's own scripts hear the same touches: Touched and TouchEnded fire
// here on both parts, for a part any script listens to (a TouchTransmitter of
// the server's or of the client's own) [sUNC's getconnections sets an
// anchored part on the character's head and waits for its Touched].
void LiveCharacter::noteTouches(lunar::Model& character)
{
    std::vector<const lunar::BasePart*> ours;
    for (const lunar::InstancePtr& d : character.descendants())
        if (const auto* part = dynamic_cast<const lunar::BasePart*>(d.get()))
            ours.push_back(part);
    for (const std::weak_ptr<lunar::Instance>& weak : ownedOrder_)
        if (const auto* part = dynamic_cast<const lunar::BasePart*>(weak.lock().get());
            part && !std::ranges::contains(ours, part))
            ours.push_back(part);
    auto now = world_->touches(
        ours, [](const lunar::BasePart& part) { return part.findFirstChildOfClass("TouchTransmitter") != nullptr; });
    // Each event moves its pair's state (0x1046d0a6f): new pairs start at 0
    // (touch) or 3 (untouch); then touch, untouch alternate through the rest.
    auto change = [&](const lunar::physics::World::Touch& pair, bool touched) {
        auto it = std::ranges::find_if(touchChanges_, [&](const TouchChange& c) {
            return c.ours.lock().get() == pair.ours && c.other.lock().get() == pair.other;
        });
        if (it == touchChanges_.end()) {
            touchChanges_.push_back({pair.ours->weak_from_this(), pair.other->weak_from_this(),
                                     static_cast<std::uint8_t>(touched ? 0 : 3)});
            return;
        }
        static constexpr std::uint8_t afterUntouch[] = {4, 5, 4, 3, 4, 5}; // 0, 1, 2 end touched
        static constexpr std::uint8_t afterTouch[] = {0, 1, 2, 1, 2, 1};   // 3, 4, 5 end untouched
        it->state = touched ? afterTouch[it->state] : afterUntouch[it->state];
    };
    // Whether the server listens to the part: a TouchTransmitter it sent.
    auto serverListens = [&](const lunar::BasePart& part) {
        return std::ranges::any_of(part.children(), [&](const lunar::InstancePtr& child) {
            return child->isA("TouchTransmitter") && replica_->idOf(child.get());
        });
    };
    struct Heard {
        std::weak_ptr<lunar::Instance> ours, other;
        bool touched;
    };
    std::vector<Heard> heard;
    auto note = [&](const lunar::physics::World::Touch& pair, bool touched) {
        if (serverListens(*pair.other))
            change(pair, touched);
        heard.push_back({pair.ours->weak_from_this(), pair.other->weak_from_this(), touched});
    };
    for (const auto& pair : now)
        if (!std::ranges::contains(touching_, pair, &Touching::pair))
            note(pair, true);
    for (const Touching& t : touching_)
        if (!t.ours.expired() && !t.other.expired() && !std::ranges::contains(now, t.pair))
            note(t.pair, false);
    touching_.clear();
    for (const auto& pair : now)
        touching_.push_back({pair, pair.ours->weak_from_this(), pair.other->weak_from_this()});
    // After the bookkeeping: a handler may move or destroy either part.
    for (const Heard& h : heard) {
        const auto ours = h.ours.lock(), other = h.other.lock();
        if (!ours || !other)
            continue;
        const char* event = h.touched ? "Touched" : "TouchEnded";
        const std::array onOther{lunar::Value(ours)};
        const std::array onOurs{lunar::Value(other)};
        other->fireEvent(event, onOther);
        ours->fireEvent(event, onOurs);
    }
}

// The pairs that changed since the last call as one ID_TOUCHES: { ref; ref;
// u8 state } each, the lower id first (0x10129a730), then a null ref.
// States [code: the receiver's jump table at 0x1046d4140]: 0 touch, 1
// untouch then touch, 2 touch untouch touch, 3 untouch, 4 touch then
// untouch, 5 untouch touch untouch. Sent reliable and ordered (0x10558f532).
std::optional<std::vector<std::uint8_t>> LiveCharacter::takeTouches()
{
    if (touchChanges_.empty())
        return std::nullopt;
    lunar::net::TouchesMessage message;
    for (const TouchChange& change : touchChanges_) {
        const auto ours = change.ours.lock(), other = change.other.lock();
        const auto a = replica_->idOf(ours.get()), b = replica_->idOf(other.get());
        if (!a || !b)
            continue; // not replicated: the client drops it too (0x105609894)
        const bool aFirst = std::pair(a->scope, a->index) < std::pair(b->scope, b->index);
        message.touches.push_back({aFirst ? *a : *b, aFirst ? *b : *a, change.state});
    }
    touchChanges_.clear();
    if (message.touches.empty())
        return std::nullopt;
    return lunar::net::frame(lunar::net::encodeTouches(message), replica_->framing);
}

std::string LiveCharacter::describe() const
{
    const lunar::Humanoid* h = humanoid();
    const lunar::BasePart* root = h ? h->rootPart() : nullptr;
    if (!root)
        return "no character yet";
    const lunar::Vector3 p = root->position();
    const lunar::Vector3 v = root->assemblyLinearVelocity();
    const float moved = spawn_ ? (p - *spawn_).magnitude() : 0.0f;
    const auto offset = sync_.clockOffset();
    return std::format("root at ({:.1f}, {:.1f}, {:.1f}), {:.1f} studs from spawn, speed {:.1f}, {}; sent {} ISR "
                       "(clock offset {}), {} physics, {} Humanoid changes, {} track updates, {} CFrame acks",
                       p.x, p.y, p.z, moved, lunar::Vector3{v.x, 0.0f, v.z}.magnitude(), stateName(h->getState()),
                       sync_.isrPackets, offset ? std::to_string(*offset) : std::string("none yet"),
                       sync_.physicsPackets, sync_.propertyChanges, sync_.animatorEvents, sync_.cframeAcks)
           + (controller_ && !controller_->deathCause().empty()
                  ? std::format(" (died of {}, health {})", controller_->deathCause(), h->health())
                  : "")
           + (sync_.ownsRoot ? "" : "; the server moves it");
}

std::optional<WalkPlan> WalkPlan::parse(std::string_view text)
{
    if (text == "square")
        text = "i1,n3,j,e3,s3,w3,i1";
    else if (text == "line")
        text = "i1,n4,j,s4,i1";
    WalkPlan plan;
    plan.text_ = std::string(text);
    lunar::Vector3 last;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view token = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (token.empty())
            return std::nullopt;
        Step step;
        if (token == "j") {
            step = {last, true, 0.25};
        } else {
            switch (token[0]) {
            case 'n': step.move = {0.0f, 0.0f, -1.0f}; break;
            case 's': step.move = {0.0f, 0.0f, 1.0f}; break;
            case 'e': step.move = {1.0f, 0.0f, 0.0f}; break;
            case 'w': step.move = {-1.0f, 0.0f, 0.0f}; break;
            case 'i': break;
            default: return std::nullopt;
            }
            const std::string_view number = token.substr(1);
            const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), step.seconds);
            if (error != std::errc{} || end != number.data() + number.size() || step.seconds < 0)
                return std::nullopt;
            last = step.move;
        }
        plan.steps_.push_back(step);
    }
    if (plan.steps_.empty())
        return std::nullopt;
    return plan;
}

WalkPlan::Input WalkPlan::at(double seconds) const
{
    for (const Step& step : steps_) {
        if (seconds < step.seconds)
            return {step.move, step.jump, false};
        seconds -= step.seconds;
    }
    return {{}, false, true};
}

double WalkPlan::duration() const
{
    double total = 0;
    for (const Step& step : steps_)
        total += step.seconds;
    return total;
}
