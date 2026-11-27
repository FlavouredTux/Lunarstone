#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lunar::render {

// One ray tracing feature the shaders can be told to skip (LUNAR_RT_SKIP), as
// the bit it has in the mask the shaders are specialized with.
enum class RayTracingSkip : std::uint32_t { // NOLINT(performance-enum-size): the mask's bits, a uint32
    Sun = 1u << 0,                          // the sun's shadows, which are ray queries in the opaque shader
    AmbientOcclusion = 1u << 1,             // how open the world in front of a surface is
    LocalLights = 1u << 2,                  // the point and spot lights' shadows
    Reflections = 1u << 3,                  // the sharp reflections
    LightGather = 1u << 4,                  // the local lights' whole ray traced gather, shadows and bookkeeping alike
    Denoise = 1u << 5,                      // the local lights' shared shadow ray left as it was found
    SkyReach = 1u << 6,                     // how far out a surface reads the light grid's sky
};

// The features LUNAR_RT_SKIP names, as the mask the opaque shaders are
// specialized with: zero when it is unset, empty or "none", or the bits of
// what it names. Read once, the first time anything asks for it (the pipelines
// are made with it), and never again: the environment after that is not seen.
std::uint32_t rayTracingSkipMask();

// What LUNAR_RT_SKIP is set to, as a line for the log ("LUNAR_RT_SKIP=sun,ao"
// gives "sun,ao"): "none" when it is unset or names nothing it knows. Names it
// doesn't know are ignored (the rest of the list still counts), and the case of
// a name, and the whitespace around it, don't matter.
std::string rayTracingSkipText();

// The name of one feature, for a bit of rayTracingSkipMask's: "sun", "ao",
// "lights", "reflect", "gather", "denoise" or "sky", or "" for a bit of none.
std::string_view rayTracingSkipName(std::uint32_t bit);

} // namespace lunar::render