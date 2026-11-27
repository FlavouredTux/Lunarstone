#pragma once

#include <lunar/render/Camera.hpp>
#include <lunar/render/LocalLights.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace lunar::render {

// Which local lights reach each cluster of the view: screen tiles of
// tilePixels on a side, cut by depth into slices the way the client's
// froxel grid is (its Future shaders): slice = log2(depth * 0.1) * 1.7095 + 1,
// so the first slice ends 10 studs out and each after it is 1.5 times as
// deep as the one before. The shaders find a pixel's cluster and walk its
// lights.
//
// cells holds a word per cluster (x fastest, then y, then slice): the
// count of its lights in the low 12 bits and where they start in indices
// above them, as the client packs its LightGrid texels. indices lists
// lights by their index in the span given to build.
struct LightClusters {
    static constexpr std::uint32_t tilePixels = 64;
    static constexpr std::uint32_t slices = 16;
    static constexpr float sliceScale = 1.7095f;  // [shader: DefaultUnifiedPlasticFS, Metal]
    static constexpr float sliceDepth = 0.1f;     // per stud, inside the log
    static constexpr std::uint32_t maxPerCluster = 4095;
    static constexpr std::uint32_t maxIndices = 1u << 20;

    std::uint32_t tilesX = 0;
    std::uint32_t tilesY = 0;
    std::vector<std::uint32_t> cells;
    std::vector<std::uint32_t> indices;

    // The slice a view depth (studs along the view) falls in.
    static std::uint32_t sliceOf(float depth);
    // The nearest depth of a slice.
    static float sliceStart(std::uint32_t slice);

    // Bins the lights for a camera drawing width x height pixels.
    void build(std::span<const LocalLight> lights, const Camera& camera, std::uint32_t width, std::uint32_t height);

    // The lights of the cluster a pixel at depth is in (for tests).
    std::span<const std::uint32_t> at(std::uint32_t x, std::uint32_t y, float depth) const;
};

} // namespace lunar::render
