#pragma once

#include <lunar/Math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The client's smooth terrain mesher, rebuilt from RobloxPlayer 0.742 (macOS
// x86_64). One function (0x105159fb6) meshes a box of voxels for both the
// renderer and physics; this is it, for the voxel input every place without
// Terrain.SmoothVoxelsUpgraded uses.
//
// It is a dual method on the cell centres, not marching cubes: every cube of
// eight neighbouring cell centres whose cells differ (air, water, solid) gets
// one vertex, and every grid edge between two differing cells becomes a quad
// joining the four cubes around it. A vertex is the mean of the points where
// the cube's differing edges are crossed, and an edge is crossed
// (occupancy + 1) / 256 of the way from its fuller end: occupancy is a
// distance, not a density. A full cell next to air puts its surface on the
// air cell's centre, half a cell beyond its own face; a lone full cell is a
// cube two thirds of a cell wide. Materials then move the vertex (Rock
// jitters by a hash of the cube, Brick snaps to the cell corners, Concrete
// and Pavement to a grid), water surfaces drop half a cell, and the result is
// clamped into its cube. Quads split along the diagonal whose ends share a
// material.
//
// Everything here is from the binary unless marked GUESS; the tests pin
// vertex positions taken from running the client's own code.

namespace lunar::terrain {

// One cell as the client keeps it in memory, the same bytes place files store.
struct SmoothCell {
    std::uint8_t material = 0;  // index into Terrain::materials; 0 air, 1 water
    std::uint8_t occupancy = 0; // (occupancy + 1) / 256 of the cell
    std::uint8_t water = 0;     // water filling the rest of a solid cell, same scale
};

// How a material moves its vertices (materialTable.json "shift", "cubify",
// "quantize", "barrel", "water"; the parser at 0x105151c94 stores them at
// entry +0x14 and the amount at +0x18).
enum class Deform : std::uint8_t { None, Shift, Cubify, Quantize, Barrel, Water };

struct MaterialDeform {
    Deform kind = Deform::None;
    float amount = 0.0f;
};

using MaterialDeforms = std::array<MaterialDeform, 23>;

// The tables the client loads: rbxasset://terrain2022/materialTable.json when
// MaterialService.Use2022Materials is on, else rbxasset://terrain/materialTable.json
// (FStringTerrainMaterialTable2022 / Pre2022 hold the same).
const MaterialDeforms& materialDeforms(bool use2022Materials);

// The box to mesh: cells x fastest, then z, then y, the box's cell (0, 0, 0)
// sitting at world cell `origin` (full detail cells, also when the box's
// cells are 2^lod wide). The mesher reads every cell but makes vertices only
// for cubes from box cell 1 to size - 3 and quads only for edges starting at
// box cells 2 to size - 3, so a box two cells bigger than a region on every
// side meshes exactly that region's edges, and neighbouring regions share
// their border vertices.
struct SmoothBox {
    std::array<int, 3> size{};
    std::span<const SmoothCell> cells;
    std::array<int, 3> origin{};
    int lod = 0; // cells are 2^lod world cells wide
};

struct SmoothVertex {
    Vector3 position;          // in world cells: cell (0, 0, 0) spans 0 to 1
    std::uint8_t material = 0; // the vertex's one material (1 on water surfaces)
    bool border = false;       // its cube is on the edge of the meshed range
};

struct SmoothMesh {
    std::vector<SmoothVertex> vertices;
    std::vector<std::uint32_t> indices; // counter-clockwise from outside
};

enum class SmoothPass : std::uint8_t { Solid, Water };

// What a box holds, from the cell classification (0x105156da7's result).
struct SmoothContents {
    bool solid = false;
    bool water = false;
};

// Reusable scratch space; one per thread.
class SmoothMesher {
public:
    // Meshes one pass of the box into `out` (appending). The solid pass
    // treats water as air; the water pass makes the water surfaces.
    SmoothContents mesh(const SmoothBox& box, SmoothPass pass, const MaterialDeforms& deforms, SmoothMesh& out);

private:
    struct Cell {
        std::uint8_t state = 0; // 0 air, 1 water, 2 solid
        std::uint8_t occupancy = 0;
        std::uint8_t flags = 0;
    };
    static constexpr std::uint8_t solidFlag = 0x80; // water pass: the cell is partly solid
    static constexpr int margin = 1;                // box cells read around the meshed cubes

    static std::size_t index(const std::array<int, 3>& size, int x, int y, int z);
    SmoothContents classify(const SmoothBox& box, bool water);
    void settleWater(const std::array<int, 3>& size);
    void makeVertices(const SmoothBox& box, bool water, const MaterialDeforms& deforms, SmoothMesh& out);
    void makeQuads(const std::array<int, 3>& size, bool water, std::uint32_t first, SmoothMesh& out);

    std::vector<Cell> cells_;
    std::vector<Cell> scratch_;
    std::vector<std::uint32_t> cubeVertex_;
};

} // namespace lunar::terrain
