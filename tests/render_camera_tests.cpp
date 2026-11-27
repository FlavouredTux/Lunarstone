#include "framework.hpp"

#include <lunar/render/Camera.hpp>
#include <lunar/render/Frustum.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Renderer.hpp>

#include <cmath>
#include <numbers>

using namespace lunar;
using namespace lunar::render;

namespace {

bool near(Vector3 a, Vector3 b, float eps = 1e-4f)
{
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

} // namespace

TEST(camera_defaults_match_roblox)
{
    Camera camera;
    CHECK_EQ(camera.fieldOfView, 70.0f);
    CHECK(camera.cframe.lookVector() == (Vector3{0.0f, 0.0f, -1.0f}));
}

TEST(camera_fov_spans_the_vertical_view)
{
    Camera camera;
    camera.fieldOfView = 90.0f;
    Mat4 vp = camera.viewProjection(1.0f);
    // At the edge of a 90 degree FOV, a point 10 studs ahead and 10 up hits the top.
    Vec4 clip = vp * Vec4{0.0f, 10.0f, -10.0f, 1.0f};
    CHECK(std::abs(clip.y / clip.w + 1.0f) < 1e-5f);
}

TEST(camera_look_at_points_the_look_vector)
{
    CFrame cf = Camera::lookAt({10.0f, 10.0f, 10.0f}, {0.0f, 0.0f, 0.0f});
    CHECK(near(cf.position, {10.0f, 10.0f, 10.0f}));
    CHECK(near(cf.lookVector(), Vector3{-1.0f, -1.0f, -1.0f}.unit()));
    CHECK(std::abs(cf.rightVector().y) < 1e-6f); // no roll
    CHECK(cf.upVector().y > 0.0f);
    // Straight down still gives an orthonormal frame.
    CFrame down = Camera::lookAt({0.0f, 10.0f, 0.0f}, {});
    CHECK(near(down.lookVector(), {0.0f, -1.0f, 0.0f}));
    CHECK(std::abs(down.rightVector().dot(down.upVector())) < 1e-6f);
}

TEST(free_fly_round_trips_a_cframe)
{
    CFrame start = Camera::lookAt({3.0f, 4.0f, 5.0f}, {-2.0f, 1.0f, 9.0f});
    FreeFlyCamera fly(start);
    CHECK(near(fly.cframe().position, start.position));
    CHECK(near(fly.cframe().lookVector(), start.lookVector()));
    CHECK(near(fly.cframe().rightVector(), start.rightVector()));
}

TEST(free_fly_mouse_turns_and_clamps)
{
    FreeFlyCamera fly;
    fly.sensitivity = 0.01f;
    FreeFlyCamera::Input input;
    input.mouseDeltaX = 50.0f; // half a radian to the right
    fly.update(input, 0.0f);
    CHECK(fly.cframe().lookVector().x > 0.4f);

    input = {};
    input.mouseDeltaY = -1000.0f; // way up
    fly.update(input, 0.0f);
    CHECK(fly.pitch() < std::numbers::pi_v<float> / 2.0f);
    CHECK(fly.cframe().lookVector().y > 0.99f);
}

TEST(free_fly_moves_relative_to_view)
{
    FreeFlyCamera fly(Camera::lookAt({}, {1.0f, 0.0f, 0.0f}));
    fly.speed = 10.0f;
    FreeFlyCamera::Input input;
    input.forward = true;
    fly.update(input, 0.5f);
    CHECK(near(fly.position(), {5.0f, 0.0f, 0.0f}));

    input = {};
    input.right = true;
    input.fast = true;
    fly.update(input, 0.1f);
    CHECK(near(fly.position(), {5.0f, 0.0f, 4.0f}));

    input = {};
    input.up = true;
    input.forward = true;
    input.back = true; // cancels out
    fly.update(input, 1.0f);
    CHECK(near(fly.position(), {5.0f, 10.0f, 4.0f}));
}

TEST(sun_shadow_covers_the_near_view)
{
    Camera camera;
    camera.cframe = Camera::lookAt({10.0f, 20.0f, 30.0f}, {60.0f, 5.0f, -40.0f});
    const Vector3 sun = Vector3{0.3f, 0.8f, -0.2f}.unit();
    const float aspect = 16.0f / 9.0f;
    const SunShadow shadow = SunShadow::fit(camera, aspect, sun);

    // Every point the camera sees within shadowDistance lands on the map.
    const float tanHalf = std::tan(camera.fieldOfView * std::numbers::pi_v<float> / 360.0f);
    for (float d : {1.0f, 50.0f, SunShadow::shadowDistance}) {
        for (float sx : {-1.0f, 1.0f}) {
            for (float sy : {-1.0f, 1.0f}) {
                const Vector3 p = camera.cframe.position + camera.cframe.lookVector() * d
                                  + camera.cframe.rightVector() * (sx * d * tanHalf * aspect)
                                  + camera.cframe.upVector() * (sy * d * tanHalf);
                const Vec4 m = shadow.viewProjection * Vec4{p.x, p.y, p.z, 1.0f};
                CHECK(std::abs(m.x) <= 1.0f);
                CHECK(std::abs(m.y) <= 1.0f);
                CHECK(m.z > 0.0f);
                CHECK(m.z < 1.0f);
                CHECK(shadow.reaches(p, 0.0f));
            }
        }
    }
    // Things far towards the sun still cast; things far below don't matter.
    CHECK(shadow.reaches(camera.cframe.position + sun * 500.0f, 1.0f));
    CHECK(!shadow.reaches(camera.cframe.position - sun * 2000.0f, 1.0f));

    // Turning keeps the size; nudging the camera moves the map in whole texels.
    Camera turned = camera;
    turned.cframe = Camera::lookAt({10.0f, 20.0f, 30.0f}, {-60.0f, 0.0f, 0.0f});
    CHECK(std::abs(SunShadow::fit(turned, aspect, sun).radius - shadow.radius) < 1e-3f);
    Camera nudged = camera;
    nudged.cframe.position = nudged.cframe.position + Vector3{0.013f, 0.0f, 0.007f};
    const SunShadow moved = SunShadow::fit(nudged, aspect, sun);
    const float texel = 2.0f / static_cast<float>(SunShadow::resolution);
    for (float shift : {moved.viewProjection.m[3][0] - shadow.viewProjection.m[3][0],
                        moved.viewProjection.m[3][1] - shadow.viewProjection.m[3][1]}) {
        const float texels = shift / texel;
        CHECK(std::abs(texels - std::round(texels)) < 1e-2f);
    }
}

TEST(graphics_presets)
{
    // Average is the renderer's defaults; Minimal turns things off, Max up.
    CHECK(graphicsSettings(GraphicsPreset::Average) == GraphicsSettings{});
    const GraphicsSettings minimal = graphicsSettings(GraphicsPreset::Minimal);
    CHECK(!minimal.shadows);
    CHECK(!minimal.sharpReflections);
    CHECK(!minimal.pixelLights);
    CHECK(!minimal.physicalShading);
    CHECK(minimal.lodPixelError > meshLodPixelError);
    CHECK(minimal.drawDistance < GraphicsSettings{}.drawDistance);
    CHECK(minimal.textureQuality < GraphicsSettings{}.textureQuality);
    CHECK(minimal.terrainDistance < 1.0f);
    const GraphicsSettings max = graphicsSettings(GraphicsPreset::Max);
    CHECK(max.shadows);
    CHECK(max.pixelLights);
    CHECK(max.shadowResolution > GraphicsSettings{}.shadowResolution);
    CHECK(max.shadowDistance > GraphicsSettings{}.shadowDistance);
    CHECK(max.lodPixelError < meshLodPixelError);
    CHECK(max.terrainDistance > 1.0f);
    // Potato: Minimal's off switches, and everything Minimal turns down turned down further.
    const GraphicsSettings potato = graphicsSettings(GraphicsPreset::Potato);
    CHECK(potato.potato && !minimal.potato && !GraphicsSettings{}.potato);
    CHECK(!potato.shadows && !potato.pixelLights && !potato.physicalShading && !potato.postEffects);
    CHECK(potato.lodPixelError > minimal.lodPixelError);
    CHECK(potato.drawDistance < minimal.drawDistance);
    CHECK(potato.textureQuality < minimal.textureQuality);
    CHECK(potato.textureSide > 0 && minimal.textureSide == 0);
    CHECK(potato.terrainDistance < minimal.terrainDistance);
    CHECK(potato.particleDensity < minimal.particleDensity);
    // No preset touches the render scale.
    for (GraphicsPreset preset :
         {GraphicsPreset::Minimal, GraphicsPreset::Average, GraphicsPreset::Max, GraphicsPreset::Potato})
        CHECK_EQ(graphicsSettings(preset).renderScale, 1.0f);

    CHECK(parseGraphicsPreset("minimal") == GraphicsPreset::Minimal);
    CHECK(parseGraphicsPreset("average") == GraphicsPreset::Average);
    CHECK(parseGraphicsPreset("max") == GraphicsPreset::Max);
    CHECK(parseGraphicsPreset("potato") == GraphicsPreset::Potato);
    CHECK(!parseGraphicsPreset("raytracing"));
    CHECK(!parseGraphicsPreset("Max"));
}

TEST(sun_shadow_fit_takes_its_distance_and_resolution)
{
    Camera camera;
    camera.cframe = Camera::lookAt({10.0f, 20.0f, 30.0f}, {60.0f, 5.0f, -40.0f});
    const Vector3 sun = Vector3{0.3f, 0.8f, -0.2f}.unit();
    const float aspect = 16.0f / 9.0f;
    const SunShadow near = SunShadow::fit(camera, aspect, sun);
    CHECK_EQ(near.texels, SunShadow::resolution);
    const SunShadow far = SunShadow::fit(camera, aspect, sun, 2.0f * SunShadow::shadowDistance, 4096);
    CHECK_EQ(far.texels, 4096u);
    // Twice the distance, twice the box.
    CHECK(std::abs(far.radius - 2.0f * near.radius) < 1e-2f);
    // The point twice as far still lands on the map.
    const Vector3 p = camera.cframe.position + camera.cframe.lookVector() * (1.9f * SunShadow::shadowDistance);
    const Vec4 m = far.viewProjection * Vec4{p.x, p.y, p.z, 1.0f};
    CHECK(std::abs(m.x) <= 1.0f);
    CHECK(std::abs(m.y) <= 1.0f);
    // It moves in whole texels of its own resolution.
    Camera nudged = camera;
    nudged.cframe.position = nudged.cframe.position + Vector3{0.013f, 0.0f, 0.007f};
    const SunShadow moved = SunShadow::fit(nudged, aspect, sun, 2.0f * SunShadow::shadowDistance, 4096);
    const float texel = 2.0f / 4096.0f;
    for (float shift : {moved.viewProjection.m[3][0] - far.viewProjection.m[3][0],
                        moved.viewProjection.m[3][1] - far.viewProjection.m[3][1]}) {
        const float texels = shift / texel;
        CHECK(std::abs(texels - std::round(texels)) < 1e-2f);
    }
}

TEST(frustum_culls_spheres_and_boxes_outside_the_view)
{
    // The default camera at the origin looking down -Z.
    Camera camera;
    const Frustum frustum(camera.viewProjection(16.0f / 9.0f));
    CHECK(frustum.intersects({0.0f, 0.0f, -50.0f}, 1.0f));
    CHECK(!frustum.intersects({0.0f, 0.0f, 50.0f}, 1.0f)); // behind
    CHECK(!frustum.intersects({500.0f, 0.0f, -50.0f}, 1.0f)); // far off to the side
    // A sphere straddling the near plane, and one past the side plane by less than its radius.
    CHECK(frustum.intersects({0.0f, 0.0f, 0.0f}, 1.0f));
    CHECK(frustum.intersects({80.0f, 0.0f, -50.0f}, 50.0f));
    CHECK(!frustum.intersects({80.0f, 0.0f, -50.0f}, 1.0f));

    CHECK(frustum.intersectsBox({0.0f, 0.0f, -50.0f}, {1.0f, 1.0f, 1.0f}));
    CHECK(!frustum.intersectsBox({0.0f, 0.0f, 50.0f}, {1.0f, 1.0f, 1.0f}));
    // A long box off to the side that reaches into view only along x.
    CHECK(frustum.intersectsBox({80.0f, 0.0f, -50.0f}, {50.0f, 1.0f, 1.0f}));
    CHECK(!frustum.intersectsBox({80.0f, 0.0f, -50.0f}, {1.0f, 50.0f, 1.0f}));
}
