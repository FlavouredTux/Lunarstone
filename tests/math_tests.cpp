#include "framework.hpp"

#include <lunar/Math.hpp>

#include <cmath>
#include <numbers>

using namespace lunar;

namespace {

constexpr float halfPi = std::numbers::pi_v<float> / 2.0f;

bool near(Vector3 a, Vector3 b, float eps = 1e-5f)
{
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

bool near(const CFrame& a, const CFrame& b, float eps = 1e-5f)
{
    if (!near(a.position, b.position, eps))
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(a.r[i][j] - b.r[i][j]) >= eps)
                return false;
    return true;
}

} // namespace

TEST(vector3_basics)
{
    Vector3 a{1.0f, 2.0f, 3.0f};
    Vector3 b{4.0f, 5.0f, 6.0f};
    CHECK(a + b == (Vector3{5.0f, 7.0f, 9.0f}));
    CHECK(b - a == (Vector3{3.0f, 3.0f, 3.0f}));
    CHECK(a * 2.0f == (Vector3{2.0f, 4.0f, 6.0f}));
    CHECK_EQ(a.dot(b), 32.0f);
    CHECK(Vector3{1, 0, 0}.cross(Vector3{0, 1, 0}) == (Vector3{0, 0, 1}));
    CHECK_EQ((Vector3{3.0f, 4.0f, 0.0f}).magnitude(), 5.0f);
    CHECK(near(Vector3{0.0f, 0.0f, 9.0f}.unit(), Vector3{0.0f, 0.0f, 1.0f}));
}

TEST(cframe_identity_faces_minus_z)
{
    CFrame cf;
    CHECK(cf.rightVector() == (Vector3{1, 0, 0}));
    CHECK(cf.upVector() == (Vector3{0, 1, 0}));
    CHECK(cf.lookVector() == (Vector3{0, 0, -1}));
}

TEST(cframe_angles_matches_roblox)
{
    // CFrame.Angles(0, math.pi/2, 0).LookVector == Vector3.new(-1, 0, 0)
    CHECK(near(CFrame::angles(0.0f, halfPi, 0.0f).lookVector(), {-1.0f, 0.0f, 0.0f}));
    // CFrame.Angles(math.pi/2, 0, 0).LookVector == Vector3.new(0, 1, 0)
    CHECK(near(CFrame::angles(halfPi, 0.0f, 0.0f).lookVector(), {0.0f, 1.0f, 0.0f}));
    // Rx * Ry * Rz ordering
    CFrame combined = CFrame::angles(0.3f, 0.5f, 0.7f);
    CFrame stepwise
        = CFrame::angles(0.3f, 0.0f, 0.0f) * CFrame::angles(0.0f, 0.5f, 0.0f) * CFrame::angles(0.0f, 0.0f, 0.7f);
    CHECK(near(combined, stepwise));
}

TEST(cframe_transforms_points)
{
    CFrame cf = CFrame(10.0f, 0.0f, 0.0f) * CFrame::angles(0.0f, halfPi, 0.0f);
    // One stud forward in local space is one stud along LookVector in world space.
    CHECK(near(cf * Vector3{0.0f, 0.0f, -1.0f}, {9.0f, 0.0f, 0.0f}));
    CHECK(near(cf.pointToObjectSpace({9.0f, 0.0f, 0.0f}), {0.0f, 0.0f, -1.0f}));
    CHECK(near(cf.vectorToWorldSpace({1.0f, 0.0f, 0.0f}), cf.rightVector()));
}

TEST(cframe_inverse_undoes)
{
    CFrame cf = CFrame(1.0f, -2.0f, 3.5f) * CFrame::angles(0.4f, -1.1f, 2.0f);
    CHECK(near(cf * cf.inverse(), CFrame()));
    CHECK(near(cf.inverse() * cf, CFrame()));
    Vector3 p{3.0f, 4.0f, 5.0f};
    CHECK(near(cf.inverse() * (cf * p), p));
}

TEST(cframe_translation)
{
    CFrame cf = CFrame::angles(0.0f, 1.0f, 0.0f) + Vector3{1.0f, 2.0f, 3.0f};
    CHECK(cf.position == (Vector3{1.0f, 2.0f, 3.0f}));
    CHECK((cf - Vector3{1.0f, 2.0f, 3.0f}).position == Vector3{});
}
