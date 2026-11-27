#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Signal.hpp>
#include <lunar/Value.hpp>
#include <lunar/net/Datagram.hpp>
#include <lunar/net/Physics.hpp>
#include <lunar/net/Replication.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lunar {
class Model;
class Humanoid;
class BasePart;
} // namespace lunar

namespace lunar::net {

// The character a client owns, as that client tells the server about it
// (ROBLOX-LAYER.md 9). A live server gives a player's client the network
// ownership of its character's HumanoidRootPart (BasePart.NetworkOwnerV3 =
// the client's peer id [capture]) and from then on only follows what the
// client sends; the other clients see what the server relays. Each frame:
//
//   - the root's motion. On a server whose schema has ISR (every live one
//     [capture]) as the genuine client sends it: an ID_ISR (0xa1) with one
//     group, kind 0, age 0, holding the root's state with only its
//     ReplicationPV, unreliable, at 15 a second [client capture: 142
//     client packets 16..30 ticks apart]. The header carries our physics
//     tick (240 a second from our first frame) and our clock offset from
//     the server's ticks, which the server needs to place our motion and our
//     acknowledgements' steps in its own time (0x104b656ba) [code].
//     LUNAR_NET_PHYSICS=85 sends the older form instead, also without ISR:
//   - the root's motion as a physics packet (ID_TIMESTAMP 0x1b + 0x85), one
//     record per packet: the root's assembly as a root-only mechanism whose
//     header carries the Humanoid's state, with a single ReplicationPV and
//     no joints, the form the owner client writes (PhysicsSenderS2: record
//     writer 0x1055878dc, mechanism 0x10558857c, root body 0x1055889ac)
//     [code]. A live server that uses ISR for its own replication still
//     reads 0x85 from its clients: every ServerReplicator makes the 0x85
//     receiver whatever the ISR flags (0x10567dc6e..0x10567dd5c) [code]. At
//     S2PhysicsSenderRate, 15 a second (the FInt's default, 0x1016dec9a)
//     [code]; unreliable, like the server's ISR [capture; GUESS for 0x85];
//   - the Humanoid's network properties the server accepts from its owner
//     (the client-replicated list at 0x1057a1158 [code]) when they change:
//     NetworkHumanoidState (the genuine client sends it on state changes
//     [capture]), JumpReplicate (true while Jump is set; servers relay it
//     true then false a frame later [capture]) and MoveDirectionInternal
//     (Humanoid.MoveDirection; relayed every few frames while it turns
//     [capture]), as Change Property items in a reliable ID_DATA;
//   - its Animator's track changes as Animator.OnCombinedUpdate event
//     invocations (avatar::TrackSender builds the arguments), reliable like
//     the genuine client's [capture].
// With ISR every ID_DATA starts with an ISR Timestamp item holding our tick,
// as the genuine client's do [capture: 762 of them, each the client's tick
// at the time].
class CharacterSync {
public:
    explicit CharacterSync(std::shared_ptr<Replica> replica);
    ~CharacterSync();
    CharacterSync(const CharacterSync&) = delete;
    CharacterSync& operator=(const CharacterSync&) = delete;

    struct Outgoing {
        std::vector<std::uint8_t> message;
        Reliability reliability = Reliability::ReliableOrdered;
    };

    // Root motion packets a second (S2PhysicsSenderRate).
    static constexpr double kPhysicsRate = 15.0;
    // How the root's motion goes out: ID_ISR when the server's schema has
    // ISR for the root's class, else (or with LUNAR_NET_PHYSICS=85) 0x85.
    enum class MotionForm { Isr, Physics };
    MotionForm motionForm = MotionForm::Isr;
    void setPhysicsRate(double perSecond) { period_ = 1000.0 / perSecond; }

    // One frame of the character this client owns, after its physics, at
    // `nowMs` on the connection's clock (the one its pings answer with:
    // physics timestamps must keep rising on it). `animatorUpdates` are
    // OnCombinedUpdate argument lists for its Animator. `owned` are the
    // roots of the other assemblies the server gave us to simulate
    // (Replica::ownedHere): their motion goes out with the root's, in the
    // same packet, as does our acknowledgement of the server moving them.
    // GUESS: one group (one 0x85 packet) for them all, as the states are
    // all taken at the same tick; no capture had a client owning more than
    // its root. Nothing goes out for parts or Humanoid the server never
    // sent us (no instance id).
    std::vector<Outgoing> frame(Model& character, std::uint64_t nowMs,
                                std::span<const std::vector<Value>> animatorUpdates = {},
                                std::span<const BasePart* const> owned = {});

    // A physics packet goes out at the next frame whatever the rate (a
    // teleport, a new character).
    void sendPhysicsNow() { lastPhysicsMs_.reset(); }
    // Whether the root's motion is ours to send. False while the server
    // moves our root itself (it sends its motion): the server would drop
    // ours anyway (ServerReplicator vfn_143 [code]).
    bool ownsRoot = true;

    // What went out, for logs.
    std::size_t physicsPackets = 0, isrPackets = 0, propertyChanges = 0, animatorEvents = 0, cframeAcks = 0;
    // Our physics tick at `nowMs` (240 a second from the first frame), and
    // our clock offset from the server's as last sent (none before we heard one).
    std::uint32_t tick(std::uint64_t nowMs) const;
    std::optional<std::int32_t> clockOffset() const { return offset_; }
    std::optional<PhysicsPV> lastSent; // the root's last motion sent

private:
    // An instance's network id, looked up once (Replica::idOf walks every instance).
    struct Known {
        std::weak_ptr<const Instance> instance;
        std::optional<InstanceId> id;
    };
    std::optional<InstanceId> idOf(Known& known, const Instance* instance) const;
    std::optional<Item> propertyItem(const Instance& instance, InstanceId id, std::string_view property,
                                     Value value) const;

    // The root's class's ISR layout in the server's schema, if it has one.
    struct IsrLayout {
        std::uint16_t classId = 0, propertyCount = 0;
        std::size_t pvBit = 0;
    };
    std::optional<IsrLayout> isrLayout(const BasePart& root) const;
    void followServerTick(std::uint64_t nowMs);

    std::shared_ptr<Replica> replica_;
    double period_ = 1000.0 / kPhysicsRate;
    std::optional<std::uint64_t> firstFrameMs_;
    std::optional<std::uint32_t> serverTick_;
    std::uint64_t serverTickAtMs_ = 0;
    std::optional<std::int32_t> offset_;
    std::optional<std::uint64_t> lastPhysicsMs_;
    std::uint64_t lastTimestamp_ = 0;
    std::uint64_t sequence_ = 0;
    Known root_, humanoid_, animator_;
    // What the server last heard of the Humanoid. Its states as they come
    // (StateChanged): a Jumping that lasts less than a frame still goes out.
    const Humanoid* synced_ = nullptr;
    Connection stateChanged_;
    std::vector<Enum::HumanoidStateType> states_;
    std::optional<double> state_;
    std::optional<bool> jump_;
    std::optional<Vector3> moveDirection_;
    // Our count of the server's changes to each (part, property) of our
    // character, which each CFrame Acknowledgement carries (0x1054d787a).
    struct AckCount {
        InstanceId id;
        std::uint16_t propertyId = 0;
        std::int32_t version = 0;
    };
    std::vector<AckCount> ackCounts_;
};

} // namespace lunar::net
