#include "framework.hpp"

// The PlayerModule camera: zoom steps, pitch limits, mouse sensitivity,
// first person, the zoom spring and occlusion.

#include <lunar/Classes.hpp>
#include <lunar/physics/PlayerCamera.hpp>
#include <lunar/physics/World.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using namespace lunar;
using namespace lunar::physics;

namespace {

bool near(float a, float b, float eps = 1e-3f)
{
    return std::abs(a - b) <= eps;
}

struct Scene {
    std::shared_ptr<DataModel> game = DataModel::create();
    Workspace& ws = *game->workspace();
    std::shared_ptr<Model> character = std::make_shared<Model>();
    std::shared_ptr<Part> root = std::make_shared<Part>();
    std::shared_ptr<Humanoid> humanoid = std::make_shared<Humanoid>();

    Scene()
    {
        character->setName("Character");
        character->setParent(&ws);
        root->setName("HumanoidRootPart");
        root->setSize({2.0f, 2.0f, 1.0f});
        root->setPosition({0.0f, 3.0f, 0.0f});
        root->setAnchored(true);
        root->setParent(character.get());
        humanoid->setParent(character.get());
    }
};

} // namespace

TEST(camera_constants)
{
    // PlayerModule 0.742 CameraInput.lua / BaseCamera.lua.
    CHECK(near(camera::mouseYawPerPixel, 0.0087266f, 1e-6f));
    CHECK(near(camera::mousePitchPerPixel, 0.0067195f, 1e-6f));
    CHECK_EQ(camera::defaultDistance, 12.5f);
    CHECK(near(camera::maxPitch, 80.0f * std::numbers::pi_v<float> / 180.0f, 1e-6f));
    CHECK_EQ(camera::zoomStiffness, 4.5f);
}

TEST(camera_step_zoom)
{
    // One wheel notch out and in from the default distance.
    CHECK(near(stepZoom(12.5f, 1.0f), 19.75f));
    CHECK(near(stepZoom(12.5f, -1.0f), 7.6666667f));
    // Below a stud snaps to first person's half stud.
    CHECK_EQ(stepZoom(1.2f, -1.0f), 0.5f);
    CHECK_EQ(stepZoom(0.5f, 0.1f), 1.0f);
}

TEST(camera_follows_subject)
{
    Scene s;
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    CHECK(cam.cameraSubject() == s.humanoid.get());
    CHECK(cam.cameraType() == Enum::CameraType::Custom);
    pc.update(1.0f / 60.0f, {});
    // Focus at the head offset, the camera 12.5 studs back, 15 degrees down, behind (+Z).
    CHECK(near(cam.focus().position.y, 4.5f));
    const Vector3 back = cam.cframe().position - cam.focus().position;
    CHECK(near(back.magnitude(), 12.5f, 1e-3f));
    CHECK(near(std::asin(-cam.cframe().lookVector().y), 15.0f * camera::degrees, 1e-4f));
    CHECK(back.z > 0.0f);
}

TEST(camera_mouse_orbit_and_pitch_limit)
{
    Scene s;
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    PlayerCamera::Input input;
    // Without the right button the mouse does nothing.
    input.mouseDeltaX = 100.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK(near(cam.cframe().lookVector().x, 0.0f, 1e-4f));
    // With it, 100 px is 50 degrees of yaw to the right.
    input.rotate = true;
    pc.update(1.0f / 60.0f, input);
    const Vector3 look = cam.cframe().lookVector();
    CHECK(near(std::atan2(look.x, -look.z), 50.0f * camera::degrees, 1e-3f));
    // Pitch stops at 80 degrees.
    input.mouseDeltaX = 0.0f;
    input.mouseDeltaY = 10000.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK(near(std::asin(-cam.cframe().lookVector().y), 80.0f * camera::degrees, 1e-3f));
    input.mouseDeltaY = -10000.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK(near(std::asin(cam.cframe().lookVector().y), 80.0f * camera::degrees, 1e-3f));
}

TEST(camera_zoom_spring_and_first_person)
{
    Scene s;
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    PlayerCamera::Input input;
    input.wheel = -1.0f; // out
    pc.update(1.0f / 60.0f, input);
    CHECK(near(pc.goalDistance(), 19.75f));
    // The spring lags, then settles.
    CHECK(pc.zoom() < 19.0f);
    input.wheel = 0.0f;
    for (int i = 0; i < 120; ++i)
        pc.update(1.0f / 60.0f, input);
    CHECK(near(pc.zoom(), 19.75f, 0.01f));
    // All the way in: first person, pointer locked, character hidden.
    input.wheel = 20.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK(pc.firstPerson());
    CHECK(pc.pointerLocked({}));
    input.wheel = 0.0f;
    for (int i = 0; i < 120; ++i)
        pc.update(1.0f / 60.0f, input);
    CHECK(near(pc.zoom(), 0.5f, 0.01f));
    CHECK_EQ(pc.characterTransparency(), 1.0f);
    CHECK_EQ(s.root->localTransparencyModifier(), 1.0f);
    // Players.CameraMaxZoomDistance caps zooming out.
    pc.setZoomLimits(0.5f, 20.0f);
    input.wheel = -100.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK_EQ(pc.goalDistance(), 20.0f);
    CHECK(!pc.firstPerson());
}

TEST(camera_popper_occlusion)
{
    Scene s;
    // A wall 6 studs behind the character.
    auto wall = std::make_shared<Part>();
    wall->setSize({40.0f, 40.0f, 1.0f});
    wall->setPosition({0.0f, 3.0f, 6.5f});
    wall->setAnchored(true);
    wall->setParent(&s.ws);
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    pc.update(1.0f / 60.0f, {});
    // Pulled in straight away, so the near plane (0.1 ahead of the eye) sits
    // on the wall: the eye ends up a hair past the wall's face at z = 6.
    CHECK(cam.cframe().position.z < 6.0f + 0.1f);
    CHECK(cam.cframe().position.z > 5.5f);
    // A see-through wall doesn't occlude.
    wall->setTransparency(0.5f);
    for (int i = 0; i < 240; ++i)
        pc.update(1.0f / 60.0f, {});
    CHECK(cam.cframe().position.z > 10.0f);
}

TEST(camera_follows_a_part_subject)
{
    // The battle royale template points CameraSubject at its bus's root part
    // and raises CameraMinZoomDistance to 50 while the bus flies.
    Scene s;
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    pc.update(1.0f / 60.0f, {});
    auto bus = std::make_shared<Part>();
    bus->setSize({4.0f, 4.0f, 8.0f});
    bus->setPosition({100.0f, 2000.0f, 50.0f});
    bus->setAnchored(true);
    bus->setParent(&s.ws);
    cam.setCameraSubject(bus.get());
    pc.setZoomLimits(50.0f, 400.0f);
    CHECK_EQ(pc.goalDistance(), 50.0f);
    for (int i = 0; i < 240; ++i)
        pc.update(1.0f / 60.0f, {});
    // Focus on the part itself, no head offset, the zoom sprung out to 50.
    CHECK(near((cam.focus().position - bus->position()).magnitude(), 0.0f));
    CHECK(near((cam.cframe().position - bus->position()).magnitude(), 50.0f, 0.05f));
    // It moves with the part.
    bus->setPosition({300.0f, 2000.0f, 50.0f});
    pc.update(1.0f / 60.0f, {});
    CHECK(near(cam.focus().position.x, 300.0f));
    // Back on the character, with the limits lowered again.
    cam.setCameraSubject(s.humanoid.get());
    pc.setZoomLimits(0.5f, 400.0f);
    pc.update(1.0f / 60.0f, {});
    CHECK(near(cam.focus().position.y, 4.5f));
    CHECK_EQ(pc.goalDistance(), 50.0f);
}

TEST(camera_lock_first_person_holds_the_zoom_in)
{
    // CameraMode LockFirstPerson: first person, and the wheel can't zoom out.
    Scene s;
    World world(s.ws);
    Camera& cam = *s.ws.ensureCurrentCamera();
    PlayerCamera pc(world, cam);
    pc.setSubject(s.humanoid.get());
    auto player = std::make_shared<Player>();
    player->setCameraMode(Enum::CameraMode::LockFirstPerson);
    pc.setZoomLimits(*player);
    PlayerCamera::Input input;
    input.wheel = -10.0f; // out
    for (int i = 0; i < 120; ++i) {
        pc.update(1.0f / 60.0f, input);
        input.wheel = 0.0f;
    }
    CHECK(pc.firstPerson());
    CHECK(near(pc.zoom(), 0.5f, 0.01f));
    // Classic again: the player's own limits.
    player->setCameraMode(Enum::CameraMode::Classic);
    player->setCameraMaxZoomDistance(30.0f);
    pc.setZoomLimits(*player);
    input.wheel = -100.0f;
    pc.update(1.0f / 60.0f, input);
    CHECK_EQ(pc.goalDistance(), 30.0f);
}
