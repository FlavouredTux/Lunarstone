#include <lunar/Tools.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar {

namespace {

constexpr std::string_view gripName = "RightGrip";

Workspace* workspaceOf(const DataModel& game)
{
    return static_cast<Workspace*>(game.findFirstChildOfClass("Workspace"));
}

Humanoid* humanoidIn(const Instance* model)
{
    return model ? static_cast<Humanoid*>(model->findFirstChildOfClass("Humanoid")) : nullptr;
}

BasePart* partNamed(const Instance& parent, std::string_view name)
{
    return instanceCast<BasePart>(parent.findFirstChild(name));
}

// Humanoid::getTorsoSlow: UpperTorso for R15, Torso for R6, else the root part.
// GUESS: the binary falls back to a second cached part; the root part is assumed.
BasePart* torsoOf(const Humanoid& humanoid)
{
    const Instance* character = humanoid.parent();
    if (!character)
        return nullptr;
    const bool r15 = humanoid.rigType() == Enum::HumanoidRigType::R15;
    if (BasePart* torso = partNamed(*character, r15 ? "UpperTorso" : "Torso"))
        return torso;
    return humanoid.rootPart();
}

// The state a tool in `model` gets (sub_1043c2cb8): no Humanoid keeps it
// loose in the Workspace; with RequiresHandle the Humanoid also needs a torso
// and a right hand (R6: Right Arm with its Right Shoulder) to hold it.
ToolState gripStatus(const Tool& tool, const Instance* model)
{
    const Humanoid* humanoid = humanoidIn(model);
    if (!humanoid)
        return ToolState::InWorkspace;
    if (!tool.requiresHandle())
        return ToolState::Equipped;
    if (!torsoOf(*humanoid))
        return ToolState::InCharacter;
    if (!gripLimb(*humanoid))
        return ToolState::Gripped;
    if (humanoid->rigType() != Enum::HumanoidRigType::R15) {
        const Instance* character = humanoid->parent();
        if (!character || !character->findFirstChild("Right Shoulder", true))
            return ToolState::Gripped;
    }
    return ToolState::Equipped;
}

bool isDead(const Humanoid& humanoid)
{
    return humanoid.health() <= 0.0f || humanoid.getState() == Enum::HumanoidStateType::Dead;
}

// Moves every part of the tool rigidly so the handle lands on `to`.
void moveTool(Tool& tool, BasePart& handle, const CFrame& to)
{
    // Every place first: setting one part of a welded assembly moves the rest.
    const CFrame delta = to * handle.cframe().inverse();
    std::vector<std::pair<BasePart*, CFrame>> moves;
    for (const InstancePtr& d : tool.descendants())
        if (auto* part = instanceCast<BasePart>(d.get()))
            moves.emplace_back(part, delta * part->cframe());
    for (const auto& [part, cframe] : moves)
        part->setCFrame(cframe);
}

// Separating axis test between two boxes.
bool boxesOverlap(const BasePart& a, const BasePart& b)
{
    const CFrame& ca = a.cframe();
    const CFrame& cb = b.cframe();
    const Vector3 ha = a.size() * 0.5f;
    const Vector3 hb = b.size() * 0.5f;
    const Vector3 d = cb.position - ca.position;
    if (d.magnitude() > ha.magnitude() + hb.magnitude())
        return false;
    const std::array<Vector3, 3> axesA{ca.rightVector(), ca.upVector(), -ca.lookVector()};
    const std::array<Vector3, 3> axesB{cb.rightVector(), cb.upVector(), -cb.lookVector()};
    const std::array<float, 3> extA{ha.x, ha.y, ha.z};
    const std::array<float, 3> extB{hb.x, hb.y, hb.z};
    auto separated = [&](Vector3 axis) {
        const float length = axis.magnitude();
        if (length < 1e-6f)
            return false;
        axis = axis / length;
        float ra = 0.0f;
        float rb = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            ra += extA[i] * std::abs(axesA[i].dot(axis));
            rb += extB[i] * std::abs(axesB[i].dot(axis));
        }
        return std::abs(d.dot(axis)) > ra + rb;
    };
    for (const Vector3& axis : axesA)
        if (separated(axis))
            return false;
    for (const Vector3& axis : axesB)
        if (separated(axis))
            return false;
    for (const Vector3& u : axesA)
        for (const Vector3& v : axesB)
            if (separated(u.cross(v)))
                return false;
    return true;
}

} // namespace

// --- Free functions ------------------------------------------------------------------

BasePart* gripLimb(const Humanoid& humanoid)
{
    const Instance* character = humanoid.parent();
    if (!character)
        return nullptr;
    return partNamed(*character, humanoid.rigType() == Enum::HumanoidRigType::R15 ? "RightHand" : "Right Arm");
}

// Binary sub_103644196. The rotation is a look straight down (-Y), which
// CFrame.new(0, -1, 0, 1, 0, -0, 0, 0, 1, 0, -1, -0) spells out for R6.
CFrame gripOffset(const Humanoid& humanoid)
{
    const CFrame down = CFrame::angles(-std::numbers::pi_v<float> / 2.0f, 0.0f, 0.0f);
    if (humanoid.rigType() == Enum::HumanoidRigType::R15) {
        if (const Instance* character = humanoid.parent())
            if (const auto* attachment
                = instanceCast<const Attachment>(character->findFirstChild("RightGripAttachment", true)))
                return attachment->cframe();
        return CFrame(0.0f, -0.2f, 0.0f) * down;
    }
    return CFrame(0.0f, -1.0f, 0.0f) * down;
}

Weld* rightGrip(const Humanoid& humanoid)
{
    BasePart* limb = gripLimb(humanoid);
    return limb ? instanceCast<Weld>(limb->findFirstChild(gripName)) : nullptr;
}

Player* playerFromCharacter(const DataModel& game, const Instance& character)
{
    const Instance* players = game.findFirstChildOfClass("Players");
    if (!players)
        return nullptr;
    for (const InstancePtr& child : players->children())
        if (auto* player = instanceCast<Player>(child.get()); player && player->character() == &character)
            return player;
    return nullptr;
}

bool unequipTools(const DataModel& game, Humanoid& humanoid)
{
    Instance* character = humanoid.parent();
    if (!character)
        return true;
    Player* player = playerFromCharacter(game, *character);
    Instance* backpack = player ? player->findFirstChildOfClass("Backpack") : nullptr;
    if (!backpack)
        return false;
    for (const InstancePtr& child : std::vector(character->children().begin(), character->children().end()))
        if (child->isA("Tool") && child->parent() == character)
            child->setParent(backpack);
    return true;
}

void equipTool(const DataModel& game, Humanoid& humanoid, Tool& tool)
{
    Instance* character = humanoid.parent();
    if (!character || tool.parent() == character)
        return;
    unequipTools(game, humanoid);
    tool.setParent(character);
}

// Binary sub_1043c21aa, called on KeyCode 8 for the local player.
void dropTools(const DataModel& game, Player& player)
{
    Model* character = player.character();
    Workspace* workspace = workspaceOf(game);
    if (!character || !workspace)
        return;
    for (const InstancePtr& child : std::vector(character->children().begin(), character->children().end()))
        if (auto* tool = instanceCast<Tool>(child.get()); tool && tool->canBeDropped() && tool->parent() == character)
            tool->setParent(workspace);
}

// --- Hotbar ------------------------------------------------------------------------

void Hotbar::clear()
{
    for (auto& s : slots_)
        s.reset();
}

Tool* Hotbar::slot(int index) const
{
    if (index < 0 || index >= slotCount)
        return nullptr;
    return slots_[static_cast<std::size_t>(index)].lock().get();
}

// The lowest empty slot (FindLowestEmpty); a full hotbar sends the rest to
// the inventory, which isn't drawn.
void Hotbar::add(Tool& tool)
{
    for (auto& s : slots_)
        if (s.expired()) {
            s = std::static_pointer_cast<Tool>(tool.shared_from_this());
            return;
        }
}

void Hotbar::sync(const DataModel& game)
{
    const auto* players = instanceCast<const Players>(game.findFirstChildOfClass("Players"));
    Player* player = players ? players->localPlayer() : nullptr;
    Model* character = player ? player->character() : nullptr;
    if (!character) {
        character_.reset();
        clear();
        return;
    }
    if (character_.lock().get() != character) {
        clear();
        character_ = character->shared_from_this();
    }
    const Instance* backpack = player->findFirstChildOfClass("Backpack");
    for (auto& s : slots_)
        if (auto tool = s.lock(); !tool || (tool->parent() != character && tool->parent() != backpack))
            s.reset();
    auto known = [&](const Instance& tool) {
        return std::ranges::any_of(slots_, [&](const auto& s) { return s.lock().get() == &tool; });
    };
    for (const Instance* container : {static_cast<const Instance*>(character), backpack})
        if (container)
            for (const InstancePtr& child : container->children())
                if (auto* tool = instanceCast<Tool>(child.get()); tool && !known(*tool))
                    add(*tool);
}

void Hotbar::select(const DataModel& game, int index)
{
    Tool* tool = slot(index);
    const auto* players = instanceCast<const Players>(game.findFirstChildOfClass("Players"));
    Player* player = players ? players->localPlayer() : nullptr;
    Model* character = player ? player->character() : nullptr;
    auto* humanoid = character ? instanceCast<Humanoid>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (!tool || !humanoid)
        return;
    if (tool->parent() == character) {
        unequipTools(game, *humanoid);
    } else if (tool->parent() == player->findFirstChildOfClass("Backpack")) {
        unequipTools(game, *humanoid);
        tool->setParent(character);
    }
}

// --- ToolSystem --------------------------------------------------------------------

ToolSystem::ToolSystem(DataModel& game) : game_(&game)
{
    for (const InstancePtr& d : game.descendants())
        if (auto* tool = instanceCast<Tool>(d.get()))
            track(*tool);
    connections_.push_back(game.descendantAdded.connect([this](Instance* instance) {
        if (auto* tool = instanceCast<Tool>(instance))
            track(*tool);
    }));
}

ToolSystem::~ToolSystem()
{
    for (Connection& c : connections_)
        c.disconnect();
    for (auto& [tool, entry] : entries_)
        for (Connection& c : entry.connections)
            c.disconnect();
}

ToolState ToolSystem::state(const Tool& tool) const
{
    auto it = entries_.find(&tool);
    return it == entries_.end() ? ToolState::None : it->second.state;
}

bool ToolSystem::weldsLocally(const Tool& tool) const
{
    auto it = entries_.find(&tool);
    return it != entries_.end() && it->second.local;
}

Player* ToolSystem::localPlayer() const
{
    const auto* players = instanceCast<const Players>(game_->findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

void ToolSystem::setMode(Mode mode)
{
    if (mode == mode_)
        return;
    mode_ = mode;
    // What came before doesn't count: only a move from now on gives a client the weld.
    for (auto& [key, entry] : entries_)
        entry.local = mode == Mode::Backend;
    refreshAll();
}

void ToolSystem::refreshAll()
{
    std::vector<std::shared_ptr<Tool>> tools;
    for (const auto& [key, entry] : entries_)
        if (auto tool = entry.tool.lock())
            tools.push_back(std::move(tool));
    for (const std::shared_ptr<Tool>& tool : tools)
        refresh(*tool);
}

// The binary checks the tool's parent against the humanoid's Workspace and
// whether the parent is in one (0x1046ca598, 0x1046ca67e); a tool anywhere
// in the Workspace is taken to be what that asks [GUESS on those helpers].
void ToolSystem::equip(Humanoid& humanoid, Tool& tool)
{
    Instance* character = humanoid.parent();
    if (mode_ == Mode::Backend || !character || tool.parent() == character) {
        equipTool(*game_, humanoid, tool);
        return;
    }
    unequipTools(*game_, humanoid);
    const Workspace* workspace = workspaceOf(*game_);
    if (workspace && tool.isDescendantOf(workspace)) {
        const Value args[] = {Value(tool.shared_from_this())};
        humanoid.fireEvent("ServerEquipTool", args);
        return;
    }
    tool.setParent(character);
}

void ToolSystem::track(Tool& tool)
{
    auto it = entries_.find(&tool);
    if (it != entries_.end() && it->second.tool.lock().get() == &tool) {
        refresh(tool);
        return;
    }
    // A new tool at an address a dead one had: forget the dead one.
    std::erase_if(entries_, [](const auto& item) { return item.second.tool.expired(); });
    Entry& entry = entries_[&tool];
    entry = {};
    entry.tool = std::static_pointer_cast<Tool>(tool.shared_from_this());
    // The tool moving (or anything above it), and its Handle coming or going.
    entry.connections.push_back(tool.ancestryChanged.connect([this, &tool](Instance*, Instance*) { refresh(tool); }));
    auto handleChanged = [this, &tool](Instance* child) {
        if (child->name() == "Handle")
            refresh(tool);
    };
    entry.connections.push_back(tool.childAdded.connect(handleChanged));
    entry.connections.push_back(tool.childRemoved.connect(handleChanged));
    entry.parent = tool.parent() ? tool.parent()->weak_from_this() : std::weak_ptr<Instance>{};
    entry.local = mode_ == Mode::Backend;
    refresh(tool);
}

// Binary sub_1043c2a08: nothing outside the Workspace or without a needed
// Handle, otherwise what the parent's Humanoid allows.
//
// A client runs the machine only for the tools in its own character
// (vfn_60's client branch refreshes only those).
ToolState ToolSystem::target(const Tool& tool) const
{
    if (!tool.handle() && tool.requiresHandle())
        return ToolState::None;
    const Workspace* workspace = workspaceOf(*game_);
    if (!workspace || !tool.isDescendantOf(workspace))
        return ToolState::None;
    if (mode_ == Mode::Client) {
        const Player* player = localPlayer();
        if (!player || !player->character() || tool.parent() != player->character())
            return ToolState::None;
    }
    return gripStatus(tool, tool.parent());
}

// Binary vfn_60 (ancestry changed) then sub_1043c2a4c: leaving the character
// it was held by unwinds the tool first, then it climbs to where it is now.
void ToolSystem::refresh(Tool& tool)
{
    auto it = entries_.find(&tool);
    if (it == entries_.end())
        return;
    Entry& entry = it->second;
    // A move clears the weld duty; on a client a move from the local
    // player's Backpack into its character sets it (vfn_60, Tool+0x488).
    if (Instance* now = tool.parent(); entry.parent.lock().get() != now) {
        const std::shared_ptr<Instance> from = entry.parent.lock();
        entry.parent = now ? now->weak_from_this() : std::weak_ptr<Instance>{};
        if (mode_ == Mode::Client) {
            const Player* player = localPlayer();
            entry.local = player && from && now && now == player->character()
                          && from.get() == player->findFirstChildOfClass("Backpack");
        }
    }
    // Equipped and Unequipped run scripts that may move the tool again; that
    // nested refresh takes over and this one stops.
    const std::uint64_t generation = ++entry.generation;
    if (entry.state >= ToolState::InCharacter && entry.holder.lock().get() != tool.parent()) {
        stepDown(entry, tool, std::min(target(tool), ToolState::InWorkspace));
        if (entry.generation != generation)
            return;
    }
    const ToolState to = target(tool);
    if (to > entry.state)
        stepUp(entry, tool, to);
    else if (to < entry.state)
        stepDown(entry, tool, to);
}

void ToolSystem::stepUp(Entry& entry, Tool& tool, ToolState to)
{
    const std::uint64_t generation = entry.generation;
    while (entry.state < to && entry.generation == generation) {
        const auto next = static_cast<ToolState>(static_cast<int>(entry.state) + 1);
        entry.state = next;
        switch (next) {
        case ToolState::InCharacter:
            entry.holder = tool.parent() ? tool.parent()->shared_from_this() : nullptr;
            break;
        case ToolState::Gripped:
            grip(entry, tool);
            break;
        case ToolState::Equipped: {
            const Value args[] = {Value(mouseFor(tool.parent()))};
            tool.fireEvent("Equipped", args);
            break;
        }
        default:
            break;
        }
    }
}

void ToolSystem::stepDown(Entry& entry, Tool& tool, ToolState to)
{
    const std::uint64_t generation = entry.generation;
    while (entry.state > to && entry.generation == generation) {
        const ToolState from = entry.state;
        entry.state = static_cast<ToolState>(static_cast<int>(from) - 1);
        switch (from) {
        case ToolState::Equipped:
            tool.fireEvent("Unequipped");
            break;
        case ToolState::Gripped:
            ungrip(entry, tool);
            break;
        case ToolState::InCharacter:
            entry.holder.reset();
            break;
        default:
            break;
        }
    }
}

// Binary sub_1043c2e82: the tool moves so its Handle sits in the hand
// (SimMoveToolToHandOnEquip), then a Weld named RightGrip in the hand holds
// it there: Part0 the hand, Part1 the Handle, C0 the grip offset, C1 Grip.
// A side that doesn't weld this tool (a client, for a tool the server put in
// its character) leaves it where it is and takes the weld that's there, if
// any: a RightGrip from the hand to this Handle (0x1043c43c0's test).
void ToolSystem::grip(Entry& entry, Tool& tool)
{
    Humanoid* humanoid = humanoidIn(tool.parent());
    BasePart* handle = tool.handle();
    BasePart* limb = humanoid ? gripLimb(*humanoid) : nullptr;
    if (!handle || !limb)
        return;
    entry.madeWeld = false;
    if (!entry.local) {
        for (const InstancePtr& child : limb->children())
            if (auto* weld = instanceCast<Weld>(child.get());
                weld && weld->name() == gripName && weld->part0() == limb && weld->part1() == handle) {
                entry.weld = child;
                break;
            }
        return;
    }
    const CFrame c0 = gripOffset(*humanoid);
    moveTool(tool, *handle, limb->cframe() * c0 * tool.grip().inverse());
    auto weld = std::make_shared<Weld>();
    weld->setName(std::string(gripName));
    weld->setC0(c0);
    weld->setC1(tool.grip());
    weld->setPart0(limb);
    weld->setPart1(handle);
    weld->setParent(limb);
    entry.weld = weld;
    entry.madeWeld = true;
}

// Binary sub_1043c3fd8: the weld goes, and a tool leaving a Humanoid's hand
// is put 8 studs in front of the character and 4 up, where a dropped tool
// falls from.
//
// A weld this side didn't make isn't its to take away, and the tool isn't its to move.
void ToolSystem::ungrip(Entry& entry, Tool& tool)
{
    auto weld = entry.weld.lock();
    entry.weld.reset();
    if (!std::exchange(entry.madeWeld, false))
        return;
    Instance* character = nullptr;
    if (auto* joint = instanceCast<Weld>(weld.get())) {
        if (BasePart* limb = joint->part0())
            character = limb->parent();
        if (!weld->destroyed())
            weld->setParent(nullptr);
    }
    BasePart* handle = tool.handle();
    const Humanoid* humanoid = humanoidIn(character);
    if (!handle || !humanoid)
        return;
    const auto* model = instanceCast<const Model>(character);
    const BasePart* root = humanoid->rootPart();
    const CFrame frame = root ? root->cframe() : model ? model->getPivot() : CFrame();
    const Vector3 at = frame.position + Vector3{0.0f, 4.0f, 0.0f} + frame.lookVector() * 8.0f;
    moveTool(tool, *handle, CFrame(at) * tool.grip().inverse());
    for (const InstancePtr& d : tool.descendants())
        if (auto* part = instanceCast<BasePart>(d.get())) {
            part->setAssemblyLinearVelocity({});
            part->setAssemblyAngularVelocity({});
        }
}

InstancePtr ToolSystem::mouseFor(const Instance* character)
{
    if (!mouse || !character)
        return nullptr;
    Player* player = playerFromCharacter(*game_, *character);
    return player ? mouse(*player) : nullptr;
}

bool ToolSystem::activate(Tool& tool, bool fromScript) const
{
    if (state(tool) != ToolState::Equipped)
        return false;
    if (!fromScript && tool.manualActivationOnly())
        return true;
    if (tool.enabled())
        tool.fireEvent("Activated");
    return true;
}

bool ToolSystem::deactivate(Tool& tool, bool) const
{
    if (state(tool) != ToolState::Equipped)
        return false;
    if (tool.enabled())
        tool.fireEvent("Deactivated");
    return true;
}

// Binary sub_1043c4788, the Handle's Touched handler.
void ToolSystem::touched(Tool& tool, BasePart& hit)
{
    if (state(tool) != ToolState::InWorkspace)
        return;
    auto* character = instanceCast<Model>(hit.parent());
    if (!character || gripStatus(tool, character) != ToolState::Equipped)
        return;
    Humanoid* humanoid = humanoidIn(character);
    if (!humanoid || isDead(*humanoid))
        return;
    Player* player = playerFromCharacter(*game_, *character);
    if (!player)
        return;
    // Whatever it holds goes to the Backpack (or away, without one).
    Instance* backpack = player->findFirstChildOfClass("Backpack");
    while (Instance* held = character->findFirstChildOfClass("Tool"))
        held->setParent(backpack);
    tool.setParent(character);
}

void ToolSystem::pickUpTouching()
{
    std::vector<std::shared_ptr<Tool>> loose;
    for (const auto& [key, entry] : entries_)
        if (entry.state == ToolState::InWorkspace)
            if (auto tool = entry.tool.lock())
                loose.push_back(std::move(tool));
    if (loose.empty())
        return;
    const Instance* players = game_->findFirstChildOfClass("Players");
    if (!players)
        return;
    for (const InstancePtr& child : players->children()) {
        auto* player = instanceCast<Player>(child.get());
        Model* character = player ? player->character() : nullptr;
        if (!character)
            continue;
        for (const std::shared_ptr<Tool>& tool : loose) {
            BasePart* handle = tool->handle();
            if (!handle || !handle->canTouch() || state(*tool) != ToolState::InWorkspace)
                continue;
            for (const InstancePtr& part : std::vector(character->children().begin(), character->children().end()))
                if (auto* limb = instanceCast<BasePart>(part.get());
                    limb && limb->canTouch() && boxesOverlap(*handle, *limb)) {
                    touched(*tool, *limb);
                    break;
                }
        }
    }
}

} // namespace lunar
