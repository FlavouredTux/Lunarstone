#pragma once

#include <lunar/Value.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Values.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

struct Schema;
struct NewInstanceItem;

// The join-time messages and ID_DATA items a Roblox server sends after the
// schema (ROBLOX-LAYER.md 6, notes-joindata.md): ID_SET_GLOBALS with the top
// replication containers, the Join Data blobs (items 0x0b and 0x16), the
// shared string dictionary, and the small items that travel with a join
// (Tag, Server Time, Change Attribute, Resolve Atomic Tree, Instance Intent,
// Change Inst, ISR Timestamp). Layouts from RobloxPlayer 0.742; the
// confidence of each is in the notes.

// -- shared strings (ProtectedString 0x06/0x2f, BinaryString 0x2e)
//
// Contents travel once per connection: a value is the 16-byte key of its
// content, and a receiver that doesn't know the key gets the content right
// after the item ("deferred shared strings", sub_105605204) or in a Join
// Data V2 shared-string blob (mode 1). The key is BLAKE2b with a 16-byte
// digest of the content: SharedString(const std::string&) (0x102f73180)
// hashes through sub_102f4879c, whose state is a RBX::Crypto::HashStateBlake2b
// made for 16 bytes (sub_102f49ce0) [code]. (The log strings still say "md5".)
SharedStringKey sharedStringKey(std::string_view content);
std::string keyText(const SharedStringKey& key);

class SharedStringCache {
public:
    const std::string* find(const SharedStringKey& key) const;
    bool contains(const SharedStringKey& key) const { return strings_.contains(key); }
    // Adds (or re-adds) the content under its own key; returns the key.
    SharedStringKey add(std::string content);
    std::size_t size() const { return strings_.size(); }

private:
    std::map<SharedStringKey, std::string> strings_;
};

// One dictionary entry (reader sub_105605398, writer sub_105605174):
//   16 bytes key; u8 marker; marker 0: string content (varint length) follows,
//   marker 1: "echo" of a content the sender knows the receiver has (no content).
struct SharedStringEntry {
    SharedStringKey key{};
    std::uint8_t marker = 0;
    std::string content;
};
void writeSharedStringEntry(NetworkStream& out, const SharedStringEntry& entry);
std::optional<SharedStringEntry> readSharedStringEntry(NetworkStream& in);

// -- ID_SET_GLOBALS (0x81), server -> client right after the schema [code: writer sub_10567f27a, reader
// ClientReplicator::vfn_152 case 0x81]
struct TopContainer {
    InstanceId id;               // the instance id the service gets on this connection
    std::uint16_t classId = 0;   // schema class network id ("...included invalid class network id")
    bool flagA = true;           // GUESS: replicated eagerly (its subtree is in the join data)
    bool flagB = false;          // GUESS: streamed / persistent
};
struct SetGlobals {
    bool streamingEnabled = false;      // GUESS: server rep+0x38c8 (a streaming object) != null
    bool notCloudEdit = true;           // !vfn_0x408 on the server (GUESS: not a Team Create session)
    std::array<bool, 4> settings{};     // four bools from a settings object (+0xa9..+0xac); meaning GUESS
    bool flag8 = false;                 // rep->0x3890->0x12d; meaning GUESS
    std::uint32_t peerId = 0;           // "Assigned peer id = %u": the scope of ids the client creates
    std::optional<std::uint32_t> workspaceValue; // only with capability bit 40: Workspace->+0x3c0->+0xf0 (GUESS: a tick)
    std::vector<TopContainer> containers; // the services the join replicates, in order
};
std::vector<std::uint8_t> encodeSetGlobals(const SetGlobals& g, std::uint64_t negotiatedCapabilities);
std::expected<SetGlobals, std::string> decodeSetGlobals(std::span<const std::uint8_t> message,
                                                        std::uint64_t negotiatedCapabilities, Node* tree = nullptr,
                                                        const Schema* schema = nullptr);

// -- the extra items (ROBLOX-LAYER.md 3, notes-joindata.md 3)
namespace item {
inline constexpr std::uint8_t JoinData = 0x0b;
inline constexpr std::uint8_t Tag = 0x10;
inline constexpr std::uint8_t ResolveAtomicTree = 0x13;
inline constexpr std::uint8_t ChangeAttribute = 0x15;
inline constexpr std::uint8_t JoinDataV2 = 0x16;
inline constexpr std::uint8_t ChangeInst = 0x20;
inline constexpr std::uint8_t ServerTime = 0x26;
} // namespace item

// Join Data Items V2 (0x16), reader sub_1055ca34a + sub_1055cac8e/sub_1055caa16/sub_1055caab8:
//   u8  mode               1 = shared strings, 2..4 = instances, 5 = NetAssets
//   u32 compressedLen, u32 rawLen, zstd bytes          (the blob, same codec as the schema)
//   mode 2..4 only: u32 instanceCount; varint invalidCount; invalidCount x (varint offset, varint length)
//                   (byte ranges of the blob to skip: "Invalid offset %d length %d")
// Blob, mode 2..4: instanceCount x { ref id; u16 class; properties (one pass, delta bytes until 0xff); ref parent }
// Blob, mode 1:    shared string entries until the end.
// Blob, mode 5:    { NetAssetHandle; varint length; bytes } until the end.
struct NetAssetBatch {
    std::uint64_t handleType = 0xffff; // varint; 0xffff = none
    std::uint64_t guid = 0;            // u64 (GUESS: peer-scoped asset id)
    std::uint64_t index = 0;           // varint
    std::vector<std::uint8_t> bytes;
};
struct JoinDataV2Item {
    std::uint8_t mode = 2;
    std::vector<NewInstanceItem> instances;        // modes 2..4 (properties in `properties`, one pass)
    std::vector<SharedStringEntry> sharedStrings;  // mode 1
    std::vector<NetAssetBatch> netAssets;          // mode 5
    std::vector<std::pair<std::uint32_t, std::uint32_t>> invalidRanges; // offset, length; sent empty
};
// Join Data (0x0b), the older form: u32 count; compressed blob of count x instance (as above); then the
// deferred shared strings the instances referenced.
struct JoinDataItem {
    std::vector<NewInstanceItem> instances;
};
// `count` instances from a decompressed join-data blob body (the layout
// Join Data and Stream Data share); stops at the first it can't read and
// says why in `error`.
std::vector<NewInstanceItem> readJoinInstances(std::span<const std::uint8_t> blob, std::uint32_t count,
                                               const Schema& schema, const ValueContext& context, std::string& error);
// The other way: the blob body for these instances, their shared strings
// deferred to its end. Nothing for a class id past the schema.
std::optional<std::vector<std::uint8_t>> writeJoinInstances(std::span<const NewInstanceItem> instances,
                                                            const Schema& schema, const ValueContext& context);
struct TagItem {
    std::uint32_t tag = 0; // 13 ends the join on the client ("join data done", sub_1055e6c14) [code]
};
inline constexpr std::uint32_t kTagJoinDataDone = 13;
struct ServerTimeItem {
    std::int64_t time = 0; // zigzag64; the server's clock (GUESS: microseconds)
};
struct ResolveAtomicTreeItem {
    InstanceId instance; // an atomic tree's root, sent without a parent ...
    InstanceId other;    // ... and where it goes [live: a ScreenGui into our PlayerGui]
};
struct ChangeAttributeItem {
    InstanceId id;
    std::string name;
    Value value; // the attribute value blob (u8 type + value, the file format's codes); nil removes it
};
struct ChangeInstItem {
    std::uint32_t kind = 0; // GUESS
    std::vector<std::uint8_t> bytes;
};

// The attribute value blob of Change Attribute: u8 type code (the AttributesSerialize
// codes the place formats use) then the value. False / nullopt for types it can't carry.
bool encodeAttributeValue(NetworkStream& out, const Value& value);
std::optional<Value> decodeAttributeValue(std::span<const std::uint8_t> bytes, std::string* text = nullptr);

// Compressed blocks (sub_1056142ae "decompressBitStream"): u32 compressed length, u32 raw length, zstd.
void writeCompressedBlock(NetworkStream& out, std::span<const std::uint8_t> raw);
std::expected<std::vector<std::uint8_t>, std::string> readCompressedBlock(NetworkStream& in);

// The value-level codec of the join-time network types (writeValue/readValue hand them over).
// `text` (if given) gets a readable rendering, as readValue's.
bool writeJoinValue(NetworkStream& out, std::uint8_t type, const Value& value, const ValueContext& context);
std::optional<Value> readJoinValue(NetworkStream& in, std::uint8_t type, const ValueContext& context,
                                   std::string* text = nullptr);

} // namespace lunar::net
