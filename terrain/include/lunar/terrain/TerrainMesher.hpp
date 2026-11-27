#pragma once

#include <lunar/Math.hpp>
#include <lunar/Mesh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lunar {
class Terrain;
}

namespace lunar::terrain {

// The meshes for one 32x32x32-cell block of terrain, the same blocks place
// files store voxels in, made by the client's own mesher (SmoothMesher.hpp).
// Positions are world studs; solid vertices carry their material colour,
// water vertices Terrain.WaterColor.
//
// Each solid vertex has one material, as in the client, whose shader blends
// a triangle's three: MeshVertex::materialWeights puts all 255 on that
// material's slot in the chunk's palette of Terrain::materials indices.
// Blocks using more than eight materials keep the eight with the most
// vertices and fold the rest into the first.
//
// Level of detail nodes (meshNode) use the same struct: level L covers 2^L
// blocks per side on a grid of 32^3 cells 2^L cells wide.
struct TerrainMeshChunk {
    static constexpr int size = 32; // cells per side

    std::array<int, 3> position{}; // in chunks: covers cells position * 32 up to (position + 1) * 32
    std::uint8_t level = 0;        // a node's: position counts nodes of 2^level chunks
    Vector3 boundsMin;             // of both meshes; a little beyond the block on its low sides
    Vector3 boundsMax;
    MeshData solid;
    MeshData water; // drawn after the solid meshes, transparently
    static constexpr std::size_t paletteCapacity = 8;
    std::array<std::uint8_t, paletteCapacity> palette{}; // Terrain::materials indices for the weights
    std::uint8_t paletteSize = 0;

    bool empty() const { return solid.empty() && water.empty(); }
};

struct MeshOptions {
    unsigned threads = 0; // 0 uses every hardware thread
};

// Meshes every block holding terrain. Blocks that come out empty are left out.
std::vector<TerrainMeshChunk> meshTerrain(const Terrain& terrain, const MeshOptions& options = {});

// One block, empty when there's no surface in it.
TerrainMeshChunk meshChunk(const Terrain& terrain, std::array<int, 3> position);

// Meshes level of detail nodes one after another, reusing its scratch space.
// One per thread; the terrain must not change while it's in use.
class NodeMesher {
public:
    explicit NodeMesher(const Terrain& terrain);
    NodeMesher(const NodeMesher&) = delete;
    NodeMesher& operator=(const NodeMesher&) = delete;
    ~NodeMesher();

    // The node at `position` (in nodes of 2^level chunks). Level 0 is the
    // block meshChunk gives. With skirts, every open border edge of the solid
    // mesh gets a strip hanging two cells into the ground, hiding cracks
    // against neighbours drawn at other levels.
    TerrainMeshChunk mesh(int level, std::array<int, 3> position, bool skirts = true);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Remeshes after an edit to the cells from cellMin to cellMax (inclusive):
// every block whose surface those cells can move, empty ones included so the
// caller can drop what it had for them.
std::vector<TerrainMeshChunk> remeshRegion(const Terrain& terrain, std::array<int, 3> cellMin,
                                           std::array<int, 3> cellMax, const MeshOptions& options = {});

// Totals for logging.
struct MeshStats {
    std::size_t chunks = 0;
    std::size_t solidTriangles = 0;
    std::size_t waterTriangles = 0;
    std::size_t vertices = 0;
    std::size_t bytes = 0; // vertex and index buffers
};

MeshStats measure(const std::vector<TerrainMeshChunk>& chunks);

} // namespace lunar::terrain
