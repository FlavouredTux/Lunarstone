#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Instance.hpp>
#include <lunar/Math.hpp>
#include <lunar/Mesh.hpp>
#include <lunar/render/Camera.hpp>
#include <lunar/render/Mat4.hpp>
#include <lunar/render/Texture.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {
class BasePart;
} // namespace lunar

namespace lunar::render {

using MeshPtr = std::shared_ptr<const MeshData>;

// Geometry for a mesh content id (MeshPart.MeshId, SpecialMesh.MeshId), in
// the mesh's own units, or null while it isn't available. Returning the same
// pointer for the same id lets the renderer upload it once; extractScene
// asks once per id anyway.
using MeshProvider = std::function<MeshPtr(std::string_view meshId)>;

struct Bounds {
    Vector3 min;
    Vector3 max;

    Vector3 center() const { return (min + max) * 0.5f; }
    Vector3 extent() const { return max - min; }
};

// A Highlight over the items that point to it: the fill over everything of
// theirs that shows and an outline around it, both on top of the scene, or
// (alwaysOnTop) seen through whatever is in front of them too.
struct HighlightStyle {
    Color3 fillColor{1.0f, 0.0f, 0.0f};
    float fillTransparency = 0.5f;
    Color3 outlineColor{1.0f, 1.0f, 1.0f};
    float outlineTransparency = 0.0f;
    bool alwaysOnTop = true;
};

// One thing to draw: mesh's vertices transformed by model. Parts that share a
// mesh pointer are drawn together, instanced.
struct DrawItem {
    Mat4 model;
    Color3 color{1.0f, 1.0f, 1.0f}; // times each vertex's colour
    float transparency = 0.0f;
    Enum::Material material = Enum::Material::Plastic;
    std::string materialVariant; // BasePart.MaterialVariant: drawn with that MaterialVariant's maps
    float reflectance = 0.0f;
    MeshPtr mesh; // null is the unit box
    bool castShadow = true;
    // Which of the mesh's levels of detail draw (MeshPart.RenderFidelity);
    // see LodSelector.
    Enum::RenderFidelity renderFidelity = Enum::RenderFidelity::Automatic;

    // Colour map sampled at the mesh's UVs (times uvScale, plus uvOffset),
    // tinted by textureColor and each vertex's colour; alphaMode says what
    // its alpha does. Null draws the plain colour.
    ImagePtr texture;
    Color3 textureColor{1.0f, 1.0f, 1.0f};
    TextureAlpha alphaMode = TextureAlpha::Overlay;
    float uvScale[2]{1.0f, 1.0f};
    float uvOffset[2]{0.0f, 0.0f};
    bool clampUv = false; // stretch the texture once (decals) rather than repeat it

    // SurfaceAppearance: with any of these the item is lit physically
    // (GGX and Lambert) instead of by its material's look.
    ImagePtr normalMap;    // tangent space, green up
    ImagePtr roughnessMap; // red channel
    ImagePtr metalnessMap; // red channel
    bool physical = false; // physically lit even without maps

    // A Decal or Texture: blended onto the surface just behind it after the
    // opaque items, lower zIndex first. Never casts shadows.
    bool decal = false;
    int zIndex = 0;
    // A decal whose part is drawn opaque (no Transparency, no see-through
    // material or texture): its colour goes into the part's before the
    // opaque pass lights it, rather than it being lit again on its own over
    // it. Whoever fades the part afterwards (LocalTransparencyModifier)
    // clears it while the part is see-through.
    bool onSolid = false;

    // SurfaceType per face of a block, two bits per Enum::NormalId (bit
    // 2 * face): 0 smooth, 1 studs, 2 inlet.
    std::uint16_t surfaces = 0;

    // A terrain chunk: its vertices' materialWeights pick from these
    // Terrain::materials indices, drawn as the renderer's terrain materials
    // say (MeshRenderer::setTerrainMaterials). The mesh is in world space.
    bool terrain = false;
    std::array<std::uint8_t, 8> terrainPalette{};
    // Drawn only where a screen-space dither value in [0, 1) falls in
    // [dither[0], dither[1]): terrain cross-fading between levels of detail.
    float dither[2]{0.0f, 1.0f};
    // Terrain water: rippled and reflecting the sky, with the renderer's
    // terrain material 1 (Water) saying how (its normal map and tiling).
    bool terrainWater = false;

    // Boxes in the mesh's space that the light grid takes the item for, as
    // the client voxelizes a MeshPart: the boxes around its collision
    // geometry's convex pieces. Null: the grid works from the mesh itself.
    std::shared_ptr<const std::vector<Bounds>> occluders;

    // The Highlight this item is under, if any. Items sharing one are
    // outlined together; MeshRenderer draws the first maxHighlights.
    std::shared_ptr<const HighlightStyle> highlight;
};

// Two bits of DrawItem::surfaces.
constexpr std::uint16_t surfaceBits(Enum::NormalId face, Enum::SurfaceType type)
{
    const unsigned kind = type == Enum::SurfaceType::Studs ? 1u : type == Enum::SurfaceType::Inlet ? 2u : 0u;
    return static_cast<std::uint16_t>(kind << (2u * static_cast<unsigned>(face)));
}

// Every visible BasePart below root (root included), in tree order, shaped
// like Roblox draws it: Part.Shape, WedgePart, CornerWedgePart, MeshPart
// (fitted to Size), and BlockMesh/CylinderMesh/SpecialMesh children. Meshes
// the provider doesn't have yet draw as boxes of the part's Size. Fully
// transparent parts and Terrain are skipped.
//
// With a texture provider, MeshPart.TextureID, FileMesh.TextureId and
// SurfaceAppearance maps texture their meshes, and Decal and Texture
// children add decal items on their part's faces (even on invisible parts).
// Textures the provider doesn't have yet are left off; extract again once it
// has more.
//
// With owners, owners->at(i) is the part item i was made for, so a caller
// can move items along with parts that move.
std::vector<DrawItem> extractScene(const Instance& root, const MeshProvider& meshProvider = {},
                                   const TextureProvider& textureProvider = {},
                                   std::vector<const Instance*>* owners = nullptr);

// extractScene a part at a time, for scenes that redo only the parts that
// changed. Like one extractScene pass it looks each mesh and texture up
// once, so make a new one once more of them have loaded.
class PartExtractor {
public:
    PartExtractor(MeshProvider meshProvider, TextureProvider textureProvider);
    ~PartExtractor();
    PartExtractor(const PartExtractor&) = delete;
    PartExtractor& operator=(const PartExtractor&) = delete;

    // Appends what extractScene would make for this part alone (its own
    // items, not its descendants'); returns how many.
    std::size_t add(const BasePart& part, std::vector<DrawItem>& out);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// A unit quad facing +Z, x and y in [-0.5, 0.5], u running along +X and v
// down along -Y: what decals are drawn with.
const MeshPtr& decalQuad();

// A large mesh already in world space, such as a terrain chunk: drawn as is,
// lit like the given material. Append it to the items of a frame.
DrawItem worldMesh(MeshPtr mesh, Enum::Material material = Enum::Material::SmoothPlastic,
                   Color3 color = {1.0f, 1.0f, 1.0f});

// Object-space box around a mesh's vertices (its full level of detail's); the
// unit box for null or empty meshes.
Bounds meshBounds(const MeshData* mesh);

// World-space box around every item; nullopt when there are none.
std::optional<Bounds> sceneBounds(std::span<const DrawItem> items);

// Three-quarter view from above that fits every item, for the given width/height.
Camera framingCamera(std::span<const DrawItem> items, float aspect);

} // namespace lunar::render
