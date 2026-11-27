#include "GameWindow.hpp"
#include "RtcChat.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/ui/TextEditor.hpp>

#include <vulkan/vulkan.h>
#include <wayland-client.h>
// After wayland-client.h, which it needs.
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <thread>
#include <utility>
#include <variant>

using namespace lunar::platform;
namespace render = lunar::render;

namespace {

VkExtent2D toVk(Extent size)
{
    return {static_cast<std::uint32_t>(std::max(size.width, 0)), static_cast<std::uint32_t>(std::max(size.height, 0))};
}

} // namespace

bool healthBarsFromEnvironment()
{
    const char* value = std::getenv("LUNAR_HEALTH_BARS");
    return value && *value && std::string_view(value) != "0";
}

std::optional<Screenshot> parseScreenshot(std::string_view text)
{
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon + 1 == text.size())
        return std::nullopt;
    double at = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + colon, at);
    if (ec != std::errc{} || end != text.data() + colon || at < 0.0)
        return std::nullopt;
    return Screenshot{at, std::filesystem::path(text.substr(colon + 1))};
}

GameWindow::GameWindow(Window window, Options options) : window_(std::move(window)), options_(std::move(options))
{
    std::ranges::sort(options_.screenshots, {}, &Screenshot::at);
}

std::expected<std::unique_ptr<GameWindow>, std::string> GameWindow::create(lunar::DataModel& game,
                                                                           const Options& options)
{
    auto window = Window::create({.title = options.title});
    if (!window)
        return std::unexpected(window.error());
    std::unique_ptr<GameWindow> made(new GameWindow(std::move(*window), options));
    Window& w = made->window_;
    // Ctrl+C/X/V in the chat and TextBoxes go through this window's clipboard
    // until it closes.
    lunar::ui::setClipboard({.read = [&w] { return w.clipboardText(); },
                             .write = [&w](std::string text) { w.setClipboardText(std::move(text)); }});
    const NativeHandles handles = w.nativeHandles();
    render::ContextOptions contextOptions;
    contextOptions.instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    contextOptions.createSurface = [&](VkInstance instance) -> std::expected<VkSurfaceKHR, std::string> {
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
    // The player's kept settings, under what the command line asked for.
    render::GraphicsSettings graphics = options.graphics;
    bool shiftLock = options.shiftLock;
    if (!options.settingsFile.empty()) {
        made->saved_ = loadPlayerSettings(options.settingsFile).value_or(PlayerSettings{});
        const PlayerSettings& s = made->saved_;
        if (!options.graphicsFromFlags) {
            if (s.graphics) {
                graphics = render::graphicsSettings(*s.graphics);
                graphics.renderScale = options.graphics.renderScale;
            }
            graphics.rayTracing = s.rayTracing;
        }
        shiftLock = options.shiftLock || s.shiftLock;
        made->sensitivity_ = s.cameraSensitivity;
        made->volume_ = s.volume;
    }
    contextOptions.rayTracing = graphics.rayTracing;
    auto context = render::Context::create(contextOptions);
    if (!context)
        return std::unexpected("Vulkan: " + context.error());
    made->context_ = std::move(*context);
    if (graphics.rayTracing && !made->context_->rayTracing())
        std::println(stderr, "this GPU can't ray trace (no ray queries): shadow maps instead");
    auto presenter = render::Presenter::create(
        *made->context_, toVk(w.framebufferSize()),
        {.vsync = options.vsync,
         .readable = !options.screenshots.empty() || !options.screenshotRequest.empty() || !options.control.empty()});
    if (!presenter)
        return std::unexpected("Vulkan: " + presenter.error());
    made->presenter_ = std::move(*presenter);
    made->graphics_ = graphics;
    made->shiftLock_ = shiftLock;
    made->automated_ = options.automated;
    if (!options.control.empty()) {
        made->control_ = std::make_unique<ControlFile>(options.control);
        made->control_->note = [](const std::string& line) { std::println("play: {}", line); };
        std::println("play: input from {} too (--control)", options.control.string());
    }
    if (auto ui = made->makeUi(game); !ui)
        return std::unexpected(ui.error());
    // LUNAR_HEALTH_BARS=1: the health bars on from the start (F7 and the
    // menu still toggle them).
    lunar::ui::MenuSettings& menu = made->ui_->core().menuSettings;
    menu.healthBars = healthBarsFromEnvironment();
    if (!options.settingsFile.empty()) {
        menu.performanceStats = made->saved_.performanceStats;
        menu.healthBars = menu.healthBars || made->saved_.healthBars;
        menu.beatRing = made->saved_.beatRing;
        if (made->saved_.fullscreen)
            made->window_.setFullscreen(true);
    }
    made->overlaysAtStart_ = {menu.performanceStats, menu.healthBars, menu.beatRing};
    made->presenter_->renderer().setGraphics(graphics);
    made->opened_ = Clock::now();
    made->last_ = made->second_ = made->nextFrame_ = made->opened_;
    return made;
}

// The 2D layer over a game: the place's GUI, the classic chat and the core GUI.
std::expected<void, std::string> GameWindow::makeUi(lunar::DataModel& game)
{
    auto uiConfig = lunar::assets::AssetConfig::fromEnvironment();
    auto uiTextures
        = std::make_shared<lunar::assets::TextureLoader>(std::make_shared<lunar::assets::AssetCache>(uiConfig));
    lunar::ui::GameUi::Options uiOptions;
    uiOptions.contentDir = uiConfig.contentDir;
    uiOptions.images = [uiTextures](std::string_view id) { return uiTextures->image(id); };
    uiOptions.map = options_.map;
    if (!options_.gameOverlay) {
        uiOptions.showChat = false;
        uiOptions.showCoreGui = false;
        uiOptions.topInset = 0.0f;
    }
    auto ui = lunar::ui::GameUi::create(*context_, presenter_->format(), render::Presenter::framesInFlight, game,
                                        std::move(uiOptions));
    if (!ui)
        return std::unexpected("Vulkan: " + ui.error());
    ui_ = std::move(*ui);
    if (options_.gameOverlay) {
        if (!rtc_)
            rtc_ = std::make_unique<RtcChatSource>();
        ui_->setRtcChatSource(rtc_.get());
    }
    ui_->attach(presenter_->renderer());
    ui_->onLeave = [this] { window_.setShouldClose(true); };
    wireMenu();
    return {};
}

std::expected<void, std::string> GameWindow::rebind(lunar::DataModel& game)
{
    vkDeviceWaitIdle(context_->device());
    presenter_->renderer().setOverlay(nullptr);
    // The core GUI's own settings go with the player, as the window's do.
    const lunar::ui::MenuSettings kept = ui_->core().menuSettings;
    ui_.reset();
    auto made = makeUi(game);
    if (made) {
        lunar::ui::MenuSettings& settings = ui_->core().menuSettings;
        settings.performanceStats = kept.performanceStats;
        settings.healthBars = kept.healthBars;
        settings.beatRing = kept.beatRing;
    }
    return made;
}

// The menu's Settings tab, wired to the window, its renderer and what the
// sessions ask of it each frame (the settings as they are: a teleport keeps them).
void GameWindow::wireMenu()
{
    lunar::ui::CoreGui& core = ui_->core();
    lunar::ui::MenuSettings& settings = core.menuSettings;
    // The preset the settings are, if any (the render scale and ray tracing
    // are settings of their own, which a preset keeps).
    auto withOwn = [this](lunar::render::GraphicsSettings preset) {
        preset.renderScale = graphics_.renderScale;
        preset.rayTracing = graphics_.rayTracing;
        preset.reflectEverything = graphics_.reflectEverything;
        preset.ambientOcclusion = graphics_.ambientOcclusion;
        preset.shadowAllLights = graphics_.shadowAllLights;
        return preset;
    };
    for (const auto preset :
         {render::GraphicsPreset::Potato, render::GraphicsPreset::Minimal, render::GraphicsPreset::Average,
          render::GraphicsPreset::Max})
        if (withOwn(render::graphicsSettings(preset)) == graphics_)
            settings.graphics = preset;
    settings.fullscreen = window_.fullscreen();
    settings.cameraSensitivity = sensitivity_;
    settings.volume = volume_;
    settings.shiftLock = shiftLock_;
    const bool keeps = !options_.settingsFile.empty();
    settings.rayTracingAvailable = context_->rayTracingSupported();
    settings.rayTracingActive = context_->rayTracing();
    settings.rayTracing = keeps ? saved_.rayTracing : graphics_.rayTracing;
    core.onGraphicsQuality = [this, withOwn](render::GraphicsPreset preset) {
        graphics_ = withOwn(render::graphicsSettings(preset));
        presenter_->renderer().setGraphics(graphics_);
        if (onGraphics)
            onGraphics(graphics_);
        saved_.graphics = preset;
        keepSettings();
    };
    core.onFullscreen = [this](bool on) {
        window_.setFullscreen(on);
        saved_.fullscreen = on;
        keepSettings();
    };
    core.onCameraSensitivity = [this](float value) {
        sensitivity_ = value;
        saved_.cameraSensitivity = value;
        keepSettings();
    };
    core.onVolume = [this](float value) {
        volume_ = value;
        saved_.volume = value;
        keepSettings();
    };
    core.onShiftLock = [this](bool on) {
        shiftLock_ = on;
        saved_.shiftLock = on;
        keepSettings();
    };
    // Ray tracing takes a context made for it: the choice is for the next
    // window, which only a window that keeps settings can pass on.
    if (keeps)
        core.onRayTracing = [this](bool on) {
            saved_.rayTracing = on;
            keepSettings();
        };
}

void GameWindow::keepSettings()
{
    if (!options_.settingsFile.empty())
        settingsDirty_ = true;
}

void GameWindow::saveSettings()
{
    if (options_.settingsFile.empty() || !settingsDirty_)
        return;
    if (auto saved = savePlayerSettings(options_.settingsFile, saved_); !saved)
        std::println(stderr, "settings: {}", saved.error());
    settingsDirty_ = false;
    savedAt_ = Clock::now();
}

GameWindow::~GameWindow()
{
    // The overlays' switches (the menu's toggles, F7) are kept when they moved.
    if (ui_ && !options_.settingsFile.empty()) {
        const lunar::ui::MenuSettings& menu = ui_->core().menuSettings;
        if (std::array<bool, 3>{menu.performanceStats, menu.healthBars, menu.beatRing} != overlaysAtStart_) {
            saved_.performanceStats = menu.performanceStats;
            saved_.healthBars = menu.healthBars;
            saved_.beatRing = menu.beatRing;
            keepSettings();
        }
    }
    saveSettings();
    lunar::ui::setClipboard({});
    // The GUI draws through the presenter's renderer: it goes first.
    ui_.reset();
    presenter_.reset();
}

void GameWindow::setFrameCap(int fps)
{
    options_.fpsCap = std::max(0, fps);
    // The next frame is timed from now, so a cap just lowered isn't made up for
    // by a burst of frames that were already owed.
    nextFrame_ = Clock::now();
}

float GameWindow::beginFrame(const std::function<void(const Event&)>& game,
                             const std::function<void(const Event&, bool)>& observe)
{
    if (options_.fpsCap > 0) {
        // --fps-cap: each frame starts no sooner than this after the one
        // before. Sleeping wakes a fraction of a millisecond late: sleep to
        // just short of it, then yield the rest away.
        const auto period = std::chrono::nanoseconds(1'000'000'000 / options_.fpsCap);
        std::this_thread::sleep_until(nextFrame_ - std::chrono::milliseconds(1));
        while (Clock::now() < nextFrame_)
            std::this_thread::yield();
        // A long frame is made up by the next ones; more than a frame
        // behind, start over from now rather than rush to catch up.
        nextFrame_ += period;
        if (nextFrame_ + period < Clock::now())
            nextFrame_ = Clock::now();
    }
    if (hidden()) {
        // Hidden: there's no vsync to wait on, so the frames are paced here.
        const auto period = std::chrono::nanoseconds(1'000'000'000 / hiddenFps);
        std::this_thread::sleep_until(nextHiddenFrame_);
        nextHiddenFrame_ = std::max(nextHiddenFrame_ + period, Clock::now());
    } else {
        nextHiddenFrame_ = Clock::now();
    }
    window_.pollEvents();
    window_.watchFrames();
    // A script plays it: the window counts as focused whoever has the keyboard.
    if (automated_ && !window_.input().focused())
        window_.inject(lunar::platform::FocusEvent{.focused = true});
    if (control_) {
        control_->step(window_);
        if (!capturing_ && !requested_)
            if (auto shot = control_->takeScreenshot()) {
                requested_ = *shot;
                presenter_->captureNextFrame();
                capturing_ = true;
            }
    }
    while (auto event = window_.events().pop()) {
        if (const auto* resize = std::get_if<ResizeEvent>(&*event))
            presenter_->resize(toVk(resize->framebufferSize));
        const bool privateTyping
            = ui_->chat().focused() && ui_->chat().selectedTab() == lunar::ui::ClassicChat::Tab::Rtc;
        const bool usedByUi = ui_->handle(*event);
        // RTC drafts are private to the external chat. Never deliver their
        // keyboard presses to game scripts. Releases still clear held keys.
        const auto* key = std::get_if<KeyEvent>(&*event);
        if (observe && !(privateTyping && key && key->pressed))
            observe(*event, usedByUi || uiHasKeys());
        if (!usedByUi && game)
            game(*event);
    }
    if (window_.input().wasPressed(Key::F11)) {
        window_.toggleFullscreen();
        saved_.fullscreen = window_.fullscreen();
        keepSettings();
    }
    ui_->core().menuSettings.fullscreen = window_.fullscreen();
    if (options_.seconds > 0.0 && elapsed() >= options_.seconds)
        window_.setShouldClose(true);
    // A screenshot that's due is taken of this frame as it's presented.
    if (!capturing_ && screenshot_ < options_.screenshots.size() && elapsed() >= options_.screenshots[screenshot_].at) {
        presenter_->captureNextFrame();
        capturing_ = true;
    }
    // One asked for with --screenshot-on's file (looked for four times a second).
    if (!capturing_ && !options_.screenshotRequest.empty()
        && Clock::now() - requestCheck_ >= std::chrono::milliseconds(250)) {
        requestCheck_ = Clock::now();
        std::error_code ec;
        if (std::filesystem::exists(options_.screenshotRequest, ec)) {
            std::string out;
            std::getline(std::ifstream(options_.screenshotRequest), out);
            std::filesystem::remove(options_.screenshotRequest, ec);
            if (!out.empty()) {
                requested_ = out;
                presenter_->captureNextFrame();
                capturing_ = true;
            }
        }
    }
    const auto now = Clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last_).count(), 0.1f);
    last_ = now;
    return dt;
}

bool GameWindow::uiHasKeys() const
{
    return ui_->keyboardCaptured() || ui_->menuOpen() || ui_->mapOpen() || ui_->loadingScreenUp();
}

lunar::Vector2 GameWindow::size() const
{
    const Extent size = window_.framebufferSize();
    return {static_cast<float>(size.width), static_cast<float>(size.height)};
}

lunar::Vector2 GameWindow::logicalSize() const
{
    const float scale = std::max(static_cast<float>(window_.scale()), 1e-3f);
    return size() / scale;
}

float GameWindow::aspect() const
{
    const lunar::Vector2 s = size();
    return s.x / std::max(s.y, 1.0f);
}

void GameWindow::setRtcRobloxUsername(std::string_view username)
{
    if (rtc_)
        rtc_->setRobloxUsername(username);
}

void GameWindow::bindGui(lunar::ClientSession& session)
{
    session.setViewportSize(logicalSize());
    bindGui(session.client());
}

void GameWindow::bindGui(lunar::ScriptContext& context)
{
    ui_->gui().setDeferredInput(true);
    ui_->update(0.0, size(), static_cast<float>(window_.scale()));
    context.setTextMeasure([this](std::string_view text, const lunar::Font& font, float size, float width, bool rich) {
        return ui_->measureText(text, font, size, width, rich);
    });
    context.setImageLoadedChecker([this](std::string_view id) { return ui_->gui().imageLoaded(id); });
    context.setGuiHitTest([this](float x, float y) { return ui_->guiObjectsAt(x, y); });
    context.setGuiFocus(
        [this](lunar::TextBox* box, bool capture, bool entered) { ui_->focusTextBox(box, capture, entered); });
    context.setGuiLayoutQuery([this](lunar::Instance* object) { ui_->ensureGuiGeometry(object); });
    context.setGameInvitePromptHandler(ui_->gameInviteHandler());
    ui_->gui().onPageStep = context.guiPageStepper();
}

void GameWindow::updateUi(float dt)
{
    if (rtc_) {
        const auto extent = window_.framebufferSize();
        rtc_->setAvailable(window_.focused() && !window_.shouldClose() && !ui_->loadingScreenUp() && ui_->chat().visible
                           && extent.width > 0 && extent.height > 0);
    }
    ui_->update(dt, size(), static_cast<float>(window_.scale()));
}

std::optional<int> GameWindow::endFrame()
{
    if (settingsDirty_ && Clock::now() - savedAt_ >= std::chrono::seconds(1))
        saveSettings();
    if (capturing_)
        if (auto image = presenter_->takeCapture()) {
            capturing_ = false;
            const Screenshot shot = requested_ ? Screenshot{elapsed(), *std::exchange(requested_, std::nullopt)}
                                               : options_.screenshots[screenshot_++];
            const std::string encoded
                = shot.out.extension() == ".ppm" ? render::encodePpm(*image) : render::encodePng(*image);
            std::ofstream out(shot.out, std::ios::binary);
            out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            if (out)
                std::println("screenshot: {:.1f} s, {}x{} -> {}", elapsed(), image->width, image->height,
                             shot.out.string());
            else
                std::println(stderr, "screenshot: can't write {}", shot.out.string());
        }
    ++frames_;
    const auto now = Clock::now();
    if (now - second_ < std::chrono::seconds(1))
        return std::nullopt;
    const int frames = frames_;
    frames_ = 0;
    second_ = now;
    // The menu's performance overlay, once a second.
    const render::FrameStats& stats = presenter_->renderer().stats();
    ui_->core().setPerformanceStats({.fps = static_cast<float>(frames),
                                     .frameMilliseconds = 1000.0f / static_cast<float>(std::max(frames, 1)),
                                     .drawCalls = stats.drawCalls,
                                     .triangles = stats.triangles});
    return frames;
}

double GameWindow::elapsed() const
{
    return std::chrono::duration<double>(Clock::now() - opened_).count();
}

std::optional<lunar::Enum::KeyCode> robloxKeyCode(Key key)
{
    return lunar::ui::keyCodeFor(key);
}

std::optional<Key> platformKey(lunar::Enum::KeyCode code)
{
    for (int i = 1; i < static_cast<int>(Key::Count); ++i)
        if (robloxKeyCode(static_cast<Key>(i)) == code)
            return static_cast<Key>(i);
    return std::nullopt;
}

void forwardInput(lunar::ClientInput& input, const Event& event, lunar::Vector2& mouse, bool pointerLocked,
                  bool uiModal, bool processed, lunar::ClientInput::GuiDispatch gui)
{
    if (const auto* key = std::get_if<KeyEvent>(&event)) {
        if (key->repeat)
            return;
        if (const auto code = robloxKeyCode(key->key))
            key->pressed ? input.keyDown(*code, processed, std::move(gui))
                         : input.keyUp(*code, processed, std::move(gui));
    } else if (const auto* button = std::get_if<MouseButtonEvent>(&event)) {
        if (button->button != MouseButton::Left && button->button != MouseButton::Right
            && button->button != MouseButton::Middle)
            return;
        using lunar::Enum::UserInputType;
        const UserInputType type = button->button == MouseButton::Left    ? UserInputType::MouseButton1
                                   : button->button == MouseButton::Right ? UserInputType::MouseButton2
                                                                          : UserInputType::MouseButton3;
        const lunar::Vector2 at{static_cast<float>(button->x), static_cast<float>(button->y)};
        if (at != mouse)
            input.mouseMove(at, at - mouse, processed);
        mouse = at;
        input.mouseButton(type, button->pressed, processed, std::move(gui));
    } else if (const auto* move = std::get_if<MouseMoveEvent>(&event)) {
        const lunar::Vector2 at{static_cast<float>(move->x), static_cast<float>(move->y)};
        input.mouseMove(at, at - mouse, processed || uiModal, std::move(gui));
        mouse = at; // kept current, so closing the menu doesn't jump by the whole way it moved
    } else if (const auto* motion = std::get_if<RelativeMotionEvent>(&event)) {
        // Pointer locked: the cursor stays put, only the delta moves. Wayland
        // sends these unlocked too (and the cursor's moves already carry them).
        if (pointerLocked && !uiModal)
            input.mouseMove(mouse, {static_cast<float>(motion->rawDx), static_cast<float>(motion->rawDy)}, processed);
    } else if (const auto* scroll = std::get_if<ScrollEvent>(&event)) {
        input.mouseWheel(static_cast<float>(scroll->dy), processed, std::move(gui));
    }
}

lunar::physics::PlayerCamera::Input cameraInput(const InputState& input, bool active, bool pointerLocked, bool wheel,
                                                float aspect, float sensitivity)
{
    lunar::physics::PlayerCamera::Input in;
    if (active && pointerLocked) {
        in.mouseDeltaX = static_cast<float>(input.mouseDelta().x) * sensitivity;
        in.mouseDeltaY = static_cast<float>(input.mouseDelta().y) * sensitivity;
    }
    in.wheel = active && wheel ? static_cast<float>(input.scrollDelta().y) : 0.0f;
    in.rotate = active && input.isDown(MouseButton::Right);
    in.turnLeft = active && input.isDown(Key::Left);
    in.turnRight = active && input.isDown(Key::Right);
    in.zoomIn = active && input.isDown(Key::I);
    in.zoomOut = active && input.isDown(Key::O);
    in.toggleMouseLock = active && (input.wasPressed(Key::LeftShift) || input.wasPressed(Key::RightShift));
    in.aspect = aspect;
    return in;
}

lunar::ui::DevConsole::Kind consoleKind(lunar::MessageType type)
{
    switch (type) {
    case lunar::MessageType::Output:
        return lunar::ui::DevConsole::Kind::Output;
    case lunar::MessageType::Info:
        return lunar::ui::DevConsole::Kind::Info;
    case lunar::MessageType::Warning:
        return lunar::ui::DevConsole::Kind::Warning;
    case lunar::MessageType::Error:
        return lunar::ui::DevConsole::Kind::Error;
    }
    return lunar::ui::DevConsole::Kind::Output;
}
