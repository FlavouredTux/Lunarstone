#pragma once

#include <lunar/Classes.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/ui/GameUi.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <set>
#include <string>

namespace lunar {
class ScriptContext;
class ClientSession;
} // namespace lunar

// The 2D layer of an offscreen picture of a running game (--render of a
// joined or replayed server): what --play draws over the 3D view, so the
// local player's PlayerGui, the classic chat and the core GUI, laid out for
// the picture's size. Lunarstone's own minimap stays off; the real client
// has none.
class OffscreenUi {
public:
    // gameOverlay: Lunarstone's chat and core GUI over the game's, under the
    // top bar's inset; off for the Lua app, which draws all of its own.
    static std::expected<std::unique_ptr<OffscreenUi>, std::string> create(lunar::render::Context& context,
                                                                           lunar::render::OffscreenRenderer& offscreen,
                                                                           lunar::DataModel& game,
                                                                           bool gameOverlay = true);
    ~OffscreenUi();
    OffscreenUi(const OffscreenUi&) = delete;
    OffscreenUi& operator=(const OffscreenUi&) = delete;

    // Lays the GUI out and hands it to the renderer for the next render(),
    // waiting for the images it shows first. Images that can't be loaded
    // are reported once each. dt is the frame's time in seconds, which the
    // GUI's own animations move by; 0 leaves them where they are, what a
    // single picture of the game wants.
    void update(double dt = 0.0);
    std::size_t quads() const { return ui_->drawList().quads.size(); }
    // The GUI itself, for input a playtest gives it (GameUi::handle).
    lunar::ui::GameUi& ui() { return *ui_; }
    // Off: images still loading are left out and come in when they're there,
    // as a window draws them, without update() waiting for them (a frame
    // timed by a benchmark).
    void setWaitForImages(bool wait) { waitForImages_ = wait; }
    void bind(lunar::ScriptContext& context);
    void bind(lunar::ClientSession& session);

private:
    OffscreenUi() = default;

    lunar::render::OffscreenRenderer* offscreen_ = nullptr;
    std::shared_ptr<lunar::assets::TextureLoader> textures_;
    std::unique_ptr<lunar::ui::GameUi> ui_;
    std::set<std::string, std::less<>> asked_;    // every image the GUI showed
    std::set<std::string, std::less<>> reported_; // the ones that failed, told about
    bool loading_ = false;                        // this layout asked for one that isn't there yet
    bool waitForImages_ = true;
    float scale_ = 1.0f; // pixels per GUI pixel (LUNAR_UI_SCALE)
};
