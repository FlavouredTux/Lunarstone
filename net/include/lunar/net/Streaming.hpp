#pragma once

#include <lunar/net/Describe.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Values.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lunar::net {

// The ID_DATA items a server and a client exchange after the join: pings and
// their answers, the character request, acknowledgements, the client's
// streaming bookkeeping and the StreamingEnabled items. Layouts are the
// client's own (RobloxPlayer 0.742): item writers are `Item::vfn_2` and the
// readers the static functions `ClientReplicator::vfn_164` dispatches to;
// notes-stream.md in the research directory has the addresses and the
// confidence of every field.

// -- capability bits the runtime items consult (byte i of the u64 at rep+0x3ca0 is bits 8i..8i+7)
namespace caps {
inline constexpr std::uint64_t CFrameAckStep = 1ULL << 33;       // CFrame Ack carries a u32 step
inline constexpr std::uint64_t CapacityUpdateExtra = 1ULL << 37; // Client Quota Stream Update: 2 bits + u32
inline constexpr std::uint64_t FocusExtra = 1ULL << 39;          // Stream Replication Focus: trailing u32
inline constexpr std::uint64_t RegionRemovalA = 1ULL << 42;      // Region Removal variants (0x1056049ea)
inline constexpr std::uint64_t RegionRemovalB = 1ULL << 43;      // (0x1056049fc)
inline constexpr std::uint64_t Tracer = 1ULL << 49;              // pings may carry a tracer payload
inline constexpr std::uint64_t KickMetadata = 1ULL << 50;        // ID_KICK carries a metadata JSON
inline constexpr std::uint64_t PingMemory = 1ULL << 51;          // Ping flag 0x10: f32 memory
inline constexpr std::uint64_t StreamInfoExtra = 1ULL << 56;     // Streaming Info: trailing i32
inline constexpr std::uint64_t RegionRemovalC = 1ULL << 59;      // Region Removal: trailing u32
} // namespace caps

// -- the item type ids (ROBLOX-LAYER.md 3), the ones this file handles
namespace item {
inline constexpr std::uint8_t Ping = 0x05;
inline constexpr std::uint8_t PingBack = 0x06;
inline constexpr std::uint8_t RequestCharacter = 0x08;
inline constexpr std::uint8_t PmcConfig = 0x09; // "Other Items 3": RBX::Network::DeserializedPmcConfigItem
inline constexpr std::uint8_t CFrameAck = 0x0a;
inline constexpr std::uint8_t StreamData = 0x0d;
inline constexpr std::uint8_t RegionRemoval = 0x0e;
inline constexpr std::uint8_t InstanceRemoval = 0x0f;
inline constexpr std::uint8_t Stats = 0x11;
inline constexpr std::uint8_t StreamingInfo = 0x14;
inline constexpr std::uint8_t StreamTerrain = 0x17;
inline constexpr std::uint8_t StreamPrefetchInfo = 0x18;
inline constexpr std::uint8_t StreamPrefetchRequest = 0x19;
inline constexpr std::uint8_t StreamFocus = 0x1a;
inline constexpr std::uint8_t Destroy = 0x1b;
inline constexpr std::uint8_t ClientCapacityUpdate
    = 0x1c; // "Client Quota Stream Update": ClientStreamCapacityUpdateItem
inline constexpr std::uint8_t PersistentPlayerAdd = 0x1d;
inline constexpr std::uint8_t PersistentPlayerRemove = 0x1e;
inline constexpr std::uint8_t ClientQoS = 0x1f;
inline constexpr std::uint8_t IsrTimestamp = 0x23;
inline constexpr std::uint8_t StreamFocusRemove = 0x24;
inline constexpr std::uint8_t StreamPinData = 0x25;
inline constexpr std::uint8_t StreamFocusFrustum = 0x27;       // RTTI: StreamReplicationFocusFrustumItem
inline constexpr std::uint8_t StreamFocusFrustumRemove = 0x28; // RTTI: StreamReplicationFocusFrustumRemoveItem
inline constexpr std::uint8_t InstanceIntent = 0x29;
inline constexpr std::uint8_t RegionTerrainLodRemoval = 0x2b;
} // namespace item

// A streaming region id: three ints, each as an i8 when all three fit, else
// i32 (writer 0x1057ac0ae, reader 0x1057ac1a8) [code].
struct RegionId {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const RegionId&) const = default;
};
void writeRegionId(NetworkStream& out, RegionId id);
RegionId readRegionId(NetworkStream& in);
std::string regionText(RegionId id);

// Ping (0x05). Both peers send one periodically; the other side answers with
// a PingBack. The body depends on the SENDER's role: a server's ping carries
// an fps triple and the xor'd u32 (flags 6), a client's doesn't (flags 0)
// [code: writer 0x1055d9af8 and reader 0x1055d9ea4 agree once the "network
// version delta" test is read as the is-client test; emu: ping_items.json].
struct PingItem {
    std::uint8_t flags = 0;     // bit 1|2 server-side body, bit 3 tracer payload, bit 4 memory f32
    std::uint64_t time = 0;     // RakNet::GetTimeMS() when the ping was made; echoed back
    std::uint32_t sendMs = 0;   // GetTimeMS() when it was written
    std::array<float, 3> fps{}; // server pings only (GUESS: render/physics/heartbeat fps)
    std::uint32_t mask = 0;     // OR of client flag globals (GUESS: feature mask); 0 under emulation
    std::uint32_t extra = 0;    // item+0x58, 0 from the client; wire-inverted when (time & 0x20)
    std::uint8_t kind = 0;      // 0, or 1 for the periodic "special" ping (reader rejects > 1)
    std::uint32_t xorred = 0;   // flags & 4: u32 ^ (u32)time (GUESS: a sequence number)
    float memoryMb = 0.0f;      // flags & 0x10 with capability 51
    bool operator==(const PingItem&) const = default;
};
// PingBack (0x06): the answer, echoing the ping's time, send time and kind [code: 0x1055d8f16].
struct PingBackItem {
    std::uint8_t flags = 1; // bit 0 set; bit 3 tracer
    std::uint64_t time = 0;
    std::uint32_t pingSendMs = 0;      // the ping's sendMs
    std::uint32_t recvMs = 0;          // GetTimeMS() when the ping was read
    std::uint32_t zero = 0;            // constructor argument, 0 on the client
    std::uint32_t nowMs = 0;           // GetTimeMS() when the PingBack was written
    std::uint32_t counter = 0;         // rep+0x1774 (GUESS: a packet counter)
    std::uint32_t lastPacketBytes = 0; // rep+0x2498 low half: the last received packet's length
    std::uint32_t lastPacketHigh = 0;  // rep+0x2498 high half
    std::uint32_t mask = 0;
    std::uint32_t extra = 0; // wire-inverted when (time & 0x20)
    std::uint8_t kind = 0;   // echo of the ping's kind
    bool operator==(const PingBackItem&) const = default;
};
// Request Character (0x08): sent by the client when ID_PLACEID_VERIFICATION says ok
// [code: ClientReplicator 0x1054c8810 case 0x92; writer 0x1054dc2fc].
struct RequestCharacterItem {
    std::uint32_t playerFlags = 0; // OR of the Players service's flag list | FInt 0x1078a5890 (GUESS: 0)
    std::string appearance;        // string32 from the Players service (GUESS: the character appearance)
    InstanceId player;             // the local Player instance
    bool operator==(const RequestCharacterItem&) const = default;
};
// PMC config (0x09, "Other Items 3"): one byte that must be 4 [code: 0x1057aafb6].
struct PmcConfigItem {
    std::uint8_t value = 4;
    bool operator==(const PmcConfigItem&) const = default;
};
// CFrame Ack (0x0a): the client acknowledges a CFrame-type property change it
// applied [code: created in 0x1054d7526, writer 0x1054dd600].
struct CFrameAckItem {
    InstanceId instance;
    std::uint16_t propertyId = 0;
    // The client's count of the server's changes to that property of that
    // part (ClientReplicator vfn_137 0x1054d7c6e, from its rep+0x54f8 table):
    // the server takes our part's motion again once it matches its own count
    // (ServerReplicator 0x1056870bc records it, 0x10568712a compares) [code].
    bool hasVersion = false;
    std::int32_t version = 0;
    std::uint32_t step = 0; // capability 33: the client's physics world step (world+0xf0, 0x1054dd57d) [code]
    bool operator==(const CFrameAckItem&) const = default;
};
// Stream Data (0x0d): a Join Data (0x0b) body (u32 count, compressed instance
// blob) followed by the regions it covers [code: 0x1056e7194 -> 0x1056e4342,
// region list 0x1056e6fc2]. The blob's inner layout is the join-time decoder's;
// here it is kept raw (decompressed) and hex-dumped.
struct StreamRegionRef {
    std::uint8_t code = 0; // low 3 bits: 0 = explicit region, 1..3 = previous +1 on y/z/x, 4 = sentinel
    RegionId region;       // when code == 0
    bool operator==(const StreamRegionRef&) const = default;
};
struct StreamDataItem {
    std::uint32_t count = 0;        // instances in the blob
    std::vector<std::uint8_t> blob; // decompressed join-data body (empty when count == 0)
    std::vector<StreamRegionRef> regions;
    bool operator==(const StreamDataItem&) const = default;
};
// Region Removal (0x0e): the client's GC job reports a region it dropped and
// the instances it removed with it, so the server streams them again when
// the region is wanted again [code: built in 0x1056cb2f0, writer 0x1056d57ee]:
// region; u32 sequence; u32 n; u8 flags; [u32 extra]; n instance refs, inline
// when n <= DFInt StreamingRegionRemovalCompressThreshold (250, 0x1078b9328),
// else as one compressed block of them (flag bit 0).
inline constexpr std::uint32_t kRegionRemovalInlineRefs = 250;
struct RegionRemovalItem {
    RegionId region;
    std::uint32_t sequence = 0;      // item+0x80: GC job+0xed0, regions the client has removed so far, this one included
    std::vector<InstanceId> removed; // the item+0x50 list (its count is item+0x68)
    std::uint8_t flags = 0;          // bits 1-2: capability 42/43 variants; bit 0 (compressed refs) and bit 2
                                     // (extra follows) are the codec's own
    std::uint32_t extra = 0;         // capability 59 (item+0xb0)
    bool operator==(const RegionRemovalItem&) const = default;
};
// Instance Removal (0x0f): the client dropped this instance (and sends one per descendant) [code: 0x1056d0568].
struct InstanceRemovalItem {
    InstanceId instance;
    bool operator==(const InstanceRemovalItem&) const = default;
};
// Stats (0x11, server -> client): a versioned stats dump for the client's
// stats display [code: reader 0x1054d2a0a]. Only the head is decoded; the
// rest of the item is kept as bytes (the item is assumed to end its packet, GUESS).
struct StatsItem {
    std::int32_t version = 0;
    float a = 0.0f, b = 0.0f;   // version >= 9
    std::string block8, block7; // version >= 8 / >= 7: compressed strings
    std::vector<std::uint8_t> rest;
    bool operator==(const StatsItem&) const = default;
};
// Streaming Info (0x14): a region's version and radius [code: 0x105724f0a; "ShipsInNightGarbageCollection" logs].
struct StreamingInfoItem {
    RegionId region;
    std::int32_t radius = 0;
    std::uint32_t version = 0;
    std::int32_t extra = 0; // capability 56
    bool operator==(const StreamingInfoItem&) const = default;
};
// Stream Prefetch Info (0x18): the server's answer to a Stream Prefetch
// Request; the client logs "Validate Prefetch %d at [x, y, z]" with the low
// byte of b and the region, and finishes the request with that id [code: 0x1057258ae, 0x1054d5436].
struct StreamPrefetchInfoItem {
    RegionId region;
    std::int32_t a = 0; // item+0x2c (GUESS: the radius streamed, in regions)
    std::int32_t b = 0; // item+0x30: the request's id (read as a byte)
    bool operator==(const StreamPrefetchInfoItem&) const = default;
};
// Stream Prefetch Request (0x19): Player:RequestStreamAroundAsync. The client
// sends it (GCJob's StreamingInterface slot 0, 0x1056c57fe, builds it with
// the position, the timeout and the GC job's request counter +0x2d8, and
// queues it on rep+0x38d8) and the server answers with a Stream Prefetch Info
// carrying the id once the area is streamed; a server sends one too, for
// the client to keep the area [code: reader 0x1057250da, "Server Prefetch Request"].
struct StreamPrefetchRequestItem {
    Vector3 position;
    double timeout = 0; // seconds (item+0x50, the f64 argument of RequestStreamAroundAsync)
    std::int32_t id = 0;
    bool operator==(const StreamPrefetchRequestItem&) const = default;
};
// Stream Replication Focus (0x1a) [code: 0x105725240 + 0x1057244a4; "focusId={} completedRadius={}"].
struct StreamFocusItem {
    std::uint32_t focusId = 0;
    RegionId region;
    std::int32_t completedRadius = 0;
    std::optional<float> radius; // f32, -1 on the wire for none
    std::uint8_t flagA = 0, flagB = 0;
    std::uint32_t extra = 0; // capability 39
    bool operator==(const StreamFocusItem&) const = default;
};
struct StreamFocusRemoveItem {
    std::uint32_t focusId = 0;
    bool operator==(const StreamFocusRemoveItem&) const = default;
};
struct StreamFocusFrustumItem {
    std::uint32_t focusId = 0;
    std::array<float, 15> frustum{}; // 0x1057553f8: 15 floats (GUESS: 5 planes or position/axes/fov)
    bool operator==(const StreamFocusFrustumItem&) const = default;
};
struct StreamFocusFrustumRemoveItem {
    std::uint32_t focusId = 0;
    bool operator==(const StreamFocusFrustumRemoveItem&) const = default;
};
// Destroy (0x1b): instances removed by the server [code: 0x105695cf0].
struct DestroyItem {
    std::vector<InstanceId> instances;
    bool operator==(const DestroyItem&) const = default;
};
// Client Quota Stream Update (0x1c): the client's instance quota and the
// farthest region radius it wants streamed. The quota travels as the change
// since the last update: the server adds it up ("Received new client instance
// quota: %d, max region radius: %d", StreamJob::updateClientQuota 0x1056edea4)
// [code: 0x1054d64c0 builds it every 0.125 s at most and sends it when either
// changed, writer 0x1054dc940].
inline constexpr std::int16_t kUnlimitedRegionRadius = 0x7fff; // GC job+0x2cc's start value [code: 0x1056ba87c]
struct ClientCapacityUpdateItem {
    std::uint8_t memoryLimited = 0;  // 1 when memory ran low (the quota is then cut) [code]
    std::int32_t quotaDelta = 0;     // new quota - previous quota (instances)
    std::int16_t maxRegionRadius = 0; // regions (64 studs) around the focus; kUnlimitedRegionRadius: all
    std::uint8_t bits = 0;           // capability 37: 2 flag bits (gcjob+0x2d0, +0x2d1 << 1)
    std::uint32_t extra = 0;         // capability 37: gcjob+0x2d4
    bool operator==(const ClientCapacityUpdateItem&) const = default;
};
// Stream Model Persistent Player Add/Remove (0x1d/0x1e): a Player ref [code: 0x105725aa8 / 0x105725c28].
struct PersistentPlayerItem {
    bool add = true;
    InstanceId player;
    bool operator==(const PersistentPlayerItem&) const = default;
};
// Client QoS (0x1f): the security module's report; u8 kind (1 or 2), u8, string32 [code+emu].
struct ClientQoSItem {
    std::uint8_t kind = 2;
    std::uint8_t flag = 1;
    std::string payload;
    bool operator==(const ClientQoSItem&) const = default;
};
// ISR Timestamp (0x23): one i32, only kept when ISR is on [code: 0x105502cbc].
struct IsrTimestampItem {
    std::int32_t timestamp = 0;
    bool operator==(const IsrTimestampItem&) const = default;
};
// Stream Pin Data (0x25): Stream Data plus the pinned instance [code: 0x1056e7276].
struct StreamPinDataItem {
    StreamDataItem data;
    InstanceId instance;
    std::int32_t value = 0;
    std::uint8_t flag = 0;
    bool operator==(const StreamPinDataItem&) const = default;
};
// Instance Intent (0x29): refs with an intent byte each [code: 0x105725dd0].
struct InstanceIntentItem {
    std::vector<std::pair<InstanceId, std::uint8_t>> intents;
    bool operator==(const InstanceIntentItem&) const = default;
};
// Region Terrain LOD Removal (0x2b): region + u8 [code: 0x1056d55d6].
struct RegionTerrainLodRemovalItem {
    RegionId region;
    std::uint8_t lod = 0;
    bool operator==(const RegionTerrainLodRemovalItem&) const = default;
};

using RuntimeBody
    = std::variant<PingItem, PingBackItem, RequestCharacterItem, PmcConfigItem, CFrameAckItem, StreamDataItem,
                   RegionRemovalItem, InstanceRemovalItem, StatsItem, StreamingInfoItem, StreamPrefetchInfoItem,
                   StreamPrefetchRequestItem, StreamFocusItem, StreamFocusRemoveItem, StreamFocusFrustumItem,
                   StreamFocusFrustumRemoveItem, DestroyItem, ClientCapacityUpdateItem, PersistentPlayerItem,
                   ClientQoSItem, IsrTimestampItem, StreamPinDataItem, InstanceIntentItem, RegionTerrainLodRemovalItem>;
// One of the above inside an ID_DATA message (the Item variant in Replication.hpp holds this).
struct RuntimeItem {
    RuntimeBody body;
    bool operator==(const RuntimeItem&) const = default;
};
std::uint8_t runtimeItemType(const RuntimeItem& item);

// What the codecs need beyond the bytes: who wrote the item and which
// capabilities were negotiated (optional fields).
struct ItemContext {
    bool fromServer = true; // a server wrote it (pings differ by the sender's role)
    std::uint64_t capabilities = kBaseCapabilities;
};

// Writes the body (after the type byte). False for a body this can't write
// (a Stats item, an unknown region list).
bool writeRuntimeItem(NetworkStream& out, const RuntimeItem& item, const ValueContext& values,
                      const ItemContext& context);
// Reads the body of item `type`; nullopt for a type this doesn't know (nothing
// consumed) or a truncated body (`error` says which). `tree` gets the fields.
std::optional<RuntimeItem> readRuntimeItem(NetworkStream& in, std::uint8_t type, const ValueContext& values,
                                           const ItemContext& context, Node* tree, std::string& error);
bool isRuntimeItemType(std::uint8_t type);

// The readable form of an item body (what readRuntimeItem adds to its tree).
void describeRuntimeItem(const RuntimeItem& item, Node& into);

// Builders for the two pings, with the clock the Peer uses (ms).
PingItem makePing(std::uint64_t nowMs, bool fromServer, std::uint8_t kind = 0);
PingBackItem answerPing(const PingItem& ping, std::uint32_t recvMs, std::uint32_t nowMs);
// Round-trip time a PingBack reports to the peer that sent the ping.
std::uint32_t pingRoundTripMs(const PingBackItem& back, std::uint32_t nowMs);

// zstd bytes of a block (0x105612fe4); the framed form (u32 compressed length,
// u32 raw length, bytes) is JoinData.hpp's writeCompressedBlock/readCompressedBlock.
std::vector<std::uint8_t> compressBlock(std::span<const std::uint8_t> raw);

} // namespace lunar::net
