#pragma once

#include <lunar/Classes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/TextEditor.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::ui {

// What GuiObjects load images through (ImageLabel.Image, ...). Returns null
// while loading; the same id keeps giving the same image.
using ImageProvider = std::function<std::shared_ptr<const Image>(std::string_view contentId)>;

// What a ViewportFrame on screen shows, drawn at this many pixels: an image
// for draw() to put on its rect (colour already times its alpha), or null
// while there's nothing to show.
using ViewportProvider
    = std::function<std::shared_ptr<const Image>(ViewportFrame& frame, std::uint32_t width, std::uint32_t height)>;

// Resolves a UDim against a parent length: scale * length + offset.
constexpr float resolve(UDim u, float length)
{
    return u.scale * length + static_cast<float>(u.offset);
}

// Size and position of a GuiObject within its parent's content rect, the
// core of Roblox's 2D layout: AbsoluteSize = Size resolved against the
// parent (SizeConstraint picks which parent axis each scale uses),
// AbsolutePosition = parent + Position resolved - AnchorPoint * size.
struct Placement {
    RectF rect;
};
Vector2 resolveSize(UDim2 size, Vector2 parent, Enum::SizeConstraint constraint = Enum::SizeConstraint::RelativeXY);
Placement place(UDim2 position, UDim2 size, Vector2 anchor, const RectF& parent,
                Enum::SizeConstraint constraint = Enum::SizeConstraint::RelativeXY);
// UIAspectRatioConstraint applied to a size.
Vector2 applyAspectRatio(Vector2 size, float ratio, Enum::AspectType type, Enum::DominantAxis axis);
std::optional<Enum::KeyCode> keyCodeFor(platform::Key key);

// The engine's 2D GUI: lays out ScreenGuis (Roblox's rules for UDim2,
// AnchorPoint, UIPadding, UIListLayout, UIGridLayout, the size constraints,
// UIScale, AutomaticSize, ScrollingFrame canvases, ZIndexBehavior and
// ClipsDescendants), writes AbsolutePosition/AbsoluteSize/TextBounds back,
// draws into a DrawList and drives the GUI events from platform input.
class GuiEngine {
public:
    struct Settings {
        // GuiService:GetGuiInset().Y: ScreenGuis without IgnoreGuiInset start
        // below the top bar. 36 is the classic (pre-2023) top bar this
        // client imitates; the 0.742 client's new top bar is 58.
        float topInset = 36.0f;
        float pixelScale = 1.0f;            // framebuffer pixels per GUI pixel
        std::optional<Rect> topbarSafeArea; // logical coordinates, from the native controls
    };

    GuiEngine(FontLibrary& fonts, GlyphAtlas& atlas);

    void setImageProvider(ImageProvider provider) { images_ = std::move(provider); }
    void setViewportProvider(ViewportProvider provider) { viewports_ = std::move(provider); }
    Settings& settings() { return settings_; }

    // ScreenGuis are taken from these containers (PlayerGui, StarterGui,
    // CoreGui), in order; DisplayOrder sorts across all of them. Cheap to
    // call again with the same ones.
    void setRoots(const std::vector<Instance*>& roots);

    // BillboardGuis and SurfaceGuis come from this Workspace (and the roots,
    // when they have an Adornee). Billboards face its CurrentCamera; both are
    // drawn into the world (DrawList::layers) under the ScreenGuis: surfaces,
    // then billboards farthest first. Null: none.
    void setWorld(Workspace* workspace);

    // Where a BillboardGui shows on a screen of this many framebuffer
    // pixels through the camera, and how far ahead of it (studs along the
    // view), or nothing when it doesn't show: disabled, hidden from this
    // player, past MaxDistance, behind the camera or without an adornee.
    struct BillboardPlacement {
        RectF rect;
        float depth = 0.0f;
        Vector3 point; // where in the world it hangs (the card's depth)
    };
    static std::optional<BillboardPlacement> placeBillboard(const BillboardGui& gui, const Camera& camera,
                                                            Vector2 screen, float pixelScale = 1.0f,
                                                            const Player* localPlayer = nullptr);

    // Where a SurfaceGui's canvas lies on its part's face: canvas pixel (x, y)
    // is at origin + x * axisX + y * axisY, its front facing `normal`; or
    // nothing when it's disabled or has no part.
    struct SurfacePlacement {
        Vector3 origin, axisX, axisY, normal;
        Vector2 canvas; // canvas size in pixels
    };
    static std::optional<SurfacePlacement> placeSurface(const SurfaceGui& gui);

    // What the pointer's ray meets first in the world: the part (or the
    // Terrain) and how far along. Something nearer than a surface GUI's part
    // keeps the mouse off it. Unset: nothing in the world is in the way.
    struct WorldHit {
        const Instance* part = nullptr;
        float distance = 0.0f;
    };
    using WorldPick = std::function<std::optional<WorldHit>(Vector3 origin, Vector3 direction)>;
    void setWorldPick(WorldPick pick) { pick_ = std::move(pick); }

    // Lays everything out for a screen of this many framebuffer pixels.
    void update(Vector2 screen, double time, bool measureHidden = false);
    bool geometryQueryNeedsUpdate() const;
    bool layoutInProgress() const { return updating_; }
    void ensureGeometry(Vector2 screen, double time);
    void draw(DrawList& list) const;
    bool imageLoaded(std::string_view contentId) const
    {
        const auto image = images_ && !contentId.empty() ? images_(contentId) : nullptr;
        return image && !image->empty();
    }

    // Input in window (logical) coordinates. Each returns whether the GUI
    // used it, so the game shouldn't (camera, character, hotkeys).
    bool handle(const platform::Event& event);
    // Whether the wheel at the pointer scrolls something in the GUI (the camera
    // then doesn't zoom by it, as in the client).
    bool wheelTaken() const;
    using InputDispatch = std::function<void(const InstancePtr&)>;
    void setDeferredInput(bool deferred) { deferredInput_ = deferred; }
    void clearInputDispatch() { inputDispatch_ = {}; }
    InputDispatch takeInputDispatch() { return std::exchange(inputDispatch_, {}); }
    void finishInputEvent(const platform::Event& event);
    std::function<void(UIPageLayout&, int)> onPageStep;
    bool textBoxFocused() const { return focused_ != nullptr; }
    void releaseFocus(bool enterPressed);
    void focus(TextBox* box);
    // The topmost GuiObject at a point that takes input (GuiButtons,
    // TextBoxes, Active objects), or null.
    GuiObject* objectAt(float x, float y) const;
    // Visible screen GUI objects at a logical window point, front to back.
    std::vector<InstancePtr> objectsAt(float x, float y) const;
    // Where an object was laid out this frame (framebuffer pixels, before
    // rotation), if it's visible.
    std::optional<RectF> rectOf(const GuiObject* object) const;

private:
    struct Node {
        GuiObject* object = nullptr;
        ScrollingFrame* scrollbarOf = nullptr;                // a scroll bar drawn after the frame's children
        RectF rect;                                           // before rotation
        RectF clip;                                           // in screen space
        float cosA = 1.0f, sinA = 0.0f, tx = 0.0f, ty = 0.0f; // node to screen: p' = R p + t
        float scale = 1.0f;                                   // accumulated UIScale (and pixelScale)
        // CanvasGroups it's in: their GroupColor3 and opacity, multiplied.
        Color3 groupTint{1.0f, 1.0f, 1.0f};
        float groupOpacity = 1.0f;
        std::shared_ptr<const TextLayout> text;
        RectF textRect;
        int zIndex = 1;
        std::size_t sequence = 0; // depth-first order
        std::uint32_t layer = 0;  // layers_ index + 1: in the world; 0: on the screen
    };
    struct Frame;

    void layoutContainer(Instance* container, const RectF& content, const RectF& clip, const Node& transform,
                         float scale, Frame& frame, int depth);
    void layoutObject(GuiObject* object, const RectF& rectIn, const RectF& clip, const Node& parent, float scale,
                      Frame& frame, int depth);
    std::shared_ptr<const TextLayout> layoutTextOf(GuiObject* object, const RectF& rect, float scale) const;
    // `object`'s size once its AutomaticSize grew it from `size`, measured
    // without laying it out (a UIListLayout stacks children by it).
    // grownX/grownY: an auto-sized ancestor grows along that axis, so this
    // object's overflowing content counts there too, as it does when the
    // laid-out descendants grow it (layoutObject's last pass).
    Vector2 autoSized(GuiObject* object, Vector2 size, float scale, int depth, bool grownX = false,
                      bool grownY = false) const;
    void drawNode(DrawList& list, const Node& node) const;
    void drawImage(DrawList& list, const Node& node, const RectF& rect, const Quad& base) const;
    void drawViewport(DrawList& list, const Node& node, const RectF& rect, const Quad& base) const;
    const Node* hit(float x, float y, bool inputOnly) const;
    // The node of a surface GUI the pointer's ray meets at screen (x, y).
    const Node* surfaceHit(float x, float y, bool inputOnly) const;
    void setHover(GuiObject* object, float x, float y);
    void routeInput(const platform::Event& event, bool capturedOnly = false);

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    ViewportProvider viewports_;
    Vector2 drawnScreen_{1e9f, 1e9f}; // what update() laid out for
    Settings settings_;
    std::vector<std::weak_ptr<Instance>> roots_;
    struct GeometryWatch;
    static std::shared_ptr<GeometryWatch> watchGeometry(Instance& under);
    std::vector<std::shared_ptr<GeometryWatch>> geometryWatches_;
    std::vector<std::uint64_t> measuredRevisions_;
    bool geometryMeasured_ = false;
    bool updating_ = false;
    // What layout found for the instances (AbsolutePosition and friends,
    // AbsoluteContentSize, TextBounds), published when update() ends: a
    // frame with AutomaticSize lays its subtree out again once it knows its
    // size, and scripts watching those properties would otherwise hear of
    // every pass, not only where things end up. One entry per instance and
    // kind; a later pass replaces an earlier one's.
    enum class Published : std::uint8_t { Absolute, ContentSize, TextResults, Cells };
    void publish(const Instance* object, Published kind, std::move_only_function<void()> assign);
    void flushPublished();
    std::unordered_map<std::uintptr_t, std::size_t> publishedIndex_;
    std::vector<std::move_only_function<void()>> published_;
    struct WorldGuis;
    // The billboards and surface GUIs under an instance, kept as they come and go.
    static std::shared_ptr<WorldGuis> trackWorldGuis(Instance& under);
    std::shared_ptr<WorldGuis> worldGuis_;             // the world's
    std::vector<std::shared_ptr<WorldGuis>> rootGuis_; // each root's, which show where they have an Adornee
    std::vector<WorldLayer> layers_;                   // where this layout's world GUIs lie
    // What the mouse reaches surface GUIs through: the camera they were laid
    // out for, and each surface layer's part and whether it takes input.
    struct View {
        CFrame cframe;
        float tanHalf = 0.0f;
        Vector2 screen;
    };
    std::optional<View> view_;
    struct SurfaceInput {
        std::uint32_t layer = 0;        // layers_ index + 1
        const BasePart* part = nullptr; // compared, never followed: it may be gone
        Vector2 canvas;
        bool active = true;
    };
    std::vector<SurfaceInput> surfaces_;
    WorldPick pick_;
    std::vector<Node> nodes_; // draw order
    // A ScreenGui's nodes as the last update laid it out, and what from
    // (update reuses them when that is the same again).
    struct ScreenLayout {
        std::uint64_t revision = 0;
        Vector2 screen;
        float topInset = 0.0f, pixelScale = 0.0f;
        std::optional<Rect> topbarSafeArea;
        GuiService::Inset global;
        const TextBox* focused = nullptr;
        std::uint64_t slideSteps = 0;
        bool measuredHidden = false;
        std::vector<Node> nodes;
        bool sameAs(const ScreenLayout& now) const
        {
            return revision == now.revision && screen == now.screen && topInset == now.topInset
                   && pixelScale == now.pixelScale && topbarSafeArea == now.topbarSafeArea && global == now.global
                   && focused == now.focused && slideSteps == now.slideSteps
                   && (measuredHidden || !now.measuredHidden);
        }
    };
    std::unordered_map<const ScreenGui*, ScreenLayout> screenLayouts_;
    std::uint64_t layerRevision(const ScreenGui* gui) const;
    double time_ = 0.0;
    Vector2 coordinateOrigin_;
    // The content width of the container being laid out: what auto-sized
    // wrapped text grows to at most.
    mutable float wrapWidth_ = 1e6f;

    // Input state.
    std::weak_ptr<Instance> hovered_;
    std::weak_ptr<Instance> pressed_;
    TextBox* focused_ = nullptr;
    std::weak_ptr<Instance> focusedRef_;
    TextEditor editor_;
    using InputTargets = std::vector<std::weak_ptr<Instance>>;
    std::map<Enum::UserInputType, InputTargets> buttonTargets_;
    std::map<Enum::KeyCode, InputTargets> keyTargets_;
    std::map<Enum::UserInputType, InstancePtr> localButtons_;
    std::map<Enum::KeyCode, InstancePtr> localKeys_;
    InstancePtr localMovement_, localWheel_;
    bool deferredInput_ = false;
    InputDispatch inputDispatch_;
    double caretTime_ = 0.0;
    float mouseX_ = 0.0f, mouseY_ = 0.0f;
};

} // namespace lunar::ui
