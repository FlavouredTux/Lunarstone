#include "PlaceScene.hpp"

#include <lunar/render/BuiltinMaterials.hpp>

#include <print>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string assetUri(std::uint64_t id)
{
    return std::format("rbxassetid://{}", id);
}

// Studs one tile of a built-in material texture covers. A guess: eight
// studs at the packs' <tiling> of 1, which puts the 2022 WoodPlanks boards
// a stud apart.
constexpr float studsPerTile(float tiling)
{
    return 8.0f / tiling;
}

// A Lighting with nothing in it: its Sky ids are the client's defaults.
const std::shared_ptr<const lunar::Lighting>& defaultLighting()
{
    static const std::shared_ptr<const lunar::Lighting> lighting = std::make_shared<lunar::Lighting>();
    return lighting;
}

// The client's water ripple maps, an animation of this many frames.
constexpr int waterFrames = 25;

std::string waterFrame(int frame)
{
    return std::format("rbxasset://textures/water/normal_{:02}.dds", frame);
}

} // namespace

PlaceScene::PlaceScene(lunar::DataModel& game)
    : game_(game), cache_(std::make_shared<lunar::assets::AssetCache>()), loader_(cache_), textureLoader_(cache_),
      textures_(textureLoader_.provider()), textureGeneration_([this] { return textureLoader_.generation(); })
{
    for (const lunar::InstancePtr& child : game.workspace()->children()) {
        auto* terrain = dynamic_cast<lunar::Terrain*>(child.get());
        if (!terrain)
            continue;
        // The meshers read the cells on their threads: what streams in waits for updateTerrain.
        terrain->setDeferWrites(true);
        for (std::size_t i = 0; i < terrainColors_.size(); ++i)
            terrainColors_[i] = terrain->getMaterialColor(lunar::Terrain::materials[i]);
        waterWaveSize_ = terrain->waterWaveSize();
        waterWaveSpeed_ = terrain->waterWaveSpeed();
        waterReflectance_ = terrain->waterReflectance();
        terrain_ = terrain;
        terrainLod_ = std::make_unique<lunar::terrain::TerrainLod>(*terrain);
        break;
    }
    loadTerrainParams();
    for (const lunar::InstancePtr& d : game.descendants())
        if (auto highlight = std::dynamic_pointer_cast<const lunar::Highlight>(d))
            highlights_.push_back(highlight);
    highlightAdded_ = game.descendantAdded.connect([this](lunar::Instance* instance) {
        if (auto* highlight = dynamic_cast<lunar::Highlight*>(instance))
            highlights_.push_back(std::static_pointer_cast<const lunar::Highlight>(highlight->shared_from_this()));
    });
    // Ask early so waitForMeshes() covers them too.
    const lunar::render::SkyTextureIds sky = skyTextureIds();
    for (int frame = 1; frame <= waterFrames; ++frame)
        textureLoader_.request(waterFrame(frame));
    for (const std::string& id : sky.faces)
        textureLoader_.request(id);
    for (const std::string& id : lunar::render::skyTextureIds(*defaultLighting()).faces)
        textureLoader_.request(id); // in case the place's own can't load
    textureLoader_.request(sky.sun);
    textureLoader_.request(sky.moon);
    for (std::size_t slot = 0; slot < lunar::render::builtinMaterialSlots; ++slot) {
        const lunar::render::BuiltinMaterial& material = lunar::render::builtinMaterial(slot);
        const lunar::render::BuiltinMaterialPack& pack = use2022Materials() ? material.v2022 : material.pre2022;
        if (pack.pack == 0)
            continue;
        textureLoader_.request(assetUri(pack.pack));
        if (pack.normal != 0)
            textureLoader_.request(assetUri(pack.normal));
    }
    rebuild();
    // Through textures_, whichever provider that is by then.
    lights_ = std::make_unique<lunar::render::LocalLights>(*game.workspace());
    ribbons_ = std::make_unique<lunar::render::Ribbons>(*game.workspace(), [this](std::string_view id) {
        return textures_ ? textures_(id) : lunar::render::ImagePtr{};
    });
    adornments_ = std::make_unique<lunar::render::Adornments>(
        game, [this](std::string_view id) { return textures_ ? textures_(id) : lunar::render::ImagePtr{}; });
}

bool PlaceScene::use2022Materials() const
{
    // Places saved without a MaterialService get the engine default.
    if (const auto* service = dynamic_cast<const lunar::MaterialService*>(game_.findFirstChild("MaterialService")))
        return service->use2022Materials();
    return lunar::MaterialService().use2022Materials();
}

lunar::ui::ViewportFrames::Assets PlaceScene::viewportAssets()
{
    return {
        .meshes = loader_.provider(),
        // textures_ can be replaced (wrapTextures), so it's looked up each time.
        .textures = [this](std::string_view id) { return textures_(id); },
        .materials = materialTextures(),
    };
}

lunar::render::MaterialTextureProvider PlaceScene::materialTextures()
{
    // Content ids per slot, made once: the provider runs every frame.
    struct Slot {
        std::string color;
        std::string normal;
        lunar::render::BuiltinMaterialPack pack;
    };
    std::vector<Slot> slots;
    for (std::size_t i = 0; i < lunar::render::builtinMaterialSlots; ++i) {
        const lunar::render::BuiltinMaterial& material = lunar::render::builtinMaterial(i);
        const lunar::render::BuiltinMaterialPack& pack = use2022Materials() ? material.v2022 : material.pre2022;
        slots.push_back({pack.pack != 0 ? assetUri(pack.pack) : std::string(),
                         pack.normal != 0 ? assetUri(pack.normal) : std::string(), pack});
    }
    return [this, provider = textureLoader_.provider(), slots = std::move(slots)](lunar::Enum::Material material,
                                                                                std::string_view variantName) {
        // A MaterialVariant's own maps replace the built-in ones.
        if (const auto* service = dynamic_cast<const lunar::MaterialService*>(game_.findFirstChild("MaterialService")))
            if (const lunar::MaterialVariant* variant = lunar::findMaterialVariant(*service, material, variantName)) {
                if (variant->colorMap().empty())
                    return lunar::render::MaterialTextures{};
                return lunar::render::MaterialTextures{
                    .color = provider(variant->colorMap()),
                    .normal = variant->normalMap().empty() ? nullptr : provider(variant->normalMap()),
                    .studsPerTile = variant->studsPerTile() > 0.0f ? variant->studsPerTile() : 10.0f,
                    .tintMask = variant->alphaMode() == lunar::Enum::AlphaMode::TintMask,
                };
            }
        const std::optional<std::size_t> slot = lunar::render::builtinMaterialSlot(material);
        if (!slot || slots[*slot].color.empty())
            return lunar::render::MaterialTextures{};
        const Slot& found = slots[*slot];
        return lunar::render::MaterialTextures{
            .color = provider(found.color),
            .normal = found.normal.empty() ? nullptr : provider(found.normal),
            .studsPerTile = studsPerTile(found.pack.tiling),
            .tintMask = lunar::render::partTintMask(material, found.pack),
        };
    };
}

void PlaceScene::loadTerrainParams()
{
    // The client's terrain settings, the set MaterialService picks.
    const char* const file
        = use2022Materials() ? "rbxasset://terrain/materials2022.json" : "rbxasset://terrain/materials.json";
    if (auto bytes = cache_->fetch(file)) {
        if (auto table = lunar::terrain::parseTerrainMaterials(**bytes))
            terrainParams_ = std::move(*table);
        else
            std::println(stderr, "{}: {}", file, table.error());
    }
    if (auto bytes = cache_->fetch("rbxasset://terrain/materialTransitions.json")) {
        if (auto parsed = lunar::terrain::parseTerrainTransitions(**bytes, terrainParams_); !parsed)
            std::println(stderr, "materialTransitions.json: {}", parsed.error());
    }
}

lunar::render::SkyTextureIds PlaceScene::skyTextureIds() const
{
    if (const auto* lighting = dynamic_cast<const lunar::Lighting*>(game_.findService("Lighting")))
        return lunar::render::skyTextureIds(*lighting);
    return lunar::render::skyTextureIds(*defaultLighting());
}

lunar::render::Lighting PlaceScene::lighting()
{
    // Faces that can't load (private decals need a login) fall back to the
    // client's default sky rather than leaving a hole.
    const lunar::render::SkyTextureIds defaults = lunar::render::skyTextureIds(*defaultLighting());
    const lunar::render::SkyTextureIds ids = skyTextureIds();
    lunar::render::Lighting lighting = lunar::render::lightingFrom(game_, [&](std::string_view id) -> lunar::render::ImagePtr {
        if (textureLoader_.state(id) == lunar::assets::TextureState::Failed) {
            for (std::size_t i = 0; i < ids.faces.size(); ++i)
                if (ids.faces[i] == id)
                    return textureLoader_.image(defaults.faces[i]);
            return nullptr;
        }
        return textureLoader_.image(id);
    });
    // research: LUNAR_TECHNOLOGY=voxel|compatibility|shadowmap|future lights the place as if
    // Lighting.Technology said so (scripts can't set it).
    if (const char* technology = std::getenv("LUNAR_TECHNOLOGY"); technology && *technology) {
        const std::string_view name(technology);
        if (name == "voxel")
            lighting.technology = lunar::Enum::Technology::Voxel;
        else if (name == "compatibility")
            lighting.technology = lunar::Enum::Technology::Compatibility;
        else if (name == "shadowmap")
            lighting.technology = lunar::Enum::Technology::ShadowMap;
        else if (name == "future")
            lighting.technology = lunar::Enum::Technology::Future;
    }
    return lighting;
}

lunar::render::TerrainMaterialProvider PlaceScene::terrainMaterials()
{
    struct Layer {
        std::string color;
        std::string normal;
        lunar::render::TerrainLayer look;
    };
    std::vector<Layer> layers;
    for (std::size_t i = 0; i < lunar::Terrain::materials.size(); ++i) {
        const lunar::terrain::TerrainMaterialParams& params = terrainParams_[i];
        Layer layer;
        layer.look = {
            .tint = terrainColors_[i],
            .studsPerTileTop = params.top.studsPerTile(),
            .studsPerTileSide = params.side.studsPerTile(),
            .blendMin = params.top.blendMin,
            .blendMax = params.top.blendMax,
            .blendScale = params.top.blendScale,
            .dominance = params.transition.dominance,
            .edgeNoise = params.transition.materialEdgeBlur,
            .noiseStuds = params.transition.noiseSize,
        };
        // Terrain draws with the same built-in material TexturePacks as parts.
        if (const auto pack = lunar::render::builtinMaterialPack(lunar::Terrain::materials[i], use2022Materials())) {
            layer.color = assetUri(pack->pack);
            layer.normal = pack->normal != 0 ? assetUri(pack->normal) : std::string();
            layer.look.tintMask = pack->tintMask;
        }
        layers.push_back(std::move(layer));
    }
    // Water ripples with the client's own animated normal maps. Wave size,
    // tile size and how WaterWaveSpeed maps to the frame rate and the swell
    // are guesses: WaterWaveSize 0.15 bends the surface gently, tiles of 16
    // studs, speed 10 plays 6 frames a second.
    Layer& water = layers[1];
    water.normal = waterFrame(1);
    water.look.studsPerTileTop = 16.0f;
    water.look.waveStrength = std::clamp(waterWaveSize_, 0.0f, 1.0f) * 2.0f;
    water.look.reflectance = std::clamp(waterReflectance_, 0.0f, 1.0f);
    water.look.frameRate = std::clamp(waterWaveSpeed_, 0.0f, 100.0f) * 0.6f;
    water.look.waveHeight = std::clamp(waterWaveSize_, 0.0f, 1.0f);
    water.look.waveSpeed = std::clamp(waterWaveSpeed_, 0.0f, 100.0f) * 0.1f;
    return [provider = textureLoader_.provider(), layers = std::move(layers)](std::size_t index) {
        if (index >= layers.size())
            return lunar::render::TerrainLayer{};
        const Layer& layer = layers[index];
        lunar::render::TerrainLayer look = layer.look;
        if (!layer.color.empty())
            look.color = provider(layer.color);
        if (!layer.normal.empty())
            look.normal = provider(layer.normal);
        if (index == 1) {
            look.color = nullptr; // water has no colour map, only ripples
            // All frames or none, so the loop doesn't stutter while loading.
            for (int frame = 1; frame <= waterFrames; ++frame) {
                lunar::render::ImagePtr image = provider(waterFrame(frame));
                if (!image) {
                    look.normalFrames.clear();
                    break;
                }
                look.normalFrames.push_back(std::move(image));
            }
        }
        return look;
    };
}

// Mesh and texture providers that note the part asking whenever what it
// asks for isn't there yet (and hasn't failed), so the part is extracted
// again once it is.
std::pair<lunar::render::MeshProvider, lunar::render::TextureProvider> PlaceScene::waitingProviders()
{
    auto note = [this](std::unordered_map<std::string, Waiters>& waiting, std::string_view id) {
        waiting[std::string(id)].emplace_back(extracting_, extracting_->weak_from_this());
    };
    lunar::render::MeshProvider meshes = [this, note, inner = loader_.provider()](std::string_view id) {
        auto mesh = inner(id);
        if (!mesh && extracting_ && loader_.state(id) != lunar::assets::MeshState::Failed)
            note(waitingMeshes_, id);
        return mesh;
    };
    lunar::render::TextureProvider textures = [this, note, inner = textures_](std::string_view id) {
        auto image = inner(id);
        if (!image && extracting_ && textureLoader_.state(id) != lunar::assets::TextureState::Failed)
            note(waitingTextures_, id);
        return image;
    };
    return {std::move(meshes), std::move(textures)};
}

void PlaceScene::extractPart(const lunar::BasePart& part, lunar::render::PartExtractor& extractor)
{
    extracting_ = &part;
    extractor.add(part, items_);
    extracting_ = nullptr;
    owners_.resize(items_.size(), &part);
    watch(part);
}

void PlaceScene::watch(const lunar::BasePart& part)
{
    lunar::Connection& connection = watching_[&part];
    if (connection.connected())
        return;
    // Changed only offers a mutable signal; listening doesn't change the part.
    // What the extractor reads of the part itself: a change extracts it
    // again (the game hides the dark block in a doorway with Transparency as
    // the door opens). Its children (decals, meshes) aren't watched.
    static constexpr std::string_view looks[] = {
        "Transparency", "Color",        "Size",          "Material",       "MaterialVariant", "Reflectance",
        "CastShadow",   "TopSurface",   "BottomSurface", "LeftSurface",    "RightSurface",    "FrontSurface",
        "BackSurface",  "Shape",        "MeshId",        "TextureID",      "RenderFidelity",  "DoubleSided",
    };
    connection = const_cast<lunar::BasePart&>(part).changed.connect([this, at = &part](std::string_view property) {
        if (property == "CFrame" || property == "LocalTransparencyModifier")
            moved_.push_back(at);
        else if (std::ranges::contains(looks, property))
            touched_.insert_or_assign(at, at->weak_from_this());
    });
}

void PlaceScene::unwatchAll()
{
    for (auto& [part, connection] : watching_)
        connection.disconnect();
    watching_.clear();
}

PlaceScene::~PlaceScene()
{
    highlightAdded_.disconnect();
    unwatchAll();
    terrainLod_.reset();
    if (terrain_) {
        terrain_->setDeferWrites(false);
        terrain_->applyQueuedWrites();
    }
}

void PlaceScene::rebuild()
{
    ++extractions_;
    generation_ = loader_.generation();
    textureGenerationSeen_ = textureGeneration_ ? textureGeneration_() : 0;
    dirty_ = false;
    touched_.clear();
    waitingMeshes_.clear();
    waitingTextures_.clear();
    unwatchAll();
    items_.clear();
    owners_.clear();
    // Every part in tree order, as extractScene goes. A fresh extractor: the
    // texture provider may have changed.
    auto [meshes, textures] = waitingProviders();
    parts_ = std::make_unique<lunar::render::PartExtractor>(std::move(meshes), std::move(textures));
    auto visit = [&](const auto& self, const lunar::Instance& instance) -> void {
        if (const auto* part = dynamic_cast<const lunar::BasePart*>(&instance))
            extractPart(*part, *parts_);
        for (const lunar::InstancePtr& child : instance.children())
            self(self, *child);
    };
    visit(visit, *game_.workspace());
    partItems_ = items_.size();
    preparePartItems(0);
    appendTerrainItems();
    followingStale_ = true;
}

void PlaceScene::touch(const lunar::Instance& instance)
{
    if (dynamic_cast<const lunar::BasePart*>(&instance))
        touched_.insert_or_assign(&instance, instance.weak_from_this());
}

// The touched parts' items go, the rest keeping their order, and those still
// in the Workspace come back, extracted afresh, after them. Rebuilding every
// part instead took 50-65 ms on Duvall's 20k, a hitch whenever one came or
// went or a mesh or texture arrived.
void PlaceScene::updateTouched()
{
    // A frame's share: one texture can be on thousands of parts (Duvall's
    // first arrivals touch 18k), so the rest wait for the next refreshes.
    constexpr std::size_t perRefresh = 500;
    decltype(touched_) batch;
    if (touched_.size() <= perRefresh) {
        batch.swap(touched_);
    } else {
        for (auto it = touched_.begin(); batch.size() < perRefresh;)
            batch.insert(touched_.extract(it++));
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < partItems_; ++i) {
        if (batch.contains(owners_[i]))
            continue;
        if (kept != i) {
            items_[kept] = std::move(items_[i]);
            owners_[kept] = owners_[i];
            local_[kept] = local_[i];
            baseTransparency_[kept] = baseTransparency_[i];
            baseOnSolid_[kept] = baseOnSolid_[i];
            placed_[kept] = placed_[i];
            faded_[kept] = faded_[i];
            following_[kept] = following_[i];
        }
        ++kept;
    }
    items_.resize(kept);
    owners_.resize(kept);
    // Each listens again if it's extracted again (its address may be another part's by now).
    for (const auto& [address, weak] : batch) {
        if (auto it = watching_.find(address); it != watching_.end()) {
            it->second.disconnect();
            watching_.erase(it);
        }
    }
    for (const auto& [address, weak] : batch) {
        const auto instance = weak.lock();
        const auto* part = dynamic_cast<const lunar::BasePart*>(instance.get());
        if (part && !part->destroyed() && part->isDescendantOf(game_.workspace()))
            extractPart(*part, *parts_);
    }
    partItems_ = items_.size();
    preparePartItems(kept);
    appendTerrainItems();
    ++extractions_;
    // The kept items are where they were; the new ones are placed and
    // followed on the next followParts.
    fresh_ = std::min(fresh_, kept);
    rangesStale_ = true;
}

// Each part item's place in its part's space, base transparency and light
// occluders, from `first` on.
void PlaceScene::preparePartItems(std::size_t first)
{
    local_.resize(partItems_);
    baseTransparency_.resize(partItems_);
    baseOnSolid_.resize(partItems_);
    placed_.resize(partItems_);
    faded_.resize(partItems_);
    for (std::size_t i = first; i < partItems_; ++i) {
        baseTransparency_[i] = items_[i].transparency;
        baseOnSolid_[i] = items_[i].onSolid;
        faded_[i] = 0.0f;
        if (const auto* part = dynamic_cast<const lunar::BasePart*>(owners_[i])) {
            placed_[i] = part->cframe();
            local_[i] = lunar::render::Mat4::fromCFrame(placed_[i].inverse()) * items_[i].model;
        }
    }
    addLightOccluders(first);
}

namespace {

// The inverse of a model matrix (rotation, scale and translation).
lunar::render::Mat4 affineInverse(const lunar::render::Mat4& m)
{
    // a[r][c] = m(r, c) for the 3x3 part.
    const float a00 = m.m[0][0], a01 = m.m[1][0], a02 = m.m[2][0];
    const float a10 = m.m[0][1], a11 = m.m[1][1], a12 = m.m[2][1];
    const float a20 = m.m[0][2], a21 = m.m[1][2], a22 = m.m[2][2];
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    const float k = std::abs(det) > 1e-20f ? 1.0f / det : 0.0f;
    lunar::render::Mat4 out;
    const float inv[3][3] = {
        {(a11 * a22 - a12 * a21) * k, (a02 * a21 - a01 * a22) * k, (a01 * a12 - a02 * a11) * k},
        {(a12 * a20 - a10 * a22) * k, (a00 * a22 - a02 * a20) * k, (a02 * a10 - a00 * a12) * k},
        {(a10 * a21 - a11 * a20) * k, (a01 * a20 - a00 * a21) * k, (a00 * a11 - a01 * a10) * k},
    };
    const float t[3] = {m.m[3][0], m.m[3][1], m.m[3][2]};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c)
            out.m[c][r] = inv[r][c];
        out.m[3][r] = -(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]);
    }
    return out;
}

} // namespace

// The client's light grid voxelizes a MeshPart as the boxes around its
// collision geometry's convex pieces, each grown by the pieces' 0.05 margin
// before the part's scale (sub_1051b7700, sub_1033b73da), or as its box
// when it collides as one. The boxes go to the items in their meshes' space.
void PlaceScene::addLightOccluders(std::size_t first)
{
    std::vector<lunar::SharedBytes> blobs;
    for (std::size_t i = first; i < partItems_; ++i)
        if (const auto* mesh = dynamic_cast<const lunar::TriangleMeshPart*>(owners_[i]); mesh && !items_[i].decal) {
            const lunar::SharedBytes& blob = mesh->physicalConfigData() && !mesh->physicalConfigData()->empty()
                                                 ? mesh->physicalConfigData()
                                                 : mesh->physicsData();
            if (blob)
                blobs.push_back(blob);
        }
    collision_.prepare(blobs, [](std::size_t count, std::function_ref<void(std::size_t)> fn) {
        // A few new blobs (or none, as when a part or two come and go)
        // aren't worth starting threads for.
        if (count < 16) {
            for (std::size_t i = 0; i < count; ++i)
                fn(i);
            return;
        }
        std::atomic<std::size_t> next{0};
        auto run = [&] {
            for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1))
                fn(i);
        };
        std::vector<std::jthread> pool;
        for (unsigned t = 1; t < std::max(1u, std::thread::hardware_concurrency() / 2); ++t)
            pool.emplace_back(run);
        run();
    });
    for (std::size_t i = first; i < partItems_; ++i) {
        const auto* mesh = dynamic_cast<const lunar::TriangleMeshPart*>(owners_[i]);
        if (!mesh || items_[i].decal)
            continue;
        const lunar::physics::Shape shape = lunar::physics::shapeOf(*mesh, &collision_);
        std::vector<lunar::render::Bounds> partBoxes;
        if (shape.kind == lunar::physics::ShapeKind::Mesh && shape.mesh) {
            const lunar::Vector3 pad{lunar::physics::meshSkin, lunar::physics::meshSkin, lunar::physics::meshSkin};
            auto scaled = [&](lunar::Vector3 v) {
                return lunar::Vector3{v.x * shape.scale.x, v.y * shape.scale.y, v.z * shape.scale.z};
            };
            for (const lunar::physics::Aabb& box : shape.mesh->bounds)
                if (box.valid())
                    partBoxes.push_back({scaled(box.min - pad), scaled(box.max + pad)});
        }
        if (partBoxes.empty())
            partBoxes.push_back({mesh->size() * -0.5f, mesh->size() * 0.5f});
        // From the part's space to the mesh's.
        const lunar::render::Mat4 toMesh = affineInverse(local_[i]);
        auto boxes = std::make_shared<std::vector<lunar::render::Bounds>>();
        boxes->reserve(partBoxes.size());
        for (const lunar::render::Bounds& box : partBoxes) {
            lunar::render::Bounds out{toMesh.transformPoint(box.min), toMesh.transformPoint(box.min)};
            for (int corner = 1; corner < 8; ++corner) {
                const lunar::Vector3 p
                    = toMesh.transformPoint({corner & 1 ? box.max.x : box.min.x, corner & 2 ? box.max.y : box.min.y,
                                             corner & 4 ? box.max.z : box.min.z});
                out.min = {std::min(out.min.x, p.x), std::min(out.min.y, p.y), std::min(out.min.z, p.z)};
                out.max = {std::max(out.max.x, p.x), std::max(out.max.y, p.y), std::max(out.max.z, p.z)};
            }
            boxes->push_back(out);
        }
        items_[i].occluders = std::move(boxes);
    }
}

void PlaceScene::setTerrainDistance(float scale)
{
    if (!terrain_)
        return;
    lunar::terrain::LodOptions options;
    options.distanceScale = scale;
    // Farther out is more terrain at once: the budgets grow with the area.
    const float area = std::max(1.0f, scale * scale);
    options.drawBudget = static_cast<std::size_t>(static_cast<float>(options.drawBudget) * area);
    options.cacheBudget = static_cast<std::size_t>(static_cast<float>(options.cacheBudget) * area);
    terrainOptions_ = options;
    terrainLod_ = std::make_unique<lunar::terrain::TerrainLod>(*terrain_, options);
}

// The writes waiting go into the cells with the meshers stopped, and
// meshing starts over on what they made.
bool PlaceScene::applyTerrainWrites()
{
    if (!terrain_ || !terrain_->hasQueuedWrites())
        return false;
    terrainLod_.reset();
    terrain_->applyQueuedWrites();
    terrainLod_ = std::make_unique<lunar::terrain::TerrainLod>(*terrain_, terrainOptions_);
    terrainWritten_ = std::chrono::steady_clock::now();
    return true;
}

bool PlaceScene::updateTerrain(lunar::Vector3 eye)
{
    if (!terrainRemeshing_ && std::chrono::steady_clock::now() - terrainWritten_ >= std::chrono::milliseconds(500))
        terrainRemeshing_ = applyTerrainWrites();
    if (!terrainLod_)
        return false;
    const bool changed = terrainLod_->update(eye);
    // Starting over, the meshes from before stay up until the new ones are all there.
    if (terrainRemeshing_) {
        if (!terrainLod_->settled())
            return false;
        terrainRemeshing_ = false;
    } else if (!changed) {
        return false;
    }
    takeTerrain();
    return true;
}

void PlaceScene::settleTerrain(std::optional<lunar::Vector3> eye)
{
    applyTerrainWrites();
    terrainRemeshing_ = false;
    if (!terrainLod_)
        return;
    // Far enough off across the ground that every node is past its distance.
    terrainLod_->settle(eye.value_or(lunar::Vector3{1e9f, 0.0f, 1e9f}));
    takeTerrain();
}

lunar::terrain::TerrainLod::Stats PlaceScene::terrainStats() const
{
    return terrainLod_ ? terrainLod_->stats() : lunar::terrain::TerrainLod::Stats{};
}

// Swaps the terrain items at the end of items_ for the current selection.
void PlaceScene::takeTerrain()
{
    terrainItems_.clear();
    for (const auto& [chunk, ditherFrom, ditherTo] : terrainLod_->selection()) {
        // The meshes live inside the chunk; the items share its ownership.
        if (!chunk->solid.empty()) {
            lunar::render::DrawItem solid = lunar::render::worldMesh(lunar::render::MeshPtr(chunk, &chunk->solid),
                                                                     lunar::Enum::Material::Plastic);
            solid.terrain = true;
            solid.terrainPalette = chunk->palette;
            solid.dither[0] = ditherFrom;
            solid.dither[1] = ditherTo;
            terrainItems_.push_back(std::move(solid));
        }
        // Water blends rather than dithers: while fading, the old water
        // gives way to the new halfway through.
        if (!chunk->water.empty() && ditherTo - ditherFrom >= 0.5f) {
            lunar::render::DrawItem water = lunar::render::worldMesh(lunar::render::MeshPtr(chunk, &chunk->water),
                                                                     lunar::Enum::Material::Plastic);
            water.transparency = terrain_->waterTransparency();
            water.terrainWater = true;
            water.castShadow = false;
            terrainItems_.push_back(std::move(water));
        }
    }
    appendTerrainItems();
}

// The terrain's items after the parts'.
void PlaceScene::appendTerrainItems()
{
    items_.resize(partItems_);
    items_.insert(items_.end(), terrainItems_.begin(), terrainItems_.end());
    owners_.resize(items_.size(), nullptr);
    local_.resize(items_.size());
    baseTransparency_.resize(items_.size());
    baseOnSolid_.resize(items_.size());
    placed_.resize(items_.size());
    faded_.resize(items_.size());
    for (std::size_t i = partItems_; i < items_.size(); ++i) {
        owners_[i] = nullptr;
        baseTransparency_[i] = items_[i].transparency;
        baseOnSolid_[i] = items_[i].onSolid;
    }
    following_.resize(items_.size());
}

void PlaceScene::followParts()
{
    followParts(nullptr, {});
}

void PlaceScene::followParts(const lunar::Instance* moving, lunar::Vector3 offset)
{
    ribbons_->follow(moving, offset);
    adornments_->follow(moving, offset);
    lights_->follow(moving, offset);
    // After the items change, or the moving instance does, every item once.
    bool everything = false;
    if (fresh_ < partItems_ && !followingStale_ && moving == followingOf_) {
        // Only the items extracted since: whether they follow, and where they are.
        for (std::size_t i = fresh_; i < partItems_; ++i)
            following_[i] = moving && owners_[i] && owners_[i]->isDescendantOf(moving) ? 1 : 0;
        followingItems_.clear();
        for (std::size_t i = 0; i < partItems_; ++i)
            if (following_[i] != 0)
                followingItems_.push_back(i);
    } else
        fresh_ = partItems_;
    if (followingStale_ || moving != followingOf_) {
        following_.assign(items_.size(), 0);
        followingItems_.clear();
        if (moving)
            for (std::size_t i = 0; i < partItems_; ++i)
                if (owners_[i] && owners_[i]->isDescendantOf(moving)) {
                    following_[i] = 1;
                    followingItems_.push_back(i);
                }
        followingOf_ = moving;
        followingStale_ = false;
        rangesStale_ = true;
        everything = true;
    }
    if (rangesStale_) {
        // A part's items are together (extracted one part at a time, the
        // rest kept in order), so a run each, sorted by part.
        ranges_.clear();
        for (std::size_t i = 0; i < partItems_; ++i)
            if (!ranges_.empty() && ranges_.back().part == owners_[i]
                && ranges_.back().first + ranges_.back().count == i)
                ++ranges_.back().count;
            else
                ranges_.push_back({owners_[i], i, 1});
        std::ranges::sort(ranges_, {}, [](const Range& r) { return std::pair{r.part, r.first}; });
        // Should a part's items ever be apart: its first, and all of them counted.
        std::size_t kept = 0;
        for (std::size_t r = 0; r < ranges_.size(); ++r)
            if (kept > 0 && ranges_[kept - 1].part == ranges_[r].part)
                ranges_[kept - 1].count += ranges_[r].count;
            else
                ranges_[kept++] = ranges_[r];
        ranges_.resize(kept);
        rangesStale_ = false;
    }
    // New matrices only where the CFrame (or fade) differs from what the
    // item was last placed with.
    auto place = [&](std::size_t i) {
        const auto* part = static_cast<const lunar::BasePart*>(owners_[i]);
        if (!part)
            return;
        lunar::CFrame cframe = part->cframe();
        if (following_[i] != 0)
            cframe = cframe + offset;
        if (cframe != placed_[i]) {
            items_[i].model = lunar::render::Mat4::fromCFrame(cframe) * local_[i];
            placed_[i] = cframe;
        }
        const float fade = part->localTransparencyModifier();
        if (fade != faded_[i]) {
            items_[i].transparency = 1.0f - (1.0f - baseTransparency_[i]) * (1.0f - fade);
            // A faded part shows what's behind it, which its decals mustn't go into.
            items_[i].onSolid = baseOnSolid_[i] != 0 && fade <= 0.0f;
            faded_[i] = fade;
        }
    };
    // A list longer than the parts (nobody followed for a while) is no help either.
    if (everything || moved_.size() > partItems_) {
        for (std::size_t i = 0; i < partItems_; ++i)
            place(i);
    } else {
        for (const lunar::Instance* part : moved_)
            for (const auto [first, count] = rangeOf(part); std::size_t i : std::views::iota(first, first + count))
                place(i);
        // The followed ones shift with the offset every frame.
        for (std::size_t i : followingItems_)
            place(i);
        for (std::size_t i = fresh_; i < partItems_; ++i)
            place(i);
    }
    fresh_ = partItems_;
    moved_.clear();
    updateHighlights();
}

std::pair<std::size_t, std::size_t> PlaceScene::rangeOf(const lunar::Instance* part) const
{
    const auto it = std::ranges::lower_bound(ranges_, part, {}, &Range::part);
    if (it == ranges_.end() || it->part != part)
        return {0, 0};
    return {it->first, it->count};
}

void PlaceScene::updateHighlights()
{
    if (highlights_.empty() && highlighted_.empty())
        return;
    // After an extraction last frame's indices mean other items, and copies
    // of their looks may be anywhere: all of them go first.
    if (highlightedExtraction_ != extractions_) {
        if (!highlighted_.empty())
            for (std::size_t i = 0; i < partItems_; ++i)
                items_[i].highlight.reset();
        highlighted_.clear();
        highlightedExtraction_ = extractions_;
    }
    if (tagged_.size() != items_.size())
        tagged_.assign(items_.size(), 0);
    ++tagStamp_;

    std::erase_if(highlights_, [&](const std::weak_ptr<const lunar::Highlight>& weak) {
        const auto highlight = weak.lock();
        return !highlight || !highlight->isDescendantOf(&game_);
    });
    // What each covers: its Adornee, else its parent, if that's a part or a
    // model in the Workspace. Deeper ones cover their parts over shallower
    // ones; the renderer draws the first maxHighlights it meets, one look
    // each, told apart by address: each Highlight keeps its look's object
    // from frame to frame while it looks the same.
    struct Cover {
        const lunar::Instance* target;
        std::size_t depth;
        std::shared_ptr<const lunar::render::HighlightStyle> style;
    };
    std::vector<Cover> covers;
    std::vector<std::pair<const lunar::Highlight*, std::shared_ptr<const lunar::render::HighlightStyle>>> styles;
    const lunar::Instance* workspace = game_.workspace();
    for (const auto& weak : highlights_) {
        const auto highlight = weak.lock();
        if (!highlight->enabled())
            continue;
        const lunar::Instance* target = highlight->adornee() ? highlight->adornee() : highlight->parent();
        if (!target || !target->isDescendantOf(workspace)
            || (!lunar::instanceCast<const lunar::BasePart>(target)
                && !lunar::instanceCast<const lunar::Model>(target)))
            continue;
        std::size_t depth = 0;
        for (const lunar::Instance* at = target; at; at = at->parent())
            ++depth;
        const lunar::render::HighlightStyle look{
            .fillColor = highlight->fillColor(),
            .fillTransparency = highlight->fillTransparency(),
            .outlineColor = highlight->outlineColor(),
            .outlineTransparency = highlight->outlineTransparency(),
            .alwaysOnTop = highlight->depthMode() == lunar::Enum::HighlightDepthMode::AlwaysOnTop,
        };
        std::shared_ptr<const lunar::render::HighlightStyle> style;
        const auto kept = std::ranges::lower_bound(styles_, highlight.get(), {}, &decltype(styles_)::value_type::first);
        auto same = [](const lunar::render::HighlightStyle& a, const lunar::render::HighlightStyle& b) {
            return a.fillColor.r == b.fillColor.r && a.fillColor.g == b.fillColor.g && a.fillColor.b == b.fillColor.b
                   && a.fillTransparency == b.fillTransparency && a.outlineColor.r == b.outlineColor.r
                   && a.outlineColor.g == b.outlineColor.g && a.outlineColor.b == b.outlineColor.b
                   && a.outlineTransparency == b.outlineTransparency && a.alwaysOnTop == b.alwaysOnTop;
        };
        if (kept != styles_.end() && kept->first == highlight.get() && same(*kept->second, look))
            style = kept->second;
        else
            style = std::make_shared<const lunar::render::HighlightStyle>(look);
        styles.emplace_back(highlight.get(), style);
        covers.push_back({target, depth, std::move(style)});
    }
    std::ranges::sort(styles, {}, &decltype(styles)::value_type::first);
    styles_ = std::move(styles);
    std::ranges::stable_sort(covers, {}, &Cover::depth);
    // Each part's items, once a frame each (a deeper cover's look over a
    // shallower's), noted as this frame's.
    std::vector<std::size_t> now;
    auto tag = [&](const lunar::Instance* part, const std::shared_ptr<const lunar::render::HighlightStyle>& style) {
        const auto range = rangeOf(part);
        for (std::size_t i = range.first; i < range.first + range.second; ++i) {
            if (items_[i].highlight != style)
                items_[i].highlight = style;
            if (tagged_[i] != tagStamp_) {
                tagged_[i] = tagStamp_;
                now.push_back(i);
            }
        }
    };
    // The tree walked in place: descendants() would copy a pointer to
    // every one of them, every frame. Only parts have items.
    for (const Cover& cover : covers) {
        auto visit = [&](this const auto& self, const lunar::Instance& at) -> void {
            if (lunar::instanceCast<const lunar::BasePart>(&at))
                tag(&at, cover.style);
            for (const lunar::InstancePtr& child : at.children())
                self(*child);
        };
        visit(*cover.target);
    }
    // Last frame's that nothing covers now go.
    for (std::size_t i : highlighted_)
        if (i < items_.size() && tagged_[i] != tagStamp_)
            items_[i].highlight.reset();
    highlighted_ = std::move(now);
}

void PlaceScene::wrapTextures(const std::function<lunar::render::TextureProvider(lunar::render::TextureProvider)>& wrap)
{
    textures_ = wrap(textures_);
    rebuild();
}

void PlaceScene::setTextures(lunar::render::TextureProvider provider, std::function<std::uint64_t()> generation)
{
    textures_ = std::move(provider);
    textureGeneration_ = std::move(generation);
    rebuild();
}

bool PlaceScene::refresh()
{
    if (dirty_) {
        rebuild();
        return true;
    }
    bool changed = false;
    const std::uint64_t meshes = loader_.generation();
    const std::uint64_t textures = textureGeneration_ ? textureGeneration_() : 0;
    if (meshes != generation_ || textures != textureGenerationSeen_) {
        generation_ = meshes;
        textureGenerationSeen_ = textures;
        changed = true;
        // Whatever finished or failed since: the parts that asked for it go again.
        auto release = [this](std::unordered_map<std::string, Waiters>& waiting, const auto& loading) {
            for (auto it = waiting.begin(); it != waiting.end();) {
                if (loading(it->first)) {
                    ++it;
                    continue;
                }
                for (const auto& [address, weak] : it->second)
                    touched_.insert_or_assign(address, weak);
                it = waiting.erase(it);
            }
        };
        release(waitingMeshes_,
                [this](const std::string& id) { return loader_.state(id) == lunar::assets::MeshState::Loading; });
        release(waitingTextures_, [this](const std::string& id) {
            return textureLoader_.state(id) == lunar::assets::TextureState::Loading;
        });
    }
    if (!touched_.empty()) {
        updateTouched();
        changed = true;
    }
    return changed;
}

void PlaceScene::waitForMeshes()
{
    // A part whose mesh arrives may ask for its texture only then.
    do {
        loader_.wait();
        textureLoader_.wait();
    } while (refresh());
}
