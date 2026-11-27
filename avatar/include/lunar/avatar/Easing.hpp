#pragma once

// Pose easing: how a joint moves from one keyframe's pose to the next.
//
// Source: RobloxPlayer 0.742 (x86_64). The easing function proper is at
// 0x1034e7d34 (style - 1 indexes a jump table at 0x1034e80f0; bounce helper
// at 0x1034e7c8e). The keyframe sampler (0x10348d3f0) calls it with
// a = (tB - t) / (tB - tA), the fraction of the span still to go, and
// blends A * E + B * (1 - E). So the fraction moved towards B is
// 1 - E(1 - u), with u the fraction elapsed, and the style and direction
// are the earlier pose's. The consequences, pinned in the tests:
//   - Cubic has In and Out reversed against TweenService (Pose "In" eases
//     out); CubicV2 is the corrected copy. Elastic and Bounce are not
//     reversed: their In is TweenService's In.
//   - Constant In jumps straight to the next pose, Out holds the previous
//     one until the next keyframe, InOut switches halfway.
//   - Bounce InOut uses bounceOut for both halves.

#include <lunar/Enums.hpp>

#include <cmath>
#include <numbers>

namespace lunar::avatar {

namespace easing {

// Penner's bounceOut with Roblox's constants (0x1034e7c8e).
constexpr double bounceOut(double a)
{
    constexpr double n = 7.5625;
    constexpr double d = 2.75;
    if (a < 1.0 / d)
        return n * a * a;
    if (a < 2.0 / d) {
        a -= 1.5 / d;
        return n * a * a + 0.75;
    }
    if (a < 2.5 / d) {
        a -= 2.25 / d;
        return n * a * a + 0.9375;
    }
    a -= 2.625 / d;
    return n * a * a + 0.984375;
}

constexpr double cube(double a)
{
    return a * a * a;
}

// The raw function E(style, direction, a) of 0x1034e7d34, a being the
// fraction of the span still to go.
inline double raw(Enum::PoseEasingStyle style, Enum::PoseEasingDirection direction, double a)
{
    using enum Enum::PoseEasingDirection;
    constexpr double tau = 2.0 * std::numbers::pi;
    constexpr double k = tau / 0.3;    // Elastic period 0.3, phase 0.075
    constexpr double kIo = tau / 0.45; // Elastic InOut period 0.45, phase 0.1125
    switch (style) {
    case Enum::PoseEasingStyle::Linear:
        return a;
    case Enum::PoseEasingStyle::Constant:
        // Expressed through E so that 1 - E(1 - u) gives the table above.
        if (direction == In)
            return a < 1.0 ? 0.0 : 1.0;
        if (direction == Out)
            return a > 0.0 ? 1.0 : 0.0;
        return a > 0.5 ? 1.0 : 0.0;
    case Enum::PoseEasingStyle::Cubic:
        // Double precision pow in the binary.
        if (direction == In)
            return cube(a);
        if (direction == Out)
            return 1.0 - cube(1.0 - a);
        return a < 0.5 ? 4.0 * cube(a) : 1.0 - cube(2.0 - 2.0 * a) / 2.0;
    case Enum::PoseEasingStyle::CubicV2: {
        // Single precision powf in the binary.
        const auto af = static_cast<float>(a);
        if (direction == In)
            return 1.0 - static_cast<double>(std::pow(1.0f - af, 3.0f));
        if (direction == Out)
            return static_cast<double>(std::pow(af, 3.0f));
        return af < 0.5f ? 4.0 * static_cast<double>(std::pow(af, 3.0f))
                         : 1.0 - static_cast<double>(std::pow(2.0f - 2.0f * af, 3.0f)) / 2.0;
    }
    case Enum::PoseEasingStyle::Elastic:
        if (direction == In)
            return 1.0 + std::pow(2.0, -10.0 * a) * std::sin((a - 0.075) * k);
        if (direction == Out)
            return -std::pow(2.0, -10.0 * (1.0 - a)) * std::sin(((1.0 - a) - 0.075) * k);
        {
            const double w = 2.0 * a - 1.0;
            const double s = std::sin((w - 0.1125) * kIo);
            return a < 0.5 ? -0.5 * std::pow(2.0, 10.0 * w) * s : 0.5 * std::pow(2.0, -10.0 * w) * s + 1.0;
        }
    case Enum::PoseEasingStyle::Bounce:
        if (direction == In)
            return bounceOut(a);
        if (direction == Out)
            return 1.0 - bounceOut(1.0 - a);
        return a < 0.5 ? 0.5 * bounceOut(2.0 * a) : 0.5 * bounceOut(2.0 * a - 1.0) + 0.5;
    }
    return a; // styles out of range behave as Linear
}

} // namespace easing

// The fraction of the way from the earlier pose to the later one (possibly
// outside [0, 1] for Elastic) at fraction `alpha` of the time between their
// keyframes, with the earlier pose's style and direction. The ends are exact:
// at or past the later keyframe it's 1, at or before the earlier one 0.
inline float poseEase(Enum::PoseEasingStyle style, Enum::PoseEasingDirection direction, float alpha)
{
    if (alpha >= 1.0f)
        return 1.0f;
    if (alpha <= 0.0f)
        return 0.0f;
    return static_cast<float>(1.0 - easing::raw(style, direction, 1.0 - static_cast<double>(alpha)));
}

} // namespace lunar::avatar
