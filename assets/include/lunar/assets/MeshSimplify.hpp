#pragma once

#include <lunar/Mesh.hpp>

#include <array>
#include <cstddef>

namespace lunar::assets {

// The share of the full mesh's triangles each coarser level aims for: the
// client's own mesh simplifier's FInt CSGv2LodLevel1Percentage to ...4 (50,
// 25, 10 and 5).
inline constexpr std::array<float, 4> lodTriangleShares{0.5f, 0.25f, 0.1f, 0.05f};

// Meshes with fewer triangles than this keep their one level. GUESS: the
// client's FInt CSGv2LodLevel1Count (50) reads like this floor.
inline constexpr std::size_t lodMinimumTriangles = 50;

// Coarser levels of detail for a mesh that has none (mesh.lodEnds empty),
// standing in for the ones the client streams for meshes it transcoded on
// Roblox's servers (MeshStreamingAndImprovedLods): quadric error edge
// collapses down to each of lodTriangleShares, every level an index list
// over the mesh's own vertices (MeshData::lodIndices). Each level's
// MeshData::lodErrors is how far its surface strays from the full one, which
// picks it by screen size. Normals may split anywhere (flat shading); open
// borders and UV or colour seams only slide along themselves, and corners
// where they meet stay put. A level that can't get much below the one before
// is left out, along with the coarser ones.
void generateLods(MeshData& mesh);

} // namespace lunar::assets
