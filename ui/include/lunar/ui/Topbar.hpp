#pragma once

#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>

#include <functional>

namespace lunar::ui {

// Native desktop controls above the game's safe area. Game-provided TopbarPlus
// buttons use safeArea(), which begins after the controls actually shown.
class Topbar {
public:
    static constexpr float height = 58.0f;
    enum class Button : std::uint8_t { Roblox, Controls, Chat, PlayerList, Settings };

    Topbar(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}
    void update(Vector2 screen, float scale);
    bool handle(const platform::Event& event);
    void draw(DrawList& list);
    RectF buttonRect(Button button) const;
    Rect safeArea() const;
    bool controlsOpen() const { return open_; }
    bool pointerOver() const;

    bool enabled = true;
    bool chatEnabled = true;
    bool chatOpen = true;
    bool playerListEnabled = true;
    bool playerListOpen = true;
    std::function<void()> onMenu;
    std::function<void()> onChat;
    std::function<void()> onPlayerList;
    std::function<void()> onSettings;

private:
    void label(DrawList& list, std::string_view text, RectF rect);
    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    Vector2 screen_;
    float scale_ = 1.0f;
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
    bool open_ = false;
    bool pressed_ = false;
};

} // namespace lunar::ui
