#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Image.hpp>
#include <lunar/Math.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace lunar::render {

using ImagePtr = std::shared_ptr<const lunar::Image>;

// The decoded image for a texture content id (Decal.Texture, MeshPart.TextureID,
// SurfaceAppearance maps, ...), or null while it isn't available. Must not
// block. The renderer uploads each distinct pointer once, so returning the
// same pointer for the same id matters; assets::TextureLoader's provider has
// exactly this shape.
using TextureProvider = std::function<ImagePtr(std::string_view contentId)>;

// What a built-in material looks like when real textures are known for it.
// Both maps tile across parts and terrain by triplanar projection.
struct MaterialTextures {
    ImagePtr color;  // sRGB, multiplies the part's colour; null keeps the procedural pattern
    ImagePtr normal; // tangent space, green up; optional
    float studsPerTile = 4.0f;
    // The colour map's alpha says where the part's colour applies (and
    // where the texture keeps its own, like mortar between bricks); without
    // it the part's colour multiplies the whole texture.
    bool tintMask = false;
};

// Textures for a material, or empty ones to keep the procedural look; variant
// is the MaterialVariant its parts name (empty for none). Asked once per
// material and variant per frame, so it should be a cheap lookup.
using MaterialTextureProvider = std::function<MaterialTextures(Enum::Material material, std::string_view variant)>;

// How one terrain material (an entry of Terrain::materials) is drawn: its
// colour and normal maps tiled by triplanar projection, the colour from
// Terrain.MaterialColors, and the client's terrain JSON parameters for
// blending it with its neighbours.
struct TerrainLayer {
    ImagePtr color;                // null draws the plain colour
    ImagePtr normal;               // optional
    Color3 tint{1.0f, 1.0f, 1.0f}; // sRGB material colour
    bool tintMask = false;         // see MaterialTextures::tintMask
    float studsPerTileTop = 8.0f;  // upward faces...
    float studsPerTileSide = 8.0f; // ...and the rest
    float blendMin = 0.0f;         // height-based blending, see terrain::TerrainTextureParams
    float blendMax = 1.0f;
    float blendScale = 1.0f;
    float dominance = 0.0f;   // materialTransitions.json: spreads over neighbours
    float edgeNoise = 0.0f;   // ...and how much noise breaks up its edges
    float noiseStuds = 10.0f; // ...at this feature size
    // Water (index 1) only: normal is the ripple map, studsPerTileTop its
    // size; how far it bends the surface and how much sky it reflects.
    float waveStrength = 0.0f;
    float reflectance = 0.0f;
    // Water only: the client's animated ripple maps, played in a loop at
    // frameRate frames a second, and the swell moving the surface
    // (Terrain.WaterWaveSize and WaterWaveSpeed).
    std::vector<ImagePtr> normalFrames;
    float frameRate = 0.0f;
    float waveHeight = 0.0f; // studs
    float waveSpeed = 0.0f;  // radians a second
};

// The look of terrain material index i (0..22), asked once per frame per
// material while terrain is drawn. Should be a cheap lookup.
using TerrainMaterialProvider = std::function<TerrainLayer(std::size_t index)>;

// A one-stud tile for a part surface (Studs, Inlet), drawn once per stud
// across faces with that SurfaceType. Mid grey leaves the part's colour as
// it is; darker and lighter shade it. Null keeps the procedural studs.
using SurfaceTextureProvider = std::function<ImagePtr(Enum::SurfaceType surface)>;

// How a colour map's alpha is used (Enum::AlphaMode, plus how plain
// MeshPart and SpecialMesh textures behave).
enum class TextureAlpha : std::uint8_t {
    Overlay,      // the texture is laid over the item's colour by its alpha
    Transparency, // alpha is see-through
    TintMask,     // alpha says where the texture tint applies
    Opaque,       // alpha is ignored
};

} // namespace lunar::render
