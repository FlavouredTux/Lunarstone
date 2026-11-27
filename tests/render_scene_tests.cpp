#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Primitives.hpp>
#include <lunar/render/Scene.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::render;

namespace {

std::shared_ptr<Part> addPart(Instance* parent, Vector3 position, Vector3 size, Color3 color = {})
{
    auto part = std::make_shared<Part>();
    part->setPosition(position);
    part->setSize(size);
    part->setColor(color);
    part->setParent(parent);
    return part;
}

} // namespace

TEST(scene_collects_parts_in_tree_order)
{
    auto game = DataModel::create();
    addPart(game->workspace(), {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 0.0f, 0.0f});
    auto model = std::make_shared<Model>();
    model->setParent(game->workspace());
    addPart(model.get(), {10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
    auto folder = std::make_shared<Folder>();
    folder->setParent(model);
    addPart(folder.get(), {0.0f, 10.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f});

    auto items = extractScene(*game->workspace());
    CHECK_EQ(items.size(), 3u);
    CHECK(items[0].color == (Color3{1.0f, 0.0f, 0.0f}));
    CHECK(items[1].color == (Color3{0.0f, 1.0f, 0.0f}));
    CHECK(items[2].color == (Color3{0.0f, 0.0f, 1.0f}));
    CHECK(items[1].model == partMatrix(CFrame(10.0f, 0.0f, 0.0f), {1.0f, 1.0f, 1.0f}));
}

TEST(scene_skips_invisible_parts)
{
    auto game = DataModel::create();
    addPart(game->workspace(), {}, {1.0f, 1.0f, 1.0f})->setTransparency(1.0f);
    addPart(game->workspace(), {}, {1.0f, 1.0f, 1.0f})->setTransparency(0.5f);
    auto items = extractScene(*game->workspace());
    CHECK_EQ(items.size(), 1u);
    CHECK_EQ(items[0].transparency, 0.5f);
}

TEST(scene_skips_terrain)
{
    auto game = DataModel::create();
    auto terrain = std::make_shared<lunar::Terrain>();
    terrain->setParent(game->workspace());
    addPart(game->workspace(), {}, {1.0f, 1.0f, 1.0f});
    CHECK_EQ(extractScene(*game->workspace()).size(), 1u);
}

TEST(scene_empty_workspace_has_no_bounds)
{
    auto game = DataModel::create();
    auto items = extractScene(*game->workspace());
    CHECK(items.empty());
    CHECK(!sceneBounds(items).has_value());
}

TEST(scene_bounds_cover_rotated_parts)
{
    auto game = DataModel::create();
    addPart(game->workspace(), {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
    auto rotated = addPart(game->workspace(), {}, {4.0f, 1.0f, 1.0f});
    // A 4 stud long bar stood upright at x = 10.
    rotated->setCFrame(CFrame(10.0f, 0.0f, 0.0f) * CFrame::angles(0.0f, 0.0f, 1.5707964f));

    auto bounds = sceneBounds(extractScene(*game->workspace()));
    CHECK(bounds.has_value());
    CHECK(std::abs(bounds->min.x + 1.0f) < 1e-4f);
    CHECK(std::abs(bounds->max.x - 10.5f) < 1e-4f);
    CHECK(std::abs(bounds->max.y - 2.0f) < 1e-4f);
    CHECK(std::abs(bounds->min.y + 2.0f) < 1e-4f);
    CHECK(std::abs(bounds->center().x - 4.75f) < 1e-4f);
}

namespace {

bool near(Vector3 a, Vector3 b, float tolerance = 1e-4f)
{
    return (a - b).magnitude() <= tolerance;
}

// A mesh off the origin and not unit sized, the way mesh files come.
std::shared_ptr<const MeshData> offsetMesh()
{
    auto mesh = std::make_shared<MeshData>(makePrimitive(Primitive::Block));
    for (MeshVertex& v : mesh->vertices)
        v.position
            = Vector3{v.position.x * 2.0f, v.position.y * 4.0f, v.position.z * 6.0f} + Vector3{10.0f, 0.0f, 0.0f};
    return mesh;
}

} // namespace

TEST(scene_shapes_parts_like_roblox)
{
    auto game = DataModel::create();
    auto ball = addPart(game->workspace(), {}, {4.0f, 2.0f, 3.0f});
    ball->setShape(Enum::PartType::Ball);
    auto cylinder = addPart(game->workspace(), {}, {6.0f, 2.0f, 3.0f});
    cylinder->setShape(Enum::PartType::Cylinder);
    auto wedge = std::make_shared<WedgePart>();
    wedge->setParent(game->workspace());
    auto corner = std::make_shared<CornerWedgePart>();
    corner->setParent(game->workspace());
    auto truss = std::make_shared<TrussPart>();
    truss->setParent(game->workspace());

    auto items = extractScene(*game->workspace());
    CHECK_EQ(items.size(), 5u);
    CHECK(items[0].mesh == primitiveMesh(Primitive::Ball));
    CHECK(items[0].model == partMatrix({}, {2.0f, 2.0f, 2.0f}));
    CHECK(items[1].mesh == primitiveMesh(Primitive::Cylinder));
    CHECK(items[1].model == partMatrix({}, {6.0f, 2.0f, 2.0f}));
    CHECK(items[2].mesh == primitiveMesh(Primitive::Wedge));
    CHECK(items[3].mesh == primitiveMesh(Primitive::CornerWedge));
    CHECK(items[4].mesh == nullptr); // a box
}

TEST(scene_keeps_material_and_reflectance)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {}, {1.0f, 1.0f, 1.0f});
    part->setMaterial(Enum::Material::Neon);
    part->setReflectance(0.25f);
    auto items = extractScene(*game->workspace());
    CHECK(items[0].material == Enum::Material::Neon);
    CHECK_EQ(items[0].reflectance, 0.25f);
}

TEST(scene_fits_provided_meshes_to_mesh_parts)
{
    auto game = DataModel::create();
    for (int i = 0; i < 3; ++i) {
        auto part = std::make_shared<MeshPart>();
        part->setMeshId("rbxassetid://42");
        part->setSize({1.0f, 2.0f, 3.0f});
        part->setPosition({static_cast<float>(i) * 5.0f, 0.0f, 0.0f});
        part->setParent(game->workspace());
    }
    int calls = 0;
    auto provider = [&](std::string_view id) -> MeshPtr {
        ++calls;
        CHECK(id == "rbxassetid://42");
        return offsetMesh(); // a new copy each time: extractScene must not ask twice
    };
    auto items = extractScene(*game->workspace(), provider);
    CHECK_EQ(calls, 1);
    CHECK_EQ(items.size(), 3u);
    CHECK(items[0].mesh != nullptr);
    CHECK(items[0].mesh == items[1].mesh && items[1].mesh == items[2].mesh);

    // The mesh's box lands exactly on the part's.
    const Bounds local = meshBounds(items[1].mesh.get());
    CHECK(near(items[1].model.transformPoint(local.min), {4.5f, -1.0f, -1.5f}));
    CHECK(near(items[1].model.transformPoint(local.max), {5.5f, 1.0f, 1.5f}));
}

TEST(scene_draws_missing_meshes_as_boxes)
{
    auto game = DataModel::create();
    auto part = std::make_shared<MeshPart>();
    part->setMeshId("rbxassetid://7");
    part->setSize({2.0f, 3.0f, 4.0f});
    part->setParent(game->workspace());

    for (const MeshProvider& provider : {MeshProvider{}, MeshProvider{[](std::string_view) { return MeshPtr{}; }}}) {
        auto items = extractScene(*game->workspace(), provider);
        CHECK_EQ(items.size(), 1u);
        CHECK(items[0].mesh == nullptr);
        CHECK(items[0].model == partMatrix({}, {2.0f, 3.0f, 4.0f}));
    }
}

TEST(scene_applies_mesh_children)
{
    auto game = DataModel::create();
    auto sphere = addPart(game->workspace(), {}, {2.0f, 4.0f, 6.0f});
    auto special = std::make_shared<SpecialMesh>();
    special->setMeshType(Enum::MeshType::Sphere);
    special->setScale({0.5f, 1.0f, 1.0f});
    special->setParent(sphere);

    auto block = addPart(game->workspace(), {}, {2.0f, 2.0f, 2.0f});
    auto blockMesh = std::make_shared<BlockMesh>();
    blockMesh->setOffset({0.0f, 1.0f, 0.0f});
    blockMesh->setParent(block);

    auto filePart = addPart(game->workspace(), {}, {2.0f, 2.0f, 2.0f});
    auto file = std::make_shared<SpecialMesh>();
    file->setMeshType(Enum::MeshType::FileMesh);
    file->setMeshId("rbxassetid://9");
    file->setScale({3.0f, 3.0f, 3.0f});
    file->setParent(filePart);

    auto cylinder = addPart(game->workspace(), {}, {2.0f, 2.0f, 2.0f});
    std::make_shared<CylinderMesh>()->setParent(cylinder);

    auto items = extractScene(*game->workspace(), [](std::string_view) { return offsetMesh(); });
    CHECK_EQ(items.size(), 4u);
    CHECK(items[0].mesh == primitiveMesh(Primitive::Ball));
    CHECK(items[0].model == partMatrix({}, {1.0f, 4.0f, 6.0f}));
    CHECK(items[1].mesh == nullptr);
    CHECK(items[1].model == partMatrix(CFrame(0.0f, 1.0f, 0.0f), {2.0f, 2.0f, 2.0f}));
    // A file keeps its own size, times Scale.
    CHECK(items[2].mesh != nullptr);
    CHECK(near(items[2].model.transformPoint({10.0f, 2.0f, 0.0f}), {30.0f, 6.0f, 0.0f}));
    CHECK(items[3].mesh == primitiveMesh(Primitive::CylinderY));
}

TEST(scene_world_meshes_draw_as_is)
{
    auto mesh = std::make_shared<const MeshData>(makePrimitive(Primitive::Block));
    const DrawItem chunk = worldMesh(mesh, Enum::Material::Grass);
    CHECK(chunk.model == Mat4::identity());
    CHECK(chunk.mesh == mesh);
    CHECK(chunk.material == Enum::Material::Grass);
    CHECK(chunk.color == (Color3{1.0f, 1.0f, 1.0f}));

    std::vector<DrawItem> items{chunk};
    auto bounds = sceneBounds(items);
    CHECK(bounds.has_value());
    if (bounds)
        CHECK(near(bounds->min, {-0.5f, -0.5f, -0.5f}));
}

TEST(lighting_follows_the_clock)
{
    // Within a degree: the client's tilt carries a tiny season term.
    CHECK(near(sunDirection(12.0, 0.0f), {0.0f, 1.0f, 0.0f}, 0.02f));
    // The client's sun rises towards +X and sets towards -X.
    CHECK(near(sunDirection(6.0, 0.0f), {1.0f, 0.0f, 0.0f}, 0.02f));
    CHECK(near(sunDirection(18.0, 0.0f), {-1.0f, 0.0f, 0.0f}, 0.02f));
    CHECK(near(sunDirection(36.0, 0.0f), {0.0f, 1.0f, 0.0f}, 0.02f));
    CHECK(sunDirection(0.0, 0.0f).y < -0.99f);
    // Away from the equator the noon sun leans over.
    CHECK(sunDirection(12.0, 45.0f).y < 0.99f);
    // The moon is opposite the sun in the day's turn.
    CHECK(near(moonDirection(12.0, 0.0f), {0.0f, -1.0f, 0.0f}, 0.02f));
    CHECK(near(moonDirection(6.0, 0.0f), {-1.0f, 0.0f, 0.0f}, 0.02f));

    auto game = DataModel::create();
    auto* service = game->getService<lunar::Lighting>();
    service->setClockTime(12.0);
    const auto noon = lightingFrom(*game);
    service->setClockTime(0.0);
    const auto midnight = lightingFrom(*game);
    CHECK(noon.sunColor.r > midnight.sunColor.r * 4.0f);
    CHECK(midnight.sunDirection.y > 0.0f); // the moon is up
    CHECK(noon.sky.b > midnight.sky.b * 2.0f);

    // Light colours are squared, the client's way: Ambient lights
    // everything and OutdoorAmbient adds what it has beyond that.
    service->setAmbient({0.5f, 0.5f, 0.5f});
    service->setOutdoorAmbient({1.0f, 0.5f, 0.0f});
    const auto ambient = lightingFrom(*game);
    CHECK(std::abs(ambient.ambient.r - 0.25f) < 1e-5f);
    CHECK(std::abs(ambient.skyAmbient.r - 0.75f) < 1e-5f);
    CHECK(std::abs(ambient.skyAmbient.g) < 1e-5f);
    CHECK(std::abs(ambient.skyAmbient.b) < 1e-5f);
    CHECK_EQ(ambient.colorCorrection[0][0], 1.0f);
    CHECK_EQ(ambient.colorCorrection[1][3], 0.0f);

    // A ColorCorrection child becomes the colour matrix: Brightness adds.
    auto correction = std::make_shared<ColorCorrectionEffect>();
    correction->setBrightness(0.25f);
    correction->setParent(service);
    const auto corrected = lightingFrom(*game);
    CHECK(std::abs(corrected.colorCorrection[0][3] - 0.25f) < 1e-5f);
    CHECK(std::abs(corrected.colorCorrection[2][2] - 1.0f) < 1e-5f);
    correction->setParent(nullptr);

    service->setFogStart(10.0f);
    service->setFogEnd(200.0f);
    service->setFogColor({1.0f, 0.0f, 0.0f});
    service->setExposureCompensation(1.0f);
    service->setClockTime(12.0);
    const auto foggy = lightingFrom(*game);
    CHECK_EQ(foggy.fogStart, 10.0f);
    CHECK_EQ(foggy.fogEnd, 200.0f);
    CHECK(foggy.fogColor == (Color3{1.0f, 0.0f, 0.0f}));
    CHECK(std::abs(foggy.exposure - 2.0f) < 1e-5f);
    CHECK(!foggy.atmosphere.enabled);

    // The client's Retro tone curve is Compatibility's; the rest get Default.
    CHECK(!foggy.retroTone);
    service->setTechnology(lunar::Enum::Technology::Compatibility);
    CHECK(lightingFrom(*game).retroTone);
    service->setTechnology(lunar::Enum::Technology::Future);
    CHECK(!lightingFrom(*game).retroTone);

    // Terrain's Clouds come along.
    CHECK(!lightingFrom(*game).clouds.enabled);
    auto terrain = std::make_shared<Terrain>();
    terrain->setParent(game->workspace());
    auto clouds = std::make_shared<Clouds>();
    clouds->setCover(0.8f);
    clouds->setParent(terrain.get());
    const auto cloudy = lightingFrom(*game);
    CHECK(cloudy.clouds.enabled);
    CHECK(std::abs(cloudy.clouds.cover - 0.8f) < 1e-6f);

    std::make_shared<Atmosphere>()->setParent(service);
    const auto hazy = lightingFrom(*game);
    CHECK(hazy.atmosphere.enabled);
    CHECK(hazy.atmosphere.densityPerStud() > 0.0f);
    CHECK(hazy.fogEnd >= 100000.0f);
}

namespace {

// Hands out one image per content id, counting how often each is asked for.
struct FakeTextures {
    std::map<std::string, ImagePtr, std::less<>> images;
    int calls = 0;

    TextureProvider provider()
    {
        return [this](std::string_view id) -> ImagePtr {
            ++calls;
            auto it = images.find(id);
            if (it == images.end()) {
                auto image = std::make_shared<lunar::Image>();
                image->width = 1;
                image->height = 1;
                image->rgba = {255, 255, 255, 255};
                it = images.emplace(std::string(id), std::move(image)).first;
            }
            return it->second;
        };
    }
};

template <class T>
std::shared_ptr<T> addChild(Instance* parent)
{
    auto child = std::make_shared<T>();
    child->setParent(parent);
    return child;
}

} // namespace

TEST(scene_textures_mesh_parts_and_file_meshes)
{
    auto game = DataModel::create();
    auto meshPart = std::make_shared<MeshPart>();
    meshPart->setMeshId("rbxassetid://1");
    meshPart->setTextureID("rbxassetid://2");
    meshPart->setParent(game->workspace());
    auto seeThrough = std::make_shared<MeshPart>();
    seeThrough->setMeshId("rbxassetid://1");
    seeThrough->setTextureID("rbxassetid://2");
    seeThrough->setTransparency(0.02f);
    seeThrough->setParent(game->workspace());
    auto part = addPart(game->workspace(), {}, {1.0f, 1.0f, 1.0f});
    auto special = addChild<SpecialMesh>(part.get());
    special->setMeshType(Enum::MeshType::FileMesh);
    special->setMeshId("rbxassetid://1");
    special->setTextureId("rbxassetid://3");
    special->setVertexColor({1.0f, 0.5f, 0.0f});

    FakeTextures textures;
    auto items = extractScene(*game->workspace(), [](std::string_view) { return offsetMesh(); }, textures.provider());
    CHECK_EQ(items.size(), 3u);
    CHECK_EQ(textures.calls, 2); // asked once per id
    CHECK(items[0].texture == textures.images.at("rbxassetid://2"));
    CHECK(items[0].alphaMode == TextureAlpha::Overlay);
    CHECK(items[1].texture == items[0].texture);
    CHECK(items[1].alphaMode == TextureAlpha::Transparency); // any Transparency lets texture alpha through
    CHECK(items[2].texture == textures.images.at("rbxassetid://3"));
    CHECK(items[2].textureColor == (Color3{1.0f, 0.5f, 0.0f}));
    CHECK(!items[0].physical);

    // Without the provider, or before the mesh loads, there's no texture.
    CHECK(extractScene(*game->workspace(), [](std::string_view) { return offsetMesh(); })[0].texture == nullptr);
    CHECK(extractScene(*game->workspace(), {}, textures.provider())[0].texture == nullptr);
}

TEST(scene_surface_appearance_makes_mesh_parts_physical)
{
    auto game = DataModel::create();
    auto meshPart = std::make_shared<MeshPart>();
    meshPart->setMeshId("rbxassetid://1");
    meshPart->setTextureID("rbxassetid://2");
    meshPart->setParent(game->workspace());
    auto appearance = addChild<SurfaceAppearance>(meshPart.get());
    appearance->setColorMap("rbxassetid://10");
    appearance->setNormalMap("rbxassetid://11");
    appearance->setRoughnessMap("rbxassetid://12");
    appearance->setMetalnessMap("rbxassetid://13");
    appearance->setAlphaMode(Enum::AlphaMode::TintMask);
    appearance->setColor({0.0f, 1.0f, 0.0f});

    FakeTextures textures;
    auto items = extractScene(*game->workspace(), [](std::string_view) { return offsetMesh(); }, textures.provider());
    CHECK_EQ(items.size(), 1u);
    CHECK(items[0].physical);
    CHECK(items[0].texture == textures.images.at("rbxassetid://10"));
    CHECK(items[0].normalMap == textures.images.at("rbxassetid://11"));
    CHECK(items[0].roughnessMap == textures.images.at("rbxassetid://12"));
    CHECK(items[0].metalnessMap == textures.images.at("rbxassetid://13"));
    CHECK(items[0].alphaMode == TextureAlpha::TintMask);
    CHECK(items[0].textureColor == (Color3{0.0f, 1.0f, 0.0f}));
}

TEST(scene_places_decals_on_block_faces)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 5.0f, 0.0f}, {4.0f, 2.0f, 6.0f});
    part->setTransparency(1.0f); // invisible parts still show their decals
    auto front = addChild<Decal>(part.get());
    front->setTexture("rbxassetid://5");
    front->setColor3({1.0f, 0.0f, 0.0f});
    front->setTransparency(0.25f);
    front->setZIndex(3);
    auto top = addChild<Decal>(part.get());
    top->setTexture("rbxassetid://5");
    top->setFace(Enum::NormalId::Top);
    auto hidden = addChild<Decal>(part.get());
    hidden->setTexture("rbxassetid://5");
    hidden->setTransparency(1.0f);

    FakeTextures textures;
    CHECK(extractScene(*game->workspace()).empty()); // no provider, no decals
    auto items = extractScene(*game->workspace(), {}, textures.provider());
    CHECK_EQ(items.size(), 2u);
    const DrawItem& d = items[0];
    CHECK(d.decal);
    CHECK(d.mesh == decalQuad());
    CHECK(!d.castShadow);
    CHECK(d.clampUv);
    CHECK_EQ(d.zIndex, 3);
    CHECK_EQ(d.transparency, 0.25f);
    CHECK(d.alphaMode == TextureAlpha::Transparency);
    CHECK(d.textureColor == (Color3{1.0f, 0.0f, 0.0f}));
    // The front face is -Z: the quad's top left corner (as seen from the
    // front, looking +Z) is at +X, +Y.
    CHECK(near(d.model.transformPoint({-0.5f, 0.5f, 0.0f}), {2.0f, 6.0f, -3.002f}));
    CHECK(near(d.model.transformPoint({0.5f, -0.5f, 0.0f}), {-2.0f, 4.0f, -3.002f}));
    // Top: across X and Z, image top towards the front.
    CHECK(near(items[1].model.transformPoint({-0.5f, 0.5f, 0.0f}), {-2.0f, 6.002f, -3.0f}));
    CHECK(near(items[1].model.transformPoint({0.5f, -0.5f, 0.0f}), {2.0f, 6.002f, 3.0f}));
}

TEST(scene_tiles_textures_by_studs)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {}, {8.0f, 3.0f, 1.0f});
    auto texture = addChild<lunar::Texture>(part.get());
    texture->setTexture("rbxassetid://6");
    texture->setStudsPerTileU(2.0f);
    texture->setStudsPerTileV(1.5f);
    texture->setOffsetStudsU(1.0f);

    FakeTextures textures;
    auto items = extractScene(*game->workspace(), {}, textures.provider());
    CHECK_EQ(items.size(), 2u);
    const DrawItem& t = items[0];
    CHECK(t.decal);
    CHECK(!t.clampUv);
    CHECK_EQ(t.uvScale[0], 4.0f); // 8 studs across, 2 per tile
    CHECK_EQ(t.uvScale[1], 2.0f);
    CHECK_EQ(t.uvOffset[0], -0.5f);
    CHECK_EQ(t.uvOffset[1], 0.0f);
}

TEST(scene_marks_stud_surfaces_on_blocks)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {}, {4.0f, 1.0f, 2.0f});
    part->setTopSurface(Enum::SurfaceType::Studs);
    part->setBottomSurface(Enum::SurfaceType::Inlet);
    part->setFrontSurface(Enum::SurfaceType::Weld);
    auto ball = addPart(game->workspace(), {}, {2.0f, 2.0f, 2.0f});
    ball->setShape(Enum::PartType::Ball);
    ball->setTopSurface(Enum::SurfaceType::Studs);

    auto items = extractScene(*game->workspace());
    CHECK_EQ(items[0].surfaces, surfaceBits(Enum::NormalId::Top, Enum::SurfaceType::Studs)
                                    | surfaceBits(Enum::NormalId::Bottom, Enum::SurfaceType::Inlet));
    CHECK_EQ(items[0].surfaces, (1u << 2) | (2u << 8));
    CHECK_EQ(items[1].surfaces, 0u);
}

TEST(scene_classic_heads_draw_the_head_mesh_with_their_face)
{
    auto game = DataModel::create();
    auto head = addPart(game->workspace(), {}, {2.0f, 1.0f, 1.0f});
    auto special = addChild<SpecialMesh>(head.get());
    special->setMeshType(Enum::MeshType::Head);
    special->setScale({1.25f, 1.25f, 1.25f});
    auto face = addChild<Decal>(head.get());
    face->setTexture("rbxassetid://5");
    auto back = addChild<Decal>(head.get());
    back->setTexture("rbxassetid://6");
    back->setFace(Enum::NormalId::Back);

    std::vector<std::string> asked;
    MeshPtr mesh = offsetMesh();
    const MeshProvider meshes = [&](std::string_view id) {
        asked.emplace_back(id);
        return mesh;
    };
    FakeTextures textures;
    auto items = extractScene(*game->workspace(), meshes, textures.provider());
    CHECK(std::ranges::contains(asked, std::string("rbxasset://avatar/heads/head.mesh")));
    CHECK_EQ(items.size(), 2u); // the head and its front face; a back Decal has nowhere to go
    CHECK(items[0].mesh == mesh);
    CHECK(!items[0].decal);
    CHECK(items[1].decal);
    CHECK(items[1].mesh == mesh);
    CHECK(items[1].texture == textures.images.at("rbxassetid://5"));
    CHECK(items[1].model == items[0].model);

    // Before the mesh arrives the head is a cylinder.
    auto waiting = extractScene(*game->workspace(), [](std::string_view) { return MeshPtr{}; }, textures.provider());
    CHECK_EQ(waiting.size(), 1u);
    CHECK(waiting[0].mesh == primitiveMesh(Primitive::CylinderY));
}
