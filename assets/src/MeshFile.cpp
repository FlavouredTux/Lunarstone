#include "MeshGeometry.hpp"

#include <lunar/assets/MeshFile.hpp>

#include <lunar/PlaceFile.hpp>
#include <lunar/classes/Core.hpp>
#include <lunar/classes/Parts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace lunar::assets {

namespace {

using detail::RawGeometry;
using detail::RawVertex;

static_assert(std::endian::native == std::endian::little, "mesh files are little-endian");

template <class T>
using Result = std::expected<T, std::string>;

// Bounds-checked little-endian reads over the file.
class Reader {
public:
    explicit Reader(std::string_view data, std::size_t pos = 0) : data_(data), pos_(pos) {}

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool has(std::size_t bytes) const { return bytes <= remaining(); }

    template <class T>
        requires std::is_trivially_copyable_v<T>
    Result<T> read(std::string_view what)
    {
        if (!has(sizeof(T)))
            return std::unexpected(std::format("truncated {} at byte {}", what, pos_));
        T value;
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    Result<std::string_view> bytes(std::size_t count, std::string_view what)
    {
        if (!has(count))
            return std::unexpected(
                std::format("truncated {}: {} bytes at byte {}, {} left", what, count, pos_, remaining()));
        std::string_view out = data_.substr(pos_, count);
        pos_ += count;
        return out;
    }

    Result<void> skip(std::size_t count, std::string_view what)
    {
        return bytes(count, what).transform([](auto) {});
    }

private:
    std::string_view data_;
    std::size_t pos_;
};

// count * size, failing instead of wrapping.
Result<std::size_t> arrayBytes(std::uint64_t count, std::size_t size, std::string_view what)
{
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return std::unexpected(std::format("{} count {} is too large", what, count));
    return static_cast<std::size_t>(count) * size;
}

float floatAt(std::string_view record, std::size_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, record.data() + offset, sizeof value);
    return value;
}

// Binary vertices are at least position, normal and UV (32 bytes, plus an unused
// UV w in some exporters' 36-byte records); 40-byte records add a packed
// tangent and an RGBA colour at the end.
Result<std::vector<RawVertex>> readVertices(Reader& in, std::uint32_t count, std::size_t vertexSize)
{
    if (vertexSize < 32)
        return std::unexpected(std::format("vertex records are {} bytes, too small for a vertex", vertexSize));
    auto size = arrayBytes(count, vertexSize, "vertex");
    if (!size)
        return std::unexpected(size.error());
    auto block = in.bytes(*size, "vertices");
    if (!block)
        return std::unexpected(block.error());

    std::vector<RawVertex> vertices(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::string_view record = block->substr(i * vertexSize, vertexSize);
        RawVertex& v = vertices[i];
        v.position = {floatAt(record, 0), floatAt(record, 4), floatAt(record, 8)};
        v.normal = {floatAt(record, 12), floatAt(record, 16), floatAt(record, 20)};
        v.u = floatAt(record, 24);
        v.v = floatAt(record, 28);
        if (vertexSize >= 40)
            std::memcpy(v.rgba.data(), record.data() + 36, 4);
    }
    return vertices;
}

Result<std::vector<std::uint32_t>> readFaces(Reader& in, std::uint32_t count, std::size_t faceSize)
{
    if (faceSize < 12)
        return std::unexpected(std::format("face records are {} bytes, too small for three indices", faceSize));
    auto size = arrayBytes(count, faceSize, "face");
    if (!size)
        return std::unexpected(size.error());
    auto block = in.bytes(*size, "faces");
    if (!block)
        return std::unexpected(block.error());

    std::vector<std::uint32_t> indices(static_cast<std::size_t>(count) * 3);
    for (std::size_t i = 0; i < count; ++i)
        std::memcpy(indices.data() + i * 3, block->data() + i * faceSize, 12);
    return indices;
}

Result<std::vector<std::uint32_t>> readU32s(Reader& in, std::uint32_t count, std::string_view what)
{
    auto size = arrayBytes(count, 4, what);
    if (!size)
        return std::unexpected(size.error());
    auto block = in.bytes(*size, what);
    if (!block)
        return std::unexpected(block.error());
    std::vector<std::uint32_t> values(count);
    if (count > 0)
        std::memcpy(values.data(), block->data(), *size);
    return values;
}

// The LOD table's levels: level k is faces [lods[k], lods[k + 1]). Tables
// with fewer than two entries mean the whole file is one level; a first range
// that isn't ordered inside the faces is treated the same, since drawing every
// face beats nothing. Coarser levels end at the first range that isn't, and
// are only wanted with keepLods.
std::vector<std::pair<std::size_t, std::size_t>> lodRanges(const std::vector<std::uint32_t>& lods,
                                                           std::size_t faceCount, bool keepLods)
{
    if (lods.size() < 2 || lods[0] >= lods[1] || lods[1] > faceCount)
        return {{0, faceCount}};
    std::vector<std::pair<std::size_t, std::size_t>> ranges{{lods[0], lods[1]}};
    for (std::size_t k = 2; keepLods && k < lods.size() && lods[k] > lods[k - 1] && lods[k] <= faceCount; ++k)
        ranges.emplace_back(lods[k - 1], lods[k]);
    return ranges;
}

bool finite(Vector3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool usable(const RawVertex& v)
{
    return finite(v.position) && std::isfinite(v.u) && std::isfinite(v.v);
}

// Keeps the faces of each level of detail and only the vertices they use:
// LOD 0's first, in file order, then the ones only coarser levels add. Lower
// LODs of real files sometimes reference junk (NaN) vertices LOD 0 never
// touches, a handful of faces per level: those faces are left out, and a
// level with more than maxJunkShare of them is dropped, with coarser ones.
Result<MeshFile> buildMesh(std::string version, const RawGeometry& raw,
                           std::span<const std::pair<std::size_t, std::size_t>> levels)
{
    const std::size_t vertexCount = raw.vertices.size();
    for (std::size_t i = 0; i < raw.indices.size(); ++i)
        if (raw.indices[i] >= vertexCount)
            return std::unexpected(
                std::format("face {} references vertex {} of {}", i / 3, raw.indices[i], vertexCount));

    constexpr float maxJunkShare = 0.05f;
    constexpr std::uint32_t unused = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint32_t wanted = unused - 1;
    std::vector<std::uint32_t> remap(vertexCount, unused);
    MeshFile file;
    file.version = std::move(version);
    auto& out = file.mesh;
    std::size_t lod0Vertices = 0;
    auto faceUsable = [&](std::size_t face) {
        for (std::size_t k = 0; k < 3; ++k)
            if (!usable(raw.vertices[raw.indices[face * 3 + k]]))
                return false;
        return true;
    };
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto [faceBegin, faceEnd] = levels[level];
        std::size_t junk = 0;
        for (std::size_t face = faceBegin; face < faceEnd; ++face) {
            if (faceUsable(face))
                continue;
            if (level == 0)
                for (std::size_t k = 0; k < 3; ++k)
                    if (const std::uint32_t index = raw.indices[face * 3 + k]; !usable(raw.vertices[index]))
                        return std::unexpected(std::format("vertex {} isn't a finite number", index));
            ++junk;
        }
        if (static_cast<float>(junk) > maxJunkShare * static_cast<float>(faceEnd - faceBegin))
            break;
        for (std::size_t face = faceBegin; face < faceEnd; ++face)
            if (junk == 0 || faceUsable(face))
                for (std::size_t k = 0; k < 3; ++k)
                    if (std::uint32_t& to = remap[raw.indices[face * 3 + k]]; to == unused)
                        to = wanted;
        // Vertices keep their file order within each level's additions.
        for (std::size_t index = 0; index < vertexCount; ++index) {
            if (remap[index] != wanted)
                continue;
            const RawVertex& v = raw.vertices[index];
            MeshVertex vertex;
            vertex.position = v.position;
            const float length = finite(v.normal) ? v.normal.magnitude() : 0.0f;
            vertex.normal = length > 1e-6f ? v.normal / length : Vector3{0.0f, 1.0f, 0.0f};
            vertex.u = v.u;
            vertex.v = v.v;
            vertex.color = Color3::fromRGB(v.rgba[0], v.rgba[1], v.rgba[2]);
            remap[index] = static_cast<std::uint32_t>(out.vertices.size());
            out.vertices.push_back(vertex);
        }
        std::vector<std::uint32_t>& indices = level == 0 ? out.indices : out.lodIndices;
        const std::size_t first = indices.size();
        indices.reserve(first + (faceEnd - faceBegin - junk) * 3);
        for (std::size_t face = faceBegin; face < faceEnd; ++face)
            if (junk == 0 || faceUsable(face))
                for (std::size_t k = 0; k < 3; ++k)
                    indices.push_back(remap[raw.indices[face * 3 + k]]);
        if (level == 0)
            lod0Vertices = out.vertices.size();
        else
            out.lodEnds.push_back(static_cast<std::uint32_t>(out.lodIndices.size()));
    }

    // The bounds are LOD 0's, whose vertices come first: coarser levels can
    // poke out a little, and MeshParts are fitted to the full-detail mesh.
    if (lod0Vertices > 0) {
        file.boundsMin = file.boundsMax = out.vertices.front().position;
        for (std::size_t i = 0; i < lod0Vertices; ++i) {
            const Vector3 p = out.vertices[i].position;
            file.boundsMin
                = {std::min(file.boundsMin.x, p.x), std::min(file.boundsMin.y, p.y), std::min(file.boundsMin.z, p.z)};
            file.boundsMax
                = {std::max(file.boundsMax.x, p.x), std::max(file.boundsMax.y, p.y), std::max(file.boundsMax.z, p.z)};
        }
    }
    return file;
}

// --- version 1: text ----------------------------------------------------------------

class TextReader {
public:
    explicit TextReader(std::string_view text) : text_(text) {}

    void skipSpace()
    {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n'
                   || text_[pos_] == ','))
            ++pos_;
    }

    template <class T>
    Result<T> number(std::string_view what)
    {
        skipSpace();
        T value{};
        // from_chars doesn't take a leading '+', which some exporters write.
        if (pos_ < text_.size() && text_[pos_] == '+')
            ++pos_;
        auto [end, ec] = std::from_chars(text_.data() + pos_, text_.data() + text_.size(), value);
        if (ec != std::errc{})
            return std::unexpected(std::format("expected {} at byte {}", what, pos_));
        pos_ = static_cast<std::size_t>(end - text_.data());
        return value;
    }

    Result<void> expect(char c)
    {
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_] != c)
            return std::unexpected(std::format("expected '{}' at byte {}", c, pos_));
        ++pos_;
        return {};
    }

    // [x,y,z]
    Result<Vector3> vector()
    {
        if (auto open = expect('['); !open)
            return std::unexpected(open.error());
        Vector3 v;
        for (float* component : {&v.x, &v.y, &v.z}) {
            auto value = number<float>("a number");
            if (!value)
                return std::unexpected(value.error());
            *component = *value;
        }
        if (auto close = expect(']'); !close)
            return std::unexpected(close.error());
        return v;
    }

    std::size_t pos() const { return pos_; }
    std::size_t size() const { return text_.size(); }
    void seek(std::size_t pos) { pos_ = pos; }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

// A face count, then [position][normal][uv] for three vertices per face. 1.00
// files are at twice their real size, and V runs the other way from later
// versions.
Result<MeshFile> parseText(std::string_view bytes, std::size_t bodyStart, std::string version)
{
    TextReader in(bytes);
    in.seek(bodyStart);
    auto faceCount = in.number<std::uint64_t>("the face count");
    if (!faceCount)
        return std::unexpected(faceCount.error());
    // Each vertex takes far more than one byte of text, so this bounds the count
    // before anything is allocated.
    if (*faceCount > bytes.size())
        return std::unexpected(std::format("{} faces can't fit in {} bytes", *faceCount, bytes.size()));

    const float scale = version == "1.00" ? 0.5f : 1.0f;
    RawGeometry raw;
    const auto vertexCount = static_cast<std::size_t>(*faceCount) * 3;
    raw.vertices.reserve(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        RawVertex v;
        auto position = in.vector();
        if (!position)
            return std::unexpected(std::format("vertex {}: {}", i, position.error()));
        auto normal = in.vector();
        if (!normal)
            return std::unexpected(std::format("vertex {}: {}", i, normal.error()));
        auto uv = in.vector();
        if (!uv)
            return std::unexpected(std::format("vertex {}: {}", i, uv.error()));
        v.position = *position * scale;
        v.normal = *normal;
        v.u = uv->x;
        v.v = 1.0f - uv->y;
        raw.vertices.push_back(v);
        raw.indices.push_back(static_cast<std::uint32_t>(i));
    }
    const std::pair<std::size_t, std::size_t> all{0, static_cast<std::size_t>(*faceCount)};
    return buildMesh(std::move(version), raw, std::span(&all, 1));
}

// --- versions 2 to 5: one binary header ---------------------------------------------

Result<MeshFile> parseV2(Reader& in, std::string version, const MeshParseOptions& options)
{
    const std::size_t start = in.pos();
    auto headerSize = in.read<std::uint16_t>("header size");
    auto vertexSize = in.read<std::uint8_t>("vertex size");
    auto faceSize = in.read<std::uint8_t>("face size");
    if (!headerSize || !vertexSize || !faceSize)
        return std::unexpected("truncated header");

    std::uint32_t lodCount = 0;
    if (version.starts_with('3')) {
        auto lodSize = in.read<std::uint16_t>("LOD size");
        auto lods = in.read<std::uint16_t>("LOD count");
        if (!lodSize || !lods)
            return std::unexpected("truncated header");
        if (*lodSize != 4 && *lods != 0)
            return std::unexpected(std::format("LOD entries are {} bytes, not 4", *lodSize));
        lodCount = *lods;
    }
    auto vertexCount = in.read<std::uint32_t>("vertex count");
    auto faceCount = in.read<std::uint32_t>("face count");
    if (!vertexCount || !faceCount)
        return std::unexpected("truncated header");
    if (*headerSize < in.pos() - start)
        return std::unexpected(std::format("header size {} is smaller than its fields", *headerSize));
    if (auto skipped = in.skip(*headerSize - (in.pos() - start), "header"); !skipped)
        return std::unexpected(skipped.error());

    RawGeometry raw;
    auto vertices = readVertices(in, *vertexCount, *vertexSize);
    if (!vertices)
        return std::unexpected(vertices.error());
    raw.vertices = std::move(*vertices);
    auto indices = readFaces(in, *faceCount, *faceSize);
    if (!indices)
        return std::unexpected(indices.error());
    raw.indices = std::move(*indices);
    auto lods = readU32s(in, lodCount, "LOD table");
    if (!lods)
        return std::unexpected(lods.error());

    auto file = buildMesh(std::move(version), raw, lodRanges(*lods, *faceCount, options.keepLods));
    if (file)
        file->lodCount = std::max<std::size_t>(1, !lods->empty() ? lods->size() - 1 : 0);
    return file;
}

constexpr std::size_t envelopeSize = 8; // four bone indices, four weights
constexpr std::size_t boneSize = 60;    // name index, parents, culling radius, 3x3 rotation, position
constexpr std::size_t subsetSize = 72;  // face/vertex ranges and 26 bone indices

Result<MeshFile> parseV4(Reader& in, std::string version, const MeshParseOptions& options)
{
    const std::size_t start = in.pos();
    auto headerSize = in.read<std::uint16_t>("header size");
    auto lodType = in.read<std::uint16_t>("LOD type");
    auto vertexCount = in.read<std::uint32_t>("vertex count");
    auto faceCount = in.read<std::uint32_t>("face count");
    auto lodCount = in.read<std::uint16_t>("LOD count");
    auto boneCount = in.read<std::uint16_t>("bone count");
    auto nameBytes = in.read<std::uint32_t>("bone name size");
    auto subsetCount = in.read<std::uint16_t>("subset count");
    auto highQualityLods = in.read<std::uint8_t>("LOD count");
    auto padding = in.read<std::uint8_t>("padding");
    if (!headerSize || !lodType || !vertexCount || !faceCount || !lodCount || !boneCount || !nameBytes || !subsetCount
        || !highQualityLods || !padding)
        return std::unexpected("truncated header");
    std::uint32_t facsBytes = 0;
    if (version.starts_with('5')) {
        auto facsFormat = in.read<std::uint32_t>("FACS format");
        auto facsSize = in.read<std::uint32_t>("FACS size");
        if (!facsFormat || !facsSize)
            return std::unexpected("truncated header");
        facsBytes = *facsSize;
    }
    if (*headerSize < in.pos() - start)
        return std::unexpected(std::format("header size {} is smaller than its fields", *headerSize));
    if (auto skipped = in.skip(*headerSize - (in.pos() - start), "header"); !skipped)
        return std::unexpected(skipped.error());

    RawGeometry raw;
    auto vertices = readVertices(in, *vertexCount, 40);
    if (!vertices)
        return std::unexpected(vertices.error());
    raw.vertices = std::move(*vertices);
    if (*boneCount > 0) {
        auto envelopes = arrayBytes(*vertexCount, envelopeSize, "envelope");
        if (!envelopes)
            return std::unexpected(envelopes.error());
        if (auto skipped = in.skip(*envelopes, "skinning envelopes"); !skipped)
            return std::unexpected(skipped.error());
    }
    auto indices = readFaces(in, *faceCount, 12);
    if (!indices)
        return std::unexpected(indices.error());
    raw.indices = std::move(*indices);
    auto lods = readU32s(in, *lodCount, "LOD table");
    if (!lods)
        return std::unexpected(lods.error());

    // Skinning and FACS aren't drawn yet, but walking over them checks the file
    // is as long as its header says.
    if (auto skipped = in.skip(*boneCount * boneSize, "bones"); !skipped)
        return std::unexpected(skipped.error());
    if (auto skipped = in.skip(*nameBytes, "bone names"); !skipped)
        return std::unexpected(skipped.error());
    if (auto skipped = in.skip(*subsetCount * subsetSize, "mesh subsets"); !skipped)
        return std::unexpected(skipped.error());
    if (auto skipped = in.skip(facsBytes, "FACS data"); !skipped)
        return std::unexpected(skipped.error());

    auto file = buildMesh(std::move(version), raw, lodRanges(*lods, *faceCount, options.keepLods));
    if (file) {
        file->lodCount = std::max<std::size_t>(1, !lods->empty() ? lods->size() - 1 : 0);
        file->boneCount = *boneCount;
        file->hasFacs = facsBytes > 0;
    }
    return file;
}

// --- versions 6 and 7: chunks -------------------------------------------------------

// COREMESH version 1 is plain vertices and faces; version 2 is a Draco
// bitstream, which some writers prefix with its length.
Result<RawGeometry> parseCoreMesh(std::string_view data, std::uint32_t chunkVersion)
{
    if (chunkVersion == 2) {
        if (data.starts_with("DRACO"))
            return detail::decodeDraco(data);
        if (data.size() >= 4 && data.substr(4).starts_with("DRACO")) {
            std::uint32_t length = 0;
            std::memcpy(&length, data.data(), 4);
            return detail::decodeDraco(data.substr(4, std::min<std::size_t>(length, data.size() - 4)));
        }
        return std::unexpected("COREMESH chunk version 2 doesn't hold a Draco bitstream");
    }
    if (chunkVersion != 1)
        return std::unexpected(std::format("COREMESH chunk version {} isn't supported", chunkVersion));

    Reader in(data);
    RawGeometry raw;
    auto vertexCount = in.read<std::uint32_t>("vertex count");
    if (!vertexCount)
        return std::unexpected(vertexCount.error());
    auto vertices = readVertices(in, *vertexCount, 40);
    if (!vertices)
        return std::unexpected(vertices.error());
    raw.vertices = std::move(*vertices);
    auto faceCount = in.read<std::uint32_t>("face count");
    if (!faceCount)
        return std::unexpected(faceCount.error());
    auto indices = readFaces(in, *faceCount, 12);
    if (!indices)
        return std::unexpected(indices.error());
    raw.indices = std::move(*indices);
    return raw;
}

Result<std::vector<std::uint32_t>> parseLods(std::string_view data)
{
    Reader in(data);
    auto lodType = in.read<std::uint16_t>("LOD type");
    auto highQuality = in.read<std::uint8_t>("LOD count");
    auto count = in.read<std::uint32_t>("LOD count");
    if (!lodType || !highQuality || !count)
        return std::unexpected("truncated LODS chunk");
    return readU32s(in, *count, "LOD table");
}

Result<std::size_t> parseBoneCount(std::string_view data)
{
    Reader in(data);
    auto envelopes = in.read<std::uint32_t>("envelope count");
    if (!envelopes)
        return std::unexpected(envelopes.error());
    auto size = arrayBytes(*envelopes, envelopeSize, "envelope");
    if (!size)
        return std::unexpected(size.error());
    if (auto skipped = in.skip(*size, "envelopes"); !skipped)
        return std::unexpected(skipped.error());
    auto bones = in.read<std::uint32_t>("bone count");
    if (!bones)
        return std::unexpected(bones.error());
    return *bones;
}

Result<MeshFile> parseChunked(Reader& in, std::string version, const MeshParseOptions& options)
{
    std::optional<RawGeometry> geometry;
    std::vector<std::uint32_t> lods;
    std::size_t boneCount = 0;
    bool hasFacs = false;

    while (in.remaining() > 0) {
        auto type = in.bytes(8, "chunk type");
        auto chunkVersion = in.read<std::uint32_t>("chunk version");
        auto size = in.read<std::uint32_t>("chunk size");
        if (!type || !chunkVersion || !size)
            return std::unexpected(std::format("truncated chunk header at byte {}", in.pos()));
        const std::string_view name = type->substr(0, type->find('\0'));
        auto data = in.bytes(*size, std::format("{} chunk", name));
        if (!data)
            return std::unexpected(data.error());

        if (name == "COREMESH") {
            auto raw = parseCoreMesh(*data, *chunkVersion);
            if (!raw)
                return std::unexpected(std::format("COREMESH: {}", raw.error()));
            geometry = std::move(*raw);
        } else if (name == "LODS") {
            auto table = parseLods(*data);
            if (!table)
                return std::unexpected(std::format("LODS: {}", table.error()));
            lods = std::move(*table);
        } else if (name == "SKINNING") {
            auto bones = parseBoneCount(*data);
            if (!bones)
                return std::unexpected(std::format("SKINNING: {}", bones.error()));
            boneCount = *bones;
        } else if (name == "FACS") {
            hasFacs = true;
        }
        // Anything else (HSRAVIS, the hidden-surface flags, and whatever comes
        // later) is skipped whole.
    }
    if (!geometry)
        return std::unexpected("no COREMESH chunk");

    auto file
        = buildMesh(std::move(version), *geometry, lodRanges(lods, geometry->indices.size() / 3, options.keepLods));
    if (file) {
        file->lodCount = std::max<std::size_t>(1, !lods.empty() ? lods.size() - 1 : 0);
        file->boneCount = boneCount;
        file->hasFacs = hasFacs;
    }
    return file;
}

} // namespace

bool looksLikeMesh(std::string_view bytes)
{
    return bytes.starts_with("version ");
}

namespace {

// CSGMDL version 5 [live assets]: after the header, u16 count + count x f32
// position; u16 count, u32 size, count x u16 normal (0x7fff is zero); u16
// count + RGBA; u16 count + u8 face id; u16 count + f32 u, v; u16 count, u32
// size, u16 tangents; u32 index count, u32 size, the index deltas; u8 count +
// u32 offsets into the indices, one level of detail between each two.
std::expected<MeshFile, std::string> parseCsgMesh5(std::string_view data)
{
    std::size_t at = 10;
    bool bad = false;
    auto take = [&](std::size_t n) -> const std::uint8_t* {
        if (bad || at + n > data.size()) {
            bad = true;
            return nullptr;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data() + at);
        at += n;
        return p;
    };
    auto u16 = [&] {
        const auto* p = take(2);
        return p ? static_cast<std::uint32_t>(p[0] | (p[1] << 8)) : 0u;
    };
    auto u32 = [&] {
        std::uint32_t v = 0;
        if (const auto* p = take(4))
            std::memcpy(&v, p, 4);
        return v;
    };
    auto f32 = [](const std::uint8_t* p) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    };
    auto unit = [](const std::uint8_t* p) {
        return static_cast<float>(p[0] | (p[1] << 8)) / 32767.0f - 1.0f;
    };

    const std::uint32_t count = u16();
    const auto* positions = take(std::size_t{count} * 12);
    const std::uint32_t normalCount = u16(), normalSize = u32();
    const auto* normals = take(normalSize);
    const std::uint32_t colorCount = u16();
    const auto* colors = take(std::size_t{colorCount} * 4);
    take(u16()); // the face each vertex came from
    const std::uint32_t uvCount = u16();
    const auto* uvs = take(std::size_t{uvCount} * 8);
    take(u16() == 0 ? 0 : u32()); // tangents
    const std::uint32_t indexCount = u32(), indexSize = u32();
    const auto* deltas = take(indexSize);
    std::vector<std::uint32_t> lods;
    if (!bad && at < data.size()) {
        const std::uint8_t lodCount = static_cast<std::uint8_t>(data[at++]);
        for (std::uint32_t i = 0; i < lodCount; ++i)
            lods.push_back(u32());
    }
    if (bad)
        return std::unexpected("CSG mesh runs past the end");
    if (normalCount != count || normalSize != count * 6 || colorCount != count || uvCount != count)
        return std::unexpected("CSG mesh attribute counts disagree");

    MeshFile file;
    file.version = "CSGMDL 5";
    file.mesh.vertices.reserve(count);
    file.boundsMin = Vector3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    file.boundsMax = file.boundsMin * -1.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        MeshVertex v;
        const auto* p = positions + std::size_t{i} * 12;
        v.position = {f32(p), f32(p + 4), f32(p + 8)};
        const auto* n = normals + std::size_t{i} * 6;
        v.normal = {unit(n), unit(n + 2), unit(n + 4)};
        const auto* rgba = colors + std::size_t{i} * 4;
        v.color = Color3::fromRGB(rgba[0], rgba[1], rgba[2]);
        v.u = f32(uvs + std::size_t{i} * 8);
        v.v = f32(uvs + std::size_t{i} * 8 + 4);
        file.boundsMin = Vector3{std::min(file.boundsMin.x, v.position.x), std::min(file.boundsMin.y, v.position.y),
                                 std::min(file.boundsMin.z, v.position.z)};
        file.boundsMax = Vector3{std::max(file.boundsMax.x, v.position.x), std::max(file.boundsMax.y, v.position.y),
                                 std::max(file.boundsMax.z, v.position.z)};
        file.mesh.vertices.push_back(v);
    }

    // Each index is the previous one plus a delta: a byte under 0x80 is a
    // 7-bit signed delta, else it and the next two bytes are a 23-bit signed
    // one, big-endian.
    std::vector<std::uint32_t> indices;
    indices.reserve(indexCount);
    std::int64_t index = 0;
    for (std::size_t i = 0; i < indexSize && indices.size() < indexCount;) {
        const std::uint8_t b = deltas[i];
        std::int32_t delta;
        if (b < 0x80) {
            delta = b < 0x40 ? b : b - 0x80;
            i += 1;
        } else {
            if (i + 3 > indexSize)
                return std::unexpected("CSG mesh index delta runs past the end");
            const std::int32_t v = ((b & 0x7f) << 16) | (deltas[i + 1] << 8) | deltas[i + 2];
            delta = (v & 0x400000) != 0 ? v - 0x800000 : v;
            i += 3;
        }
        index += delta;
        if (index < 0 || index >= count)
            return std::unexpected("CSG mesh index out of range");
        indices.push_back(static_cast<std::uint32_t>(index));
    }
    if (indices.size() != indexCount)
        return std::unexpected("CSG mesh has fewer indices than it says");
    // The finest level of detail.
    std::size_t end = indexCount;
    if (lods.size() >= 2 && lods[0] == 0 && lods[1] <= indexCount)
        end = lods[1];
    if (end % 3 != 0)
        return std::unexpected("CSG mesh level of detail isn't whole triangles");
    indices.resize(end);
    file.mesh.indices = std::move(indices);
    return file;
}

} // namespace

std::expected<MeshFile, std::string> parseCsgMesh(std::string_view meshData)
{
    // The key, recovered from the data's 31-byte period [live asset].
    static constexpr std::array<std::uint8_t, 31> key{0x56, 0x2e, 0x6e, 0x58, 0x31, 0x20, 0x30, 0x04, 0x34, 0x69, 0x0c,
                                                     0x77, 0x0c, 0x01, 0x5e, 0x00, 0x1a, 0x60, 0x37, 0x69, 0x1d, 0x52,
                                                     0x2b, 0x07, 0x4f, 0x24, 0x59, 0x65, 0x53, 0x04, 0x7a};
    std::string plain(meshData);
    for (std::size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<char>(static_cast<std::uint8_t>(plain[i]) ^ key[i % key.size()]);
    if (!plain.starts_with("CSGMDL"))
        return std::unexpected("not a CSG mesh (no CSGMDL header)");
    // Version 5 keeps only the header XORed.
    if (plain.size() >= 10 && plain[6] == 5 && plain[7] == 0 && plain[8] == 0 && plain[9] == 0) {
        std::string data(meshData);
        std::memcpy(data.data(), plain.data(), 10);
        return parseCsgMesh5(data);
    }
    auto u32 = [&](std::size_t at) -> std::optional<std::uint32_t> {
        if (at + 4 > plain.size())
            return std::nullopt;
        std::uint32_t v;
        std::memcpy(&v, plain.data() + at, 4);
        return v;
    };
    auto f32 = [&](std::size_t at) {
        float v;
        std::memcpy(&v, plain.data() + at, 4);
        return v;
    };
    // Version 4 [live assets: the game's walls] is version 2 with wider vertices
    // (84 bytes: the same first 40, then 44 more, unread) and 12 bytes after
    // the indices.
    const auto version = u32(6);
    if (version != 2u && version != 4u)
        return std::unexpected(std::format("CSG mesh version {} isn't supported", version.value_or(0)));
    // "CSGMDL", u32 version, 16-character hash, 16 bytes, u32 count, u32 stride.
    const auto count = u32(42), stride = u32(46);
    constexpr std::size_t vertexStart = 50;
    if (!count || !stride || *stride < 28 || vertexStart + std::uint64_t{*count} * *stride + 4 > plain.size())
        return std::unexpected("CSG mesh vertices run past the end");
    MeshFile file;
    file.version = std::format("CSGMDL {}", *version);
    file.mesh.vertices.reserve(*count);
    file.boundsMin = Vector3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    file.boundsMax = file.boundsMin * -1.0f;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::size_t at = vertexStart + std::size_t{i} * *stride;
        MeshVertex v;
        v.position = {f32(at), f32(at + 4), f32(at + 8)};
        v.normal = {f32(at + 12), f32(at + 16), f32(at + 20)};
        const auto* rgba = reinterpret_cast<const std::uint8_t*>(plain.data() + at + 24);
        v.color = Color3::fromRGB(rgba[0], rgba[1], rgba[2]);
        if (*stride >= 40) {
            v.u = f32(at + 32);
            v.v = f32(at + 36);
        }
        file.boundsMin = Vector3{std::min(file.boundsMin.x, v.position.x), std::min(file.boundsMin.y, v.position.y),
                                 std::min(file.boundsMin.z, v.position.z)};
        file.boundsMax = Vector3{std::max(file.boundsMax.x, v.position.x), std::max(file.boundsMax.y, v.position.y),
                                 std::max(file.boundsMax.z, v.position.z)};
        file.mesh.vertices.push_back(v);
    }
    const std::size_t indexAt = vertexStart + std::size_t{*count} * *stride;
    const auto indexCount = u32(indexAt);
    if (!indexCount || indexAt + 4 + std::uint64_t{*indexCount} * 4 > plain.size() || *indexCount % 3 != 0)
        return std::unexpected("CSG mesh indices run past the end");
    file.mesh.indices.resize(*indexCount);
    std::memcpy(file.mesh.indices.data(), plain.data() + indexAt + 4, std::size_t{*indexCount} * 4);
    for (const std::uint32_t index : file.mesh.indices)
        if (index >= *count)
            return std::unexpected("CSG mesh index out of range");
    return file;
}

namespace {

// The parts in a union's ChildData (a model file of its operands). A
// NegateOperation is a cut; a nested union or a mesh has no primitive shape.
std::vector<UnionOperand> unionOperands(std::string_view childData)
{
    std::vector<UnionOperand> out;
    if (childData.empty())
        return out;
    auto model = loadModel(childData);
    if (!model)
        return out;
    for (const auto& root : model->roots) {
        const auto* part = dynamic_cast<const BasePart*>(root.get());
        if (!part)
            continue;
        UnionOperand operand{.cframe = part->cframe(), .size = part->size()};
        if (part->isA("NegateOperation")) {
            operand.negate = true;
        } else if (const auto* p = dynamic_cast<const Part*>(part)) {
            switch (p->shape()) {
            case Enum::PartType::Block: operand.kind = UnionOperand::Kind::Block; break;
            case Enum::PartType::Wedge: operand.kind = UnionOperand::Kind::Wedge; break;
            case Enum::PartType::CornerWedge: operand.kind = UnionOperand::Kind::CornerWedge; break;
            case Enum::PartType::Cylinder: operand.kind = UnionOperand::Kind::Cylinder; break;
            case Enum::PartType::Ball: operand.kind = UnionOperand::Kind::Ball; break;
            }
        } else if (part->isA("WedgePart")) {
            operand.kind = UnionOperand::Kind::Wedge;
        } else if (part->isA("CornerWedgePart")) {
            operand.kind = UnionOperand::Kind::CornerWedge;
        }
        out.push_back(operand);
    }
    return out;
}

} // namespace

std::expected<MeshFile, std::string> parseMesh(std::string_view bytes, const MeshParseOptions& options)
{
    // A union's asset: a model holding one PartOperationAsset. Any other
    // model is not a mesh.
    if (bytes.starts_with("<roblox"))
        if (auto model = loadModel(bytes))
            for (const auto& root : model->roots)
                if (const auto* asset = dynamic_cast<const PartOperationAsset*>(root.get())) {
                    auto file = parseCsgMesh(asset->meshData());
                    if (file)
                        file->operands = unionOperands(asset->childData());
                    return file;
                }
    if (!looksLikeMesh(bytes))
        return std::unexpected("not a mesh file (no \"version\" header)");
    const std::size_t lineEnd = bytes.find('\n');
    if (lineEnd == std::string_view::npos)
        return std::unexpected("mesh header has no end of line");
    std::string_view versionText = bytes.substr(8, lineEnd - 8);
    if (versionText.ends_with('\r'))
        versionText.remove_suffix(1);
    std::string version(versionText);

    if (version == "1.00" || version == "1.01")
        return parseText(bytes, lineEnd + 1, std::move(version));

    Reader in(bytes, lineEnd + 1);
    if (version == "2.00" || version == "3.00" || version == "3.01")
        return parseV2(in, std::move(version), options);
    if (version == "4.00" || version == "4.01" || version == "5.00")
        return parseV4(in, std::move(version), options);
    if (version == "6.00" || version == "7.00")
        return parseChunked(in, std::move(version), options);
    if (version.size() > 16)
        version = version.substr(0, 16) + "...";
    return std::unexpected(std::format("mesh version \"{}\" isn't supported", version));
}

} // namespace lunar::assets
