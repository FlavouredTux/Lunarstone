#pragma once

#include <lunar/Math.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace lunar::terrain {

// How one face direction of a terrain material is textured, from the
// client's terrain/materials.json (pre-2022 set) or materials2022.json:
// "texture" for every direction, or "texture_top", "texture_side" and
// "texture_bottom" (the bottom falls back to the side).
struct TerrainTextureParams {
    float tiling = 0.5f;   // repeats per cell (4 studs)
    float detiling = 1.0f; // strength of the per-tile offsets that hide repeats
    float rotation = 0.0f; // random rotation of detiled tiles, in turns
    // Height-based blending with neighbouring materials, as the client's
    // terrain shader does it: the material's height h becomes
    // clamp((h - min) / (max - min)), and scale sets how wide the blend is.
    float blendMin = 0.0f;
    float blendMax = 1.0f;
    float blendScale = 1.0f;

    float studsPerTile() const { return 4.0f / tiling; }
};

// Per material from terrain/materialTransitions.json: how its edge against
// other materials looks.
struct TerrainTransitionParams {
    float dominance = 0.0f; // 0..1: how far it spreads over its neighbours
    float materialEdgeBlur = 0.33f;
    float noiseSize = 9.859f; // studs per noise feature along the edge
    float edgeDissolve = 10.0f;
    float emissiveStrength = 0.0f; // texture alpha glows (CrackedLava)
};

struct TerrainMaterialParams {
    std::string name;   // as the file spells it
    Color3 baseColor{}; // the material's default Terrain.MaterialColors entry
    TerrainTextureParams top;
    TerrainTextureParams side;
    TerrainTextureParams bottom;
    TerrainTransitionParams transition;
};

// Indexed like Terrain::materials (Air, Water, Grass, Slate, ...).
inline constexpr std::size_t terrainMaterialCount = 23;
using TerrainMaterialTable = std::array<TerrainMaterialParams, terrainMaterialCount>;

// Reads a materials.json / materials2022.json. Materials are matched by name
// (any case); ones the file leaves out keep the defaults.
std::expected<TerrainMaterialTable, std::string> parseTerrainMaterials(std::string_view json);

// Adds materialTransitions.json's transition and emissive settings.
std::expected<void, std::string> parseTerrainTransitions(std::string_view json, TerrainMaterialTable& table);

} // namespace lunar::terrain
