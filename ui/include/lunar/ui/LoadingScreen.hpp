#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/Gui.hpp>

#include <functional>
#include <optional>
#include <string>

namespace lunar::ui {

// The screen over a game while it joins and loads: the experience's icon,
// its name and creator in the middle, a spinner and what's happening at the
// bottom, and a button in the top right corner that leaves. It covers
// everything the window draws and takes all input until it's dismissed,
// then fades out. Lunarstone's own look (sizes and colours are ours); it
// stands where the client's default loading screen does, so a game's
// ReplicatedFirst loader takes it down the same way (remove()).
class LoadingScreen {
public:
    static constexpr Color3 background = Color3::fromRGB(25, 27, 31);
    static constexpr Color3 placeholder = Color3::fromRGB(46, 49, 55); // where the icon goes until it loads
    static constexpr Color3 titleColor = Color3::fromRGB(255, 255, 255);
    static constexpr Color3 detailColor = Color3::fromRGB(170, 174, 181);
    static constexpr Color3 errorColor = Color3::fromRGB(255, 110, 100);
    static constexpr float iconSize = 160.0f;
    static constexpr float fadeSeconds = 0.4f;

    LoadingScreen(FontLibrary& fonts, GlyphAtlas& atlas);

    // The icon loads through this.
    void setImageProvider(ImageProvider images) { images_ = std::move(images); }

    // Up again, fully opaque, with nothing about the game yet.
    void show();
    // What's known about the game: its name, who made it ("By ..." under the
    // name) and the icon's content id. Empty strings leave those out.
    void setGame(std::string name, std::string creator, std::string icon);
    // The line under the spinner ("Joining server", ...); `error` draws it
    // in red and stops the spinner.
    void setStatus(std::string status, bool error = false);
    // Fades out from now on.
    void dismiss();
    // Gone at once, no fade: ReplicatedFirst:RemoveDefaultLoadingScreen().
    void remove();

    // Drawn at all (up, or still fading out).
    bool visible() const;
    // Up and not fading: it has the keyboard and the mouse.
    bool blocking() const { return shown_ && !dismissedAt_; }

    // The leave button's clicks; every other event while it's blocking is
    // used up too, so nothing reaches the game under it. True when used.
    bool handle(const platform::Event& event);
    void update(double time, Vector2 screen, float pixelScale);
    void draw(DrawList& list);

    // Where the leave button was laid out this frame.
    RectF leaveRect() const { return leave_; }
    // How opaque it is this frame, 0 to 1.
    float opacity() const;

    std::function<void()> onLeave;

private:
    void textAt(DrawList& list, std::string_view text, RectF box, float size, Enum::Font font, Color3 color,
                float alpha, Enum::TextYAlignment y = Enum::TextYAlignment::Center);

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    bool shown_ = false;
    std::optional<double> dismissedAt_;
    double time_ = 0.0;
    Vector2 screen_;
    float scale_ = 1.0f;
    std::string name_, creator_, icon_, status_;
    bool error_ = false;
    RectF leave_{};
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
    bool pressedLeave_ = false;
};

} // namespace lunar::ui
