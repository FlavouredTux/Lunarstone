#pragma once

#include "Control.hpp"
#include "Settings.hpp"

#include <lunar/Classes.hpp>
#include <lunar/physics/PlayerCamera.hpp>
#include <lunar/platform/Window.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Presenter.hpp>
#include <lunar/script/ScriptContext.hpp>
#include <lunar/ui/GameUi.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {
class ClientInput;
class ClientSession;
} // namespace lunar

class RtcChatSource;

// A picture of the window saved `at` seconds after it opened (--screenshot).
struct Screenshot {
    double at = 0.0;
    std::filesystem::path out;
};
// "seconds:out.png", as --screenshot takes it.
std::optional<Screenshot> parseScreenshot(std::string_view text);
// LUNAR_HEALTH_BARS set (and not "0"): the core GUI's health bars beside
// the other players start on, in a window and in an offscreen picture or
// benchmark alike.
bool healthBarsFromEnvironment();

// The window a game is played in (--play, --net-play): a Wayland window,
// Vulkan presenting to it at the graphics settings asked for, and the game's
// 2D layer (GameUi: the place's GUI, the chat, the core GUI and Lunarstone's
// map) drawn over the scene. Ctrl+C/X/V in the chat and TextBoxes go
// through the window's clipboard while it's open.
//
//     each frame:  dt = window.beginFrame(onEvent);   // the events the GUI didn't use
//                  ... simulate, ui().update(...), presenter().drawFrame(...)
//                  if (auto fps = window.endFrame()) ...  // once a second
class GameWindow {
public:
    struct Options {
        std::string title = "Lunarstone";
        lunar::render::GraphicsSettings graphics;
        bool vsync = true;          // false: as many frames as it can draw (it may tear)
        int fpsCap = 0;             // frames a second at most; 0 for no cap
        lunar::ui::MapSettings map; // Lunarstone's minimap and full-screen map
        bool shiftLock = false;     // the Shift Lock Switch to start with (--shift-lock)
        // Verification: pictures of the window as it's drawn, and closing it
        // after this long (0: when the user does).
        std::vector<Screenshot> screenshots;
        double seconds = 0.0;
        // --screenshot-on: a file whose appearing asks for a picture of the
        // next frame, saved where the file says (one path, a line); the
        // file goes once it's read. For looking at a running game.
        std::filesystem::path screenshotRequest;
        // --control: input from this file instead of the keyboard and mouse
        // (ControlFile), on top of theirs.
        std::filesystem::path control;
        // --bot: a console script plays the window (keypress, mousemoverel):
        // it counts as focused and its pointer as locked, as with --control.
        bool automated = false;
        // Lunarstone's chat and core GUI (with its top bar's inset) over the
        // game's GUI; off for the Lua app, which draws all of its own.
        bool gameOverlay = true;
        // The player's settings (Settings.hpp): the window starts from them
        // and keeps the menu's changes there. Empty: neither. With
        // graphicsFromFlags the graphics above win for this window (the
        // command line asked for them); a choice made in the menu is still kept.
        std::filesystem::path settingsFile;
        bool graphicsFromFlags = false;
    };

    static std::expected<std::unique_ptr<GameWindow>, std::string> create(lunar::DataModel& game,
                                                                          const Options& options);
    ~GameWindow();
    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    // A teleport: the GUI over another game (the window, the renderer and
    // the settings stay). Waits for the GPU first.
    std::expected<void, std::string> rebind(lunar::DataModel& game);
    // The online account's username, once the joined player's replica is ready.
    void setRtcRobloxUsername(std::string_view username);
    // Attach the actual font metrics, image readiness, and GUI hit testing
    // before a client's scripts start. The hooks follow this window's UI on teleports.
    void bindGui(lunar::ScriptContext& context);
    void bindGui(lunar::ClientSession& session);

    // The frame cap as it stands (0 for none), and a new one: each frame then
    // starts no sooner than this after the one before, from the next frame on.
    int frameCap() const { return options_.fpsCap; }
    // Nothing of the window is on screen: frames aren't drawn, and the game
    // runs at hiddenFps (presenting would block until it's shown again).
    bool hidden() const { return window_.suspended(); }
    static constexpr int hiddenFps = 30;
    void setFrameCap(int fps);

    lunar::platform::Window& window() { return window_; }
    lunar::render::Context& context() { return *context_; }
    lunar::render::Presenter& presenter() { return *presenter_; }
    lunar::ui::GameUi& ui() { return *ui_; }

    // The start of a frame: waits for its turn under the fps cap, then takes
    // the compositor's events. Resizes go to the presenter and F11 toggles
    // full screen; the rest go to the GUI first and to `game` when the GUI
    // didn't use them. Returns the seconds since the last frame, at most 0.1.
    float beginFrame(const std::function<void(const lunar::platform::Event&)>& game = {},
                     const std::function<void(const lunar::platform::Event&, bool)>& observe = {});
    // The GUI has the keyboard: typing in the chat or a TextBox, or the menu,
    // the full map or the loading screen is up. Movement keys and the camera
    // leave it alone.
    bool uiHasKeys() const;
    // The menu's settings: the graphics as they are now (a preset picked in
    // the menu replaces the window's; onGraphics hears it, for what the
    // session itself holds: terrain distance, particle density), how much
    // the mouse turns the camera, and whether Shift may lock the mouse.
    const lunar::render::GraphicsSettings& graphics() const { return graphics_; }
    std::function<void(const lunar::render::GraphicsSettings&)> onGraphics;
    float cameraSensitivity() const { return sensitivity_; }
    float volume() const { return volume_; } // the menu's, 0 to 1
    bool shiftLock() const { return shiftLock_; }
    // Played from a --control file or a --bot script: the pointer counts as
    // locked for the camera.
    bool controlled() const { return control_ != nullptr || automated_; }

    // The window's size in pixels, and what the GUI lays out for it.
    lunar::Vector2 size() const;
    // The same in logical pixels, what the GUI's AbsoluteSize, the mouse and
    // Camera.ViewportSize are measured in (points, on the Mac client).
    lunar::Vector2 logicalSize() const;
    float aspect() const;
    // Lays the GUI out for this frame.
    void updateUi(float dt);
    // The end of a frame, after drawing it: saves a screenshot that was due.
    // Once a second, the frames drawn in that second.
    std::optional<int> endFrame();
    // Seconds since the window opened.
    double elapsed() const;

private:
    GameWindow(lunar::platform::Window window, Options options);
    void wireMenu();
    std::expected<void, std::string> makeUi(lunar::DataModel& game);

    lunar::platform::Window window_;
    Options options_;
    std::unique_ptr<lunar::render::Context> context_;
    std::unique_ptr<lunar::render::Presenter> presenter_;
    std::unique_ptr<RtcChatSource> rtc_;
    std::unique_ptr<lunar::ui::GameUi> ui_;
    lunar::render::GraphicsSettings graphics_;
    float sensitivity_ = 1.0f;
    float volume_ = 1.0f;
    bool shiftLock_ = false;
    bool automated_ = false;
    using Clock = std::chrono::steady_clock;
    Clock::time_point opened_ = Clock::now();
    Clock::time_point last_ = opened_;
    Clock::time_point second_ = opened_;
    Clock::time_point nextFrame_ = opened_;
    Clock::time_point nextHiddenFrame_ = opened_;
    int frames_ = 0;
    std::size_t screenshot_ = 0; // the next one due
    bool capturing_ = false;
    std::optional<std::filesystem::path> requested_; // a --screenshot-on picture being taken
    std::unique_ptr<ControlFile> control_;
    Clock::time_point requestCheck_ = opened_;
    // Kept settings (Options::settingsFile): what's on file plus the menu's
    // changes, written a second at most after one, and when the window closes.
    PlayerSettings saved_;
    bool settingsDirty_ = false;
    Clock::time_point savedAt_ = opened_;
    std::array<bool, 3> overlaysAtStart_{}; // performance stats, health bars, beat ring
    void keepSettings();
    void saveSettings();
};

// The platform's key as Roblox's Enum.KeyCode; the names mostly match.
std::optional<lunar::Enum::KeyCode> robloxKeyCode(lunar::platform::Key key);
// And back: the platform's key for a KeyCode, nullopt for one it has none of.
std::optional<lunar::platform::Key> platformKey(lunar::Enum::KeyCode code);

// Platform input for scripts. GUI-consumed events still reach UserInputService
// with gameProcessedEvent, while game actions and tools ignore them. `mouse`
// tracks the window position; locked pointers supply relative motion.
void forwardInput(lunar::ClientInput& input, const lunar::platform::Event& event, lunar::Vector2& mouse,
                  bool pointerLocked, bool uiModal, bool processed = false,
                  lunar::ui::GuiEngine::InputDispatch gui = {});

// The CameraModule's input this frame, from the keyboard and mouse: none
// while the GUI has them (`active` false), the wheel only when it's the
// camera's. The mouse turns the camera while the pointer is locked, times
// the menu's camera sensitivity.
lunar::physics::PlayerCamera::Input cameraInput(const lunar::platform::InputState& input, bool active,
                                                bool pointerLocked, bool wheel, float aspect, float sensitivity = 1.0f);

// How a script's output shows in the menu's console (its Luau tab).
lunar::ui::DevConsole::Kind consoleKind(lunar::MessageType type);
