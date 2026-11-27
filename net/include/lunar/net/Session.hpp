#pragma once

#include <lunar/classes/World.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Streaming.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar::net {

class Replica;

// The Roblox-layer packets of a running session besides ID_DATA and physics:
// kicks, the legacy chat, terrain clusters, the challenges a server sends,
// and the client's side of all of it (ClientSession). RobloxPlayer 0.742;
// see notes-stream.md for the code each layout comes from.

namespace msg {
inline constexpr std::uint8_t Touches = 0x86;
inline constexpr std::uint8_t ChatAll = 0x87;
inline constexpr std::uint8_t ChatTeam = 0x88;
inline constexpr std::uint8_t ChatGame = 0x8b;
inline constexpr std::uint8_t ChatPlayer = 0x8c;
inline constexpr std::uint8_t Cluster = 0x8d;
inline constexpr std::uint8_t HashMismatch = 0x94;
inline constexpr std::uint8_t RequestStats = 0x96;
inline constexpr std::uint8_t Kick = 0x98;
inline constexpr std::uint8_t Microprofiler = 0x9a;
inline constexpr std::uint8_t BytecodeChallenge = 0x9b;
inline constexpr std::uint8_t ScriptBlob = 0x9c;
inline constexpr std::uint8_t Isr = 0xa1;
inline constexpr std::uint8_t Rna2 = 0xa2;
inline constexpr std::uint8_t Rna3 = 0xa3;
inline constexpr std::uint8_t Rna4 = 0xa4;
inline constexpr std::uint8_t Queued5 = 0xa5;
inline constexpr std::uint8_t Queued6 = 0xa6;
inline constexpr std::uint8_t SharedStringHashes = 0xa7;
} // namespace msg

// ID_KICK (0x98): string32 message, then a string32 metadata JSON when
// capability 50 is negotiated [code: ClientReplicator 0x1054c8810 case 0x98].
struct KickMessage {
    std::string message;
    std::optional<std::string> metadataJson;
    bool operator==(const KickMessage&) const = default;
};
std::vector<std::uint8_t> encodeKick(const KickMessage& kick, std::uint64_t capabilities = kBaseCapabilities);
std::optional<KickMessage> decodeKick(std::span<const std::uint8_t> message, std::uint64_t capabilities);

// The legacy chat packets 0x87 (all), 0x88 (team), 0x8c (whisper: a target
// ref follows the sender) and 0x8b (game: no sender) [code: receiver
// 0x1054a8477, senders ChatSenderImpl 0x1054ad3c8 / 0x1054ad6f4]:
//   u8 id; [ref sender]; [ref target]; string32 text; [u8 flagA]; [u8 flagB]
// The two flag bytes are optional on the wire (read only if bytes remain);
// the client writes both (GUESS on their meaning: filtered / whisper state).
struct ChatMessage {
    std::uint8_t id = msg::ChatAll;
    InstanceId sender; // 0x87/0x88/0x8c: must be a Player under Players, else "Chat message from unknown Player"
    InstanceId target; // 0x8c
    std::string text;
    std::optional<std::uint8_t> flagA, flagB;
    bool operator==(const ChatMessage&) const = default;
};
std::vector<std::uint8_t> encodeChat(const ChatMessage& chat);
std::optional<ChatMessage> decodeChat(std::span<const std::uint8_t> message);

// ID_CLUSTER (0x8d): terrain chunks for a Terrain instance [code: reader
// 0x1055bf604, chunk records 0x1055befdf].
//   ref terrain; [u8 clusterId (0..5); [i32 when clusterId == 5]]  (behind a replicator flag, GUESS: present)
//   compressed block; decompressed: repeat { u8 header; position delta; voxels }
//   header: low 4 bits chunkSizeLog2, bits 4-5 delta width (i8 / i16 / i32 / none), bits 6-7 kept
//   voxels: u32 length + bytes when terrain streaming is on (GUESS: the usual form), else
//           u8 empty; [empty == 0: an unbounded voxel stream, which ends this decode]
// ID_CLUSTER (0x8D): terrain, a zstd block of chunks [live: 0.743, the game's
// lobby to The Mimic, 876 chunks of a Mimic chapter all walked]. Each: a
// header byte, the position's delta from the last chunk's (three int8, int16
// or int32 by header bits 4-5), a byte that's 1 for a chunk all air and 0
// for one whose runs follow (SmoothGrid's run encoding, for all
// 2^(header & 15) cubed cells, 16 a side as seen, X fastest, then Z, then
// Y), and a 0 byte (GUESS: never seen otherwise; what else it may say is unknown).
struct ClusterChunk {
    std::uint8_t header = 0; // low 4 bits chunkSizeLog2, bits 6-7 as sent; bits 4-5 (delta width) are the codec's
    RegionId position;       // in chunks of its size, absolute after the delta
    bool empty = false;      // all air: no runs
    std::vector<std::uint8_t> voxels; // the runs as sent
    bool operator==(const ClusterChunk&) const = default;
};
struct ClusterMessage {
    InstanceId terrain;
    std::optional<std::uint8_t> clusterId;
    std::int32_t clusterValue = 0;
    std::vector<ClusterChunk> chunks;
    std::vector<std::uint8_t> undecoded; // the decompressed bytes this couldn't walk
    bool operator==(const ClusterMessage&) const = default;
};
std::vector<std::uint8_t> encodeCluster(const ClusterMessage& cluster);
std::optional<ClusterMessage> decodeCluster(std::span<const std::uint8_t> message);
// A chunk as the cells it writes into the Terrain; nullopt when its runs
// don't cover it or name a material terrain doesn't have.
std::optional<Terrain::Block> clusterBlock(const ClusterChunk& chunk);

// ID_TOUCHES (0x86): touch events the server relays [code: 0x105582abe ->
// 0x10558283b]: repeat { ref partA (null ends the list); ref partB; u8 state }
// with the state's low 7 bits < 6 (GUESS: 0 touched, 1 untouched ...) and
// bit 7 a flag.
struct TouchEvent {
    InstanceId partA, partB;
    std::uint8_t state = 0;
    bool operator==(const TouchEvent&) const = default;
};
struct TouchesMessage {
    std::vector<TouchEvent> touches;
    bool operator==(const TouchesMessage&) const = default;
};
std::vector<std::uint8_t> encodeTouches(const TouchesMessage& touches);
std::optional<TouchesMessage> decodeTouches(std::span<const std::uint8_t> message);

// 0xa7 (client -> server): shared string hashes, u16 count then 16-byte
// hashes; a second server reader (0x10569a0de) takes u16 count x { u8 kind;
// kind 0: 16-byte hash; kind 1: not decoded } [code: server 0x105699fc2;
// GUESS: the client announces the SharedString contents it already has].
struct SharedStringHashesMessage {
    std::vector<std::array<std::uint8_t, 16>> hashes;
    bool operator==(const SharedStringHashesMessage&) const = default;
};
std::vector<std::uint8_t> encodeSharedStringHashes(const SharedStringHashesMessage& m);
std::optional<SharedStringHashesMessage> decodeSharedStringHashes(std::span<const std::uint8_t> message);

// 0x9a: the server's microprofiler dump: i32, i32 expected length, compressed block holding a string.
struct MicroprofilerMessage {
    std::int32_t kind = 0;
    std::int32_t length = 0;
    std::string html;
    bool operator==(const MicroprofilerMessage&) const = default;
};
std::optional<MicroprofilerMessage> decodeMicroprofiler(std::span<const std::uint8_t> message);

// 0x9b from the server: a Luau bytecode challenge ("Bytecode Auth Check"):
// u32 a, u32 b, string32 bytecode; the client runs it and answers with
// 0x9b: u32 b, u32 result [code: 0x1054c9fe4 / 0x1054c829a].
struct BytecodeChallenge {
    std::uint32_t a = 0, b = 0;
    std::string bytecode;
    bool operator==(const BytecodeChallenge&) const = default;
};
struct BytecodeResponse {
    std::uint32_t b = 0, result = 0;
    bool operator==(const BytecodeResponse&) const = default;
};
std::vector<std::uint8_t> encodeBytecodeChallenge(const BytecodeChallenge& c);
std::optional<BytecodeChallenge> decodeBytecodeChallenge(std::span<const std::uint8_t> message);
std::vector<std::uint8_t> encodeBytecodeResponse(const BytecodeResponse& r);
std::optional<BytecodeResponse> decodeBytecodeResponse(std::span<const std::uint8_t> message);
// The challenge's bytecode as Roblox ships script bytecode
// (RBX::ProtectedString, 0x103b1dfa8): every byte i XORed with
// (k[i % 4] + 41 i), where k is the u32 LE xxHash32 (seed 42) of the plain
// blob and the plain blob starts "RSB1"; then u32 LE size and a zstd frame.
// The result is Luau bytecode whose opcode bytes are still multiplied by 227
// (ScriptContext::callBytecode undoes that) [live].
std::expected<std::string, std::string> decodeProtectedBytecode(std::string_view blob);

// 0x9c: u8 kind (0..2), u32 id, and for kind 0 an i16-counted blob the
// client hands to its script runtime (0x1054c1060) [code: 0x10557e1d6].
struct ScriptBlobMessage {
    std::uint8_t kind = 0;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> blob;
    bool operator==(const ScriptBlobMessage&) const = default;
};
std::optional<ScriptBlobMessage> decodeScriptBlob(std::span<const std::uint8_t> message);

// Describes any of the packets above (and the ones only hex-dumped: 0x86,
// 0x8e, 0x94, 0x95, 0x96, 0xa1-0xa7) into `into`. False for other ids.
bool describeSessionMessage(std::span<const std::uint8_t> message, Node& into, std::uint64_t capabilities,
                            const Replica* replica = nullptr);
// Applies one (records a kick, a challenge ...) and describes it. False for other ids.
bool handleSessionMessage(Replica& replica, std::span<const std::uint8_t> message, Node* tree);
// Applies one decoded runtime item to the replica (destroys, pings to answer ...).
void applyRuntimeItem(Replica& replica, const RuntimeItem& item);

// The instances one Stream Data (or Stream Pin Data) item brought, with the
// regions it named, in arrival order: what the client's streaming bookkeeping
// (StreamingClient) works from.
struct StreamedBatch {
    std::vector<StreamRegionRef> regions; // as on the wire: delta codes refer to the previous region
    std::vector<InstanceId> instances;    // the item's instances, in its order
    bool pinned = false;                  // a Stream Pin Data item
};

// What a Replica collects for the session layer.
struct RuntimeState {
    std::vector<PingItem> pendingPings;  // the peer's pings, to answer with PingBacks
    std::vector<PingBackItem> pingBacks; // answers to our pings (the session turns them into RTTs)
    std::vector<KickMessage> kicks;
    std::vector<BytecodeChallenge> challenges;
    std::vector<ChatMessage> chats;
    // The latest terrain clusters and touches (at most twice recordLimit;
    // the terrain is written as they come).
    static constexpr std::size_t recordLimit = 256;
    std::vector<ClusterMessage> clusters;
    std::vector<TouchesMessage> touches;
    std::vector<RequestCharacterItem> characterRequests; // server side
    std::vector<StreamFocusItem> focuses;
    std::vector<StreamingInfoItem> streamingInfos;
    std::vector<ClientCapacityUpdateItem> capacityUpdates;
    // Streaming, for the client's bookkeeping to take (StreamingClient::tick empties them).
    std::vector<StreamedBatch> streamedBatches;
    std::vector<StreamPrefetchInfoItem> prefetchInfos;       // answers to our Stream Prefetch Requests
    std::vector<StreamPrefetchRequestItem> prefetchRequests; // the server's (from a client: the requests, server side)
    std::vector<std::uint32_t> focusRemovals;                // Stream Replication Focus Remove ids
    std::vector<RegionRemovalItem> regionRemovals;           // server side: what a client dropped
    std::size_t destroyed = 0;
    std::size_t streamDataItems = 0;
    std::size_t placeIdVerifications = 0; // 0x92 with ok
    // The server's physics tick (ISR, 240 a second) as last heard: an ISR
    // packet's or an ISR Timestamp item's. A client's ISR carries its clock
    // offset from it.
    std::optional<std::uint32_t> serverIsrTick;
};

// The client's side of a session, as a stream of messages a Peer should
// send: a Ping every 500 ms (the schedule 0x1055fd006 runs; GUESS that the
// client runs it as the server does), a PingBack for each server ping
// (0x1056175d6), the marker echo, the Request Character item once the place
// id is verified (0x1054c8810 case 0x92), the bytecode challenge answer when
// a runner is given, and the initial streaming quota when the server streams.
struct ClientSessionOptions {
    std::uint64_t capabilities = kBaseCapabilities;
    bool sendPings = true;
    std::uint32_t pingIntervalMs = 500;
    std::int32_t specialPingEvery = -1; // FInt 0x1078b6ef8: every n pings a second ping with kind 1; -1 never (GUESS)
    bool echoMarkers = true;
    bool requestCharacter = true;
    std::string appearance;            // the Request Character string
    std::uint32_t playerFlags = 0;     // its u32
    bool sendCapacityUpdate = true;    // once, after the first Stream Data item
    std::int32_t initialQuota = 2000;  // DFInt ClientInstanceQuotaInitial (0x1078b2500) [code: 0x1054e6571]
    std::function<std::optional<std::uint32_t>(const BytecodeChallenge&)> challenge; // null: challenges are logged only
};

class ClientSession {
public:
    explicit ClientSession(ClientSessionOptions options = {});

    // Every message from the server (before or after the Replica handles it).
    void onMessage(std::span<const std::uint8_t> message);
    // What to send now, in order; `replica` is the client's (its schema frames the items).
    std::vector<std::vector<std::uint8_t>> tick(Replica& replica, std::uint64_t nowMs);

    // A legacy chat line from this client's Player (0x87 all / 0x88 team / 0x8c whisper / 0x8b game).
    static std::vector<std::uint8_t> chat(InstanceId player, std::string_view text, std::uint8_t id = msg::ChatAll,
                                          InstanceId target = {});

    std::size_t pingsSent = 0, pingBacksSent = 0, pingBacksReceived = 0, markersEchoed = 0, challengesAnswered = 0;
    bool characterRequested = false;
    bool capacityUpdateSent = false;
    std::vector<std::uint32_t> roundTripsMs; // one per PingBack received
    std::optional<KickMessage> kicked;
    const ClientSessionOptions& options() const { return options_; }

private:
    ClientSessionOptions options_;
    std::uint64_t lastPingMs_ = 0;
    std::int32_t pingsSinceSpecial_ = 0;
    bool placeIdVerified_ = false;
    std::size_t markersSeen_ = 0;
    std::vector<BytecodeChallenge> pendingChallenges_;
};

// The server's side for one client: pings it every 500 ms (0x1055fd006 in
// the sender schedule job) and answers its pings.
class ServerSession {
public:
    explicit ServerSession(std::uint64_t capabilities = kBaseCapabilities, bool sendPings = true)
        : capabilities_(capabilities), sendPings_(sendPings)
    {
    }
    // Items the client sent (decoded by the caller): pings get a PingBack, PingBacks an RTT.
    void onItem(const RuntimeItem& item, std::uint64_t nowMs);
    std::vector<RuntimeItem> take(std::uint64_t nowMs); // items to send now (as one ID_DATA)
    std::vector<std::uint32_t> roundTripsMs;
    std::size_t pingsSent = 0, pingBacksSent = 0;
    std::vector<RequestCharacterItem> characterRequests;

private:
    std::uint64_t capabilities_;
    bool sendPings_;
    std::uint64_t lastPingMs_ = 0;
    std::vector<RuntimeItem> due_;
};

} // namespace lunar::net
