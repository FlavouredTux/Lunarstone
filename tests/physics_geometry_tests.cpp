#include "framework.hpp"

// Collision shapes, ray tests against each of them, the BVH and contact
// generation.

#include <lunar/physics/Bvh.hpp>
#include <lunar/physics/Collide.hpp>
#include <lunar/physics/Geometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <vector>

using namespace lunar;
using namespace lunar::physics;

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps;
}
bool near(Vector3 a, Vector3 b, float eps = 1e-4f)
{
    return (a - b).magnitude() <= eps;
}

} // namespace

TEST(physics_ray_box)
{
    const Shape box = makeShape(ShapeKind::Box, {4.0f, 2.0f, 6.0f});
    auto hit = raycast(box, {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 9.0f / 20.0f));
    CHECK(near(hit->normal, {0.0f, 1.0f, 0.0f}));
    hit = raycast(box, {-10.0f, 0.0f, 2.9f}, {20.0f, 0.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 8.0f / 20.0f));
    CHECK(near(hit->normal, {-1.0f, 0.0f, 0.0f}));
    CHECK(!raycast(box, {-10.0f, 0.0f, 3.1f}, {20.0f, 0.0f, 0.0f}));
    // Too short, and starting inside.
    CHECK(!raycast(box, {0.0f, 10.0f, 0.0f}, {0.0f, -8.0f, 0.0f}));
    CHECK(!raycast(box, {0.0f, 0.0f, 0.0f}, {0.0f, -8.0f, 0.0f}));
}

TEST(physics_ray_wedge)
{
    // Slope rises from the front bottom edge (z = -1) to the back top edge (z = 1).
    const Shape wedge = makeShape(ShapeKind::Wedge, {2.0f, 2.0f, 2.0f});
    auto hit = raycast(wedge, {0.0f, 5.0f, 0.0f}, {0.0f, -10.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 0.5f)); // the slope crosses y = 0 at z = 0
    const float s = std::numbers::sqrt2_v<float> / 2.0f;
    CHECK(near(hit->normal, {0.0f, s, -s}));
    // The back face is vertical and full height.
    hit = raycast(wedge, {0.0f, 0.9f, 5.0f}, {0.0f, 0.0f, -10.0f});
    CHECK(hit);
    CHECK(near(hit->t, 0.4f));
    CHECK(near(hit->normal, {0.0f, 0.0f, 1.0f}));
    // Above the slope near the front there's nothing.
    CHECK(!raycast(wedge, {-5.0f, 0.5f, -0.9f}, {10.0f, 0.0f, 0.0f}));
}

TEST(physics_ray_corner_wedge)
{
    // Apex over the +X -Z corner: high there, nothing above the -X +Z corner.
    const Shape wedge = makeShape(ShapeKind::CornerWedge, {2.0f, 2.0f, 2.0f});
    auto hit = raycast(wedge, {0.9f, 5.0f, -0.9f}, {0.0f, -10.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->t < 0.45f);
    hit = raycast(wedge, {-0.9f, 5.0f, 0.9f}, {0.0f, -10.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 0.6f, 0.02f));
}

TEST(physics_ray_ball_and_cylinder)
{
    // Balls use the smallest side.
    const Shape ball = makeShape(ShapeKind::Ball, {4.0f, 2.0f, 3.0f});
    CHECK_EQ(ball.radius, 1.0f);
    auto hit = raycast(ball, {0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 10.0f});
    CHECK(hit);
    CHECK(near(hit->t, 0.4f));
    CHECK(near(hit->normal, {0.0f, 0.0f, -1.0f}));
    CHECK(!raycast(ball, {0.0f, 1.01f, -5.0f}, {0.0f, 0.0f, 10.0f}));

    // Cylinders lie along X, round in Y and Z.
    const Shape cylinder = makeShape(ShapeKind::Cylinder, {6.0f, 2.0f, 2.0f});
    hit = raycast(cylinder, {0.0f, 5.0f, 0.0f}, {0.0f, -10.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 0.4f));
    CHECK(near(hit->normal, {0.0f, 1.0f, 0.0f}));
    hit = raycast(cylinder, {10.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f});
    CHECK(hit);
    CHECK(near(hit->t, 7.0f / 20.0f));
    CHECK(near(hit->normal, {1.0f, 0.0f, 0.0f}));
    // The round side is round: a ray through the corner of the bounding square misses.
    CHECK(raycast(cylinder, {0.0f, 0.75f, -5.0f}, {0.0f, 0.0f, 10.0f}));
    CHECK(!raycast(cylinder, {0.0f, -1.0f, 2.8f}, {0.0f, 4.0f, -4.0f}));
    CHECK(!raycast(makeShape(ShapeKind::Cylinder, {6.0f, 2.0f, 2.0f}), {0.0f, 0.5f, 0.5f}, {0.0f, -2.0f, 0.0f}));
}

TEST(physics_ray_triangle)
{
    auto t = rayTriangle({0.2f, 5.0f, 0.2f}, {0.0f, -10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                         {1.0f, 0.0f, 0.0f});
    CHECK(t);
    CHECK(near(*t, 0.5f));
    CHECK(!rayTriangle({0.8f, 5.0f, 0.8f}, {0.0f, -10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                       {1.0f, 0.0f, 0.0f}));
}

TEST(physics_shape_volumes)
{
    CHECK(near(makeShape(ShapeKind::Box, {2.0f, 3.0f, 4.0f}).volume(), 24.0f));
    CHECK(near(makeShape(ShapeKind::Wedge, {2.0f, 3.0f, 4.0f}).volume(), 12.0f));
    CHECK(near(makeShape(ShapeKind::CornerWedge, {2.0f, 3.0f, 4.0f}).volume(), 8.0f));
    CHECK(near(makeShape(ShapeKind::Ball, {2.0f, 2.0f, 2.0f}).volume(), 4.0f / 3.0f * std::numbers::pi_v<float>));
    // Every polyhedron face points away from the vertices' centroid.
    for (ShapeKind kind : {ShapeKind::Box, ShapeKind::Wedge, ShapeKind::CornerWedge, ShapeKind::Cylinder}) {
        const Shape s = makeShape(kind, {2.0f, 2.0f, 2.0f});
        Vector3 centroid;
        for (const Vector3& v : s.polyhedron->vertices)
            centroid = centroid + v / static_cast<float>(s.polyhedron->vertices.size());
        for (const auto& face : s.polyhedron->faces)
            CHECK(face.offset - face.normal.dot(centroid) > 0.1f);
    }
}

TEST(physics_bvh_query_and_ray)
{
    std::vector<Aabb> boxes;
    for (int i = 0; i < 100; ++i) {
        const float x = static_cast<float>(i) * 3.0f;
        boxes.push_back({{x, 0.0f, 0.0f}, {x + 1.0f, 1.0f, 1.0f}});
    }
    const Bvh bvh(boxes);
    std::set<std::uint32_t> found;
    bvh.query({{29.5f, 0.5f, 0.5f}, {33.5f, 0.6f, 0.6f}}, [&](std::uint32_t i) { found.insert(i); });
    CHECK_EQ(found, (std::set<std::uint32_t>{10, 11}));

    // A ray along the row reaches the nearest leaf first; shortening stops the walk.
    std::vector<std::uint32_t> visited;
    bvh.raycast({-5.0f, 0.5f, 0.5f}, {400.0f, 0.0f, 0.0f}, [&](std::uint32_t i, float maxT) {
        visited.push_back(i);
        return i == 0 ? 0.0125f : maxT;
    });
    CHECK(std::ranges::find(visited, 0u) != visited.end());
    CHECK(visited.size() <= 4);
    CHECK(std::ranges::all_of(visited, [](std::uint32_t i) { return i < 4; }));
}

TEST(physics_collide_resting_box)
{
    // A 2-stud cube resting on a baseplate: four points along the bottom face, normal up.
    const Shape plate = makeShape(ShapeKind::Box, {50.0f, 1.0f, 50.0f});
    const Shape cube = makeShape(ShapeKind::Box, {2.0f, 2.0f, 2.0f});
    Manifold m;
    CHECK(collide(plate, CFrame(0.0f, 0.0f, 0.0f), cube, CFrame(0.0f, 1.45f, 0.0f), m));
    CHECK_EQ(m.count, 4);
    CHECK(near(m.normal, {0.0f, 1.0f, 0.0f}));
    for (int i = 0; i < m.count; ++i) {
        CHECK(near(m.points[static_cast<std::size_t>(i)].depth, 0.05f));
        CHECK(near(m.points[static_cast<std::size_t>(i)].position.y, 0.475f));
    }
    // Swapped, the normal still points from A to B.
    CHECK(collide(cube, CFrame(0.0f, 1.45f, 0.0f), plate, CFrame(0.0f, 0.0f, 0.0f), m));
    CHECK(near(m.normal, {0.0f, -1.0f, 0.0f}));
    // Apart.
    CHECK(!collide(plate, CFrame(0.0f, 0.0f, 0.0f), cube, CFrame(0.0f, 2.0f, 0.0f), m));
}

TEST(physics_collide_tilted_and_round)
{
    const Shape plate = makeShape(ShapeKind::Box, {50.0f, 1.0f, 50.0f});
    // A cube on its edge touches along that edge.
    const Shape cube = makeShape(ShapeKind::Box, {2.0f, 2.0f, 2.0f});
    CFrame tilted = CFrame::angles(0.0f, 0.0f, std::numbers::pi_v<float> / 4.0f);
    tilted.position = {0.0f, 0.5f + std::numbers::sqrt2_v<float> - 0.01f, 0.0f};
    Manifold m;
    CHECK(collide(plate, CFrame(), cube, tilted, m));
    CHECK(near(m.normal, {0.0f, 1.0f, 0.0f}));
    CHECK(m.count == 2);
    // A ball.
    const Shape ball = makeShape(ShapeKind::Ball, {2.0f, 2.0f, 2.0f});
    CHECK(collide(plate, CFrame(), ball, CFrame(3.0f, 1.4f, 0.0f), m));
    CHECK_EQ(m.count, 1);
    CHECK(near(m.normal, {0.0f, 1.0f, 0.0f}));
    CHECK(near(m.points[0].depth, 0.1f));
    // Ball against a box corner from the side.
    CHECK(collide(ball, CFrame(25.5f, 0.0f, 0.0f), plate, CFrame(), m));
    CHECK(near(m.normal, {-1.0f, 0.0f, 0.0f}));
    // Terrain triangle under a cube.
    CHECK(collideTriangle(cube, CFrame(0.0f, 0.95f, 0.0f), {-5.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 5.0f},
                          {5.0f, 0.0f, -5.0f}, m));
    CHECK(near(m.normal, {0.0f, -1.0f, 0.0f}));
    CHECK(m.count >= 3);
}

TEST(physics_bvh_refit)
{
    std::vector<Aabb> boxes;
    for (int i = 0; i < 50; ++i) {
        const float x = static_cast<float>(i) * 3.0f;
        boxes.push_back({{x, 0.0f, 0.0f}, {x + 1.0f, 1.0f, 1.0f}});
    }
    Bvh bvh(boxes);
    // Box 7 moves far away; queries find it at its new place only.
    boxes[7] = {{500.0f, 500.0f, 500.0f}, {501.0f, 501.0f, 501.0f}};
    bvh.refit(boxes);
    std::set<std::uint32_t> found;
    bvh.query({{499.0f, 499.0f, 499.0f}, {502.0f, 502.0f, 502.0f}}, [&](std::uint32_t i) { found.insert(i); });
    CHECK_EQ(found, (std::set<std::uint32_t>{7}));
    found.clear();
    bvh.query({{20.5f, 0.0f, 0.0f}, {22.5f, 1.0f, 1.0f}}, [&](std::uint32_t i) { found.insert(i); });
    CHECK(found.empty());
}
