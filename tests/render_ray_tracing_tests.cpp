#include "framework.hpp"

// LUNAR_RT_SKIP's names: each feature the opaque shaders' rays can leave
// out, by the bit mesh.frag tests for it.

#include <lunar/render/RayTracing.hpp>

#include <cstdint>

using namespace lunar::render;

TEST(ray_tracing_skip_names_follow_the_shader_bits)
{
    CHECK_EQ(rayTracingSkipName(static_cast<std::uint32_t>(RayTracingSkip::Sun)), std::string_view("sun"));
    CHECK_EQ(rayTracingSkipName(static_cast<std::uint32_t>(RayTracingSkip::Denoise)), std::string_view("denoise"));
    // mesh.frag's skipSkyReach (64): the rays that decide whether a closed
    // room gets the grid's sky.
    CHECK_EQ(static_cast<std::uint32_t>(RayTracingSkip::SkyReach), 64u);
    CHECK_EQ(rayTracingSkipName(static_cast<std::uint32_t>(RayTracingSkip::SkyReach)), std::string_view("sky"));
    CHECK_EQ(rayTracingSkipName(1u << 7), std::string_view(""));
}
