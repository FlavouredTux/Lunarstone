#pragma once

#include <lunar/ui/Notifications.hpp>

#include <lunar/Classes.hpp>
#include <lunar/StyleEngine.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/ui/ClassicChat.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/GameInvitePrompt.hpp>
#include <lunar/ui/Gui.hpp>
#include <lunar/ui/LoadingScreen.hpp>
#include <lunar/ui/UiRenderer.hpp>
#include <lunar/ui/ViewportFrames.hpp>
#include <lunar/ui/WorldMap.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace lunar::render {
class Context;
}

namespace lunar::ui {

// Everything 2D a game window shows, wired together: the place's
// ScreenGuis (the local player's PlayerGui, else StarterGui), the classic
// chat and the core GUI, drawn by a UiRenderer over the 3D scene, and the
// loading screen over all of it while it's up (loading(); hidden until
// someone shows it).
//
//     auto ui = GameUi::create(context, presenter.format(), Presenter::framesInFlight, game, {...});
//     (*ui)->attach(presenter.renderer());
//     each event:  if ((*ui)->handle(event)) continue; // the GUI used it
//     each frame:  (*ui)->update(dt, framebufferSize, scale); presenter.drawFrame(...);
//
// Chat: chat().post(...) injects a message; setChatSource() connects a
// backend (offline it echoes what the local player types).
//
// Map: map().setMesh(...) hands the minimap and the full-screen map the
// place (the hotkey, M by default, opens the full map unless typing).
class GameUi {
public:
    struct Options {
        std::filesystem::path contentDir;
        ImageProvider images; // ImageLabel images; may be null
        float topInset = Topbar::height;
        bool modernUi = true;
        bool showChat = true;
        bool showCoreGui = true;
        MapSettings map; // Lunarstone's minimap and full-screen map
    };

    static std::expected<std::unique_ptr<GameUi>, std::string>
    create(render::Context& context, VkFormat colorFormat, std::uint32_t slots, DataModel& game, Options options);
    // No GPU: the GUI lays out and takes input (scripts and bots see the same
    // geometry), but nothing is drawn and there is no map. map() isn't usable.
    static std::unique_ptr<GameUi> createHeadless(DataModel& game, Options options);
    bool headless() const { return renderer_ == nullptr; }
    ~GameUi();
    GameUi(const GameUi&) = delete;
    GameUi& operator=(const GameUi&) = delete;

    // Draws over the renderer's frames until destroyed or detached.
    void attach(render::MeshRenderer& renderer);
    void detach();

    // Routes input: a focused text field first, then the menu, the chat and
    // the place's GUI. True when the event shouldn't reach the game.
    bool handle(const platform::Event& event);
    // True while typing (chat or a TextBox): movement keys belong to the GUI.
    bool keyboardCaptured() const;
    // True while a core modal (the menu or an invitation prompt) has input.
    bool menuOpen() const { return core_.menuOpen() || invites_.visible(); }
    // True while the full-screen map is open; like the menu, it takes all input.
    bool mapOpen() const { return map_ && map_->fullOpen(); }
    // True while the loading screen is up (not fading out); it takes all input.
    bool loadingScreenUp() const { return loading_.blocking(); }
    // True while the mouse wheel belongs to the GUI (over the minimap, or the
    // map is open), so the camera shouldn't zoom.
    bool wheelCaptured() const;

    // Where the last update() spent its milliseconds (for LUNAR_PROFILE).
    struct UpdateTimes {
        double styles = 0, core = 0, layout = 0, others = 0, draw = 0, submit = 0;
        std::uint64_t glyphsRasterized = 0; // bitmaps made this update (cache misses)
        GlyphAtlas::RasterStats raster;     // their time, and the slowest one
    };
    const UpdateTimes& lastUpdateTimes() const { return lastTimes_; }
    std::uint64_t glyphsRasterized() const { return atlas_.rasterized(); } // all so far
    // The minimap shows: on in the settings, the core GUI on, and the game
    // hasn't hidden the core HUD (StarterGui:SetCoreGuiEnabled(All, false)).
    bool minimapShown() const;

    // Lays out and hands this frame's drawing to the renderer.
    void update(double dt, Vector2 framebufferSize, float pixelScale = 1.0f);
    // The game's mouse icon (ScriptContext::mouseIcon), drawn over
    // everything at its own size, centred on `at`: the pointer, or the
    // screen's centre while the pointer is locked. Empty draws none; nor
    // does anything while the menu or the full map has the mouse.
    void setCursor(std::string icon, Vector2 at)
    {
        cursorIcon_ = std::move(icon);
        cursorAt_ = at;
    }

    ClassicChat& chat() { return chat_; }
    CoreGui& core() { return core_; }
    LoadingScreen& loading() { return loading_; }
    WorldMap& map() { return *map_; }
    // The ViewportFrames' pictures; null when headless.
    ViewportFrames* viewports() { return viewports_.get(); }
    GuiEngine& gui() { return gui_; }
    const DrawList& drawList() const { return list_; }
    void setChatSource(ChatSource* source) { chat_.setSource(source); }
    void setRtcChatSource(ChatSource* source) { chat_.setSource(ClassicChat::Tab::Rtc, source); }
    // The bounds `text` takes in `font` at `size` pixels, wrapped at `width`
    // (0: one line), as the GUI lays it out: for TextService's measuring.
    Vector2 measureText(std::string_view text, const Font& font, float size, float width, bool richText);
    std::vector<InstancePtr> guiObjectsAt(float x, float y);
    void focusTextBox(TextBox* box, bool capture, bool enterPressed);
    void ensureGuiGeometry(Instance* object);
    GameInvitePrompt& gameInvites() { return invites_; }
    std::function<bool(std::shared_ptr<GameInviteSession>)> gameInviteHandler();

    std::function<void()> onLeave; // the menu's Leave Game, or the loading screen's leave button

private:
    UpdateTimes lastTimes_;
    GameUi(DataModel& game, Options options);
    void refreshRoots();

    DataModel* game_;
    Options options_;
    std::vector<Instance*> roots_; // the GUI's, as refreshRoots found them
    StyleEngine styles_;           // the StyleSheets linked into them
    FontLibrary fonts_;
    GlyphAtlas atlas_;
    GuiEngine gui_;
    ClassicChat chat_;
    CoreGui core_;
    LoadingScreen loading_;
    Notifications notifications_;
    GameInvitePrompt invites_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::unique_ptr<UiRenderer> renderer_;
    std::unique_ptr<WorldMap> map_;
    std::unique_ptr<ViewportFrames> viewports_; // after the renderer, whose prepass it is
    render::MeshRenderer* attached_ = nullptr;
    std::unique_ptr<LocalEchoChatSource> echo_;
    DrawList list_;
    double time_ = 0.0;
    Vector2 framebufferSize_{1280, 720};
    bool ensuringGeometry_ = false;
    std::string cursorIcon_;
    Vector2 cursorAt_;
};

} // namespace lunar::ui
