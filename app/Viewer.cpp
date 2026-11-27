#include "Viewer.hpp"

#include "PlaceScene.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/platform/Window.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/Presenter.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/ui/GameUi.hpp>

#include <vulkan/vulkan.h>
#include <wayland-client.h>
// After wayland-client.h, which it needs.
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <variant>

namespace {

using namespace lunar::platform;
namespace render = lunar::render;

VkExtent2D toVk(Extent size)
{
    return {static_cast<std::uint32_t>(std::max(size.width, 0)), static_cast<std::uint32_t>(std::max(size.height, 0))};
}

render::FreeFlyCamera::Input flyInput(const InputState& input, bool looking)
{
    const Vec2d delta = looking ? input.mouseDelta() : Vec2d{};
    return {
        .mouseDeltaX = static_cast<float>(delta.x),
        .mouseDeltaY = static_cast<float>(delta.y),
        .forward = input.isDown(Key::W),
        .back = input.isDown(Key::S),
        .left = input.isDown(Key::A),
        .right = input.isDown(Key::D),
        .up = input.isDown(Key::E),
        .down = input.isDown(Key::Q),
        .fast = input.isDown(Key::LeftShift) || input.isDown(Key::RightShift),
    };
}

} // namespace

int viewPlace(const std::filesystem::path& path)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    // Meshes stream in while flying around; parts are boxes until theirs arrive.
    PlaceScene scene(*place->game);
    render::Lighting lighting = scene.lighting();
    double gameTime = 0.0;
    render::ParticleSystem particles(*place->game->workspace(), scene.textures());

    auto window = Window::create({.title = std::format("Lunarstone - {}", path.filename().string())});
    if (!window) {
        std::println(stderr, "{}", window.error());
        return 1;
    }

    const NativeHandles handles = window->nativeHandles();
    render::ContextOptions options;
    options.instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    options.createSurface = [&](VkInstance instance) -> std::expected<VkSurfaceKHR, std::string> {
        const VkWaylandSurfaceCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = handles.display,
            .surface = handles.surface,
        };
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface) != VK_SUCCESS)
            return std::unexpected("vkCreateWaylandSurfaceKHR failed");
        return surface;
    };
    auto context = render::Context::create(options);
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 1;
    }
    auto presenter = render::Presenter::create(**context, toVk(window->framebufferSize()));
    if (!presenter) {
        std::println(stderr, "Vulkan: {}", presenter.error());
        return 1;
    }
    (*presenter)->renderer().setMaterialTextures(scene.materialTextures());
    (*presenter)->renderer().setTerrainMaterials(scene.terrainMaterials());
    (*presenter)->renderer().addEffects(&scene.ribbons());
    (*presenter)->renderer().setLights(&scene.lights());
    (*presenter)->renderer().addEffects(&particles);

    // The 2D layer: the place's StarterGui, the classic chat and the core GUI.
    auto uiConfig = lunar::assets::AssetConfig::fromEnvironment();
    auto uiTextures
        = std::make_shared<lunar::assets::TextureLoader>(std::make_shared<lunar::assets::AssetCache>(uiConfig));
    lunar::ui::GameUi::Options uiOptions;
    uiOptions.contentDir = uiConfig.contentDir;
    uiOptions.images = [uiTextures](std::string_view id) { return uiTextures->image(id); };
    auto ui = lunar::ui::GameUi::create(**context, (*presenter)->format(), render::Presenter::framesInFlight,
                                        *place->game, std::move(uiOptions));
    if (!ui) {
        std::println(stderr, "Vulkan: {}", ui.error());
        return 1;
    }
    (*ui)->attach((*presenter)->renderer());
    (*ui)->viewports()->setAssets(scene.viewportAssets());
    (*ui)->onLeave = [&] { window->setShouldClose(true); };
    (*ui)->core().onFullscreen = [&](bool on) { window->setFullscreen(on); };

    // The whole terrain coarse first, for framing; detail streams in as the camera moves.
    scene.settleTerrain(std::nullopt);
    const Extent size = window->framebufferSize();
    render::Camera camera = render::framingCamera(scene.items(), static_cast<float>(size.width)
                                                                     / static_cast<float>(std::max(size.height, 1)));
    render::FreeFlyCamera fly(camera.cframe);
    if (auto bounds = render::sceneBounds(scene.items()))
        fly.speed = std::clamp(bounds->extent().magnitude() / 20.0f, 16.0f, 400.0f);
    std::println(
        "{} parts. Hold the right mouse button to look, WASD/QE to move, Shift for speed, / to chat, Esc for the menu.",
        scene.items().size());

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    auto fpsStart = last;
    int frames = 0;

    while (!window->shouldClose()) {
        window->pollEvents();
        bool uiTookRightClick = false;
        while (auto event = window->events().pop()) {
            if (const auto* resize = std::get_if<ResizeEvent>(&*event))
                (*presenter)->resize(toVk(resize->framebufferSize));
            const bool used = (*ui)->handle(*event);
            if (const auto* button = std::get_if<MouseButtonEvent>(&*event))
                uiTookRightClick |= used && button->button == MouseButton::Right;
        }

        const InputState& input = window->input();
        // Typing in the chat or a TextBox, or the menu being open, keeps keys from the camera.
        const bool uiHasKeys = (*ui)->keyboardCaptured() || (*ui)->menuOpen();
        if (input.wasPressed(Key::F11))
            window->toggleFullscreen();
        (*ui)->core().menuSettings.fullscreen = window->fullscreen();
        if (input.wasPressed(MouseButton::Right) && !uiTookRightClick && !(*ui)->menuOpen())
            window->setPointerLocked(true);
        if (input.wasReleased(MouseButton::Right) || !input.focused())
            window->setPointerLocked(false);

        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;
        fly.update(uiHasKeys ? render::FreeFlyCamera::Input{} : flyInput(input, window->pointerLocked()), dt);
        camera.cframe = fly.cframe();
        if (scene.refresh())
            lighting = scene.lighting(); // more of the sky may have loaded
        scene.updateTerrain(camera.cframe.position);
        const Extent framebuffer = window->framebufferSize();
        particles.step(dt, camera.cframe.position);
        (*ui)->update(dt, {static_cast<float>(framebuffer.width), static_cast<float>(framebuffer.height)},
                      static_cast<float>(window->scale()));
        gameTime += static_cast<double>(dt);
        lighting.time = gameTime; // animates the water
        (*presenter)->drawFrame(camera, scene.items(), lighting);

        ++frames;
        if (now - fpsStart >= std::chrono::seconds(1)) {
            window->setTitle(std::format("Lunarstone - {} - {} fps", path.filename().string(), frames));
            frames = 0;
            fpsStart = now;
        }
    }

    if (window->error()) {
        std::println(stderr, "{}", *window->error());
        return 1;
    }
    return 0;
}
