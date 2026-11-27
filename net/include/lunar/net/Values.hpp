#pragma once

#include <lunar/RemoteValue.hpp>
#include <lunar/Value.hpp>
#include <lunar/net/NetworkStream.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// Property and argument values as the Roblox layer writes them, switched on
// the schema's network type id (writer 0x1055f132c, reader 0x1055ed9e0;
// ROBLOX-LAYER.md section 4).
namespace nettype {
inline constexpr std::uint8_t Nil = 0x01;
inline constexpr std::uint8_t String = 0x02;
inline constexpr std::uint8_t Void = 0x03;
inline constexpr std::uint8_t ProtectedString = 0x06;
inline constexpr std::uint8_t Enum = 0x07;
inline constexpr std::uint8_t SharedString = 0x08;
inline constexpr std::uint8_t Bool = 0x09;
inline constexpr std::uint8_t Int = 0x0a;
inline constexpr std::uint8_t Float = 0x0b;
inline constexpr std::uint8_t Double = 0x0c;
inline constexpr std::uint8_t UDim = 0x0d;
inline constexpr std::uint8_t UDim2 = 0x0e;
inline constexpr std::uint8_t Ray = 0x0f;
inline constexpr std::uint8_t Faces = 0x10;
inline constexpr std::uint8_t Axes = 0x11;
inline constexpr std::uint8_t BrickColor = 0x12;
inline constexpr std::uint8_t Color3 = 0x13;
inline constexpr std::uint8_t Color3uint8 = 0x14;
inline constexpr std::uint8_t Vector2 = 0x15;
inline constexpr std::uint8_t Vector3 = 0x16;
inline constexpr std::uint8_t Vector2int16 = 0x18;
inline constexpr std::uint8_t Vector3int16 = 0x19;
inline constexpr std::uint8_t CFrame = 0x1a;
inline constexpr std::uint8_t CFrameCompressed = 0x1b;
inline constexpr std::uint8_t Object = 0x1c;
inline constexpr std::uint8_t Tuple = 0x1d;
inline constexpr std::uint8_t Array = 0x1e;
inline constexpr std::uint8_t Dictionary = 0x1f;
inline constexpr std::uint8_t Map = 0x20;
inline constexpr std::uint8_t NumberSequence = 0x23;
inline constexpr std::uint8_t NumberSequenceKeypoint = 0x24;
inline constexpr std::uint8_t NumberRange = 0x25;
inline constexpr std::uint8_t ColorSequence = 0x26;
inline constexpr std::uint8_t ColorSequenceKeypoint = 0x27;
inline constexpr std::uint8_t Rect2D = 0x28;
inline constexpr std::uint8_t PhysicalProperties = 0x29;
inline constexpr std::uint8_t Region3int16 = 0x2b;
inline constexpr std::uint8_t Int64 = 0x2c;
inline constexpr std::uint8_t DateTime = 0x30;
inline constexpr std::uint8_t OptionalCFrame = 0x32;
inline constexpr std::uint8_t OptionalCFrameCompressed = 0x33; // as 0x32 with a 0x1b quaternion (sub_1057af3a2)
inline constexpr std::uint8_t TypedNumber = 0x40;       // u8 kind, u32, f64 (writer sub_1057ae2bc)
inline constexpr std::uint8_t AnimTrackPlayState = 0x41; // the same layout as 0x40 (shares its writer)
inline constexpr std::uint8_t AnimTrackMetadata = 0x42; // u8, u16 (sub_1057ae230)
inline constexpr std::uint8_t AnimTrackWeight = 0x43;   // f32, f32, f64 (writer sub_1057ae372)
inline constexpr std::uint8_t User = 0x44;                     // zigzag64, varint, zigzag64, zigzag64 (sub_1057ac28e)
inline constexpr std::uint8_t Font = 0x36;
inline constexpr std::uint8_t SecurityCapabilities = 0x37;
inline constexpr std::uint8_t Random = 0x3e;
inline constexpr std::uint8_t ReplicationPV = 0x39;
// Join-time types (notes-joindata.md 4): strings through the schema's
// dictionaries, content-keyed strings, NetAssets and Content.
inline constexpr std::uint8_t ContentId = 0x21;               // string, or prefix index + rest when prefixedContent
inline constexpr std::uint8_t PeerAddress = 0x22;             // SystemAddress: varint peer id
inline constexpr std::uint8_t BinaryString = 0x2e;            // 16-byte content key (shared string)
inline constexpr std::uint8_t ProtectedStringBytecode = 0x2f; // as ProtectedString
inline constexpr std::uint8_t DictionaryString = 0x31;        // u8 tag: short inline, long inline or dictionary id
inline constexpr std::uint8_t Buffer = 0x38;                  // varint compressed length (0 = raw), varint length, bytes
inline constexpr std::uint8_t NetAssetHandle = 0x3b;
inline constexpr std::uint8_t NetAssetRef = 0x3c;
inline constexpr std::uint8_t Content = 0x3d; // varint kind: 0 none, 1 uri (ContentId), 2 object, 3 NetAsset
} // namespace nettype

// The 16-byte key of a shared string's content (BLAKE2b-128 of the content, JoinData.hpp).
using SharedStringKey = std::array<std::uint8_t, 16>;
class SharedStringCache;

const char* netTypeName(std::uint8_t type);
// What Luau's typeof answers for a value the type carries ("number" for an
// Int, "Instance" for an Object); null for one that carries any (a Tuple, a
// Variant) or none a script can make.
const char* luauTypeName(std::uint8_t type);

// The scope:index pair an instance travels as (3.2).
struct InstanceId {
    std::uint32_t scope = 0;
    std::uint32_t index = 0;
    bool operator==(const InstanceId&) const = default;
    bool null() const { return scope == 0 && index == 0; }
};
void writeInstanceId(NetworkStream& out, InstanceId id);
InstanceId readInstanceId(NetworkStream& in);

// What values need from the replicator around them.
struct ValueContext {
    std::function<InstanceId(const InstancePtr&)> idOf; // writing refs
    std::function<InstancePtr(InstanceId)> resolve;     // reading refs
    const EnumInfo* enumType = nullptr;                 // for Enum properties
    std::function<void(InstanceId)> sawRef;             // decoder: refs that didn't resolve
    // Join-time types (JoinData.hpp). Shared-string values (0x06/0x2e/0x2f)
    // travel as the key of their content; the cache is what this connection
    // already exchanged. A key the cache lacks is reported through
    // deferSharedString (reader: content empty; writer: the content to send
    // right after the item), and the reader gets the key's hex as the value
    // until the content arrives.
    SharedStringCache* sharedStrings = nullptr;
    std::function<void(const SharedStringKey&, std::string_view content)> deferSharedString;
    const std::vector<std::string>* contentPrefixes = nullptr;  // schema list 1: ContentId prefixes ([0] = "")
    const std::vector<std::string>* stringDictionary = nullptr; // schema list 2: DictionaryString ids
    bool prefixedContent = false; // ContentId as prefix index + rest (New item pass 1 and Change Property), else plain
    // Enums inside variants carry a u16 enum id, the schema's enum index
    // [live: a server read Enum.SavedQualitySetting.Automatic sent as 438];
    // these map it both ways. Without them an EnumItem goes out as enum 0 and
    // comes back as its number.
    std::function<const EnumInfo*(std::uint16_t)> enumById;
    std::function<std::optional<std::uint16_t>(const EnumInfo&)> enumIdOf;
};

// Writes `value` as `type`; false when the value doesn't fit the type (nothing is written then).
bool writeValue(NetworkStream& out, std::uint8_t type, const Value& value, const ValueContext& context = {});
// Reads a `type` value. Types without a lunar::Value of their own come back
// as the closest one (Ray as its origin, int64 and DateTime as numbers,
// Color3uint8 as Color3 ...); `text` (if given) gets a readable rendering.
std::optional<Value> readValue(NetworkStream& in, std::uint8_t type, const ValueContext& context = {},
                               std::string* text = nullptr);

// Variants (Tuple/Array members, 4.3): u8 type, [u16 enum id], value.
bool writeVariant(NetworkStream& out, const Value& value, const ValueContext& context = {});
std::optional<Value> readVariant(NetworkStream& in, const ValueContext& context = {}, std::string* text = nullptr);
// The network type a Luau value goes out as inside a variant.
std::uint8_t variantType(const Value& value);
// Variants that may be tables, what remote events carry: a table goes out as
// an Array (0x1e: varint count, variants) or a Dictionary (0x1f: varint
// count, {string key, variant}), and Map (0x20) reads like a Dictionary.
bool writeVariant(NetworkStream& out, const RemoteValue& value, const ValueContext& context = {});
std::optional<RemoteValue> readRemoteVariant(NetworkStream& in, const ValueContext& context = {},
                                             std::string* text = nullptr);
// The same tables where the type is given, not read (an event parameter
// typed Array, Dictionary or Map: TextChatService's
// ServerToClientMessageReplicateSignalV2(data)): the count and members only.
bool writeRemoteTable(NetworkStream& out, std::uint8_t type, const RemoteValue& value, const ValueContext& context = {});
std::optional<RemoteValue> readRemoteTable(NetworkStream& in, std::uint8_t type, const ValueContext& context = {},
                                           std::string* text = nullptr);
inline bool isTableType(std::uint8_t type)
{
    return type == nettype::Array || type == nettype::Dictionary || type == nettype::Map;
}

// CFrame rotations: the .rbxl axis-aligned rotation ids (4.1) and the
// smallest-three quaternion of type 0x1b (4.2, sub_1054e75c6).
std::uint8_t rotationId(const CFrame& cf); // 0 when not axis-aligned
std::optional<CFrame> rotationFromId(std::uint8_t id);
void writeQuaternion(NetworkStream& out, const CFrame& cf);
CFrame readQuaternion(NetworkStream& in);

} // namespace lunar::net
