#pragma once

#include <lunar/Classes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/render/MapMesh.hpp>
#include <lunar/render/MapRenderer.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lunar::render {
class Context;
}

namespace lunar::ui {

class UiRenderer;

// How the map behaves; the client's settings (and later the escape menu)
// change these.
struct MapSettings {
    enum class Corner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

    bool minimap = true;
    Corner corner = Corner::BottomRight;
    float minimapSize = 190.0f; // across, logical pixels
    // Studs across the minimap; the wheel over it zooms between
    // minMinimapSpan and the whole place.
    float minimapSpan = 400.0f;
    // Opens and closes the full-screen map; none for no key. Ignored while
    // typing in the chat or a TextBox.
    std::optional<platform::Key> fullMapKey = platform::Key::M;
    // Other players' markers. Only for local sessions (Play Solo, our own
    // loopback server): on a real Roblox server the map would show where
    // everyone is through walls and across the place, which the real client
    // never does and which would make Lunarstone a cheat. Off by default so
    // a session has to say it's local; the local player's own marker always
    // shows.
    bool showOtherPlayers = false;
};

// Lunarstone's own map of the place (the real client has none): a small
// minimap in a corner of the HUD and a full-screen map, both drawing the
// whole place in a flat-coloured 3D look (render::MapMesh, drawn by a
// render::MapRenderer into offscreen images) with players' markers drawn
// crisply on top by the UI every frame. Purely client side: it reads the
// game (players, characters, the camera) and never changes it.
//
// The minimap looks straight down, north (-Z) up before it turns with the
// camera, centred on the local character. Its picture is redrawn only
// minimapRate times a second (a little bigger than shown); in between the
// UI slides and turns it, so it still follows smoothly.
//
// The full map has two modes: 3D, a free orbit around the place from high
// above down to street level (drag to turn, right-drag or middle-drag to
// pan, the wheel to zoom), and 2D, straight down (drag to pan). C (or the
// Center button) goes back to your character. Pointing at a part outlines
// it and tells what the client knows of it (name, distance, size,
// CanCollide). It never acts on the game: no teleporting, no clicking
// through. It draws into any rectangle (drawFull, handleFull), so a menu can
// embed it as a panel; GameUi shows it full screen.
class WorldMap {
public:
    static constexpr float minimapRate = 12.0f;    // pictures a second
    static constexpr float minMinimapSpan = 80.0f; // studs
    // The minimap's picture covers this much more than it shows, so it can
    // slide between pictures.
    static constexpr float minimapMargin = 1.3f;

    static std::expected<std::unique_ptr<WorldMap>, std::string> create(render::Context& context, std::uint32_t slots,
                                                                        FontLibrary& fonts, GlyphAtlas& atlas);
    WorldMap(const WorldMap&) = delete;
    WorldMap& operator=(const WorldMap&) = delete;
    ~WorldMap();

    // The game whose players and camera to show; null for none.
    void setGame(DataModel* game);
    // The place (static) and its moving parts (boxes), as they're rebuilt.
    void setMesh(std::shared_ptr<const render::MapMesh> mesh);
    void setMoving(std::shared_ptr<const render::MapMesh> mesh);
    // The place's name, for the full map's title.
    void setTitle(std::string title) { title_ = std::move(title); }

    MapSettings& settings() { return settings_; }
    const MapSettings& settings() const { return settings_; }

    // Draws the map's pictures in the renderer's prepass and shows them.
    void attach(UiRenderer& renderer);
    void detach(UiRenderer& renderer);

    // Once a frame before drawing: where things are, which pictures to
    // redraw. screen in framebuffer pixels.
    void update(double time, Vector2 screen, float pixelScale);

    // The minimap (if on, not hidden and there is a map), in its corner.
    void drawMinimap(DrawList& list);
    RectF minimapRect() const;
    // The wheel over the minimap zooms it; true when used.
    bool handleMinimap(const platform::Event& event);
    // Hidden for now whatever the settings say: the game turned the core
    // HUD off, or a menu covers it. Its picture isn't redrawn meanwhile.
    void setMinimapHidden(bool hidden) { minimapHidden_ = hidden; }
    bool minimapShown() const { return settings_.minimap && !minimapHidden_ && !fullOpen_ && mesh_; }
    bool pointerOverMinimap() const;

    bool fullOpen() const { return fullOpen_; }
    void setFullOpen(bool open);
    // The hotkey (a press of settings().fullMapKey) toggles the full map;
    // true when used. Call only when the keyboard isn't typing.
    bool handleHotkey(const platform::Event& event);
    // The full map in `area` (framebuffer pixels). handleFull takes every
    // event while open (it's modal): Escape and the hotkey close it.
    void drawFull(DrawList& list, RectF area);
    bool handleFull(const platform::Event& event, RectF area);
    // The whole screen as of the last update().
    RectF fullArea() const { return {0, 0, screen_.x, screen_.y}; }

    render::MapRenderer& renderer() { return *renderer_; }

    // Where the full map looks: at target from distance studs away, turned
    // yaw radians about Y (0 looks north) and tilted pitch radians down. In
    // 2D, straight down at target, span studs from top to bottom.
    struct Orbit {
        Vector3 target;
        float yaw = 0.0f;
        float pitch = 0.9f;
        float distance = 500.0f;
        float span = 1000.0f;
    };
    Orbit& orbit() { return orbit_; }
    enum class Mode : std::uint8_t { TwoD, ThreeD };
    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    // Points at the part under a spot of the full map (framebuffer pixels),
    // as moving the mouse there does.
    void pointAt(float x, float y);
    // Frames the whole place (and the full map's next open keeps it).
    void frameAll();
    // Centres the full map on the local character.
    void findSelf();

private:
    struct Marker {
        Vector3 position;
        Vector3 look; // facing, along the ground
        std::string name;
        Color3 color;
        bool self = false;
    };
    struct Button {
        RectF rect;
        std::string label;
        bool active = false;
        void (WorldMap::*action)() = nullptr;
    };
    // The part under the pointer: what it is and its box.
    struct Hover {
        std::weak_ptr<const Instance> part;
        render::Mat4 box;   // the unit cube to world
        bool filled = true; // a see-through box as well as the outline (not for huge parts)
    };

    WorldMap(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}
    void gatherMarkers();
    // Yaw of the game camera (radians clockwise from north, as seen from above).
    float cameraHeading() const;
    std::optional<Vector3> selfPosition() const;
    render::Mat4 fullViewProjection(RectF area) const;
    Vector3 fullEye() const;
    void layoutButtons(RectF area);
    void hoverAt(RectF area);
    void close() { setFullOpen(false); }
    void twoD() { setMode(Mode::TwoD); }
    void threeD() { setMode(Mode::ThreeD); }
    void text(DrawList& list, std::string_view text, RectF box, float size, Color4 color, Enum::TextXAlignment x,
              Enum::TextYAlignment y, const RectF& clip, Enum::Font font = Enum::Font::Code);
    void arrow(DrawList& list, float x, float y, float size, float angle, Color4 color, const RectF& clip);
    void dot(DrawList& list, float x, float y, float radius, Color4 color, const RectF& clip) const;

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    std::unique_ptr<render::MapRenderer> renderer_;
    std::weak_ptr<Instance> game_;
    MapSettings settings_;
    bool minimapHidden_ = false;
    std::shared_ptr<const render::MapMesh> mesh_;
    std::shared_ptr<const render::MapMesh> moving_;
    std::string title_;
    std::vector<Marker> markers_;
    // Placeholders that stand for the map's two GPU pictures in DrawLists
    // (UiRenderer::setExternalImage) and the arrow sprite.
    std::shared_ptr<const lunar::Image> minimapImage_;
    std::shared_ptr<const lunar::Image> fullImage_;
    std::shared_ptr<const lunar::Image> arrowImage_;

    double time_ = 0.0;
    Vector2 screen_;
    float scale_ = 1.0f;
    float heading_ = 0.0f; // the camera's, radians clockwise from north

    // The minimap's last picture: around this point, this many studs across.
    Vector3 minimapCenter_;
    float minimapPictureSpan_ = 0.0f;
    float minimapPictureSize_ = 0.0f;
    double minimapDrawnAt_ = -1.0;

    bool fullOpen_ = false;
    bool framed_ = false;
    Mode mode_ = Mode::ThreeD;
    Orbit orbit_;
    std::vector<Button> buttons_;
    std::optional<Hover> hover_;
    bool hoverDirty_ = false;
    float pressX_ = 0.0f, pressY_ = 0.0f;
    render::Mat4 fullDrawn_; // the view-projection of the full map's last picture
    double fullDrawnAt_ = -1.0;
    enum class Drag : std::uint8_t { None, Turn, Pan };
    Drag drag_ = Drag::None;
    float mouseX_ = -1.0f, mouseY_ = -1.0f; // off screen until the pointer moves
};

} // namespace lunar::ui
