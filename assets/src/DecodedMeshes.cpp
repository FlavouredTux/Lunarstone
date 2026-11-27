#include "DecodedMeshes.hpp"

#include <array>
#include <cstring>
#include <format>
#include <span>
#include <type_traits>
#include <vector>

namespace lunar::assets {

namespace {

constexpr std::array<char, 8> magic{'L', 'U', 'N', 'A', 'R', 'M', 'S', 'H'};

// Vertices are written as they lie in memory; a build whose vertices lie
// otherwise writes and reads files of its own layout.
static_assert(std::is_trivially_copyable_v<MeshVertex>);
constexpr std::uint32_t vertexBytes = sizeof(MeshVertex);

class Writer {
public:
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void value(const T& v)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        bytes_.insert(bytes_.end(), p, p + sizeof v);
    }
    template <class T>
    void array(const std::vector<T>& values)
    {
        value(static_cast<std::uint64_t>(values.size()));
        const auto* p = reinterpret_cast<const std::uint8_t*>(values.data());
        bytes_.insert(bytes_.end(), p, p + values.size() * sizeof(T));
    }
    void text(std::string_view s)
    {
        value(static_cast<std::uint64_t>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }
    void vector3(Vector3 v)
    {
        value(v.x);
        value(v.y);
        value(v.z);
    }
    std::vector<std::uint8_t>& bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// Reads what Writer wrote, never past the end: a short or garbled file
// reads as bad.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    template <class T>
    T value()
    {
        T v{};
        if (bytes_.size() - at_ < sizeof v) {
            bad_ = true;
            return v;
        }
        std::memcpy(&v, bytes_.data() + at_, sizeof v);
        at_ += sizeof v;
        return v;
    }
    template <class T>
    void array(std::vector<T>& out)
    {
        const auto count = value<std::uint64_t>();
        if (bad_ || count > (bytes_.size() - at_) / sizeof(T)) {
            bad_ = true;
            return;
        }
        out.resize(static_cast<std::size_t>(count));
        std::memcpy(out.data(), bytes_.data() + at_, out.size() * sizeof(T));
        at_ += out.size() * sizeof(T);
    }
    std::string text()
    {
        const auto count = value<std::uint64_t>();
        if (bad_ || count > bytes_.size() - at_) {
            bad_ = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes_.data() + at_), static_cast<std::size_t>(count));
        at_ += s.size();
        return s;
    }
    Vector3 vector3()
    {
        const float x = value<float>();
        const float y = value<float>();
        const float z = value<float>();
        return {x, y, z};
    }
    bool bad() const { return bad_; }
    bool done() const { return at_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    bool bad_ = false;
};

} // namespace

DecodedMeshes::DecodedMeshes(const DerivedCache& cache, bool levelsOfDetail)
    : cache_(&cache), levelsOfDetail_(levelsOfDetail)
{
}

std::string DecodedMeshes::nameFor(std::string_view source) const
{
    return DerivedCache::name(source,
                              std::format("{}.v{}.{}.mesh", levelsOfDetail_ ? "lods" : "nolods", version, vertexBytes));
}

std::optional<MeshFile> DecodedMeshes::find(std::string_view source) const
{
    if (!cache_->enabled())
        return std::nullopt;
    auto file = cache_->open(nameFor(source));
    if (!file)
        return std::nullopt;
    Reader in(file->bytes());
    const auto fileMagic = in.value<std::array<char, 8>>();
    const auto fileVersion = in.value<std::uint32_t>();
    const auto fileVertexBytes = in.value<std::uint32_t>();
    const auto lods = in.value<std::uint8_t>();
    const auto key = in.value<std::array<std::uint8_t, 16>>();
    if (in.bad() || fileMagic != magic || fileVersion != version || fileVertexBytes != vertexBytes
        || lods != (levelsOfDetail_ ? 1 : 0) || key != DerivedCache::key(source))
        return std::nullopt;
    MeshFile mesh;
    mesh.version = in.text();
    in.array(mesh.mesh.vertices);
    in.array(mesh.mesh.indices);
    in.array(mesh.mesh.lodIndices);
    in.array(mesh.mesh.lodEnds);
    in.array(mesh.mesh.lodErrors);
    mesh.boundsMin = in.vector3();
    mesh.boundsMax = in.vector3();
    mesh.lodCount = static_cast<std::size_t>(in.value<std::uint64_t>());
    mesh.boneCount = static_cast<std::size_t>(in.value<std::uint64_t>());
    mesh.hasFacs = in.value<std::uint8_t>() != 0;
    const auto operands = in.value<std::uint64_t>();
    for (std::uint64_t i = 0; i < operands && !in.bad(); ++i) {
        UnionOperand operand;
        operand.kind = static_cast<UnionOperand::Kind>(in.value<std::uint8_t>());
        operand.cframe.position = in.vector3();
        for (auto& row : operand.cframe.r)
            for (float& cell : row)
                cell = in.value<float>();
        operand.size = in.vector3();
        operand.negate = in.value<std::uint8_t>() != 0;
        mesh.operands.push_back(operand);
    }
    if (in.bad() || !in.done())
        return std::nullopt;
    return mesh;
}

void DecodedMeshes::keep(std::string_view source, const MeshFile& mesh) const
{
    if (!cache_->enabled() || mesh.mesh.indices.size() / 3 < minimumTriangles)
        return;
    Writer out;
    out.value(magic);
    out.value(version);
    out.value(vertexBytes);
    out.value(static_cast<std::uint8_t>(levelsOfDetail_ ? 1 : 0));
    out.value(DerivedCache::key(source));
    out.text(mesh.version);
    out.array(mesh.mesh.vertices);
    out.array(mesh.mesh.indices);
    out.array(mesh.mesh.lodIndices);
    out.array(mesh.mesh.lodEnds);
    out.array(mesh.mesh.lodErrors);
    out.vector3(mesh.boundsMin);
    out.vector3(mesh.boundsMax);
    out.value(static_cast<std::uint64_t>(mesh.lodCount));
    out.value(static_cast<std::uint64_t>(mesh.boneCount));
    out.value(static_cast<std::uint8_t>(mesh.hasFacs ? 1 : 0));
    out.value(static_cast<std::uint64_t>(mesh.operands.size()));
    for (const UnionOperand& operand : mesh.operands) {
        out.value(static_cast<std::uint8_t>(operand.kind));
        out.vector3(operand.cframe.position);
        for (const auto& row : operand.cframe.r)
            for (float cell : row)
                out.value(cell);
        out.vector3(operand.size);
        out.value(static_cast<std::uint8_t>(operand.negate ? 1 : 0));
    }
    const std::array<std::span<const std::uint8_t>, 1> parts{std::span<const std::uint8_t>(out.bytes())};
    (void)cache_->write(nameFor(source), parts);
}

} // namespace lunar::assets
