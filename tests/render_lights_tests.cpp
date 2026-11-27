#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/LightClusters.hpp>
#include <lunar/render/LightGrid.hpp>
#include <lunar/render/LocalLights.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

using lunar::Attachment;
using lunar::CFrame;
using lunar::Color3;
using lunar::DataModel;
using lunar::Part;
using lunar::PointLight;
using lunar::SpotLight;
using lunar::SurfaceLight;
using lunar::Vector3;
using namespace lunar::render;
namespace Enum = lunar::Enum;

namespace {

bool close(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

bool close(Vector3 a, Vector3 b, float tolerance = 1e-3f)
{
    return (a - b).magnitude() <= tolerance;
}

std::shared_ptr<Part> partAt(const std::shared_ptr<DataModel>& game, CFrame frame, Vector3 size)
{
    auto part = std::make_shared<Part>();
    part->setCFrame(frame);
    part->setSize(size);
    part->setParent(game->workspace());
    return part;
}

// A white point light of a range at a point, as the renderer gets it.
LocalLight pointLight(Vector3 at, float range, bool shadows = false)
{
    LocalLight light;
    light.position = at;
    light.range = range;
    light.color = {1.0f, 1.0f, 1.0f};
    light.brightness = 1.0f;
    light.shadows = shadows;
    return light;
}

DrawItem block(Vector3 center, Vector3 size)
{
    DrawItem item;
    item.model = Mat4::translation(center) * Mat4::scale(size);
    return item;
}

} // namespace

TEST(lights_point_light_shines_from_its_part)
{
    auto game = DataModel::create();
    auto part = partAt(game, CFrame(10.0f, 5.0f, 0.0f), {2.0f, 2.0f, 2.0f});
    auto light = std::make_shared<PointLight>();
    light->setRange(200.0f);
    light->setBrightness(2.0f);
    light->setColor({1.0f, 0.5f, 0.0f});
    light->setParent(part.get());

    LocalLight placed;
    CHECK(placeLight(*light, placed));
    CHECK(placed.kind == LocalLight::Kind::Point);
    CHECK(close(placed.position, {10.0f, 5.0f, 0.0f}));
    CHECK(close(placed.range, 120.0f)); // the client's limit
    CHECK_EQ(placed.cosHalfAngle, 0.0f);
    // Per pixel: Color to the 2.2 times Brightness; in the grid, Color to
    // 8 bits squared times Brightness.
    CHECK(close(placed.pixelColor().g, 2.0f * std::pow(0.5f, 2.2f)));
    CHECK(close(placed.gridColor().g, 2.0f * (128.0f / 255.0f) * (128.0f / 255.0f)));
    CHECK(close(placed.gridColor().r, 2.0f));
}

TEST(lights_spot_light_points_out_of_its_face)
{
    auto game = DataModel::create();
    // Turned a quarter round Y: its Front (-Z) faces -X.
    auto part = partAt(game, CFrame(0.0f, 3.0f, 0.0f) * CFrame::angles(0.0f, std::numbers::pi_v<float> / 2.0f, 0.0f),
                       {2.0f, 2.0f, 2.0f});
    auto spot = std::make_shared<SpotLight>();
    spot->setAngle(60.0f);
    spot->setParent(part.get());
    LocalLight placed;
    CHECK(placeLight(*spot, placed));
    CHECK(placed.kind == LocalLight::Kind::Spot);
    CHECK(close(placed.position, {0.0f, 3.0f, 0.0f}));
    CHECK(close(placed.direction, {-1.0f, 0.0f, 0.0f}));
    CHECK(close(placed.cosHalfAngle, std::cos(30.0f * std::numbers::pi_v<float> / 180.0f)));
    spot->setFace(Enum::NormalId::Top);
    CHECK(placeLight(*spot, placed));
    CHECK(close(placed.direction, {0.0f, 1.0f, 0.0f}));

    // Under an attachment: from it, turned with it.
    auto attachment = std::make_shared<Attachment>();
    attachment->setCFrame(CFrame(0.0f, 0.0f, -2.0f));
    attachment->setParent(part.get());
    spot->setParent(attachment.get());
    spot->setFace(Enum::NormalId::Front);
    CHECK(placeLight(*spot, placed));
    CHECK(close(placed.position, {-2.0f, 3.0f, 0.0f}));
    CHECK(close(placed.direction, {-1.0f, 0.0f, 0.0f}));
    // Half of 180 degrees or more: the client keeps a sliver of cone.
    spot->setAngle(180.0f);
    CHECK(placeLight(*spot, placed));
    CHECK(close(placed.cosHalfAngle, 0.0001f, 1e-6f));
}

TEST(lights_surface_light_covers_its_face)
{
    auto game = DataModel::create();
    auto part = partAt(game, CFrame(0.0f, 0.0f, 0.0f), {4.0f, 2.0f, 6.0f});
    auto surface = std::make_shared<SurfaceLight>();
    surface->setFace(Enum::NormalId::Right);
    surface->setParent(part.get());
    LocalLight placed;
    CHECK(placeLight(*surface, placed));
    CHECK(placed.kind == LocalLight::Kind::Surface);
    CHECK(close(placed.position, {2.0f, 0.0f, 0.0f}));
    CHECK(close(placed.direction, {1.0f, 0.0f, 0.0f}));
    // X faces span Y and Z, Y faces Z and X.
    CHECK(close(placed.axisU, {0.0f, 1.0f, 0.0f}));
    CHECK(close(placed.halfU, 1.0f));
    CHECK(close(placed.axisV, {0.0f, 0.0f, 1.0f}));
    CHECK(close(placed.halfV, 3.0f));
    surface->setFace(Enum::NormalId::Top);
    CHECK(placeLight(*surface, placed));
    CHECK(close(placed.position, {0.0f, 1.0f, 0.0f}));
    CHECK(close(placed.axisU, {0.0f, 0.0f, 1.0f}));
    CHECK(close(placed.halfU, 3.0f));
    CHECK(close(placed.halfV, 2.0f));
    CHECK(close(placed.reach(), 16.0f + std::sqrt(13.0f)));
}

TEST(lights_that_cannot_shine_are_left_out)
{
    auto game = DataModel::create();
    auto part = partAt(game, CFrame(), {2.0f, 2.0f, 2.0f});
    auto light = std::make_shared<PointLight>();
    light->setParent(part.get());
    LocalLight placed;
    CHECK(placeLight(*light, placed));
    light->setEnabled(false);
    CHECK(!placeLight(*light, placed));
    light->setEnabled(true);
    light->setRange(0.0f);
    CHECK(!placeLight(*light, placed));
    light->setRange(8.0f);
    auto model = std::make_shared<lunar::Model>();
    model->setParent(game->workspace());
    light->setParent(model.get());
    CHECK(!placeLight(*light, placed));
    auto spot = std::make_shared<SpotLight>();
    spot->setAngle(0.0f);
    spot->setParent(part.get());
    CHECK(!placeLight(*spot, placed));
}

TEST(lights_pixel_strength_is_the_clients_future_falloff)
{
    const LocalLight light = pointLight({0.0f, 0.0f, 0.0f}, 20.0f);
    // Full strength out to 71% of the range, facing it.
    CHECK(close(light.pixelStrength({0.0f, -5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), 1.0f));
    CHECK(close(light.pixelStrength({0.0f, -14.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), 1.0f));
    // Then 110 / (d^2 + 110) times a smooth cut-off: 2.8 studs past at 17.
    const float t = 2.8f / 5.8f;
    const float expected = 110.0f * (1.0f - t * t * (3.0f - 2.0f * t)) / (2.8f * 2.8f + 110.0f);
    CHECK(close(light.pixelStrength({0.0f, -17.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), expected));
    CHECK_EQ(light.pixelStrength({0.0f, -21.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), 0.0f);
    // N.L: turned away, nothing; edge on, nothing.
    CHECK_EQ(light.pixelStrength({0.0f, -5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}), 0.0f);
    CHECK(close(light.pixelStrength({0.0f, -5.0f, 0.0f}, {1.0f, 0.0f, 0.0f}), 0.0f));

    // A spot's cone, squared: inside it full, outside nothing.
    LocalLight spot = light;
    spot.kind = LocalLight::Kind::Spot;
    spot.direction = {0.0f, -1.0f, 0.0f};
    spot.cosHalfAngle = std::cos(30.0f * std::numbers::pi_v<float> / 180.0f);
    CHECK(close(spot.pixelStrength({0.0f, -5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), 1.0f));
    CHECK_EQ(spot.pixelStrength({5.0f, -5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), 0.0f);
    const float c = (std::cos(20.0f * std::numbers::pi_v<float> / 180.0f) - spot.cosHalfAngle) / (1.0f - spot.cosHalfAngle);
    const Vector3 p{5.0f * std::tan(20.0f * std::numbers::pi_v<float> / 180.0f), -5.0f, 0.0f};
    const Vector3 toLight = (Vector3{} - p).unit();
    CHECK(close(spot.pixelStrength(p, toLight), c * c));
}

TEST(lights_grid_strength_is_the_clients_voxel_falloff)
{
    const LocalLight light = pointLight({0.0f, 0.0f, 0.0f}, 20.0f);
    // In voxels: full to 3.55 out, then 1.35 / (d^2 + 1.35) times the
    // cut-off, whichever way a surface faces.
    CHECK(close(light.gridStrength({8.0f, 0.0f, 0.0f}), 1.0f));
    const float past = 4.0f - 0.71f * 5.0f;
    const float t = past / (5.0f - 0.71f * 5.0f);
    const float expected = (1.0f - t * t * (3.0f - 2.0f * t)) * 1.35f / (past * past + 1.35f);
    CHECK(close(light.gridStrength({0.0f, 16.0f, 0.0f}), expected));
    CHECK_EQ(light.gridStrength({0.0f, 0.0f, 21.0f}), 0.0f);
    // A cone, linear across it.
    LocalLight spot = light;
    spot.kind = LocalLight::Kind::Spot;
    spot.direction = {1.0f, 0.0f, 0.0f};
    spot.cosHalfAngle = 0.5f;
    CHECK(close(spot.gridStrength({8.0f, 0.0f, 0.0f}), 1.0f));
    CHECK_EQ(spot.gridStrength({-8.0f, 0.0f, 0.0f}), 0.0f);
    const Vector3 p = Vector3{std::cos(0.5f), std::sin(0.5f), 0.0f} * 8.0f;
    CHECK(close(spot.gridStrength(p), (std::cos(0.5f) - 0.5f) / 0.5f));
}

TEST(lights_follow_the_workspace)
{
    auto game = DataModel::create();
    auto part = partAt(game, CFrame(1.0f, 2.0f, 3.0f), {2.0f, 2.0f, 2.0f});
    auto first = std::make_shared<PointLight>();
    first->setParent(part.get());
    LocalLights lights(*game->workspace());
    CHECK_EQ(lights.count(), 1u);
    auto second = std::make_shared<SpotLight>();
    second->setParent(part.get());
    CHECK_EQ(lights.count(), 2u);
    std::vector<LocalLight> out;
    lights.collect(out);
    CHECK_EQ(out.size(), 2u);
    // They move with their part, and follow a moving character's offset.
    part->setCFrame(CFrame(5.0f, 2.0f, 3.0f));
    lights.follow(part.get(), {0.0f, 1.0f, 0.0f});
    out.clear();
    lights.collect(out);
    CHECK(close(out[0].position, {5.0f, 3.0f, 3.0f}));
    first->setParent(nullptr);
    CHECK_EQ(lights.count(), 1u);
    second->setEnabled(false);
    out.clear();
    lights.collect(out);
    CHECK(out.empty());
    CHECK_EQ(lights.shining(), 0u);
}

TEST(lightclusters_bin_lights_by_tile_and_depth)
{
    Camera camera;
    camera.cframe = CFrame(0.0f, 0.0f, 0.0f); // looking down -Z
    const std::vector<LocalLight> lights{pointLight({0.0f, 0.0f, -50.0f}, 5.0f), pointLight({0.0f, 0.0f, 50.0f}, 5.0f),
                                         pointLight({0.0f, 0.0f, -2.0f}, 10.0f)};
    LightClusters clusters;
    clusters.build(lights, camera, 1280, 720);
    CHECK_EQ(clusters.tilesX, 20u);
    CHECK_EQ(clusters.tilesY, 12u);
    // The far light where it is on screen, at its depth; not elsewhere.
    auto has = [](std::span<const std::uint32_t> list, std::uint32_t light) {
        return std::ranges::find(list, light) != list.end();
    };
    CHECK(has(clusters.at(640, 360, 50.0f), 0));
    CHECK(!has(clusters.at(640, 360, 20.0f), 0));
    CHECK(!has(clusters.at(10, 10, 50.0f), 0));
    // Behind the camera: nowhere.
    for (std::uint32_t y = 0; y < 720; y += 64)
        for (std::uint32_t x = 0; x < 1280; x += 64)
            CHECK(!has(clusters.at(x, y, 50.0f), 1));
    // Around the camera: everywhere near.
    CHECK(has(clusters.at(10, 10, 1.0f), 2));
    CHECK(has(clusters.at(1270, 710, 5.0f), 2));
    // Slices: the first ends 10 studs out, each after 1.5 times as deep.
    CHECK_EQ(LightClusters::sliceOf(9.0f), 0u);
    CHECK_EQ(LightClusters::sliceOf(11.0f), 1u);
    CHECK_EQ(LightClusters::sliceOf(16.0f), 2u);
    for (float depth : {3.0f, 30.0f, 300.0f, 900.0f})
        CHECK(LightClusters::sliceStart(LightClusters::sliceOf(depth)) <= depth);
}

TEST(lightgrid_light_map_holds_a_lights_light)
{
    LightGrid grid;
    const std::vector<LocalLight> lights{pointLight({2.0f, 2.0f, 2.0f}, 20.0f)};
    grid.build({}, {}, {.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true}, lights);
    CHECK(grid.lightReady());
    // Blurred over a voxel each way, and encoded to 8 bits.
    const float near = grid.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r;
    const float mid = grid.light(LightGrid::voxelAt({14.0f, 2.0f, 2.0f})).r;
    const float far = grid.light(LightGrid::voxelAt({18.0f, 2.0f, 2.0f})).r;
    CHECK(close(near, 1.0f, 0.03f));
    CHECK(mid < near);
    CHECK(far < mid);
    CHECK_EQ(grid.light(LightGrid::voxelAt({40.0f, 2.0f, 2.0f})).r, 0.0f);
    // Without GlobalShadows too.
    LightGrid unshadowed;
    unshadowed.build({}, {}, {.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = false}, lights);
    CHECK(close(unshadowed.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r, near));
}

TEST(lightgrid_walls_stop_only_shadowed_lights)
{
    // A wall two voxels thick between the light and a point past it.
    const std::vector<DrawItem> wall{block({10.0f, 8.0f, 2.0f}, {8.0f, 40.0f, 40.0f})};
    const Vector3 past{22.0f, 2.0f, 2.0f};
    const LightGrid::Settings settings{.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true};
    for (bool shadows : {false, true}) {
        const std::vector<LocalLight> lights{pointLight({2.0f, 2.0f, 2.0f}, 30.0f, shadows)};
        LightGrid open, walled;
        open.build({}, {}, settings, lights);
        walled.build(wall, {}, settings, lights);
        const float without = open.light(LightGrid::voxelAt(past)).r;
        const float with = walled.light(LightGrid::voxelAt(past)).r;
        CHECK(without > 0.1f);
        if (shadows)
            CHECK(with < without * 0.1f);
        else
            CHECK(close(with, without, 1e-4f));
    }
}

TEST(lightgrid_lights_again_when_a_light_moves)
{
    LightGrid grid;
    const LightGrid::Settings settings{.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true};
    std::vector<LocalLight> lights{pointLight({2.0f, 2.0f, 2.0f}, 16.0f)};
    grid.build({}, {}, settings, lights);
    grid.takeUploads();
    CHECK(grid.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r > 0.5f);
    lights[0].position = {200.0f, 2.0f, 2.0f};
    grid.update({}, {}, settings, lights);
    grid.wait();
    grid.update({}, {}, settings, lights);
    CHECK_EQ(grid.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r, 0.0f);
    CHECK(grid.light(LightGrid::voxelAt({200.0f, 2.0f, 2.0f})).r > 0.5f);
    CHECK(!grid.takeUploads().empty());
}

// The grid keeps each shadowed light's occlusion and each light's strengths
// in a chunk from one lighting to the next, and works out again only the
// voxels around what changed: what it lights must be what a grid built
// afresh with the same lights would. The lights are far enough apart in one
// chunk that one changing doesn't bring the others' voxels with it.
TEST(lightgrid_kept_light_matches_a_fresh_grid)
{
    const std::vector<DrawItem> wall{block({-52.0f, 8.0f, -60.0f}, {8.0f, 40.0f, 40.0f})};
    const LightGrid::Settings settings{.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true};
    // Lights are told apart by their instances.
    const auto a = std::make_shared<lunar::Part>(), b = std::make_shared<lunar::Part>(),
               c = std::make_shared<lunar::Part>(), d = std::make_shared<lunar::Part>();
    std::vector<LocalLight> lights{pointLight({-60.0f, 2.0f, -60.0f}, 30.0f, true),
                                   pointLight({100.0f, 6.0f, 100.0f}, 12.0f), pointLight({20.0f, 4.0f, 20.0f}, 12.0f)};
    lights[0].key = a.get();
    lights[1].key = b.get();
    lights[2].key = c.get();
    LightGrid grid;
    grid.build(wall, {}, settings, lights);
    auto same = [&] {
        LightGrid fresh;
        fresh.build(wall, {}, settings, lights);
        for (float x = -130.0f; x <= 130.0f; x += 4.0f)
            for (float y = -8.0f; y <= 24.0f; y += 4.0f)
                for (float z = -130.0f; z <= 130.0f; z += 4.0f) {
                    const GridPoint voxel = LightGrid::voxelAt({x, y, z});
                    const Color3 kept = grid.light(voxel), want = fresh.light(voxel);
                    if (kept.r != want.r || kept.g != want.g || kept.b != want.b)
                        return false;
                }
        return true;
    };
    for (int step = 0; step < 12; ++step) {
        // The moving one: a little a step, and once a long way.
        lights.back().position
            = lights.back().position + (step == 2 ? Vector3{40.0f, 0.0f, 0.0f} : Vector3{-3.0f, 0.0f, 2.0f});
        if (step == 3)
            lights[0].brightness = 0.5f; // a dimmed one, shadowed
        if (step == 4)
            lights[1].color = {1.0f, 0.5f, 0.25f}; // one in the moving one's chunk, recoloured
        if (step == 5)
            lights[0].position = lights[0].position + Vector3{0.0f, 0.0f, 4.0f}; // the shadowed one moves
        if (step == 7) { // one comes, in the middle of the order
            lights.insert(lights.begin() + 1, pointLight({60.0f, 2.0f, -10.0f}, 18.0f));
            lights[1].key = d.get();
        }
        if (step == 9) // one goes
            lights.erase(lights.begin());
        grid.update(wall, {}, settings, lights);
        grid.wait();
        grid.update(wall, {}, settings, lights);
        grid.wait();
        CHECK(same());
    }
}

// A camera nowhere (NaN, as a character flung a billion studs away leaves
// it) or far past where whole voxels fit an int leaves the grid where it
// was, and lit as it was; a grid built there sits at the world's middle.
// Following it, the grid's chunk coordinates overflowed and lightChunk
// wrote its texels far outside the chunk.
TEST(lightgrid_stays_put_for_a_camera_nowhere)
{
    const LightGrid::Settings settings{.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true};
    const auto part = std::make_shared<lunar::Part>();
    std::vector<LocalLight> lights{pointLight({2.0f, 2.0f, 2.0f}, 20.0f, true)};
    lights[0].key = part.get();
    const std::vector<DrawItem> wall{block({10.0f, 8.0f, 2.0f}, {8.0f, 40.0f, 40.0f})};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const Vector3 nowhere[] = {{nan, nan, nan}, {nan, 0.0f, 0.0f}, {inf, -inf, inf},
                               {1e12f, -1e12f, 1e12f}, {0.0f, 3e9f, 0.0f}};
    LightGrid fresh;
    fresh.build(wall, {}, settings, lights);
    const float lit = fresh.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r;
    CHECK(lit > 0.5f);
    for (const Vector3& camera : nowhere) {
        LightGrid built;
        built.build(wall, camera, settings, lights);
        CHECK_EQ(built.originVoxel(), fresh.originVoxel());
        CHECK_EQ(built.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r, lit);

        LightGrid followed;
        followed.setScanInterval({});
        followed.update(wall, {}, settings, lights);
        for (int frame = 0; frame < 3; ++frame) {
            followed.update(wall, camera, settings, lights);
            followed.wait();
        }
        CHECK_EQ(followed.originVoxel(), fresh.originVoxel());
        CHECK_EQ(followed.light(LightGrid::voxelAt({2.0f, 2.0f, 2.0f})).r, lit);
    }
    // A voxel past the int range is the last one there is, not an overflow.
    CHECK(LightGrid::voxelAt({nan, nan, nan}).x < 0);
    CHECK(LightGrid::voxelAt({1e30f, 0.0f, 0.0f}).x > 0);
    CHECK(LightGrid::voxelAt({-1e30f, 0.0f, 0.0f}).x < 0);
}
