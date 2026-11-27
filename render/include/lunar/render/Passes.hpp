#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lunar::render {

// The GPU passes a frame is recorded as, in the order they run. A frame's
// timestamps are one at its start and one after each of these, so a pass
// takes the gap to the next. Named as the passes are in the renderer.
enum class GpuPass : std::uint8_t {
    TextureUpload,
    LightGrid,
    MeshUpload,
    BlasBuild,
    TlasBuild,
    SunShadow,
    LocalShadows,
    Depth,
    DecalLayer, // decals on solid parts, into what the opaque pass reads
    Opaque,
    Sky,
    Decals,
    Denoise, // ray traced: the local lights' shared shadow ray, denoised
    Water,
    Transparent,
    Effects,
    Highlights,
    Post,
    Upscale,
    Overlay,
    Readback,
    Count,
};
inline constexpr std::size_t gpuPassCount = static_cast<std::size_t>(GpuPass::Count);
// One name per pass, in the same order, for a table and for the JSON.
inline constexpr std::array<std::string_view, gpuPassCount> gpuPassNames{
    "texture upload", "light grid",  "mesh upload", "BLAS build", "TLAS build", "sun shadow map", "local shadow atlas",
    "depth prepass",  "decal layer", "opaque",      "sky",        "decals",     "denoise",        "water",
    "transparent",    "effects",     "highlights",  "post",       "upscale",    "GUI overlay",    "readback",
};
// What one frame's passes took on the GPU, in milliseconds: a pass the frame
// did not do is 0.
using PassTimes = std::array<double, gpuPassCount>;

} // namespace lunar::render