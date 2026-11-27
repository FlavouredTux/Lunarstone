#pragma once

#include <lunar/Math.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Values.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar::net {

// Physics replication (ROBLOX-LAYER.md 6): the quantized motion a server
// sends for moving parts and a network owner sends back, carried as network
// type ReplicationPV (0x39) inside physics packets (0x85, behind the 0x1b
// timestamp). Every quantizer below was pinned to RobloxPlayer 0.742's own
// code under unicorn (emu/t_pv.py, vectors/replication_pv.json) [emu].

// -- the quantizers (all [emu])

// Vector3 position, variable width by magnitude (0x1054e7088 / 0x1054e73fc):
// 1-byte header (3-bit exponent-ish + 3 sign bits) then 10-, 16- or 21-bit
// signed components. Reads back with readPvPosition.
void writePvPosition(NetworkStream& out, Vector3 v);
Vector3 readPvPosition(NetworkStream& in);

// Vector3 velocity, 11-bit components (0x1054e7b92 / 0x1054e7d36): one zero
// byte when every component is below 0.001, else a 1-byte header + u32.
void writePvVelocity(NetworkStream& out, Vector3 v);
Vector3 readPvVelocity(NetworkStream& in);

// The compact 6-bit velocity used for non-root parts (0x1054e7dda /
// 0x1054e7f8c): one zero byte, or 3 bytes.
void writePvVelocity6(NetworkStream& out, Vector3 v);
Vector3 readPvVelocity6(NetworkStream& in);

// A 10-bit smallest-three quaternion (0x1054e78de / 0x1054e7a98): one u32,
// for non-root part rotations.
void writeQuaternion10(NetworkStream& out, const CFrame& cf);
CFrame readQuaternion10(NetworkStream& in);

// -- ReplicationPV (network type 0x39): position + rotation + linear and
// angular velocity, each dropped when it is near zero (0x1057ad024 /
// 0x1057ad1de).
struct PhysicsPV {
    CFrame cframe;
    Vector3 linearVelocity;
    Vector3 angularVelocity;
    bool operator==(const PhysicsPV&) const = default;
};

// The FInt zero cutoffs the writer uses (defaults from the static init at
// 0x10558e9ff, not stored in the image; GUESS for the numbers): a component
// below 10^exponent is sent as zero, and an axis-aligned rotation within
// 10^orientTolerance of an axis goes as a rotation id instead of a quaternion.
inline constexpr int kPvTranslationZeroCutoffExponent = -2;
inline constexpr int kPvRotationOrientIdToleranceExponent = -6;
inline constexpr int kPvLinearVelocityZeroCutoffExponent = -2;
inline constexpr int kPvRotationalVelocityZeroCutoffExponent = -2;

void writeReplicationPV(NetworkStream& out, const PhysicsPV& pv);
PhysicsPV readReplicationPV(NetworkStream& in);
// The flags byte the writer would emit (bit 7 position-zero, 6 linvel-zero,
// 5 angvel-zero, low 5 the rotation encoding), for the decoder's printout.
std::uint8_t replicationPvFlags(const PhysicsPV& pv);
// A readable one-line rendering of a PV, for the decoder.
std::string pvText(const PhysicsPV& pv);

// -- the physics packet (ID_TIMESTAMP 0x1b + ID_PHYSICS 0x85), both
// directions (ROBLOX-LAYER.md 6; notes-physics.md section 2). Writer
// PhysicsSenderS2 0x105587668, reader 0x1055817ee. The server sends a frame
// list per part (with ages, for interpolation); the owner client sends a
// single PV per owned assembly.

// A Motor6D-style joint transform in the assembly (usually none for a single part).
struct PhysicsJoint {
    bool hasTranslation = false;
    bool hasRotation = false;
    Vector3 translation;
    CFrame rotation;
    bool operator==(const PhysicsJoint&) const = default;
};

// One motion sample of a part. Server to client: `age` seconds back from the
// packet time, plus the root ref or a basis index. Client to server: one of
// these, `age` ignored.
struct PhysicsFrame {
    float age = 0.0f;
    PhysicsPV pv;
    InstanceId rootRef;        // root part: the replication-root ref
    std::uint8_t basis = 0xff; // non-root part: basis index (0xff = world space)
    bool operator==(const PhysicsFrame&) const = default;
};

struct PhysicsPartState {
    InstanceId ref; // written before the body for non-root parts; the root's is the record's part
    std::vector<PhysicsJoint> joints;
    std::vector<PhysicsFrame> frames; // >= 1 (server->client), exactly 1 (client->server)
    bool operator==(const PhysicsPartState&) const = default;
};

struct PhysicsMechanism {
    std::uint8_t humanoidState = 0;      // Enum.HumanoidStateType value (GUESS)
    bool compactVelocities = false;      // mechanism header bit 6: 3-byte velocities
    bool refBasis = false;               // mechanism header bit 7: non-root parts carry a ref (capability 45)
    std::vector<PhysicsPartState> parts; // parts[0] is the assembly root
    bool operator==(const PhysicsMechanism&) const = default;
};

struct PhysicsRecord {
    InstanceId part;
    std::optional<PhysicsMechanism> mechanism; // nullopt: no mechanism (hasMechanism byte 0)
    bool operator==(const PhysicsRecord&) const = default;
};

struct PhysicsPacket {
    std::uint64_t timestamp = 0;   // sender clock, milliseconds
    std::uint64_t sequence = 0;    // per-sender counter
    bool serverToClient = true;    // frame lists (true) or single PVs (false)
    bool timestampIdFirst = false; // capability bit 25: 0x85 before the sequence, else after
    std::vector<PhysicsRecord> records;
    bool operator==(const PhysicsPacket&) const = default;
};

std::vector<std::uint8_t> encodePhysicsPacket(const PhysicsPacket& packet, const Framing& framing = {});
// The direction (frame lists vs single PVs) is not on the wire, so the caller
// states it (the flow direction in a capture, the role in the loop).
// `tree`, if given, gets the readable form.
std::expected<PhysicsPacket, std::string> decodePhysicsPacket(std::span<const std::uint8_t> message,
                                                              bool serverToClient, const Framing& framing = {},
                                                              Node* tree = nullptr);

// The common case for the local loop: a single-part assembly, one frame.
// `age` is used only server to client.
PhysicsPacket onePartPacket(std::uint64_t timestamp, std::uint64_t sequence, bool serverToClient, InstanceId part,
                            const PhysicsPV& pv, std::uint8_t humanoidState = 0, float age = 0.0f);

// -- ISR, instance state replication (ID_ISR 0xa1): the same motion as a
// property, BasePart.ReplicationPV, the way live servers send it [capture:
// every physics update in three recordings of a busy game came as 0xa1, none
// as 0x85]. The schema's ISR section names each class's ISR properties: a
// part's PhysicsRepRootPart, PhysicsRepRootRef and ReplicationPV, a Motor6D's
// ReplicateCurrentAngle6D/Offset6D/Angle, an Animator's FacsReplicationData.
//
//   u8  0xa1
//   u32 tick           the server's physics step, 240 a second [capture]
//   u32 groups         [capture: always the groups that follow]
//   i32 clock offset   the sender's tick minus the receiver's, as the
//                      sender estimates it: a client's (from the server's
//                      ticks it saw) [capture: genuine client packets,
//                      tick - offset runs a few ticks ahead of the server's
//                      last ISR tick]; 0x7fffffff, "none", from a server
//                      [capture]. The server's step conversion subtracts it
//                      (0x104b656ba) [code]
//   groups x {         writer InstanceStateReplicator::vfn_3 0x104b69fb6 [code]
//     u16 count        instances in the group, written after them
//     u8  kind         below 4 (reader 0x104b89678); which ReplicationPV
//                      coder the group's parts use (0x10550b6c5 reads,
//                      0x10550fd69 writes): 0 the 0x39 one (writeReplicationPV),
//                      1 the same with compact velocities, 2 compact with
//                      10-bit quaternions [code; capture: a game lobby's
//                      kind 0 and kind 1 states read that way]
//     u8  age          how many ticks before `tick` the state was taken,
//                      clamped to 0..255 (the writer subtracts a time it
//                      keeps per instance from the current tick, 0x104b89636
//                      [code]; that it is the state's age is [capture]: with
//                      it a walking character's samples space out exactly by
//                      its speed, without it they jump)
//     count x {
//       ref instance
//       u16 class      schema class index (reader 0x1055087c4) [code]
//       u16 n          how many ISR properties the class has [capture]
//       u64 dirty      bit i: the class's i-th ISR property follows [capture]
//       u16 length     of the property data [capture]
//       the dirty properties in bit order, each as its network type
//       (GUESS but for ReplicationPV, the only one seen with a known type:
//       written as writeReplicationPV does [capture: every part's data in
//       the recordings reads to its end that way])
//     }
//   }
inline constexpr double kIsrTicksPerSecond = 240.0; // [capture: 0x3a0 ticks in 3.87 s, and so on]

struct IsrInstanceState {
    InstanceId instance;
    std::uint16_t classId = 0;
    std::uint16_t propertyCount = 0;
    std::uint64_t dirty = 0;
    std::vector<std::uint8_t> data;
    bool operator==(const IsrInstanceState&) const = default;
};

struct IsrGroup {
    std::uint8_t kind = 0;
    std::uint8_t age = 0;
    std::vector<IsrInstanceState> instances;
    bool operator==(const IsrGroup&) const = default;
};

inline constexpr std::int32_t kIsrNoClockOffset = 0x7fffffff;

struct IsrPacket {
    std::uint32_t tick = 0;
    std::int32_t clockOffset = kIsrNoClockOffset;
    std::vector<IsrGroup> groups;
    bool operator==(const IsrPacket&) const = default;
};

// The whole message, id byte first (0xa1 is not remapped: it's the same both ways).
std::vector<std::uint8_t> encodeIsrPacket(const IsrPacket& packet);
std::expected<IsrPacket, std::string> decodeIsrPacket(std::span<const std::uint8_t> message);

// What a client sends of the part it owns, the way the genuine one does
// [capture]: one group (kind 0, age 0) with the part's state, only its
// ReplicationPV dirty, written by writeReplicationPV. `propertyCount` and
// `pvBit` come from the schema's ISR section for the part's class.
IsrPacket ownPartIsrPacket(std::uint32_t tick, std::int32_t clockOffset, InstanceId part, std::uint16_t classId,
                           std::uint16_t propertyCount, std::size_t pvBit, const PhysicsPV& pv);
// That part's state alone, for a group of several.
IsrInstanceState ownPartIsrState(InstanceId part, std::uint16_t classId, std::uint16_t propertyCount, std::size_t pvBit,
                                 const PhysicsPV& pv);

// One ISR property of an instance state: its index among the class's ISR
// properties (the dirty bit), the global property id when the schema has it,
// and its bytes within the data.
struct IsrProperty {
    std::size_t bit = 0;
    std::optional<std::uint32_t> propertyId;
    std::span<const std::uint8_t> bytes;
};
// Splits an instance state's data into its properties by the schema's types
// (ReplicationPV, or any type readValue knows). Stops at the first property
// it can't size; `complete` says whether the data was read to its end.
std::vector<IsrProperty> isrProperties(const IsrInstanceState& state, const Schema& schema, bool* complete = nullptr);

} // namespace lunar::net
