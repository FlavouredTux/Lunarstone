#include <lunar/net/JoinData.hpp>

#include <lunar/Enums.hpp>
#include <lunar/Instance.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Replication.hpp>

#include <sodium.h>
#include <zstd.h>

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace lunar::net {

// -- the shared string key: BLAKE2b-128 of the content (libsodium's generic hash is BLAKE2b)

SharedStringKey sharedStringKey(std::string_view content)
{
    SharedStringKey out{};
    crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(content.data()),
                       content.size(), nullptr, 0);
    return out;
}

std::string keyText(const SharedStringKey& key)
{
    std::string s;
    for (std::uint8_t b : key)
        s += std::format("{:02x}", b);
    return s;
}

const std::string* SharedStringCache::find(const SharedStringKey& key) const
{
    auto it = strings_.find(key);
    return it == strings_.end() ? nullptr : &it->second;
}

SharedStringKey SharedStringCache::add(std::string content)
{
    const SharedStringKey key = sharedStringKey(content);
    strings_[key] = std::move(content);
    return key;
}

void writeSharedStringEntry(NetworkStream& out, const SharedStringEntry& entry)
{
    out.writeBytes(entry.key);
    out.writeU8(entry.marker);
    if (entry.marker == 0)
        out.writeString(entry.content);
}

std::optional<SharedStringEntry> readSharedStringEntry(NetworkStream& in)
{
    SharedStringEntry e;
    const auto key = in.readBytes(16);
    if (in.bad())
        return std::nullopt; // "malformed shared string dictionary, md5 ended early"
    std::copy_n(key.begin(), 16, e.key.begin());
    e.marker = in.readU8();
    if (in.bad())
        return std::nullopt; // "... marker ended early"
    if (e.marker == 0)
        e.content = in.readString();
    else if (e.marker != 1)
        return std::nullopt; // "Unknown new shared string format marker %d"
    if (in.bad())
        return std::nullopt;
    return e;
}

// -- compressed blocks

void writeCompressedBlock(NetworkStream& out, std::span<const std::uint8_t> raw)
{
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    const std::size_t n = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 3);
    packed.resize(ZSTD_isError(n) ? 0 : n);
    out.writeU32(static_cast<std::uint32_t>(packed.size()));
    out.writeU32(static_cast<std::uint32_t>(raw.size()));
    out.writeBytes(packed);
}

std::expected<std::vector<std::uint8_t>, std::string> readCompressedBlock(NetworkStream& in)
{
    const std::uint32_t packedSize = in.readU32(), rawSize = in.readU32();
    if (in.bad() || packedSize > in.left() || rawSize > (256u << 20))
        return std::unexpected(std::format("bad compressed block (compressed {}, raw {}, {} bytes left)", packedSize,
                                           rawSize, in.left()));
    const auto packed = in.readBytes(packedSize);
    std::vector<std::uint8_t> body(rawSize);
    if (rawSize == 0)
        return body;
    const std::size_t n = ZSTD_decompress(body.data(), body.size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != rawSize)
        return std::unexpected(std::format("zstd: {}", ZSTD_isError(n) ? ZSTD_getErrorName(n) : "short output"));
    return body;
}

// -- the join-time value types (readValue/writeValue hand these over)

namespace {

std::string shortText(std::string_view s)
{
    std::string t;
    for (char c : s.substr(0, 120))
        t += (c >= 0x20 && c < 0x7f) ? c : '.';
    return s.size() > 120 ? t + "..." : t;
}

void writeContentId(NetworkStream& out, const std::string& s, const ValueContext& context)
{
    if (!context.prefixedContent) {
        out.writeString(s);
        return;
    }
    // Prefix form (reader sub_105605074): u8 tag = index << 1 | numeric; the
    // index is into the schema's prefix list ([0] = no prefix); the rest
    // follows as a string, or when numeric as a zigzag varint printed after
    // the prefix ("%s%lld", sub_1016e9b43) [code]. Written as a string
    // always; the longest matching prefix wins.
    std::size_t best = 0;
    if (context.contentPrefixes)
        for (std::size_t i = 1; i < context.contentPrefixes->size() && i < 128; ++i) {
            const std::string& p = (*context.contentPrefixes)[i];
            if (!p.empty() && s.starts_with(p) && p.size() > (*context.contentPrefixes)[best].size())
                best = i;
        }
    out.writeU8(static_cast<std::uint8_t>(best << 1));
    out.writeString(best ? s.substr((*context.contentPrefixes)[best].size()) : s);
}

std::string readContentId(NetworkStream& in, const ValueContext& context, std::string* text)
{
    if (!context.prefixedContent) {
        std::string s = in.readString();
        if (text)
            *text = std::format("\"{}\"", shortText(s));
        return s;
    }
    const std::uint8_t tag = in.readU8();
    const std::size_t index = tag >> 1;
    std::string prefix;
    if (context.contentPrefixes && index < context.contentPrefixes->size())
        prefix = (*context.contentPrefixes)[index];
    else if (index != 0 && text)
        *text = std::format("(prefix {} not in the schema) ", index);
    std::string s;
    if (tag & 1) {
        // Animator.OnCombinedUpdate names animations this way [capture].
        const std::int64_t n = in.readZigzag64();
        s = prefix + std::to_string(n);
        if (text)
            *text += std::format("\"{}\" (prefix {} + {})", shortText(s), index, n);
    } else {
        s = prefix + in.readString();
        if (text)
            *text += std::format("\"{}\" (prefix {})", shortText(s), index);
    }
    return s;
}

bool writeKeyedString(NetworkStream& out, const std::string& content, const ValueContext& context)
{
    const SharedStringKey key = sharedStringKey(content);
    out.writeBytes(key);
    if (context.sharedStrings && !context.sharedStrings->contains(key) && context.deferSharedString)
        context.deferSharedString(key, content);
    return true;
}

Value readKeyedString(NetworkStream& in, const ValueContext& context, std::string* text)
{
    const auto raw = in.readBytes(16);
    SharedStringKey key{};
    if (raw.size() == 16)
        std::copy_n(raw.begin(), 16, key.begin());
    if (const std::string* content = context.sharedStrings ? context.sharedStrings->find(key) : nullptr) {
        if (text)
            *text = std::format("content key {} ({} bytes, known)", keyText(key), content->size());
        return *content;
    }
    if (context.deferSharedString)
        context.deferSharedString(key, {});
    if (text)
        *text = std::format("content key {} (content follows)", keyText(key));
    return keyText(key);
}

} // namespace

bool writeJoinValue(NetworkStream& out, std::uint8_t type, const Value& value, const ValueContext& context)
{
    switch (type) {
    case nettype::ContentId:
        if (const auto* s = std::get_if<std::string>(&value)) {
            writeContentId(out, *s, context);
            return true;
        }
        return false;
    case nettype::Content:
        // varint kind: 0 none, 1 uri (a ContentId), 2 object (a ref), 3 NetAsset (GUESS).
        if (std::holds_alternative<std::monostate>(value)) {
            out.writeVarint(0);
            return true;
        }
        if (const auto* s = std::get_if<std::string>(&value)) {
            if (s->empty()) {
                out.writeVarint(0);
            } else {
                out.writeVarint(1);
                writeContentId(out, *s, context);
            }
            return true;
        }
        if (const auto* inst = std::get_if<InstancePtr>(&value)) {
            out.writeVarint(2);
            writeInstanceId(out, context.idOf && *inst ? context.idOf(*inst) : InstanceId{});
            return true;
        }
        return false;
    case nettype::PeerAddress:
        if (const double* d = std::get_if<double>(&value)) {
            out.writeVarint(static_cast<std::uint64_t>(*d));
            return true;
        }
        return false;
    case nettype::ProtectedString:
    case nettype::ProtectedStringBytecode:
    case nettype::BinaryString:
        if (const auto* s = std::get_if<std::string>(&value))
            return writeKeyedString(out, *s, context);
        return false;
    case nettype::DictionaryString: {
        // u8 tag: 0x80|id (id < 0x7f) or 0xff + varint (id - 0x7f) for a
        // dictionary string; else an inline string: tag = length (< 0x7f) and
        // the bytes, or 0x7f and a varint-length string (reader sub_1055ed9e0 case 0x31).
        const auto* s = std::get_if<std::string>(&value);
        if (!s)
            return false;
        if (context.stringDictionary) {
            auto it = std::ranges::find(*context.stringDictionary, *s);
            if (it != context.stringDictionary->end()) {
                const auto id = static_cast<std::uint64_t>(it - context.stringDictionary->begin());
                if (id < 0x7f) {
                    out.writeU8(static_cast<std::uint8_t>(0x80 | id));
                } else {
                    out.writeU8(0xff);
                    out.writeVarint(id - 0x7f);
                }
                return true;
            }
        }
        if (s->size() < 0x7f) {
            out.writeU8(static_cast<std::uint8_t>(s->size()));
            out.writeBytes(std::span(reinterpret_cast<const std::uint8_t*>(s->data()), s->size()));
        } else {
            out.writeU8(0x7f);
            out.writeString(*s);
        }
        return true;
    }
    case nettype::Buffer: {
        const std::string* s = std::get_if<std::string>(&value);
        if (const auto* b = std::get_if<Buffer>(&value))
            s = &b->bytes;
        if (s) {
            out.writeVarint(0); // raw
            out.writeVarint(s->size());
            out.writeBytes(std::span(reinterpret_cast<const std::uint8_t*>(s->data()), s->size()));
            return true;
        }
        return false;
    }
    case nettype::NetAssetHandle:
    case nettype::NetAssetRef:
        if (std::holds_alternative<std::monostate>(value)) {
            out.writeVarint(0xffff); // none
            return true;
        }
        return false;
    default:
        return false;
    }
}

std::optional<Value> readJoinValue(NetworkStream& in, std::uint8_t type, const ValueContext& context, std::string* text)
{
    Value v;
    switch (type) {
    case nettype::ContentId:
        v = readContentId(in, context, text);
        break;
    case nettype::Content: {
        const std::uint64_t kind = in.readVarint();
        if (kind == 0) {
            if (text)
                *text = "Content.none";
            v = std::string();
        } else if (kind == 1) {
            std::string inner;
            v = readContentId(in, context, text ? &inner : nullptr);
            if (text)
                *text = "Content.uri " + inner;
        } else if (kind == 2) {
            const InstanceId id = readInstanceId(in);
            InstancePtr inst = !id.null() && context.resolve ? context.resolve(id) : nullptr;
            if (text)
                *text = std::format("Content.object {}:{}{}", id.scope, id.index, inst ? " " + inst->fullName() : "");
            if (inst)
                v = inst;
            else if (!id.null() && context.sawRef)
                context.sawRef(id);
        } else if (kind == 3) {
            std::string inner;
            v = readContentId(in, context, text ? &inner : nullptr); // GUESS: a NetAsset content carries its uri first
            if (text)
                *text = "Content.netAsset " + inner;
        } else {
            if (text)
                *text = std::format("Content kind {} (unknown)", kind);
            return std::nullopt;
        }
        break;
    }
    case nettype::PeerAddress: {
        const std::uint64_t n = in.readVarint();
        v = static_cast<double>(n);
        if (text)
            *text = std::format("peer {}", n);
        break;
    }
    case nettype::ProtectedString:
    case nettype::ProtectedStringBytecode:
    case nettype::BinaryString:
        v = readKeyedString(in, context, text);
        break;
    case nettype::DictionaryString: {
        const std::uint8_t tag = in.readU8();
        if (tag & 0x80) {
            std::uint64_t id = tag & 0x7f;
            if (id == 0x7f)
                id += in.readVarint();
            if (context.stringDictionary && id < context.stringDictionary->size()) {
                v = (*context.stringDictionary)[id];
                if (text)
                    *text = std::format("\"{}\" (dictionary {})", shortText((*context.stringDictionary)[id]), id);
            } else {
                v = std::string();
                if (text)
                    *text = std::format("(dictionary id {} not in the schema)", id);
            }
        } else if (tag == 0x7f) {
            std::string s = in.readString();
            if (text)
                *text = std::format("\"{}\"", shortText(s));
            v = std::move(s);
        } else {
            const auto bytes = in.readBytes(tag);
            std::string s(bytes.begin(), bytes.end());
            if (text)
                *text = std::format("\"{}\"", shortText(s));
            v = std::move(s);
        }
        break;
    }
    case nettype::Buffer: {
        const std::uint64_t packed = in.readVarint();
        const std::uint64_t raw = in.readVarint();
        if (packed == 0) {
            const auto bytes = in.readBytes(raw);
            v = Buffer{std::string(bytes.begin(), bytes.end())};
            if (text)
                *text = std::format("buffer {} bytes", raw);
        } else {
            const auto bytes = in.readBytes(packed);
            std::string out(raw, '\0');
            const std::size_t n = raw ? ZSTD_decompress(out.data(), out.size(), bytes.data(), bytes.size()) : 0;
            if (ZSTD_isError(n) || n != raw) {
                if (text)
                    *text = std::format("buffer {} bytes (zstd {} bytes, undecodable)", raw, packed);
                return std::nullopt;
            }
            v = Buffer{std::move(out)};
            if (text)
                *text = std::format("buffer {} bytes (zstd {} bytes)", raw, packed);
        }
        break;
    }
    case nettype::NetAssetHandle:
    case nettype::NetAssetRef: {
        // GUESS (sub_1055ed274 with its FFlag on): varint schema type (0xffff = none); then u64, varint.
        const std::uint64_t handleType = in.readVarint();
        if (handleType == 0xffff) {
            if (text)
                *text = "NetAsset none";
        } else {
            const std::uint64_t guid = in.readU64();
            const std::uint64_t index = in.readVarint();
            if (text)
                *text = std::format("NetAsset type {} guid {:#x} index {}", handleType, guid, index);
        }
        break;
    }
    default:
        if (text)
            *text = std::format("(type {:#04x} not decoded)", type);
        return std::nullopt;
    }
    if (in.bad())
        return std::nullopt;
    return v;
}

// -- ID_SET_GLOBALS

std::vector<std::uint8_t> encodeSetGlobals(const SetGlobals& g, std::uint64_t negotiatedCapabilities)
{
    NetworkStream out;
    out.writeU8(msg::SetGlobals);
    out.writeU8(g.streamingEnabled ? 1 : 0);
    out.writeU8(g.notCloudEdit ? 1 : 0);
    for (bool b : g.settings)
        out.writeU8(b ? 1 : 0);
    out.writeU8(g.flag8 ? 1 : 0);
    out.writeVarint(g.peerId);
    if (negotiatedCapabilities & (1ULL << 40))
        out.writeU32(g.workspaceValue.value_or(0));
    out.writeVarint(g.containers.size());
    for (const TopContainer& c : g.containers) {
        writeInstanceId(out, c.id);
        out.writeU16(c.classId);
        out.writeU8(c.flagA ? 1 : 0);
        out.writeU8(c.flagB ? 1 : 0);
    }
    return std::move(out).take();
}

std::expected<SetGlobals, std::string> decodeSetGlobals(std::span<const std::uint8_t> message,
                                                        std::uint64_t negotiatedCapabilities, Node* tree,
                                                        const Schema* schema)
{
    NetworkStream in(message);
    in.readU8();
    SetGlobals g;
    g.streamingEnabled = in.readU8() != 0;
    g.notCloudEdit = in.readU8() != 0;
    for (bool& b : g.settings)
        b = in.readU8() != 0;
    g.flag8 = in.readU8() != 0;
    g.peerId = static_cast<std::uint32_t>(in.readVarint());
    if (negotiatedCapabilities & (1ULL << 40))
        g.workspaceValue = in.readU32();
    if (in.bad())
        return std::unexpected("ID_SET_GLOBALS truncated before the top containers");
    if (tree) {
        tree->add(std::format("streamingEnabled: {} (GUESS)", g.streamingEnabled));
        tree->add(std::format("notCloudEdit: {} (GUESS)", g.notCloudEdit));
        tree->add(std::format("settings: {} {} {} {} (four bools, meaning GUESS)", g.settings[0], g.settings[1],
                              g.settings[2], g.settings[3]));
        tree->add(std::format("flag8: {} (GUESS)", g.flag8));
        tree->add(std::format("peerId: {} (the scope of this client's instance ids)", g.peerId));
        if (g.workspaceValue)
            tree->add(std::format("workspaceValue: {} (capability bit 40; GUESS)", *g.workspaceValue));
    }
    const std::uint64_t n = in.readVarint();
    if (in.bad() || n > 4096)
        return std::unexpected(std::format("Too many top replication containers ({})", n));
    Node* list = tree ? &tree->add(std::format("top replication containers ({})", n)) : nullptr;
    for (std::uint64_t i = 0; i < n; ++i) {
        TopContainer c;
        c.id = readInstanceId(in);
        c.classId = in.readU16();
        c.flagA = in.readU8() != 0;
        c.flagB = in.readU8() != 0;
        if (in.bad())
            return std::unexpected("ID_SET_GLOBALS truncated in the top containers");
        if (list) {
            const std::string name = schema && c.classId < schema->classes.size()
                                         ? schema->classes[c.classId].name
                                         : std::format("class {}", c.classId);
            list->add(std::format("[{}] {} id {}:{} flags {} {}", i, name, c.id.scope, c.id.index, c.flagA, c.flagB));
        }
        g.containers.push_back(c);
    }
    if (tree && in.left() > 0) {
        Node& rest = tree->add(std::format("unparsed tail at +{}", in.offset()));
        addHex(rest, message.subspan(in.offset()), in.offset(), 256);
    }
    return g;
}

// -- attribute values (the AttributesSerialize type codes; engine/src/BinaryFormat.cpp decodes whole blobs)

namespace {

enum : std::uint8_t {
    attrString = 0x02,
    attrBool = 0x03,
    attrFloat64 = 0x06,
    attrUDim = 0x09,
    attrUDim2 = 0x0a,
    attrBrickColor = 0x0e,
    attrColor3 = 0x0f,
    attrVector2 = 0x10,
    attrVector3 = 0x11,
    attrCFrame = 0x14,
    attrEnum = 0x15,
    attrNumberSequence = 0x17,
    attrColorSequence = 0x19,
    attrNumberRange = 0x1b,
    attrRect = 0x1c,
    attrFont = 0x21,
};

void writeAttrString(NetworkStream& out, std::string_view s)
{
    out.writeString32(s);
}

std::string readAttrString(NetworkStream& in)
{
    return in.readString32();
}

} // namespace

bool encodeAttributeValue(NetworkStream& out, const Value& value)
{
    return std::visit(
        [&](const auto& x) -> bool {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) {
                out.writeU8(attrString);
                writeAttrString(out, x);
            } else if constexpr (std::is_same_v<T, bool>) {
                out.writeU8(attrBool);
                out.writeU8(x ? 1 : 0);
            } else if constexpr (std::is_same_v<T, double>) {
                out.writeU8(attrFloat64);
                out.writeF64(x);
            } else if constexpr (std::is_same_v<T, lunar::UDim>) {
                out.writeU8(attrUDim);
                out.writeF32(x.scale);
                out.writeI32(x.offset);
            } else if constexpr (std::is_same_v<T, lunar::UDim2>) {
                out.writeU8(attrUDim2);
                out.writeF32(x.x.scale);
                out.writeI32(x.x.offset);
                out.writeF32(x.y.scale);
                out.writeI32(x.y.offset);
            } else if constexpr (std::is_same_v<T, lunar::BrickColor>) {
                out.writeU8(attrBrickColor);
                out.writeU32(x.number);
            } else if constexpr (std::is_same_v<T, lunar::Color3>) {
                out.writeU8(attrColor3);
                out.writeF32(x.r);
                out.writeF32(x.g);
                out.writeF32(x.b);
            } else if constexpr (std::is_same_v<T, lunar::Vector2>) {
                out.writeU8(attrVector2);
                out.writeF32(x.x);
                out.writeF32(x.y);
            } else if constexpr (std::is_same_v<T, lunar::Vector3>) {
                out.writeU8(attrVector3);
                out.writeF32(x.x);
                out.writeF32(x.y);
                out.writeF32(x.z);
            } else if constexpr (std::is_same_v<T, lunar::CFrame>) {
                out.writeU8(attrCFrame);
                out.writeF32(x.position.x);
                out.writeF32(x.position.y);
                out.writeF32(x.position.z);
                out.writeU8(0);
                for (const auto& row : x.r)
                    for (float cell : row)
                        out.writeF32(cell);
            } else if constexpr (std::is_same_v<T, lunar::EnumItem>) {
                out.writeU8(attrEnum);
                writeAttrString(out, x.enumType ? x.enumType->name : std::string_view{});
                out.writeU32(x.value);
            } else if constexpr (std::is_same_v<T, lunar::NumberSequence>) {
                out.writeU8(attrNumberSequence);
                out.writeU32(static_cast<std::uint32_t>(x.keypoints.size()));
                for (const auto& k : x.keypoints) {
                    out.writeF32(k.envelope);
                    out.writeF32(k.time);
                    out.writeF32(k.value);
                }
            } else if constexpr (std::is_same_v<T, lunar::ColorSequence>) {
                out.writeU8(attrColorSequence);
                out.writeU32(static_cast<std::uint32_t>(x.keypoints.size()));
                for (const auto& k : x.keypoints) {
                    out.writeF32(0); // envelope
                    out.writeF32(k.time);
                    out.writeF32(k.value.r);
                    out.writeF32(k.value.g);
                    out.writeF32(k.value.b);
                }
            } else if constexpr (std::is_same_v<T, lunar::NumberRange>) {
                out.writeU8(attrNumberRange);
                out.writeF32(x.min);
                out.writeF32(x.max);
            } else if constexpr (std::is_same_v<T, lunar::Rect>) {
                out.writeU8(attrRect);
                out.writeF32(x.min.x);
                out.writeF32(x.min.y);
                out.writeF32(x.max.x);
                out.writeF32(x.max.y);
            } else if constexpr (std::is_same_v<T, lunar::Font>) {
                out.writeU8(attrFont);
                out.writeU16(static_cast<std::uint16_t>(x.weight));
                out.writeU8(static_cast<std::uint8_t>(x.style));
                writeAttrString(out, x.family);
                writeAttrString(out, ""); // cached face id
            } else {
                return false;
            }
            return true;
        },
        value);
}

std::optional<Value> decodeAttributeValue(std::span<const std::uint8_t> bytes, std::string* text)
{
    NetworkStream in(bytes);
    std::string scratch;
    std::string& t = text ? *text : scratch;
    Value v;
    const std::uint8_t type = in.readU8();
    switch (type) {
    case attrString: {
        std::string s = readAttrString(in);
        if (text)
            t = std::format("\"{}\"", shortText(s));
        v = std::move(s);
        break;
    }
    case attrBool:
        v = in.readU8() != 0;
        if (text)
            t = std::get<bool>(v) ? "true" : "false";
        break;
    case attrFloat64:
        v = in.readF64();
        if (text)
            t = std::format("{}", std::get<double>(v));
        break;
    case 0x04: { // int32
        const std::int32_t n = in.readI32();
        v = static_cast<double>(n);
        if (text)
            t = std::to_string(n);
        break;
    }
    case 0x05: { // float32
        const float f = in.readF32();
        v = static_cast<double>(f);
        if (text)
            t = std::format("{}", f);
        break;
    }
    case attrUDim: {
        lunar::UDim u;
        u.scale = in.readF32();
        u.offset = in.readI32();
        v = u;
        if (text)
            t = std::format("UDim({}, {})", u.scale, u.offset);
        break;
    }
    case attrUDim2: {
        lunar::UDim2 u;
        u.x.scale = in.readF32();
        u.x.offset = in.readI32();
        u.y.scale = in.readF32();
        u.y.offset = in.readI32();
        v = u;
        if (text)
            t = std::format("UDim2({}, {}, {}, {})", u.x.scale, u.x.offset, u.y.scale, u.y.offset);
        break;
    }
    case attrBrickColor: {
        const std::uint32_t n = in.readU32();
        v = lunar::BrickColor::fromNumber(static_cast<int>(n));
        if (text)
            t = std::format("BrickColor {}", n);
        break;
    }
    case attrColor3: {
        lunar::Color3 c;
        c.r = in.readF32();
        c.g = in.readF32();
        c.b = in.readF32();
        v = c;
        if (text)
            t = std::format("Color3({}, {}, {})", c.r, c.g, c.b);
        break;
    }
    case attrVector2: {
        lunar::Vector2 p;
        p.x = in.readF32();
        p.y = in.readF32();
        v = p;
        if (text)
            t = std::format("Vector2({}, {})", p.x, p.y);
        break;
    }
    case attrVector3: {
        lunar::Vector3 p;
        p.x = in.readF32();
        p.y = in.readF32();
        p.z = in.readF32();
        v = p;
        if (text)
            t = std::format("Vector3({}, {}, {})", p.x, p.y, p.z);
        break;
    }
    case attrCFrame: {
        lunar::CFrame cf;
        cf.position.x = in.readF32();
        cf.position.y = in.readF32();
        cf.position.z = in.readF32();
        const std::uint8_t id = in.readU8();
        if (id == 0) {
            for (auto& row : cf.r)
                for (float& cell : row)
                    cell = in.readF32();
        } else if (auto r = rotationFromId(id)) {
            r->position = cf.position;
            cf = *r;
        } else {
            if (text)
                t = std::format("bad CFrame rotation id {:#04x}", id);
            return std::nullopt;
        }
        v = cf;
        if (text)
            t = std::format("CFrame({}, {}, {})", cf.position.x, cf.position.y, cf.position.z);
        break;
    }
    case attrEnum: {
        const std::string name = readAttrString(in);
        const std::uint32_t value = in.readU32();
        const EnumInfo* info = enums::find(name);
        const EnumItem* item = info ? info->find(value) : nullptr;
        if (text)
            t = std::format("Enum.{}.{}", name, item ? std::string(item->name) : std::to_string(value));
        if (!item)
            return std::nullopt;
        v = *item;
        break;
    }
    case attrNumberSequence: {
        const std::uint32_t n = in.readU32();
        std::vector<NumberSequenceKeypoint> points;
        for (std::uint32_t i = 0; i < n && !in.bad() && i < 1024; ++i) {
            NumberSequenceKeypoint k;
            k.envelope = in.readF32();
            k.time = in.readF32();
            k.value = in.readF32();
            points.push_back(k);
        }
        v = lunar::NumberSequence(std::move(points));
        if (text)
            t = std::format("NumberSequence[{}]", n);
        break;
    }
    case attrColorSequence: {
        const std::uint32_t n = in.readU32();
        std::vector<ColorSequenceKeypoint> points;
        for (std::uint32_t i = 0; i < n && !in.bad() && i < 1024; ++i) {
            ColorSequenceKeypoint k;
            in.readF32();
            k.time = in.readF32();
            k.value.r = in.readF32();
            k.value.g = in.readF32();
            k.value.b = in.readF32();
            points.push_back(k);
        }
        v = lunar::ColorSequence(std::move(points));
        if (text)
            t = std::format("ColorSequence[{}]", n);
        break;
    }
    case attrNumberRange: {
        lunar::NumberRange r;
        r.min = in.readF32();
        r.max = in.readF32();
        v = r;
        if (text)
            t = std::format("NumberRange({}, {})", r.min, r.max);
        break;
    }
    case attrRect: {
        lunar::Rect r;
        r.min.x = in.readF32();
        r.min.y = in.readF32();
        r.max.x = in.readF32();
        r.max.y = in.readF32();
        v = r;
        if (text)
            t = std::format("Rect({}, {}, {}, {})", r.min.x, r.min.y, r.max.x, r.max.y);
        break;
    }
    case attrFont: {
        lunar::Font f;
        f.weight = static_cast<Enum::FontWeight>(in.readU16());
        f.style = static_cast<Enum::FontStyle>(in.readU8());
        f.family = readAttrString(in);
        readAttrString(in);
        if (text)
            t = std::format("Font(\"{}\")", f.family);
        v = std::move(f);
        break;
    }
    default:
        if (text)
            t = std::format("attribute type {:#04x} isn't supported", type);
        return std::nullopt;
    }
    if (in.bad()) {
        if (text)
            t += " (truncated)";
        return std::nullopt;
    }
    return v;
}

// -- the items

namespace {

std::string idText(InstanceId id)
{
    return id.null() ? "nil" : std::format("{}:{}", id.scope, id.index);
}

std::string refText(InstanceId id, const ValueContext& context)
{
    if (!id.null() && context.resolve)
        if (InstancePtr inst = context.resolve(id))
            return std::format("{} ({})", inst->fullName(), idText(id));
    return idText(id);
}

// One join instance: ref id, u16 class, one property list, ref parent.
void writeJoinInstance(NetworkStream& out, const NewInstanceItem& n, const Schema& schema, const ValueContext& context)
{
    writeInstanceId(out, n.id);
    out.writeU16(n.classId);
    std::vector<std::pair<std::uint16_t, Value>> all = n.properties;
    all.insert(all.end(), n.refProperties.begin(), n.refProperties.end());
    writePropertyList(out, all, schema.classes[n.classId], schema, context);
    writeInstanceId(out, n.parent);
}

bool readJoinInstance(NetworkStream& in, NewInstanceItem& n, const Schema& schema, const ValueContext& context,
                      Node* tree, std::string& error)
{
    n.id = readInstanceId(in);
    n.classId = in.readU16();
    if (in.bad()) {
        error = "truncated join instance";
        return false;
    }
    if (n.classId >= schema.classes.size()) {
        error = std::format("Join data contained invalid class network id ({})", n.classId);
        return false;
    }
    const SchemaClass& cls = schema.classes[n.classId];
    if (tree)
        tree->text += std::format(": {} {}", cls.name, idText(n.id));
    ValueContext c = context;
    c.prefixedContent = false; // the join readers pass isJoin = 0 (plain content ids)
    if (!readPropertyList(in, n, false, cls, schema, c, tree, error))
        return false;
    n.parent = readInstanceId(in);
    if (tree)
        tree->add("parent: " + refText(n.parent, context));
    return !in.bad();
}

void writeNetAssetBatch(NetworkStream& out, const NetAssetBatch& b)
{
    out.writeVarint(b.handleType);
    if (b.handleType != 0xffff) {
        out.writeU64(b.guid);
        out.writeVarint(b.index);
    }
    out.writeVarint(b.bytes.size());
    out.writeBytes(b.bytes);
}

std::optional<NetAssetBatch> readNetAssetBatch(NetworkStream& in)
{
    NetAssetBatch b;
    b.handleType = in.readVarint();
    if (b.handleType != 0xffff) {
        b.guid = in.readU64();
        b.index = in.readVarint();
    }
    const std::uint64_t len = in.readVarint();
    if (in.bad() || len > in.left())
        return std::nullopt; // "NetAsset Batch Size Issue"
    b.bytes = in.readBytes(len);
    return b;
}

} // namespace

std::vector<NewInstanceItem> readJoinInstances(std::span<const std::uint8_t> blob, std::uint32_t count,
                                               const Schema& schema, const ValueContext& context, std::string& error)
{
    std::vector<NewInstanceItem> out;
    NetworkStream body(blob);
    std::set<SharedStringKey> wanted;
    for (std::uint32_t i = 0; i < count && i < 1000000; ++i) {
        NewInstanceItem ni;
        if (!readJoinInstance(body, ni, schema, context, nullptr, error))
            break;
        for (const auto& [pid, key] : ni.pendingSharedStrings)
            wanted.insert(key);
        out.push_back(std::move(ni));
    }
    // The shared strings the instances named and the receiver lacked follow
    // them, as in Join Data (sub_105605204): into the cache, where the
    // replica finds them for the instances' properties.
    while (!wanted.empty() && error.empty() && body.left() > 0) {
        auto e = readSharedStringEntry(body);
        if (!e || e->marker != 0)
            break;
        wanted.erase(e->key);
        if (context.sharedStrings)
            context.sharedStrings->add(e->content);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> writeJoinInstances(std::span<const NewInstanceItem> instances,
                                                            const Schema& schema, const ValueContext& context)
{
    NetworkStream body;
    std::vector<SharedStringEntry> deferred;
    ValueContext c = context;
    std::set<SharedStringKey> seen;
    c.deferSharedString = [&](const SharedStringKey& key, std::string_view content) {
        if (seen.insert(key).second)
            deferred.push_back({key, 0, std::string(content)});
    };
    for (const NewInstanceItem& n : instances) {
        if (n.classId >= schema.classes.size())
            return std::nullopt;
        writeJoinInstance(body, n, schema, c);
    }
    for (const SharedStringEntry& e : deferred) {
        writeSharedStringEntry(body, e);
        if (context.sharedStrings)
            context.sharedStrings->add(e.content);
    }
    return std::move(body).take();
}

bool encodeJoinItem(NetworkStream& out, const Item& item, const Schema& schema, const ValueContext& context,
                    const Framing& framing)
{
    auto type = [&](std::uint8_t t) { out.writeU8(framing.remapIds ? encodeItemType(t) : t); };
    if (const auto* j = std::get_if<JoinDataV2Item>(&item)) {
        NetworkStream body;
        if (j->mode == 1) {
            for (const SharedStringEntry& e : j->sharedStrings)
                writeSharedStringEntry(body, e);
        } else if (j->mode == 5) {
            for (const NetAssetBatch& b : j->netAssets)
                writeNetAssetBatch(body, b);
        } else {
            for (const NewInstanceItem& n : j->instances) {
                if (n.classId >= schema.classes.size())
                    return false;
                writeJoinInstance(body, n, schema, context);
            }
        }
        type(item::JoinDataV2);
        out.writeU8(j->mode);
        writeCompressedBlock(out, body.bytes());
        if (j->mode >= 2 && j->mode <= 4) {
            out.writeU32(static_cast<std::uint32_t>(j->instances.size()));
            out.writeVarint(j->invalidRanges.size());
            for (auto [offset, length] : j->invalidRanges) {
                out.writeVarint(offset);
                out.writeVarint(length);
            }
        }
        return true;
    }
    if (const auto* j = std::get_if<JoinDataItem>(&item)) {
        auto body = writeJoinInstances(j->instances, schema, context);
        if (!body)
            return false;
        type(item::JoinData);
        out.writeU32(static_cast<std::uint32_t>(j->instances.size()));
        writeCompressedBlock(out, *body);
        return true;
    }
    if (const auto* t = std::get_if<TagItem>(&item)) {
        type(item::Tag);
        out.writeU32(t->tag);
        return true;
    }
    if (const auto* s = std::get_if<ServerTimeItem>(&item)) {
        type(item::ServerTime);
        out.writeZigzag64(s->time);
        return true;
    }
    if (const auto* r = std::get_if<ResolveAtomicTreeItem>(&item)) {
        type(item::ResolveAtomicTree);
        writeInstanceId(out, r->instance);
        writeInstanceId(out, r->other);
        return true;
    }
    if (const auto* a = std::get_if<ChangeAttributeItem>(&item)) {
        NetworkStream blob;
        if (!std::holds_alternative<std::monostate>(a->value) && !encodeAttributeValue(blob, a->value))
            return false;
        type(item::ChangeAttribute);
        writeInstanceId(out, a->id);
        out.writeString(a->name);
        out.writeString(std::string_view(reinterpret_cast<const char*>(blob.bytes().data()), blob.bytes().size()));
        return true;
    }
    if (const auto* c = std::get_if<ChangeInstItem>(&item)) {
        type(item::ChangeInst);
        out.writeU32(c->kind);
        out.writeU32(static_cast<std::uint32_t>(c->bytes.size()));
        out.writeBytes(c->bytes);
        return true;
    }
    return false;
}

bool decodeJoinItem(std::uint8_t t, NetworkStream& in, const Schema& schema, const ValueContext& context, Node* n,
                    std::string& error, std::vector<Item>& items)
{
    switch (t) {
    case item::JoinDataV2: {
        JoinDataV2Item j;
        j.mode = in.readU8();
        auto blob = readCompressedBlock(in);
        if (!blob) {
            error = "Join Data V2: " + blob.error();
            return false;
        }
        if (n)
            n->text += std::format(": mode {} ({}), blob {} bytes", j.mode,
                                   j.mode == 1   ? "shared strings"
                                   : j.mode == 5 ? "NetAssets"
                                   : (j.mode >= 2 && j.mode <= 4) ? "instances"
                                                                  : "unknown",
                                   blob->size());
        std::uint32_t count = 0;
        if (j.mode >= 2 && j.mode <= 4) {
            count = in.readU32();
            const std::uint64_t invalid = in.readVarint();
            for (std::uint64_t i = 0; i < invalid && !in.bad() && i < 65536; ++i) {
                const auto offset = static_cast<std::uint32_t>(in.readVarint());
                const auto length = static_cast<std::uint32_t>(in.readVarint());
                if (offset > blob->size() || length > blob->size() || offset + length > blob->size()) {
                    error = "Offset index of JDIv2 larger than bitstream length";
                    return false;
                }
                j.invalidRanges.emplace_back(offset, length);
            }
            if (n) {
                n->text += std::format(", {} instance(s)", count);
                for (auto [offset, length] : j.invalidRanges)
                    n->add(std::format("invalid range: offset {} length {}", offset, length));
            }
        }
        if (in.bad()) {
            error = "Join Data V2 truncated";
            return false;
        }
        NetworkStream body(*blob);
        if (j.mode == 1) {
            while (body.left() > 0) {
                auto e = readSharedStringEntry(body);
                if (!e) {
                    error = "malformed shared string dictionary";
                    break;
                }
                if (e->marker == 0 && context.sharedStrings)
                    context.sharedStrings->add(e->content);
                if (n)
                    n->add(std::format("shared string {} marker {}: {} bytes {}", keyText(e->key), e->marker,
                                       e->content.size(), e->marker == 0 ? "\"" + shortText(e->content) + "\"" : "(echo)"));
                j.sharedStrings.push_back(std::move(*e));
            }
        } else if (j.mode == 5) {
            while (body.left() > 0) {
                auto b = readNetAssetBatch(body);
                if (!b) {
                    error = "NetAsset Batch Size Issue";
                    break;
                }
                if (n)
                    n->add(std::format("NetAsset type {} guid {:#x} index {}: {} bytes", b->handleType, b->guid, b->index,
                                       b->bytes.size()));
                j.netAssets.push_back(std::move(*b));
            }
        } else if (j.mode >= 2 && j.mode <= 4) {
            // The sender's own holes in the blob ("Skipping offset %u length %u"):
            // cached instances it won't send this client, in no particular order
            // [live: a game batch listed them unsorted; read in
            // order, a skipped union was parsed and the batch's last 1298
            // instances were lost].
            std::map<std::uint64_t, std::uint64_t> holes;
            for (auto [offset, length] : j.invalidRanges)
                holes.emplace(offset, length);
            for (std::uint32_t i = 0; i < count && error.empty(); ++i) {
                for (auto hole = holes.find(body.offset()); hole != holes.end(); hole = holes.find(body.offset())) {
                    body.seek(body.offset() + hole->second);
                    holes.erase(hole);
                }
                NewInstanceItem ni;
                Node* in_ = n ? &n->add(std::format("+{} instance {}", body.offset(), i)) : nullptr;
                if (!readJoinInstance(body, ni, schema, context, in_, error))
                    break;
                j.instances.push_back(std::move(ni));
            }
        } else {
            error = std::format("Join Data V2 mode {} unknown", j.mode);
        }
        if (!error.empty()) {
            if (n) {
                Node& e = n->add("stopped inside the blob: " + error);
                const std::size_t at = std::min(body.offset(), blob->size());
                addHex(e, std::span(*blob).subspan(at), at, 256);
            }
            // The outer stream is intact (the blob has its own length): keep going with what was read.
            error.clear();
        }
        items.emplace_back(std::move(j));
        return true;
    }
    case item::JoinData: {
        JoinDataItem j;
        const std::uint32_t count = in.readU32();
        if (count == 0) {
            if (n)
                n->text += ": empty";
            items.emplace_back(std::move(j));
            return true;
        }
        auto blob = readCompressedBlock(in);
        if (!blob) {
            error = "Join Data: " + blob.error();
            return false;
        }
        if (n)
            n->text += std::format(": {} instance(s), blob {} bytes", count, blob->size());
        NetworkStream body(*blob);
        std::vector<std::pair<std::size_t, std::vector<std::pair<std::uint16_t, SharedStringKey>>>> pending;
        for (std::uint32_t i = 0; i < count && i < 1000000; ++i) {
            NewInstanceItem ni;
            Node* in_ = n ? &n->add(std::format("+{} instance {}", body.offset(), i)) : nullptr;
            if (!readJoinInstance(body, ni, schema, context, in_, error))
                break;
            if (!ni.pendingSharedStrings.empty())
                pending.emplace_back(j.instances.size(), ni.pendingSharedStrings);
            j.instances.push_back(std::move(ni));
        }
        // The contents the instances referenced and the receiver lacked (sub_105605204).
        std::set<SharedStringKey> wanted;
        for (const auto& [index, keys] : pending)
            for (const auto& [pid, key] : keys)
                wanted.insert(key);
        while (!wanted.empty() && error.empty() && body.left() > 0) {
            auto e = readSharedStringEntry(body);
            if (!e || e->marker != 0) {
                error = "deferred shared string malformed";
                break;
            }
            if (n)
                n->add(std::format("deferred shared string {}: {} bytes", keyText(e->key), e->content.size()));
            if (!wanted.erase(e->key) && n)
                n->add("  (not one that was pending)");
            if (context.sharedStrings)
                context.sharedStrings->add(e->content);
            for (const auto& [index, keys] : pending)
                for (const auto& [pid, key] : keys)
                    if (key == e->key)
                        for (auto& [id, value] : j.instances[index].properties)
                            if (id == pid)
                                value = e->content;
        }
        if (!error.empty()) {
            if (n) {
                Node& e = n->add("stopped inside the blob: " + error);
                const std::size_t at = std::min(body.offset(), blob->size());
                addHex(e, std::span(*blob).subspan(at), at, 256);
            }
            error.clear();
        }
        items.emplace_back(std::move(j));
        return true;
    }
    case item::Tag: {
        TagItem t2{in.readU32()};
        if (n)
            n->text += std::format(": tag {}{}", t2.tag, t2.tag == kTagJoinDataDone ? " (join data done)" : "");
        items.emplace_back(t2);
        return !in.bad();
    }
    case item::ServerTime: {
        ServerTimeItem s{in.readZigzag64()};
        if (n)
            n->text += std::format(": {}", s.time);
        items.emplace_back(s);
        return !in.bad();
    }
    case item::ResolveAtomicTree: {
        ResolveAtomicTreeItem r;
        r.instance = readInstanceId(in);
        r.other = readInstanceId(in);
        if (n)
            n->text += std::format(": {} / {}", refText(r.instance, context), refText(r.other, context));
        items.emplace_back(r);
        return !in.bad();
    }
    case item::ChangeAttribute: {
        ChangeAttributeItem a;
        a.id = readInstanceId(in);
        a.name = in.readString(1u << 16);
        const std::string blob = in.readString(1u << 24);
        std::string text = "nil";
        if (!blob.empty()) {
            auto v = decodeAttributeValue(std::span(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()),
                                          n ? &text : nullptr);
            if (v)
                a.value = std::move(*v);
        }
        if (n)
            n->text += std::format(": {}.{} = {}", refText(a.id, context), a.name, text);
        items.emplace_back(std::move(a));
        return !in.bad();
    }
    case item::ChangeInst: {
        ChangeInstItem c;
        c.kind = in.readU32();
        const std::uint32_t len = in.readU32();
        if (in.bad() || len > in.left()) {
            error = "Change Inst truncated";
            return false;
        }
        c.bytes = in.readBytes(len);
        if (n) {
            n->text += std::format(": kind {}, {} bytes (meaning GUESS)", c.kind, len);
            addHex(*n, c.bytes, 0, 64);
        }
        items.emplace_back(std::move(c));
        return true;
    }
    default:
        return false;
    }
}

} // namespace lunar::net
