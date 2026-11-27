#include "BinaryFormat.hpp"

#include <lunar/Enums.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>

namespace lunar::detail {

namespace {

struct Truncated {};

// Little-endian reads over the attribute blob; running out throws Truncated.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool done() const { return pos_ == data_.size(); }

    std::string_view bytes(std::size_t count)
    {
        if (count > data_.size() - pos_)
            throw Truncated{};
        std::string_view out = data_.substr(pos_, count);
        pos_ += count;
        return out;
    }

    template <class T>
    T read()
    {
        std::array<char, sizeof(T)> raw{};
        bytes(sizeof(T)).copy(raw.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
            std::ranges::reverse(raw);
        return std::bit_cast<T>(raw);
    }

    std::string string() { return std::string(bytes(read<std::uint32_t>())); }
    // An element count, checked against what's left so bad data can't ask for gigabytes.
    std::size_t count(std::size_t elementSize)
    {
        auto n = read<std::uint32_t>();
        if (n > (data_.size() - pos_) / elementSize)
            throw Truncated{};
        return n;
    }
    float f32() { return read<float>(); }
    Vector2 vector2() { return {f32(), f32()}; }
    Vector3 vector3() { return {f32(), f32(), f32()}; }
    UDim udim()
    {
        float scale = f32();
        return {scale, read<std::int32_t>()};
    }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

// Attribute value type ids.
enum : std::uint8_t {
    attrString = 0x02,
    attrBool = 0x03,
    attrInt32 = 0x04,
    attrFloat32 = 0x05,
    attrFloat64 = 0x06,
    attrUDim = 0x09,
    attrUDim2 = 0x0A,
    attrBrickColor = 0x0E,
    attrColor3 = 0x0F,
    attrVector2 = 0x10,
    attrVector3 = 0x11,
    attrCFrame = 0x14,
    attrEnum = 0x15,
    attrNumberSequence = 0x17,
    attrColorSequence = 0x19,
    attrNumberRange = 0x1B,
    attrRect = 0x1C,
    attrFont = 0x21,
};

std::expected<Value, std::string> readValue(Reader& r)
{
    auto type = r.read<std::uint8_t>();
    switch (type) {
    case attrString:
        return Value(r.string());
    case attrBool:
        return Value(r.read<std::uint8_t>() != 0);
    case attrInt32:
        return Value(static_cast<double>(r.read<std::int32_t>()));
    case attrFloat32:
        return Value(static_cast<double>(r.f32()));
    case attrFloat64:
        return Value(r.read<double>());
    case attrUDim:
        return Value(r.udim());
    case attrUDim2: {
        UDim x = r.udim();
        return Value(UDim2{x, r.udim()});
    }
    case attrBrickColor:
        return Value(BrickColor::fromNumber(static_cast<int>(r.read<std::uint32_t>())));
    case attrColor3: {
        Vector3 c = r.vector3();
        return Value(Color3{c.x, c.y, c.z});
    }
    case attrVector2:
        return Value(r.vector2());
    case attrVector3:
        return Value(r.vector3());
    case attrCFrame: {
        CFrame cf;
        Vector3 position = r.vector3();
        if (auto id = r.read<std::uint8_t>(); id == 0) {
            for (auto& row : cf.r)
                for (float& cell : row)
                    cell = r.f32();
        } else if (auto rotation = basicRotation(id)) {
            cf = *rotation;
        } else {
            return std::unexpected(std::format("bad CFrame rotation id 0x{:02X}", id));
        }
        cf.position = position;
        return Value(cf);
    }
    case attrEnum: {
        std::string enumName = r.string();
        auto value = r.read<std::uint32_t>();
        const EnumInfo* info = enums::find(enumName);
        const EnumItem* item = info ? info->find(value) : nullptr;
        if (!item)
            return std::unexpected(std::format("unknown enum item {} of Enum.{}", value, enumName));
        return Value(*item);
    }
    case attrNumberSequence: {
        std::vector<NumberSequenceKeypoint> points(r.count(12));
        for (auto& k : points) {
            k.envelope = r.f32();
            k.time = r.f32();
            k.value = r.f32();
        }
        return Value(NumberSequence(std::move(points)));
    }
    case attrColorSequence: {
        std::vector<ColorSequenceKeypoint> points(r.count(20));
        for (auto& k : points) {
            r.f32(); // envelope
            k.time = r.f32();
            Vector3 c = r.vector3();
            k.value = {c.x, c.y, c.z};
        }
        return Value(ColorSequence(std::move(points)));
    }
    case attrNumberRange: {
        float lo = r.f32();
        return Value(NumberRange{lo, r.f32()});
    }
    case attrRect: {
        Vector2 lo = r.vector2();
        return Value(Rect{lo, r.vector2()});
    }
    case attrFont: {
        Font font;
        font.weight = static_cast<Enum::FontWeight>(r.read<std::uint16_t>());
        font.style = static_cast<Enum::FontStyle>(r.read<std::uint8_t>());
        font.family = r.string();
        r.string(); // cached face id
        return Value(std::move(font));
    }
    default:
        return std::unexpected(std::format("attribute type 0x{:02X} isn't supported yet", type));
    }
}

} // namespace

// Axis-aligned rotations are stored as one byte: id - 1 = 6 * right + up, with
// the axes numbered +X +Y +Z -X -Y -Z. Those are the first two matrix columns,
// the third (back, -LookVector) is right x up.
std::optional<CFrame> basicRotation(std::uint8_t id)
{
    static constexpr std::array<Vector3, 6> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};
    if (id < 0x02 || id > 0x23)
        return std::nullopt;
    std::size_t right = (id - 1u) / 6;
    std::size_t up = (id - 1u) % 6;
    if (right % 3 == up % 3)
        return std::nullopt; // parallel axes
    std::array<Vector3, 3> columns{axes[right], axes[up], axes[right].cross(axes[up])};

    CFrame cf;
    for (std::size_t c = 0; c < 3; ++c) {
        cf.r[0][c] = columns[c].x;
        cf.r[1][c] = columns[c].y;
        cf.r[2][c] = columns[c].z;
    }
    return cf;
}

std::expected<std::vector<std::pair<std::string, Value>>, std::string> decodeAttributes(std::string_view bytes)
{
    std::vector<std::pair<std::string, Value>> result;
    if (bytes.empty())
        return result;
    try {
        Reader r(bytes);
        for (auto count = r.read<std::uint32_t>(); count > 0; --count) {
            std::string name = r.string();
            auto value = readValue(r);
            if (!value)
                return std::unexpected(std::format("attribute {}: {}", name, value.error()));
            result.emplace_back(std::move(name), std::move(*value));
        }
        if (!r.done())
            return std::unexpected("attribute data has bytes left over");
    } catch (const Truncated&) {
        return std::unexpected("attribute data is truncated");
    }
    return result;
}

std::vector<std::string> decodeTags(std::string_view bytes)
{
    std::vector<std::string> tags;
    for (auto part : bytes | std::views::split('\0'))
        if (!std::ranges::empty(part))
            tags.emplace_back(std::string_view(part));
    return tags;
}

std::expected<std::vector<CollisionGroup>, std::string> decodeCollisionGroups(std::string_view text)
{
    std::vector<CollisionGroup> groups;
    for (auto entry : text | std::views::split('\\')) {
        std::vector<std::string_view> fields;
        for (auto field : entry | std::views::split('^'))
            fields.emplace_back(field);
        if (fields.size() == 1 && fields[0].empty())
            continue;
        int id = 0;
        std::int64_t mask = 0;
        if (fields.size() != 3 || std::from_chars(fields[1].begin(), fields[1].end(), id).ec != std::errc{}
            || std::from_chars(fields[2].begin(), fields[2].end(), mask).ec != std::errc{})
            return std::unexpected(std::format("bad collision group '{}'", std::string_view(entry)));
        groups.push_back({std::string(fields[0]), id, static_cast<std::uint32_t>(mask)});
    }
    return groups;
}

std::expected<std::vector<CollisionGroup>, std::string> decodeCollisionGroupData(std::string_view bytes)
{
    // Version, group count, then per group: id, mask size and mask, name.
    std::vector<CollisionGroup> groups;
    if (bytes.empty())
        return groups;
    try {
        Reader r(bytes);
        if (auto version = r.read<std::uint8_t>(); version != 1)
            return std::unexpected(std::format("collision group data version {} isn't supported", version));
        for (auto count = r.read<std::uint8_t>(); count > 0; --count) {
            CollisionGroup group;
            group.id = r.read<std::uint8_t>();
            if (auto maskSize = r.read<std::uint8_t>(); maskSize != 4)
                return std::unexpected(std::format("unexpected collision mask size {}", maskSize));
            group.mask = r.read<std::uint32_t>();
            group.name = std::string(r.bytes(r.read<std::uint8_t>()));
            groups.push_back(std::move(group));
        }
        if (!r.done())
            return std::unexpected("collision group data has bytes left over");
    } catch (const Truncated&) {
        return std::unexpected("collision group data is truncated");
    }
    return groups;
}

std::expected<std::vector<TerrainChunk>, std::string> decodeSmoothGrid(std::string_view bytes)
{
    // Version 1, chunk size shift 5 (32 cells), then per chunk: a coordinate
    // delta (three big-endian int32s stored one byte plane at a time) and runs
    // of cells. A run header byte has the material index in its low 6 bits;
    // 0x40 means an occupancy byte follows (else full), 0x80 a count byte
    // (count + 1 cells, with a water byte after a zero count).
    constexpr auto side = static_cast<std::size_t>(TerrainChunk::size);
    constexpr std::size_t chunkCells = side * side * side;
    std::vector<TerrainChunk> chunks;
    if (bytes.empty())
        return chunks;
    try {
        Reader r(bytes);
        if (auto version = r.read<std::uint8_t>(); version != 1)
            return std::unexpected(std::format("SmoothGrid version {} isn't supported", version));
        if (auto shift = r.read<std::uint8_t>(); shift != 5)
            return std::unexpected(std::format("SmoothGrid chunk shift {} isn't supported", shift));
        std::array<std::int64_t, 3> position{};
        while (!r.done()) {
            std::string_view delta = r.bytes(12);
            TerrainChunk chunk;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                std::uint32_t d = 0;
                for (std::size_t plane = 0; plane < 4; ++plane)
                    d = d << 8 | static_cast<std::uint8_t>(delta[axis + 3 * plane]);
                position[axis] += std::bit_cast<std::int32_t>(d);
                if (position[axis] < -(1 << 20) || position[axis] >= (1 << 20))
                    return std::unexpected("SmoothGrid chunk coordinate out of range");
                chunk.position[axis] = static_cast<int>(position[axis]);
            }
            bool occupied = false;
            for (std::size_t cells = 0; cells < chunkCells;) {
                auto header = r.read<std::uint8_t>();
                std::size_t index = header & 0x3F;
                if (index >= Terrain::materials.size())
                    return std::unexpected(std::format("SmoothGrid material index {} isn't known", index));
                TerrainChunk::Run run{0, static_cast<std::uint8_t>(index), 255, 0};
                if (header & 0x40)
                    run.occupancy = r.read<std::uint8_t>();
                std::size_t count = 1;
                if (header & 0x80) {
                    auto n = r.read<std::uint8_t>();
                    if (n == 0)
                        run.water = r.read<std::uint8_t>();
                    count = n + 1u;
                }
                if (count > chunkCells - cells)
                    return std::unexpected("SmoothGrid run crosses a chunk boundary");
                if (index == 0)
                    run.occupancy = 0;
                if (index < 2 || run.occupancy == 255)
                    run.water = 0;
                occupied = occupied || index != 0;
                TerrainChunk::Run* last = chunk.runs.empty() ? nullptr : &chunk.runs.back();
                if (last && last->material == run.material && last->occupancy == run.occupancy
                    && last->water == run.water && last->count + count <= 0xFFFF)
                    last->count = static_cast<std::uint16_t>(last->count + count);
                else
                    chunk.runs.push_back({static_cast<std::uint16_t>(count), run.material, run.occupancy, run.water});
                cells += count;
            }
            if (occupied)
                chunks.push_back(std::move(chunk));
        }
    } catch (const Truncated&) {
        return std::unexpected("SmoothGrid data is truncated");
    }
    return chunks;
}

std::expected<std::array<Color3, Terrain::materials.size()>, std::string> decodeMaterialColors(std::string_view bytes)
{
    std::array<Color3, Terrain::materials.size()> colors{};
    if (bytes.size() != colors.size() * 3)
        return std::unexpected(
            std::format("MaterialColors should be {} bytes, not {}", colors.size() * 3, bytes.size()));
    for (std::size_t i = 0; i < colors.size(); ++i)
        colors[i]
            = Color3::fromRGB(static_cast<std::uint8_t>(bytes[3 * i]), static_cast<std::uint8_t>(bytes[3 * i + 1]),
                              static_cast<std::uint8_t>(bytes[3 * i + 2]));
    return colors;
}

} // namespace lunar::detail
