#pragma once

#include <cstdint>
#include <span>

// SPIR-V compiled from render/shaders at build time.
namespace lunar::render::shaders {

std::span<const std::uint32_t> meshVert();
std::span<const std::uint32_t> meshFrag();
// mesh.frag with RAYTRACING defined: needs ray queries (Context::rayTracing).
std::span<const std::uint32_t> meshRayFrag();
// The same for the opaque pass and the decals, which also hand the denoiser
// what the local lights' shared shadow ray found (DENOISE).
std::span<const std::uint32_t> meshRayDenoiseFrag();
// The denoiser's blur (compute) and its composite over the scene.
std::span<const std::uint32_t> denoiseComp();
std::span<const std::uint32_t> denoiseCompositeFrag();
// One triangle over the whole target (post.vert).
std::span<const std::uint32_t> fullscreenVert();
std::span<const std::uint32_t> shadowVert();
std::span<const std::uint32_t> shadowFrag();
std::span<const std::uint32_t> skyVert();
std::span<const std::uint32_t> skyFrag();
std::span<const std::uint32_t> mapVert();
std::span<const std::uint32_t> mapFrag();
// Highlights' mask: mesh.vert's items, each pixel the highlight's id.
std::span<const std::uint32_t> highlightMaskFrag();
// Decals on solid parts into the decal layer: mesh.vert's items, each pixel
// their colour, premultiplied.
std::span<const std::uint32_t> decalLayerFrag();

} // namespace lunar::render::shaders
