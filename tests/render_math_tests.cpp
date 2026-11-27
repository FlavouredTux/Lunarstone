#include "framework.hpp"

#include <lunar/render/Mat4.hpp>

#include <cmath>
#include <numbers>

using namespace lunar;
using namespace lunar::render;

namespace {

constexpr float halfPi = std::numbers::pi_v<float> / 2.0f;

bool near(float a, float b, float eps = 1e-5f)
{
    return std::abs(a - b) < eps;
}

bool near(Vector3 a, Vector3 b, float eps = 1e-4f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

bool near(const Mat4& a, const Mat4& b, float eps = 1e-5f)
{
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t r = 0; r < 4; ++r)
            if (!near(a.m[c][r], b.m[c][r], eps))
                return false;
    return true;
}

// Clip space to normalized device coordinates.
Vector3 project(const Mat4& m, Vector3 p)
{
    Vec4 clip = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

} // namespace

TEST(mat4_is_column_major)
{
    Mat4 t = Mat4::translation({1.0f, 2.0f, 3.0f});
    // GLSL reads the translation from the fourth column.
    CHECK_EQ(t.m[3][0], 1.0f);
    CHECK_EQ(t(1, 3), 2.0f);
    CHECK(t.transformPoint({1.0f, 1.0f, 1.0f}) == (Vector3{2.0f, 3.0f, 4.0f}));
}

TEST(mat4_multiply_composes_right_to_left)
{
    Mat4 t = Mat4::translation({10.0f, 0.0f, 0.0f});
    Mat4 s = Mat4::scale({2.0f, 3.0f, 4.0f});
    CHECK(near((t * s).transformPoint({1.0f, 1.0f, 1.0f}), {12.0f, 3.0f, 4.0f}));
    CHECK(near((s * t).transformPoint({1.0f, 1.0f, 1.0f}), {22.0f, 3.0f, 4.0f}));
    CHECK(Mat4::identity() * t == t);
}

TEST(mat4_from_cframe_matches_cframe)
{
    CFrame cf = CFrame(1.0f, -2.0f, 3.5f) * CFrame::angles(0.4f, -1.1f, 2.0f);
    Mat4 m = Mat4::fromCFrame(cf);
    for (Vector3 p : {Vector3{}, Vector3{1.0f, 0.0f, 0.0f}, Vector3{-3.0f, 2.0f, 7.0f}})
        CHECK(near(m.transformPoint(p), cf * p));
    CHECK(near(Mat4::fromCFrame(cf) * Mat4::fromCFrame(cf.inverse()), Mat4::identity()));
}

TEST(part_matrix_scales_the_unit_cube)
{
    CFrame cf = CFrame(0.0f, 5.0f, 0.0f) * CFrame::angles(0.0f, halfPi, 0.0f);
    Mat4 m = partMatrix(cf, {4.0f, 1.0f, 2.0f});
    // The +X face of a 4x1x2 part sits 2 studs out along RightVector.
    CHECK(near(m.transformPoint({0.5f, 0.0f, 0.0f}), cf.position + cf.rightVector() * 2.0f));
    CHECK(near(m.transformPoint({0.0f, 0.5f, 0.5f}), cf.position + cf.upVector() * 0.5f - cf.lookVector() * 1.0f));
}

TEST(view_matrix_puts_look_vector_on_minus_z)
{
    CFrame cam = CFrame(5.0f, 1.0f, -2.0f) * CFrame::angles(0.2f, 0.9f, 0.0f);
    Mat4 view = viewMatrix(cam);
    CHECK(near(view.transformPoint(cam.position), {}));
    CHECK(near(view.transformPoint(cam.position + cam.lookVector() * 10.0f), {0.0f, 0.0f, -10.0f}));
    CHECK(near(view.transformPoint(cam.position + cam.rightVector()), {1.0f, 0.0f, 0.0f}));
    CHECK(near(view.transformPoint(cam.position + cam.upVector()), {0.0f, 1.0f, 0.0f}));
}

TEST(perspective_uses_vulkan_reversed_z)
{
    Mat4 proj = perspective(halfPi, 2.0f, 0.5f, 100.0f);
    // Reversed Z: near maps to depth 1, far to 0.
    CHECK(near(project(proj, {0.0f, 0.0f, -0.5f}).z, 1.0f));
    CHECK(near(project(proj, {0.0f, 0.0f, -100.0f}).z, 0.0f));
    CHECK(project(proj, {0.0f, 0.0f, -10.0f}).z > project(proj, {0.0f, 0.0f, -20.0f}).z);
    // A 90 degree FOV: at distance 1 the edges are 1 unit up and 2 across.
    CHECK(near(project(proj, {0.0f, 1.0f, -1.0f}).y, -1.0f)); // up is -Y in Vulkan
    CHECK(near(project(proj, {2.0f, 0.0f, -1.0f}).x, 1.0f));
}
