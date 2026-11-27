#include <lunar/Rbxl.hpp>

#include "BinaryFormat.hpp"
#include "Builder.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Lz4.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <zstd.h>

namespace lunar::rbxl {

namespace {

struct Failure {
    std::string message;
};

[[noreturn]] void fail(std::string message)
{
    throw Failure{std::move(message)};
}

constexpr std::string_view magic = "<roblox!";
constexpr std::string_view signature = "\x89\xff\r\n\x1a\n";
constexpr std::string_view zstdMagic = "\x28\xb5\x2f\xfd";

// Binary property type ids we can decode.
enum : std::uint8_t {
    typeString = 0x01,
    typeBool = 0x02,
    typeInt32 = 0x03,
    typeFloat32 = 0x04,
    typeFloat64 = 0x05,
    typeUDim = 0x06,
    typeUDim2 = 0x07,
    typeFaces = 0x09,
    typeAxes = 0x0A,
    typeBrickColor = 0x0B,
    typeColor3 = 0x0C,
    typeVector2 = 0x0D,
    typeVector3 = 0x0E,
    typeCFrame = 0x10,
    typeEnum = 0x12,
    typeReferent = 0x13,
    typeVector3int16 = 0x14,
    typeNumberSequence = 0x15,
    typeColorSequence = 0x16,
    typeNumberRange = 0x17,
    typeRect = 0x18,
    typePhysicalProperties = 0x19,
    typeColor3uint8 = 0x1A,
    typeInt64 = 0x1B,
    typeSharedString = 0x1C,
    // A script's Source compiled: Luau bytecode behind a header, with the
    // opcodes scrambled (the client's own models: UniversalApp.rbxm).
    typeSourceBytecode = 0x1D,
    typeOptionalCFrame = 0x1E,
    typeUniqueId = 0x1F,
    typeFont = 0x20,
    typeSecurityCapabilities = 0x21,
    typeContent = 0x22,
};

class Reader {
public:
    Reader(std::string_view data, std::string_view context) : data_(data), context_(context) {}

    bool done() const { return pos_ >= data_.size(); }
    std::size_t remaining() const { return data_.size() - pos_; }

    std::string_view bytes(std::size_t count)
    {
        if (count > remaining())
            fail(std::format("{}: unexpected end of data", context_));
        std::string_view out = data_.substr(pos_, count);
        pos_ += count;
        return out;
    }

    // Little-endian scalar.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    T read()
    {
        std::array<char, sizeof(T)> raw{};
        bytes(sizeof(T)).copy(raw.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
            std::ranges::reverse(raw);
        return std::bit_cast<T>(raw);
    }

    std::uint8_t byte() { return read<std::uint8_t>(); }

    std::string string() { return std::string(bytes(read<std::uint32_t>())); }

    // count big-endian values stored one byte plane at a time: the first byte
    // of every value, then the second byte of every value, and so on.
    template <std::unsigned_integral U>
    std::vector<U> interleaved(std::size_t count)
    {
        if (count > remaining() / sizeof(U))
            fail(std::format("{}: unexpected end of data", context_));
        std::string_view raw = bytes(count * sizeof(U));
        std::vector<U> values(count, 0);
        for (std::size_t plane = 0; plane < sizeof(U); ++plane)
            for (std::size_t i = 0; i < count; ++i)
                values[i] = static_cast<U>((values[i] << 8) | static_cast<std::uint8_t>(raw[plane * count + i]));
        return values;
    }

private:
    std::string_view data_;
    std::string_view context_;
    std::size_t pos_ = 0;
};

// Integers are zigzag encoded so small negatives stay small.
constexpr std::int32_t untransform(std::uint32_t v)
{
    return std::bit_cast<std::int32_t>((v >> 1) ^ (0u - (v & 1u)));
}

constexpr std::int64_t untransform(std::uint64_t v)
{
    return std::bit_cast<std::int64_t>((v >> 1) ^ (0ull - (v & 1ull)));
}

// Floats have their sign bit moved to the end.
float robloxFloat(std::uint32_t v)
{
    return std::bit_cast<float>(std::rotr(v, 1));
}

std::vector<float> floats(Reader& r, std::size_t count)
{
    return r.interleaved<std::uint32_t>(count) | std::views::transform(robloxFloat) | std::ranges::to<std::vector>();
}

// Referents are stored as differences from the previous one.
std::vector<std::int32_t> referents(Reader& r, std::size_t count)
{
    std::vector<std::int32_t> result;
    result.reserve(count);
    std::uint32_t total = 0;
    for (std::uint32_t v : r.interleaved<std::uint32_t>(count)) {
        total += std::bit_cast<std::uint32_t>(untransform(v));
        result.push_back(std::bit_cast<std::int32_t>(total));
    }
    return result;
}

using detail::basicRotation;

template <class T>
void addAll(std::vector<Value>& values, std::vector<T> items)
{
    for (T& item : items)
        values.emplace_back(std::move(item));
}

std::vector<std::int32_t> ints(Reader& r, std::size_t count)
{
    return r.interleaved<std::uint32_t>(count) | std::views::transform([](std::uint32_t v) { return untransform(v); })
           | std::ranges::to<std::vector>();
}

std::vector<CFrame> cframes(Reader& r, std::size_t count)
{
    std::vector<CFrame> result(count);
    for (CFrame& cf : result) {
        std::uint8_t id = r.byte();
        if (id == 0) {
            for (auto& row : cf.r)
                for (float& cell : row)
                    cell = r.read<float>();
        } else if (auto rotation = basicRotation(id)) {
            cf = *rotation;
        } else {
            fail(std::format("bad CFrame rotation id 0x{:02X}", id));
        }
    }
    auto x = floats(r, count), y = floats(r, count), z = floats(r, count);
    for (auto [cf, px, py, pz] : std::views::zip(result, x, y, z))
        cf.position = {px, py, pz};
    return result;
}

// 16-byte values, one byte plane at a time like interleaved().
std::vector<std::array<std::uint8_t, 16>> interleavedBlobs(Reader& r, std::size_t count)
{
    if (count > r.remaining() / 16)
        fail("unexpected end of data");
    std::string_view raw = r.bytes(count * 16);
    std::vector<std::array<std::uint8_t, 16>> blobs(count);
    for (std::size_t plane = 0; plane < 16; ++plane)
        for (std::size_t i = 0; i < count; ++i)
            blobs[i][plane] = static_cast<std::uint8_t>(raw[plane * count + i]);
    return blobs;
}

// The hex form XML files use: random, time, index.
std::string uniqueIdString(const std::array<std::uint8_t, 16>& blob)
{
    auto bigEndian = [&](std::size_t from, std::size_t size) {
        std::uint64_t v = 0;
        for (std::size_t i = from; i < from + size; ++i)
            v = (v << 8) | blob[i];
        return v;
    };
    auto index = bigEndian(0, 4), time = bigEndian(4, 4), random = std::rotr(bigEndian(8, 8), 1);
    // What std::format("{:016x}{:08x}{:08x}") gives, written by hand: every
    // instance of a model has one, and the format was most of the decode.
    std::string out(32, '0');
    auto hex = [&out](std::size_t at, std::uint64_t v, std::size_t digits) {
        for (std::size_t i = digits; i-- > 0; v >>= 4)
            out[at + i] = "0123456789abcdef"[v & 0xf];
    };
    hex(0, random, 16);
    hex(16, time, 8);
    hex(24, index, 8);
    return out;
}

// count reads; asking for more elements than bytes left is an error up front.
std::size_t checkedCount(Reader& r, std::size_t elementSize)
{
    auto n = r.read<std::uint32_t>();
    if (n > r.remaining() / elementSize)
        fail("unexpected end of data");
    return n;
}

void decodeColumn(Reader& r, PropertyColumn& column, std::size_t count, const std::vector<SharedBytes>& sharedStrings)
{
    auto& values = column.values;
    values.reserve(count);

    switch (column.type) {
    case typeString:
    case typeSourceBytecode: // kept as it is; the script runtime knows it by its header
        for (std::size_t i = 0; i < count; ++i)
            values.emplace_back(r.string());
        break;
    case typeBool:
        for (std::size_t i = 0; i < count; ++i)
            values.emplace_back(r.byte() != 0);
        break;
    case typeInt32:
        for (std::uint32_t v : r.interleaved<std::uint32_t>(count))
            values.emplace_back(static_cast<double>(untransform(v)));
        break;
    case typeFloat32:
        for (float f : floats(r, count))
            values.emplace_back(static_cast<double>(f));
        break;
    case typeFloat64:
        for (std::size_t i = 0; i < count; ++i)
            values.emplace_back(r.read<double>());
        break;
    case typeEnum:
        for (std::uint32_t v : r.interleaved<std::uint32_t>(count))
            values.emplace_back(static_cast<double>(v));
        break;
    case typeBrickColor:
        for (std::uint32_t v : r.interleaved<std::uint32_t>(count))
            values.emplace_back(BrickColor::fromNumber(static_cast<int>(v)));
        break;
    case typeSecurityCapabilities:
        for (std::uint64_t v : r.interleaved<std::uint64_t>(count))
            values.emplace_back(static_cast<double>(v));
        break;
    case typeUDim: {
        auto scale = floats(r, count);
        auto offset = ints(r, count);
        for (auto [s, o] : std::views::zip(scale, offset))
            values.emplace_back(UDim{s, o});
        break;
    }
    case typeUDim2: {
        auto sx = floats(r, count), sy = floats(r, count);
        auto ox = ints(r, count), oy = ints(r, count);
        for (auto [a, b, c, d] : std::views::zip(sx, sy, ox, oy))
            values.emplace_back(UDim2{{a, c}, {b, d}});
        break;
    }
    case typeFaces:
        for (std::size_t i = 0; i < count; ++i)
            values.emplace_back(Faces::fromBits(r.byte()));
        break;
    case typeAxes:
        for (std::size_t i = 0; i < count; ++i)
            values.emplace_back(Axes::fromBits(r.byte()));
        break;
    case typeVector2: {
        auto x = floats(r, count), y = floats(r, count);
        for (auto [vx, vy] : std::views::zip(x, y))
            values.emplace_back(Vector2{vx, vy});
        break;
    }
    case typeVector3int16:
        for (std::size_t i = 0; i < count; ++i) {
            auto x = r.read<std::int16_t>(), y = r.read<std::int16_t>(), z = r.read<std::int16_t>();
            values.emplace_back(Vector3int16{x, y, z});
        }
        break;
    case typeNumberSequence:
        for (std::size_t i = 0; i < count; ++i) {
            std::vector<NumberSequenceKeypoint> points(checkedCount(r, 12));
            for (auto& k : points)
                k = {r.read<float>(), r.read<float>(), r.read<float>()};
            values.emplace_back(NumberSequence(std::move(points)));
        }
        break;
    case typeColorSequence:
        for (std::size_t i = 0; i < count; ++i) {
            std::vector<ColorSequenceKeypoint> points(checkedCount(r, 20));
            for (auto& k : points) {
                k.time = r.read<float>();
                k.value = {r.read<float>(), r.read<float>(), r.read<float>()};
                r.read<float>(); // envelope, unused by Roblox
            }
            values.emplace_back(ColorSequence(std::move(points)));
        }
        break;
    case typeNumberRange:
        for (std::size_t i = 0; i < count; ++i) {
            auto lo = r.read<float>();
            values.emplace_back(NumberRange{lo, r.read<float>()});
        }
        break;
    case typeRect: {
        auto x0 = floats(r, count), y0 = floats(r, count), x1 = floats(r, count), y1 = floats(r, count);
        for (auto [a, b, c, d] : std::views::zip(x0, y0, x1, y1))
            values.emplace_back(Rect{{a, b}, {c, d}});
        break;
    }
    case typePhysicalProperties:
        // Flag bit 0: custom values follow (nil otherwise); bit 1: acoustic absorption too.
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t flags = r.byte();
            if ((flags & 1) == 0) {
                values.emplace_back();
                continue;
            }
            PhysicalProperties p;
            for (float* field : {&p.density, &p.friction, &p.elasticity, &p.frictionWeight, &p.elasticityWeight})
                *field = r.read<float>();
            if (flags & 2)
                p.acousticAbsorption = r.read<float>();
            values.emplace_back(p);
        }
        break;
    case typeSharedString:
        for (std::uint32_t index : r.interleaved<std::uint32_t>(count)) {
            if (index >= sharedStrings.size())
                fail(std::format("shared string {} doesn't exist", index));
            column.shared.push_back(index);
        }
        break;
    case typeOptionalCFrame: {
        // A CFrame column and a Bool column, each led by its type id.
        if (r.byte() != typeCFrame)
            fail("malformed OptionalCFrame");
        auto frames = cframes(r, count);
        if (r.byte() != typeBool)
            fail("malformed OptionalCFrame");
        for (const CFrame& cf : frames)
            values.emplace_back(r.byte() != 0 ? Value(cf) : Value());
        break;
    }
    case typeUniqueId:
        for (const auto& blob : interleavedBlobs(r, count))
            values.emplace_back(uniqueIdString(blob));
        break;
    case typeFont:
        for (std::size_t i = 0; i < count; ++i) {
            Font font;
            font.family = r.string();
            font.weight = static_cast<Enum::FontWeight>(r.read<std::uint16_t>());
            font.style = static_cast<Enum::FontStyle>(r.byte());
            r.string(); // cached face id
            values.emplace_back(std::move(font));
        }
        break;
    case typeContent: {
        // Source types (0 none, 1 uri, 2 object), then the uris and object referents in order.
        auto sources = ints(r, count);
        std::vector<std::string> uris(checkedCount(r, 4));
        for (std::string& uri : uris)
            uri = r.string();
        referents(r, checkedCount(r, 4));
        referents(r, checkedCount(r, 4)); // external objects
        std::size_t nextUri = 0;
        for (std::int32_t source : sources)
            values.emplace_back(source == 1 && nextUri < uris.size() ? uris[nextUri++] : std::string());
        break;
    }
    case typeInt64:
        for (std::uint64_t v : r.interleaved<std::uint64_t>(count))
            values.emplace_back(static_cast<double>(untransform(v)));
        break;
    case typeColor3: {
        auto red = floats(r, count), green = floats(r, count), blue = floats(r, count);
        for (auto [cr, cg, cb] : std::views::zip(red, green, blue))
            values.emplace_back(Color3{cr, cg, cb});
        break;
    }
    case typeColor3uint8: {
        std::string_view red = r.bytes(count), green = r.bytes(count), blue = r.bytes(count);
        for (auto [cr, cg, cb] : std::views::zip(red, green, blue))
            values.emplace_back(Color3::fromRGB(static_cast<std::uint8_t>(cr), static_cast<std::uint8_t>(cg),
                                                static_cast<std::uint8_t>(cb)));
        break;
    }
    case typeVector3: {
        auto x = floats(r, count), y = floats(r, count), z = floats(r, count);
        for (auto [vx, vy, vz] : std::views::zip(x, y, z))
            values.emplace_back(Vector3{vx, vy, vz});
        break;
    }
    case typeCFrame:
        addAll(values, cframes(r, count));
        break;
    case typeReferent:
        column.referents = referents(r, count);
        break;
    default:
        return; // not decoded
    }
    column.decoded = true;
}

Document parseDocument(std::string_view bytes)
{
    Reader file(bytes, "header");
    if (bytes.size() < magic.size() + signature.size() || file.bytes(magic.size()) != magic
        || file.bytes(signature.size()) != signature)
        fail("not a binary Roblox file");
    if (auto version = file.read<std::uint16_t>(); version != 0)
        fail(std::format("unsupported format version {}", version));
    file.read<std::int32_t>(); // class count, implied by the INST chunks
    auto instanceCount = file.read<std::int32_t>();
    file.bytes(8);
    if (instanceCount < 0)
        fail("negative instance count");

    Document doc;
    doc.instanceCount = static_cast<std::size_t>(instanceCount);
    std::unordered_map<std::int32_t, std::size_t> classIndex;

    for (;;) {
        if (file.done())
            fail("missing END chunk");
        std::string name(file.bytes(4));
        std::erase(name, '\0');
        auto compressedSize = file.read<std::uint32_t>();
        auto size = file.read<std::uint32_t>();
        file.bytes(4);

        std::string data;
        if (compressedSize == 0) {
            data = file.bytes(size);
        } else if (std::string_view block = file.bytes(compressedSize); block.starts_with(zstdMagic)) {
            // A ZSTD frame instead of an LZ4 block (UniversalApp.rbxm's chunks).
            data.resize(size);
            const std::size_t n = ZSTD_decompress(data.data(), data.size(), block.data(), block.size());
            if (ZSTD_isError(n) || n != size)
                fail(std::format("{} chunk: zstd: {}", name, ZSTD_isError(n) ? ZSTD_getErrorName(n) : "short output"));
        } else {
            auto decoded = lz4::decompressBlock(block, size);
            if (!decoded)
                fail(std::format("{} chunk: {}", name, decoded.error()));
            data = std::move(*decoded);
        }

        Reader chunk(data, name);
        if (name == "META") {
            for (auto count = chunk.read<std::uint32_t>(); count > 0; --count) {
                std::string key = chunk.string();
                doc.metadata.emplace_back(std::move(key), chunk.string());
            }
        } else if (name == "SSTR") {
            chunk.read<std::uint32_t>(); // version
            for (auto count = chunk.read<std::uint32_t>(); count > 0; --count) {
                chunk.bytes(16); // MD5 hash, unused
                doc.sharedStrings.push_back(std::make_shared<const std::string>(chunk.string()));
            }
        } else if (name == "INST") {
            ClassChunk cls;
            cls.id = chunk.read<std::int32_t>();
            cls.name = chunk.string();
            cls.isService = chunk.byte() == 1;
            cls.referents = referents(chunk, chunk.read<std::uint32_t>());
            if (!classIndex.emplace(cls.id, doc.classes.size()).second)
                fail(std::format("class id {} appears twice", cls.id));
            doc.classes.push_back(std::move(cls));
        } else if (name == "PROP") {
            auto id = chunk.read<std::int32_t>();
            auto it = classIndex.find(id);
            if (it == classIndex.end())
                fail(std::format("PROP chunk for unknown class id {}", id));
            ClassChunk& cls = doc.classes[it->second];
            PropertyColumn column;
            column.name = chunk.string();
            column.type = chunk.byte();
            // A column we can't read only costs that property, not the file.
            try {
                decodeColumn(chunk, column, cls.referents.size(), doc.sharedStrings);
                if (column.decoded && !chunk.done())
                    fail(std::format("{} bytes left over", chunk.remaining()));
            } catch (const Failure& failure) {
                column.decoded = false;
                column.error = failure.message;
                column.values.clear();
                column.referents.clear();
                column.shared.clear();
            }
            cls.properties.push_back(std::move(column));
        } else if (name == "PRNT") {
            chunk.byte(); // version
            auto count = chunk.read<std::uint32_t>();
            auto children = referents(chunk, count);
            auto parents = referents(chunk, count);
            for (auto [child, parent] : std::views::zip(children, parents))
                doc.parents.emplace_back(child, parent);
        } else if (name == "END") {
            return doc;
        }
        // Unknown chunks are skipped.
    }
}

// Turns a parsed document into instances; referents index `instances`.
class TreeBuilder {
public:
    detail::Builder builder;

    TreeBuilder(const Document& doc, DataModel* game) : doc_(doc), instances_(doc.instanceCount)
    {
        for (const ClassChunk& cls : doc.classes) {
            const ClassInfo* info = ClassRegistry::find(cls.name);
            bool service = game && info && info->kind == ClassKind::Service;
            for (std::int32_t referent : cls.referents) {
                if (!inRange(referent) || instances_[static_cast<std::size_t>(referent)])
                    fail(std::format("bad referent {} for class {}", referent, cls.name));
                instances_[static_cast<std::size_t>(referent)]
                    = service ? game->getService(cls.name)->shared_from_this() : builder.create(cls.name);
            }
        }
    }

    void applyProperties()
    {
        for (const ClassChunk& cls : doc_.classes) {
            if (cls.referents.empty())
                continue;
            Instance& sample = at(cls.referents.front());
            for (const PropertyColumn& column : cls.properties) {
                if (column.decoded && column.type == typeSharedString) {
                    applyShared(cls, column);
                    continue;
                }
                const Property* property = builder.settableProperty(sample, column.name);
                if (!property)
                    continue;
                if (!column.decoded) {
                    if (column.error.empty())
                        builder.warnOnce(std::format("{}.{}: binary type 0x{:02X} isn't supported yet",
                                                     sample.className(), column.name, column.type));
                    else
                        builder.warnOnce(std::format("{}.{}: {}", sample.className(), column.name, column.error));
                    continue;
                }
                // Some properties are written both ways, e.g. MaterialVariant
                // as a string and as an always-nil reference.
                if (column.type == typeReferent && !property->isRef) {
                    if (!std::ranges::all_of(column.referents, [](std::int32_t r) { return r == -1; }))
                        builder.warnOnce(std::format("{}.{}: a reference can't go into a {} property",
                                                     sample.className(), column.name, property->type));
                    continue;
                }
                for (auto [i, referent] : std::views::enumerate(cls.referents)) {
                    auto index = static_cast<std::size_t>(i);
                    if (column.type != typeReferent)
                        builder.assign(at(referent), *property, column.values[index]);
                    else if (auto target = refValue(column.referents[index]))
                        builder.assign(at(referent), *property, *target);
                    else
                        builder.warnOnce(std::format("{}.{} points at missing referent {}", sample.className(),
                                                     column.name, column.referents[index]));
                }
            }
        }
    }

    // A SharedString column: shared blobs go to their setter as they are,
    // anything else gets its own copy of the string.
    void applyShared(const ClassChunk& cls, const PropertyColumn& column)
    {
        Instance& sample = at(cls.referents.front());
        if (auto set = detail::Builder::sharedSetter(sample, column.name)) {
            for (auto [referent, index] : std::views::zip(cls.referents, column.shared))
                set(at(referent), doc_.sharedStrings[index]);
            return;
        }
        const Property* property = builder.settableProperty(sample, column.name);
        if (!property)
            return;
        for (auto [referent, index] : std::views::zip(cls.referents, column.shared))
            builder.assign(at(referent), *property, Value(*doc_.sharedStrings[index]));
    }

    // Applies PRNT and returns the instances the file leaves without a parent.
    std::vector<InstancePtr> link()
    {
        std::vector<bool> parented(instances_.size());
        for (auto [child, parent] : doc_.parents) {
            if (!exists(child) || (parent != -1 && !exists(parent))) {
                builder.warnOnce(std::format("PRNT links missing referents {} -> {}", child, parent));
                continue;
            }
            if (parent == -1)
                continue;
            try {
                at(child).setParent(&at(parent));
                parented[static_cast<std::size_t>(child)] = true;
            } catch (const Error& e) {
                builder.warnOnce(e.what());
            }
        }

        std::vector<InstancePtr> roots;
        for (auto [instance, hasParent] : std::views::zip(instances_, parented))
            if (instance && !hasParent)
                roots.push_back(instance);
        return roots;
    }

private:
    bool inRange(std::int32_t referent) const { return referent >= 0 && std::cmp_less(referent, instances_.size()); }
    bool exists(std::int32_t referent) const
    {
        return inRange(referent) && instances_[static_cast<std::size_t>(referent)];
    }
    Instance& at(std::int32_t referent) { return *instances_[static_cast<std::size_t>(referent)]; }

    std::optional<Value> refValue(std::int32_t referent) const
    {
        if (referent == -1)
            return Value();
        if (!exists(referent))
            return std::nullopt;
        return Value(instances_[static_cast<std::size_t>(referent)]);
    }

    const Document& doc_;
    std::vector<InstancePtr> instances_;
};

template <class F>
auto guarded(F&& f) -> std::expected<std::invoke_result_t<F>, std::string>
{
    try {
        return std::forward<F>(f)();
    } catch (const Failure& failure) {
        return std::unexpected(failure.message);
    }
}

} // namespace

std::expected<Document, std::string> parse(std::string_view bytes)
{
    return guarded([&] { return parseDocument(bytes); });
}

std::expected<LoadedPlace, std::string> loadPlace(std::string_view bytes)
{
    return parse(bytes).and_then([](const Document& doc) {
        return guarded([&] {
            auto game = DataModel::create();
            TreeBuilder tree(doc, game.get());
            tree.applyProperties();
            for (const InstancePtr& root : tree.link()) {
                try {
                    root->setParent(game);
                } catch (const Error& e) {
                    tree.builder.warnOnce(e.what());
                }
            }
            return LoadedPlace{std::move(game), std::move(tree.builder.warnings)};
        });
    });
}

std::expected<LoadedModel, std::string> loadModel(std::string_view bytes)
{
    return parse(bytes).and_then([](const Document& doc) {
        return guarded([&] {
            TreeBuilder tree(doc, nullptr);
            tree.applyProperties();
            auto roots = tree.link();
            return LoadedModel{std::move(roots), std::move(tree.builder.warnings)};
        });
    });
}

} // namespace lunar::rbxl
