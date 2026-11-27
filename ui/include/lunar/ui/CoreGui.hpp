#pragma once

#include <lunar/Classes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/BackpackGui.hpp>
#include <lunar/ui/DevConsole.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/GameMenu.hpp>
#include <lunar/ui/HealthBars.hpp>
#include <lunar/ui/Topbar.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lunar::ui {

// The health bar colour of the classic top bar (Topbar.lua's
// HealthbarColorTransferFunction): red below 10%, green above 80%, and
// Shepard (inverse distance squared) interpolation between red at 0.1,
// yellow at 0.5 and green at 0.8 in between.
Color3 healthColor(float fraction);

// The native core GUI of the classic client: the health bar in the top
// right, the player list (Tab), the backpack hotbar and the Escape menu
// (GameMenu, a panel sliding in from the right). Layout follows the
// 2016-2020 CoreScripts where remembered; sizes marked "guess" are not.
// Lunarstone adds the health bars beside the other players (HealthBars,
// Settings > Health Bars or F7, off by default).
//
// The menu reaches the app through the callbacks below and menuSettings:
// a setting whose callback is null shows greyed out.
class CoreGui {
public:
    static constexpr Color3 healthRed = Color3::fromRGB(255, 28, 0);
    static constexpr Color3 healthYellow = Color3::fromRGB(250, 235, 0);
    static constexpr Color3 healthGreen = Color3::fromRGB(27, 252, 107);
    static constexpr Color3 healthBackground = Color3::fromRGB(228, 236, 246);
    // Shows and hides the health bars beside the other players
    // (MenuSettings::healthBars). The client's own F-keys are F9 and F11,
    // and games rarely bind any.
    static constexpr platform::Key healthBarsKey = platform::Key::F7;

    CoreGui(FontLibrary& fonts, GlyphAtlas& atlas);
    CoreGui(const CoreGui&) = delete; // the menu points back at it
    CoreGui& operator=(const CoreGui&) = delete;

    // The game whose players and local humanoid to show; null for none.
    void setGame(DataModel* game);
    // Tool icons (TextureId) load through this.
    void setImageProvider(ImageProvider images)
    {
        menu_.setImageProvider(images);
        backpack_.setImageProvider(std::move(images));
    }
    // The experience's name, for the menu's header.
    void setTitle(std::string title) { menu_.setTitle(std::move(title)); }
    // The full map's key, for the menu's Help tab; none without a map.
    void setMapKey(std::optional<platform::Key> key) { menu_.setMapKey(key); }
    // The performance overlay's numbers (Settings > Performance Stats).
    void setPerformanceStats(const PerformanceStats& stats) { stats_ = stats; }

    // Tab, Escape, F7 and everything while the menu is open; true when used.
    bool handle(const platform::Event& event);
    void update(double time, Vector2 screen, float topInset, float pixelScale);
    void draw(DrawList& list);
    // What it puts in the world, drawn before the place's GUI as the game's
    // own BillboardGuis are: the health bars beside the other players.
    void drawWorld(DrawList& list);

    // The menu takes input; it slides out a moment longer (menuVisible).
    bool menuOpen() const { return menu_.open(); }
    bool menuVisible() const { return menu_.visible(); }
    // At once, without the slide.
    void setMenuOpen(bool open) { menu_.setOpen(open); }
    GameMenu& menu() { return menu_; }
    bool playerListOpen() const { return playerListOpen_; }
    bool coreGuiEnabled(Enum::CoreGuiType type) const;
    bool resetEnabled() const;
    void setModern(bool modern) { modern_ = modern; }
    Topbar& topbar() { return topbar_; }
    const Topbar& topbar() const { return topbar_; }
    // The pointer is over the player list, whose rows the wheel scrolls.
    bool pointerOverPlayerList() const;
    BackpackGui& backpack() { return backpack_; }
    // Settings > Health Bars: a bar beside every other player's character.
    const HealthBars& healthBars() const { return healthBars_; }
    // The menu's Luau tab: its log and how it runs code.
    DevConsole& console() { return console_; }

    std::function<void()> onLeave; // Leave, confirmed
    std::function<void()> onReset; // Reset Character, confirmed
    std::function<void()> onToggleChat;
    // The Settings tab's choices; menuSettings holds what's current.
    std::function<void(render::GraphicsPreset)> onGraphicsQuality;
    std::function<void(bool)> onFullscreen;
    std::function<void(float)> onCameraSensitivity;
    std::function<void(float)> onVolume;
    std::function<void(bool)> onShiftLock;
    std::function<void(bool)> onRayTracing; // the choice for the next game
    MenuSettings menuSettings;

private:
    void drawStats(DrawList& list);
    void textAt(DrawList& list, std::string_view text, RectF box, float size, Enum::Font font, Color4 color,
                Enum::TextXAlignment x, Enum::TextYAlignment y, std::optional<RectF> clip = std::nullopt);

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    std::weak_ptr<Instance> game_;
    bool playerListOpen_ = true; // shown by default in the classic client, Tab hides it
    bool lastConsoleVisible_ = false;
    Vector2 screen_;
    float top_ = 36.0f;
    float scale_ = 1.0f;
    float mouseX_ = 0.0f, mouseY_ = 0.0f;
    BackpackGui backpack_;
    DevConsole console_;
    GameMenu menu_;
    HealthBars healthBars_;
    Topbar topbar_;
    bool modern_ = false;
    std::optional<PerformanceStats> stats_;
    RectF playerList_{};         // where the list was drawn last (empty when hidden)
    float playerListScroll_ = 0; // pixels scrolled down past its first row
};

} // namespace lunar::ui
