#include <lunar/physics/CollisionData.hpp>

// The assets library's zstd decoder; v8 blobs are one zstd frame.
#include "Zstd.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
#include <limits>
#include <mdspan>
#include <span>
#include <type_traits>

namespace lunar::physics {

namespace {

static_assert(std::endian::native == std::endian::little, "collision data is little-endian");

template <class T>
using Result = std::expected<T, std::string>;

constexpr std::string_view magic = "CSGPHS";
constexpr std::size_t headerSize = 10;    // magic + u32 version
constexpr std::uint32_t versionCount = 9; // sub_102fac390 accepts versions below 9

// Bounds-checked little-endian reads.
class Reader {
public:
    explicit Reader(std::string_view data, std::size_t pos = 0) : data_(data), pos_(pos) {}

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool done() const { return pos_ >= data_.size(); }

    template <class T>
        requires std::is_trivially_copyable_v<T>
    Result<T> read(std::string_view what)
    {
        if (sizeof(T) > remaining())
            return std::unexpected(std::format("truncated {} at byte {}", what, pos_));
        T value{};
        data_.substr(pos_, sizeof(T)).copy(reinterpret_cast<char*>(&value), sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    // count u32-sized items, checked against what's left before anything is allocated.
    Result<std::string_view> words(std::uint64_t count, std::string_view what)
    {
        if (count > remaining() / 4)
            return std::unexpected(
                std::format("truncated {}: {} values at byte {}, {} bytes left", what, count, pos_, remaining()));
        std::string_view out = data_.substr(pos_, static_cast<std::size_t>(count) * 4);
        pos_ += out.size();
        return out;
    }

private:
    std::string_view data_;
    std::size_t pos_;
};

template <class T>
std::vector<T> copyWords(std::string_view bytes)
{
    std::vector<T> out(bytes.size() / sizeof(T));
    bytes.copy(reinterpret_cast<char*>(out.data()), out.size() * sizeof(T));
    return out;
}

// x y z float triples, viewed as rows.
std::vector<Vector3> toVertices(std::span<const float> floats)
{
    const std::mdspan<const float, std::extents<std::size_t, std::dynamic_extent, 3>> rows(floats.data(),
                                                                                           floats.size() / 3);
    std::vector<Vector3> vertices;
    vertices.reserve(rows.extent(0));
    for (std::size_t i = 0; i < rows.extent(0); ++i)
        vertices.push_back({rows[i, 0], rows[i, 1], rows[i, 2]});
    return vertices;
}

// The piece check of sub_103093754 (old) and sub_102fbe4c0 (v8): every index
// names a vertex.
Result<void> checkIndices(const CollisionPiece& piece, std::size_t number)
{
    if (piece.indices.size() % 3 != 0)
        return std::unexpected(
            std::format("piece {}: {} indices aren't whole triangles", number, piece.indices.size()));
    for (std::uint32_t index : piece.indices)
        if (index >= piece.vertices.size())
            return std::unexpected(
                std::format("piece {}: index {} out of {} vertices", number, index, piece.vertices.size()));
    return {};
}

void computeBounds(CollisionData& data)
{
    Vector3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vector3 hi = -lo;
    for (const CollisionPiece& piece : data.pieces)
        for (const Vector3& v : piece.vertices) {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        }
    if (lo.x <= hi.x) {
        data.boundsMin = lo;
        data.boundsMax = hi;
    }
}

// PhysicsMeshDecoderImplOld (sub_102facb6e): versions 1 to 7.
Result<void> decodeOld(Reader& r, CollisionData& data)
{
    if (data.version == 7) {
        auto type = r.read<std::uint8_t>("geometry type");
        if (!type)
            return std::unexpected(type.error());
        if (*type > 7)
            return std::unexpected(std::format("geometry type {} out of range", *type));
        data.geometryType = *type;
    }
    if (data.version == 6 || data.version == 7) {
        auto mass = r.read<std::array<float, 10>>("mass properties");
        if (!mass)
            return std::unexpected(mass.error());
        const auto& m = *mass;
        data.mass = CollisionMassProperties{m[0], {m[1], m[2], m[3]}, {m[4], m[5], m[6], m[7], m[8], m[9]}};
    }
    // ClosedVoxel(_Dynamic) decompositions use a voxel encoding of their own
    // (sub_10304fbcc and friends), not pieces.
    if (data.version == 7
        && (data.geometryType == std::to_underlying(CollisionGeometryType::ClosedVoxelDynamic)
            || data.geometryType == std::to_underlying(CollisionGeometryType::ClosedVoxel)))
        return std::unexpected("ClosedVoxel collision data isn't supported yet");

    while (!r.done()) {
        const std::size_t number = data.pieces.size();
        auto stride = r.read<std::uint32_t>("translation stride");
        if (!stride)
            return std::unexpected(stride.error());
        if (*stride != 16)
            return std::unexpected(std::format("piece {}: translation stride {}, expected 16", number, *stride));
        if (auto t = r.words(4, "translation"); !t)
            return std::unexpected(t.error());
        stride = r.read<std::uint32_t>("rotation stride");
        if (!stride)
            return std::unexpected(stride.error());
        if (*stride != 16)
            return std::unexpected(std::format("piece {}: rotation stride {}, expected 16", number, *stride));
        if (auto q = r.words(4, "rotation"); !q)
            return std::unexpected(q.error());

        auto floatCount = r.read<std::uint32_t>("vertex count");
        if (!floatCount)
            return std::unexpected(floatCount.error());
        stride = r.read<std::uint32_t>("vertex stride");
        if (!stride)
            return std::unexpected(stride.error());
        if (*stride != 4)
            return std::unexpected(std::format("piece {}: vertex stride {}, expected 4", number, *stride));
        if (*floatCount % 3 != 0)
            return std::unexpected(std::format("piece {}: {} floats aren't whole vertices", number, *floatCount));
        auto floats = r.words(*floatCount, "vertices");
        if (!floats)
            return std::unexpected(floats.error());
        auto indexCount = r.read<std::uint32_t>("index count");
        if (!indexCount)
            return std::unexpected(indexCount.error());
        auto indices = r.words(*indexCount, "indices");
        if (!indices)
            return std::unexpected(indices.error());

        CollisionPiece piece{toVertices(copyWords<float>(*floats)), copyWords<std::uint32_t>(*indices)};
        if (auto ok = checkIndices(piece, number); !ok)
            return std::unexpected(ok.error());
        data.pieces.push_back(std::move(piece));
    }
    if (data.version <= 6) // what the old decoder reports without a stored type
        data.geometryType = static_cast<std::uint8_t>(std::min<std::size_t>(data.pieces.size(), 2));
    computeBounds(data);
    return {};
}

// Frame_Content_Size of a zstd frame (RFC 8878 3.1.1.1); the client refuses
// frames without one (sub_103205ff2 failing is error -5).
std::optional<std::uint64_t> zstdContentSize(std::string_view frame)
{
    if (frame.size() < 5 || !frame.starts_with("\x28\xb5\x2f\xfd"))
        return std::nullopt;
    const auto descriptor = static_cast<std::uint8_t>(frame[4]);
    const unsigned sizeFlag = descriptor >> 6;
    const bool singleSegment = (descriptor >> 5) & 1;
    const unsigned dictionaryFlag = descriptor & 3;
    const std::size_t sizeBytes = sizeFlag == 0 ? (singleSegment ? 1 : 0) : std::size_t{1} << sizeFlag;
    if (sizeBytes == 0)
        return std::nullopt;
    const std::size_t at = 5 + (singleSegment ? 0 : 1) + (dictionaryFlag == 3 ? 4 : dictionaryFlag);
    if (frame.size() < at + sizeBytes)
        return std::nullopt;
    std::uint64_t size = 0;
    for (std::size_t i = 0; i < sizeBytes; ++i)
        size |= std::uint64_t{static_cast<std::uint8_t>(frame[at + i])} << (8 * i);
    return sizeBytes == 2 ? size + 256 : size;
}

// One ragged array of a v8 multimesh (sub_102fbe258): u32 n, n offsets that
// start at 0 and never go down, then offsets[n - 1] u32 of data.
struct Ragged {
    std::vector<std::uint32_t> offsets;
    std::string_view data;

    std::size_t parts() const { return offsets.size() < 2 ? 0 : offsets.size() - 1; }
    std::string_view part(std::size_t i) const
    {
        return data.substr(std::size_t{offsets[i]} * 4, std::size_t{offsets[i + 1] - offsets[i]} * 4);
    }
};

Result<Ragged> readRagged(Reader& r, std::string_view what)
{
    Ragged ragged;
    if (r.done())
        return ragged;
    auto count = r.read<std::uint32_t>(what);
    if (!count)
        return std::unexpected(count.error());
    auto offsets = r.words(*count, what);
    if (!offsets)
        return std::unexpected(offsets.error());
    ragged.offsets = copyWords<std::uint32_t>(*offsets);
    for (std::size_t i = 0; i < ragged.offsets.size(); ++i)
        if ((i == 0 && ragged.offsets[i] != 0) || (i > 0 && ragged.offsets[i] < ragged.offsets[i - 1]))
            return std::unexpected(std::format("{}: bad offset table", what));
    if (!ragged.offsets.empty()) {
        auto data = r.words(ragged.offsets.back(), what);
        if (!data)
            return std::unexpected(data.error());
        ragged.data = *data;
    }
    return ragged;
}

// Section B: bits in u32 words, most significant first, except that the
// last word holds its bits at the bottom (sub_102fbd6ac's index math).
class Bits {
public:
    Bits(std::string_view words, std::uint64_t count) : words_(copyWords<std::uint32_t>(words)), count_(count) {}

    std::optional<bool> next()
    {
        if (pos_ >= count_)
            return std::nullopt;
        const std::uint64_t word = pos_ / 32;
        const std::uint64_t width = word + 1 == words_.size() ? count_ - word * 32 : 32;
        const std::uint64_t within = pos_ % 32;
        ++pos_;
        if (within >= width)
            return std::nullopt; // "Bitvec: left shift by a negative val"
        return ((words_[static_cast<std::size_t>(word)] >> (width - 1 - within)) & 1u) != 0;
    }

    // Edgebreaker symbol: 0 is C, then 1 followed by two bits: 4 S, 5 L, 6 R, 7 E.
    std::optional<int> symbol()
    {
        auto first = next();
        if (!first)
            return std::nullopt;
        if (!*first)
            return 0;
        if (pos_ + 2 > count_)
            return std::nullopt;
        auto b1 = next();
        auto b2 = next();
        if (!b1 || !b2)
            return std::nullopt;
        return 4 | (*b1 ? 2 : 0) | (*b2 ? 1 : 0);
    }

private:
    std::vector<std::uint32_t> words_;
    std::uint64_t count_;
    std::uint64_t pos_ = 0;
};

// PhysicsMeshDecoderImplV8 (sub_102fadf34).
Result<void> decodeV8(std::string_view bytes, CollisionData& data)
{
    if (bytes.size() < 12)
        return std::unexpected("v8 header is truncated");
    data.geometryType = static_cast<std::uint8_t>(bytes[10]);
    if (data.geometryType > 7)
        return std::unexpected(std::format("geometry type {} out of range", data.geometryType));
    const std::string_view frame = bytes.substr(12);
    const auto contentSize = zstdContentSize(frame);
    if (!contentSize || *contentSize == 0)
        return std::unexpected("v8 data has no zstd content size");
    // The header's sizes are u32s, so nothing real is bigger than this.
    constexpr std::uint64_t limit = std::uint64_t{1} << 30;
    if (*contentSize > limit)
        return std::unexpected("v8 data is implausibly large");
    auto inflated
        = assets::detail::zstdDecompress(std::span(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()),
                                         static_cast<std::size_t>(*contentSize));
    if (!inflated)
        return std::unexpected(std::format("v8 data: {}", inflated.error()));
    if (inflated->size() != *contentSize)
        return std::unexpected("v8 data is shorter than its zstd content size");
    const std::string_view body(reinterpret_cast<const char*>(inflated->data()), inflated->size());

    Reader r(body);
    auto header = r.read<std::array<std::uint32_t, 9>>("v8 header");
    if (!header)
        return std::unexpected(header.error());
    auto bounds = r.read<std::array<float, 6>>("v8 bounds");
    if (!bounds)
        return std::unexpected(bounds.error());
    const auto [extraPieces, extraVertices, extraTriangles, maxPieceVertices, maxPieceTriangles, sizeA, bitCount, sizeB,
                sizeC]
        = *header;
    if (std::uint64_t{sizeA} + sizeB + sizeC != r.remaining())
        return std::unexpected("v8 section sizes don't add up");
    if (sizeA % 4 || sizeB % 4 || sizeC % 4)
        return std::unexpected("v8 sections aren't aligned");
    if ((std::uint64_t{bitCount} + 31) / 32 > sizeB / 4)
        return std::unexpected("v8 bit section is too short");
    const auto& b = *bounds;
    data.boundsMin = {b[0], b[1], b[2]};
    data.boundsMax = {b[3], b[4], b[5]};

    const std::string_view sectionA = body.substr(r.pos(), sizeA);
    const std::string_view sectionB = body.substr(r.pos() + sizeA, sizeB);
    const std::string_view sectionC = body.substr(r.pos() + sizeA + sizeB, sizeC);

    // A: indices, then vertex floats, one ragged array each.
    Reader a(sectionA);
    auto indices = readRagged(a, "piece indices");
    if (!indices)
        return std::unexpected(indices.error());
    auto vertices = readRagged(a, "piece vertices");
    if (!vertices)
        return std::unexpected(vertices.error());
    if (!a.done())
        return std::unexpected("v8 piece section has bytes left over");
    if (indices->offsets.size() != vertices->offsets.size())
        return std::unexpected("v8 piece tables disagree");
    for (std::size_t i = 0; i < vertices->parts(); ++i) {
        const std::string_view floats = vertices->part(i);
        if (floats.size() % 12 != 0)
            return std::unexpected(std::format("piece {}: floats aren't whole vertices", i));
        CollisionPiece piece{toVertices(copyWords<float>(floats)), copyWords<std::uint32_t>(indices->part(i))};
        if (auto ok = checkIndices(piece, i); !ok)
            return std::unexpected(ok.error());
        data.pieces.push_back(std::move(piece));
    }

    // B and C: pieces whose triangles are Edgebreaker symbols. Each run
    // starts with one triangle (3 vertices, depth 1) and each symbol adds
    // one; C adds a vertex, S opens a branch, E closes one; the piece ends
    // when depth reaches 0 (sub_102fbd9c2). Their vertices follow each other
    // in C. [live: on 25 distinct blobs every bit and vertex is used, and
    // the triangles come to the header's total]
    Bits bits(sectionB, bitCount);
    const std::vector<float> floats = copyWords<float>(sectionC);
    std::uint64_t vertexAt = 0;
    std::uint64_t triangles = 0;
    for (std::uint32_t k = 0; k < extraPieces; ++k) {
        std::uint64_t count = 3;
        std::uint64_t pieceTriangles = 1;
        int depth = 1;
        while (depth > 0) {
            auto symbol = bits.symbol();
            if (!symbol)
                return std::unexpected(std::format("extra piece {}: symbols run out", k));
            if (++pieceTriangles > maxPieceTriangles)
                return std::unexpected(std::format("extra piece {}: more triangles than the header allows a piece", k));
            if (*symbol == 0)
                ++count;
            else if (*symbol == 4)
                ++depth;
            else if (*symbol == 7)
                --depth;
        }
        triangles += pieceTriangles;
        if (triangles > extraTriangles)
            return std::unexpected(std::format("extra piece {}: more triangles than the header says", k));
        if (count > maxPieceVertices)
            return std::unexpected(std::format("extra piece {}: more vertices than the header allows a piece", k));
        if (vertexAt + count > extraVertices || (vertexAt + count) * 3 > floats.size())
            return std::unexpected(std::format("extra piece {}: vertices run out", k));
        data.pieces.push_back({toVertices(std::span(floats).subspan(static_cast<std::size_t>(vertexAt * 3),
                                                                    static_cast<std::size_t>(count * 3))),
                               {}});
        vertexAt += count;
    }
    return {};
}

std::uint32_t readVersion(std::string_view bytes)
{
    std::uint32_t version = 0;
    bytes.substr(magic.size(), sizeof version).copy(reinterpret_cast<char*>(&version), sizeof version);
    return version;
}

} // namespace

bool isCollisionData(std::string_view bytes)
{
    if (bytes.size() < headerSize || !bytes.starts_with(magic))
        return false;
    return readVersion(bytes) < versionCount;
}

std::expected<CollisionData, std::string> decodeCollisionData(std::string_view bytes)
{
    if (bytes.size() < headerSize)
        return std::unexpected("empty or incomplete header");
    if (!bytes.starts_with(magic))
        return std::unexpected("header identifier mismatch");
    CollisionData data;
    data.version = readVersion(bytes);
    if (data.version >= versionCount)
        return std::unexpected(std::format("version {} not in acceptable range", data.version));

    if (data.version == 0) {
        // "CSGPHS" 0 alone or followed by "BLOCK": the box (sub_102fac16c,
        // sub_102fabee8). Anything else in a v0 blob is an error to the old
        // decoder (-8), which also leaves the part a box.
        if (bytes.size() == headerSize || bytes.substr(headerSize).starts_with("BLOCK")) {
            data.block = true;
            return data;
        }
        return std::unexpected("version 0 data that isn't BLOCK");
    }
    if (data.version == 8) {
        if (auto ok = decodeV8(bytes, data); !ok)
            return std::unexpected(ok.error());
        return data;
    }
    Reader r(bytes, headerSize);
    if (auto ok = decodeOld(r, data); !ok)
        return std::unexpected(ok.error());
    return data;
}

} // namespace lunar::physics
