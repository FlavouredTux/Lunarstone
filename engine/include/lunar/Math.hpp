#pragma once

#include <cmath>
#include <cstdint>

namespace lunar {

struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr float dot(Vector2 o) const { return x * o.x + y * o.y; }
    constexpr float cross(Vector2 o) const { return x * o.y - y * o.x; }
    float magnitude() const { return std::sqrt(dot(*this)); }
    Vector2 unit() const { return *this / magnitude(); }

    constexpr Vector2 operator-() const { return {-x, -y}; }
    friend constexpr bool operator==(const Vector2&, const Vector2&) = default;
    friend constexpr Vector2 operator+(Vector2 a, Vector2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vector2 operator-(Vector2 a, Vector2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vector2 operator*(Vector2 v, float s) { return {v.x * s, v.y * s}; }
    friend constexpr Vector2 operator*(float s, Vector2 v) { return v * s; }
    friend constexpr Vector2 operator/(Vector2 v, float s) { return {v.x / s, v.y / s}; }
};

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr float dot(Vector3 o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vector3 cross(Vector3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float magnitude() const { return std::sqrt(dot(*this)); }
    Vector3 unit() const { return *this / magnitude(); }

    constexpr Vector3 operator-() const { return {-x, -y, -z}; }
    friend constexpr bool operator==(const Vector3&, const Vector3&) = default;
    friend constexpr Vector3 operator+(Vector3 a, Vector3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vector3 operator-(Vector3 a, Vector3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vector3 operator*(Vector3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
    friend constexpr Vector3 operator*(float s, Vector3 v) { return v * s; }
    friend constexpr Vector3 operator/(Vector3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }
};

// Integer vector, used by a few legacy properties and terrain.
struct Vector3int16 {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t z = 0;

    friend constexpr bool operator==(const Vector3int16&, const Vector3int16&) = default;
};

struct Color3 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    static constexpr Color3 fromRGB(int red, int green, int blue)
    {
        return {static_cast<float>(red) / 255.0f, static_cast<float>(green) / 255.0f,
                static_cast<float>(blue) / 255.0f};
    }

    friend constexpr bool operator==(const Color3&, const Color3&) = default;
};

// Position plus rotation, stored like Roblox does: a row-major 3x3 matrix whose
// columns are RightVector, UpVector and -LookVector.
struct CFrame {
    Vector3 position{};
    float r[3][3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    constexpr CFrame() = default;
    constexpr explicit CFrame(Vector3 pos) : position(pos) {}
    constexpr CFrame(float x, float y, float z) : position{x, y, z} {}

    // CFrame.Angles / fromEulerAnglesXYZ: R = Rx * Ry * Rz.
    static CFrame angles(float rx, float ry, float rz)
    {
        const float cx = std::cos(rx), sx = std::sin(rx);
        const float cy = std::cos(ry), sy = std::sin(ry);
        const float cz = std::cos(rz), sz = std::sin(rz);
        CFrame cf;
        cf.r[0][0] = cy * cz;
        cf.r[0][1] = -cy * sz;
        cf.r[0][2] = sy;
        cf.r[1][0] = cx * sz + sx * sy * cz;
        cf.r[1][1] = cx * cz - sx * sy * sz;
        cf.r[1][2] = -sx * cy;
        cf.r[2][0] = sx * sz - cx * sy * cz;
        cf.r[2][1] = sx * cz + cx * sy * sz;
        cf.r[2][2] = cx * cy;
        return cf;
    }

    // CFrame.fromOrientation: R = Ry * Rx * Rz, the order Orientation uses.
    static CFrame fromOrientation(float rx, float ry, float rz)
    {
        return angles(0.0f, ry, 0.0f) * angles(rx, 0.0f, 0.0f) * angles(0.0f, 0.0f, rz);
    }

    // CFrame:ToOrientation(): the fromOrientation angles, in radians.
    Vector3 toOrientation() const
    {
        float sx = -r[1][2];
        float rx = std::asin(sx < -1.0f ? -1.0f : (sx > 1.0f ? 1.0f : sx));
        // + 0.0f turns -0 into 0.
        return {rx + 0.0f, std::atan2(r[0][2], r[2][2]) + 0.0f, std::atan2(r[1][0], r[1][1]) + 0.0f};
    }

    // Only the rotation, at the origin.
    constexpr CFrame rotation() const
    {
        CFrame result = *this;
        result.position = {};
        return result;
    }

    constexpr Vector3 rightVector() const { return {r[0][0], r[1][0], r[2][0]}; }
    constexpr Vector3 upVector() const { return {r[0][1], r[1][1], r[2][1]}; }
    constexpr Vector3 lookVector() const { return {-r[0][2], -r[1][2], -r[2][2]}; }

    constexpr Vector3 vectorToWorldSpace(Vector3 v) const
    {
        return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z,
                r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z};
    }
    constexpr Vector3 vectorToObjectSpace(Vector3 v) const
    {
        return {r[0][0] * v.x + r[1][0] * v.y + r[2][0] * v.z, r[0][1] * v.x + r[1][1] * v.y + r[2][1] * v.z,
                r[0][2] * v.x + r[1][2] * v.y + r[2][2] * v.z};
    }
    constexpr Vector3 pointToWorldSpace(Vector3 p) const { return vectorToWorldSpace(p) + position; }
    constexpr Vector3 pointToObjectSpace(Vector3 p) const { return vectorToObjectSpace(p - position); }

    // Whether the rotation is one: unit columns at right angles, to within
    // what rounding leaves after a few products. inverse() is only right
    // for those.
    constexpr bool orthonormal(float tolerance = 1e-5f) const
    {
        const Vector3 x = rightVector(), y = upVector(), z{r[0][2], r[1][2], r[2][2]};
        const float errors[6] = {x.dot(x) - 1.0f, y.dot(y) - 1.0f, z.dot(z) - 1.0f, x.dot(y), x.dot(z), y.dot(z)};
        for (float e : errors)
            if (!(e <= tolerance && e >= -tolerance)) // NaN fails too
                return false;
        return true;
    }

    constexpr CFrame inverse() const
    {
        CFrame result;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                result.r[i][j] = r[j][i];
        result.position = -result.vectorToWorldSpace(position);
        return result;
    }

    // What a part's CFrame becomes as it's set (BasePart.CFrame in Studio):
    // the rotation made one by Gram-Schmidt over its columns, right, then up,
    // then back -- projected, not crossed, so a left-handed one stays so. One
    // that can't be (a column zero, two of them parallel, or not finite) is
    // the identity, and a position that isn't finite is (0, -1e6, 0), under
    // where parts fall out of the world. All of it as Studio does it.
    CFrame orthonormalized() const
    {
        CFrame result = *this;
        const bool placed = std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
        if (!placed)
            result.position = {0.0f, -1e6f, 0.0f};
        Vector3 columns[3] = {rightVector(), upVector(), {r[0][2], r[1][2], r[2][2]}};
        // One that already is (to a millionth) stays exactly as it is: the
        // error a set leaves can't add up, it's taken out past that.
        constexpr float tolerance = 1e-6f;
        auto off = [](float value, float want) { return !(std::abs(value - want) <= tolerance); };
        if (!off(columns[0].dot(columns[0]), 1) && !off(columns[1].dot(columns[1]), 1) && !off(columns[2].dot(columns[2]), 1)
            && !off(columns[0].dot(columns[1]), 0) && !off(columns[0].dot(columns[2]), 0) && !off(columns[1].dot(columns[2]), 0))
            return result;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < i; ++j)
                columns[i] = columns[i] - columns[j] * columns[j].dot(columns[i]);
            const float length = columns[i].magnitude();
            if (!(length > 1e-6f) || !std::isfinite(length)) {
                result.r[0][0] = result.r[1][1] = result.r[2][2] = 1.0f;
                result.r[0][1] = result.r[0][2] = result.r[1][0] = result.r[1][2] = result.r[2][0] = result.r[2][1] = 0.0f;
                return result;
            }
            columns[i] = columns[i] / length;
        }
        for (int j = 0; j < 3; ++j) {
            result.r[0][j] = columns[j].x;
            result.r[1][j] = columns[j].y;
            result.r[2][j] = columns[j].z;
        }
        return result;
    }

    friend constexpr CFrame operator*(const CFrame& a, const CFrame& b)
    {
        CFrame result;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                result.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        result.position = a.pointToWorldSpace(b.position);
        return result;
    }
    friend constexpr Vector3 operator*(const CFrame& cf, Vector3 p) { return cf.pointToWorldSpace(p); }
    friend constexpr CFrame operator+(CFrame cf, Vector3 v)
    {
        cf.position = cf.position + v;
        return cf;
    }
    friend constexpr CFrame operator-(CFrame cf, Vector3 v)
    {
        cf.position = cf.position - v;
        return cf;
    }
    friend constexpr bool operator==(const CFrame&, const CFrame&) = default;
};

} // namespace lunar
