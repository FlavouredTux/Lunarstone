#include "framework.hpp"

// MeshPart and union collision: the CSGPHS decoder (every version the client
// reads), convex hulls, scaling onto the part, rays, contacts and the world.

#include <lunar/Classes.hpp>
#include <lunar/physics/Collide.hpp>
#include <lunar/physics/CollisionData.hpp>
#include <lunar/physics/Geometry.hpp>
#include <lunar/physics/World.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lunar;
using namespace lunar::physics;
using namespace std::string_literals;

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps;
}
bool near(Vector3 a, Vector3 b, float eps = 1e-4f)
{
    return (a - b).magnitude() <= eps;
}

struct Bytes {
    std::string out;

    Bytes& u8(std::uint8_t v)
    {
        out += static_cast<char>(v);
        return *this;
    }
    Bytes& u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Bytes& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    Bytes& vec(Vector3 v) { return f32(v.x).f32(v.y).f32(v.z); }
    Bytes& raw(std::string_view s)
    {
        out += s;
        return *this;
    }
};

struct Piece {
    std::vector<Vector3> vertices;
    std::vector<std::uint32_t> indices;
};

// An axis-aligned box as 8 vertices and 12 triangles.
Piece boxPiece(Vector3 lo, Vector3 hi)
{
    Piece p;
    for (int i = 0; i < 8; ++i)
        p.vertices.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    p.indices
        = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    return p;
}

Bytes header(std::uint32_t version)
{
    return Bytes{}.raw("CSGPHS").u32(version);
}

// The per-piece record of versions 1 to 7.
void writePiece(Bytes& b, const Piece& p)
{
    b.u32(16).f32(0).f32(0).f32(0).f32(0);
    b.u32(16).f32(0).f32(0).f32(0).f32(1);
    b.u32(static_cast<std::uint32_t>(p.vertices.size() * 3)).u32(4);
    for (const Vector3& v : p.vertices)
        b.vec(v);
    b.u32(static_cast<std::uint32_t>(p.indices.size()));
    for (std::uint32_t i : p.indices)
        b.u32(i);
}

void writeMass(Bytes& b, float volume)
{
    b.f32(volume).vec({0.5f, 0.25f, 0.0f});
    for (float v : {1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 3.0f})
        b.f32(v);
}

std::string oldBlob(std::uint32_t version, const std::vector<Piece>& pieces, float volume = 0.0f,
                    std::uint8_t geometryType = 2)
{
    Bytes b = header(version);
    if (version == 7)
        b.u8(geometryType);
    if (version == 6 || version == 7)
        writeMass(b, volume);
    for (const Piece& p : pieces)
        writePiece(b, p);
    return b.out;
}

// A zstd frame holding `content` as one raw block (RFC 8878).
std::string zstdRaw(std::string_view content)
{
    Bytes b;
    b.raw("\x28\xb5\x2f\xfd"s);
    if (content.size() < 256) {
        b.u8(0x20).u8(static_cast<std::uint8_t>(content.size())); // single segment, 1-byte size
    } else {
        const auto size = static_cast<std::uint32_t>(content.size() - 256);
        b.u8(0x60).u8(static_cast<std::uint8_t>(size)).u8(static_cast<std::uint8_t>(size >> 8));
    }
    const auto blockHeader = static_cast<std::uint32_t>(content.size() << 3 | 1); // raw, last
    b.u8(static_cast<std::uint8_t>(blockHeader))
        .u8(static_cast<std::uint8_t>(blockHeader >> 8))
        .u8(static_cast<std::uint8_t>(blockHeader >> 16));
    return b.raw(content).out;
}

// A v8 blob: one multimesh piece (a tetrahedron) and one Edgebreaker piece
// whose symbols are C C E: five vertices.
std::string v8Blob()
{
    const std::vector<Vector3> tetra{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}};
    Bytes a;
    a.u32(2).u32(0).u32(12); // index offsets
    for (std::uint32_t i : {0u, 2u, 1u, 0u, 1u, 3u, 0u, 3u, 2u, 1u, 2u, 3u})
        a.u32(i);
    a.u32(2).u32(0).u32(12); // vertex float offsets
    for (const Vector3& v : tetra)
        a.vec(v);
    // C = 0, E = 111: 00111 in the low 5 bits of the only (last) word.
    Bytes bits;
    bits.u32(0b00111);
    Bytes c;
    for (Vector3 v : {Vector3{4, 0, 0}, {6, 0, 0}, {4, 2, 0}, {4, 0, 2}, {5, 1, 1}})
        c.vec(v);
    Bytes body;
    body.u32(1).u32(5).u32(4).u32(5).u32(4); // extra pieces, vertices, triangles; a piece's most vertices, triangles
    body.u32(static_cast<std::uint32_t>(a.out.size())).u32(5).u32(4).u32(static_cast<std::uint32_t>(c.out.size()));
    body.vec({0, 0, 0}).vec({6, 2, 2});
    body.raw(a.out).raw(bits.out).raw(c.out);
    return header(8).u8(2).u8(0).raw(zstdRaw(body.out)).out;
}

// A v8 blob of two Edgebreaker pieces, C C E (five vertices, four
// triangles) then C E (four, three): the header's per-piece maximums are
// smaller than its totals, as in every live blob.
std::string v8TwoPieceBlob()
{
    Bytes bits;
    bits.u32(0b0'0111'0111); // C C E, C E: nine bits in the only (last) word
    Bytes c;
    for (Vector3 v : {Vector3{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}, {1, 1, 1}, {4, 0, 0}, {6, 0, 0}, {4, 2, 0},
                      {4, 0, 2}})
        c.vec(v);
    Bytes body;
    body.u32(2).u32(9).u32(7).u32(5).u32(4); // extra pieces, vertices, triangles; a piece's most vertices, triangles
    body.u32(0).u32(9).u32(4).u32(static_cast<std::uint32_t>(c.out.size()));
    body.vec({0, 0, 0}).vec({6, 2, 2});
    body.raw(bits.out).raw(c.out);
    return header(8).u8(2).u8(0).raw(zstdRaw(body.out)).out;
}

std::shared_ptr<MeshPart> meshPart(std::string blob, Vector3 size, CFrame cf = {})
{
    auto part = std::make_shared<MeshPart>();
    part->setSize(size);
    part->setCFrame(cf);
    part->setAnchored(true);
    part->setPhysicalConfigData(std::make_shared<const std::string>(std::move(blob)));
    return part;
}

// A tree: a 2x10x2 trunk under a 10x6x10 canopy, 10 x 16 x 10 in all.
std::string treeBlob()
{
    return oldBlob(6, {boxPiece({-1, 0, -1}, {1, 10, 1}), boxPiece({-5, 10, -5}, {5, 16, 5})}, 140.0f);
}

} // namespace

TEST(physics_csgphs_block)
{
    for (const std::string& blob : {"CSGPHS\0\0\0\0BLOCK"s, "CSGPHS\0\0\0\0"s}) {
        auto data = decodeCollisionData(blob);
        CHECK(data.has_value());
        CHECK(data->block);
        CHECK(data->pieces.empty());
        CHECK(!meshGeometry(blob));
    }
    CHECK(!decodeCollisionData("CSGPHS\0\0\0\0JUNK!"s).has_value());
    CHECK(isCollisionData("CSGPHS\x08\0\0\0"s));
    CHECK(!isCollisionData("CSGPHS\x09\0\0\0"s));
    CHECK(!isCollisionData("CSGPH"));
    CHECK_EQ(decodeCollisionData("CSGPHS\x09\0\0\0"s).error(), "version 9 not in acceptable range");
    CHECK_EQ(decodeCollisionData("CSGXHS\x06\0\0\0"s).error(), "header identifier mismatch");
    CHECK_EQ(decodeCollisionData("CSG").error(), "empty or incomplete header");
}

TEST(physics_csgphs_v6_pieces)
{
    const std::string blob = oldBlob(6, {boxPiece({0, 0, 0}, {1, 2, 3}), boxPiece({-1, -1, -1}, {0, 0, 0})}, 7.0f);
    auto data = decodeCollisionData(blob);
    CHECK(data.has_value());
    CHECK_EQ(data->version, 6u);
    CHECK(!data->block);
    CHECK_EQ(data->pieces.size(), 2u);
    CHECK_EQ(data->pieces[0].vertices.size(), 8u);
    CHECK_EQ(data->pieces[0].indices.size(), 36u);
    CHECK(data->pieces[0].vertices[7] == (Vector3{1, 2, 3}));
    CHECK(data->mass.has_value());
    CHECK_EQ(data->mass->volume, 7.0f);
    CHECK(data->mass->centerOfMass == (Vector3{0.5f, 0.25f, 0.0f}));
    CHECK_EQ(data->mass->inertia[5], 3.0f);
    CHECK_EQ(data->geometryType, 2); // more than one piece
    CHECK(data->boundsMin == (Vector3{-1, -1, -1}));
    CHECK(data->boundsMax == (Vector3{1, 2, 3}));
}

TEST(physics_csgphs_old_versions)
{
    const Piece cube = boxPiece({0, 0, 0}, {1, 1, 1});
    // v7: a geometry type byte before the mass properties.
    auto v7 = decodeCollisionData(oldBlob(7, {cube}, 1.0f, 6));
    CHECK(v7.has_value());
    CHECK_EQ(v7->geometryType, 6);
    CHECK(v7->mass);
    CHECK(v7->mass->volume == 1.0f);
    CHECK_EQ(v7->pieces.size(), 1u);
    // v3 to v5: pieces right after the header.
    for (std::uint32_t version : {3u, 4u, 5u}) {
        auto data = decodeCollisionData(oldBlob(version, {cube, cube}));
        CHECK(data.has_value());
        CHECK(!data->mass);
        CHECK_EQ(data->pieces.size(), 2u);
    }
    // The client only builds mesh geometry from version 3 on.
    for (std::uint32_t version : {1u, 2u}) {
        std::string error;
        CHECK(decodeCollisionData(oldBlob(version, {cube})).has_value());
        CHECK(!meshGeometry(oldBlob(version, {cube}), &error));
        CHECK(error.contains("isn't used"));
    }
    // ClosedVoxel data has a format of its own.
    CHECK(!decodeCollisionData(oldBlob(7, {cube}, 1.0f, 5)).has_value());
    CHECK(!decodeCollisionData(oldBlob(7, {cube}, 1.0f, 9)).has_value());
}

TEST(physics_csgphs_rejects_bad_pieces)
{
    Piece cube = boxPiece({0, 0, 0}, {1, 1, 1});
    std::string blob = oldBlob(5, {cube});
    // Translation stride.
    std::string bad = blob;
    bad[10] = 12;
    CHECK(decodeCollisionData(bad).error().contains("translation stride 12"));
    // Rotation stride, right after the 16 translation bytes.
    bad = blob;
    bad[30] = 8;
    CHECK(decodeCollisionData(bad).error().contains("rotation stride 8"));
    // Vertex stride.
    bad = blob;
    bad[54] = 8;
    CHECK(decodeCollisionData(bad).error().contains("vertex stride 8"));
    // An index past the vertices.
    cube.indices[4] = 8;
    CHECK(decodeCollisionData(oldBlob(5, {cube})).error().contains("index 8 out of 8 vertices"));
    // Triangles must be whole.
    cube.indices = {0, 1};
    CHECK(decodeCollisionData(oldBlob(5, {cube})).error().contains("aren't whole triangles"));
    // No usable pieces at all: the box.
    std::string error;
    CHECK(!meshGeometry(header(5).out, &error));
    CHECK(error.contains("no pieces"));
}

TEST(physics_csgphs_v8)
{
    auto data = decodeCollisionData(v8Blob());
    CHECK(data.has_value());
    CHECK_EQ(data->version, 8u);
    CHECK_EQ(data->geometryType, 2);
    CHECK_EQ(data->pieces.size(), 2u);
    CHECK_EQ(data->pieces[0].vertices.size(), 4u);
    CHECK_EQ(data->pieces[0].indices.size(), 12u);
    CHECK(data->pieces[0].vertices[1] == (Vector3{2, 0, 0}));
    // Edgebreaker piece: three vertices plus one per C.
    CHECK_EQ(data->pieces[1].vertices.size(), 5u);
    CHECK(data->pieces[1].vertices[4] == (Vector3{5, 1, 1}));
    CHECK(data->pieces[1].indices.empty());
    CHECK(data->boundsMax == (Vector3{6, 2, 2}));
    CHECK(!data->mass);
    auto geometry = meshGeometry(v8Blob());
    CHECK(geometry);
    CHECK(geometry->pieces.size() == 2);
    CHECK(near(geometry->extent, {6, 2, 2}));
}

TEST(physics_csgphs_v8_several_edgebreaker_pieces)
{
    auto data = decodeCollisionData(v8TwoPieceBlob());
    CHECK(data.has_value());
    CHECK_EQ(data->pieces.size(), 2u);
    CHECK_EQ(data->pieces[0].vertices.size(), 5u);
    CHECK_EQ(data->pieces[1].vertices.size(), 4u);
    CHECK(data->pieces[1].vertices[0] == (Vector3{4, 0, 0}));
    CHECK(meshGeometry(v8TwoPieceBlob()));
}

TEST(physics_csgphs_survives_truncation_and_corruption)
{
    const std::vector<std::string> blobs{oldBlob(6, {boxPiece({0, 0, 0}, {1, 2, 3}), boxPiece({0, 0, 0}, {1, 1, 1})}),
                                         oldBlob(7, {boxPiece({0, 0, 0}, {1, 1, 1})}, 1.0f, 2), v8Blob(),
                                         "CSGPHS\0\0\0\0BLOCK"s};
    for (const std::string& blob : blobs) {
        // Every prefix: either an error or (when whole pieces drop off) fewer pieces.
        for (std::size_t n = 0; n < blob.size(); ++n) {
            auto data = decodeCollisionData(std::string_view(blob).substr(0, n));
            if (data)
                CHECK(data->pieces.size() <= 2);
            if (auto geometry = meshGeometry(std::string_view(blob).substr(0, n)))
                CHECK(!geometry->pieces.empty());
        }
        // Flipped bytes everywhere, deterministically.
        std::uint32_t seed = 12345;
        for (int round = 0; round < 400; ++round) {
            std::string bad = blob;
            for (int k = 0; k < 3; ++k) {
                seed = seed * 1664525u + 1013904223u;
                const std::size_t at = 6 + (seed >> 8) % (bad.size() - 6);
                bad[at] = static_cast<char>(seed >> 24);
            }
            if (auto data = decodeCollisionData(bad); !data)
                CHECK(!data.error().empty());
            if (auto geometry = meshGeometry(bad))
                CHECK(!geometry->pieces.empty());
        }
    }
}

TEST(physics_convex_hull)
{
    // A cube's corners plus points inside and on faces: 8 corners, 6 quads.
    std::vector<Vector3> points;
    points.reserve(12);
    for (int i = 0; i < 8; ++i)
        points.push_back({(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f});
    points.push_back({0.1f, 0.2f, -0.3f});
    points.push_back({0.0f, 1.0f, 0.0f});
    points.push_back({1.0f, 1.0f, 0.0f});
    points.push_back(points[3]); // a duplicate
    Polyhedron hull = convexHull(points);
    CHECK_EQ(hull.vertices.size(), 8u);
    CHECK_EQ(hull.faces.size(), 6u);
    CHECK_EQ(hull.edges.size(), 12u);
    CHECK_EQ(hull.edgeDirections.size(), 3u);
    for (const auto& face : hull.faces) {
        CHECK_EQ(face.indices.size(), 4u);
        CHECK(near(face.offset, 1.0f));
        // Every corner is on or behind every face.
        for (const Vector3& v : hull.vertices)
            CHECK(face.normal.dot(v) <= face.offset + 1e-5f);
        // Counter-clockwise from outside.
        const Vector3 a = hull.vertices[face.indices[0]];
        const Vector3 b = hull.vertices[face.indices[1]];
        const Vector3 c = hull.vertices[face.indices[2]];
        CHECK((b - a).cross(c - a).dot(face.normal) > 0.0f);
    }
    // A tetrahedron.
    hull = convexHull(std::vector<Vector3>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}});
    CHECK_EQ(hull.vertices.size(), 4u);
    CHECK_EQ(hull.faces.size(), 4u);
    // Points in one plane: a thin slab with the polygon's outline.
    hull = convexHull(std::vector<Vector3>{{0, 0, 0}, {2, 0, 0}, {2, 0, 2}, {0, 0, 2}, {1, 0, 1}});
    CHECK_EQ(hull.faces.size(), 6u); // top, bottom, four sides
    CHECK_EQ(hull.vertices.size(), 8u);
    // A line or a point: nothing.
    CHECK(convexHull(std::vector<Vector3>{{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}).empty());
    CHECK(convexHull(std::vector<Vector3>{{1, 1, 1}}).empty());
}

TEST(physics_convex_hull_of_nearly_coplanar_rings)
{
    // A pipe's piece: rings of points whose heights and radii are off by
    // float noise, as decoded collision data has them. The hull must stay a
    // convex polyhedron (it once grew hundreds of overlapping faces here,
    // and took seconds on bigger rings).
    std::uint32_t state = 12345;
    auto noise = [&] {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 4e-6f;
    };
    std::vector<Vector3> points;
    for (int ring = 0; ring < 4; ++ring)
        for (int i = 0; i < 16; ++i) {
            const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / 16.0f;
            const float radius = 1.4f + 0.05f * static_cast<float>(ring % 3);
            points.push_back({radius * std::cos(a) + noise(), 1.3f - 0.2f * static_cast<float>(ring) + noise(),
                              radius * std::sin(a) + noise()});
        }
    const Polyhedron hull = convexHull(points);
    CHECK(!hull.empty());
    CHECK(hull.faces.size() <= 2 * hull.vertices.size() - 4);
    for (const auto& face : hull.faces)
        for (const Vector3& v : hull.vertices)
            CHECK(face.normal.dot(v) <= face.offset + 1e-4f);
}

TEST(physics_convex_hull_edge_faces)
{
    // A cube: each of the 12 edges borders two different faces that both hold
    // its corners, and the second normal crossed with the first runs against
    // the first face's walk (from > to), which the edge test's arcs rely on.
    std::vector<Vector3> points;
    points.reserve(8);
    for (int i = 0; i < 8; ++i)
        points.push_back({(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f});
    const Polyhedron hull = convexHull(points);
    CHECK_EQ(hull.edgeFaces.size(), hull.edges.size());
    for (std::size_t i = 0; i < hull.edges.size(); ++i) {
        const auto [from, to] = hull.edges[i];
        const auto [f, g] = hull.edgeFaces[i];
        CHECK(f != g);
        for (std::uint16_t face : {f, g}) {
            const auto& ix = hull.faces[face].indices;
            CHECK(std::ranges::find(ix, from) != ix.end());
            CHECK(std::ranges::find(ix, to) != ix.end());
        }
        const Vector3 across = hull.faces[g].normal.cross(hull.faces[f].normal);
        CHECK(near(across.unit(), (hull.vertices[from] - hull.vertices[to]).unit()));
    }
}

TEST(physics_mesh_edge_contacts_match_brute_force)
{
    // Round hulls of 12 to 30 points, many edges each, placed at random
    // around each other: the narrow phase must agree with trying every face
    // normal and every pair of edges by both hulls' extremes.
    std::uint32_t state = 2024;
    auto random = [&](float lo, float hi) {
        state = state * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>(state >> 8) / 16777216.0f;
    };
    auto hull = [&] {
        std::vector<Vector3> points;
        const Vector3 stretch{random(0.5f, 2.0f), random(0.5f, 2.0f), random(0.5f, 2.0f)};
        const int count = static_cast<int>(random(12.0f, 30.0f));
        while (std::cmp_less(points.size(), count)) {
            const Vector3 v{random(-1, 1), random(-1, 1), random(-1, 1)};
            const float len = v.magnitude();
            if (len > 0.2f && len <= 1.0f)
                points.push_back(mul(v / len, stretch));
        }
        auto mesh = std::make_shared<MeshGeometry>();
        mesh->pieces.push_back(convexHull(points));
        Aabb box;
        for (const Vector3& v : mesh->pieces[0].vertices)
            box.expand(v);
        // Centered, so the part's Size scales it by 1.
        for (Vector3& v : mesh->pieces[0].vertices)
            v = v - box.center();
        for (auto& face : mesh->pieces[0].faces)
            face.offset -= face.normal.dot(box.center());
        mesh->bounds.push_back({box.min - box.center(), box.max - box.center()});
        mesh->extent = box.max - box.min;
        mesh->volume = 1.0f;
        return std::pair{makeMeshShape(mesh, mesh->extent), mesh};
    };
    auto rotation = [&] {
        constexpr float turn = 2.0f * std::numbers::pi_v<float>;
        return CFrame::angles(random(0.0f, turn), random(0.0f, turn), random(0.0f, turn));
    };
    constexpr float margin = 0.02f;
    int touching = 0;
    int dropped = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        const auto [a, meshA] = hull();
        const auto [b, meshB] = hull();
        const Polyhedron& pa = meshA->pieces[0];
        const Polyhedron& pb = meshB->pieces[0];
        CHECK_EQ(pa.edgeFaces.size(), pa.edges.size());
        CHECK_EQ(pb.edgeFaces.size(), pb.edges.size());
        const CFrame ca = rotation();
        CFrame cb = rotation();
        cb.position = {random(-2.5f, 2.5f), random(-2.5f, 2.5f), random(-2.5f, 2.5f)};
        std::vector<Vector3> va;
        std::vector<Vector3> vb;
        va.reserve(pa.vertices.size());
        vb.reserve(pb.vertices.size());
        for (const Vector3& v : pa.vertices)
            va.push_back(ca.pointToWorldSpace(v));
        for (const Vector3& v : pb.vertices)
            vb.push_back(cb.pointToWorldSpace(v));
        const float skin = a.skin + b.skin;
        // B's distance beyond A along n, less the skins.
        auto gap = [&](Vector3 n) {
            float hiA = -1e30f;
            float loB = 1e30f;
            for (const Vector3& v : va)
                hiA = std::max(hiA, n.dot(v));
            for (const Vector3& v : vb)
                loB = std::min(loB, n.dot(v));
            return loB - hiA - skin;
        };
        float best = -1e30f;
        auto tryAxis = [&](Vector3 n) {
            const float len = n.magnitude();
            if (len > 1e-6f)
                best = std::max({best, gap(n / len), gap(-n / len)});
        };
        for (const auto& face : pa.faces)
            tryAxis(ca.vectorToWorldSpace(face.normal));
        for (const auto& face : pb.faces)
            tryAxis(cb.vectorToWorldSpace(face.normal));
        for (const auto& [i, j] : pa.edges)
            for (const auto& [k, l] : pb.edges)
                tryAxis((va[j] - va[i]).cross(vb[l] - vb[k]));

        Manifold m;
        const bool hit = collide(a, ca, b, cb, m, margin);
        if (best > margin + 1e-3f)
            CHECK(!hit);
        if (best < -0.05f) {
            // Known gap: in a deep, skewed overlap the other face can lie
            // wholly outside the chosen face's sides and clip away, and the
            // contact is lost (props placed overlapping keep still that way;
            // pushing them apart needs gentler depenetration first).
            if (!hit) {
                ++dropped;
                continue;
            }
            ++touching;
            // The normal is the least overlap's axis, or one the narrow phase
            // prefers within its tolerance (an edge loses to a face 5% and
            // 0.01 worse, and face B to face A the same way).
            const float along = gap(m.normal);
            CHECK(along <= best + 1e-3f);
            CHECK(along >= ((best - 0.01f) / 0.95f - 0.01f) / 0.95f - 1e-3f);
        }
    }
    CHECK(touching > 300);
    CHECK(dropped * 10 < touching); // 48 of 1115 at this seed
}

TEST(physics_mesh_scale)
{
    CHECK(near(meshScale({4, 6, 8}, {2, 3, 4}), {2, 2, 2}));
    CHECK(near(meshScale({4, 6, 8}, {1, 6, 16}), {4, 1, 0.5f}));
    // A flat axis keeps scale 1.
    CHECK(near(meshScale({4, 6, 8}, {2, 0, 4}), {2, 1, 2}));
    CHECK(near(meshScale({4, 6, 8}, {2, 1e-6f, 4}), {2, 1, 2}));
}

TEST(physics_mesh_shape_from_part)
{
    // Pieces off center (0..2, 0..4, 0..2) on a 4x4x4 part: centered, then
    // stretched 2x in X and Z.
    auto part = meshPart(oldBlob(6, {boxPiece({0, 0, 0}, {2, 4, 2})}, 16.0f), {4, 4, 4});
    const Shape shape = shapeOf(*part);
    CHECK(shape.kind == ShapeKind::Mesh);
    CHECK(near(shape.scale, {2, 1, 2}));
    CHECK(near(shape.volume(), 64.0f)); // stored 16, scaled by 4
    CHECK(near(shape.support({1, 1, 1}), {2, 2, 2}));
    auto hit = raycast(shape, {10, 0, 0}, {-20, 0, 0});
    CHECK(hit);
    CHECK(near(hit->t, 8.0f / 20.0f));
    CHECK(near(hit->normal, {1, 0, 0}));
    hit = raycast(shape, {0, 10, 0}, {0, -20, 0});
    CHECK(hit);
    CHECK(near(hit->t, 8.0f / 20.0f));
    CHECK(near(hit->normal, {0, 1, 0}));

    // BLOCK, nothing, or unusable data: the Size box.
    for (const std::string& blob :
         {"CSGPHS\0\0\0\0BLOCK"s, ""s, "garbage"s, oldBlob(2, {boxPiece({0, 0, 0}, {1, 1, 1})})})
        CHECK(shapeOf(*meshPart(blob, {4, 4, 4})).kind == ShapeKind::Box);
    auto none = std::make_shared<MeshPart>();
    CHECK(shapeOf(*none).kind == ShapeKind::Box);
    // PhysicsData stands in when PhysicalConfigData is missing.
    none->setPhysicsData(std::make_shared<const std::string>(treeBlob()));
    CHECK(shapeOf(*none).kind == ShapeKind::Mesh);
    // Unions go the same way.
    auto unionPart = std::make_shared<UnionOperation>();
    unionPart->setPhysicalConfigData(std::make_shared<const std::string>(treeBlob()));
    CHECK(shapeOf(*unionPart).kind == ShapeKind::Mesh);
}

TEST(physics_mesh_ray_normals_under_scale)
{
    // A wedge-like piece: its slope's normal changes when stretched.
    const Piece wedge{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}, {2, 0, 2}, {0, 2, 2}}, {}};
    auto part = meshPart(oldBlob(5, {wedge}), {4, 2, 2}); // 2x along X
    const Shape shape = shapeOf(*part);
    // Centered on (1, 1, 1) the slope is x + y = 0; stretched 2x along X it's
    // x / 2 + y = 0, whose normal leans less toward X.
    auto hit = raycast(shape, {0, 5, 0}, {0, -10, 0});
    CHECK(hit);
    CHECK(near(hit->t, 0.5f));
    CHECK(near(hit->normal, Vector3{0.5f, 1.0f, 0.0f}.unit()));
    // Starting inside doesn't hit.
    CHECK(!raycast(shape, {-0.5f, -0.5f, 0}, {0, -10, 0}));
}

TEST(physics_mesh_contacts)
{
    // A cube resting on a mesh floor: one manifold with four points, 0.05
    // deeper than the bare polyhedra overlap (the pieces' margin).
    auto floor = meshPart(oldBlob(5, {boxPiece({0, 0, 0}, {10, 1, 10})}), {20, 1, 20});
    const Shape floorShape = shapeOf(*floor);
    const Shape cube = makeShape(ShapeKind::Box, {2, 2, 2});
    std::vector<Manifold> manifolds;
    collide(cube, CFrame(0, 1.45f, 0), floorShape, CFrame(), manifolds);
    CHECK_EQ(manifolds.size(), 1u);
    CHECK_EQ(manifolds[0].count, 4);
    CHECK(near(manifolds[0].normal, {0, -1, 0}));
    CHECK(near(manifolds[0].points[0].depth, 0.05f + meshSkin));
    // Over the stretched part of the floor (x = 8 is past the unscaled 5).
    manifolds.clear();
    collide(cube, CFrame(8, 1.45f, 8), floorShape, CFrame(), manifolds);
    CHECK_EQ(manifolds.size(), 1u);
    // Two pieces under one cube: one manifold each.
    auto steps = meshPart(oldBlob(5, {boxPiece({-2, 0, -1}, {0, 1, 1}), boxPiece({0, 0, -1}, {2, 1, 1})}), {4, 1, 2});
    manifolds.clear();
    collide(cube, CFrame(0, 1.45f, 0), shapeOf(*steps), CFrame(), manifolds);
    CHECK_EQ(manifolds.size(), 2u);
    // A ball, and a mesh against a mesh.
    const Shape ball = makeShape(ShapeKind::Ball, {2, 2, 2});
    Manifold m;
    CHECK(collide(floorShape, CFrame(), ball, CFrame(3, 1.4f, 0), m));
    CHECK(near(m.normal, {0, 1, 0}));
    CHECK(near(m.points[0].depth, 0.1f + meshSkin));
    CHECK(collide(floorShape, CFrame(), floorShape, CFrame(0, 0.9f, 0), m));
    CHECK(near(m.normal, {0, 1, 0}));
    CHECK(!collide(floorShape, CFrame(), floorShape, CFrame(0, 1.5f, 0), m));
    // A mesh on a terrain triangle.
    CHECK(collideTriangle(floorShape, CFrame(0, 0.45f, 0), {-50, 0, -50}, {0, 0, 50}, {50, 0, -50}, m));
    CHECK(near(m.normal, {0, -1, 0}));
}

TEST(physics_mesh_tree_regression)
{
    // The bug: trees collided as their whole box. A point inside the box but
    // away from the trunk and under the canopy is empty space.
    auto tree = meshPart(treeBlob(), {10, 16, 10}, CFrame(100, 8, 50));
    const Shape shape = shapeOf(*tree);
    CHECK(shape.kind == ShapeKind::Mesh);
    const Shape cube = makeShape(ShapeKind::Box, {1, 1, 1});
    Manifold m;
    // Box corner region near the ground: nothing there.
    CHECK(!collide(cube, CFrame(104, 1, 54), shape, tree->cframe(), m, 0.0f));
    // The trunk is.
    CHECK(collide(cube, CFrame(100.5f, 1, 50), shape, tree->cframe(), m, 0.0f));
    // And the canopy.
    CHECK(collide(cube, CFrame(104, 13, 54), shape, tree->cframe(), m, 0.0f));
    // Rays at waist height reach the trunk, not the box face.
    const CFrame cf = tree->cframe();
    auto hit = raycast(shape, cf.pointToObjectSpace({110, 3, 50}), cf.vectorToObjectSpace({-20, 0, 0}));
    CHECK(hit);
    CHECK(near(hit->t, 9.0f / 20.0f));
}

TEST(physics_world_meshes)
{
    auto game = std::make_shared<DataModel>();
    Workspace& workspace = *game->workspace();
    auto ground = std::make_shared<Part>();
    ground->setSize({200, 2, 200});
    ground->setCFrame(CFrame(0, -1, 0));
    ground->setAnchored(true);
    ground->setParent(&workspace);
    // Two trees sharing one blob.
    const auto blob = std::make_shared<const std::string>(treeBlob());
    std::vector<std::shared_ptr<MeshPart>> trees;
    for (float x : {0.0f, 30.0f}) {
        auto tree = std::make_shared<MeshPart>();
        tree->setSize({10, 16, 10});
        tree->setCFrame(CFrame(x, 8, 0));
        tree->setAnchored(true);
        tree->setPhysicalConfigData(blob);
        tree->setParent(&workspace);
        trees.push_back(tree);
    }
    // A ball dropped inside the first tree's box, beside the trunk, under the canopy.
    auto ball = std::make_shared<Part>();
    ball->setShape(Enum::PartType::Ball);
    ball->setSize({1, 1, 1});
    ball->setCFrame(CFrame(3.5f, 6, 3.5f));
    ball->setParent(&workspace);

    World world(workspace);
    world.refresh();
    CHECK_EQ(world.meshColliderCount(), 2u);
    CHECK_EQ(world.meshGeometryCount(), 1u);

    // Workspace:Raycast at waist height stops at the trunk.
    auto hit = world.raycast({20, 3, 0}, {-40, 0, 0});
    CHECK(hit);
    CHECK(hit->instance == trees[0].get());
    CHECK(near(hit->position.x, 1.0f));
    CHECK(near(hit->normal, {1, 0, 0}));
    // Down through the canopy: its top.
    hit = world.raycast({4, 30, 4}, {0, -40, 0});
    CHECK(hit);
    CHECK(hit->instance == trees[0].get());
    CHECK(near(hit->position.y, 16.0f));

    // The ball falls to the ground instead of being pushed out of the box.
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    CHECK(near(ball->position().y, 0.5f, 0.05f));
    CHECK(near(ball->position().x, 3.5f, 0.05f));
    CHECK(near(ball->position().z, 3.5f, 0.05f));

    // A crate dropped on a mesh slab settles on the slab's margin, 0.05 up.
    auto slab = std::make_shared<MeshPart>();
    slab->setSize({20, 1, 20});
    slab->setCFrame(CFrame(60, 0.5f, 0));
    slab->setAnchored(true);
    slab->setPhysicalConfigData(std::make_shared<const std::string>(oldBlob(5, {boxPiece({0, 0, 0}, {1, 1, 1})})));
    slab->setParent(&workspace);
    auto crate = std::make_shared<Part>();
    crate->setSize({2, 2, 2});
    crate->setCFrame(CFrame(60, 3, 0));
    crate->setParent(&workspace);
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    const float gap = crate->position().y - 1.0f - 1.0f;
    CHECK(gap > meshSkin - 0.02f);
    CHECK(gap < meshSkin + 0.01f);

    // Taking the data away makes it a box again.
    trees[0]->setPhysicalConfigData(nullptr);
    hit = world.raycast({20, 3, 0}, {-40, 0, 0});
    CHECK(hit);
    CHECK(near(hit->position.x, 5.0f));
}
