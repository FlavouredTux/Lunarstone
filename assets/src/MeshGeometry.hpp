#pragma once

// Shared between the mesh parser and the optional Draco decoder.

#include <lunar/Math.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::assets::detail {

// A vertex as the file stores it, before LOD selection and normalising.
struct RawVertex {
    Vector3 position;
    Vector3 normal;
    float u = 0.0f;
    float v = 0.0f;
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
};

struct RawGeometry {
    std::vector<RawVertex> vertices;
    std::vector<std::uint32_t> indices; // three per face, all LODs
};

// Version 7's COREMESH chunk: a Google Draco bitstream.
std::expected<RawGeometry, std::string> decodeDraco(std::string_view bitstream);

} // namespace lunar::assets::detail
