#pragma once

#include <lunar/Classes.hpp>
#include <lunar/assets/MeshLoader.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/physics/Geometry.hpp>
#include <lunar/render/Adornments.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/LocalLights.hpp>
#include <lunar/render/Ribbons.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/terrain/TerrainLod.hpp>
#include <lunar/terrain/TerrainMaterials.hpp>
#include <lunar/ui/ViewportFrames.hpp>

#include <chrono>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Everything drawable in a place: parts (with their meshes and textures as
// they finish loading) plus terrain, meshed in the background at the level
// of detail the camera calls for (updateTerrain).
class PlaceScene {
public:
    explicit PlaceScene(lunar::DataModel& game);
    ~PlaceScene();
    PlaceScene(const PlaceScene&) = delete;
    PlaceScene& operator=(const PlaceScene&) = delete;

    const std::vector<lunar::render::DrawItem>& items() const { return items_; }
    // The part each of the first partItemCount() items was made for (the
    // rest are terrain, with null owners), and a counter that moves each
    // time the parts are extracted again (their indices change then).
    const std::vector<const lunar::Instance*>& owners() const { return owners_; }
    std::size_t partItemCount() const { return partItems_; }
    std::uint64_t extractions() const { return extractions_; }
    const lunar::Terrain* terrain() const { return terrain_; }
    // The Workspace's Beams and Trails, for the renderer (MeshRenderer::addEffects).
    lunar::render::Ribbons& ribbons() { return *ribbons_; }
    // The game's SelectionBoxes and HandleAdornments, likewise.
    lunar::render::Adornments& adornments() { return *adornments_; }
    // The Workspace's PointLights, SpotLights and SurfaceLights, for the
    // renderer (MeshRenderer::setLights).
    lunar::render::LocalLights& lights() { return *lights_; }
    // Terrain.MaterialColors, in Terrain::materials order.
    const std::array<lunar::Color3, lunar::Terrain::materials.size()>& terrainColors() const { return terrainColors_; }

    // Where part textures come from, and a counter that moves whenever more
    // of them have loaded. The scene's own TextureLoader unless replaced.
    // Re-extracts right away.
    void setTextures(lunar::render::TextureProvider provider, std::function<std::uint64_t()> generation);
    // Puts a provider in front of the current one (keeping its generation
    // counter), e.g. one resolving generated textures first.
    void wrapTextures(const std::function<lunar::render::TextureProvider(lunar::render::TextureProvider)>& wrap);
    // The texture provider parts use, for the rest of what's drawn (particles).
    const lunar::render::TextureProvider& textures() const { return textures_; }

    // What ViewportFrames draw their parts with; the scene has to outlive
    // them, or be replaced with ViewportFrames::setAssets({}).
    lunar::ui::ViewportFrames::Assets viewportAssets();

    // Roblox's built-in material textures (pre-2022 or 2022 set, following
    // MaterialService.Use2022Materials), for the renderer's material hook.
    lunar::render::MaterialTextureProvider materialTextures();
    // The terrain's materials: the same TexturePacks, Terrain.MaterialColors
    // and the client's terrain JSON settings, for the renderer's terrain hook.
    lunar::render::TerrainMaterialProvider terrainMaterials();
    // The game's lighting with its Sky's textures as far as they've loaded.
    lunar::render::Lighting lighting();

    // Re-extracts the parts waiting for meshes or textures that finished
    // loading and the parts touched since the last time (all of them after
    // invalidate); true if anything changed.
    bool refresh();
    // A part was added, removed or moved (Workspace's DescendantAdded and
    // DescendantRemoving): it's extracted again, or dropped, on the next
    // refresh. Anything but a BasePart is ignored.
    void touch(const lunar::Instance& instance);
    // Re-extract everything on the next refresh.
    void invalidate() { dirty_ = true; }
    // Moves every item along with its part and fades it by the part's
    // LocalTransparencyModifier, for play mode where parts move each frame.
    void followParts();
    // The same, drawing the parts below `moving` (a character) shifted by
    // `offset`, for interpolating it between physics steps.
    void followParts(const lunar::Instance* moving, lunar::Vector3 offset);
    // Blocks until every requested mesh and texture has loaded or failed,
    // including textures only parts with newly loaded meshes ask for.
    void waitForMeshes();

    // Terrain's levels of detail switch this much farther out (a graphics
    // preset's terrainDistance); call before the first updateTerrain.
    void setTerrainDistance(float scale);

    // Picks the terrain meshes to draw for a camera at `eye` (call once a
    // frame); true if the items changed. Terrain a server streams in waits
    // while the meshers read the cells and goes in from here, at most twice
    // a second, the old meshes drawn until the new ones are ready.
    bool updateTerrain(lunar::Vector3 eye);
    // The same, waiting until everything the camera wants is meshed. Without
    // an eye, the whole terrain at its coarsest, e.g. to frame a view.
    void settleTerrain(std::optional<lunar::Vector3> eye);

    std::size_t pendingMeshes() const { return loader_.pending(); }
    std::size_t pendingTextures() const { return textureLoader_.pending(); }
    // What the terrain draws now; empty stats without terrain.
    lunar::terrain::TerrainLod::Stats terrainStats() const;

private:
    void rebuild();
    void updateTouched();
    // Extracts one part onto the end of items_, noting what it waits for.
    void extractPart(const lunar::BasePart& part, lunar::render::PartExtractor& extractor);
    // Fills the per-item state for the part items from `first` on.
    void preparePartItems(std::size_t first);
    void appendTerrainItems();
    std::pair<lunar::render::MeshProvider, lunar::render::TextureProvider> waitingProviders();
    // Kept from one refresh to the next: it only remembers meshes and
    // textures that were there (each mesh's bounds worked out once). Made
    // anew by every rebuild, the first in the constructor.
    std::unique_ptr<lunar::render::PartExtractor> parts_;
    void addLightOccluders(std::size_t first);
    // Points the items of each enabled Highlight's parts at its look
    // (DrawItem::highlight), and the ones no longer under one at nothing.
    void updateHighlights();
    // Listens for the part's moves and fades (followParts' list).
    void watch(const lunar::BasePart& part);
    void unwatchAll();
    void takeTerrain();
    bool use2022Materials() const;
    void loadTerrainParams();
    lunar::render::SkyTextureIds skyTextureIds() const;

    lunar::DataModel& game_;
    std::shared_ptr<lunar::assets::AssetCache> cache_;
    lunar::assets::MeshLoader loader_;
    lunar::assets::TextureLoader textureLoader_;
    lunar::Terrain* terrain_ = nullptr;
    std::unique_ptr<lunar::terrain::TerrainLod> terrainLod_;
    lunar::terrain::LodOptions terrainOptions_;
    // Streamed-in terrain: when it last went in, and whether the meshes for it are still coming.
    std::chrono::steady_clock::time_point terrainWritten_;
    bool terrainRemeshing_ = false;
    bool applyTerrainWrites();
    std::vector<lunar::render::DrawItem> terrainItems_; // the terrain's current selection
    std::vector<lunar::render::DrawItem> items_;        // parts, then the terrain items
    std::size_t partItems_ = 0;
    std::vector<const lunar::Instance*> owners_; // the part each item was made for, null for terrain
    std::vector<lunar::render::Mat4> local_;     // each item's model in its part's space
    // Mesh parts' collision geometry, which the light grid voxelizes them as.
    lunar::physics::MeshGeometryCache collision_;
    std::vector<float> baseTransparency_;
    std::vector<std::uint8_t> baseOnSolid_; // DrawItem::onSolid as extracted, before any fade
    // What each item's model and transparency were last computed from, so
    // followParts only touches the parts that moved or faded.
    std::vector<lunar::CFrame> placed_;
    std::vector<float> faded_;
    // Which items belong to followParts' `moving` instance, worked out again
    // when it changes or the items do.
    std::vector<std::uint8_t> following_;
    const lunar::Instance* followingOf_ = nullptr;
    bool followingStale_ = true;
    // The part items from here on are new since the last followParts
    // (updateTouched puts them at the end): placed and followed then.
    std::size_t fresh_ = 0;
    bool dirty_ = false;
    // Parts to extract again (or drop) on the next refresh, by address; weak,
    // since they may be gone by then.
    std::unordered_map<const lunar::Instance*, std::weak_ptr<const lunar::Instance>> touched_;
    // The parts each mesh and texture was asked for by while it wasn't there.
    using Waiters = std::vector<std::pair<const lunar::Instance*, std::weak_ptr<const lunar::Instance>>>;
    std::unordered_map<std::string, Waiters> waitingMeshes_;
    std::unordered_map<std::string, Waiters> waitingTextures_;
    const lunar::BasePart* extracting_ = nullptr; // the part the providers are asked for
    // Every extracted part's Changed, noting moves and fades in moved_ for
    // followParts, which then only places those (and the moving
    // character's): checking all 20k parts a frame took 0.65 ms on Duvall.
    std::unordered_map<const lunar::Instance*, lunar::Connection> watching_;
    std::vector<const lunar::Instance*> moved_;
    // Each part's items: first index and count, worked out again after the
    // items change.
    struct Range {
        const lunar::Instance* part;
        std::size_t first, count;
    };
    std::vector<Range> ranges_; // sorted by part
    // A part's items (first, count); none for what isn't an extracted part.
    std::pair<std::size_t, std::size_t> rangeOf(const lunar::Instance* part) const;
    bool rangesStale_ = true;
    std::vector<std::size_t> followingItems_; // the items following_ marks
    // Every Highlight in the game (they can be anywhere, Adornee naming
    // what they cover), and the items they covered last frame, as of
    // which extraction.
    std::vector<std::weak_ptr<const lunar::Highlight>> highlights_;
    lunar::Connection highlightAdded_;
    std::vector<std::size_t> highlighted_;
    std::uint64_t highlightedExtraction_ = 0;
    // Which items this frame's covers tagged (by stamp), and each enabled
    // Highlight's look as last made, by address.
    std::vector<std::uint32_t> tagged_;
    std::uint32_t tagStamp_ = 0;
    std::vector<std::pair<const lunar::Highlight*, std::shared_ptr<const lunar::render::HighlightStyle>>> styles_;
    lunar::render::TextureProvider textures_;
    std::function<std::uint64_t()> textureGeneration_;
    std::uint64_t generation_ = 0;
    std::uint64_t extractions_ = 0;
    std::uint64_t textureGenerationSeen_ = 0;
    std::unique_ptr<lunar::render::Ribbons> ribbons_;
    std::unique_ptr<lunar::render::Adornments> adornments_;
    std::unique_ptr<lunar::render::LocalLights> lights_;
    lunar::terrain::TerrainMaterialTable terrainParams_{};
    std::array<lunar::Color3, lunar::Terrain::materials.size()> terrainColors_{};
    float waterWaveSize_ = 0.15f;
    float waterWaveSpeed_ = 10.0f;
    float waterReflectance_ = 1.0f;
};
