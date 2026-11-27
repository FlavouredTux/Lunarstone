#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/Mat4.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace lunar::render {

// The view volume's six planes (inside is positive, normals of unit length),
// from a reversed-Z Vulkan view-projection: -w <= x, y <= w and 0 <= z <= w.
// What everything that draws culls against.
struct Frustum {
    std::array<Vec4, 6> planes;

    explicit Frustum(const Mat4& vp)
    {
        auto row = [&](std::size_t r) { return Vec4{vp.m[0][r], vp.m[1][r], vp.m[2][r], vp.m[3][r]}; };
        const Vec4 x = row(0);
        const Vec4 y = row(1);
        const Vec4 z = row(2);
        const Vec4 w = row(3);
        auto plus = [](Vec4 a, Vec4 b) { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
        auto minus = [](Vec4 a, Vec4 b) { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; };
        planes = {plus(w, x), minus(w, x), plus(w, y), minus(w, y), z, minus(w, z)};
        for (Vec4& p : planes) {
            const float length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            p = {p.x / length, p.y / length, p.z / length, p.w / length};
        }
    }

    // Whether a sphere can be in view: false only when it's wholly outside a plane.
    bool intersects(Vector3 center, float radius) const
    {
        for (const Vec4& p : planes)
            if (p.x * center.x + p.y * center.y + p.z * center.z + p.w < -radius)
                return false;
        return true;
    }

    // The same for an axis-aligned box: false only when its corner farthest
    // in along a plane's normal is still outside it.
    bool intersectsBox(Vector3 center, Vector3 halfExtent) const
    {
        for (const Vec4& p : planes)
            if (p.x * center.x + p.y * center.y + p.z * center.z + p.w + std::abs(p.x) * halfExtent.x
                    + std::abs(p.y) * halfExtent.y + std::abs(p.z) * halfExtent.z
                < 0.0f)
                return false;
        return true;
    }
};

} // namespace lunar::render
