#include <lunar/net/Replication.hpp>

#include <lunar/net/Messages.hpp>
#include <lunar/net/Motion.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Physics.hpp>

#include <lunar/ClassRegistry.hpp>
#include <lunar/Enums.hpp>
#include <lunar/Instance.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/Property.hpp>
#include <lunar/classes/Core.hpp>
#include <lunar/classes/Effects.hpp>
#include <lunar/classes/Parts.hpp>
#include <lunar/classes/World.hpp>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <format>
#include <print>
#include <set>
#include <span>

namespace lunar::net {

namespace {

// Sets a property by the name the schema gives it, which is the serialized
// one ("size", "Color3uint8" ...), the way the place loaders map those names.
// One the engine doesn't model (or can't set) is kept on the instance as it
// came (Instance::unmodelled), for the console's gethiddenproperty [sUNC
// reads a MeshPart's AeroMeshData]: Instance::set could only throw for it,
// and the servers send a hundred thousand and more of those in a session
// (for example, when loading a large place).
void setNetworkProperty(Instance& instance, std::string_view name, const Value& value)
{
    if (const Property* property = findSerializedProperty(instance, name))
        property->set(instance, value);
    else
        instance.setUnmodelled(name, value);
}

// From the binary (ROBLOX-LAYER.md 1.1/1.2; emu/vectors/id_remap.json): raw -> wire.
constexpr std::array<std::uint8_t, 256> kPacketEncode{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
    0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e,
    0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x83, 0xcc, 0xa0,
    0xff, 0xae, 0x87, 0x88, 0xf5, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0xd3, 0x92, 0x93, 0xe5, 0xb9, 0xe8, 0x97,
    0x89, 0xc5, 0xe9, 0xe3, 0x9c, 0xb2, 0x94, 0xdc, 0xbf, 0xa1, 0xed, 0xc4, 0xc9, 0xa5, 0xdb, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb6, 0xd6, 0x9f, 0xf7, 0xa3, 0xfe, 0xb0, 0xfd, 0xd2, 0xeb, 0xf0, 0xe0, 0xe7, 0xc3, 0xcb, 0xd9, 0xf1, 0xbc, 0xca,
    0xde, 0xb4, 0xea, 0xa4, 0xd1, 0xf8, 0xf3, 0xb8, 0x96, 0xcf, 0xe6, 0x9e, 0xc7, 0xd7, 0xf2, 0x85, 0x82, 0xc8, 0xcd,
    0xac, 0xab, 0x9b, 0x84, 0xc0, 0xb5, 0xd5, 0xc2, 0xf6, 0xa2, 0xd8, 0xaf, 0xad, 0xee, 0xe2, 0xbd, 0xc6, 0xf4, 0xba,
    0xd0, 0xa6, 0x91, 0x95, 0xfa, 0xc1, 0xb3, 0x86, 0xf9, 0xd4, 0xec, 0xef, 0x98, 0xfc, 0xfb, 0xdf, 0xdd, 0xbe, 0xbb,
    0x9d, 0xe1, 0xb1, 0x9a, 0xce, 0x99, 0xb7, 0xda, 0xe4,
};
constexpr std::array<std::uint8_t, 256> kItemEncode{
    0x00, 0x83, 0xae, 0x5f, 0x33, 0x86, 0x78, 0xf8, 0x37, 0xf0, 0x35, 0xd7, 0x9c, 0x4c, 0x99, 0xa4, 0x8e, 0xb2, 0xb3,
    0x56, 0x91, 0x0d, 0x16, 0xb8, 0xbe, 0xce, 0xb9, 0x90, 0xaa, 0x17, 0xd9, 0x2b, 0x43, 0x31, 0x29, 0xa9, 0xb6, 0x3f,
    0x18, 0xd0, 0xf5, 0xc7, 0xd1, 0x77, 0x2c, 0xe2, 0xb5, 0xc1, 0x87, 0xac, 0x34, 0x40, 0x75, 0x25, 0x12, 0x72, 0x62,
    0xa8, 0x5a, 0x84, 0x76, 0x8f, 0x41, 0xa2, 0x1b, 0xb4, 0x1f, 0x55, 0x26, 0xe9, 0x66, 0xdd, 0x7a, 0x3e, 0x8d, 0xa0,
    0x15, 0x48, 0x02, 0x53, 0x1c, 0x8c, 0x08, 0xc3, 0x7e, 0x05, 0x82, 0xaf, 0xb0, 0xf7, 0x8b, 0x5c, 0x10, 0xde, 0xc5,
    0x61, 0x74, 0x7d, 0xe1, 0x6d, 0x09, 0x2f, 0x50, 0x89, 0x30, 0x14, 0x9d, 0xa7, 0x3d, 0xf9, 0x5e, 0xda, 0x49, 0x4a,
    0xa5, 0x9a, 0x63, 0x19, 0xbd, 0x52, 0x96, 0x38, 0xe5, 0xd8, 0x71, 0xa6, 0x2e, 0xc8, 0xc0, 0x03, 0x3a, 0x6e, 0x59,
    0x60, 0xef, 0xbb, 0xcf, 0x94, 0x4e, 0x6f, 0x4b, 0xfc, 0x7c, 0x6c, 0xdf, 0x67, 0xfe, 0x24, 0xb1, 0x04, 0xf1, 0xe6,
    0x8a, 0x93, 0xdc, 0x95, 0x23, 0x54, 0x06, 0x0a, 0x44, 0x0b, 0xc2, 0x7f, 0xeb, 0x97, 0xe7, 0xdb, 0xc4, 0x0c, 0x3b,
    0xb7, 0x47, 0xfb, 0xf3, 0x70, 0x0f, 0xd2, 0xe0, 0x0e, 0xcd, 0x7b, 0xf4, 0x5b, 0x58, 0x3c, 0x98, 0x32, 0x9e, 0x81,
    0xa1, 0xea, 0xe3, 0xba, 0xd4, 0x2a, 0x07, 0x42, 0x1d, 0xa3, 0xca, 0x57, 0xfd, 0x6a, 0x9b, 0x13, 0x11, 0x80, 0x1e,
    0xc6, 0x6b, 0x46, 0xcb, 0x27, 0xd5, 0xbc, 0x21, 0xed, 0xd3, 0x2d, 0xab, 0xad, 0x4d, 0xc9, 0x20, 0xd6, 0x9f, 0x1a,
    0x68, 0xfa, 0x69, 0xe8, 0x51, 0x45, 0x39, 0xe4, 0x5d, 0xec, 0xf2, 0x88, 0x79, 0x22, 0x92, 0x01, 0x4f, 0x64, 0xbf,
    0x73, 0x28, 0xf6, 0x65, 0xff, 0xee, 0xcc, 0x36, 0x85,
};

struct Tables {
    std::array<std::uint8_t, 256> packetEnc{}, packetDec{}, itemEnc{}, itemDec{};
};

const Tables& tables()
{
    static const Tables t = [] {
        Tables r;
        r.packetEnc = kPacketEncode;
        r.itemEnc = kItemEncode;
        for (std::size_t i = 0; i < 256; ++i) {
            r.packetDec[r.packetEnc[i]] = static_cast<std::uint8_t>(i);
            r.itemDec[r.itemEnc[i]] = static_cast<std::uint8_t>(i);
        }
        return r;
    }();
    return t;
}

// Network type for an engine property, from the Luau type it reports.
std::uint8_t netTypeFor(const Property& p)
{
    const std::string_view t = p.type;
    if (p.isRef)
        return nettype::Object;
    if (t.starts_with("Enum."))
        return nettype::Enum;
    static const std::map<std::string_view, std::uint8_t> types{
        {"boolean", nettype::Bool},
        {"number", nettype::Double}, // GUESS: Roblox has int/float/double per property; doubles keep everything
        {"string", nettype::String},
        {"Vector3", nettype::Vector3},
        {"Vector2", nettype::Vector2},
        {"Color3", nettype::Color3},
        {"CFrame", nettype::CFrame},
        {"UDim", nettype::UDim},
        {"UDim2", nettype::UDim2},
        {"Rect", nettype::Rect2D},
        {"NumberRange", nettype::NumberRange},
        {"NumberSequence", nettype::NumberSequence},
        {"ColorSequence", nettype::ColorSequence},
        {"BrickColor", nettype::BrickColor},
        {"Faces", nettype::Faces},
        {"Axes", nettype::Axes},
        {"PhysicalProperties", nettype::PhysicalProperties},
        {"Vector3int16", nettype::Vector3int16},
        {"Font", nettype::Font},
    };
    if (t == "CFrame?")
        return nettype::OptionalCFrame;
    auto it = types.find(t.ends_with('?') ? t.substr(0, t.size() - 1) : t);
    return it == types.end() ? 0 : it->second;
}

const EnumInfo* enumFor(const Schema& schema, std::uint16_t enumId)
{
    return enumId < schema.enums.size() ? enums::findAny(schema.enums[enumId].name) : nullptr;
}

// Reads the shared-string contents an item deferred (sub_105605204): one
// entry per pending key, in any order, right after the item's values. Fills
// the properties that were waiting for them.
bool readDeferredSharedStrings(NetworkStream& in, std::vector<std::pair<std::uint16_t, SharedStringKey>>& pending,
                               std::vector<std::pair<std::uint16_t, Value>>& props, const ValueContext& context,
                               Node* tree, std::string& error)
{
    std::set<SharedStringKey> wanted;
    for (const auto& [pid, key] : pending)
        wanted.insert(key);
    while (!wanted.empty()) {
        auto e = readSharedStringEntry(in);
        if (!e || e->marker != 0) {
            error = "malformed deferred shared string";
            return false;
        }
        if (tree)
            tree->add(std::format("deferred shared string {}: {} bytes", keyText(e->key), e->content.size()));
        if (!wanted.erase(e->key)) {
            error = std::format("Received new shared string {} that does not match deferred data", keyText(e->key));
            return false;
        }
        for (const auto& [pid, key] : pending)
            if (key == e->key)
                for (auto& [id, value] : props)
                    if (id == pid)
                        value = e->content;
        if (context.sharedStrings)
            context.sharedStrings->add(std::move(e->content));
    }
    pending.clear();
    return true;
}

// The writer's side of the same: the contents the values referenced and the
// receiver lacks, collected through deferSharedString while writing.
struct DeferredStrings {
    std::vector<SharedStringEntry> entries;
    std::set<SharedStringKey> seen;
    ValueContext collecting(const ValueContext& context)
    {
        ValueContext c = context;
        c.deferSharedString = [this](const SharedStringKey& key, std::string_view content) {
            if (seen.insert(key).second)
                entries.push_back({key, 0, std::string(content)});
        };
        return c;
    }
    void write(NetworkStream& out, const ValueContext& context)
    {
        for (const SharedStringEntry& e : entries) {
            writeSharedStringEntry(out, e);
            if (context.sharedStrings)
                context.sharedStrings->add(e.content);
        }
        entries.clear();
    }
};

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

} // namespace

void writePropertyList(NetworkStream& out, const std::vector<std::pair<std::uint16_t, Value>>& props,
                       const SchemaClass& cls, const Schema& schema, const ValueContext& context)
{
    std::vector<std::pair<std::uint16_t, const Value*>> sorted;
    sorted.reserve(props.size());
    for (const auto& [id, v] : props)
        sorted.emplace_back(id, &v);
    std::ranges::sort(sorted, {}, &std::pair<std::uint16_t, const Value*>::first);
    for (const auto& [id, v] : sorted) {
        if (id < cls.firstProperty || id >= cls.firstProperty + cls.propertyCount || id - cls.firstProperty >= 0xff)
            continue;
        const SchemaProperty& p = schema.properties[id];
        NetworkStream value;
        if (!writeValue(value, p.type, *v, context))
            continue;
        out.writeU8(static_cast<std::uint8_t>(id - cls.firstProperty));
        out.writeBytes(value.bytes());
    }
    out.writeU8(0xff);
}

bool readPropertyList(NetworkStream& in, NewInstanceItem& into, bool refPass, const SchemaClass& cls,
                      const Schema& schema, const ValueContext& context, Node* tree, std::string& error)
{
    std::vector<std::pair<std::uint16_t, Value>>& props = refPass ? into.refProperties : into.properties;
    unsigned next = 0; // the lowest delta still allowed
    // One context for the whole list, its hooks reporting into these two,
    // which every property starts afresh: copying the context's functions
    // for each property was most of what reading a list cost.
    InstanceId target{};
    std::optional<SharedStringKey> missing;
    ValueContext c = context;
    c.sawRef = [&target](InstanceId seen) { target = seen; };
    c.deferSharedString = [&missing](const SharedStringKey& key, std::string_view) { missing = key; };
    for (;;) {
        const std::uint8_t delta = in.readU8();
        if (in.bad()) {
            error = std::format("truncated property list of {}", cls.name);
            return false;
        }
        if (delta == 0xff)
            return true;
        if (delta < next || delta >= cls.propertyCount) {
            error = std::format("Invalid property id: out of bounds or out of order ({} delta {} after {})", cls.name,
                                delta, next);
            return false;
        }
        next = delta + 1u;
        const auto id = static_cast<std::uint16_t>(cls.firstProperty + delta);
        const SchemaProperty& p = schema.properties[id];
        c.enumType = p.type == nettype::Enum ? enumFor(schema, p.enumId) : nullptr;
        target = {};
        missing.reset();
        std::string text;
        auto v = readValue(in, p.type, c, tree ? &text : nullptr);
        if (tree)
            tree->add(std::format("[{}] {} ({}): {}{}", delta, p.name, netTypeName(p.type), text,
                                  target.null() ? "" : " (not known yet)"));
        if (!v) {
            error = std::format("can't read {}.{} as {} ({:#04x}) at +{}", cls.name, p.name, netTypeName(p.type),
                                p.type, in.offset());
            return false;
        }
        if (p.type == nettype::Object && !target.null()) {
            into.forwardRefs.emplace_back(id, target);
            continue;
        }
        if (missing)
            into.pendingSharedStrings.emplace_back(id, *missing);
        props.emplace_back(id, std::move(*v));
    }
}

Framing Framing::fromCapabilities(std::uint64_t negotiated)
{
    Framing f;
    f.remapIds = (negotiated & (1ULL << 12)) != 0;
    f.hashed = (negotiated & (1ULL << 55)) != 0;
    return f;
}

std::uint8_t encodePacketId(std::uint8_t id)
{
    return tables().packetEnc[id];
}
std::uint8_t decodePacketId(std::uint8_t wire)
{
    return tables().packetDec[wire];
}
std::uint8_t encodeItemType(std::uint8_t type)
{
    return tables().itemEnc[type];
}
std::uint8_t decodeItemType(std::uint8_t wire)
{
    return tables().itemDec[wire];
}

std::uint32_t fnv1a32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t h = 0x811c9dc5u;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x01000193u;
    }
    return h;
}

std::vector<std::uint8_t> frame(std::vector<std::uint8_t> message, const Framing& framing)
{
    if (message.empty())
        return message;
    if (framing.remapIds)
        message[0] = encodePacketId(message[0]);
    if (!framing.hashed)
        return message;
    std::vector<std::uint8_t> out{0xaa, 0, 0, 0, 0};
    out.insert(out.end(), message.begin(), message.end());
    const std::uint32_t h = fnv1a32(std::span(out).subspan(5));
    for (std::size_t i = 0; i < 4; ++i)
        out[1 + i] = static_cast<std::uint8_t>(h >> (8 * i));
    return out;
}

std::expected<std::vector<std::uint8_t>, std::string> unframe(std::span<const std::uint8_t> wire,
                                                              const Framing& framing)
{
    std::vector<std::uint8_t> m(wire.begin(), wire.end());
    if (framing.hashed && m.size() >= 6 && m[0] == 0xaa) {
        const std::uint32_t h = static_cast<std::uint32_t>(m[1]) | static_cast<std::uint32_t>(m[2]) << 8
                                | static_cast<std::uint32_t>(m[3]) << 16 | static_cast<std::uint32_t>(m[4]) << 24;
        if (h != fnv1a32(std::span(m).subspan(5)))
            return std::unexpected("PacketCorruption: FNV-1a hash mismatch");
        m.erase(m.begin(), m.begin() + 5);
    }
    if (framing.remapIds && !m.empty())
        m[0] = decodePacketId(m[0]);
    return m;
}

Schema Schema::fromEngine()
{
    Schema s;
    std::map<std::string, std::uint16_t, std::less<>> enumIds;
    for (const EnumInfo* e : enums::all()) {
        std::uint32_t maxValue = 0;
        for (const EnumItem& item : e->items)
            maxValue = std::max(maxValue, item.value);
        enumIds.emplace(std::string(e->name), static_cast<std::uint16_t>(s.enums.size()));
        s.enums.push_back({std::string(e->name), static_cast<std::uint8_t>(std::bit_width(maxValue | 1u))});
    }
    for (const ClassInfo* info : ClassRegistry::all()) {
        SchemaClass c;
        c.name = std::string(info->name);
        c.firstProperty = static_cast<std::uint16_t>(s.properties.size());
        const auto owner = static_cast<std::uint16_t>(s.classes.size());
        // Every property the class has, bases first, the way New Instance
        // items index them (relative to the class's first id).
        std::vector<const ClassInfo*> chain;
        for (const ClassInfo* k = info; k; k = k->base)
            chain.insert(chain.begin(), k);
        std::set<std::string_view> seen;
        // Properties that are views of others (Position/Orientation of
        // CFrame, the World* ones, BrickColor of Color, the deprecated
        // velocity aliases, GuiObject.Transparency): sending both would let
        // the second undo the first's precision (or TextTransparency).
        // GUESS: Roblox's schema leaves them out too; a 0.742 server's has
        // no GuiObject.Transparency.
        const bool placed = info->isA("BasePart") || info->isA("Attachment");
        auto derived = [&](std::string_view name) {
            return (name.starts_with("World") && name != "WorldPivotData") || name == "Velocity"
                   || name == "RotVelocity" || (info->isA("GuiObject") && name == "Transparency")
                   || (placed && (name == "Position" || name == "Orientation" || name == "Rotation"))
                   || (info->isA("BasePart") && name == "BrickColor");
        };
        for (const ClassInfo* k : chain) {
            for (const Property& p : k->properties) {
                if (p.readOnly() || p.name == "Parent" || derived(p.name) || !seen.insert(p.name).second)
                    continue;
                const std::uint8_t type = netTypeFor(p);
                if (type == 0 || c.propertyCount >= 0xfe)
                    continue;
                SchemaProperty sp{std::string(p.name), type, 0xffff, 0, owner};
                if (type == nettype::Enum)
                    if (auto it = enumIds.find(p.type.substr(5)); it != enumIds.end())
                        sp.enumId = it->second;
                s.properties.push_back(std::move(sp));
                ++c.propertyCount;
            }
        }
        c.firstEvent = static_cast<std::uint16_t>(s.events.size());
        // Events the replication needs, as a live server's schema has them
        // [capture]: FireServer travels as OnRemoteServerEvent (the server
        // knows who sent it), RemoteFunction calls as an invocation id with
        // the arguments, answered by a success or an error event.
        if (info->isA("BaseRemoteEvent")) {
            s.events.push_back({"OnClientEvent", {{nettype::Tuple, 0xffff}}, owner});
            s.events.push_back({"OnRemoteServerEvent", {{nettype::Tuple, 0xffff}}, owner});
            s.events.push_back({"OnServerEvent", {{nettype::Object, 0xffff}, {nettype::Tuple, 0xffff}}, owner});
            c.eventCount = 3;
        }
        if (info->isA("RemoteFunction")) {
            s.events.push_back({"RemoteOnInvokeClient", {{nettype::Int, 0xffff}, {nettype::Tuple, 0xffff}}, owner});
            s.events.push_back({"RemoteOnInvokeError", {{nettype::Int, 0xffff}, {nettype::String, 0xffff}}, owner});
            s.events.push_back({"RemoteOnInvokeServer",
                                {{nettype::Int, 0xffff}, {nettype::Object, 0xffff}, {nettype::Tuple, 0xffff}},
                                owner});
            s.events.push_back({"RemoteOnInvokeSuccess", {{nettype::Int, 0xffff}, {nettype::Tuple, 0xffff}}, owner});
            c.eventCount = 4;
        }
        // The ones a client sends for its tools [capture: a live schema has
        // Tool.Activated() and Deactivated(), Humanoid.ServerEquipTool(Object)].
        if (info->name == "Tool") {
            s.events.push_back({"Activated", {}, owner});
            s.events.push_back({"Deactivated", {}, owner});
            c.eventCount = 2;
        }
        if (info->name == "Humanoid") {
            s.events.push_back({"ServerEquipTool", {{nettype::Object, 0xffff}}, owner});
            c.eventCount = 1;
        }
        // A Sound's playing goes both ways as events [capture: a live schema
        // has these five; the server's Play and Stop come as the FromServer ones].
        if (info->name == "Sound") {
            const std::pair<std::uint8_t, std::uint16_t> b{nettype::Bool, 0xffff}, d{nettype::Double, 0xffff},
                i{nettype::Int, 0xffff};
            s.events.push_back({"PlayingUpdatedFromClient", {b}, owner});
            s.events.push_back({"PlayingUpdatedFromServer", {b}, owner});
            s.events.push_back({"TimePositionUpdatedFromClient", {d, d}, owner});
            s.events.push_back({"TimePositionUpdatedFromServer", {d, d}, owner});
            s.events.push_back({"playbackActionSync", {i}, owner});
            c.eventCount = 5;
        }
        if (info->isA("TextChatService")) {
            // The chat's network events, parameters as the live schema lists
            // them [capture]: SendAsync goes up as ...V4 (V2/V3 are older
            // shapes the server still takes), messages come down as
            // ServerToClient... and the hidden ones as UserMessageIntentSentRemote;
            // preset chat's switches come with the join.
            using P = std::pair<std::uint8_t, std::uint16_t>;
            const P str{nettype::String, 0xffff}, obj{nettype::Object, 0xffff}, b{nettype::Bool, 0xffff},
                i64{nettype::Int64, 0xffff};
            P status{nettype::Enum, 0xffff};
            if (auto it = enumIds.find("TextChatMessageStatus"); it != enumIds.end())
                status.second = it->second;
            s.events.push_back({"ClientToServerMessageReplicateSignalV2", {str, str, str, obj, obj}, owner});
            s.events.push_back({"ClientToServerMessageReplicateSignalV3", {str, str, str, obj, obj, b, b}, owner});
            s.events.push_back(
                {"ClientToServerMessageReplicateSignalV4", {str, str, str, obj, obj, b, b, str, str}, owner});
            s.events.push_back(
                {"ServerToClientMessageReplicateSignal", {str, str, str, str, obj, obj, i64, status}, owner});
            s.events.push_back({"ServerToClientMessageReplicateSignalV2", {{nettype::Dictionary, 0xffff}}, owner});
            s.events.push_back({"ServerToClientPresetChatConfigChangedSignal", {b, str}, owner});
            s.events.push_back({"ServerToClientPresetChatUserAccessChangedSignal", {str}, owner});
            s.events.push_back({"UserMessageIntentSentRemote", {str, str, str, str, obj, obj, i64}, owner});
            c.eventCount = 8;
        }
        s.classes.push_back(std::move(c));
    }
    return s;
}

std::optional<std::uint16_t> Schema::classId(std::string_view name) const
{
    for (std::size_t i = 0; i < classes.size(); ++i)
        if (classes[i].name == name)
            return static_cast<std::uint16_t>(i);
    return std::nullopt;
}

std::optional<std::uint16_t> Schema::propertyId(std::uint16_t classId, std::string_view name) const
{
    if (classId >= classes.size())
        return std::nullopt;
    const SchemaClass& c = classes[classId];
    for (std::uint16_t i = 0; i < c.propertyCount; ++i)
        if (properties[c.firstProperty + i].name == name)
            return static_cast<std::uint16_t>(c.firstProperty + i);
    return std::nullopt;
}

std::optional<std::uint16_t> Schema::eventId(std::uint16_t classId, std::string_view name) const
{
    if (classId >= classes.size())
        return std::nullopt;
    const SchemaClass& c = classes[classId];
    for (std::uint16_t i = 0; i < c.eventCount; ++i)
        if (events[c.firstEvent + i].name == name)
            return static_cast<std::uint16_t>(c.firstEvent + i);
    return std::nullopt;
}

std::vector<std::uint8_t> Schema::encodeBody() const
{
    NetworkStream out;
    out.writeVarint(3);
    out.writeU64(flags);
    out.writeVarint(enums.size());
    for (const SchemaEnum& e : enums) {
        out.writeString(e.name);
        out.writeU8(e.size);
    }
    out.writeVarint(classes.size());
    out.writeVarint(properties.size());
    out.writeVarint(events.size());
    for (const SchemaClass& c : classes) {
        out.writeString(c.name);
        out.writeVarint(c.propertyCount);
        for (std::uint16_t i = 0; i < c.propertyCount; ++i) {
            const SchemaProperty& p = properties[c.firstProperty + i];
            out.writeString(p.name);
            out.writeU8(p.type);
            if (p.type == 0x3c)
                out.writeVarint(p.extra);
            if (p.type == nettype::Enum)
                out.writeU16(p.enumId);
        }
        out.writeU16(c.tag);
        out.writeVarint(c.eventCount);
        for (std::uint16_t i = 0; i < c.eventCount; ++i) {
            const SchemaEvent& e = events[c.firstEvent + i];
            out.writeString(e.name);
            out.writeVarint(e.params.size());
            for (auto [type, enumId] : e.params) {
                out.writeU8(type);
                out.writeU16(enumId);
            }
        }
    }
    // The trailing sections, always written (the reader takes them only when bytes remain).
    out.writeVarint(contentPrefixes.size() > 1 ? contentPrefixes.size() - 1 : 0);
    for (std::size_t i = 1; i < contentPrefixes.size(); ++i)
        out.writeString(contentPrefixes[i]);
    out.writeVarint(stringDictionary.size());
    for (const std::string& s : stringDictionary)
        out.writeString(s);
    out.writeU32(static_cast<std::uint32_t>(isrClasses.size()));
    for (const IsrClass& c : isrClasses) {
        out.writeU32(c.classId);
        out.writeU32(static_cast<std::uint32_t>(c.properties.size()));
        for (std::uint32_t p : c.properties)
            out.writeU32(p);
    }
    out.writeVarint(lrTypeRanges.size());
    for (const LrTypeRange& r : lrTypeRanges) {
        out.writeVarint(r.type);
        out.writeVarint(r.a);
        out.writeVarint(r.b);
        if (r.b != 0)
            out.writeVarint(r.c);
    }
    return std::move(out).take();
}

std::expected<Schema, std::string> Schema::decodeBody(std::span<const std::uint8_t> body)
{
    NetworkStream in(body);
    Schema s;
    if (const std::uint64_t version = in.readVarint(); version != 3)
        return std::unexpected(std::format("Requesting Invalid Schema Version ({})", version));
    s.flags = in.readU64();
    const std::uint64_t enumCount = in.readVarint();
    for (std::uint64_t i = 0; i < enumCount && !in.bad(); ++i) {
        SchemaEnum e;
        e.name = in.readString();
        e.size = in.readU8();
        s.enums.push_back(std::move(e));
    }
    const std::uint64_t classCount = in.readVarint();
    const std::uint64_t propertyCount = in.readVarint();
    const std::uint64_t eventCount = in.readVarint();
    if (in.bad() || classCount > 0xffff || propertyCount > 0x10000 || eventCount > 0x10000)
        return std::unexpected("schema: bad counts");
    for (std::uint64_t i = 0; i < classCount && !in.bad(); ++i) {
        SchemaClass c;
        const auto owner = static_cast<std::uint16_t>(i);
        c.name = in.readString();
        c.firstProperty = static_cast<std::uint16_t>(s.properties.size());
        c.propertyCount = static_cast<std::uint16_t>(in.readVarint());
        for (std::uint16_t k = 0; k < c.propertyCount && !in.bad(); ++k) {
            SchemaProperty p;
            p.name = in.readString();
            p.type = in.readU8();
            p.owner = owner;
            if (p.type == 0x3c)
                p.extra = in.readVarint();
            if (p.type == nettype::Enum)
                p.enumId = in.readU16();
            s.properties.push_back(std::move(p));
        }
        c.tag = in.readU16();
        c.firstEvent = static_cast<std::uint16_t>(s.events.size());
        c.eventCount = static_cast<std::uint16_t>(in.readVarint());
        for (std::uint16_t k = 0; k < c.eventCount && !in.bad(); ++k) {
            SchemaEvent e;
            e.name = in.readString();
            e.owner = owner;
            const std::uint64_t n = in.readVarint();
            for (std::uint64_t j = 0; j < n && !in.bad() && j < 256; ++j) {
                const std::uint8_t type = in.readU8();
                e.params.emplace_back(type, in.readU16());
            }
            s.events.push_back(std::move(e));
        }
        s.classes.push_back(std::move(c));
    }
    if (in.bad())
        return std::unexpected("schema: truncated");
    if (s.properties.size() != propertyCount || s.events.size() != eventCount)
        return std::unexpected(std::format("schema: {} properties and {} events in the classes, {} and {} announced",
                                           s.properties.size(), s.events.size(), propertyCount, eventCount));
    // The trailing sections, each only when bytes remain (sub_105575b0e+0xad2..).
    if (in.left() > 0) {
        const std::uint64_t n = in.readVarint();
        for (std::uint64_t i = 0; i < n && !in.bad() && i < 65536; ++i)
            s.contentPrefixes.push_back(in.readString());
    }
    if (in.left() > 0) {
        const std::uint64_t n = in.readVarint();
        for (std::uint64_t i = 0; i < n && !in.bad() && i < 1000000; ++i)
            s.stringDictionary.push_back(in.readString());
    }
    if (in.left() > 0) {
        const std::uint32_t n = in.readU32();
        if (n > classCount)
            return std::unexpected("Cannot construct schema, too many classes with ISR properties");
        for (std::uint32_t i = 0; i < n && !in.bad(); ++i) {
            Schema::IsrClass c;
            c.classId = in.readU32();
            if (c.classId >= classCount)
                return std::unexpected("Cannot construct schema, invalid class id for ISR properties");
            const std::uint32_t count = in.readU32();
            for (std::uint32_t k = 0; k < count && !in.bad() && k < 65536; ++k)
                c.properties.push_back(in.readU32());
            s.isrClasses.push_back(std::move(c));
        }
    }
    if (in.left() > 0) {
        const std::uint64_t n = in.readVarint();
        if (n > 0xffff)
            return std::unexpected("Cannot construct schema, invalid lr type range");
        for (std::uint64_t i = 0; i < n && !in.bad(); ++i) {
            Schema::LrTypeRange r;
            r.type = in.readVarint();
            r.a = in.readVarint();
            r.b = in.readVarint();
            if (r.b != 0)
                r.c = in.readVarint();
            s.lrTypeRanges.push_back(r);
        }
    }
    if (in.bad())
        return std::unexpected("schema: truncated in the trailing sections");
    return s;
}

std::vector<std::uint8_t> encodeSchemaPacket(const Schema& schema)
{
    const std::vector<std::uint8_t> body = schema.encodeBody();
    std::vector<std::uint8_t> packed(ZSTD_compressBound(body.size()));
    const std::size_t n = ZSTD_compress(packed.data(), packed.size(), body.data(), body.size(), 3);
    packed.resize(ZSTD_isError(n) ? 0 : n);
    NetworkStream out;
    out.writeU8(kSchemaPacketId);
    out.writeU32(static_cast<std::uint32_t>(packed.size()));
    out.writeU32(static_cast<std::uint32_t>(body.size()));
    out.writeBytes(packed);
    return std::move(out).take();
}

std::expected<Schema, std::string> decodeSchemaPacket(std::span<const std::uint8_t> message)
{
    NetworkStream in(message);
    in.readU8();
    const std::uint32_t packedSize = in.readU32(), rawSize = in.readU32();
    if (in.bad() || packedSize > in.left() || rawSize > (64u << 20))
        return std::unexpected("schema: bad compressed block");
    const auto packed = in.readBytes(packedSize);
    std::vector<std::uint8_t> body(rawSize);
    const std::size_t n = ZSTD_decompress(body.data(), body.size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != rawSize)
        return std::unexpected("schema: zstd says no");
    return Schema::decodeBody(body);
}

std::vector<std::uint8_t> encodeJoinRequest(const JoinRequest& request, std::uint64_t negotiatedCapabilities)
{
    NetworkStream out;
    out.writeU8(kJoinRequestId);
    out.writeU8(request.flags);
    out.writeU32(request.clientValue);
    if (negotiatedCapabilities & (1ULL << 20)) {
        out.writeU8(request.luauMin);
        out.writeU8(request.luauMax);
    } else if (negotiatedCapabilities & (1ULL << 11)) {
        out.writeU8(request.luauMin); // the client's single version byte is its minimum (0x1054b7644) [emu]
    }
    // GUESS: the u16-counted list is optional on the reader's side ("if
    // bytes remain"), but the join info follows it, so it is always written.
    out.writeU16(static_cast<std::uint16_t>(request.extra.size()));
    for (const std::string& e : request.extra)
        out.writeString(e);
    out.writeString(request.joinInfo);
    return std::move(out).take();
}

std::expected<JoinRequest, std::string> decodeJoinRequest(std::span<const std::uint8_t> message,
                                                          std::uint64_t negotiatedCapabilities)
{
    NetworkStream in(message);
    JoinRequest r;
    in.readU8();
    r.flags = in.readU8();
    r.clientValue = in.readU32();
    if (negotiatedCapabilities & (1ULL << 20)) {
        r.luauMin = in.readU8();
        r.luauMax = in.readU8();
    } else if (negotiatedCapabilities & (1ULL << 11)) {
        r.luauMin = r.luauMax = in.readU8();
    }
    if (in.left() >= 2) {
        const std::uint16_t n = in.readU16();
        for (std::uint16_t i = 0; i < n && !in.bad(); ++i)
            r.extra.push_back(in.readString());
    }
    r.joinInfo = in.readString();
    if (in.bad())
        return std::unexpected("truncated join request");
    return r;
}

const char* itemTypeName(std::uint8_t type)
{
    static constexpr std::array<const char*, 0x2c> names{
        "End",
        "Delete",
        "New Instance",
        "Change Property",
        "Marker",
        "Ping",
        "PingBack",
        "Event Invocation",
        "Request Character",
        "Other Items 3",
        "CFrame Ack",
        "Join Data",
        "Client Quota Update",
        "Stream Data",
        "Region Removal",
        "Instance Removal",
        "Tag",
        "Stats",
        "Other Items 4",
        "Resolve Atomic Tree",
        "Streaming Info",
        "Change Attribute",
        "Join Data V2",
        "Stream Terrain",
        "Stream Prefetch Info",
        "Stream Prefetch Request",
        "Stream Replication Focus",
        "Destroy",
        "Client Quota Stream Update",
        "Stream Model Persistent Player Add",
        "Stream Model Persistent Player Remove",
        "Client QoS",
        "Change Inst",
        "Change ObjProp",
        "Deferred SharedString",
        "ISR Timestamp",
        "Stream Replication Focus Remove",
        "Stream Pin Data",
        "Server Time",
        "?",
        "?",
        "Instance Intent",
        "Stream Excluded Instances",
        "Region Terrain LOD Removal",
    };
    return type < names.size() ? names[type] : "unknown";
}

std::vector<std::uint8_t> encodeData(std::span<const Item> items, const Schema& schema, const ValueContext& context,
                                     const Framing& framing, const ItemContext& itemContext)
{
    NetworkStream out;
    out.writeU8(0x83);
    auto type = [&](std::uint8_t t) { out.writeU8(framing.remapIds ? encodeItemType(t) : t); };
    for (const Item& it : items) {
        if (const auto* n = std::get_if<NewInstanceItem>(&it)) {
            if (n->classId >= schema.classes.size())
                continue;
            type(item::New);
            writeInstanceId(out, n->id);
            out.writeU16(n->classId);
            // Pass 1 reads content ids in prefix form, pass 2 plain (notes-joindata.md 4);
            // contents of shared strings the receiver lacks follow the passes.
            DeferredStrings deferred;
            ValueContext pass = deferred.collecting(context);
            pass.prefixedContent = true;
            writePropertyList(out, n->properties, schema.classes[n->classId], schema, pass);
            pass.prefixedContent = false;
            writePropertyList(out, n->refProperties, schema.classes[n->classId], schema, pass);
            deferred.write(out, context);
            writeInstanceId(out, n->parent);
        } else if (const auto* c = std::get_if<ChangePropertyItem>(&it)) {
            if (c->parent) {
                type(item::ChangeProperty);
                writeInstanceId(out, c->id);
                out.writeU16(static_cast<std::uint16_t>(schema.properties.size()));
                out.writeU8(c->flag ? 1 : 0);
                if (!itemContext.fromServer && c->flag)
                    out.writeZigzag32(c->flagValue);
                writeInstanceId(out, *c->parent);
                continue;
            }
            if (c->propertyId >= schema.properties.size())
                continue;
            DeferredStrings deferred;
            ValueContext ctx = deferred.collecting(context);
            ctx.prefixedContent = true;
            NetworkStream value;
            if (!writeValue(value, schema.properties[c->propertyId].type, c->value, ctx))
                continue;
            type(item::ChangeProperty);
            writeInstanceId(out, c->id);
            out.writeU16(c->propertyId);
            out.writeU8(c->flag ? 1 : 0);
            if (!itemContext.fromServer && c->flag)
                out.writeZigzag32(c->flagValue);
            out.writeBytes(value.bytes());
            deferred.write(out, context);
        } else if (const auto* d = std::get_if<DeleteItem>(&it)) {
            type(item::Delete);
            writeInstanceId(out, d->id);
        } else if (const auto* m = std::get_if<MarkerItem>(&it)) {
            type(item::Marker);
            out.writeU32(m->marker);
        } else if (const auto* e = std::get_if<EventInvocationItem>(&it)) {
            if (e->eventId >= schema.events.size())
                continue;
            type(item::EventInvocation);
            writeInstanceId(out, e->id);
            out.writeU16(e->eventId);
            std::size_t arg = 0;
            for (auto [ptype, enumId] : schema.events[e->eventId].params) {
                if (ptype == nettype::Tuple) {
                    out.writeVarint(e->arguments.size() - std::min(arg, e->arguments.size()));
                    for (; arg < e->arguments.size(); ++arg) {
                        if (auto t = e->tables.find(arg); t != e->tables.end())
                            writeVariant(out, RemoteValue{Value{}, t->second}, context);
                        else if (auto r = e->randoms.find(arg); r != e->randoms.end())
                            writeVariant(out, RemoteValue{.random = r->second}, context);
                        else
                            writeVariant(out, e->arguments[arg], context);
                    }
                } else if (isTableType(ptype)) {
                    // A table typed by the schema (TextChatService's ...V2(data)): no type byte.
                    const auto t = e->tables.find(arg);
                    if (t == e->tables.end() || !writeRemoteTable(out, ptype, RemoteValue{Value{}, t->second}, context))
                        out.writeVarint(0);
                    ++arg;
                } else {
                    // Event arguments use the prefixed ContentId form, as Change Property does
                    // (the event reader 0x1055c289c passes the value reader 1, like 0x1055bd192).
                    ValueContext ctx = context;
                    ctx.prefixedContent = true;
                    writeValue(out, ptype, arg < e->arguments.size() ? e->arguments[arg] : Value{}, ctx);
                    ++arg;
                }
            }
        } else if (const auto* r = std::get_if<RuntimeItem>(&it)) {
            // The post-join items (Streaming.hpp): body first, so a body this can't write is skipped whole.
            NetworkStream body;
            if (writeRuntimeItem(body, *r, context, itemContext)) {
                type(runtimeItemType(*r));
                out.writeBytes(body.bytes());
            }
        } else {
            encodeJoinItem(out, it, schema, context, framing);
        }
    }
    type(item::End);
    return frame(std::move(out).take(), framing);
}

std::expected<std::vector<Item>, std::string> decodeData(std::span<const std::uint8_t> message, const Schema& schema,
                                                         const ValueContext& context, Node* tree,
                                                         const Framing& framing, const ItemContext& itemContext,
                                                         const std::function<void(Item&&)>& onItem)
{
    NetworkStream in(message);
    in.readU8(); // 0x83
    std::vector<Item> items;
    std::size_t handed = 0;
    // Each item read goes to onItem before the next is read, so the next's
    // refs see what it made.
    auto hand = [&] {
        if (!onItem)
            return;
        for (Item& item : items)
            onItem(std::move(item));
        handed += items.size();
        items.clear();
    };
    std::string error;
    // The Change Property items' context, made once: its hooks report into
    // the item being read and the key it lacked (copying a context's
    // functions for every item was a good part of reading one).
    ChangePropertyItem* changing = nullptr;
    std::optional<SharedStringKey> changeMissing;
    ValueContext changeContext = context;
    changeContext.prefixedContent = true;
    changeContext.sawRef = [&changing](InstanceId seen) { changing->forwardRef = seen; };
    changeContext.deferSharedString
        = [&changeMissing](const SharedStringKey& key, std::string_view) { changeMissing = key; };
    for (;;) {
        hand();
        const std::size_t at = in.offset();
        const std::uint8_t wire = in.readU8();
        if (in.bad()) {
            error = "no End item";
            break;
        }
        const std::uint8_t t = framing.remapIds ? decodeItemType(wire) : wire;
        Node* n = tree ? &tree->add(std::format("+{} item {:#04x} {}", at, t, itemTypeName(t))) : nullptr;
        if (t == item::End)
            break;
        if (t == item::New) {
            NewInstanceItem ni;
            ni.id = readInstanceId(in);
            ni.classId = in.readU16();
            if (ni.classId >= schema.classes.size()) {
                error = std::format("class network id {} out of range", ni.classId);
                break;
            }
            const SchemaClass& cls = schema.classes[ni.classId];
            if (n)
                n->text += std::format(": {} {}", cls.name, idText(ni.id));
            Node* p1 = n ? &n->add("properties") : nullptr;
            ValueContext pass = context;
            pass.prefixedContent = true;
            if (!readPropertyList(in, ni, false, cls, schema, pass, p1, error))
                break;
            Node* p2 = n ? &n->add("reference properties") : nullptr;
            pass.prefixedContent = false;
            if (!readPropertyList(in, ni, true, cls, schema, pass, p2, error))
                break;
            if (!ni.pendingSharedStrings.empty()
                && !readDeferredSharedStrings(in, ni.pendingSharedStrings, ni.properties, context, n, error))
                break;
            ni.parent = readInstanceId(in);
            if (n)
                n->add("parent: " + refText(ni.parent, context));
            items.emplace_back(std::move(ni));
        } else if (t == item::ChangeProperty) {
            ChangePropertyItem c;
            c.id = readInstanceId(in);
            c.propertyId = in.readU16();
            if (c.propertyId == schema.properties.size()) { // Parent
                c.flag = in.readU8() != 0;
                if (!itemContext.fromServer && c.flag)
                    c.flagValue = in.readZigzag32();
                c.parent = readInstanceId(in);
                if (n)
                    n->text += std::format(": {}.Parent = {}", refText(c.id, context), refText(*c.parent, context));
                items.emplace_back(std::move(c));
                continue;
            }
            if (c.propertyId >= schema.properties.size()) {
                error = std::format("Invalid property network id. Read offset={:X}", in.offset());
                break;
            }
            const SchemaProperty& p = schema.properties[c.propertyId];
            c.flag = in.readU8() != 0;
            if (!itemContext.fromServer && c.flag)
                c.flagValue = in.readZigzag32();
            changeContext.enumType = p.type == nettype::Enum ? enumFor(schema, p.enumId) : nullptr;
            changing = &c;
            changeMissing.reset();
            std::string text;
            auto v = readValue(in, p.type, changeContext, n ? &text : nullptr);
            const std::optional<SharedStringKey> missing = changeMissing;
            if (n)
                n->text += std::format(": {}.{} ({}) = {}{}{}", refText(c.id, context), p.name, netTypeName(p.type), text,
                                       c.forwardRef.null() ? "" : " (not known yet)", c.flag ? " [flag]" : "");
            if (!v) {
                error = std::format("can't read {}.{} as {} ({:#04x}) at +{}",
                                    p.owner < schema.classes.size() ? schema.classes[p.owner].name : "?", p.name,
                                    netTypeName(p.type), p.type, in.offset());
                break;
            }
            c.value = std::move(*v);
            if (missing) {
                std::vector<std::pair<std::uint16_t, SharedStringKey>> pending{{c.propertyId, *missing}};
                std::vector<std::pair<std::uint16_t, Value>> holder{{c.propertyId, c.value}};
                if (!readDeferredSharedStrings(in, pending, holder, context, n, error))
                    break;
                c.value = std::move(holder[0].second);
            }
            items.emplace_back(std::move(c));
        } else if (t == item::Delete) {
            DeleteItem d{readInstanceId(in)};
            if (n)
                n->text += ": " + refText(d.id, context);
            items.emplace_back(d);
        } else if (t == item::Marker) {
            MarkerItem m{in.readU32()};
            if (n)
                n->text += std::format(": marker {}", m.marker);
            items.emplace_back(m);
        } else if (t == item::EventInvocation) {
            EventInvocationItem e;
            e.id = readInstanceId(in);
            e.eventId = in.readU16();
            if (e.eventId >= schema.events.size()) {
                error = std::format("event network id {} out of range", e.eventId);
                break;
            }
            const SchemaEvent& ev = schema.events[e.eventId];
            if (n)
                n->text += std::format(": {}.{}", refText(e.id, context), ev.name);
            for (auto [ptype, enumId] : ev.params) {
                if (ptype == nettype::Tuple) {
                    const std::uint64_t count = in.readVarint();
                    for (std::uint64_t i = 0; i < count && !in.bad() && i < 4096; ++i) {
                        std::string text;
                        auto v = readRemoteVariant(in, context, n ? &text : nullptr);
                        if (n)
                            n->add(std::format("arg {}: {}", e.arguments.size() + 1, text));
                        if (v && v->table)
                            e.tables.emplace(e.arguments.size(), std::move(v->table));
                        if (v && v->random)
                            e.randoms.emplace(e.arguments.size(), *v->random);
                        e.arguments.push_back(v ? std::move(v->value) : Value{});
                    }
                } else if (isTableType(ptype)) {
                    std::string text;
                    auto v = readRemoteTable(in, ptype, context, n ? &text : nullptr);
                    if (n)
                        n->add(std::format("arg {} ({}): {}", e.arguments.size() + 1, netTypeName(ptype), text));
                    if (v && v->table)
                        e.tables.emplace(e.arguments.size(), std::move(v->table));
                    e.arguments.emplace_back();
                } else {
                    ValueContext ctx = context;
                    ctx.enumType = ptype == nettype::Enum ? enumFor(schema, enumId) : nullptr;
                    ctx.prefixedContent = true; // as the writer above
                    std::string text;
                    auto v = readValue(in, ptype, ctx, n ? &text : nullptr);
                    if (n)
                        n->add(std::format("arg {} ({}): {}", e.arguments.size() + 1, netTypeName(ptype), text));
                    e.arguments.push_back(v.value_or(Value{}));
                }
            }
            if (in.bad()) {
                error = "truncated event invocation";
                break;
            }
            items.emplace_back(std::move(e));
        } else if (decodeJoinItem(t, in, schema, context, n, error, items)) {
            continue;
        } else if (isRuntimeItemType(t)) {
            // The post-join items (Streaming.hpp).
            auto r = readRuntimeItem(in, t, context, itemContext, n, error);
            if (!r)
                break;
            items.emplace_back(std::move(*r));
        } else {
            if (error.empty())
                error = std::format("item type {:#04x} ({}) not decoded", t, itemTypeName(t));
            break;
        }
    }
    hand();
    if (!error.empty()) {
        if (tree) {
            Node& e = tree->add("stopped: " + error);
            const std::size_t at = std::min(in.offset(), message.size());
            addHex(e, message.subspan(at), at, 256);
        }
        if (items.empty() && handed == 0)
            return std::unexpected(error);
    }
    return items;
}

std::vector<RemoteValue> remoteArguments(const EventInvocationItem& item)
{
    std::vector<RemoteValue> out;
    out.reserve(item.arguments.size());
    for (std::size_t i = 0; i < item.arguments.size(); ++i) {
        if (auto t = item.tables.find(i); t != item.tables.end())
            out.push_back(RemoteValue{Value{}, t->second});
        else if (auto r = item.randoms.find(i); r != item.randoms.end())
            out.push_back(RemoteValue{.random = r->second});
        else
            out.push_back(remoteValue(item.arguments[i]));
    }
    return out;
}

EventInvocationItem eventInvocation(InstanceId id, std::uint16_t eventId, std::span<const RemoteValue> arguments)
{
    EventInvocationItem e{id, eventId, {}, {}};
    e.arguments.reserve(arguments.size());
    for (const RemoteValue& a : arguments) {
        if (a.table)
            e.tables.emplace(e.arguments.size(), a.table);
        if (a.random)
            e.randoms.emplace(e.arguments.size(), *a.random);
        e.arguments.push_back(a.table || a.random ? Value{} : a.value);
    }
    return e;
}

namespace {

std::uint64_t key(InstanceId id)
{
    return static_cast<std::uint64_t>(id.scope) << 32 | id.index;
}

// The DataModel has no wire id (the top containers hang off it); this key keeps it in the instance table.
constexpr std::uint64_t kRootKey = ~std::uint64_t{0};

// The server's Play and Stop reach a Sound as events, not as its Playing
// [live: a game's doors, drawers, lights, ambient sounds and NPCs, 1357 in
// one run]: playbackActionSync(action), then PlayingUpdatedFromServer(playing),
// and TimePositionUpdatedFromServer(at, GUESS: at again) when it moves it.
// Actions seen: 0 Play, 3 Stop. A Play starts the sound over, as Sound:Play() does.
void applySoundEvent(Instance& target, std::string_view event, std::span<const Value> arguments)
{
    auto* sound = dynamic_cast<Sound*>(&target);
    if (!sound || arguments.empty())
        return;
    // research: LUNAR_SOUND_TRACE (lunar::audio::SoundSystem) says who played or stopped it.
    if (static const bool tracing = std::getenv("LUNAR_SOUND_TRACE") != nullptr; tracing)
        std::println(stderr, "soundtrace: cause server-{}({}) {}", event, toString(arguments[0]),
                     static_cast<const void*>(sound));
    if (event == "playbackActionSync") {
        if (const auto* action = std::get_if<double>(&arguments[0]); action && *action == 0.0)
            sound->setTimePosition(0.0);
    } else if (event == "PlayingUpdatedFromServer") {
        if (const auto* playing = std::get_if<bool>(&arguments[0]))
            sound->setPlaying(*playing);
    } else if (event == "TimePositionUpdatedFromServer") {
        if (const auto* at = std::get_if<double>(&arguments[0]))
            sound->setTimePosition(std::max(*at, 0.0));
    }
}

} // namespace

Replica::Replica() : game_(DataModel::create()), motion_(std::make_shared<ReplicatedMotion>())
{
    // The client's own network service, which no server sends: a real
    // client's game has it with its ClientReplicator under it, and scripts
    // ask for it (Adonis's client watches its ChildRemoved for the
    // disconnect, and failed to load without it [live]).
    auto client = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("NetworkClient"));
    auto replicator = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("ClientReplicator"));
    replicator->setParent(client);
    client->setParent(game_);
}

double Replica::clockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Replica::stepMotion(double now)
{
    motion_->step(
        now, [this](InstanceId id) { return find(id); },
        [this](InstanceId id) {
            if (!simulatedHere)
                return false;
            const auto* part = dynamic_cast<const BasePart*>(find(id).get());
            return part && simulatedHere(*part);
        });
}

namespace {

std::string instanceText(const Replica& replica, InstanceId id)
{
    if (const InstancePtr inst = replica.find(id))
        return std::format("{} ({}:{})", inst->fullName(), id.scope, id.index);
    return std::format("{}:{} (not here)", id.scope, id.index);
}

} // namespace

// ID_TIMESTAMP + ID_PHYSICS (ROBLOX-LAYER.md 9.2). From the server, every
// frame of every part goes to the motion playback: the root's at the
// record's part, the other parts of the mechanism at their own refs. Their
// time is the packet's (milliseconds) less the frame's age.
bool Replica::handlePhysics(std::span<const std::uint8_t> message, Node* tree)
{
    auto packet = decodePhysicsPacket(message, decodingServerToClient, framing, tree);
    if (!packet)
        return false;
    if (!decodingServerToClient)
        return true;
    const double arrival = receiveTime.value_or(clockSeconds());
    const auto resolve = [this](InstanceId id) { return find(id); };
    for (const PhysicsRecord& record : packet->records) {
        if (!record.mechanism)
            continue;
        const auto& parts = record.mechanism->parts;
        for (std::size_t k = 0; k < parts.size(); ++k) {
            const InstanceId part = k == 0 ? record.part : parts[k].ref;
            if (!find(part)) {
                ++motionForUnknown;
                continue;
            }
            for (std::size_t f = 0; f < parts[k].frames.size(); ++f) {
                const PhysicsFrame& frame = parts[k].frames[f];
                PhysicsPV pv = frame.pv;
                // A non-root part's frame can be in another part's space
                // (basis index into the list, 0xff world). GUESS: the
                // same-numbered frame of that part.
                if (k != 0 && !record.mechanism->refBasis && frame.basis != 0xff && frame.basis < parts.size()
                    && f < parts[frame.basis].frames.size())
                    pv.cframe = parts[frame.basis].frames[f].pv.cframe * pv.cframe;
                motion_->receive(part, static_cast<double>(packet->timestamp) / 1000.0 - static_cast<double>(frame.age),
                                 arrival, pv);
            }
            if (!motion_->stepping())
                motion_->applyNewest(part, resolve);
        }
    }
    return true;
}

// ISR (Physics.hpp): each instance state's ReplicationPV goes to the motion
// playback, stamped with the packet's tick less its group's age. The other
// ISR properties (a Motor6D's replicated angles, an Animator's FACS data)
// are only described.
bool Replica::handleIsr(std::span<const std::uint8_t> message, Node* tree)
{
    if (tree && std::getenv("LUNAR_NET_ISR_HEX")) // research
        addHex(tree->add("message"), message);
    auto packet = decodeIsrPacket(message);
    if (!packet) {
        if (tree)
            tree->add(packet.error());
        return true;
    }
    if (decodingServerToClient)
        runtime.serverIsrTick = packet->tick;
    const double tickTime = packet->tick / kIsrTicksPerSecond;
    const double arrival = receiveTime.value_or(clockSeconds());
    const auto resolve = [this](InstanceId id) { return find(id); };
    if (tree)
        tree->add(std::format("tick {} ({:.3f} s), {} group(s), {}", packet->tick, tickTime, packet->groups.size(),
                              packet->clockOffset == kIsrNoClockOffset
                                  ? std::string("no clock offset")
                                  : std::format("clock offset {} (sender's tick - receiver's)", packet->clockOffset)));
    for (const IsrGroup& group : packet->groups) {
        const double sampleTime = (static_cast<double>(packet->tick) - group.age) / kIsrTicksPerSecond;
        Node* groupNode
            = tree ? &tree->add(std::format("group: kind {}, {} ticks old", group.kind, group.age)) : nullptr;
        for (const IsrInstanceState& state : group.instances) {
            const char* className
                = haveSchema_ && state.classId < schema_.classes.size() ? schema_.classes[state.classId].name.c_str()
                                                                       : "?";
            Node* stateNode = groupNode ? &groupNode->add(std::format("{} {}, dirty {:#x}", className,
                                                                      instanceText(*this, state.instance),
                                                                      state.dirty))
                                        : nullptr;
            if (!haveSchema_) {
                if (stateNode)
                    addHex(*stateNode, state.data);
                continue;
            }
            bool complete = false;
            for (const IsrProperty& property : isrProperties(state, schema_, &complete)) {
                const SchemaProperty& p = schema_.properties[*property.propertyId];
                if (p.type != nettype::ReplicationPV) {
                    if (stateNode)
                        addHex(stateNode->add(std::format("{} ({})", p.name, netTypeName(p.type))), property.bytes);
                    continue;
                }
                NetworkStream in(property.bytes);
                const PhysicsPV pv = readReplicationPV(in);
                if (stateNode)
                    stateNode->add(std::format("{}: {}", p.name, pvText(pv)));
                if (!find(state.instance)) {
                    ++motionForUnknown;
                    continue;
                }
                if (!decodingServerToClient && !motionFromClients)
                    continue;
                motion_->receive(state.instance, sampleTime, arrival, pv, group.age < 255);
                if (!motion_->stepping())
                    motion_->applyNewest(state.instance, resolve);
            }
            if (!complete && stateNode)
                addHex(stateNode->add("not read (type unknown or data left over)"), state.data);
            else if (stateNode && std::getenv("LUNAR_NET_ISR_HEX")) // research
                addHex(stateNode->add("data"), state.data);
        }
    }
    return true;
}

std::vector<std::pair<std::uint64_t, InstancePtr>> Replica::instances() const
{
    std::vector<std::pair<std::uint64_t, InstancePtr>> all(instances_.begin(), instances_.end());
    std::ranges::sort(all, {}, &std::pair<std::uint64_t, InstancePtr>::first);
    return all;
}

InstancePtr Replica::find(InstanceId id) const
{
    auto it = instances_.find(key(id));
    return it == instances_.end() ? nullptr : it->second;
}

void Replica::noteOwner(InstanceId id, std::string_view property, const Value& value)
{
    if (property != "NetworkOwnerV3")
        return;
    const double* peer = std::get_if<double>(&value);
    if (!peer)
        return;
    const auto owner = static_cast<std::uint32_t>(*peer);
    auto [it, added] = networkOwners_.try_emplace(key(id), owner);
    if (added || it->second != owner) {
        it->second = owner;
        ++ownershipChanges_;
    }
}

bool Replica::ownedHere(InstanceId id) const
{
    if (!globals || globals->peerId == 0)
        return false;
    const auto it = networkOwners_.find(key(id));
    return it != networkOwners_.end() && it->second == globals->peerId;
}

std::vector<InstanceId> Replica::partsOwnedHere() const
{
    std::vector<InstanceId> out;
    if (!globals || globals->peerId == 0)
        return out;
    for (const auto& [k, owner] : networkOwners_)
        if (owner == globals->peerId)
            out.push_back({static_cast<std::uint32_t>(k >> 32), static_cast<std::uint32_t>(k)});
    return out;
}

std::optional<InstanceId> Replica::idOf(const Instance* instance) const
{
    auto it = keys_.find(instance);
    if (it == keys_.end())
        return std::nullopt;
    return InstanceId{static_cast<std::uint32_t>(it->second >> 32), static_cast<std::uint32_t>(it->second)};
}

void Replica::bind(std::uint64_t k, const InstancePtr& instance)
{
    InstancePtr& slot = instances_[k];
    if (slot && slot != instance)
        if (auto old = keys_.find(slot.get()); old != keys_.end() && old->second == k)
            keys_.erase(old);
    slot = instance;
    if (k == kRootKey || !instance)
        return;
    // An instance with two ids (a service the globals and a New Instance item
    // both name) answers with the lower one.
    auto [it, fresh] = keys_.emplace(instance.get(), k);
    if (!fresh && k < it->second)
        it->second = k;
}

void Replica::unbind(std::uint64_t k)
{
    auto it = instances_.find(k);
    if (it == instances_.end())
        return;
    if (it->second)
        if (auto old = keys_.find(it->second.get()); old != keys_.end() && old->second == k)
            keys_.erase(old);
    instances_.erase(it);
}

ValueContext Replica::context() const
{
    ValueContext c;
    c.resolve = [this](InstanceId id) { return find(id); };
    c.idOf = [this](const InstancePtr& inst) { return idOf(inst.get()).value_or(InstanceId{}); };
    c.sharedStrings = &sharedStrings_;
    if (haveSchema_) {
        c.contentPrefixes = &schema_.contentPrefixes;
        c.stringDictionary = &schema_.stringDictionary;
        c.enumById = [this](std::uint16_t id) { return enumFor(schema_, id); };
        c.enumIdOf = [this](const EnumInfo& e) -> std::optional<std::uint16_t> {
            for (std::size_t i = 0; i < schema_.enums.size(); ++i)
                if (schema_.enums[i].name == e.name)
                    return static_cast<std::uint16_t>(i);
            return std::nullopt;
        };
    }
    return c;
}

std::optional<EventInvocationItem> Replica::eventItem(const Instance& instance, std::string_view event,
                                                      std::span<const RemoteValue> arguments) const
{
    const auto id = idOf(&instance);
    const auto cls = haveSchema_ ? schema_.classId(instance.className()) : std::nullopt;
    const auto eid = cls ? schema_.eventId(*cls, event) : std::nullopt;
    if (!id || !eid)
        return std::nullopt;
    return eventInvocation(*id, *eid, arguments);
}

std::optional<std::vector<std::uint8_t>> Replica::eventParameters(const Instance& instance,
                                                                  std::string_view event) const
{
    const auto cls = haveSchema_ ? schema_.classId(instance.className()) : std::nullopt;
    const auto eid = cls ? schema_.eventId(*cls, event) : std::nullopt;
    if (!eid)
        return std::nullopt;
    std::vector<std::uint8_t> types;
    for (const auto& [type, enumId] : schema_.events[*eid].params)
        types.push_back(type);
    return types;
}

void Replica::applyGlobals(const SetGlobals& g)
{
    globals = g;
    bind(kRootKey, game_);
    for (const TopContainer& c : g.containers) {
        if (c.classId >= schema_.classes.size() || c.id.null())
            continue;
        const std::string& className = schema_.classes[c.classId].name;
        InstancePtr inst;
        try {
            if (Instance* service = game_->getService(className))
                inst = service->shared_from_this();
        } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a service the engine doesn't have
        }
        if (!inst) {
            // A container the engine has no class for keeps its subtree under a placeholder.
            if (Instance* existing = game_->findFirstChildOfClass(className))
                inst = existing->shared_from_this();
            else {
                inst = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
                inst->setParent(game_);
            }
        }
        bind(key(c.id), inst);
        if (auto it = pendingRefs_.find(key(c.id)); it != pendingRefs_.end()) {
            for (const auto& [weak, prop] : it->second)
                if (InstancePtr holder = weak.lock()) {
                    try {
                        setNetworkProperty(*holder, prop, Value(inst));
                    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a value the engine refuses
                    }
                }
            pendingRefs_.erase(it);
        }
    }
}

bool Replica::handle(std::span<const std::uint8_t> message, Node* tree)
{
    if (message.empty())
        return false;
    struct Applying {
        int& depth;
        explicit Applying(int& d) : depth(++d) {}
        ~Applying() { --depth; }
        Applying(const Applying&) = delete;
        Applying& operator=(const Applying&) = delete;
    } applying(applying_);
    if (message[0] == kSchemaPacketId) {
        auto s = decodeSchemaPacket(message);
        if (!s) {
            if (tree)
                tree->add(s.error());
            return false;
        }
        schema_ = std::move(*s);
        haveSchema_ = true;
        if (std::getenv("LUNAR_NET_SCHEMA")) { // research: the enums, every class's properties, events and their parameters
            for (std::size_t i = 0; i < schema_.enums.size(); ++i)
                std::println(stderr, "schema enum {}: {} (size {})", i, schema_.enums[i].name, schema_.enums[i].size);
            for (const SchemaClass& c : schema_.classes)
                for (std::uint16_t k = 0; k < c.eventCount; ++k) {
                    const SchemaEvent& e = schema_.events[c.firstEvent + k];
                    std::string params;
                    for (auto [type, enumId] : e.params)
                        params += std::format("{}{}", params.empty() ? "" : ", ", netTypeName(type));
                    std::println(stderr, "schema event {}: {}.{}({})", c.firstEvent + k, c.name, e.name, params);
                }
            for (const SchemaClass& c : schema_.classes)
                for (std::uint16_t k = 0; k < c.propertyCount; ++k) {
                    const SchemaProperty& p = schema_.properties[c.firstProperty + k];
                    std::println(stderr, "schema property {}: {}.{} {}", c.firstProperty + k, c.name, p.name,
                                 netTypeName(p.type));
                }
            for (std::size_t i = 0; i < schema_.classes.size(); ++i)
                std::println(stderr, "schema class {}: {}", i, schema_.classes[i].name);
            for (const Schema::IsrClass& c : schema_.isrClasses) {
                std::string props;
                for (std::uint32_t p : c.properties)
                    props += std::format("{}{} {} ({})", props.empty() ? "" : ", ", p,
                                         p < schema_.properties.size() ? schema_.properties[p].name : "?",
                                         p < schema_.properties.size() ? netTypeName(schema_.properties[p].type) : "?");
                std::println(stderr, "schema ISR class {} {}: {}", c.classId,
                             c.classId < schema_.classes.size() ? schema_.classes[c.classId].name : "?", props);
            }
        }
        if (tree) {
            tree->add(std::format("network schema: {} enums, {} classes, {} properties, {} events",
                                  schema_.enums.size(), schema_.classes.size(), schema_.properties.size(),
                                  schema_.events.size()));
            Node& cl = tree->add("classes (first 12)");
            for (std::size_t i = 0; i < std::min<std::size_t>(schema_.classes.size(), 12); ++i) {
                const SchemaClass& c = schema_.classes[i];
                Node& cn = cl.add(std::format("[{}] {}: property ids {}..{}, {} event(s)", i, c.name, c.firstProperty,
                                              c.firstProperty + c.propertyCount, c.eventCount));
                for (std::uint16_t k = 0; k < std::min<std::uint16_t>(c.propertyCount, 4); ++k) {
                    const SchemaProperty& p = schema_.properties[c.firstProperty + k];
                    cn.add(std::format("{} {}", netTypeName(p.type), p.name));
                }
            }
            tree->add(std::format("trailing sections: {} content prefixes, {} dictionary strings, {} ISR classes, "
                                  "{} lr type ranges",
                                  schema_.contentPrefixes.size() - 1, schema_.stringDictionary.size(),
                                  schema_.isrClasses.size(), schema_.lrTypeRanges.size()));
            if (schema_.contentPrefixes.size() > 1) {
                Node& pn = tree->add("content prefixes");
                for (std::size_t i = 1; i < std::min<std::size_t>(schema_.contentPrefixes.size(), 24); ++i)
                    pn.add(std::format("[{}] \"{}\"", i, schema_.contentPrefixes[i]));
            }
        }
        return true;
    }
    if (message[0] == msg::SetGlobals) {
        auto g = decodeSetGlobals(message, capabilities, tree, haveSchema_ ? &schema_ : nullptr);
        if (!g) {
            if (tree)
                tree->add(g.error());
            return false;
        }
        if (haveSchema_)
            applyGlobals(*g);
        else if (tree)
            tree->add("ID_SET_GLOBALS before any schema: the top containers can't be named");
        return true;
    }
    if (message[0] == 0x93 && message.size() >= 2) {
        // ClientReplicator::vfn_89 skips the id and one byte, then vfn_175
        // (0x1054d8eb4) reads u16 count and count x { u16-length string key,
        // u16-length string value } into the map at rep+0x30d0 (the set the
        // join request's optional strings go to) [code]. Meaning GUESS.
        NetworkStream in(message);
        in.readU8();
        const std::uint8_t flags = in.readU8();
        if (tree) {
            tree->add(std::format("byte 1: {:#04x} (skipped by the client)", flags));
            if (in.left() >= 2) {
                const std::uint16_t count = in.readU16();
                Node& pairs = tree->add(std::format("string pairs ({}; meaning GUESS)", count));
                for (std::uint16_t i = 0; i < count && !in.bad(); ++i) {
                    const std::uint16_t klen = in.readU16();
                    const auto k = in.readBytes(klen);
                    const std::uint16_t vlen = in.readU16();
                    const auto v = in.readBytes(vlen);
                    pairs.add(std::format("\"{}\" = \"{}\"", std::string(k.begin(), k.end()),
                                          std::string(v.begin(), v.end())));
                }
            }
            if (in.left() > 0)
                addHex(tree->add(std::format("unparsed tail at +{}", in.offset())), message.subspan(in.offset()),
                       in.offset(), 256);
        }
        return true;
    }
    if (message[0] == kJoinRequestId) {
        auto r = decodeJoinRequest(message, capabilities);
        if (!r)
            return false;
        if (tree) {
            tree->add(std::format("joinFlags: {}", r->flags));
            tree->add(std::format("clientValue: {} (GUESS: protocol version)", r->clientValue));
            tree->add(std::format("luau versions: {}..{}", r->luauMin, r->luauMax));
            for (const std::string& e : r->extra)
                tree->add(std::format("extra: \"{}\"", e));
            tree->add(std::format("joinInfo: {}", r->joinInfo));
        }
        return true;
    }
    if (message[0] == msg::Timestamp)
        return handlePhysics(message, tree);
    if (message[0] == msg::Isr)
        return handleIsr(message, tree);
    if (handleSessionMessage(*this, message, tree)) // kick, chat, cluster, challenges ... (Session.hpp)
        return true;
    if (message[0] == kPlaceIdVerifiedId && message.size() >= 2) {
        if (message[1] != 0)
            ++runtime.placeIdVerifications;
        if (tree)
            tree->add(std::format("place id verified: {}", message[1] != 0));
        return true;
    }
    if (message[0] == kMarkerId && message.size() >= 5) {
        NetworkStream in(message);
        in.readU8();
        const std::uint32_t m = in.readU32();
        markers.push_back(m);
        if (tree)
            tree->add(std::format("marker: {}", m));
        return true;
    }
    if (message[0] != 0x83)
        return false;
    // Before the schema only the schema-free items (pings, markers ...) can be read.
    static const Schema kNoSchema;
    Node* items = tree ? &tree->add(haveSchema_ ? "items" : "items (no schema yet: schema-free items only)") : nullptr;
    // Each item is applied as it's read: an event's argument can name an
    // instance a New item earlier in the same message made [capture: the game's
    // grab effect names the RootAnchor accessory made 316 bytes
    // before it, which read as nil when the whole message was read first].
    auto decoded = decodeData(message, haveSchema_ ? schema_ : kNoSchema, context(), items, framing,
                              ItemContext{decodingServerToClient, capabilities},
                              [this](Item&& item) { apply(std::move(item)); });
    if (!decoded) {
        ++decodeFailures;
        if (decodeErrors.size() < 8)
            decodeErrors.push_back(decoded.error());
        if (std::getenv("LUNAR_NET_DUMPFAIL") && decodeErrors.size() <= 3) { // research
            Node dump("failed ID_DATA");
            (void)decodeData(message, haveSchema_ ? schema_ : kNoSchema, context(), &dump, framing,
                             ItemContext{decodingServerToClient, capabilities});
            addHex(dump.add("bytes"), message, 0, 160);
            std::println(stderr, "{}", render(dump));
        }
        return true;
    }
    // Contents that arrived for properties still waiting on their key.
    for (auto it = pendingStrings_.begin(); it != pendingStrings_.end();) {
        const std::string* content = sharedStrings_.find(it->first);
        if (!content) {
            ++it;
            continue;
        }
        for (const auto& [weak, prop] : it->second)
            if (InstancePtr holder = weak.lock()) {
                try {
                    setNetworkProperty(*holder, prop, Value(*content));
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a value the engine refuses
                }
            }
        it = pendingStrings_.erase(it);
    }
    return true;
}

void Replica::apply(Item item)
{
    if (auto* n = std::get_if<NewInstanceItem>(&item)) {
        const std::string& className = schema_.classes[n->classId].name;
        InstancePtr parent = n->parent.null() ? nullptr : find(n->parent);
        // An id ID_SET_GLOBALS already named (the services) keeps its
        // instance: the server also sends those as New Instance items, with
        // their properties and no parent [live].
        InstancePtr inst = find(n->id);
        bool fresh = false; // made here, not one we had (asking idOf made a join quadratic)
        if (inst) {
        } else if (className == "DataModel") {
            inst = game_;
        } else if (parent.get() == game_.get()) {
            if (Instance* service = game_->getService(className))
                inst = service->shared_from_this();
        }
        if (!inst) {
            if (const ClassInfo* info = ClassRegistry::find(className)) {
                // Engine-made classes (Terrain, Camera ...): reuse the one the
                // parent already has, else make it with the class's factory.
                if (info->kind != ClassKind::Creatable && parent)
                    if (Instance* existing = parent->findFirstChildOfClass(className); existing && !idOf(existing))
                        inst = existing->shared_from_this();
                if (!inst && info->factory) {
                    inst = info->factory();
                    fresh = true;
                }
            }
        }
        if (!inst) {
            inst = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
        } else if (fresh) {
            // A fresh part starts from Roblox's class defaults, which the server
            // leaves off the wire: studs on top, inlets below (a server sends
            // Smooth explicitly; the genuine client shows the omitted ones studded).
            if (auto* part = dynamic_cast<BasePart*>(inst.get())) {
                part->setTopSurface(Enum::SurfaceType::Studs);
                part->setBottomSurface(Enum::SurfaceType::Inlet);
            }
        }
        bind(key(n->id), inst);
        auto set = [&](const std::vector<std::pair<std::uint16_t, Value>>& props) {
            for (const auto& [id, v] : props) {
                noteOwner(n->id, schema_.properties[id].name, v);
                try {
                    setNetworkProperty(*inst, schema_.properties[id].name, v);
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a value the engine refuses keeps its
                                                  // default
                }
            }
        };
        set(n->properties);
        set(n->refProperties);
        for (const auto& [pid, key] : n->pendingSharedStrings) {
            if (const std::string* content = sharedStrings_.find(key)) {
                try {
                    setNetworkProperty(*inst, schema_.properties[pid].name, Value(*content));
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): as above
                }
            } else {
                pendingStrings_[key].emplace_back(inst, schema_.properties[pid].name);
            }
        }
        for (const auto& [pid, target] : n->forwardRefs) {
            if (InstancePtr known = find(target)) { // earlier in the same message
                try {
                    setNetworkProperty(*inst, schema_.properties[pid].name, Value(known));
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): as above
                }
            } else {
                pendingRefs_[key(target)].emplace_back(inst, schema_.properties[pid].name);
            }
        }
        // A locked or circular parent leaves it where it is (as for a Parent
        // change below): one instance mustn't end the session.
        try {
            if (parent && inst != game_ && inst->parent() != parent.get())
                inst->setParent(parent);
            else if (!parent && !n->parent.null() && inst != game_)
                pendingParents_[key(n->parent)].push_back(inst); // parented when it arrives
        } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
        }
        // Children that arrived before this one, unless they were destroyed
        // while they waited (their Parent is locked then) [live: the game].
        if (auto it = pendingParents_.find(key(n->id)); it != pendingParents_.end()) {
            for (const auto& weak : it->second)
                if (InstancePtr child = weak.lock(); child && child->parent() == nullptr && !child->destroyed()) {
                    try {
                        child->setParent(inst);
                    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
                    }
                }
            pendingParents_.erase(it);
        }
        // Whoever was waiting for this one.
        if (auto it = pendingRefs_.find(key(n->id)); it != pendingRefs_.end()) {
            for (const auto& [weak, prop] : it->second)
                if (InstancePtr holder = weak.lock()) {
                    try {
                        setNetworkProperty(*holder, prop, Value(inst));
                    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): as above
                    }
                }
            pendingRefs_.erase(it);
        }
    } else if (auto* reparent = std::get_if<ChangePropertyItem>(&item); reparent && reparent->parent) {
        if (InstancePtr inst = find(reparent->id); inst && inst != game_) {
            try {
                if (reparent->parent->null()) {
                    inst->setParent(nullptr);
                } else if (InstancePtr parent = find(*reparent->parent)) {
                    if (inst->parent() != parent.get())
                        inst->setParent(parent);
                } else {
                    inst->setParent(nullptr);
                    pendingParents_[key(*reparent->parent)].push_back(inst); // parented when it arrives
                }
            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a locked or circular parent stays put
            }
        }
    } else if (auto* c = std::get_if<ChangePropertyItem>(&item)) {
        if (InstancePtr inst = find(c->id)) {
            const std::string& name = schema_.properties[c->propertyId].name;
            noteOwner(c->id, name, c->value);
            InstancePtr target = c->forwardRef.null() ? nullptr : find(c->forwardRef);
            if (!c->forwardRef.null() && !target) {
                pendingRefs_[key(c->forwardRef)].emplace_back(inst, name); // set when it arrives
            } else {
                try {
                    setNetworkProperty(*inst, name, target ? Value(target) : c->value);
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): as above
                }
            }
            if (name == "CFrame" && cframeChanges.size() < 4096)
                cframeChanges.push_back({c->id, c->propertyId, c->flag});
        }
    } else if (auto* d = std::get_if<DeleteItem>(&item)) {
        // Children still waiting for this one go with it.
        if (auto it = pendingParents_.find(key(d->id)); it != pendingParents_.end()) {
            for (const auto& weak : it->second)
                if (InstancePtr child = weak.lock(); child && child->parent() == nullptr)
                    child->destroy();
            pendingParents_.erase(it);
        }
        if (InstancePtr inst = find(d->id)) {
            inst->destroy();
            unbind(key(d->id));
        }
        if (networkOwners_.erase(key(d->id)))
            ++ownershipChanges_;
    } else if (auto* m = std::get_if<MarkerItem>(&item)) {
        markers.push_back(m->marker);
    } else if (auto* e = std::get_if<EventInvocationItem>(&item)) {
        if (InstancePtr inst = find(e->id)) {
            const std::string& name = schema_.events[e->eventId].name;
            applySoundEvent(*inst, name, e->arguments);
            if (!takeEvent || !takeEvent(inst, name, *e))
                inst->fireEvent(name, e->arguments);
        }
        events.push_back(std::move(*e));
    } else if (auto* v2 = std::get_if<JoinDataV2Item>(&item)) {
        // Shared strings and NetAssets were taken into the cache while decoding; the instances are New items.
        for (NewInstanceItem& ni : v2->instances)
            apply(Item(std::move(ni)));
    } else if (auto* v1 = std::get_if<JoinDataItem>(&item)) {
        for (NewInstanceItem& ni : v1->instances)
            apply(Item(std::move(ni)));
    } else if (auto* t = std::get_if<TagItem>(&item)) {
        tags.push_back(t->tag);
        if (t->tag == kTagJoinDataDone)
            joinDataDone = true;
    } else if (auto* s = std::get_if<ServerTimeItem>(&item)) {
        serverTime = s->time;
    } else if (const auto* tree = std::get_if<ResolveAtomicTreeItem>(&item)) {
        // An atomic tree (sent whole, its root without a parent) put in its
        // place: the root under the other instance [live: Adonis's client, a
        // ScreenGui holding its loader, resolved into our PlayerGui; left
        // out, nothing answered Adonis and it kicked us after 300 s].
        if (InstancePtr root = find(tree->instance); root && root != game_) {
            try {
                if (InstancePtr parent = find(tree->other)) {
                    if (root->parent() != parent.get())
                        root->setParent(parent);
                } else {
                    pendingParents_[key(tree->other)].push_back(root); // parented when it arrives
                }
            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a locked or circular parent stays put
            }
        }
    } else if (auto* a = std::get_if<ChangeAttributeItem>(&item)) {
        if (InstancePtr inst = find(a->id)) {
            try {
                inst->setAttribute(a->name, a->value);
            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): an attribute the engine refuses
            }
        }
    } else if (const auto* r = std::get_if<RuntimeItem>(&item)) {
        // Streamed instances: the blob is a join-data body.
        const StreamDataItem* stream = std::get_if<StreamDataItem>(&r->body);
        const auto* pin = std::get_if<StreamPinDataItem>(&r->body);
        if (pin)
            stream = &pin->data;
        if (stream) {
            // Recorded for the client's streaming bookkeeping (StreamingClient),
            // empty items too: their regions take part in the delta codes.
            StreamedBatch batch;
            batch.regions = stream->regions;
            batch.pinned = pin != nullptr;
            if (stream->count > 0 && haveSchema_) {
                std::string error;
                auto streamed = readJoinInstances(stream->blob, stream->count, schema_, context(), error);
                if (std::getenv("LUNAR_NET_STREAM_DEBUG"))
                    std::println(stderr, "stream data: {} of {} instances{}", streamed.size(), stream->count,
                                 error.empty() ? "" : ", stopped: " + error);
                batch.instances.reserve(streamed.size());
                for (NewInstanceItem& ni : streamed) {
                    batch.instances.push_back(ni.id);
                    apply(Item(std::move(ni)));
                }
            }
            if (decodingServerToClient)
                runtime.streamedBatches.push_back(std::move(batch));
        }
        applyRuntimeItem(*this, *r); // Session.cpp
    }
    // Change Inst is recorded by the describer only; the engine has no
    // concept for it.
}

std::optional<InstanceId> Replica::addLocal(const InstancePtr& instance)
{
    if (!instance || !globals || globals->peerId == 0)
        return std::nullopt;
    if (auto known = idOf(instance.get()))
        return known;
    InstanceId id{globals->peerId, nextLocalIndex_++};
    while (instances_.contains(key(id)))
        id.index = nextLocalIndex_++;
    instances_[key(id)] = instance;
    return id;
}

void Replica::forgetLocal(InstanceId id)
{
    instances_.erase(key(id));
}

void Replica::remove(InstanceId id)
{
    auto it = instances_.find(key(id));
    if (it == instances_.end())
        return;
    if (it->second && it->second != game_)
        it->second->destroy();
    unbind(key(id));
    if (networkOwners_.erase(key(id)))
        ++ownershipChanges_;
}

ReplicationServer::ReplicationServer(std::shared_ptr<DataModel> game, const std::vector<std::string>& services)
    : game_(std::move(game)), schema_(Schema::fromEngine())
{
    idOf(game_.get()); // index 1: the DataModel
    track(game_);
    for (const std::string& name : services)
        if (Instance* service = game_->findService(name)) {
            InstancePtr s = service->shared_from_this();
            track(s);
            services_.push_back(s);
            for (const InstancePtr& d : s->descendants())
                track(d);
        }
}

ReplicationServer::~ReplicationServer()
{
    for (lunar::Connection& c : connections_)
        c.disconnect();
}

InstanceId ReplicationServer::idOf(const Instance* instance)
{
    if (!instance)
        return {};
    auto [it, fresh] = ids_.emplace(instance, static_cast<std::uint32_t>(ids_.size() + 1));
    if (fresh)
        byIndex_[it->second] = std::const_pointer_cast<Instance>(instance->shared_from_this());
    // GUESS: scope = the server's peer id; Lunarstone's server is peer 1.
    return {1, it->second};
}

InstancePtr ReplicationServer::find(InstanceId id) const
{
    auto it = byIndex_.find(id.index);
    return it == byIndex_.end() ? nullptr : it->second.lock();
}

ValueContext ReplicationServer::context()
{
    ValueContext c;
    c.idOf = [this](const InstancePtr& inst) {
        // Only instances the client will know about.
        return ids_.contains(inst.get()) ? idOf(inst.get()) : InstanceId{};
    };
    c.resolve = [this](InstanceId id) { return find(id); };
    c.sharedStrings = &sharedStrings_;
    c.contentPrefixes = &schema_.contentPrefixes;
    c.stringDictionary = &schema_.stringDictionary;
    return c;
}

void ReplicationServer::track(const InstancePtr& instance)
{
    idOf(instance.get());
    order_.push_back(instance);
    const Instance* raw = instance.get();
    connections_.push_back(instance->changed.connect([this, raw](std::string_view property) {
        if (property != "Parent")
            dirty_.emplace_back(raw, std::string(property));
    }));
    connections_.push_back(instance->attributeChanged.connect(
        [this, raw](std::string_view attribute) { dirtyAttributes_.emplace_back(raw, std::string(attribute)); }));
}

NewInstanceItem ReplicationServer::newItem(const Instance& instance)
{
    NewInstanceItem n;
    n.id = idOf(&instance);
    n.classId = schema_.classId(instance.className()).value_or(schema_.classId("Folder").value_or(0));
    const SchemaClass& cls = schema_.classes[n.classId];
    for (std::uint16_t i = 0; i < cls.propertyCount; ++i) {
        const SchemaProperty& p = schema_.properties[cls.firstProperty + i];
        Value v;
        try {
            v = instance.get(p.name);
        } catch (const std::exception&) {
            continue;
        }
        if (p.type == nettype::Object) {
            const auto* ref = std::get_if<InstancePtr>(&v);
            if (ref && *ref && ids_.contains(ref->get()))
                n.refProperties.emplace_back(cls.firstProperty + i, v);
        } else {
            n.properties.emplace_back(cls.firstProperty + i, std::move(v));
        }
    }
    n.parent = instance.parent() ? idOf(instance.parent()) : InstanceId{};
    return n;
}

SetGlobals ReplicationServer::globals() const
{
    SetGlobals g;
    g.notCloudEdit = true;
    g.peerId = 2; // GUESS: the server is peer 1 (its ids' scope), the first client 2
    for (const InstancePtr& s : services_) {
        TopContainer c;
        auto it = ids_.find(s.get());
        c.id = {1, it == ids_.end() ? 0u : it->second};
        c.classId = schema_.classId(s->className()).value_or(0);
        g.containers.push_back(c);
    }
    return g;
}

std::vector<std::vector<std::uint8_t>> ReplicationServer::joinMessages(std::size_t itemBytes, JoinStyle style)
{
    std::vector<std::vector<std::uint8_t>> out;
    out.push_back(encodeSchemaPacket(schema_));
    const ValueContext ctx = context();
    if (style == JoinStyle::NewItems) {
        std::vector<Item> batch;
        std::size_t estimate = 0;
        auto flush = [&] {
            if (!batch.empty())
                out.push_back(encodeData(batch, schema_, ctx));
            batch.clear();
            estimate = 0;
        };
        for (const InstancePtr& inst : order_) {
            if (inst.get() == game_.get()) {
                NewInstanceItem root;
                root.id = idOf(game_.get());
                root.classId = schema_.classId("DataModel").value_or(0);
                batch.emplace_back(std::move(root));
                continue;
            }
            NewInstanceItem n = newItem(*inst);
            estimate += 16 + n.properties.size() * 12;
            batch.emplace_back(std::move(n));
            if (estimate >= itemBytes)
                flush();
        }
        flush();
    } else {
        // ID_SET_GLOBALS names the services; the join data carries their
        // descendants (parents first, as order_ is), shared strings ahead.
        out.push_back(encodeSetGlobals(globals(), kBaseCapabilities));
        std::vector<NewInstanceItem> instances;
        std::set<const Instance*> containers;
        for (const InstancePtr& s : services_)
            containers.insert(s.get());
        for (const InstancePtr& inst : order_)
            if (inst.get() != game_.get() && !containers.contains(inst.get()))
                instances.push_back(newItem(*inst));
        // The contents of the keyed string properties, once, before the instances that use them.
        JoinDataV2Item strings;
        strings.mode = 1;
        for (const NewInstanceItem& n : instances) {
            for (const auto& [id, v] : n.properties) {
                const std::uint8_t type = schema_.properties[id].type;
                const auto* content = std::get_if<std::string>(&v);
                if (!content || !(type == nettype::ProtectedString || type == nettype::ProtectedStringBytecode
                                  || type == nettype::BinaryString))
                    continue;
                const SharedStringKey key = sharedStringKey(*content);
                if (!sharedStrings_.contains(key)) {
                    sharedStrings_.add(*content);
                    strings.sharedStrings.push_back({key, 0, *content});
                }
            }
        }
        if (!strings.sharedStrings.empty()) {
            const std::array<Item, 1> one{std::move(strings)};
            out.push_back(encodeData(one, schema_, ctx));
        }
        JoinDataV2Item batch;
        std::size_t estimate = 0;
        auto flush = [&] {
            if (!batch.instances.empty()) {
                const std::array<Item, 1> one{std::move(batch)};
                out.push_back(encodeData(one, schema_, ctx));
            }
            batch = JoinDataV2Item{};
            estimate = 0;
        };
        for (NewInstanceItem& n : instances) {
            estimate += 16 + n.properties.size() * 12;
            batch.instances.push_back(std::move(n));
            if (estimate >= itemBytes)
                flush();
        }
        flush();
        const std::array<Item, 1> tag{TagItem{kTagJoinDataDone}};
        out.push_back(encodeData(tag, schema_, ctx));
    }
    // The join's end: a marker the client sees once everything before it is in.
    const std::array<Item, 1> marker{MarkerItem{1}};
    out.push_back(encodeData(marker, schema_, ctx));
    dirty_.clear();
    dirtyAttributes_.clear();
    return out;
}

std::vector<std::vector<std::uint8_t>> ReplicationServer::takeChanges()
{
    std::vector<std::vector<std::uint8_t>> out;
    if (dirty_.empty() && dirtyAttributes_.empty())
        return out;
    std::vector<Item> batch;
    for (const auto& [raw, attribute] : dirtyAttributes_) {
        auto it = ids_.find(raw);
        if (it == ids_.end())
            continue;
        InstancePtr inst = find({1, it->second});
        if (!inst)
            continue;
        batch.emplace_back(ChangeAttributeItem{{1, it->second}, attribute, inst->getAttribute(attribute)});
    }
    dirtyAttributes_.clear();
    for (const auto& [raw, property] : dirty_) {
        auto it = ids_.find(raw);
        if (it == ids_.end())
            continue;
        InstancePtr inst = find({1, it->second});
        if (!inst)
            continue;
        const auto cls = schema_.classId(inst->className());
        const auto pid = cls ? schema_.propertyId(*cls, property) : std::nullopt;
        if (!pid)
            continue;
        Value v;
        try {
            v = inst->get(property);
        } catch (const std::exception&) {
            continue;
        }
        batch.emplace_back(ChangePropertyItem{{1, it->second}, *pid, std::move(v)});
    }
    dirty_.clear();
    if (!batch.empty())
        out.push_back(encodeData(batch, schema_, context()));
    return out;
}

std::vector<std::uint8_t> ReplicationServer::addInstance(const InstancePtr& instance)
{
    if (!instance || ids_.contains(instance.get()))
        return {};
    track(instance);
    const std::array<Item, 1> items{newItem(*instance)};
    return encodeData(items, schema_, context());
}

std::vector<std::uint8_t> ReplicationServer::streamData(std::span<const InstancePtr> instances,
                                                        std::vector<StreamRegionRef> regions)
{
    std::vector<NewInstanceItem> items;
    for (const InstancePtr& instance : instances) {
        if (!instance)
            continue;
        if (!ids_.contains(instance.get()))
            track(instance);
        items.push_back(newItem(*instance));
    }
    StreamDataItem stream;
    stream.count = static_cast<std::uint32_t>(items.size());
    if (!items.empty()) {
        auto body = writeJoinInstances(items, schema_, context());
        if (!body)
            return {};
        stream.blob = std::move(*body);
    }
    stream.regions = std::move(regions);
    const std::array<Item, 1> wrapped{RuntimeItem{std::move(stream)}};
    return encodeData(wrapped, schema_, context());
}

std::vector<std::uint8_t> ReplicationServer::invoke(const Instance& instance, std::string_view event,
                                                    std::vector<Value> arguments)
{
    const auto cls = schema_.classId(instance.className());
    const auto eid = cls ? schema_.eventId(*cls, event) : std::nullopt;
    if (!eid)
        return {};
    const std::array<Item, 1> items{EventInvocationItem{idOf(&instance), *eid, std::move(arguments)}};
    return encodeData(items, schema_, context());
}

MessageDescriber robloxDescriber(const std::shared_ptr<Replica>& replica)
{
    return [replica](std::span<const std::uint8_t> payload, Node& into) { return replica->handle(payload, &into); };
}

} // namespace lunar::net
