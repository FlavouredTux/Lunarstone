#pragma once

// Unit quaternions for blending rotations: CFrame stores a matrix, but
// interpolating and averaging joint rotations is done on quaternions.

#include <lunar/Math.hpp>

#include <cmath>

namespace lunar::avatar {

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    // The rotation part of a CFrame (assumed orthonormal).
    static Quaternion fromCFrame(const CFrame& cf)
    {
        const auto& m = cf.r;
        const float trace = m[0][0] + m[1][1] + m[2][2];
        Quaternion q;
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (m[2][1] - m[1][2]) / s;
            q.y = (m[0][2] - m[2][0]) / s;
            q.z = (m[1][0] - m[0][1]) / s;
        } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
            const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            q.w = (m[2][1] - m[1][2]) / s;
            q.x = 0.25f * s;
            q.y = (m[0][1] + m[1][0]) / s;
            q.z = (m[0][2] + m[2][0]) / s;
        } else if (m[1][1] > m[2][2]) {
            const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            q.w = (m[0][2] - m[2][0]) / s;
            q.x = (m[0][1] + m[1][0]) / s;
            q.y = 0.25f * s;
            q.z = (m[1][2] + m[2][1]) / s;
        } else {
            const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            q.w = (m[1][0] - m[0][1]) / s;
            q.x = (m[0][2] + m[2][0]) / s;
            q.y = (m[1][2] + m[2][1]) / s;
            q.z = 0.25f * s;
        }
        return q.normalized();
    }

    // A CFrame with this rotation at the given position.
    CFrame toCFrame(Vector3 position = {}) const
    {
        CFrame cf(position);
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, xz = x * z, yz = y * z;
        const float wx = w * x, wy = w * y, wz = w * z;
        cf.r[0][0] = 1.0f - 2.0f * (yy + zz);
        cf.r[0][1] = 2.0f * (xy - wz);
        cf.r[0][2] = 2.0f * (xz + wy);
        cf.r[1][0] = 2.0f * (xy + wz);
        cf.r[1][1] = 1.0f - 2.0f * (xx + zz);
        cf.r[1][2] = 2.0f * (yz - wx);
        cf.r[2][0] = 2.0f * (xz - wy);
        cf.r[2][1] = 2.0f * (yz + wx);
        cf.r[2][2] = 1.0f - 2.0f * (xx + yy);
        return cf;
    }

    constexpr float dot(const Quaternion& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }

    Quaternion normalized() const
    {
        const float length = std::sqrt(dot(*this));
        if (length <= 0.0f)
            return {};
        return {x / length, y / length, z / length, w / length};
    }

    constexpr Quaternion operator-() const { return {-x, -y, -z, -w}; }
    friend constexpr Quaternion operator+(const Quaternion& a, const Quaternion& b)
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr Quaternion operator*(const Quaternion& q, float s) { return {q.x * s, q.y * s, q.z * s, q.w * s}; }

    // Spherical interpolation along the shorter arc; falls back to a
    // normalised lerp when the two are nearly equal.
    static Quaternion slerp(const Quaternion& a, Quaternion b, float t)
    {
        float cosine = a.dot(b);
        if (cosine < 0.0f) {
            b = -b;
            cosine = -cosine;
        }
        if (cosine > 0.9995f)
            return (a * (1.0f - t) + b * t).normalized();
        const float angle = std::acos(cosine);
        const float sine = std::sin(angle);
        const float wa = std::sin((1.0f - t) * angle) / sine;
        const float wb = std::sin(t * angle) / sine;
        return (a * wa + b * wb).normalized();
    }
};

// CFrame:ToAxisAngle() as one vector: the axis scaled by the angle, the
// angle in [0, pi].
inline Vector3 toRotationVector(const Quaternion& rotation)
{
    Quaternion q = rotation.normalized();
    if (q.w < 0.0f)
        q = -q;
    const float sine = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (sine < 1e-8f)
        return {};
    const float angle = 2.0f * std::atan2(sine, q.w);
    return Vector3{q.x, q.y, q.z} * (angle / sine);
}

inline Vector3 toRotationVector(const CFrame& cf)
{
    return toRotationVector(Quaternion::fromCFrame(cf));
}

// The rotation by |v| radians about v.
inline Quaternion fromRotationVector(Vector3 v)
{
    const float angle = v.magnitude();
    if (angle < 1e-8f)
        return {};
    const float s = std::sin(angle * 0.5f) / angle;
    return {v.x * s, v.y * s, v.z * s, std::cos(angle * 0.5f)};
}

// Interpolates two CFrames: positions linearly, rotations by slerp. What
// CFrame:Lerp does.
inline CFrame lerp(const CFrame& a, const CFrame& b, float t)
{
    const Quaternion q = Quaternion::slerp(Quaternion::fromCFrame(a), Quaternion::fromCFrame(b), t);
    return q.toCFrame(a.position + (b.position - a.position) * t);
}

} // namespace lunar::avatar
