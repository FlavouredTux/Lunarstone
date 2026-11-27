#include "framework.hpp"

#include <lunar/assets/MeshFile.hpp>

#include <array>
#include <format>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::assets;
using namespace std::string_literals;

namespace {

// Little-endian mesh bytes, built field by field.
class Bytes {
public:
    template <class T>
    Bytes& put(T value)
    {
        char raw[sizeof(T)];
        std::memcpy(raw, &value, sizeof(T));
        bytes_.append(raw, sizeof(T));
        return *this;
    }
    Bytes& u8(std::uint8_t v) { return put(v); }
    Bytes& u16(std::uint16_t v) { return put(v); }
    Bytes& u32(std::uint32_t v) { return put(v); }
    Bytes& f32(float v) { return put(v); }
    Bytes& text(std::string_view t)
    {
        bytes_ += t;
        return *this;
    }
    Bytes& zeros(std::size_t count)
    {
        bytes_.append(count, '\0');
        return *this;
    }
    Bytes& chunk(std::string_view type, std::uint32_t version, const std::string& data)
    {
        std::string name(type);
        name.resize(8, '\0');
        return text(name).u32(version).u32(static_cast<std::uint32_t>(data.size())).text(data);
    }
    const std::string& str() const { return bytes_; }

private:
    std::string bytes_;
};

// A tetrahedron with its faces wound counter-clockwise from outside.
constexpr std::array<std::array<float, 3>, 4> corners{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
constexpr std::array<std::array<std::uint32_t, 3>, 4> faces{{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}};

void vertex40(Bytes& out, std::size_t i)
{
    const auto& p = corners[i];
    out.f32(p[0]).f32(p[1]).f32(p[2]);
    out.f32(0).f32(1).f32(0);
    out.f32(static_cast<float>(i) * 0.25f).f32(0.5f);
    out.u8(127).u8(0).u8(0).u8(127);                                 // tangent
    out.u8(255).u8(static_cast<std::uint8_t>(i * 60)).u8(0).u8(255); // colour
}

// Vertices 0-3 are the tetrahedron, vertex 4 is junk only a lower LOD uses.
void vertices40(Bytes& out, bool withJunk)
{
    for (std::size_t i = 0; i < 4; ++i)
        vertex40(out, i);
    if (withJunk) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        out.f32(nan).f32(nan).f32(nan).zeros(28);
    }
}

void faces12(Bytes& out, bool withLod1)
{
    for (const auto& f : faces)
        out.u32(f[0]).u32(f[1]).u32(f[2]);
    if (withLod1)
        out.u32(0).u32(4).u32(1);
}

std::string version1(std::string_view version)
{
    std::string out = "version "s + std::string(version) + "\r\n4\r\n";
    for (const auto& f : faces)
        for (std::uint32_t i : f) {
            const auto& p = corners[i];
            out += std::format("[{},{},{}][0,1,0][{},0.25,0]", p[0] * 2, p[1] * 2, p[2] * 2, i);
        }
    return out;
}

std::string version2(std::uint8_t vertexSize)
{
    Bytes out;
    out.text("version 2.00\n").u16(12).u8(vertexSize).u8(12).u32(4).u32(4);
    if (vertexSize == 36) {
        for (const auto& p : corners)
            out.f32(p[0]).f32(p[1]).f32(p[2]).f32(0).f32(1).f32(0).f32(0.5f).f32(0.5f).f32(0);
    } else {
        vertices40(out, false);
    }
    faces12(out, false);
    return out.str();
}

std::string version3()
{
    Bytes out;
    out.text("version 3.00\n").u16(16).u8(40).u8(12).u16(4).u16(3).u32(5).u32(5);
    vertices40(out, true);
    faces12(out, true);
    out.u32(0).u32(4).u32(5);
    return out.str();
}

// 4.00 and 4.01 share a layout; 5.00 adds FACS after the subsets.
std::string version45(std::string_view version, bool bones)
{
    const bool facs = version == "5.00";
    const std::string names = bones ? "Root\0Head\0"s : ""s;
    Bytes out;
    out.text("version ").text(version).text("\n");
    out.u16(facs ? 32 : 24).u16(0).u32(5).u32(5).u16(3).u16(bones ? 2 : 0);
    out.u32(static_cast<std::uint32_t>(names.size())).u16(bones ? 1 : 0).u8(1).u8(0);
    if (facs)
        out.u32(0).u32(12);
    vertices40(out, true);
    if (bones)
        out.zeros(40); // five envelopes
    faces12(out, true);
    out.u32(0).u32(4).u32(5);
    if (bones)
        out.zeros(120).text(names).zeros(72); // two bones, names, one subset
    if (facs)
        out.zeros(12);
    return out.str();
}

std::string coreMeshV1()
{
    Bytes core;
    core.u32(5);
    vertices40(core, true);
    core.u32(5);
    faces12(core, true);
    return core.str();
}

std::string lodsChunk()
{
    return Bytes().u16(0).u8(1).u32(3).u32(0).u32(4).u32(5).str();
}

std::string version6()
{
    Bytes skinning;
    skinning.u32(5).zeros(40).u32(2).zeros(120).u32(5).text("Root\0"s).u32(0);
    return Bytes()
        .text("version 6.00\n")
        .chunk("COREMESH", 1, coreMeshV1())
        .chunk("LODS", 1, lodsChunk())
        .chunk("SKINNING", 1, skinning.str())
        .chunk("FACS", 1, Bytes().u32(4).zeros(4).str())
        .chunk("HSRAVIS", 1, Bytes().u32(1).u8(0xff).str())
        .str();
}

// A tetrahedron (as a triangle soup of 12 points) encoded by Draco 1.5 the
// way Roblox lays it out: position, the normal as a generic float3, UV and an
// RGBA colour (200, 100, 50).
const std::string dracoTetrahedron
    = "\x44\x52\x41\x43\x4f\x02\x02\x01\x00\x00\x00\x04\x0c\x01\x00\x01\x02\x03\x04\x05" "\x06\x07\x08" "\x09\x0a\x0b" "\x01" "\x04" "\x00" "\x09" "\x03" "\x00" "\x00" "\x04" "\x09\x03\x00\x01\x03\x09" "\x02\x00\x02\x02\x02\x04\x00\x03\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x80\x3f" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00" "\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\xbf" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x80\xbf" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\xbf\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\xbf\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f" "\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x80\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x01\x01\x01" "\x02\x70\x05\x3c\x55\x01\xff\x87\x55\x01\x27\x55\x01\x05\x6b\x96\xbb\xff\x86\x32" "\x00\x00\x00\xff\x00\x00\x00"s;

// Roblox's writer puts the bitstream's length in front of it.
std::string version7()
{
    Bytes core;
    core.u32(static_cast<std::uint32_t>(dracoTetrahedron.size())).text(dracoTetrahedron);
    return Bytes()
        .text("version 7.00\n")
        .chunk("COREMESH", 2, core.str())
        .chunk("LODS", 1, Bytes().u16(0).u8(1).u32(2).u32(0).u32(0).str())
        .str();
}

// Signed volume of a closed mesh: positive when faces wind counter-clockwise
// seen from outside.
double volume(const MeshData& mesh)
{
    double sum = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vector3 a = mesh.vertices[mesh.indices[i]].position;
        const Vector3 b = mesh.vertices[mesh.indices[i + 1]].position;
        const Vector3 c = mesh.vertices[mesh.indices[i + 2]].position;
        sum += static_cast<double>(a.dot(b.cross(c)));
    }
    return sum / 6.0;
}

// What every parsed tetrahedron has in common.
void checkTetrahedron(const MeshFile& file)
{
    CHECK_EQ(file.mesh.indices.size(), 12u);
    CHECK(file.boundsMin == (Vector3{0, 0, 0}));
    CHECK(file.boundsMax == (Vector3{1, 1, 1}));
    CHECK(file.size() == (Vector3{1, 1, 1}));
    CHECK(std::abs(volume(file.mesh) - 1.0 / 6.0) < 1e-6);
    for (std::uint32_t index : file.mesh.indices)
        CHECK(index < file.mesh.vertices.size());
}

std::vector<std::pair<std::string, std::string>> samples()
{
    std::vector<std::pair<std::string, std::string>> out{
        {"1.00", version1("1.00")},
        {"1.01", version1("1.01")},
        {"2.00/36", version2(36)},
        {"2.00/40", version2(40)},
        {"3.00", version3()},
        {"4.00", version45("4.00", false)},
        {"4.01", version45("4.01", true)},
        {"5.00", version45("5.00", true)},
        {"6.00", version6()},
    };
    if (meshDracoSupported())
        out.emplace_back("7.00", version7());
    return out;
}

} // namespace

TEST(mesh_v1_text)
{
    auto half = parseMesh(version1("1.00"));
    CHECK(half.has_value());
    CHECK_EQ(half->version, "1.00");
    // 1.00 is stored at twice its size; 1.01 isn't.
    checkTetrahedron(*half);
    auto full = parseMesh(version1("1.01"));
    CHECK(full.has_value());
    CHECK(full->size() == (Vector3{2, 2, 2}));
    // V is flipped to match the binary versions.
    CHECK_EQ(half->mesh.vertices[0].v, 0.75f);
    CHECK_EQ(half->mesh.vertices[1].u, 2.0f);
}

TEST(mesh_v2_vertex_layouts)
{
    auto plain = parseMesh(version2(36));
    CHECK(plain.has_value());
    checkTetrahedron(*plain);
    CHECK(plain->mesh.vertices[0].color == (Color3{1, 1, 1}));

    auto coloured = parseMesh(version2(40));
    CHECK(coloured.has_value());
    checkTetrahedron(*coloured);
    CHECK_EQ(coloured->mesh.vertices.size(), 4u);
    const MeshVertex& v = coloured->mesh.vertices[1];
    CHECK(v.color == Color3::fromRGB(255, 60, 0));
    CHECK(v.normal == (Vector3{0, 1, 0}));
    CHECK_EQ(v.u, 0.25f);
    CHECK_EQ(v.v, 0.5f);
}

TEST(mesh_v3_keeps_lod0_only)
{
    auto file = parseMesh(version3());
    CHECK(file.has_value());
    checkTetrahedron(*file);
    CHECK_EQ(file->lodCount, 2u);
    // The NaN vertex only the second LOD uses is dropped.
    CHECK_EQ(file->mesh.vertices.size(), 4u);
}

namespace {

// A 3.00 file whose second LOD is real: one face over vertices 0, 1 and a
// vertex of its own (4), then a third LOD the table cuts off as junk.
std::string version3WithLods()
{
    Bytes out;
    out.text("version 3.00\n").u16(16).u8(40).u8(12).u16(4).u16(4).u32(6).u32(6);
    for (std::size_t i = 0; i < 4; ++i)
        vertex40(out, i);
    out.f32(0.5f).f32(0.5f).f32(-1.0f).f32(0).f32(1).f32(0).f32(0).f32(0).zeros(8); // below LOD 0's box
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.f32(nan).f32(nan).f32(nan).zeros(28);
    faces12(out, false);
    out.u32(0).u32(1).u32(4);
    out.u32(4).u32(5).u32(0);
    out.u32(0).u32(4).u32(5).u32(6);
    return out.str();
}

} // namespace

TEST(mesh_v3_keeps_coarser_lods_when_asked)
{
    auto lod0 = parseMesh(version3WithLods());
    CHECK(lod0.has_value());
    checkTetrahedron(*lod0);
    CHECK_EQ(lod0->lodCount, 3u);
    CHECK_EQ(lod0->mesh.levels(), 1u);
    CHECK_EQ(lod0->mesh.vertices.size(), 4u);

    auto file = parseMesh(version3WithLods(), {.keepLods = true});
    CHECK(file.has_value());
    checkTetrahedron(*file); // LOD 0 and its bounds are as before
    CHECK_EQ(file->lodCount, 3u);
    CHECK_EQ(file->mesh.levels(), 2u); // the NaN level is dropped
    CHECK_EQ(file->mesh.vertices.size(), 5u);
    CHECK(file->mesh.vertices[4].position == (Vector3{0.5f, 0.5f, -1.0f}));
    CHECK(file->mesh.lodEnds == (std::vector<std::uint32_t>{3}));
    CHECK(file->mesh.lodIndices == (std::vector<std::uint32_t>{0, 1, 4}));
}

TEST(mesh_coarser_lods_leave_out_a_few_junk_faces)
{
    // The second LOD is 21 good faces and one over the NaN vertex (5): that
    // one face goes, the level stays.
    Bytes out;
    out.text("version 3.00\n").u16(16).u8(40).u8(12).u16(4).u16(3).u32(6).u32(26);
    for (std::size_t i = 0; i < 4; ++i)
        vertex40(out, i);
    out.f32(0.5f).f32(0.5f).f32(-1.0f).f32(0).f32(1).f32(0).f32(0).f32(0).zeros(8);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.f32(nan).f32(nan).f32(nan).zeros(28);
    faces12(out, false);
    for (int i = 0; i < 21; ++i)
        out.u32(0).u32(1).u32(4);
    out.u32(0).u32(4).u32(5);
    out.u32(0).u32(4).u32(26);
    auto file = parseMesh(out.str(), {.keepLods = true});
    CHECK(file.has_value());
    CHECK_EQ(file->mesh.levels(), 2u);
    CHECK_EQ(file->mesh.vertices.size(), 5u);
    CHECK(file->mesh.lodEnds == (std::vector<std::uint32_t>{63}));
}

TEST(mesh_v4_and_v5_skip_skinning_and_facs)
{
    for (auto [version, bones] : {std::pair{"4.00", false}, {"4.01", true}, {"5.00", true}}) {
        auto file = parseMesh(version45(version, bones));
        CHECK(file.has_value());
        CHECK_EQ(file->version, version);
        checkTetrahedron(*file);
        CHECK_EQ(file->lodCount, 2u);
        CHECK_EQ(file->boneCount, bones ? 2u : 0u);
        CHECK_EQ(file->hasFacs, version == "5.00"s);
    }
}

TEST(mesh_v6_chunks)
{
    auto file = parseMesh(version6());
    CHECK(file.has_value());
    checkTetrahedron(*file);
    CHECK_EQ(file->lodCount, 2u);
    CHECK_EQ(file->boneCount, 2u);
    CHECK(file->hasFacs);
    CHECK(file->mesh.vertices[2].color == Color3::fromRGB(255, 120, 0));

    // Without a COREMESH there's nothing to draw.
    CHECK_EQ(parseMesh(Bytes().text("version 6.00\n").chunk("LODS", 1, lodsChunk()).str()).error(),
             "no COREMESH chunk");
}

TEST(mesh_v7_draco)
{
    if (!meshDracoSupported()) {
        CHECK(!parseMesh(version7()).has_value());
        SKIP("built without Draco");
    }
    auto file = parseMesh(version7());
    if (!file)
        lt::fail(file.error(), std::source_location::current());
    checkTetrahedron(*file);
    CHECK_EQ(file->lodCount, 1u);
    const MeshVertex& v = file->mesh.vertices[0];
    CHECK(v.color == Color3::fromRGB(200, 100, 50));
    // The generic float3 attribute is the normal, and points out of the face.
    CHECK(std::abs(v.normal.magnitude() - 1.0f) < 1e-5f);
    const Vector3 a = file->mesh.vertices[file->mesh.indices[0]].position;
    const Vector3 b = file->mesh.vertices[file->mesh.indices[1]].position;
    const Vector3 c = file->mesh.vertices[file->mesh.indices[2]].position;
    CHECK((b - a).cross(c - a).dot(file->mesh.vertices[file->mesh.indices[0]].normal) > 0.0f);

    // An unprefixed bitstream works too.
    auto bare = parseMesh(Bytes().text("version 7.00\n").chunk("COREMESH", 2, dracoTetrahedron).str());
    CHECK(bare.has_value());
}

TEST(mesh_rejects_bad_files)
{
    CHECK_EQ(parseMesh("").error(), "not a mesh file (no \"version\" header)");
    CHECK_EQ(parseMesh("<roblox!").error(), "not a mesh file (no \"version\" header)");
    CHECK_EQ(parseMesh("version 2.00").error(), "mesh header has no end of line");
    CHECK_EQ(parseMesh("version 9.99\n").error(), "mesh version \"9.99\" isn't supported");
    CHECK(!parseMesh("version 1.00\nlots\n").has_value());
    CHECK(!parseMesh("version 1.00\n999999999\n[1,2,3]").has_value());

    // An index past the vertices.
    std::string bad = version2(40);
    bad[bad.size() - 4] = 9;
    CHECK_EQ(parseMesh(bad).error(), "face 3 references vertex 9 of 4");

    // Counts far larger than the file.
    Bytes huge;
    huge.text("version 2.00\n").u16(12).u8(40).u8(12).u32(0xffffffff).u32(0xffffffff);
    CHECK(!parseMesh(huge.str()).has_value());
    Bytes chunkTooBig;
    chunkTooBig.text("version 6.00\nCOREMESH").u32(1).u32(0xffffffff);
    CHECK(!parseMesh(chunkTooBig.str()).has_value());

    // A NaN in a vertex LOD 0 uses.
    std::string nan = version2(40);
    const float bits = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nan.data() + 13 + 12, &bits, 4);
    CHECK_EQ(parseMesh(nan).error(), "vertex 0 isn't a finite number");
}

TEST(mesh_truncated_files_fail_cleanly)
{
    for (const auto& [name, bytes] : samples()) {
        CHECK(parseMesh(bytes).has_value());
        for (std::size_t length = 0; length < bytes.size(); ++length)
            // Chunked files may end after any whole chunk.
            if (parseMesh(std::string_view(bytes).substr(0, length)) && !bytes.substr(length).starts_with("LODS")
                && !bytes.substr(length).starts_with("SKINNING") && !bytes.substr(length).starts_with("FACS")
                && !bytes.substr(length).starts_with("HSRAVIS"))
                lt::fail(std::format("{} cut to {} bytes still parsed", name, length), std::source_location::current());
    }
}

TEST(mesh_corrupted_files_never_crash)
{
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (const auto& [name, bytes] : samples()) {
        for (int round = 0; round < 3000; ++round) {
            std::string corrupted = bytes;
            const int flips = 1 + static_cast<int>(next() % 4);
            for (int i = 0; i < flips; ++i)
                corrupted[next() % corrupted.size()] = static_cast<char>(next());
            auto file = parseMesh(corrupted);
            if (!file)
                continue;
            for (std::uint32_t index : file->mesh.indices)
                CHECK(index < file->mesh.vertices.size());
            for (const MeshVertex& v : file->mesh.vertices)
                CHECK(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z));
        }
    }
}

// A union's CSG mesh (CSGMDL version 2), built by hand and XORed with the
// key the way Roblox ships it: one triangle, vertex colours, indices.
// Version 4 (the game's walls) is laid out the same, with 12 bytes after the
// indices.
TEST(assets_csg_mesh_v2_and_v4)
{
    static constexpr std::array<std::uint8_t, 31> key{0x56, 0x2e, 0x6e, 0x58, 0x31, 0x20, 0x30, 0x04, 0x34, 0x69, 0x0c,
                                                     0x77, 0x0c, 0x01, 0x5e, 0x00, 0x1a, 0x60, 0x37, 0x69, 0x1d, 0x52,
                                                     0x2b, 0x07, 0x4f, 0x24, 0x59, 0x65, 0x53, 0x04, 0x7a};
    for (const std::uint32_t version : {2u, 4u}) {
        std::string plain = "CSGMDL";
        auto u32 = [&](std::uint32_t v) { plain.append(reinterpret_cast<const char*>(&v), 4); };
        auto f32 = [&](float v) { plain.append(reinterpret_cast<const char*>(&v), 4); };
        u32(version);
        plain += "0123456789abcdef";
        plain += std::string(16, '\x07');
        u32(3);  // vertices
        u32(84); // stride
        const float corners[3][3] = {{0, 0, 0}, {2, 0, 0}, {0, 4, -1}};
        for (const auto& c : corners) {
            f32(c[0]), f32(c[1]), f32(c[2]);
            f32(0), f32(0), f32(1);                      // normal
            plain += std::string("\xff\x80\x00\xff", 4); // RGBA
            u32(1);
            f32(0.25f), f32(0.75f); // uv
            plain += std::string(44, '\0');
        }
        u32(3);
        u32(0), u32(1), u32(2);
        if (version == 4)
            plain += std::string(12, '\0');
        std::string sealed = plain;
        for (std::size_t i = 0; i < sealed.size(); ++i)
            sealed[i] = static_cast<char>(static_cast<std::uint8_t>(sealed[i]) ^ key[i % key.size()]);

        const auto mesh = lunar::assets::parseCsgMesh(sealed);
        CHECK(mesh.has_value());
        CHECK_EQ(mesh->version, std::format("CSGMDL {}", version));
        CHECK_EQ(mesh->mesh.vertices.size(), std::size_t{3});
        CHECK_EQ(mesh->mesh.indices.size(), std::size_t{3});
        CHECK_EQ(mesh->size().x, 2.0f);
        CHECK_EQ(mesh->size().y, 4.0f);
        CHECK_EQ(mesh->size().z, 1.0f);
        CHECK_EQ(mesh->mesh.vertices[1].normal.z, 1.0f);
        CHECK(std::abs(mesh->mesh.vertices[2].color.g - 128.0f / 255.0f) < 1e-3f);
        CHECK_EQ(mesh->mesh.vertices[0].u, 0.25f);
        // Not XORed, or cut short: errors, not crashes.
        CHECK(!lunar::assets::parseCsgMesh(plain).has_value());
        CHECK(!lunar::assets::parseCsgMesh(std::string_view(sealed).substr(0, 120)).has_value());
    }
}

TEST(assets_csg_mesh_v5)
{
    static constexpr std::array<std::uint8_t, 31> key{0x56, 0x2e, 0x6e, 0x58, 0x31, 0x20, 0x30, 0x04, 0x34, 0x69, 0x0c,
                                                     0x77, 0x0c, 0x01, 0x5e, 0x00, 0x1a, 0x60, 0x37, 0x69, 0x1d, 0x52,
                                                     0x2b, 0x07, 0x4f, 0x24, 0x59, 0x65, 0x53, 0x04, 0x7a};
    std::string data = "CSGMDL";
    auto u16 = [&](std::uint16_t v) { data.append(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [&](std::uint32_t v) { data.append(reinterpret_cast<const char*>(&v), 4); };
    auto f32 = [&](float v) { data.append(reinterpret_cast<const char*>(&v), 4); };
    u32(5);
    const float corners[4][3] = {{0, 0, 0}, {2, 0, 0}, {2, 4, 0}, {0, 4, -1}};
    u16(4);
    for (const auto& c : corners)
        f32(c[0]), f32(c[1]), f32(c[2]);
    u16(4), u32(24);
    for (int i = 0; i < 4; ++i)
        u16(0x7fff), u16(0xfffe), u16(0x7fff); // straight up
    u16(4);
    for (int i = 0; i < 4; ++i)
        data += std::string("\xff\x80\x00\xff", 4);
    u16(4);
    data += std::string(4, '\x01'); // face ids
    u16(4);
    for (int i = 0; i < 4; ++i)
        f32(static_cast<float>(i)), f32(0.5f);
    u16(4), u32(24);
    for (int i = 0; i < 4; ++i)
        u16(0), u16(0x7fff), u16(0x7fff); // tangents
    // Two levels of detail: 0 1 2 0 2 3, then 0 1 2; the -3 as a 3-byte delta.
    const std::string deltas("\x00\x01\x01\x7e\x02\x01\xff\xff\xfd\x01\x01", 11);
    u32(9), u32(static_cast<std::uint32_t>(deltas.size()));
    data += deltas;
    data += '\x03';
    u32(0), u32(6), u32(9);
    // Only the header is XORed.
    std::string sealed = data;
    for (std::size_t i = 0; i < 10; ++i)
        sealed[i] = static_cast<char>(static_cast<std::uint8_t>(sealed[i]) ^ key[i]);

    const auto mesh = lunar::assets::parseCsgMesh(sealed);
    CHECK(mesh.has_value());
    CHECK(mesh->version == "CSGMDL 5");
    CHECK_EQ(mesh->mesh.vertices.size(), std::size_t{4});
    CHECK(mesh->mesh.indices == (std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3})); // the finest level only
    CHECK_EQ(mesh->size().x, 2.0f);
    CHECK_EQ(mesh->size().y, 4.0f);
    CHECK_EQ(mesh->size().z, 1.0f);
    CHECK(std::abs(mesh->mesh.vertices[1].normal.y - 1.0f) < 1e-3f);
    CHECK(std::abs(mesh->mesh.vertices[1].normal.x) < 1e-3f);
    CHECK(std::abs(mesh->mesh.vertices[2].color.g - 128.0f / 255.0f) < 1e-3f);
    CHECK_EQ(mesh->mesh.vertices[3].u, 3.0f);
    CHECK_EQ(mesh->mesh.vertices[3].v, 0.5f);
    // Cut short, or an index past the vertices: errors, not crashes.
    CHECK(!lunar::assets::parseCsgMesh(std::string_view(sealed).substr(0, 90)).has_value());
    std::string wild = sealed;
    wild[wild.size() - 13 - deltas.size() + 1] = '\x3f';
    CHECK(!lunar::assets::parseCsgMesh(wild).has_value());
}
