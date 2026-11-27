#pragma once

#include <lunar/Math.hpp>

#include <array>
#include <cstddef>

namespace lunar::render {

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

// 4x4 float matrix stored column-major, the layout GLSL expects: m[c][r] is
// column c, row r. Transforms column vectors (M * v).
struct Mat4 {
    std::array<std::array<float, 4>, 4> m{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};

    static constexpr Mat4 identity() { return {}; }

    static constexpr Mat4 translation(Vector3 t)
    {
        Mat4 result;
        result.m[3] = {t.x, t.y, t.z, 1.0f};
        return result;
    }

    static constexpr Mat4 scale(Vector3 s)
    {
        Mat4 result;
        result.m[0][0] = s.x;
        result.m[1][1] = s.y;
        result.m[2][2] = s.z;
        return result;
    }

    // The rigid transform a CFrame describes, object space to world space.
    static constexpr Mat4 fromCFrame(const CFrame& cf)
    {
        Mat4 result;
        for (std::size_t c = 0; c < 3; ++c)
            for (std::size_t r = 0; r < 3; ++r)
                result.m[c][r] = cf.r[r][c];
        result.m[3] = {cf.position.x, cf.position.y, cf.position.z, 1.0f};
        return result;
    }

    constexpr float operator()(std::size_t row, std::size_t col) const { return m[col][row]; }

    constexpr Vec4 operator*(Vec4 v) const
    {
        Vec4 out;
        out.x = m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w;
        out.y = m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w;
        out.z = m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w;
        out.w = m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w;
        return out;
    }

    constexpr Vector3 transformPoint(Vector3 p) const
    {
        Vec4 v = *this * Vec4{p.x, p.y, p.z, 1.0f};
        return {v.x, v.y, v.z};
    }

    friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 result;
        for (std::size_t c = 0; c < 4; ++c)
            for (std::size_t r = 0; r < 4; ++r)
                result.m[c][r]
                    = a.m[0][r] * b.m[c][0] + a.m[1][r] * b.m[c][1] + a.m[2][r] * b.m[c][2] + a.m[3][r] * b.m[c][3];
        return result;
    }

    friend constexpr bool operator==(const Mat4&, const Mat4&) = default;
};

// Model matrix of a part: a unit cube centred on the origin, scaled to size,
// then placed by the CFrame.
constexpr Mat4 partMatrix(const CFrame& cf, Vector3 size)
{
    return Mat4::fromCFrame(cf) * Mat4::scale(size);
}

// World to view space for a camera at cf. View space is the camera's object
// space: X right, Y up, looking down -Z (along the CFrame's LookVector).
constexpr Mat4 viewMatrix(const CFrame& cf)
{
    return Mat4::fromCFrame(cf.inverse());
}

// View space to Vulkan clip space with reversed Z: the near plane maps to
// depth 1 and the far plane to 0, which spreads float precision evenly over
// distance. Clip Y points down, so the image comes out upright. fovY is the
// vertical field of view in radians.
Mat4 perspective(float fovY, float aspect, float nearZ, float farZ);

} // namespace lunar::render
