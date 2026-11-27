#include <lunar/render/Camera.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lunar::render {

namespace {

constexpr float maxPitch = 89.0f * std::numbers::pi_v<float> / 180.0f;

} // namespace

Mat4 perspective(float fovY, float aspect, float nearZ, float farZ)
{
    const float f = 1.0f / std::tan(fovY / 2.0f);
    Mat4 result;
    result.m[0] = {f / aspect, 0.0f, 0.0f, 0.0f};
    result.m[1] = {0.0f, -f, 0.0f, 0.0f};
    result.m[2] = {0.0f, 0.0f, nearZ / (farZ - nearZ), -1.0f};
    result.m[3] = {0.0f, 0.0f, nearZ * farZ / (farZ - nearZ), 0.0f};
    return result;
}

Mat4 Camera::projection(float aspect) const
{
    return perspective(fieldOfView * std::numbers::pi_v<float> / 180.0f, aspect, nearPlane, farPlane);
}

CFrame Camera::lookAt(Vector3 eye, Vector3 target)
{
    const Vector3 look = (target - eye).unit();
    Vector3 right = look.cross({0.0f, 1.0f, 0.0f});
    if (right.magnitude() < 1e-6f)
        right = look.cross({0.0f, 0.0f, -1.0f});
    right = right.unit();
    const Vector3 up = right.cross(look);

    CFrame cf(eye);
    const Vector3 columns[3] = {right, up, -look};
    for (int c = 0; c < 3; ++c) {
        cf.r[0][c] = columns[c].x;
        cf.r[1][c] = columns[c].y;
        cf.r[2][c] = columns[c].z;
    }
    return cf;
}

void FreeFlyCamera::setCFrame(const CFrame& cframe)
{
    position_ = cframe.position;
    const Vector3 look = cframe.lookVector();
    yaw_ = std::atan2(-look.x, -look.z);
    pitch_ = std::clamp(std::asin(std::clamp(look.y, -1.0f, 1.0f)), -maxPitch, maxPitch);
}

CFrame FreeFlyCamera::cframe() const
{
    return CFrame(position_) * CFrame::angles(0.0f, yaw_, 0.0f) * CFrame::angles(pitch_, 0.0f, 0.0f);
}

void FreeFlyCamera::update(const Input& input, float deltaSeconds)
{
    yaw_ -= input.mouseDeltaX * sensitivity;
    yaw_ = std::remainder(yaw_, 2.0f * std::numbers::pi_v<float>);
    pitch_ = std::clamp(pitch_ - input.mouseDeltaY * sensitivity, -maxPitch, maxPitch);

    const CFrame cf = cframe();
    auto axis
        = [](bool positive, bool negative) { return static_cast<float>(positive) - static_cast<float>(negative); };
    const Vector3 move = cf.lookVector() * axis(input.forward, input.back)
                         + cf.rightVector() * axis(input.right, input.left)
                         + Vector3{0.0f, 1.0f, 0.0f} * axis(input.up, input.down);
    if (move.magnitude() < 1e-6f)
        return;

    const float distance = speed * (input.fast ? fastMultiplier : 1.0f) * deltaSeconds;
    position_ = position_ + move.unit() * distance;
}

} // namespace lunar::render
