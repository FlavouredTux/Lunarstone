#pragma once

// 2D user interface: screen, billboard and surface GUIs, the GuiObjects in
// them and the UI components that style and lay them out. Included through
// <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/World.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace lunar {

// An event of a GUI object (MouseButton1Click, FocusLost, ...). Copies start
// with no connections, so cloning an object doesn't clone its listeners.
template <class... Args>
class GuiEvent : public Signal<Args...> {
public:
    GuiEvent() = default;
    GuiEvent(const GuiEvent&) : Signal<Args...>() {}
    GuiEvent& operator=(const GuiEvent&) { return *this; }
    GuiEvent(GuiEvent&&) = delete;
    GuiEvent& operator=(GuiEvent&&) = delete;
    ~GuiEvent() = default;
};

class GuiBase : public InstanceImpl<GuiBase, Instance, ClassKind::Abstract> {};

class GuiBase2d : public InstanceImpl<GuiBase2d, GuiBase, ClassKind::Abstract> {
public:
    // Where the GUI engine last laid this out, in pixels; read-only for scripts.
    // A GuiObject changed since (Size, Position, AnchorPoint, its parent or
    // the parent's rectangle) is worked out from its parent on reading, as
    // the client lays out on demand: scripts read AbsoluteSize right after
    // resizing or cloning and parenting a frame.
    LUNAR_PROPERTY() Vector2 absolutePosition() const { return absoluteRect().position; }
    LUNAR_PROPERTY() Vector2 absoluteSize() const { return absoluteRect().size; }
    LUNAR_PROPERTY() float absoluteRotation() const { return absoluteRotation_; }
    void assignAbsolute(Vector2 position, Vector2 size, float rotation);

    struct AbsoluteRect {
        Vector2 position;
        Vector2 size;
        friend bool operator==(const AbsoluteRect&, const AbsoluteRect&) = default;
    };
    AbsoluteRect absoluteRect() const;

    LUNAR_FIELD(bool, autoLocalize, AutoLocalize, true)
    LUNAR_REF(Instance, rootLocalizationTable, RootLocalizationTable)
    LUNAR_FIELD(bool, selectionGroup, SelectionGroup, false)
    LUNAR_FIELD(Enum::SelectionBehavior, selectionBehaviorUp, SelectionBehaviorUp, Enum::SelectionBehavior::Escape)
    LUNAR_FIELD(Enum::SelectionBehavior, selectionBehaviorDown, SelectionBehaviorDown, Enum::SelectionBehavior::Escape)
    LUNAR_FIELD(Enum::SelectionBehavior, selectionBehaviorLeft, SelectionBehaviorLeft, Enum::SelectionBehavior::Escape)
    LUNAR_FIELD(Enum::SelectionBehavior, selectionBehaviorRight, SelectionBehaviorRight,
                Enum::SelectionBehavior::Escape)

protected:
    template <class T>
    void assignReadOnly(T& field, const T& value, std::string_view property)
    {
        if (field == value)
            return;
        field = value;
        changed.fire(property);
    }

private:
    Vector2 absolutePosition_;
    Vector2 absoluteSize_;
    float absoluteRotation_ = 0.0f;
    // What the last layout went by (GuiObjects only).
    struct LaidOutFrom {
        UDim2 size;
        UDim2 position;
        Vector2 anchorPoint;
        const Instance* parent = nullptr;
        AbsoluteRect parentRect;
        friend bool operator==(const LaidOutFrom&, const LaidOutFrom&) = default;
    };
    std::optional<LaidOutFrom> laidOutFrom_;
    std::optional<LaidOutFrom> layoutInputs() const;
};

// --- Roots: where GuiObjects get drawn ---------------------------------------------

class LayerCollector : public InstanceImpl<LayerCollector, GuiBase2d, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, resetOnSpawn, ResetOnSpawn, true)
    // Global unless set: a server leaves it out of a new instance then, and
    // sends Sibling, which Studio gives the GUIs it inserts [capture: the game's
    // health bar ScreenGui comes with no properties, and its label's shadow,
    // a child at a lower ZIndex, belongs under it].
    LUNAR_FIELD(Enum::ZIndexBehavior, zIndexBehavior, ZIndexBehavior, Enum::ZIndexBehavior::Global)
};

class ScreenGui final : public InstanceImpl<ScreenGui, LayerCollector> {
public:
    LUNAR_FIELD(int, displayOrder, DisplayOrder, 0)
    // Native ScreenGuiProp stores ScreenInsets, not a separate ignore flag:
    // 0x103f3bfc8 maps None/Device/Topbar to true; CoreUI to false.
    LUNAR_PROPERTY() bool ignoreGuiInset() const { return screenInsets_ != Enum::ScreenInsets::CoreUISafeInsets; }
    void setIgnoreGuiInset(bool ignore)
    {
        if (ignore == ignoreGuiInset())
            return;
        setScreenInsets(ignore ? Enum::ScreenInsets::DeviceSafeInsets : Enum::ScreenInsets::CoreUISafeInsets);
    }
    LUNAR_FIELD(bool, clipToDeviceSafeArea, ClipToDeviceSafeArea, true)
    // Where it's laid out: IgnoreGuiInset still wins when set, as on the
    // client. CoreUISafeInsets is below the top bar.
    LUNAR_PROPERTY() Enum::ScreenInsets screenInsets() const { return screenInsets_; }
    void setScreenInsets(Enum::ScreenInsets insets)
    {
        if (screenInsets_ == insets)
            return;
        const bool before = ignoreGuiInset();
        screenInsets_ = insets;
        changed.fire("ScreenInsets");
        if (before != ignoreGuiInset())
            changed.fire("IgnoreGuiInset");
    }

private:
    Enum::ScreenInsets screenInsets_ = Enum::ScreenInsets::CoreUISafeInsets;
};

// Drawn in the world, facing the camera, above its Adornee (or parent).
class BillboardGui final : public InstanceImpl<BillboardGui, LayerCollector> {
public:
    LUNAR_FIELD(bool, active, Active, false)
    LUNAR_REF(Instance, adornee, Adornee)
    LUNAR_FIELD(bool, alwaysOnTop, AlwaysOnTop, false)
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(bool, clipsDescendants, ClipsDescendants, false)
    LUNAR_FIELD(float, distanceLowerLimit, DistanceLowerLimit, 0.0f)
    LUNAR_FIELD(float, distanceStep, DistanceStep, 0.0f)
    LUNAR_FIELD(float, distanceUpperLimit, DistanceUpperLimit, -1.0f)
    LUNAR_FIELD(Vector3, extentsOffset, ExtentsOffset)
    LUNAR_FIELD(Vector3, extentsOffsetWorldSpace, ExtentsOffsetWorldSpace)
    LUNAR_FIELD(float, lightInfluence, LightInfluence, 1.0f)
    LUNAR_FIELD(float, maxDistance, MaxDistance, 1e38f)
    LUNAR_REF(Instance, playerToHideFrom, PlayerToHideFrom)
    LUNAR_FIELD(UDim2, size, Size)
    LUNAR_FIELD(Vector2, sizeOffset, SizeOffset)
    LUNAR_FIELD(Vector3, studsOffset, StudsOffset)
    LUNAR_FIELD(Vector3, studsOffsetWorldSpace, StudsOffsetWorldSpace)
};

class SurfaceGuiBase : public InstanceImpl<SurfaceGuiBase, LayerCollector, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, active, Active, true)
    LUNAR_REF(Instance, adornee, Adornee)
    LUNAR_FIELD(Enum::NormalId, face, Face, Enum::NormalId::Front)
};

// Drawn on a face of its Adornee (or parent) part.
class SurfaceGui final : public InstanceImpl<SurfaceGui, SurfaceGuiBase> {
public:
    LUNAR_FIELD(bool, alwaysOnTop, AlwaysOnTop, false)
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(Vector2, canvasSize, CanvasSize, Vector2(800.0f, 600.0f))
    LUNAR_FIELD(bool, clipsDescendants, ClipsDescendants, true)
    LUNAR_FIELD(float, lightInfluence, LightInfluence, 1.0f)
    LUNAR_FIELD(float, pixelsPerStud, PixelsPerStud, 50.0f)
    LUNAR_FIELD(Enum::SurfaceGuiSizingMode, sizingMode, SizingMode, Enum::SurfaceGuiSizingMode::FixedSize)
    LUNAR_FIELD(float, toolPunchThroughDistance, ToolPunchThroughDistance, 0.0f)
    LUNAR_FIELD(float, zOffset, ZOffset, 0.0f)
};

// --- GuiObjects --------------------------------------------------------------------

class GuiObject : public InstanceImpl<GuiObject, GuiBase2d, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, active, Active, false)
    LUNAR_FIELD(Vector2, anchorPoint, AnchorPoint)
    LUNAR_FIELD(Enum::AutomaticSize, automaticSize, AutomaticSize, Enum::AutomaticSize::None)
    LUNAR_FIELD(Color3, backgroundColor3, BackgroundColor3, Color3::fromRGB(163, 162, 165))
    LUNAR_FIELD(float, backgroundTransparency, BackgroundTransparency, 0.0f)
    LUNAR_FIELD(Color3, borderColor3, BorderColor3, Color3::fromRGB(27, 42, 53))
    LUNAR_FIELD(Enum::BorderMode, borderMode, BorderMode, Enum::BorderMode::Outline)
    LUNAR_FIELD(int, borderSizePixel, BorderSizePixel, 1)
    LUNAR_FIELD(bool, clipsDescendants, ClipsDescendants, false)
    LUNAR_FIELD(bool, draggable, Draggable, false)
    LUNAR_FIELD(bool, interactable, Interactable, true)
    LUNAR_FIELD(int, layoutOrder, LayoutOrder, 0)
    LUNAR_REF(GuiObject, nextSelectionUp, NextSelectionUp)
    LUNAR_REF(GuiObject, nextSelectionDown, NextSelectionDown)
    LUNAR_REF(GuiObject, nextSelectionLeft, NextSelectionLeft)
    LUNAR_REF(GuiObject, nextSelectionRight, NextSelectionRight)
    LUNAR_FIELD(UDim2, position, Position)
    LUNAR_FIELD(float, rotation, Rotation, 0.0f)
    LUNAR_FIELD(bool, selectable, Selectable, false)
    LUNAR_REF(GuiObject, selectionImageObject, SelectionImageObject)
    LUNAR_FIELD(int, selectionOrder, SelectionOrder, 0)
    LUNAR_FIELD(UDim2, size, Size)
    LUNAR_FIELD(Enum::SizeConstraint, sizeConstraint, SizeConstraint, Enum::SizeConstraint::RelativeXY)
    LUNAR_FIELD(bool, visible, Visible, true)
    LUNAR_FIELD(int, zIndex, ZIndex, 1)
    // The deprecated Transparency, which old scripts still set. It reads as
    // BackgroundTransparency (the 0.742 client registers that property's
    // getter for it) and sets it, and TextTransparency too on the text
    // objects (GUESS: the setter is virtual there, and the API calls it a
    // mix of the two). Servers don't replicate it.
    LUNAR_PROPERTY("Transparency") float transparency() const { return backgroundTransparency(); }
    void setTransparency(float transparency);

    // The GUI engine's mouse events: each fires the C++ signal, then the
    // scripts' event of that name with the client's arguments (x, y).
    GuiEvent<int, int> mouseEnter; // x, y
    GuiEvent<int, int> mouseLeave;
    GuiEvent<int, int> mouseMoved;
    void fireMouseEnter(int x, int y);
    void fireMouseLeave(int x, int y);
    void fireMouseMoved(int x, int y);
};

// Shared by the image and text GuiObjects; Roblox repeats them per class too.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define LUNAR_GUI_IMAGE_PROPERTIES                                                                                     \
    LUNAR_FIELD(std::string, image, Image)                                                                             \
    LUNAR_FIELD(Color3, imageColor3, ImageColor3, Color3(1.0f, 1.0f, 1.0f))                                            \
    LUNAR_FIELD(Vector2, imageRectOffset, ImageRectOffset)                                                             \
    LUNAR_FIELD(Vector2, imageRectSize, ImageRectSize)                                                                 \
    LUNAR_FIELD(float, imageTransparency, ImageTransparency, 0.0f)                                                     \
    LUNAR_FIELD(Enum::ResamplerMode, resampleMode, ResampleMode, Enum::ResamplerMode::Default)                         \
    LUNAR_FIELD(Enum::ScaleType, scaleType, ScaleType, Enum::ScaleType::Stretch)                                       \
    LUNAR_FIELD(Rect, sliceCenter, SliceCenter)                                                                        \
    LUNAR_FIELD(float, sliceScale, SliceScale, 1.0f)                                                                   \
    LUNAR_FIELD(UDim2, tileSize, TileSize, UDim2::fromScale(1.0f, 1.0f))

// The deprecated FontSize: TextSize in steps (Size14 is 14 pixels). Setting
// it sets TextSize; reading it gives the step nearest TextSize (GUESS: the
// client's choice between two steps).
float fontSizePixels(Enum::FontSize size);
Enum::FontSize nearestFontSize(float pixels);

#define LUNAR_GUI_TEXT_PROPERTIES                                                                                      \
public:                                                                                                                \
    LUNAR_PROPERTY() Enum::FontSize fontSize() const                                                                   \
    {                                                                                                                  \
        return nearestFontSize(textSize());                                                                            \
    }                                                                                                                  \
    void setFontSize(Enum::FontSize size)                                                                              \
    {                                                                                                                  \
        this->setTextSize(fontSizePixels(size));                                                                       \
    }                                                                                                                  \
    LUNAR_FIELD(std::string, text, Text)                                                                               \
    LUNAR_FIELD(Font, fontFace, FontFace)                                                                              \
    LUNAR_FIELD(Enum::Font, font, Font, Enum::Font::Legacy)                                                            \
    LUNAR_FIELD(float, lineHeight, LineHeight, 1.0f)                                                                   \
    LUNAR_FIELD(int, maxVisibleGraphemes, MaxVisibleGraphemes, -1)                                                     \
    LUNAR_FIELD(bool, richText, RichText, false)                                                                       \
    LUNAR_FIELD(Color3, textColor3, TextColor3, Color3::fromRGB(27, 42, 53))                                           \
    LUNAR_FIELD(bool, textScaled, TextScaled, false)                                                                   \
    LUNAR_FIELD(float, textSize, TextSize, 14.0f)                                                                      \
    LUNAR_FIELD(Color3, textStrokeColor3, TextStrokeColor3)                                                            \
    LUNAR_FIELD(float, textStrokeTransparency, TextStrokeTransparency, 1.0f)                                           \
    LUNAR_FIELD(float, textTransparency, TextTransparency, 0.0f)                                                       \
    LUNAR_FIELD(Enum::TextTruncate, textTruncate, TextTruncate, Enum::TextTruncate::None)                              \
    LUNAR_FIELD(bool, textWrapped, TextWrapped, false)                                                                 \
    LUNAR_FIELD(Enum::TextXAlignment, textXAlignment, TextXAlignment, Enum::TextXAlignment::Center)                    \
    LUNAR_FIELD(Enum::TextYAlignment, textYAlignment, TextYAlignment, Enum::TextYAlignment::Center)

// What laying out the text gave; read-only for scripts.
#define LUNAR_GUI_TEXT_RESULTS                                                                                         \
public:                                                                                                                \
    LUNAR_PROPERTY() Vector2 textBounds() const                                                                        \
    {                                                                                                                  \
        return textBounds_;                                                                                            \
    }                                                                                                                  \
    LUNAR_PROPERTY() bool textFits() const                                                                             \
    {                                                                                                                  \
        return textFits_;                                                                                              \
    }                                                                                                                  \
    void assignTextResults(Vector2 bounds, bool fits)                                                                  \
    {                                                                                                                  \
        this->assignReadOnly(textBounds_, bounds, "TextBounds");                                                       \
        this->assignReadOnly(textFits_, fits, "TextFits");                                                             \
    }                                                                                                                  \
                                                                                                                       \
private:                                                                                                               \
    Vector2 textBounds_;                                                                                               \
    bool textFits_ = true;                                                                                             \
                                                                                                                       \
public:
// NOLINTEND(bugprone-macro-parentheses)

class Frame final : public InstanceImpl<Frame, GuiObject> {
public:
    LUNAR_FIELD(Enum::FrameStyle, style, Style, Enum::FrameStyle::Custom)
};

class GuiLabel : public InstanceImpl<GuiLabel, GuiObject, ClassKind::Abstract> {};

class ImageLabel final : public InstanceImpl<ImageLabel, GuiLabel> {
public:
    LUNAR_GUI_IMAGE_PROPERTIES
};

class TextLabel final : public InstanceImpl<TextLabel, GuiLabel> {
public:
    LUNAR_GUI_TEXT_PROPERTIES
    LUNAR_GUI_TEXT_RESULTS
    LUNAR_FIELD(bool, localize, Localize, true)
};

class GuiButton : public InstanceImpl<GuiButton, GuiObject, ClassKind::Abstract> {
public:
    GuiButton() { setActive(true); }
    LUNAR_FIELD(bool, autoButtonColor, AutoButtonColor, true)
    LUNAR_FIELD(bool, modal, Modal, false)
    LUNAR_FIELD(bool, selected, Selected, false)
    LUNAR_FIELD(Enum::ButtonStyle, style, Style, Enum::ButtonStyle::Custom)

    GuiEvent<> mouseButton1Click;
    GuiEvent<> mouseButton1Down;
    GuiEvent<> mouseButton1Up;
    GuiEvent<> mouseButton2Click;
    GuiEvent<> activated;
    // The left button pressed on it at x, y (MouseButton1Down), and let go
    // (MouseButton1Up), over it a click (then MouseButton1Click and
    // Activated(InputObject, 1)); the right button let go over it
    // (MouseButton2Click). The signals, then the scripts' events.
    void firePress(int x, int y);
    void fireRelease(int x, int y, bool over);
    void fireRightClick(int x, int y);
};

class ImageButton final : public InstanceImpl<ImageButton, GuiButton> {
public:
    LUNAR_GUI_IMAGE_PROPERTIES
    LUNAR_FIELD(std::string, hoverImage, HoverImage)
    LUNAR_FIELD(std::string, pressedImage, PressedImage)
};

class TextButton final : public InstanceImpl<TextButton, GuiButton> {
public:
    LUNAR_GUI_TEXT_PROPERTIES
    LUNAR_GUI_TEXT_RESULTS
};

class TextBox final : public InstanceImpl<TextBox, GuiObject> {
public:
    TextBox() { setActive(true); }
    LUNAR_GUI_TEXT_PROPERTIES
    LUNAR_GUI_TEXT_RESULTS
    LUNAR_FIELD(bool, clearTextOnFocus, ClearTextOnFocus, true)
    LUNAR_FIELD(bool, multiLine, MultiLine, false)
    LUNAR_FIELD(std::string, placeholderText, PlaceholderText)
    LUNAR_FIELD(Color3, placeholderColor3, PlaceholderColor3, Color3::fromRGB(178, 178, 178))
    LUNAR_FIELD(bool, textEditable, TextEditable, true)
    LUNAR_FIELD(int, cursorPosition, CursorPosition, -1) // 1-based byte position, -1 when not focused
    LUNAR_FIELD(int, selectionStart, SelectionStart, -1)

    GuiEvent<> focused;
    GuiEvent<bool /*enterPressed*/> focusLost;
    // Both the signals and the scripts' Focused and FocusLost(enterPressed, nil).
    void fireFocused();
    void fireFocusLost(bool enterPressed);
};

class ScrollingFrame final : public InstanceImpl<ScrollingFrame, GuiObject> {
public:
    ScrollingFrame() { setActive(true); }
    LUNAR_FIELD(Enum::AutomaticSize, automaticCanvasSize, AutomaticCanvasSize, Enum::AutomaticSize::None)
    LUNAR_FIELD(std::string, bottomImage, BottomImage, "rbxasset://textures/ui/Scroll/scroll-bottom.png")
    LUNAR_FIELD(std::string, midImage, MidImage, "rbxasset://textures/ui/Scroll/scroll-middle.png")
    LUNAR_FIELD(std::string, topImage, TopImage, "rbxasset://textures/ui/Scroll/scroll-top.png")
    LUNAR_FIELD(Vector2, canvasPosition, CanvasPosition)
    LUNAR_FIELD(UDim2, canvasSize, CanvasSize, UDim2::fromScale(0.0f, 2.0f))
    LUNAR_FIELD(Enum::ElasticBehavior, elasticBehavior, ElasticBehavior, Enum::ElasticBehavior::WhenScrollable)
    LUNAR_FIELD(Enum::ScrollBarInset, horizontalScrollBarInset, HorizontalScrollBarInset, Enum::ScrollBarInset::None)
    LUNAR_FIELD(Enum::ScrollBarInset, verticalScrollBarInset, VerticalScrollBarInset, Enum::ScrollBarInset::None)
    LUNAR_FIELD(Enum::VerticalScrollBarPosition, verticalScrollBarPosition, VerticalScrollBarPosition,
                Enum::VerticalScrollBarPosition::Right)
    LUNAR_FIELD(Color3, scrollBarImageColor3, ScrollBarImageColor3, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, scrollBarImageTransparency, ScrollBarImageTransparency, 0.0f)
    LUNAR_FIELD(int, scrollBarThickness, ScrollBarThickness, 12)
    LUNAR_FIELD(Enum::ScrollingDirection, scrollingDirection, ScrollingDirection, Enum::ScrollingDirection::XY)
    LUNAR_FIELD(bool, scrollingEnabled, ScrollingEnabled, true)

    LUNAR_PROPERTY() Vector2 absoluteCanvasSize() const { return absoluteCanvasSize_; }
    LUNAR_PROPERTY() Vector2 absoluteWindowSize() const { return absoluteWindowSize_; }
    void assignCanvas(Vector2 canvas, Vector2 window)
    {
        assignReadOnly(absoluteCanvasSize_, canvas, "AbsoluteCanvasSize");
        assignReadOnly(absoluteWindowSize_, window, "AbsoluteWindowSize");
    }

private:
    Vector2 absoluteCanvasSize_;
    Vector2 absoluteWindowSize_;
};

// Renders its 3D children through CurrentCamera.
class ViewportFrame final : public InstanceImpl<ViewportFrame, GuiObject> {
public:
    LUNAR_FIELD(Color3, ambient, Ambient, Color3::fromRGB(200, 200, 200))
    LUNAR_REF(Camera, currentCamera, CurrentCamera)
    LUNAR_FIELD(Color3, imageColor3, ImageColor3, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, imageTransparency, ImageTransparency, 0.0f)
    LUNAR_FIELD(Color3, lightColor, LightColor, Color3::fromRGB(140, 140, 140))
    LUNAR_FIELD(Vector3, lightDirection, LightDirection, Vector3(-1.0f, -1.0f, -1.0f))
};

// A Frame whose descendants are drawn as one picture, tinted and faded as a whole.
class CanvasGroup final : public InstanceImpl<CanvasGroup, GuiObject> {
public:
    LUNAR_FIELD(Color3, groupColor3, GroupColor3, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, groupTransparency, GroupTransparency, 0.0f)
};

// A video in the GUI; drawn as its background (no video playback).
class VideoFrame final : public InstanceImpl<VideoFrame, GuiObject> {
public:
    LUNAR_FIELD(std::string, video, Video)
    LUNAR_FIELD(bool, looped, Looped, false)
    LUNAR_FIELD(bool, playing, Playing, false)
    LUNAR_FIELD(float, timePosition, TimePosition, 0.0f)
    LUNAR_FIELD(float, volume, Volume, 1.0f)
};

// --- UI components: children that style or lay out their parent -----------------

class UIBase : public InstanceImpl<UIBase, Instance, ClassKind::Abstract> {};
class UIComponent : public InstanceImpl<UIComponent, UIBase, ClassKind::Abstract> {};

class UICorner final : public InstanceImpl<UICorner, UIComponent> {
public:
    LUNAR_FIELD(UDim, cornerRadius, CornerRadius, UDim(0.0f, 8))
    LUNAR_FIELD(UDim, topLeftRadius, TopLeftRadius, UDim(0.0f, 8))
    LUNAR_FIELD(UDim, topRightRadius, TopRightRadius, UDim(0.0f, 8))
    LUNAR_FIELD(UDim, bottomLeftRadius, BottomLeftRadius, UDim(0.0f, 8))
    LUNAR_FIELD(UDim, bottomRightRadius, BottomRightRadius, UDim(0.0f, 8))
};

class UIPadding final : public InstanceImpl<UIPadding, UIComponent> {
public:
    LUNAR_FIELD(UDim, paddingTop, PaddingTop)
    LUNAR_FIELD(UDim, paddingBottom, PaddingBottom)
    LUNAR_FIELD(UDim, paddingLeft, PaddingLeft)
    LUNAR_FIELD(UDim, paddingRight, PaddingRight)
};

class UIGradient final : public InstanceImpl<UIGradient, UIComponent> {
public:
    LUNAR_FIELD(ColorSequence, color, Color)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(Vector2, offset, Offset)
    LUNAR_FIELD(float, rotation, Rotation, 0.0f)
    LUNAR_FIELD(float, scale, Scale, 1.0f)
    LUNAR_FIELD(NumberSequence, transparency, Transparency)
};

class UIStroke final : public InstanceImpl<UIStroke, UIComponent> {
public:
    LUNAR_FIELD(Enum::ApplyStrokeMode, applyStrokeMode, ApplyStrokeMode, Enum::ApplyStrokeMode::Contextual)
    LUNAR_FIELD(Color3, color, Color)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(Enum::LineJoinMode, lineJoinMode, LineJoinMode, Enum::LineJoinMode::Round)
    LUNAR_FIELD(float, thickness, Thickness, 1.0f)
    LUNAR_FIELD(float, transparency, Transparency, 0.0f)
    LUNAR_FIELD(Enum::StrokeSizingMode, strokeSizingMode, StrokeSizingMode, Enum::StrokeSizingMode::FixedSize)
    LUNAR_FIELD(Enum::BorderStrokePosition, borderStrokePosition, BorderStrokePosition,
                Enum::BorderStrokePosition::Outer)
    LUNAR_FIELD(int, zIndex, ZIndex, 1)
};

// A drop shadow behind its parent. GUESS: the defaults.
class UIShadow final : public InstanceImpl<UIShadow, UIComponent> {
public:
    LUNAR_FIELD(UDim, blurRadius, BlurRadius, UDim(0.0f, 8))
    LUNAR_FIELD(Color3, color, Color)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, inset, Inset, false)
    LUNAR_FIELD(Enum::ApplyShadowMode, mode, Mode, Enum::ApplyShadowMode::Shape)
    LUNAR_FIELD(UDim2, offset, Offset)
    LUNAR_FIELD(bool, showBehindParent, ShowBehindParent, true)
    LUNAR_FIELD(UDim2, spread, Spread)
    LUNAR_FIELD(float, transparency, Transparency, 0.5f)
    LUNAR_FIELD(int, zIndex, ZIndex, 1)
};

// How a child of a UIListLayout grows or shrinks along the list. GUESS:
// the ratios' defaults.
class UIFlexItem final : public InstanceImpl<UIFlexItem, UIComponent> {
public:
    LUNAR_FIELD(Enum::UIFlexMode, flexMode, FlexMode, Enum::UIFlexMode::None)
    LUNAR_FIELD(float, growRatio, GrowRatio, 0.0f)
    LUNAR_FIELD(float, shrinkRatio, ShrinkRatio, 0.0f)
    LUNAR_FIELD(Enum::ItemLineAlignment, itemLineAlignment, ItemLineAlignment, Enum::ItemLineAlignment::Automatic)
};

class UIScale final : public InstanceImpl<UIScale, UIComponent> {
public:
    LUNAR_FIELD(float, scale, Scale, 1.0f)
};

class UIConstraint : public InstanceImpl<UIConstraint, UIComponent, ClassKind::Abstract> {};

class UIAspectRatioConstraint final : public InstanceImpl<UIAspectRatioConstraint, UIConstraint> {
public:
    LUNAR_FIELD(float, aspectRatio, AspectRatio, 1.0f)
    LUNAR_FIELD(Enum::AspectType, aspectType, AspectType, Enum::AspectType::FitWithinMaxSize)
    LUNAR_FIELD(Enum::DominantAxis, dominantAxis, DominantAxis, Enum::DominantAxis::Width)
};

class UISizeConstraint final : public InstanceImpl<UISizeConstraint, UIConstraint> {
public:
    LUNAR_FIELD(Vector2, maxSize, MaxSize, Vector2(1e38f, 1e38f))
    LUNAR_FIELD(Vector2, minSize, MinSize)
};

class UITextSizeConstraint final : public InstanceImpl<UITextSizeConstraint, UIConstraint> {
public:
    LUNAR_FIELD(int, maxTextSize, MaxTextSize, 100)
    LUNAR_FIELD(int, minTextSize, MinTextSize, 1)
};

class UILayout : public InstanceImpl<UILayout, UIComponent, ClassKind::Abstract> {};

class UIGridStyleLayout : public InstanceImpl<UIGridStyleLayout, UILayout, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::FillDirection, fillDirection, FillDirection, Enum::FillDirection::Horizontal)
    LUNAR_FIELD(Enum::HorizontalAlignment, horizontalAlignment, HorizontalAlignment, Enum::HorizontalAlignment::Left)
    LUNAR_FIELD(Enum::SortOrder, sortOrder, SortOrder, Enum::SortOrder::LayoutOrder)
    LUNAR_FIELD(Enum::VerticalAlignment, verticalAlignment, VerticalAlignment, Enum::VerticalAlignment::Top)

    // What the GUI engine last laid out: the items' extent, in pixels;
    // read-only for scripts (they size a ScrollingFrame's canvas by it).
    LUNAR_PROPERTY() Vector2 absoluteContentSize() const { return absoluteContentSize_; }
    void assignContentSize(Vector2 size)
    {
        if (size == absoluteContentSize_)
            return;
        absoluteContentSize_ = size;
        changed.fire("AbsoluteContentSize");
    }

private:
    Vector2 absoluteContentSize_;
};

class UIListLayout final : public InstanceImpl<UIListLayout, UIGridStyleLayout> {
public:
    // Lists stack downwards by default; grids fill rows.
    UIListLayout() { setFillDirection(Enum::FillDirection::Vertical); }

    LUNAR_FIELD(UDim, padding, Padding)
    LUNAR_FIELD(bool, wraps, Wraps, false)
    LUNAR_FIELD(UDim, horizontalPadding, HorizontalPadding)
    LUNAR_FIELD(UDim, verticalPadding, VerticalPadding)
    LUNAR_FIELD(Enum::UIFlexAlignment, horizontalFlex, HorizontalFlex, Enum::UIFlexAlignment::None)
    LUNAR_FIELD(Enum::UIFlexAlignment, verticalFlex, VerticalFlex, Enum::UIFlexAlignment::None)
    LUNAR_FIELD(Enum::ItemLineAlignment, itemLineAlignment, ItemLineAlignment, Enum::ItemLineAlignment::Automatic)
};

class UIGridLayout final : public InstanceImpl<UIGridLayout, UIGridStyleLayout> {
public:
    LUNAR_PROPERTY() Vector2 absoluteCellSize() const { return absoluteCellSize_; }
    LUNAR_PROPERTY() Vector2 absoluteCellCount() const { return absoluteCellCount_; }
    void assignCells(Vector2 size, Vector2 count)
    {
        if (absoluteCellSize_ != size) {
            absoluteCellSize_ = size;
            changed.fire("AbsoluteCellSize");
        }
        if (absoluteCellCount_ != count) {
            absoluteCellCount_ = count;
            changed.fire("AbsoluteCellCount");
        }
    }
    LUNAR_FIELD(UDim2, cellPadding, CellPadding, UDim2::fromOffset(5, 5))
    LUNAR_FIELD(UDim2, cellSize, CellSize, UDim2::fromOffset(100, 100))
    LUNAR_FIELD(int, fillDirectionMaxCells, FillDirectionMaxCells, 0)
    LUNAR_FIELD(Enum::StartCorner, startCorner, StartCorner, Enum::StartCorner::TopLeft)
private:
    Vector2 absoluteCellSize_;
    Vector2 absoluteCellCount_;
};

// The siblings as pages in a row (or a column), each at its own size: the
// current one where the alignments put it, the others beside it, where the
// parent usually clips them. JumpTo, JumpToIndex, Next and Previous (the
// script library) move to another: PageLeave and PageEnter fire, the pages
// slide over TweenTime by the easing when Animated, and Stopped fires once
// they're there.
class UIPageLayout final : public InstanceImpl<UIPageLayout, UIGridStyleLayout> {
public:
    UIPageLayout()
    {
        setHorizontalAlignment(Enum::HorizontalAlignment::Center);
        setVerticalAlignment(Enum::VerticalAlignment::Center);
    }

    LUNAR_FIELD(bool, animated, Animated, true)
    LUNAR_FIELD(bool, circular, Circular, false)
    LUNAR_FIELD(Enum::EasingDirection, easingDirection, EasingDirection, Enum::EasingDirection::Out)
    LUNAR_FIELD(Enum::EasingStyle, easingStyle, EasingStyle, Enum::EasingStyle::Back)
    LUNAR_FIELD(bool, gamepadInputEnabled, GamepadInputEnabled, true)
    LUNAR_FIELD(UDim, padding, Padding)
    LUNAR_FIELD(bool, scrollWheelInputEnabled, ScrollWheelInputEnabled, true)
    LUNAR_FIELD(bool, touchInputEnabled, TouchInputEnabled, true)
    LUNAR_FIELD(float, tweenTime, TweenTime, 1.0f)

    // The pages in order: the parent's visible GuiObjects by SortOrder.
    std::vector<GuiObject*> pages() const;
    // The page in view (or being slid to); the first until one is jumped to.
    // Read-only for scripts.
    LUNAR_PROPERTY() GuiObject* currentPage() const;
    void assignCurrentPage(GuiObject* page);

    // Sliding: from the page that was in view, `progress` (eased) of the way to the current one.
    struct Slide {
        std::weak_ptr<Instance> from;
        float progress = 1.0f;
    };
    const Slide& slide() const { return slide_; }
    void assignSlide(Instance* from, float progress);
    // Counts every slide's every step, in any page layout: no property says
    // so, and a GUI's layout kept from before one is out of date.
    static std::uint64_t slideSteps() { return slideSteps_.load(std::memory_order_relaxed); }

private:
    std::weak_ptr<Instance> currentPage_;
    Slide slide_;
    static inline std::atomic<std::uint64_t> slideSteps_ = 0;
};

// --- TextChatService's look -----------------------------------------------------------

class TextChatConfigurations : public InstanceImpl<TextChatConfigurations, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Color3, backgroundColor3, BackgroundColor3, Color3::fromRGB(25, 27, 29))
    LUNAR_FIELD(double, backgroundTransparency, BackgroundTransparency, 0.3)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(Font, fontFace, FontFace)
    LUNAR_FIELD(Color3, textColor3, TextColor3, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(std::int64_t, textSize, TextSize, 14)
    LUNAR_FIELD(Color3, textStrokeColor3, TextStrokeColor3)
    LUNAR_FIELD(double, textStrokeTransparency, TextStrokeTransparency, 0.5)
};

class ChatWindowConfiguration final : public InstanceImpl<ChatWindowConfiguration, TextChatConfigurations> {
public:
    LUNAR_PROPERTY() Vector2 absolutePosition() const { return absolutePosition_; }
    LUNAR_PROPERTY() Vector2 absoluteSize() const { return absoluteSize_; }
    void assignChatGeometry(Vector2 position, Vector2 size)
    {
        this->setProperty(absolutePosition_, position, "AbsolutePosition");
        this->setProperty(absoluteSize_, size, "AbsoluteSize");
    }
    LUNAR_FIELD(float, heightScale, HeightScale, 1.0f)
    LUNAR_FIELD(float, widthScale, WidthScale, 1.0f)
    LUNAR_FIELD(Enum::HorizontalAlignment, horizontalAlignment, HorizontalAlignment, Enum::HorizontalAlignment::Left)
    LUNAR_FIELD(Enum::VerticalAlignment, verticalAlignment, VerticalAlignment, Enum::VerticalAlignment::Top)
private:
    Vector2 absolutePosition_;
    Vector2 absoluteSize_;
};

class ChatInputBarConfiguration final : public InstanceImpl<ChatInputBarConfiguration, TextChatConfigurations> {
public:
    LUNAR_PROPERTY() Vector2 absolutePosition() const { return absolutePosition_; }
    LUNAR_PROPERTY() Vector2 absoluteSize() const { return absoluteSize_; }
    LUNAR_PROPERTY() bool isFocused() const { return focused_; }
    void assignFocused(bool focused) { this->setProperty(focused_, focused, "IsFocused"); }
    void requestFocus() { focusRequested_ = true; }
    bool takeFocusRequest() { return std::exchange(focusRequested_, false); }
    void assignChatGeometry(Vector2 position, Vector2 size)
    {
        this->setProperty(absolutePosition_, position, "AbsolutePosition");
        this->setProperty(absoluteSize_, size, "AbsoluteSize");
    }
    LUNAR_FIELD(bool, autocompleteEnabled, AutocompleteEnabled, true)
    LUNAR_FIELD(Enum::KeyCode, keyboardKeyCode, KeyboardKeyCode, Enum::KeyCode::Slash)
    LUNAR_FIELD(Color3, placeholderColor3, PlaceholderColor3, Color3::fromRGB(178, 178, 178))
    LUNAR_REF(Instance, targetTextChannel, TargetTextChannel)
private:
    Vector2 absolutePosition_;
    Vector2 absoluteSize_;
    bool focused_ = false;
    bool focusRequested_ = false;
};

class ChannelTabsConfiguration final : public InstanceImpl<ChannelTabsConfiguration, TextChatConfigurations> {
public:
    LUNAR_FIELD(Color3, hoverBackgroundColor3, HoverBackgroundColor3, Color3::fromRGB(125, 125, 125))
    LUNAR_FIELD(Color3, selectedTabTextColor3, SelectedTabTextColor3, Color3(1.0f, 1.0f, 1.0f))
};

class BubbleChatConfiguration final : public InstanceImpl<BubbleChatConfiguration, TextChatConfigurations> {
public:
    LUNAR_FIELD(std::string, adorneeName, AdorneeName, "HumanoidRootPart")
    LUNAR_FIELD(float, bubbleDuration, BubbleDuration, 15.0f)
    LUNAR_FIELD(float, bubblesSpacing, BubblesSpacing, 6.0f)
    LUNAR_FIELD(Enum::Font, font, Font, Enum::Font::GothamMedium)
    LUNAR_FIELD(Vector3, localPlayerStudsOffset, LocalPlayerStudsOffset)
    LUNAR_FIELD(float, maxBubbles, MaxBubbles, 3.0f)
    LUNAR_FIELD(float, maxDistance, MaxDistance, 100.0f)
    LUNAR_FIELD(float, minimizeDistance, MinimizeDistance, 40.0f)
    LUNAR_FIELD(bool, tailVisible, TailVisible, true)
    LUNAR_FIELD(float, verticalStudsOffset, VerticalStudsOffset, 0.0f)
};

} // namespace lunar
