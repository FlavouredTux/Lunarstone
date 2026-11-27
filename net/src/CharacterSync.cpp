#include <lunar/net/CharacterSync.hpp>

#include <lunar/Classes.hpp>
#include <lunar/RemoteValue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

namespace lunar::net {

namespace {

// Capability bit 25: the physics id goes before the sequence (ROBLOX-LAYER.md 9.2).
constexpr std::uint64_t kTimestampIdFirstCapability = std::uint64_t{1} << 25;

// MoveDirection components closer than this count as unchanged (float noise
// from the controller's own arithmetic). GUESS: the client's threshold.
constexpr float kMoveDirectionEpsilon = 1e-4f;

// How far one ISR packet moves our clock offset toward a new estimate, in
// ticks; a bigger difference (a hitch, the first estimate) is taken at once.
// GUESS: the genuine client's offset moved 0..8 ticks a packet while it
// settled, and stayed put after [observed in a client capture].
constexpr std::int64_t kOffsetSlew = 8;
constexpr std::int64_t kOffsetJump = 240;

bool sameDirection(Vector3 a, Vector3 b)
{
    return std::abs(a.x - b.x) <= kMoveDirectionEpsilon && std::abs(a.y - b.y) <= kMoveDirectionEpsilon
           && std::abs(a.z - b.z) <= kMoveDirectionEpsilon;
}

} // namespace

CharacterSync::CharacterSync(std::shared_ptr<Replica> replica) : replica_(std::move(replica))
{
    if (const char* form = std::getenv("LUNAR_NET_PHYSICS"); form && std::string_view(form) == "85")
        motionForm = MotionForm::Physics; // research: the older motion packet, for comparison
}

std::uint32_t CharacterSync::tick(std::uint64_t nowMs) const
{
    if (!firstFrameMs_ || nowMs < *firstFrameMs_)
        return 0;
    return static_cast<std::uint32_t>(static_cast<double>(nowMs - *firstFrameMs_) * kIsrTicksPerSecond / 1000.0);
}

std::optional<CharacterSync::IsrLayout> CharacterSync::isrLayout(const BasePart& root) const
{
    const Schema* schema = replica_->schema();
    const auto cls = schema ? schema->classId(root.className()) : std::nullopt;
    if (!cls)
        return std::nullopt;
    const auto isr
        = std::ranges::find(schema->isrClasses, static_cast<std::uint32_t>(*cls), &Schema::IsrClass::classId);
    if (isr == schema->isrClasses.end() || isr->properties.size() > 64)
        return std::nullopt;
    for (std::size_t bit = 0; bit < isr->properties.size(); ++bit) {
        const std::uint32_t pid = isr->properties[bit];
        if (pid < schema->properties.size() && schema->properties[pid].type == nettype::ReplicationPV)
            return IsrLayout{*cls, static_cast<std::uint16_t>(isr->properties.size()), bit};
    }
    return std::nullopt;
}

// When the server's tick last changed, on our clock: what we estimate its
// tick from (it runs at the same 240 a second).
void CharacterSync::followServerTick(std::uint64_t nowMs)
{
    const auto& heard = replica_->runtime.serverIsrTick;
    if (heard && heard != serverTick_) {
        serverTick_ = heard;
        serverTickAtMs_ = nowMs;
    }
}

CharacterSync::~CharacterSync()
{
    stateChanged_.disconnect();
}

std::optional<InstanceId> CharacterSync::idOf(Known& known, const Instance* instance) const
{
    if (!instance)
        return std::nullopt;
    if (known.instance.lock().get() != instance || !known.id) {
        known.instance = instance->weak_from_this();
        known.id = replica_->idOf(instance);
    }
    return known.id;
}

std::optional<Item> CharacterSync::propertyItem(const Instance& instance, InstanceId id, std::string_view property,
                                                Value value) const
{
    const Schema* schema = replica_->schema();
    const auto cls = schema ? schema->classId(instance.className()) : std::nullopt;
    const auto pid = cls ? schema->propertyId(*cls, property) : std::nullopt;
    if (!pid)
        return std::nullopt;
    ChangePropertyItem item;
    item.id = id;
    item.propertyId = *pid;
    item.value = std::move(value);
    return Item(std::move(item));
}

std::vector<CharacterSync::Outgoing> CharacterSync::frame(Model& character, std::uint64_t nowMs,
                                                          std::span<const std::vector<Value>> animatorUpdates,
                                                          std::span<const BasePart* const> owned)
{
    std::vector<Outgoing> out;
    const Schema* schema = replica_->schema();
    if (!schema)
        return out;
    auto* humanoid = dynamic_cast<Humanoid*>(character.findFirstChildOfClass("Humanoid"));
    const BasePart* root = humanoid ? humanoid->rootPart() : nullptr;
    if (humanoid != synced_) {
        // A new character: the server hears its Humanoid afresh.
        synced_ = humanoid;
        stateChanged_.disconnect();
        states_.clear();
        if (humanoid)
            stateChanged_ = humanoid->events.stateChanged.connect(
                [this](Enum::HumanoidStateType, Enum::HumanoidStateType now) { states_.push_back(now); });
        state_.reset();
        jump_.reset();
        moveDirection_.reset();
        lastPhysicsMs_.reset();
    }
    if (!firstFrameMs_)
        firstFrameMs_ = nowMs;
    followServerTick(nowMs);
    const std::uint32_t nowTick = tick(nowMs);
    const std::optional<IsrLayout> isr
        = motionForm == MotionForm::Isr && root ? isrLayout(*root) : std::optional<IsrLayout>{};
    const auto state = humanoid ? humanoid->getState() : Enum::HumanoidStateType::None;
    // The packet's state field takes 0..16; the receiver refuses more (0x105580298) [code].
    const bool stateValid = static_cast<std::uint32_t>(state) <= static_cast<std::uint32_t>(Enum::HumanoidStateType::Physics);

    // The motion of what we simulate: the root while it's ours, and the other
    // assemblies the server gave us.
    struct Moving {
        InstanceId id;
        const BasePart* part = nullptr;
        std::uint8_t state = 0; // the 0x85 mechanism's Humanoid state: the root's, 0 for the rest (GUESS)
    };
    std::vector<Moving> moving;
    if (!lastPhysicsMs_ || (nowMs >= *lastPhysicsMs_ && static_cast<double>(nowMs - *lastPhysicsMs_) >= period_)) {
        if (const auto rootId = idOf(root_, root); rootId && ownsRoot && stateValid && !root->anchored())
            moving.push_back({*rootId, root, static_cast<std::uint8_t>(state)});
        for (const BasePart* part : owned)
            if (part && part != root && !part->anchored())
                if (const auto id = replica_->idOf(part))
                    moving.push_back({*id, part});
    }
    IsrGroup isrGroup;
    PhysicsPacket physics;
    for (const Moving& m : moving) {
        PhysicsPV pv;
        pv.cframe = m.part->cframe();
        pv.linearVelocity = m.part->assemblyLinearVelocity();
        pv.angularVelocity = m.part->assemblyAngularVelocity();
        if (m.part == root)
            lastSent = pv;
        if (const auto layout = motionForm == MotionForm::Isr ? isrLayout(*m.part) : std::nullopt) {
            isrGroup.instances.push_back(
                ownPartIsrState(m.id, layout->classId, layout->propertyCount, layout->pvBit, pv));
        } else if (physics.records.empty()) {
            // The receiver drops motion stamped no later than the last it took (0x1055802eb) [code].
            const std::uint64_t timestamp = std::max(nowMs, lastTimestamp_ + 1);
            lastTimestamp_ = timestamp;
            physics = onePartPacket(timestamp, ++sequence_, /*serverToClient=*/false, m.id, pv, m.state);
            physics.timestampIdFirst = (replica_->capabilities & kTimestampIdFirstCapability) != 0;
        } else { // another record in the same packet
            physics.records.push_back(std::move(onePartPacket(0, 0, false, m.id, pv, m.state).records.front()));
        }
    }
    if (!moving.empty()) {
        lastPhysicsMs_ = nowMs;
        if (!isrGroup.instances.empty()) {
            if (serverTick_) {
                // Its tick now, as we guess it, and ours minus that.
                const double elapsed = static_cast<double>(nowMs - std::min(nowMs, serverTickAtMs_));
                const auto server = static_cast<std::int64_t>(*serverTick_)
                                    + static_cast<std::int64_t>(elapsed * kIsrTicksPerSecond / 1000.0);
                const std::int64_t target = std::clamp<std::int64_t>(static_cast<std::int64_t>(nowTick) - server,
                                                                     std::numeric_limits<std::int32_t>::min() + 1,
                                                                     std::numeric_limits<std::int32_t>::max() - 1);
                if (!offset_ || std::abs(target - *offset_) > kOffsetJump)
                    offset_ = static_cast<std::int32_t>(target);
                else
                    offset_ = static_cast<std::int32_t>(*offset_
                                                        + std::clamp(target - *offset_, -kOffsetSlew, kOffsetSlew));
            }
            IsrPacket packet;
            packet.tick = nowTick;
            packet.clockOffset = offset_.value_or(kIsrNoClockOffset);
            packet.groups.push_back(std::move(isrGroup));
            out.push_back({encodeIsrPacket(packet), Reliability::Unreliable});
            ++isrPackets;
        }
        if (!physics.records.empty()) {
            out.push_back({encodePhysicsPacket(physics, replica_->framing), Reliability::Unreliable});
            ++physicsPackets;
        }
    }

    // The Humanoid's properties and the Animator's tracks, in one ID_DATA.
    std::vector<Item> items;
    if (const auto humanoidId = idOf(humanoid_, humanoid)) {
        auto change = [&](std::string_view property, Value value) {
            if (auto item = propertyItem(*humanoid, *humanoidId, property, std::move(value))) {
                items.push_back(std::move(*item));
                ++propertyChanges;
            }
        };
        // Every state it went through since the last frame, then where it is.
        states_.push_back(state);
        for (const Enum::HumanoidStateType s : states_)
            if (s <= Enum::HumanoidStateType::Physics && state_ != static_cast<double>(s)) {
                state_ = static_cast<double>(s);
                change("NetworkHumanoidState", *state_);
            }
        states_.clear();
        if (jump_ != humanoid->jump()) {
            // Nothing to say about a Jump that was never set.
            if (jump_ || humanoid->jump())
                change("JumpReplicate", humanoid->jump());
            jump_ = humanoid->jump();
        }
        const Vector3 direction = humanoid->moveDirection();
        if (moveDirection_ ? !sameDirection(*moveDirection_, direction) : direction.magnitude() > 0.0f) {
            moveDirection_ = direction;
            change("MoveDirectionInternal", direction);
        }
    }
    // The server moved parts of ours: each change acknowledged with our count
    // of them, or the server keeps dropping our motion (ServerReplicator
    // 0x10568712a: its count and ours must match) [code]. Ours are our
    // character's and those the server gave us (GUESS for the latter, as
    // their motion is ours to send too); others' parts are none of ours to
    // acknowledge.
    for (const Replica::CFrameChange& change : replica_->cframeChanges) {
        const InstancePtr part = replica_->find(change.id);
        if (!part || !(part->isDescendantOf(&character) || replica_->ownedHere(change.id)))
            continue;
        auto it = std::ranges::find_if(ackCounts_, [&](const AckCount& a) {
            return a.id == change.id && a.propertyId == change.propertyId;
        });
        if (it == ackCounts_.end())
            it = ackCounts_.insert(ackCounts_.end(), AckCount{change.id, change.propertyId, 0});
        it->version = change.restart ? 0 : it->version + 1;
        CFrameAckItem ack;
        ack.instance = change.id;
        ack.propertyId = change.propertyId;
        ack.hasVersion = true;
        ack.version = it->version;
        // Our physics tick: the genuine client's acks carry its ISR tick of the
        // moment (steps 139, 163 between its ISR ticks 123..175) [capture].
        ack.step = nowTick;
        items.emplace_back(RuntimeItem{ack});
        ++cframeAcks;
    }
    replica_->cframeChanges.clear();
    const Instance* animator = humanoid ? humanoid->findFirstChildOfClass("Animator") : nullptr;
    if (!animatorUpdates.empty() && idOf(animator_, animator))
        for (const std::vector<Value>& arguments : animatorUpdates) {
            std::vector<RemoteValue> remote;
            remote.reserve(arguments.size());
            for (const Value& v : arguments)
                remote.push_back(remoteValue(v));
            if (auto item = replica_->eventItem(*animator, "OnCombinedUpdate", remote)) {
                items.emplace_back(std::move(*item));
                ++animatorEvents;
            }
        }
    if (isr && !items.empty())
        items.insert(items.begin(), Item(RuntimeItem{IsrTimestampItem{static_cast<std::int32_t>(nowTick)}}));
    if (!items.empty())
        out.push_back({encodeData(items, *schema, replica_->context(), replica_->framing,
                                  ItemContext{false, replica_->capabilities}),
                       Reliability::ReliableOrdered});
    return out;
}

} // namespace lunar::net
