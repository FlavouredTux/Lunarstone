#pragma once

#include <lunar/Math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lunar {

struct MeshVertex {
    Vector3 position;
    Vector3 normal;
    float u = 0.0f;
    float v = 0.0f;
    Color3 color{1.0f, 1.0f, 1.0f}; // multiplied with the part's colour
    // Terrain only: how much of each of up to eight materials covers the
    // vertex, out of 255, indexed through the mesh's material palette
    // (render::DrawItem::terrainPalette). All zero elsewhere.
    std::array<std::uint8_t, 8> materialWeights{};
};

// A CPU-side triangle mesh in object space, shared by everything that makes
// geometry (terrain, mesh files, primitive shapes) and the renderer that
// draws it. Three indices per triangle; front faces wind counter-clockwise
// when seen from outside, in Roblox's right-handed Y-up space.
//
// indices is the full-detail mesh. Mesh files can carry coarser levels of
// detail too; those follow in lodIndices, level k + 1 (k from 0) being
// lodIndices[k == 0 ? 0 : lodEnds[k - 1], lodEnds[k]), over the same
// vertices. Whatever only reads indices sees the full-detail mesh.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> lodIndices;
    std::vector<std::uint32_t> lodEnds;
    // For levels made by simplifying the full one: how far (in the mesh's
    // units) each strays from it at most, one per entry of lodEnds. Empty
    // when unknown, as for the levels mesh files carry.
    std::vector<float> lodErrors;

    bool empty() const { return indices.empty(); }
    // Levels of detail, the full one included.
    std::size_t levels() const { return 1 + lodEnds.size(); }
};

} // namespace lunar
