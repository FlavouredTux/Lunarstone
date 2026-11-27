#include "framework.hpp"

#include <lunar/render/Material.hpp>
#include <lunar/render/Primitives.hpp>

#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

using lunar::MeshData;
using lunar::Vector3;
using namespace lunar::render;

namespace {

constexpr Primitive allPrimitives[] = {Primitive::Block,    Primitive::Wedge,     Primitive::CornerWedge,
                                       Primitive::Cylinder, Primitive::CylinderY, Primitive::Ball};

Vector3 faceNormal(const MeshData& mesh, std::size_t triangle)
{
    const Vector3 a = mesh.vertices[mesh.indices[triangle * 3]].position;
    const Vector3 b = mesh.vertices[mesh.indices[triangle * 3 + 1]].position;
    const Vector3 c = mesh.vertices[mesh.indices[triangle * 3 + 2]].position;
    return (b - a).cross(c - a);
}

// Divergence theorem: positive and equal to the enclosed volume only when
// every triangle winds counter-clockwise seen from outside.
float volume(const MeshData& mesh)
{
    float sum = 0.0f;
    for (std::size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        const Vector3 a = mesh.vertices[mesh.indices[t * 3]].position;
        const Vector3 b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
        const Vector3 c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
        sum += a.dot(b.cross(c));
    }
    return sum / 6.0f;
}

// Every edge, by position, belongs to as many triangles going one way as the other.
bool closed(const MeshData& mesh)
{
    auto key = [](Vector3 p) {
        return std::tuple{std::lround(p.x * 1e4f), std::lround(p.y * 1e4f), std::lround(p.z * 1e4f)};
    };
    std::map<std::pair<decltype(key({})), decltype(key({}))>, int> edges;
    for (std::size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        for (std::size_t e = 0; e < 3; ++e) {
            const auto a = key(mesh.vertices[mesh.indices[t * 3 + e]].position);
            const auto b = key(mesh.vertices[mesh.indices[t * 3 + (e + 1) % 3]].position);
            if (a == b)
                continue;
            if (a < b)
                ++edges[{a, b}];
            else
                --edges[{b, a}];
        }
    }
    for (const auto& [edge, balance] : edges)
        if (balance != 0)
            return false;
    return true;
}

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

} // namespace

TEST(primitives_fill_the_unit_box)
{
    for (Primitive primitive : allPrimitives) {
        const MeshData mesh = makePrimitive(primitive);
        CHECK(!mesh.empty());
        CHECK_EQ(mesh.indices.size() % 3, 0u);
        Vector3 lo{1, 1, 1};
        Vector3 hi{-1, -1, -1};
        for (const auto& v : mesh.vertices) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
            CHECK(near(v.normal.magnitude(), 1.0f, 1e-3f));
        }
        for (float c : {lo.x, lo.y, lo.z})
            CHECK(near(c, -0.5f, 1e-3f));
        for (float c : {hi.x, hi.y, hi.z})
            CHECK(near(c, 0.5f, 1e-3f));
        for (std::uint32_t index : mesh.indices)
            CHECK(index < mesh.vertices.size());
    }
}

TEST(primitives_are_closed_and_wound_outwards)
{
    const float pi = std::numbers::pi_v<float>;
    const std::pair<Primitive, float> volumes[] = {
        {Primitive::Block, 1.0f},         {Primitive::Wedge, 0.5f},          {Primitive::CornerWedge, 1.0f / 3.0f},
        {Primitive::Cylinder, pi / 4.0f}, {Primitive::CylinderY, pi / 4.0f}, {Primitive::Ball, pi / 6.0f},
    };
    for (auto [primitive, expected] : volumes) {
        const MeshData mesh = makePrimitive(primitive, 64);
        CHECK(closed(mesh));
        // Curved shapes are polygons, a little smaller than the real thing.
        CHECK(std::abs(volume(mesh) - expected) < expected * 0.01f);
    }
}

TEST(primitive_vertex_normals_match_their_faces)
{
    for (Primitive primitive : allPrimitives) {
        const MeshData mesh = makePrimitive(primitive);
        const bool flat
            = primitive == Primitive::Block || primitive == Primitive::Wedge || primitive == Primitive::CornerWedge;
        for (std::size_t t = 0; t < mesh.indices.size() / 3; ++t) {
            const Vector3 n = faceNormal(mesh, t).unit();
            for (std::size_t k = 0; k < 3; ++k) {
                const Vector3 vertexNormal = mesh.vertices[mesh.indices[t * 3 + k]].normal;
                // Flat faces share one normal; smooth ones lean within a segment.
                CHECK(n.dot(vertexNormal) > (flat ? 0.9999f : 0.9f));
            }
        }
    }
}

TEST(wedge_rises_to_the_back)
{
    const MeshData wedge = makePrimitive(Primitive::Wedge);
    bool sloped = false;
    for (const auto& v : wedge.vertices) {
        // Only the back edge is high.
        if (v.position.y > 0.0f)
            CHECK(near(v.position.z, 0.5f));
        // The slope looks forward and up.
        if (near(v.normal.y, std::sqrt(0.5f)) && near(v.normal.z, -std::sqrt(0.5f)))
            sloped = true;
    }
    CHECK(sloped);
}

TEST(corner_wedge_peaks_at_the_right_front)
{
    const MeshData corner = makePrimitive(Primitive::CornerWedge);
    for (const auto& v : corner.vertices)
        if (v.position.y > 0.0f)
            CHECK(v.position == (Vector3{0.5f, 0.5f, -0.5f}));
}

TEST(cylinders_run_along_their_axis)
{
    for (auto [primitive, axis] : {std::pair{Primitive::Cylinder, 0}, std::pair{Primitive::CylinderY, 1}}) {
        const MeshData mesh = makePrimitive(primitive);
        for (const auto& v : mesh.vertices) {
            const float p[3] = {v.position.x, v.position.y, v.position.z};
            const float n[3] = {v.normal.x, v.normal.y, v.normal.z};
            const auto a = static_cast<std::size_t>(axis);
            const std::size_t b = (a + 1) % 3;
            const std::size_t c = (a + 2) % 3;
            if (std::abs(n[a]) > 0.5f) {
                // Caps: flat, at the ends.
                CHECK(near(std::abs(p[a]), 0.5f));
                CHECK(p[b] * p[b] + p[c] * p[c] <= 0.25f + 1e-4f);
            } else {
                // Sides: round, normals pointing straight out.
                CHECK(near(p[b] * p[b] + p[c] * p[c], 0.25f));
                CHECK(near(n[a], 0.0f));
                CHECK(near(n[b], p[b] * 2.0f) && near(n[c], p[c] * 2.0f));
            }
        }
    }
}

TEST(ball_is_a_sphere)
{
    const MeshData ball = makePrimitive(Primitive::Ball);
    for (const auto& v : ball.vertices) {
        CHECK(near(v.position.magnitude(), 0.5f));
        CHECK(near(v.normal.x, v.position.x * 2.0f) && near(v.normal.y, v.position.y * 2.0f));
    }
}

TEST(primitive_meshes_are_shared)
{
    CHECK(primitiveMesh(Primitive::Ball).get() == primitiveMesh(Primitive::Ball).get());
    CHECK(primitiveMesh(Primitive::Ball).get() != primitiveMesh(Primitive::Block).get());
    CHECK_EQ(primitiveMesh(Primitive::Block)->indices.size(), 36u);
}

TEST(materials_look_different)
{
    using lunar::Enum::Material;
    CHECK_EQ(materialLook(Material::Neon).emissive, 1.0f);
    CHECK_EQ(materialLook(Material::Plastic).emissive, 0.0f);
    CHECK(materialLook(Material::SmoothPlastic).specular > materialLook(Material::Plastic).specular);
    CHECK(materialLook(Material::Metal).specular > materialLook(Material::SmoothPlastic).specular);
    CHECK(materialLook(Material::ForceField).minTransparency > 0.0f);
    CHECK_EQ(materialLook(Material::Glass).minTransparency, 0.0f);
    CHECK(materialLook(Material::Grass).pattern != SurfacePattern::None);
    CHECK(materialLook(Material::Brick).pattern == SurfacePattern::Bricks);
}
