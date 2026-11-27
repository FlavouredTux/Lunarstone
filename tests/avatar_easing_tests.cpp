#include "framework.hpp"

#include <lunar/avatar/Easing.hpp>

#include <cmath>
#include <span>
#include <vector>

#if __cpp_impl_reflection
#include <meta>
#endif

using namespace lunar;
using namespace lunar::avatar;
using Style = Enum::PoseEasingStyle;
using Direction = Enum::PoseEasingDirection;

namespace {

bool near(float a, double b)
{
    return std::abs(static_cast<double>(a) - b) <= 1e-5;
}

} // namespace

// Every value below follows from the client's easing function (0x1034e7d34)
// and the way its keyframe sampler calls it (0x10348d3f0): the result is the
// fraction moved towards the later pose, u the fraction of time elapsed.

#if __cpp_impl_reflection
// Every enumerator of E, found by reflection so a new style can't be missed.
template <class E>
consteval std::span<const E> enumValues()
{
    std::vector<E> values;
    for (std::meta::info e : std::meta::enumerators_of(^^E))
        values.push_back(std::meta::extract<E>(e));
    return std::define_static_array(values);
}
#endif

TEST(avatar_easing_ends_are_exact_for_every_style)
{
#if __cpp_impl_reflection
    for (Style style : enumValues<Style>())
        for (Direction direction : enumValues<Direction>()) {
            CHECK(poseEase(style, direction, 0.0f) == 0.0f);
            CHECK(poseEase(style, direction, 1.0f) == 1.0f);
            CHECK(poseEase(style, direction, -0.5f) == 0.0f);
            CHECK(poseEase(style, direction, 1.5f) == 1.0f);
        }
    CHECK_EQ(enumValues<Style>().size(), 6u);
#endif
}

TEST(avatar_easing_linear)
{
    for (Direction d : {Direction::In, Direction::Out, Direction::InOut})
        CHECK(near(poseEase(Style::Linear, d, 0.3f), 0.3));
}

TEST(avatar_easing_constant_jumps_holds_or_switches)
{
    // In goes straight to the next pose, Out holds the previous one until
    // the next keyframe, InOut switches halfway.
    CHECK(near(poseEase(Style::Constant, Direction::In, 0.01f), 1.0));
    CHECK(near(poseEase(Style::Constant, Direction::Out, 0.99f), 0.0));
    CHECK(near(poseEase(Style::Constant, Direction::InOut, 0.49f), 0.0));
    CHECK(near(poseEase(Style::Constant, Direction::InOut, 0.5f), 1.0));
}

TEST(avatar_easing_cubic_is_reversed_and_cubicv2_is_not)
{
    // Legacy Cubic "In" decelerates (TweenService's Out shape)...
    CHECK(near(poseEase(Style::Cubic, Direction::In, 0.5f), 0.875));
    CHECK(near(poseEase(Style::Cubic, Direction::Out, 0.5f), 0.125));
    CHECK(near(poseEase(Style::Cubic, Direction::InOut, 0.25f), 0.0625));
    CHECK(near(poseEase(Style::Cubic, Direction::InOut, 0.75f), 0.9375));
    // ...and CubicV2 is the corrected copy.
    CHECK(near(poseEase(Style::CubicV2, Direction::In, 0.5f), 0.125));
    CHECK(near(poseEase(Style::CubicV2, Direction::Out, 0.5f), 0.875));
    CHECK(near(poseEase(Style::CubicV2, Direction::InOut, 0.25f), 0.0625));
}

TEST(avatar_easing_bounce)
{
    // bounceOut(0.5) = 7.5625 * (0.5 - 1.5 / 2.75)^2 + 0.75 = 0.765625.
    CHECK(near(poseEase(Style::Bounce, Direction::Out, 0.5f), 0.765625));
    CHECK(near(poseEase(Style::Bounce, Direction::In, 0.5f), 0.234375));
    // InOut uses bounceOut for both halves: 1 - E(0.75) with
    // E(a) = 0.5 * bounceOut(2a - 1) + 0.5 = 0.8828125.
    CHECK(near(poseEase(Style::Bounce, Direction::InOut, 0.25f), 0.1171875));
}

TEST(avatar_easing_elastic)
{
    // Period 0.3, phase 0.075: sin((0.5 - 0.075) * 2pi / 0.3) = 0.5.
    CHECK(near(poseEase(Style::Elastic, Direction::Out, 0.5f), 1.0 + 0.5 / 32.0));
    CHECK(near(poseEase(Style::Elastic, Direction::In, 0.5f), -0.5 / 32.0));
    // InOut: period 0.45, phase 0.1125; at the middle it's exactly half way.
    CHECK(near(poseEase(Style::Elastic, Direction::InOut, 0.5f), 0.5));
    // Elastic overshoots 1 on the way out.
    CHECK(poseEase(Style::Elastic, Direction::Out, 0.2f) > 1.0f);
}
