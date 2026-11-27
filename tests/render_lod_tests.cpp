#include "render_gpu.hpp"

#include <lunar/render/Lod.hpp>
#include <lunar/render/Offscreen.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

using lunar::CFrame;
using lunar::Color3;
using lunar::MeshData;
using lunar::Vector3;
using lunar::Enum::RenderFidelity;
using namespace lunar::render;

namespace {

Camera cameraAt(Vector3 eye, float fieldOfView = 70.0f)
{
    Camera camera;
    camera.cframe = Camera::lookAt(eye, {0.0f, 0.0f, 0.0f});
    camera.fieldOfView = fieldOfView;
    return camera;
}

// A unit square facing +Z: two triangles at full detail, one coarser, and
// a single sliver coarser still. The coarse levels add a vertex of their own.
std::shared_ptr<const MeshData> squareWithLevels()
{
    auto mesh = std::make_shared<MeshData>();
    for (auto [x, y] : {std::pair{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}, {0.0f, 0.5f}})
        mesh->vertices.push_back({.position = {x, y, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}});
    mesh->indices = {0, 1, 2, 0, 2, 3};
    mesh->lodIndices = {0, 1, 4, 0, 1, 2};
    mesh->lodEnds = {3, 6};
    return mesh;
}

} // namespace

TEST(lod_levels_switch_at_the_client_distances)
{
    const LodSelector lods(cameraAt({0.0f, 0.0f, 0.0f}));
    auto at = [&](float distance) { return lods.levelAt(distance * distance, 5); };
    CHECK_EQ(at(0.0f), 0u);
    CHECK_EQ(at(499.0f), 0u);
    CHECK_EQ(at(501.0f), 1u);
    CHECK_EQ(at(999.0f), 1u);
    CHECK_EQ(at(1001.0f), 2u);
    CHECK_EQ(at(2001.0f), 3u);
    CHECK_EQ(at(3001.0f), 4u);
    CHECK_EQ(at(1e6f), 4u);
    // A mesh with three levels stays at its last.
    CHECK_EQ(lods.levelAt(5000.0f * 5000.0f, 3), 2u);
    CHECK_EQ(lods.levelAt(5000.0f * 5000.0f, 1), 0u);
}

TEST(lod_distance_is_to_the_box_and_scales_with_zoom)
{
    // The box's near face is 450 studs off, its centre 550.
    const LodSelector wide(cameraAt({0.0f, 0.0f, 550.0f}));
    CHECK_EQ(wide.level({}, {100.0f, 100.0f, 100.0f}, 3), 0u);
    CHECK_EQ(wide.level({}, {10.0f, 10.0f, 10.0f}, 3), 1u);
    CHECK(std::abs(squaredDistanceToBox({0.0f, 0.0f, 550.0f}, {}, {100.0f, 100.0f, 100.0f}) - 450.0f * 450.0f) < 1.0f);
    CHECK_EQ(squaredDistanceToBox({1.0f, 2.0f, 3.0f}, {}, {5.0f, 5.0f, 5.0f}), 0.0f);

    // Zoomed in to half the field of view's tangent, switches happen twice
    // as far out.
    const float zoomed = 2.0f * std::atan(0.5f * std::tan(35.0f * std::numbers::pi_v<float> / 180.0f)) * 180.0f
                         / std::numbers::pi_v<float>;
    const LodSelector narrow(cameraAt({}, zoomed));
    CHECK_EQ(narrow.levelAt(990.0f * 990.0f, 5), 0u);
    CHECK_EQ(narrow.levelAt(1010.0f * 1010.0f, 5), 1u);
}

TEST(lod_render_fidelity)
{
    const LodSelector lods(cameraAt({}));
    CHECK_EQ(lods.levelAt(1e8f, 3, RenderFidelity::Precise), 0u);
    CHECK_EQ(lods.levelAt(0.0f, 3, RenderFidelity::Performance), 2u);
    CHECK_EQ(lods.levelAt(0.0f, 1, RenderFidelity::Performance), 0u);
    CHECK_EQ(lods.levelAt(700.0f * 700.0f, 3, RenderFidelity::Automatic), 1u);
}

TEST(lod_levels_by_screen_error)
{
    // 720 pixels over 70 degrees: one stud 1000 studs off is about half a
    // pixel, so 1.2 pixels there is 2.33 studs.
    const LodSelector lods(cameraAt({}), 720.0f);
    const std::array<float, 3> errors{0.0f, 1.0f, 4.0f};
    auto at = [&](float distance, float scale = 1.0f, RenderFidelity fidelity = RenderFidelity::Automatic) {
        return lods.levelForError(distance * distance, scale, errors, fidelity);
    };
    CHECK_EQ(at(0.0f), 0u);
    CHECK_EQ(at(400.0f), 0u);
    CHECK_EQ(at(500.0f), 1u);
    CHECK_EQ(at(1700.0f), 1u);
    CHECK_EQ(at(1800.0f), 2u);
    // A part scaled up twice strays twice as far.
    CHECK_EQ(at(500.0f, 2.0f), 0u);
    CHECK_EQ(at(1000.0f, 2.0f), 1u);
    CHECK_EQ(at(1e5f, 1.0f, RenderFidelity::Precise), 0u);
    CHECK_EQ(at(0.0f, 1.0f, RenderFidelity::Performance), 2u);
}

TEST(lod_quality_draw_distances)
{
    CHECK_EQ(qualityDrawDistance(1), 200.0f);
    CHECK_EQ(qualityDrawDistance(10), 700.0f);
    CHECK_EQ(qualityDrawDistance(18), 9051.0f);
    CHECK_EQ(qualityDrawDistance(21), 100000.0f);
    CHECK_EQ(qualityDrawDistance(0), 100000.0f);
    CHECK_EQ(qualityDrawDistance(99), 100000.0f);
}

GPU_TEST(offscreen_draws_far_meshes_at_coarser_levels)
{
    auto offscreen = OffscreenRenderer::create(context, 64, 48);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    const auto mesh = squareWithLevels();
    auto square = [&](float z, RenderFidelity fidelity = RenderFidelity::Automatic) {
        DrawItem item{.model = partMatrix(CFrame(0.0f, 0.0f, z), {40.0f, 40.0f, 1.0f}), .color = {1.0f, 0.0f, 0.0f}};
        item.mesh = mesh;
        item.renderFidelity = fidelity;
        return item;
    };
    Lighting unshadowed;
    unshadowed.shadows = false;
    Camera camera;
    camera.farPlane = 5000.0f;

    // 100 studs off: full detail; 700: the second level; 1200: the third.
    std::vector items{square(-100.0f), square(-700.0f), square(-1200.0f)};
    (*offscreen)->render(camera, items, unshadowed);
    const FrameStats& stats = (*offscreen)->renderer().stats();
    CHECK_EQ(stats.visible, 3u);
    CHECK_EQ(stats.drawCalls, 3u);
    CHECK_EQ(stats.triangles, 4u);
    CHECK_EQ(stats.levels[0], 1u);
    CHECK_EQ(stats.levels[1], 1u);
    CHECK_EQ(stats.levels[2], 1u);

    // The same level shares a draw; Precise stays at full detail.
    items = {square(-1200.0f), square(-1300.0f), square(-1400.0f, RenderFidelity::Precise)};
    (*offscreen)->render(camera, items, unshadowed);
    CHECK_EQ((*offscreen)->renderer().stats().drawCalls, 2u);
    CHECK_EQ((*offscreen)->renderer().stats().triangles, 4u);
}

GPU_TEST(offscreen_leaves_out_parts_past_the_draw_distance)
{
    auto offscreen = OffscreenRenderer::create(context, 64, 48);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    auto box = [](float z) {
        return DrawItem{.model = partMatrix(CFrame(0.0f, 0.0f, z), {4.0f, 4.0f, 4.0f}), .color = {1.0f, 1.0f, 1.0f}};
    };
    const std::vector items{box(-50.0f), box(-150.0f), box(-300.0f)};
    Camera camera;
    (*offscreen)->renderer().setDrawDistance(qualityDrawDistance(1)); // 200 studs
    (*offscreen)->render(camera, items);
    CHECK_EQ((*offscreen)->renderer().stats().visible, 2u);
    CHECK_EQ((*offscreen)->renderer().stats().tooFar, 1u);
    (*offscreen)->renderer().setDrawDistance(qualityDrawDistance(21));
    (*offscreen)->render(camera, items);
    CHECK_EQ((*offscreen)->renderer().stats().visible, 3u);
    CHECK_EQ((*offscreen)->renderer().stats().tooFar, 0u);
}
