#include <lunar/ui/Gui.hpp>

#include <lunar/render/Lighting.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <ranges>
#include <unordered_map>

namespace lunar::ui {

namespace {

constexpr float degrees = std::numbers::pi_v<float> / 180.0f;

template <class T>
T* childOf(const Instance* parent)
{
    for (const InstancePtr& child : parent->children())
        if (auto* c = instanceCast<T>(child.get()))
            return c;
    return nullptr;
}

// UDim with its offset scaled by an accumulated UIScale.
float resolveScaled(UDim u, float length, float scale)
{
    return u.scale * length + static_cast<float>(u.offset) * scale;
}

Color3 darken(Color3 c, float f)
{
    return {c.r * f, c.g * f, c.b * f};
}

struct TextProps {
    const std::string* text = nullptr;
    const lunar::Font* fontFace = nullptr;
    Enum::Font font = Enum::Font::Legacy;
    float lineHeight = 1.0f;
    int maxVisibleGraphemes = -1;
    bool richText = false;
    Color3 color;
    bool scaled = false;
    float size = 14.0f;
    Color3 strokeColor;
    float strokeTransparency = 1.0f;
    float transparency = 0.0f;
    Enum::TextTruncate truncate = Enum::TextTruncate::None;
    bool wrapped = false;
    Enum::TextXAlignment xAlignment = Enum::TextXAlignment::Center;
    Enum::TextYAlignment yAlignment = Enum::TextYAlignment::Center;
};

template <class T>
TextProps textPropsOf(const T& o)
{
    TextProps p;
    p.text = &o.text();
    p.fontFace = &o.fontFace();
    p.font = o.font();
    p.lineHeight = o.lineHeight();
    p.maxVisibleGraphemes = o.maxVisibleGraphemes();
    p.richText = o.richText();
    p.color = o.textColor3();
    p.scaled = o.textScaled();
    p.size = o.textSize();
    p.strokeColor = o.textStrokeColor3();
    p.strokeTransparency = o.textStrokeTransparency();
    p.transparency = o.textTransparency();
    p.truncate = o.textTruncate();
    p.wrapped = o.textWrapped();
    p.xAlignment = o.textXAlignment();
    p.yAlignment = o.textYAlignment();
    return p;
}

std::optional<TextProps> textProps(const GuiObject* object)
{
    if (const auto* l = instanceCast<const TextLabel>(object))
        return textPropsOf(*l);
    if (const auto* b = instanceCast<const TextButton>(object))
        return textPropsOf(*b);
    if (const auto* t = instanceCast<const TextBox>(object))
        return textPropsOf(*t);
    if (const auto* label = instanceCast<const InputActionLabel>(object)) {
        if (!label->resolvedImageContent().empty())
            return std::nullopt;
        TextProps p;
        p.text = &label->resolvedText();
        p.fontFace = &label->fontFace();
        p.color = label->textColor3();
        p.size = label->textSize();
        p.transparency = label->textTransparency();
        p.wrapped = label->textWrapped();
        p.xAlignment = label->textXAlignment();
        p.yAlignment = label->textYAlignment();
        return p;
    }
    return std::nullopt;
}

// The font a text object uses. FontFace is what the client renders with;
// files that only set the legacy Font enum leave FontFace at its default,
// so a non-default enum with a default FontFace means the enum.
lunar::Font effectiveFont(const TextProps& p)
{
    static const lunar::Font defaultFace;
    if (*p.fontFace == defaultFace && p.font != Enum::Font::Legacy && p.font != Enum::Font::SourceSans)
        return fontFromEnum(p.font);
    return *p.fontFace;
}

void assignTextResults(GuiObject* object, Vector2 bounds, bool fits)
{
    if (auto* l = instanceCast<TextLabel>(object))
        l->assignTextResults(bounds, fits);
    else if (auto* b = instanceCast<TextButton>(object))
        b->assignTextResults(bounds, fits);
    else if (auto* t = instanceCast<TextBox>(object))
        t->assignTextResults(bounds, fits);
}

struct ImageProps {
    const std::string* image = nullptr;
    Color3 color;
    Vector2 rectOffset;
    Vector2 rectSize;
    float transparency = 0.0f;
    Enum::ResamplerMode resample = Enum::ResamplerMode::Default;
    Enum::ScaleType scaleType = Enum::ScaleType::Stretch;
    Rect sliceCenter;
    float sliceScale = 1.0f;
    UDim2 tileSize;
};

template <class T>
ImageProps imagePropsOf(const T& o)
{
    return {&o.image(),       o.imageColor3(), o.imageRectOffset(), o.imageRectSize(), o.imageTransparency(),
            o.resampleMode(), o.scaleType(),   o.sliceCenter(),     o.sliceScale(),    o.tileSize()};
}

std::vector<GuiObject*> guiChildren(const Instance* parent)
{
    std::vector<GuiObject*> out;
    // React's provider/page wrappers are Folders. They contribute no size
    // or clipping of their own; their GUI descendants share this container.
    auto collect = [&](this auto& self, const Instance& container) -> void {
        for (const InstancePtr& child : container.children()) {
            if (auto* gui = instanceCast<GuiObject>(child.get())) {
                if (gui->visible())
                    out.push_back(gui);
            } else if (child->isA("Folder")) {
                self(*child);
            }
        }
    };
    collect(*parent);
    return out;
}

// Sibling draw order: ZIndex, then child order.
void sortByZ(std::vector<GuiObject*>& objects)
{
    std::ranges::stable_sort(objects, {}, [](const GuiObject* g) { return g->zIndex(); });
}

std::vector<GuiObject*> sortForLayout(std::vector<GuiObject*> objects, Enum::SortOrder order)
{
    if (order == Enum::SortOrder::Name)
        std::ranges::stable_sort(objects, {}, [](const GuiObject* g) { return g->name(); });
    else if (order == Enum::SortOrder::LayoutOrder)
        std::ranges::stable_sort(objects, {}, [](const GuiObject* g) { return g->layoutOrder(); });
    return objects;
}

// A child's size: Size against the parent, then its own constraints and UIScale.
Vector2 childSize(const GuiObject* child, Vector2 parentSize, float scale, float& childScale)
{
    Vector2 size{
        resolveScaled(child->size().x,
                      child->sizeConstraint() == Enum::SizeConstraint::RelativeYY ? parentSize.y : parentSize.x, scale),
        resolveScaled(child->size().y,
                      child->sizeConstraint() == Enum::SizeConstraint::RelativeXX ? parentSize.x : parentSize.y,
                      scale)};
    if (const auto* aspect = childOf<UIAspectRatioConstraint>(child))
        size = applyAspectRatio(size, aspect->aspectRatio(), aspect->aspectType(), aspect->dominantAxis());
    if (const auto* limits = childOf<UISizeConstraint>(child)) {
        size.x = std::clamp(size.x, limits->minSize().x * scale,
                            std::max(limits->maxSize().x * scale, limits->minSize().x * scale));
        size.y = std::clamp(size.y, limits->minSize().y * scale,
                            std::max(limits->maxSize().y * scale, limits->minSize().y * scale));
    }
    childScale = scale;
    if (const auto* uiScale = childOf<UIScale>(child)) {
        size = {size.x * uiScale->scale(), size.y * uiScale->scale()};
        childScale = scale * uiScale->scale();
    }
    return size;
}

RectF inset(const RectF& r, const UIPadding* padding, float scale)
{
    if (!padding)
        return r;
    const float l = resolveScaled(padding->paddingLeft(), r.w, scale);
    const float rt = resolveScaled(padding->paddingRight(), r.w, scale);
    const float t = resolveScaled(padding->paddingTop(), r.h, scale);
    const float b = resolveScaled(padding->paddingBottom(), r.h, scale);
    return {r.x + l, r.y + t, std::max(r.w - l - rt, 0.0f), std::max(r.h - t - b, 0.0f)};
}

struct LegacySkin {
    std::string_view image;
    float border;
    float minimum;
    float padding;
    float contentMinimum;
};

// RobloxPlayer 0.742: Frame::render2d (0x103e76bdc), the button
// background renderer (0x103ec3464), and their client-rectangle methods
// (0x103e7694c / 0x103ec2f80). These styles carry their own art and insets.
std::optional<LegacySkin> legacySkin(const GuiObject* object, bool hovered = false, bool pressed = false)
{
    if (const auto* button = instanceCast<const GuiButton>(object)) {
        const bool enabled = button->active() && button->interactable();
        const bool highlight = button->autoButtonColor() && enabled && (hovered || pressed || button->selected());
        const bool disabled = button->autoButtonColor() && !enabled;
        using S = Enum::ButtonStyle;
        switch (button->style()) {
        case S::RobloxButtonDefault:
            return LegacySkin{disabled    ? "rbxasset://textures/ui/btn_greyTransp.png"
                              : highlight ? "rbxasset://textures/ui/btn_redGlow.png"
                                          : "rbxasset://textures/ui/btn_red.png",
                              8, 36, 12, 36};
        case S::RobloxButton:
            return LegacySkin{button->autoButtonColor() && button->selected() && enabled
                                  ? "rbxasset://textures/ui/btn_red.png"
                              : highlight ? "rbxasset://textures/ui/btn_white.png"
                              : disabled  ? "rbxasset://textures/ui/btn_greyTransp.png"
                                          : "rbxasset://textures/ui/btn_grey.png",
                              8, 36, 12, 36};
        case S::RobloxRoundButton:
            return LegacySkin{highlight ? "rbxasset://textures/ui/btn_newGreyGlow.png"
                                        : "rbxasset://textures/ui/btn_newGrey.png",
                              6, 20, 12, 26};
        case S::RobloxRoundDefaultButton:
            return LegacySkin{disabled    ? "rbxasset://textures/ui/btn_newGrey.png"
                              : highlight ? "rbxasset://textures/ui/btn_newBlueGlow.png"
                                          : "rbxasset://textures/ui/btn_newBlue.png",
                              6, 20, 12, 26};
        case S::RobloxRoundDropdownButton:
            return LegacySkin{disabled    ? "rbxasset://textures/ui/btn_newGrey.png"
                              : highlight ? "rbxasset://textures/ui/btn_newWhiteGlow.png"
                                          : "rbxasset://textures/ui/btn_newWhite.png",
                              6, 20, 12, 26};
        case S::Custom:
            break;
        }
    } else if (const auto* frame = instanceCast<const lunar::Frame>(object)) {
        using S = Enum::FrameStyle;
        switch (frame->style()) {
        case S::ChatBlue:
            return LegacySkin{"rbxasset://textures/ui/dialog_blue.png", 7, 40, 17, 60};
        case S::ChatGreen:
            return LegacySkin{"rbxasset://textures/ui/dialog_green.png", 7, 40, 17, 60};
        case S::ChatRed:
            return LegacySkin{"rbxasset://textures/ui/dialog_red.png", 7, 40, 17, 60};
        case S::RobloxSquare:
            return LegacySkin{"rbxasset://textures/blackBkg_square.png", 7, 14, 8, 21};
        case S::RobloxRound:
            return LegacySkin{"rbxasset://textures/blackBkg_round.png", 7, 14, 8, 21};
        case S::DropShadow:
            return LegacySkin{"rbxasset://textures/ui/newBkg_square.png", 10, 40, 8, 21};
        case S::Custom:
            break;
        }
    }
    return std::nullopt;
}

RectF childContentRect(const GuiObject* object, RectF rect, const UIPadding* padding, float scale)
{
    if (const auto skin = legacySkin(object); skin && scale > 0) {
        const float fraction = std::clamp(std::min(rect.w, rect.h) / (skin->contentMinimum * scale), 0.0f, 1.0f);
        const float margin = skin->padding * scale * fraction;
        rect = {rect.x + margin, rect.y + margin, std::max(rect.w - margin * 2, 0.0f),
                std::max(rect.h - margin * 2, 0.0f)};
    }
    return inset(rect, padding, scale);
}

void drawLegacySkin(DrawList& list, const ImageProvider& images, const LegacySkin& skin, const RectF& rect, float scale,
                    const Quad& base)
{
    if (scale <= 0 || rect.w <= 0 || rect.h <= 0)
        return;
    const auto image = images ? images(skin.image) : nullptr;
    if (!image || image->empty())
        return;
    const float width = static_cast<float>(image->width), height = static_cast<float>(image->height);
    const float corner = std::min(skin.border, std::min(width, height) / 2);
    const float fraction = std::clamp(std::min(rect.w, rect.h) / (skin.minimum * scale), 0.0f, 1.0f);
    const float margin = std::min(skin.border * scale * fraction, std::min(rect.w, rect.h) / 2);
    const float xs[]{rect.x, rect.x + margin, rect.right() - margin, rect.right()};
    const float ys[]{rect.y, rect.y + margin, rect.bottom() - margin, rect.bottom()};
    const float us[]{0, corner / width, 1 - corner / width, 1};
    const float vs[]{0, corner / height, 1 - corner / height, 1};
    Quad quad = base;
    quad.kind = QuadKind::Image;
    quad.texture = list.texture(image);
    quad.color = {1, 1, 1, 1};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) {
            quad.rect = {xs[x], ys[y], xs[x + 1] - xs[x], ys[y + 1] - ys[y]};
            quad.uv = {us[x], vs[y], us[x + 1], vs[y + 1]};
            if (quad.rect.w > 0 && quad.rect.h > 0)
                list.push(quad);
        }
}

} // namespace

// --- Pure layout math ------------------------------------------------------------------

Vector2 resolveSize(UDim2 size, Vector2 parent, Enum::SizeConstraint constraint)
{
    float sx = parent.x;
    float sy = parent.y;
    if (constraint == Enum::SizeConstraint::RelativeXX)
        sy = parent.x;
    else if (constraint == Enum::SizeConstraint::RelativeYY)
        sx = parent.y;
    return {resolve(size.x, sx), resolve(size.y, sy)};
}

Placement place(UDim2 position, UDim2 size, Vector2 anchor, const RectF& parent, Enum::SizeConstraint constraint)
{
    const Vector2 s = resolveSize(size, {parent.w, parent.h}, constraint);
    const float x = parent.x + resolve(position.x, parent.w) - anchor.x * s.x;
    const float y = parent.y + resolve(position.y, parent.h) - anchor.y * s.y;
    return {{x, y, s.x, s.y}};
}

Vector2 applyAspectRatio(Vector2 size, float ratio, Enum::AspectType type, Enum::DominantAxis axis)
{
    if (ratio <= 0.0f)
        return size;
    if (type == Enum::AspectType::FitWithinMaxSize) {
        if (size.y <= 0.0f || size.x / size.y > ratio)
            return {size.y * ratio, size.y};
        return {size.x, size.x / ratio};
    }
    if (axis == Enum::DominantAxis::Width)
        return {size.x, size.x / ratio};
    return {size.y * ratio, size.y};
}

// --- GuiEngine -------------------------------------------------------------------------

std::optional<Enum::KeyCode> keyCodeFor(platform::Key key)
{
    static constexpr std::array<std::string_view, 10> digits{"Zero", "One", "Two",   "Three", "Four",
                                                             "Five", "Six", "Seven", "Eight", "Nine"};
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> renames{{
        {"Enter", "Return"},
        {"LeftCtrl", "LeftControl"},
        {"RightCtrl", "RightControl"},
        {"Grave", "Backquote"},
        {"Apostrophe", "Quote"},
        {"KeypadDecimal", "KeypadPeriod"},
    }};
    std::string name(platform::keyName(key));
    for (const auto& [from, to] : renames)
        if (name == from)
            name = to;
    if (name.size() == 4 && name.starts_with("Num") && std::isdigit(static_cast<unsigned char>(name[3])))
        name = digits[static_cast<std::size_t>(name[3] - '0')];
    else if (name.size() == 7 && name.starts_with("Keypad") && std::isdigit(static_cast<unsigned char>(name[6])))
        name = std::format("Keypad{}", digits[static_cast<std::size_t>(name[6] - '0')]);
    const lunar::EnumInfo* info = lunar::enums::find("KeyCode");
    const lunar::EnumItem* item = info ? info->find(name) : nullptr;
    if (!item)
        return std::nullopt;
    return static_cast<lunar::Enum::KeyCode>(item->value);
}

struct GuiEngine::Frame {
    bool measureHidden = false;
    std::vector<Node> nodes;
    std::size_t sequence = 0;
    const GuiObject* growing = nullptr;
    bool reflowing = false;
    std::unordered_map<const GuiObject*, Vector2> grownSizes;

    void beginReflow()
    {
        nodes.clear();
        sequence = 0;
        growing = nullptr;
        reflowing = true;
    }
};

struct GuiEngine::GeometryWatch {
    std::uint64_t revision = 0;
    // The changes by the ScreenGui they're in (the nearest, at the time),
    // those in none under null: update lays out again only the ScreenGuis
    // whose count moved.
    std::unordered_map<const Instance*, std::uint64_t> layers;
    void count(const Instance* object)
    {
        ++revision;
        while (object && !instanceCast<ScreenGui>(object))
            object = object->parent();
        ++layers[object];
    }
    Connection added, removed;
    std::unordered_map<const Instance*, std::vector<Connection>> properties;
    ~GeometryWatch()
    {
        added.disconnect();
        removed.disconnect();
        for (auto& [_, list] : properties)
            for (auto& connection : list)
                connection.disconnect();
    }
};

std::shared_ptr<GuiEngine::GeometryWatch> GuiEngine::watchGeometry(Instance& under)
{
    auto watch = std::make_shared<GeometryWatch>();
    const std::weak_ptr<GeometryWatch> weak = watch;
    auto attach = [weak](Instance* object) {
        auto state = weak.lock();
        if (!state || state->properties.contains(object))
            return;
        // What a script changes on a Sound or a value kept in the GUI moves
        // nothing (the game turns a Sound's Volume every frame); its coming
        // and going still counts.
        if (!instanceCast<GuiBase>(object) && !instanceCast<UIBase>(object))
            return;
        auto& connections = state->properties[object];
        connections.push_back(object->changed.connect([weak, object](std::string_view property) {
            if (property.starts_with("Absolute") || property == "TextBounds" || property == "TextFits"
                || property == "ContentText" || property == "IsLoaded" || property == "CursorPosition"
                || property == "SelectionStart" || property.ends_with("Color3") || property.ends_with("Transparency"))
                return;
            if (auto current = weak.lock())
                current->count(object);
        }));
        connections.push_back(object->attributeChanged.connect([weak, object](std::string_view) {
            if (auto current = weak.lock())
                current->count(object);
        }));
    };
    attach(&under);
    for (const auto& object : under.descendants())
        attach(object.get());
    watch->added = under.descendantAdded.connect([weak, attach](Instance* object) {
        attach(object);
        if (auto current = weak.lock())
            current->count(object);
    });
    watch->removed = under.descendantRemoving.connect([weak](Instance* object) {
        if (auto state = weak.lock()) {
            state->count(object);
            if (auto it = state->properties.find(object); it != state->properties.end()) {
                for (auto& connection : it->second)
                    connection.disconnect();
                state->properties.erase(it);
            }
        }
    });
    return watch;
}

std::uint64_t GuiEngine::layerRevision(const ScreenGui* gui) const
{
    std::uint64_t sum = 0;
    for (const auto& watch : geometryWatches_)
        for (const Instance* layer : {static_cast<const Instance*>(gui), static_cast<const Instance*>(nullptr)})
            if (auto it = watch->layers.find(layer); it != watch->layers.end())
                sum += it->second;
    return sum;
}

bool GuiEngine::geometryQueryNeedsUpdate() const
{
    if (updating_)
        return false;
    if (!geometryMeasured_ || measuredRevisions_.size() != geometryWatches_.size())
        return true;
    for (std::size_t i = 0; i < geometryWatches_.size(); ++i)
        if (measuredRevisions_[i] != geometryWatches_[i]->revision)
            return true;
    return false;
}

void GuiEngine::ensureGeometry(Vector2 screen, double time)
{
    if (geometryQueryNeedsUpdate())
        update(screen, time, true);
}

GuiEngine::GuiEngine(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}

void GuiEngine::setRoots(const std::vector<Instance*>& roots)
{
    std::vector<Instance*> wanted;
    for (Instance* root : roots)
        if (root)
            wanted.push_back(root);
    // Asked every frame: the roots' world GUIs are only looked for again
    // when the roots change.
    if (std::ranges::equal(wanted, roots_, {}, {}, [](const std::weak_ptr<Instance>& w) { return w.lock().get(); }))
        return;
    roots_.clear();
    rootGuis_.clear();
    geometryWatches_.clear();
    geometryMeasured_ = false;
    screenLayouts_.clear();
    for (Instance* root : wanted) {
        roots_.push_back(root->shared_from_this());
        rootGuis_.push_back(trackWorldGuis(*root));
        geometryWatches_.push_back(watchGeometry(*root));
    }
}

std::shared_ptr<const TextLayout> GuiEngine::layoutTextOf(GuiObject* object, const RectF& rect, float scale) const
{
    auto props = textProps(object);
    if (!props)
        return nullptr;
    TextParams params;
    std::string_view shown = *props->text;
    auto* box = instanceCast<TextBox>(object);
    if (box && box == focused_)
        shown = editor_.text;
    const bool placeholder = box && shown.empty() && box != focused_;
    if (placeholder)
        shown = box->placeholderText();
    params.text = shown;
    params.style.font = effectiveFont(*props);
    params.style.size = props->size * scale;
    params.style.color = placeholder ? box->placeholderColor3() : props->color;
    params.style.transparency = props->transparency;
    params.lineHeight = props->lineHeight;
    params.wrapped = props->wrapped;
    params.scaled = props->scaled;
    params.richText = props->richText && (!box || box != focused_);
    params.xAlignment = props->xAlignment;
    params.yAlignment = props->yAlignment;
    params.truncate = props->truncate;
    params.box = {rect.w, rect.h};
    params.maxVisibleGraphemes = props->maxVisibleGraphemes;
    if (const auto* limit = childOf<UITextSizeConstraint>(object)) {
        params.minScaledSize = static_cast<int>(static_cast<float>(limit->minTextSize()) * scale);
        params.maxScaledSize = static_cast<int>(static_cast<float>(limit->maxTextSize()) * scale);
    } else {
        params.maxScaledSize = static_cast<int>(100.0f * scale);
    }
    // Shared with the layout cache. Its plain text is the shown text with
    // any markup taken out; a focused TextBox, whose caret indexes it,
    // shows no markup.
    return layoutTextShared(*fonts_, params);
}

// --- BillboardGuis and SurfaceGuis ---------------------------------------------------

struct GuiEngine::WorldGuis {
    std::weak_ptr<Instance> world;
    std::vector<std::weak_ptr<Instance>> guis;
    Connection added, removing;

    ~WorldGuis()
    {
        added.disconnect();
        removing.disconnect();
    }
};

namespace {

bool isWorldGui(const Instance* instance)
{
    return instanceCast<const BillboardGui>(instance) || instanceCast<const SurfaceGui>(instance);
}

} // namespace

std::shared_ptr<GuiEngine::WorldGuis> GuiEngine::trackWorldGuis(Instance& under)
{
    auto set = std::make_shared<WorldGuis>();
    set->world = under.weak_from_this();
    for (const InstancePtr& instance : under.descendants())
        if (isWorldGui(instance.get()))
            set->guis.push_back(instance);
    std::weak_ptr<WorldGuis> weak = set;
    set->added = under.descendantAdded.connect([weak](Instance* instance) {
        if (auto s = weak.lock(); s && isWorldGui(instance))
            s->guis.push_back(instance->weak_from_this());
    });
    set->removing = under.descendantRemoving.connect([weak](Instance* instance) {
        if (auto s = weak.lock(); s && isWorldGui(instance))
            std::erase_if(s->guis, [instance](const std::weak_ptr<Instance>& w) {
                const InstancePtr gui = w.lock();
                return !gui || gui.get() == instance;
            });
    });
    return set;
}

void GuiEngine::setWorld(Workspace* workspace)
{
    if (worldGuis_ && worldGuis_->world.lock().get() == workspace)
        return;
    worldGuis_.reset();
    if (workspace)
        worldGuis_ = trackWorldGuis(*workspace);
}

namespace {

// The part a world GUI's adornee comes down to: the Adornee, else the
// parent; a model gives its PrimaryPart, else its first part.
const BasePart* adorneePart(const Instance* adornee)
{
    if (const auto* model = instanceCast<const Model>(adornee)) {
        if (model->primaryPart())
            return model->primaryPart();
        for (const InstancePtr& d : model->descendants())
            if (const auto* part = instanceCast<const BasePart>(d.get()))
                return part;
        return nullptr;
    }
    return instanceCast<const BasePart>(adornee);
}

// The point a BillboardGui hangs from and its adornee's half extents (of its
// world bounding box): a part, an attachment or a model.
// GUESS: a model's own bounding box isn't used; ExtentsOffset scales by the
// part's world bounding box.
std::optional<std::pair<Vector3, Vector3>> billboardAnchor(const BillboardGui& gui)
{
    const Instance* adornee = gui.adornee() ? gui.adornee() : gui.parent();
    if (const BasePart* part = adorneePart(adornee)) {
        const CFrame frame = part->cframe();
        const Vector3 half = part->size() * 0.5f;
        Vector3 extents;
        for (int axis = 0; axis < 3; ++axis) {
            float e = 0.0f;
            for (int k = 0; k < 3; ++k)
                e += std::abs(frame.r[axis][k]) * (k == 0 ? half.x : k == 1 ? half.y : half.z);
            (axis == 0 ? extents.x : axis == 1 ? extents.y : extents.z) = e;
        }
        return std::pair{frame.position, extents};
    }
    if (const auto* attachment = instanceCast<const Attachment>(adornee))
        return std::pair{attachment->worldCFrame().position, Vector3{}};
    return std::nullopt;
}

Vector3 times(Vector3 a, Vector3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// How much light reaches a world GUI facing `normal` (null: lit from above,
// a billboard), estimated the way an outdoor part's face is lit (mesh.frag's
// diffuseLight without the light grid): ambient, outdoor ambient, the sun
// on it, the back light and the sky's (the renderer's ambient cube, from the
// skybox's average colours: here the sky colour through the gradient, the
// cube's weights adding up to about one). The light is linear and GUI
// colours are sRGB, so it multiplies them encoded. Then Brightness, and
// LightInfluence mixes it in (0: the GUI's own colours).
// GUESS: the real client's GUI lighting; this follows the parts'.
Color3 tintOf(const render::Lighting& light, std::optional<Vector3> normal, float brightness, float influence)
{
    const Vector3 n = normal.value_or(Vector3{0.0f, 1.0f, 0.0f});
    const float ndl = n.dot(light.sunDirection);
    const float sun = std::max(ndl, 0.0f), back = std::max(-ndl, 0.0f);
    const float up = std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f);
    auto channel
        = [&](float ambient, float outdoor, float sunColor, float backColor, float sky, float top, float bottom) {
              const float skyLight = light.environmentDiffuse * sky * sky * (bottom + (top - bottom) * up);
              const float lit = (ambient + outdoor + sunColor * sun + backColor * back + skyLight) * light.exposure;
              const float encoded = std::pow(std::clamp(lit, 0.0f, 1.0f), 1.0f / 2.2f);
              return brightness * (1.0f + (encoded - 1.0f) * std::clamp(influence, 0.0f, 1.0f));
          };
    return {channel(light.ambient.r, light.skyAmbient.r, light.sunColor.r, light.backColor.r, light.sky.r,
                    light.skyGradientTop.r, light.skyGradientBottom.r),
            channel(light.ambient.g, light.skyAmbient.g, light.sunColor.g, light.backColor.g, light.sky.g,
                    light.skyGradientTop.g, light.skyGradientBottom.g),
            channel(light.ambient.b, light.skyAmbient.b, light.sunColor.b, light.backColor.b, light.sky.b,
                    light.skyGradientTop.b, light.skyGradientBottom.b)};
}

} // namespace

std::optional<GuiEngine::SurfacePlacement> GuiEngine::placeSurface(const SurfaceGui& gui)
{
    if (!gui.enabled())
        return std::nullopt;
    const BasePart* part = adorneePart(gui.adornee() ? gui.adornee() : gui.parent());
    if (!part)
        return std::nullopt;
    // Each face in the part's space: the canvas's top-left corner, its x and
    // y directions (y down, as the face is seen from outside) and how many
    // studs they span. Sides read upright; the top reads from the Front
    // (its canvas top at the Back), the bottom from below with the Front up.
    // GUESS: the top and bottom orientation.
    const Vector3 h = part->size() * 0.5f;
    Vector3 corner, x, y, normal;
    float width = 0.0f, height = 0.0f;
    switch (gui.face()) {
    case Enum::NormalId::Front:
        corner = {h.x, h.y, -h.z}, x = {-1, 0, 0}, y = {0, -1, 0}, normal = {0, 0, -1}, width = 2 * h.x,
        height = 2 * h.y;
        break;
    case Enum::NormalId::Back:
        corner = {-h.x, h.y, h.z}, x = {1, 0, 0}, y = {0, -1, 0}, normal = {0, 0, 1}, width = 2 * h.x, height = 2 * h.y;
        break;
    case Enum::NormalId::Right:
        corner = {h.x, h.y, h.z}, x = {0, 0, -1}, y = {0, -1, 0}, normal = {1, 0, 0}, width = 2 * h.z, height = 2 * h.y;
        break;
    case Enum::NormalId::Left:
        corner = {-h.x, h.y, -h.z}, x = {0, 0, 1}, y = {0, -1, 0}, normal = {-1, 0, 0}, width = 2 * h.z,
        height = 2 * h.y;
        break;
    case Enum::NormalId::Top:
        corner = {h.x, h.y, h.z}, x = {-1, 0, 0}, y = {0, 0, -1}, normal = {0, 1, 0}, width = 2 * h.x, height = 2 * h.z;
        break;
    case Enum::NormalId::Bottom:
        corner = {h.x, -h.y, -h.z}, x = {-1, 0, 0}, y = {0, 0, 1}, normal = {0, -1, 0}, width = 2 * h.x,
        height = 2 * h.z;
        break;
    }
    if (width <= 0.0f || height <= 0.0f)
        return std::nullopt;
    const Vector2 canvas = gui.sizingMode() == Enum::SurfaceGuiSizingMode::PixelsPerStud
                               ? Vector2{width * gui.pixelsPerStud(), height * gui.pixelsPerStud()}
                               : gui.canvasSize();
    if (canvas.x <= 0.0f || canvas.y <= 0.0f)
        return std::nullopt;
    const CFrame frame = part->cframe();
    const Vector3 n = frame.vectorToWorldSpace(normal);
    // A little off the face so the part doesn't cover it; ZOffset orders
    // GUIs on the same face. GUESS: how far each step of ZOffset moves.
    const float lift = 0.01f + 0.001f * std::max(gui.zOffset(), 0.0f);
    return SurfacePlacement{frame * corner + n * lift, frame.vectorToWorldSpace(x) * (width / canvas.x),
                            frame.vectorToWorldSpace(y) * (height / canvas.y), n, canvas};
}

std::optional<GuiEngine::BillboardPlacement> GuiEngine::placeBillboard(const BillboardGui& gui, const Camera& camera,
                                                                       Vector2 screen, float pixelScale,
                                                                       const Player* localPlayer)
{
    if (!gui.enabled() || screen.x <= 0.0f || screen.y <= 0.0f)
        return std::nullopt;
    if (localPlayer && gui.playerToHideFrom() == localPlayer)
        return std::nullopt;
    const auto anchor = billboardAnchor(gui);
    if (!anchor)
        return std::nullopt;
    const auto& [at, extents] = *anchor;
    // The world offsets along the world axes, StudsOffset and ExtentsOffset
    // along the camera's (x right, y up, z towards the camera).
    const CFrame view = camera.cframe();
    const Vector3 local = gui.studsOffset() + times(gui.extentsOffset(), extents);
    const Vector3 point = at + gui.studsOffsetWorldSpace() + times(gui.extentsOffsetWorldSpace(), extents)
                          + view.rightVector() * local.x + view.upVector() * local.y - view.lookVector() * local.z;
    const Vector3 c = view.pointToObjectSpace(point);
    const float depth = -c.z;
    if (depth < 0.1f || (point - view.position).magnitude() > gui.maxDistance())
        return std::nullopt;
    // Size: Scale in studs (a world size, so smaller farther away), Offset in pixels.
    const float tanHalf = std::tan(camera.fieldOfView() * std::numbers::pi_v<float> / 360.0f);
    const float pixelsPerStud = screen.y / (2.0f * depth * tanHalf);
    const float w = gui.size().x.scale * pixelsPerStud + static_cast<float>(gui.size().x.offset) * pixelScale;
    const float h = gui.size().y.scale * pixelsPerStud + static_cast<float>(gui.size().y.offset) * pixelScale;
    if (w <= 0.0f || h <= 0.0f)
        return std::nullopt;
    const float aspect = screen.x / screen.y;
    const float sx = (c.x / (depth * tanHalf * aspect) + 1.0f) * 0.5f * screen.x;
    const float sy = (1.0f - c.y / (depth * tanHalf)) * 0.5f * screen.y;
    // SizeOffset moves it by fractions of its size. GUESS: positive y is up.
    const Vector2 shift = gui.sizeOffset();
    return BillboardPlacement{{sx - w * 0.5f + shift.x * w, sy - h * 0.5f - shift.y * h, w, h}, depth, point};
}

void GuiEngine::update(Vector2 screen, double time, bool measureHidden)
{
    if (updating_)
        return;
    updating_ = true;
    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{updating_};
    drawnScreen_ = screen;
    geometryMeasured_ = false;
    std::vector<std::uint64_t> revisionAtStart;
    for (const auto& state : geometryWatches_)
        revisionAtStart.push_back(state->revision);
    time_ = time;
    nodes_.clear();
    const float scale = settings_.pixelScale;
    const float inset = settings_.topInset * scale;
    // What of the top bar the game may use: all but the health bar's corner
    // (CoreGui's, 160 wide). And what the app's own bars keep out.
    GuiService::Inset global;
    for (const auto& weak : roots_)
        if (const InstancePtr root = weak.lock()) {
            const Instance* top = root.get();
            while (top->parent())
                top = top->parent();
            if (const auto* game = instanceCast<const DataModel>(top))
                if (auto* service = instanceCast<GuiService>(game->findService("GuiService"))) {
                    service->assignTopbarInset(settings_.topbarSafeArea.value_or(
                        Rect{{0.0f, 0.0f}, {std::max(screen.x / scale - 160.0f, 0.0f), settings_.topInset}}));
                    global = service->globalGuiInset();
                }
            break;
        }

    struct Root {
        LayerCollector* gui;
        int order;
        std::size_t index;
    };
    coordinateOrigin_ = {global.left, settings_.topInset + global.top};
    std::vector<Root> guis;
    // A root's ScreenGuis, also those kept in Folders (gethui()'s is one): not
    // into another GUI, where a ScreenGui doesn't show.
    auto collect = [&](this auto& self, const Instance& parent) -> void {
        for (const InstancePtr& child : parent.children()) {
            if (auto* screenGui = instanceCast<ScreenGui>(child.get())) {
                if (screenGui->enabled() || measureHidden)
                    guis.push_back({screenGui, screenGui->displayOrder(), guis.size()});
            } else if (!instanceCast<GuiBase>(child.get())) {
                self(*child);
            }
        }
    };
    for (const auto& weak : roots_)
        if (auto root = weak.lock())
            collect(*root);
    std::ranges::stable_sort(guis, {}, &Root::order);

    const Node identity{};
    // The world's GUIs first, surfaces then billboards farthest first; the
    // ScreenGuis draw over them. Each is a layer the renderer puts in the
    // world, behind what's in front of it unless AlwaysOnTop.
    layers_.clear();
    surfaces_.clear();
    view_.reset();
    if (const InstancePtr world = worldGuis_ ? worldGuis_->world.lock() : nullptr) {
        auto* workspace = static_cast<Workspace*>(world.get());
        const Camera* camera = workspace->currentCamera();
        if (camera && screen.x > 0.0f && screen.y > 0.0f)
            view_
                = View{camera->cframe(), std::tan(camera->fieldOfView() * std::numbers::pi_v<float> / 360.0f), screen};
        const DataModel* game = instanceCast<const DataModel>(workspace->parent());
        const auto* players = game ? instanceCast<const Players>(game->findFirstChildOfClass("Players")) : nullptr;
        const Player* local = players ? players->localPlayer() : nullptr;
        const render::Lighting light = game ? render::lightingFrom(*game) : render::Lighting{};
        std::vector<Instance*> found;
        std::erase_if(worldGuis_->guis, [](const std::weak_ptr<Instance>& w) { return w.expired(); });
        for (const auto& weak : worldGuis_->guis)
            if (const InstancePtr gui = weak.lock(); gui && gui->isDescendantOf(workspace))
                found.push_back(gui.get());
        // Ones in the player's GUI show on their Adornee.
        for (const auto& set : rootGuis_) {
            std::erase_if(set->guis, [](const std::weak_ptr<Instance>& w) { return w.expired(); });
            for (const auto& weak : set->guis)
                if (const InstancePtr gui = weak.lock();
                    gui
                    && (instanceCast<const BillboardGui>(gui.get())
                            ? static_cast<const BillboardGui*>(gui.get())->adornee()
                            : static_cast<const SurfaceGui*>(gui.get())->adornee()))
                    found.push_back(gui.get());
        }

        auto addLayer
            = [&](LayerCollector* gui, const RectF& area, const RectF& clip, float layoutScale, WorldLayer layer) {
                  layers_.push_back(layer);
                  Frame frame;
                  frame.measureHidden = measureHidden;
                  Node worldTransform = identity;
                  worldTransform.layer = static_cast<std::uint32_t>(layers_.size());
                  layoutContainer(gui, area, clip, worldTransform, layoutScale, frame, 0);
                  if (!frame.grownSizes.empty()) {
                      frame.beginReflow();
                      layoutContainer(gui, area, clip, worldTransform, layoutScale, frame, 0);
                  }
                  if (gui->zIndexBehavior() == Enum::ZIndexBehavior::Global)
                      std::ranges::stable_sort(frame.nodes, {}, &Node::zIndex);
                  for (Node& node : frame.nodes) {
                      node.layer = static_cast<std::uint32_t>(layers_.size());
                      nodes_.push_back(std::move(node));
                  }
              };
        // Surfaces: laid out on their canvas, only seen from the front.
        for (Instance* instance : found) {
            auto* gui = instanceCast<SurfaceGui>(instance);
            const auto where = gui && camera ? placeSurface(*gui) : std::nullopt;
            if (!where || where->normal.dot(camera->cframe().position - where->origin) <= 0.0f)
                continue;
            gui->assignAbsolute({0.0f, 0.0f}, where->canvas, 0.0f);
            const RectF canvas{0.0f, 0.0f, where->canvas.x, where->canvas.y};
            addLayer(gui, canvas, gui->clipsDescendants() ? canvas : RectF{-1e9f, -1e9f, 2e9f, 2e9f}, 1.0f,
                     {WorldLayer::Kind::Surface, where->origin, where->axisX, where->axisY, !gui->alwaysOnTop(),
                      tintOf(light, where->normal, gui->brightness(), gui->lightInfluence())});
            surfaces_.push_back({static_cast<std::uint32_t>(layers_.size()),
                                 adorneePart(gui->adornee() ? gui->adornee() : gui->parent()), where->canvas,
                                 gui->active()});
        }
        // Billboards: in screen space, as deep as the point they hang from.
        struct Placed {
            BillboardGui* gui;
            BillboardPlacement where;
        };
        std::vector<Placed> placed;
        for (Instance* instance : found)
            if (auto* gui = instanceCast<BillboardGui>(instance); gui && camera)
                if (auto where = placeBillboard(*gui, *camera, screen, scale, local))
                    placed.push_back({gui, *where});
        std::ranges::stable_sort(placed, std::ranges::greater{}, [](const Placed& p) { return p.where.depth; });
        const RectF whole{0, 0, screen.x, screen.y};
        for (const Placed& p : placed) {
            const RectF& r = p.where.rect;
            p.gui->assignAbsolute({r.x / scale, r.y / scale}, {r.w / scale, r.h / scale}, 0.0f);
            addLayer(p.gui, r, p.gui->clipsDescendants() ? r.intersect(whole) : whole, scale,
                     {WorldLayer::Kind::Billboard,
                      p.where.point,
                      {},
                      {},
                      !p.gui->alwaysOnTop(),
                      tintOf(light, std::nullopt, p.gui->brightness(), p.gui->lightInfluence())});
        }
    }

    // Each ScreenGui as the last update laid it out, if nothing it's laid
    // out from has changed since: its instances (its count of changes,
    // GeometryWatch::layers), the screen, the settings, the global inset,
    // the TextBox being typed in (its placeholder goes), the page layouts'
    // slides. One laid out measuring the hidden ones serves an update that
    // doesn't, not the other way round. A tween in one ScreenGui leaves the
    // others as they are (the game's: its scripts read AbsolutePosition after
    // each, which lays out again).
    std::unordered_map<const ScreenGui*, ScreenLayout> kept;
    for (const Root& root : guis) {
        auto* screenGui = static_cast<ScreenGui*>(root.gui);
        const ScreenLayout key{layerRevision(screenGui), screen,  settings_.topInset,         settings_.pixelScale,
                               settings_.topbarSafeArea,  global,  focused_, UIPageLayout::slideSteps(),
                               measureHidden};
        if (auto it = screenLayouts_.find(screenGui); it != screenLayouts_.end() && it->second.sameAs(key)) {
            nodes_.insert(nodes_.end(), it->second.nodes.begin(), it->second.nodes.end());
            kept.emplace(screenGui, std::move(it->second));
            continue;
        }
        const std::size_t from = nodes_.size();
        // IgnoreGuiInset wins; else ScreenInsets, where a desktop's device
        // safe area is the whole screen and the top bar's is the strip of
        // it between its buttons. GUESS: that strip's ends (the classic top
        // bar's menu and chat buttons, its health bar). The core UI's
        // (the default) is under the top bar, less the global GUI inset.
        const float left = global.left * scale, top = inset + global.top * scale;
        RectF area{left, top, std::max(screen.x - left - global.right * scale, 0.0f),
                   std::max(screen.y - top - global.bottom * scale, 0.0f)};
        const Enum::ScreenInsets insets = screenGui->screenInsets();
        if (insets == Enum::ScreenInsets::None || insets == Enum::ScreenInsets::DeviceSafeInsets)
            area = {0, 0, screen.x, screen.y};
        else if (insets == Enum::ScreenInsets::TopbarSafeInsets) {
            if (settings_.topbarSafeArea) {
                const Rect& safe = *settings_.topbarSafeArea;
                area = {safe.min.x * scale, safe.min.y * scale, safe.width() * scale, safe.height() * scale};
            } else {
                area = {104.0f * scale, 0, std::max(screen.x - 274.0f * scale, 0.0f), inset};
            }
        }
        screenGui->assignAbsolute({area.x / scale - coordinateOrigin_.x, area.y / scale - coordinateOrigin_.y},
                                  {area.w / scale, area.h / scale}, 0.0f);
        Frame frame;
        frame.measureHidden = measureHidden;
        layoutContainer(screenGui, area, {0, 0, screen.x, screen.y}, identity, scale, frame, 0);
        if (!frame.grownSizes.empty()) {
            frame.beginReflow();
            layoutContainer(screenGui, area, {0, 0, screen.x, screen.y}, identity, scale, frame, 0);
        }
        if (screenGui->zIndexBehavior() == Enum::ZIndexBehavior::Global)
            std::ranges::stable_sort(frame.nodes, {}, &Node::zIndex);
        if (screenGui->enabled())
            for (Node& node : frame.nodes)
                nodes_.push_back(std::move(node));
        ScreenLayout& layout = kept.insert_or_assign(screenGui, key).first->second;
        layout.nodes.assign(nodes_.begin() + static_cast<std::ptrdiff_t>(from), nodes_.end());
    }
    // Those not shown this time go (a ScreenGui gone, or disabled while
    // this update doesn't measure the hidden).
    screenLayouts_ = std::move(kept);
    flushPublished();
    if (measureHidden) {
        measuredRevisions_ = std::move(revisionAtStart);
        geometryMeasured_ = true;
    }
}

void GuiEngine::publish(const Instance* object, Published kind, std::move_only_function<void()> assign)
{
    if (!updating_) {
        assign();
        return;
    }
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(object) * 4 + static_cast<std::uintptr_t>(kind);
    if (auto it = publishedIndex_.find(key); it != publishedIndex_.end()) {
        published_[it->second] = std::move(assign);
        return;
    }
    publishedIndex_.emplace(key, published_.size());
    published_.push_back(std::move(assign));
}

void GuiEngine::flushPublished()
{
    // Taken first: an assignment's Changed may run code that lays out again.
    auto pending = std::exchange(published_, {});
    publishedIndex_.clear();
    for (auto& assign : pending)
        assign();
}

Vector2 GuiEngine::autoSized(GuiObject* object, Vector2 size, float scale, int depth, bool grownX, bool grownY) const
{
    const Enum::AutomaticSize automatic = object->automaticSize();
    const bool autoX = grownX || automatic == Enum::AutomaticSize::X || automatic == Enum::AutomaticSize::XY;
    const bool autoY = grownY || automatic == Enum::AutomaticSize::Y || automatic == Enum::AutomaticSize::XY;
    if ((!autoX && !autoY) || depth > 64)
        return size;
    const UIPadding* padding = childOf<UIPadding>(object);
    const RectF rect{0.0f, 0.0f, size.x, size.y};
    const RectF content
        = textProps(object) ? inset(rect, padding, scale) : childContentRect(object, rect, padding, scale);
    const Vector2 pad{rect.w - content.w, rect.h - content.h};
    Vector2 extent{0.0f, 0.0f};
    if (const auto props = textProps(object)) {
        // As layoutObject: the text's bounds, unbounded along the growing
        // axes (but wrapped text no wider than its container).
        RectF probe = content;
        if (autoX)
            probe.w = props->wrapped ? std::max(content.w, wrapWidth_ - pad.x) : 1e6f;
        if (autoY)
            probe.h = 1e6f;
        extent = layoutTextOf(object, probe, scale)->bounds;
    }
    if (!instanceCast<ScrollingFrame>(object)) {
        // From the children too (a text object grows to both: Cascade's
        // buttons are an empty TextButton around their label): stacked by a
        // UIListLayout, else where they sit.
        const Vector2 inner{content.w, content.h};
        const auto* list = childOf<UIListLayout>(object);
        const bool vertical = list && list->fillDirection() == Enum::FillDirection::Vertical;
        float along = 0.0f;
        int count = 0;
        for (GuiObject* child : guiChildren(object)) {
            float childScale = scale;
            // A child that doesn't grow itself still counts what overflows it
            // along the axes this one grows (a carousel at 100% height whose
            // auto-sized rows are taller).
            const Vector2 s
                = autoSized(child, childSize(child, inner, scale, childScale), childScale, depth + 1, autoX, autoY);
            // The list stacks siblings; what a Folder holds counts where it sits.
            if (list && child->parent() == object) {
                along += vertical ? s.y : s.x;
                extent = vertical ? Vector2{std::max(extent.x, s.x), extent.y}
                                  : Vector2{extent.x, std::max(extent.y, s.y)};
                ++count;
                continue;
            }
            const Vector2 anchor = child->anchorPoint();
            extent.x = std::max(extent.x, resolveScaled(child->position().x, inner.x, scale) + (1.0f - anchor.x) * s.x);
            extent.y = std::max(extent.y, resolveScaled(child->position().y, inner.y, scale) + (1.0f - anchor.y) * s.y);
        }
        if (list && count > 0) {
            along
                += resolveScaled(list->padding(), vertical ? inner.y : inner.x, scale) * static_cast<float>(count - 1);
            if (vertical)
                extent.y = std::max(extent.y, along);
            else
                extent.x = std::max(extent.x, along);
        }
    }
    if (autoX)
        size.x = std::max(size.x, extent.x + pad.x);
    if (autoY)
        size.y = std::max(size.y, extent.y + pad.y);
    return size;
}

void GuiEngine::layoutContainer(Instance* container, const RectF& content, const RectF& clip, const Node& transform,
                                float scale, Frame& frame, int depth)
{
    if (depth > 64)
        return;
    // How wide auto-sized wrapped text in here may grow (autoSized, layoutObject).
    struct Width {
        float& value;
        float kept;
        ~Width() { value = kept; }
    } width{wrapWidth_, wrapWidth_};
    wrapWidth_ = content.w;
    std::vector<GuiObject*> children = guiChildren(container);
    if (frame.measureHidden) {
        auto hidden = [&](this auto& collect, Instance& holder, bool prototype, int levels) -> void {
            if (levels > 64)
                return;
            for (const auto& child : holder.children()) {
                if (auto* object = instanceCast<GuiObject>(child.get())) {
                    if (!prototype && object->visible())
                        continue;
                    float childScale = scale;
                    const auto size = childSize(object, {content.w, content.h}, scale, childScale);
                    const auto anchor = object->anchorPoint();
                    const float x
                        = content.x + resolveScaled(object->position().x, content.w, scale) - anchor.x * size.x;
                    const float y
                        = content.y + resolveScaled(object->position().y, content.h, scale) - anchor.y * size.y;
                    Frame measurement;
                    measurement.measureHidden = true;
                    layoutObject(object, {x, y, size.x, size.y}, {}, transform, childScale, measurement, depth);
                    if (!measurement.grownSizes.empty()) {
                        measurement.beginReflow();
                        layoutObject(object, {x, y, size.x, size.y}, {}, transform, childScale, measurement, depth);
                    }
                } else {
                    collect(*child, prototype || !child->isA("Folder"), levels + 1);
                }
            }
        };
        hidden(*container, false, 0);
    }
    if (children.empty()) {
        if (auto* layout = childOf<UIGridStyleLayout>(container))
            publish(layout, Published::ContentSize, [layout] { layout->assignContentSize({}); });
        if (auto* grid = childOf<UIGridLayout>(container))
            publish(grid, Published::Cells,
                    [grid, size = Vector2{resolveScaled(grid->cellSize().x, content.w, scale) / settings_.pixelScale,
                                          resolveScaled(grid->cellSize().y, content.h, scale) / settings_.pixelScale}] {
                        grid->assignCells(size, {});
                    });
        return;
    }
    auto* list = childOf<UIListLayout>(container);
    auto* grid = list ? nullptr : childOf<UIGridLayout>(container);
    auto* pages = list || grid ? nullptr : childOf<UIPageLayout>(container);
    const Vector2 parentSize{content.w, content.h};

    auto sizeOf = [&](GuiObject* child, float& childScale) {
        Vector2 size = childSize(child, parentSize, scale, childScale);
        if (const auto it = frame.grownSizes.find(child); it != frame.grownSizes.end()) {
            size.x = std::max(size.x, it->second.x);
            size.y = std::max(size.y, it->second.y);
        }
        return size;
    };

    struct Placed {
        GuiObject* object;
        RectF rect;
        float scale;
    };
    // Where Position and AnchorPoint put a child.
    auto atPosition = [&](GuiObject* child) {
        float childScale = scale;
        const Vector2 size = sizeOf(child, childScale);
        const Vector2 anchor = child->anchorPoint();
        const float x = content.x + resolveScaled(child->position().x, content.w, scale) - anchor.x * size.x;
        const float y = content.y + resolveScaled(child->position().y, content.h, scale) - anchor.y * size.y;
        return Placed{child, {x, y, size.x, size.y}, childScale};
    };

    if (!list && !grid && !pages) {
        sortByZ(children);
        for (GuiObject* child : children) {
            const Placed p = atPosition(child);
            layoutObject(child, p.rect, clip, transform, p.scale, frame, depth);
        }
        return;
    }

    // Layouts place children themselves; Position (and AnchorPoint) are
    // ignored. Only their siblings: what a Folder holds sits where its
    // Position says (Cascade keeps a window's resize handles out of its
    // row that way, in Folders named LayoutIgnore).
    std::vector<GuiObject*> arranged;
    std::vector<Placed> loose;
    for (GuiObject* child : children) {
        if (child->parent() == container)
            arranged.push_back(child);
        else
            loose.push_back(atPosition(child));
    }
    std::vector<Placed> placed;

    if (list) {
        // Worked along the list (main) and across it (cross), then turned
        // into rects. Lines: one, or as many as Wraps needs.
        const bool vertical = list->fillDirection() == Enum::FillDirection::Vertical;
        const float mainLength = vertical ? content.h : content.w;
        const float crossLength = vertical ? content.w : content.h;
        const float padding = resolveScaled(list->padding(), mainLength, scale);
        // Between wrapped lines: the cross axis's own padding, else Padding.
        const UDim crossUDim = vertical ? list->horizontalPadding() : list->verticalPadding();
        const float crossPadding = crossUDim.scale != 0.0f || crossUDim.offset != 0
                                       ? resolveScaled(crossUDim, crossLength, scale)
                                       : resolveScaled(list->padding(), crossLength, scale);
        const Enum::UIFlexAlignment mainFlex = vertical ? list->verticalFlex() : list->horizontalFlex();
        const Enum::UIFlexAlignment crossFlex = vertical ? list->horizontalFlex() : list->verticalFlex();
        // Where the free space goes without flex: the alignment along each axis, as 0, 0.5 or 1.
        auto fraction = [&](bool alongX) {
            if (alongX)
                return list->horizontalAlignment() == Enum::HorizontalAlignment::Center  ? 0.5f
                       : list->horizontalAlignment() == Enum::HorizontalAlignment::Right ? 1.0f
                                                                                         : 0.0f;
            return list->verticalAlignment() == Enum::VerticalAlignment::Center   ? 0.5f
                   : list->verticalAlignment() == Enum::VerticalAlignment::Bottom ? 1.0f
                                                                                  : 0.0f;
        };
        const float mainAlign = fraction(!vertical);
        const float crossAlign = fraction(vertical);

        struct Item {
            GuiObject* object;
            float main;
            float cross;
            float scale;
            const UIFlexItem* flex;
            float at = 0.0f; // along the line
        };
        std::vector<Item> items;
        for (GuiObject* child : sortForLayout(arranged, list->sortOrder())) {
            float childScale = scale;
            // Stacked by the size AutomaticSize grows it to, as the client's
            // layout reads AbsoluteSize: wrapped text with AutomaticSize Y
            // otherwise lay on top of each other (JJS' rules).
            const Vector2 size = autoSized(child, sizeOf(child, childScale), childScale, depth + 1);
            items.push_back({child, vertical ? size.y : size.x, vertical ? size.x : size.y, childScale,
                             childOf<UIFlexItem>(child)});
        }
        struct Line {
            std::size_t first;
            std::size_t count;
            float thickness = 0.0f;
            float at = 0.0f; // across the list
        };
        std::vector<Line> lines;
        for (std::size_t i = 0; i < items.size();) {
            Line line{i, 0};
            float used = 0.0f;
            while (i < items.size()) {
                const float next = used + (line.count > 0 ? padding : 0.0f) + items[i].main;
                if (list->wraps() && line.count > 0 && next > mainLength + 0.01f)
                    break;
                used = next;
                ++line.count;
                ++i;
            }
            lines.push_back(line);
        }

        // Along each line: flex items grow into the free space or shrink out
        // of too little, then the rest of it is spread as the flex says.
        for (Line& line : lines) {
            const std::span<Item> in(items.data() + line.first, line.count);
            float used = padding * static_cast<float>(line.count - 1);
            for (const Item& item : in)
                used += item.main;
            float free = mainLength - used;
            auto weight = [&](const Item& item, bool grow) {
                const Enum::UIFlexMode mode = item.flex ? item.flex->flexMode() : Enum::UIFlexMode::None;
                if (mode == Enum::UIFlexMode::Custom)
                    return std::max(grow ? item.flex->growRatio() : item.flex->shrinkRatio(), 0.0f);
                if (mode == Enum::UIFlexMode::Fill
                    || (mode == Enum::UIFlexMode::None && mainFlex == Enum::UIFlexAlignment::Fill))
                    return 1.0f;
                return mode == (grow ? Enum::UIFlexMode::Grow : Enum::UIFlexMode::Shrink) ? 1.0f : 0.0f;
            };
            if (free > 0.0f) {
                float total = 0.0f;
                for (const Item& item : in)
                    total += weight(item, true);
                if (total > 0.0f) {
                    for (Item& item : in)
                        item.main += free * weight(item, true) / total;
                    free = 0.0f;
                }
            } else if (free < 0.0f) {
                // Shrinking takes from each in proportion to its size too, as CSS does.
                float total = 0.0f;
                for (const Item& item : in)
                    total += weight(item, false) * item.main;
                if (total > 0.0f) {
                    const float take = std::min(-free, total);
                    for (Item& item : in)
                        item.main = std::max(item.main - take * weight(item, false) * item.main / total, 0.0f);
                    free += take;
                }
            }
            float cursor = 0.0f;
            float gap = padding;
            const auto n = static_cast<float>(line.count);
            if (free > 0.0f && mainFlex == Enum::UIFlexAlignment::SpaceBetween && line.count > 1) {
                gap += free / (n - 1.0f);
            } else if (free > 0.0f && mainFlex == Enum::UIFlexAlignment::SpaceAround) {
                cursor = free / (2.0f * n);
                gap += free / n;
            } else if (free > 0.0f && mainFlex == Enum::UIFlexAlignment::SpaceEvenly) {
                cursor = free / (n + 1.0f);
                gap += free / (n + 1.0f);
            } else {
                cursor = free * mainAlign;
            }
            for (Item& item : in) {
                item.at = cursor;
                cursor += item.main + gap;
                line.thickness = std::max(line.thickness, item.cross);
            }
        }

        // Across: one line spans the whole list; wrapped lines stack, spread
        // as the cross axis's flex says or aligned as a block.
        if (lines.empty()) {
        } else if (!list->wraps()) {
            lines.front().thickness = crossLength;
        } else {
            float used = crossPadding * static_cast<float>(lines.size() - 1);
            for (const Line& line : lines)
                used += line.thickness;
            float free = crossLength - used;
            const auto n = static_cast<float>(lines.size());
            float cursor = 0.0f;
            float gap = crossPadding;
            if (free > 0.0f && crossFlex == Enum::UIFlexAlignment::Fill) {
                for (Line& line : lines)
                    line.thickness += free / n;
            } else if (free > 0.0f && crossFlex == Enum::UIFlexAlignment::SpaceBetween && lines.size() > 1) {
                gap += free / (n - 1.0f);
            } else if (free > 0.0f && crossFlex == Enum::UIFlexAlignment::SpaceAround) {
                cursor = free / (2.0f * n);
                gap += free / n;
            } else if (free > 0.0f && crossFlex == Enum::UIFlexAlignment::SpaceEvenly) {
                cursor = free / (n + 1.0f);
                gap += free / (n + 1.0f);
            } else {
                cursor = free * crossAlign;
            }
            for (Line& line : lines) {
                line.at = cursor;
                cursor += line.thickness + gap;
            }
        }

        // Each item within its line: ItemLineAlignment (the item's own
        // first), Automatic following the list's alignment across it.
        for (const Line& line : lines)
            for (std::size_t i = line.first; i < line.first + line.count; ++i) {
                Item& item = items[i];
                Enum::ItemLineAlignment align
                    = item.flex ? item.flex->itemLineAlignment() : Enum::ItemLineAlignment::Automatic;
                if (align == Enum::ItemLineAlignment::Automatic)
                    align = list->itemLineAlignment();
                float offset = (line.thickness - item.cross) * crossAlign;
                if (align == Enum::ItemLineAlignment::Start)
                    offset = 0.0f;
                else if (align == Enum::ItemLineAlignment::Center)
                    offset = (line.thickness - item.cross) * 0.5f;
                else if (align == Enum::ItemLineAlignment::End)
                    offset = line.thickness - item.cross;
                else if (align == Enum::ItemLineAlignment::Stretch) {
                    offset = 0.0f;
                    item.cross = line.thickness;
                }
                const float across = line.at + offset;
                placed.push_back({item.object,
                                  vertical ? RectF{content.x + across, content.y + item.at, item.cross, item.main}
                                           : RectF{content.x + item.at, content.y + across, item.main, item.cross},
                                  item.scale});
            }
    } else if (pages) {
        // In a row (or column), each page at its own size with Padding
        // between: the row shifted so the current page sits where the
        // alignments say, part of the way from the last one while they slide.
        const bool vertical = pages->fillDirection() == Enum::FillDirection::Vertical;
        const float mainLength = vertical ? content.h : content.w;
        const float crossLength = vertical ? content.w : content.h;
        const float padding = resolveScaled(pages->padding(), mainLength, scale);
        const float alignX = pages->horizontalAlignment() == Enum::HorizontalAlignment::Left     ? 0.0f
                             : pages->horizontalAlignment() == Enum::HorizontalAlignment::Center ? 0.5f
                                                                                                 : 1.0f;
        const float alignY = pages->verticalAlignment() == Enum::VerticalAlignment::Top      ? 0.0f
                             : pages->verticalAlignment() == Enum::VerticalAlignment::Center ? 0.5f
                                                                                             : 1.0f;
        const float mainAlign = vertical ? alignY : alignX;
        const float crossAlign = vertical ? alignX : alignY;
        struct Page {
            GuiObject* object;
            Vector2 size;
            float scale;
            float at; // along the row
        };
        std::vector<Page> row;
        float cursor = 0.0f;
        for (GuiObject* child : sortForLayout(arranged, pages->sortOrder())) {
            float childScale = scale;
            const Vector2 size = sizeOf(child, childScale);
            row.push_back({child, size, childScale, cursor});
            cursor += (vertical ? size.y : size.x) + padding;
        }
        // Where the row starts for `page` to be the one in view.
        auto shiftFor = [&](const Instance* page) -> std::optional<float> {
            for (const Page& at : row)
                if (at.object == page)
                    return mainAlign * (mainLength - (vertical ? at.size.y : at.size.x)) - at.at;
            return std::nullopt;
        };
        float shift = shiftFor(pages->currentPage()).value_or(0.0f);
        // Eased, so Back overshoots past 1 before it settles.
        const UIPageLayout::Slide& slide = pages->slide();
        if (const std::optional<float> from = shiftFor(slide.from.lock().get()))
            shift = *from + (shift - *from) * slide.progress;
        for (const Page& at : row) {
            const float across = crossAlign * (crossLength - (vertical ? at.size.x : at.size.y));
            placed.push_back({at.object,
                              vertical ? RectF{content.x + across, content.y + shift + at.at, at.size.x, at.size.y}
                                       : RectF{content.x + shift + at.at, content.y + across, at.size.x, at.size.y},
                              at.scale});
        }
    } else {
        const auto ordered = sortForLayout(arranged, grid->sortOrder());
        const float cellW = resolveScaled(grid->cellSize().x, content.w, scale);
        const float cellH = resolveScaled(grid->cellSize().y, content.h, scale);
        const float padX = resolveScaled(grid->cellPadding().x, content.w, scale);
        const float padY = resolveScaled(grid->cellPadding().y, content.h, scale);
        const bool horizontal = grid->fillDirection() == Enum::FillDirection::Horizontal;
        // Cells per row (or column): as many as fit, at least one, at most FillDirectionMaxCells.
        const float fit = horizontal ? std::floor((content.w + padX) / std::max(cellW + padX, 1e-3f))
                                     : std::floor((content.h + padY) / std::max(cellH + padY, 1e-3f));
        int perLine = std::isfinite(fit) ? static_cast<int>(std::clamp(fit, 1.0f, 1'000'000.0f)) : 1;
        if (grid->fillDirectionMaxCells() > 0)
            perLine = std::min(perLine, grid->fillDirectionMaxCells());
        // A container with AutomaticSize along the fill direction grows to
        // the cells instead of wrapping them at its size, up to
        // FillDirectionMaxCells: the game's sidebar (a scale-sized
        // frame of AutomaticSize XY) shows its two columns in any window.
        if (const auto* sized = instanceCast<GuiObject>(container); sized && grid->fillDirectionMaxCells() > 0) {
            const Enum::AutomaticSize automatic = sized->automaticSize();
            const bool grows = automatic == Enum::AutomaticSize::XY
                               || automatic == (horizontal ? Enum::AutomaticSize::X : Enum::AutomaticSize::Y);
            if (grows)
                perLine = grid->fillDirectionMaxCells();
        }
        struct Cell {
            GuiObject* object;
            std::int64_t main;
            std::int64_t cross;
            int mainSpan;
            int crossSpan;
            Vector2 size;
            float scale;
        };
        std::vector<Cell> cells;
        struct Reserved {
            std::int64_t main0, cross0, main1, cross1;
        };
        std::vector<Reserved> occupied;
        std::int64_t cursor = 0, maxMain = 0, maxCross = 0;
        const auto span = [](float size, float cell, float padding) {
            const float value = std::ceil((size + padding) / std::max(cell + padding, 1e-3f));
            return std::isfinite(value) ? static_cast<int>(std::clamp(value, 1.0f, 1'000'000.0f)) : 1;
        };
        for (GuiObject* child : ordered) {
            Vector2 size{cellW, cellH};
            if (const auto* limit = childOf<UISizeConstraint>(child)) {
                size.x = std::clamp(size.x, limit->minSize().x * scale,
                                    std::max(limit->minSize().x, limit->maxSize().x) * scale);
                size.y = std::clamp(size.y, limit->minSize().y * scale,
                                    std::max(limit->minSize().y, limit->maxSize().y) * scale);
            }
            if (const auto* aspect = childOf<UIAspectRatioConstraint>(child))
                size = applyAspectRatio(size, aspect->aspectRatio(), aspect->aspectType(), aspect->dominantAxis());
            const int spanMain = horizontal ? span(size.x, cellW, padX) : span(size.y, cellH, padY);
            const int spanCross = horizontal ? span(size.y, cellH, padY) : span(size.x, cellW, padX);
            std::int64_t main = 0, cross = 0;
            for (;;) {
                main = cursor % perLine;
                cross = cursor / perLine;
                if ((spanMain <= perLine && main + spanMain > perLine) || (spanMain > perLine && main != 0)) {
                    cursor = (cross + 1) * perLine;
                    continue;
                }
                const auto collision = std::ranges::find_if(occupied, [&](const Reserved& box) {
                    return main < box.main1 && main + spanMain > box.main0 && cross < box.cross1
                           && cross + spanCross > box.cross0;
                });
                if (collision == occupied.end())
                    break;
                cursor = collision->main0 == 0 && collision->main1 >= perLine ? collision->cross1 * perLine
                         : collision->main1 >= perLine                        ? (cross + 1) * perLine
                                                                              : cross * perLine + collision->main1;
            }
            if (spanMain > 1 || spanCross > 1)
                occupied.push_back({main, cross, main + spanMain, cross + spanCross});
            maxMain = std::max(maxMain, main + spanMain);
            maxCross = std::max(maxCross, cross + spanCross);
            cursor += std::min(spanMain, perLine);
            float ownScale = 1;
            if (const auto* uiScale = childOf<UIScale>(child))
                ownScale = uiScale->scale();
            cells.push_back({child, main, cross, spanMain, spanCross, size * ownScale, scale * ownScale});
        }
        const auto columns = horizontal ? maxMain : maxCross;
        const auto rows = horizontal ? maxCross : maxMain;
        publish(grid, Published::Cells,
                [grid, size = Vector2{cellW / settings_.pixelScale, cellH / settings_.pixelScale},
                 count = Vector2{static_cast<float>(columns), static_cast<float>(rows)}] { grid->assignCells(size, count); });
        const float blockW
            = static_cast<float>(columns) * cellW + static_cast<float>(std::max<std::int64_t>(columns - 1, 0)) * padX;
        const float blockH
            = static_cast<float>(rows) * cellH + static_cast<float>(std::max<std::int64_t>(rows - 1, 0)) * padY;
        float originX = content.x, originY = content.y;
        if (grid->horizontalAlignment() == Enum::HorizontalAlignment::Center)
            originX += (content.w - blockW) / 2;
        else if (grid->horizontalAlignment() == Enum::HorizontalAlignment::Right)
            originX += content.w - blockW;
        if (grid->verticalAlignment() == Enum::VerticalAlignment::Center)
            originY += (content.h - blockH) / 2;
        else if (grid->verticalAlignment() == Enum::VerticalAlignment::Bottom)
            originY += content.h - blockH;
        const bool fromRight = grid->startCorner() == Enum::StartCorner::TopRight
                               || grid->startCorner() == Enum::StartCorner::BottomRight;
        const bool fromBottom = grid->startCorner() == Enum::StartCorner::BottomLeft
                                || grid->startCorner() == Enum::StartCorner::BottomRight;
        for (const auto& cell : cells) {
            auto col = horizontal ? cell.main : cell.cross;
            auto row = horizontal ? cell.cross : cell.main;
            const int width = horizontal ? cell.mainSpan : cell.crossSpan;
            const int height = horizontal ? cell.crossSpan : cell.mainSpan;
            if (fromRight)
                col = columns - col - width;
            if (fromBottom)
                row = rows - row - height;
            placed.push_back({cell.object,
                              {originX + static_cast<float>(col) * (cellW + padX),
                               originY + static_cast<float>(row) * (cellH + padY), cell.size.x, cell.size.y},
                              cell.scale});
        }
    }
    // The items' extent, for scripts sizing a canvas by it.
    {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!placed.empty()) {
            x0 = x1 = placed.front().rect.x;
            y0 = y1 = placed.front().rect.y;
        }
        for (const Placed& p : placed) {
            x0 = std::min(x0, p.rect.x);
            y0 = std::min(y0, p.rect.y);
            x1 = std::max(x1, p.rect.x + p.rect.w);
            y1 = std::max(y1, p.rect.y + p.rect.h);
        }
        UIGridStyleLayout* layout = list   ? static_cast<UIGridStyleLayout*>(list)
                                    : grid ? static_cast<UIGridStyleLayout*>(grid)
                                           : static_cast<UIGridStyleLayout*>(pages);
        const float pixel = settings_.pixelScale;
        publish(layout, Published::ContentSize,
                [layout, size = Vector2{(x1 - x0) / pixel, (y1 - y0) / pixel}] { layout->assignContentSize(size); });
    }
    placed.insert(placed.end(), loose.begin(), loose.end());
    // LayoutOrder chooses placement, not paint order. Equal-ZIndex siblings
    // retain their hierarchy order, just like objects without a layout.
    // Sorting the placed vector would instead keep the layout's ordering.
    std::unordered_map<const GuiObject*, const Placed*> positions;
    positions.reserve(placed.size());
    for (const Placed& p : placed)
        positions.emplace(p.object, &p);
    sortByZ(children);
    for (const GuiObject* child : children)
        if (const auto found = positions.find(child); found != positions.end()) {
            const Placed& p = *found->second;
            layoutObject(p.object, p.rect, clip, transform, p.scale, frame, depth);
        }
}

void GuiEngine::layoutObject(GuiObject* object, const RectF& rectIn, const RectF& clip, const Node& parent, float scale,
                             Frame& frame, int depth)
{
    // A negative Size reaches back from where the object starts: it is drawn
    // and its children laid out flipped, while AbsolutePosition and
    // AbsoluteSize keep the sign [Studio: a {1, 0}, {-0.5, 0} frame at a 60
    // pixel slot's bottom reads 60, -30 at the bottom]. The game's cooldowns
    // shrink that way from a slot's bottom.
    RectF rect = rectIn;
    const bool flipX = rect.w < 0.0f, flipY = rect.h < 0.0f;
    if (flipX) {
        rect.x += rect.w;
        rect.w = -rect.w;
    }
    if (flipY) {
        rect.y += rect.h;
        rect.h = -rect.h;
    }
    if (const auto it = frame.grownSizes.find(object); it != frame.grownSizes.end()) {
        const float width = std::max(rect.w, it->second.x);
        const float height = std::max(rect.h, it->second.y);
        const Vector2 anchor = object->anchorPoint();
        rect.x -= anchor.x * (width - rect.w);
        rect.y -= anchor.y * (height - rect.h);
        rect.w = width;
        rect.h = height;
    }
    const std::size_t first = frame.nodes.size();
    const UIPadding* padding = childOf<UIPadding>(object);

    // AutomaticSize from text: grow to the text's bounds plus padding.
    const Enum::AutomaticSize automatic = frame.growing == object ? Enum::AutomaticSize::None : object->automaticSize();
    const bool autoX = automatic == Enum::AutomaticSize::X || automatic == Enum::AutomaticSize::XY;
    const bool autoY = automatic == Enum::AutomaticSize::Y || automatic == Enum::AutomaticSize::XY;
    // Text with GUI children inside grows to both; autoSized measures them together.
    const bool textAlone = textProps(object) && guiChildren(object).empty();
    if ((autoX || autoY) && textAlone) {
        RectF probe = inset(rect, padding, scale);
        const float padW = rect.w - probe.w;
        // Wrapped text grows only as wide as its container, then wraps
        // (the game's item descriptions, a label in a wrapping list).
        if (autoX)
            probe.w = textProps(object)->wrapped ? std::max(probe.w, wrapWidth_ - padW) : 1e6f;
        if (autoY)
            probe.h = 1e6f;
        const auto text = layoutTextOf(object, probe, scale);
        const float padH = rect.h - inset(rect, padding, scale).h;
        const Vector2 anchor = object->anchorPoint();
        if (autoX && text->bounds.x + padW > rect.w) {
            const float grow = text->bounds.x + padW - rect.w;
            rect.x -= anchor.x * grow;
            rect.w += grow;
        }
        if (autoY && text->bounds.y + padH > rect.h) {
            const float grow = text->bounds.y + padH - rect.h;
            rect.y -= anchor.y * grow;
            rect.h += grow;
        }
    } else if ((autoX || autoY) && !instanceCast<ScrollingFrame>(object) && !childOf<UIGridLayout>(object)
               && !childOf<UIPageLayout>(object)) {
        // Measure children before placing them, including frames without a
        // list layout. Otherwise nested auto-sized wrappers each lay out
        // their entire subtree twice. Measured after, children a row
        // aligns to its bottom (or right) sit above a row that hasn't grown
        // yet and never count (the Lua app's game page header).
        const Vector2 grown = autoSized(object, {rect.w, rect.h}, scale, depth);
        const Vector2 anchor = object->anchorPoint();
        rect.x -= anchor.x * (grown.x - rect.w);
        rect.y -= anchor.y * (grown.y - rect.h);
        rect.w = grown.x;
        rect.h = grown.y;
    }

    Node node;
    node.object = object;
    node.layer = parent.layer;
    node.rect = rect;
    node.clip = clip;
    node.scale = scale;
    node.zIndex = object->zIndex();
    node.sequence = frame.sequence++;
    node.cosA = parent.cosA;
    node.sinA = parent.sinA;
    node.tx = parent.tx;
    node.ty = parent.ty;
    // A CanvasGroup tints and fades itself and everything in it, as one
    // picture. GUESS: per quad, so overlaps inside it show through.
    node.groupTint = parent.groupTint;
    node.groupOpacity = parent.groupOpacity;
    if (const auto* group = instanceCast<const CanvasGroup>(object)) {
        const Color3 c = group->groupColor3();
        node.groupTint = {node.groupTint.r * c.r, node.groupTint.g * c.g, node.groupTint.b * c.b};
        node.groupOpacity *= 1.0f - std::clamp(group->groupTransparency(), 0.0f, 1.0f);
    }
    if (const float rotation = object->rotation(); rotation != 0.0f) {
        // Rotate about the centre, then apply the parent's transform.
        const float c = std::cos(rotation * degrees);
        const float s = std::sin(rotation * degrees);
        const float cx = rect.x + rect.w / 2.0f;
        const float cy = rect.y + rect.h / 2.0f;
        const float lx = cx - (c * cx - s * cy);
        const float ly = cy - (s * cx + c * cy);
        node.cosA = parent.cosA * c - parent.sinA * s;
        node.sinA = parent.sinA * c + parent.cosA * s;
        node.tx = parent.cosA * lx - parent.sinA * ly + parent.tx;
        node.ty = parent.sinA * lx + parent.cosA * ly + parent.ty;
    }
    node.textRect = inset(rect, padding, scale);
    node.text = layoutTextOf(object, node.textRect, scale);
    if (node.text)
        publish(object, Published::TextResults,
                [object, bounds = Vector2{node.text->bounds.x / scale, node.text->bounds.y / scale},
                 fits = node.text->fits] { assignTextResults(object, bounds, fits); });
    const float pixel = settings_.pixelScale;
    const RectF signedRect{flipX ? rect.x + rect.w : rect.x, flipY ? rect.y + rect.h : rect.y, flipX ? -rect.w : rect.w,
                           flipY ? -rect.h : rect.h};
    const Vector2 origin = node.layer == 0 ? coordinateOrigin_ : Vector2{};
    publish(object, Published::Absolute,
            [object, position = Vector2{signedRect.x / pixel - origin.x, signedRect.y / pixel - origin.y},
             size = Vector2{signedRect.w / pixel, signedRect.h / pixel},
             rotation = std::atan2(node.sinA, node.cosA) / degrees] { object->assignAbsolute(position, size, rotation); });
    frame.nodes.push_back(node);

    const bool rotated = node.sinA != 0.0f;
    RectF content = childContentRect(object, rect, padding, scale);
    RectF childClip = clip;
    auto* scrolling = instanceCast<ScrollingFrame>(object);
    if ((object->clipsDescendants() || scrolling) && !rotated)
        childClip = clip.intersect(rect);

    if (scrolling) {
        // The canvas: CanvasSize against the window, moved by CanvasPosition.
        const float bar = static_cast<float>(scrolling->scrollBarThickness()) * scale;
        Vector2 window{rect.w, rect.h};
        Vector2 canvas{resolveScaled(scrolling->canvasSize().x, window.x, scale),
                       resolveScaled(scrolling->canvasSize().y, window.y, scale)};
        if (canvas.y > window.y && scrolling->verticalScrollBarInset() != Enum::ScrollBarInset::None)
            window.x -= bar;
        if (canvas.x > window.x && scrolling->horizontalScrollBarInset() != Enum::ScrollBarInset::None)
            window.y -= bar;
        // The canvas is never smaller than the window: a CanvasSize of 0 with
        // AutomaticCanvasSize (the usual scrolling list) still gives its
        // children the frame's width to fill.
        canvas = {std::max(canvas.x, window.x), std::max(canvas.y, window.y)};
        const Enum::AutomaticSize autoCanvas = scrolling->automaticCanvasSize();
        const Vector2 position = scrolling->canvasPosition();
        RectF canvasRect{rect.x - position.x * scale, rect.y - position.y * scale, canvas.x, canvas.y};
        const std::size_t before = frame.nodes.size();
        layoutContainer(object, inset(canvasRect, padding, scale), childClip, node, scale, frame, depth + 1);
        if (autoCanvas != Enum::AutomaticSize::None) {
            float right = canvasRect.x;
            float bottom = canvasRect.y;
            for (std::size_t i = before; i < frame.nodes.size(); ++i) {
                right = std::max(right, frame.nodes[i].rect.right());
                bottom = std::max(bottom, frame.nodes[i].rect.bottom());
            }
            const float padRight = padding ? resolveScaled(padding->paddingRight(), canvas.x, scale) : 0.0f;
            const float padBottom = padding ? resolveScaled(padding->paddingBottom(), canvas.y, scale) : 0.0f;
            if (autoCanvas != Enum::AutomaticSize::Y)
                canvas.x = std::max(canvas.x, right + padRight - canvasRect.x);
            if (autoCanvas != Enum::AutomaticSize::X)
                canvas.y = std::max(canvas.y, bottom + padBottom - canvasRect.y);
        }
        // Keep CanvasPosition inside the canvas.
        const Vector2 maxPosition{std::max((canvas.x - window.x) / scale, 0.0f),
                                  std::max((canvas.y - window.y) / scale, 0.0f)};
        const Vector2 clamped{std::clamp(position.x, 0.0f, maxPosition.x), std::clamp(position.y, 0.0f, maxPosition.y)};
        if (clamped.x != position.x || clamped.y != position.y)
            scrolling->setCanvasPosition(clamped);
        scrolling->assignCanvas({canvas.x / pixel, canvas.y / pixel}, {window.x / pixel, window.y / pixel});
        if (canvas.y > window.y + 0.5f || canvas.x > window.x + 0.5f) {
            Node scrollbar = node;
            scrollbar.scrollbarOf = scrolling;
            scrollbar.text = nullptr;
            scrollbar.zIndex = INT_MAX; // above the canvas, also with Global ZIndexBehavior
            scrollbar.sequence = frame.sequence++;
            frame.nodes.push_back(scrollbar);
        }
    } else {
        layoutContainer(object, content, childClip, node, scale, frame, depth + 1);
    }

    // AutomaticSize from children: grow to their extent and lay out again.
    if (!frame.reflowing && (autoX || autoY) && !textAlone && !scrolling && frame.nodes.size() > first + 1) {
        float right = rect.x;
        float bottom = rect.y;
        for (std::size_t i = first + 1; i < frame.nodes.size(); ++i) {
            right = std::max(right, frame.nodes[i].rect.right());
            bottom = std::max(bottom, frame.nodes[i].rect.bottom());
        }
        const float padRight = rect.right() - content.right();
        const float padBottom = rect.bottom() - content.bottom();
        RectF grown = rect;
        if (autoX)
            grown.w = std::max(rect.w, right + padRight - rect.x);
        if (autoY)
            grown.h = std::max(rect.h, bottom + padBottom - rect.y);
        if (grown.w > rect.w + 0.5f || grown.h > rect.h + 0.5f) {
            const Vector2 anchor = object->anchorPoint();
            grown.x -= anchor.x * (grown.w - rect.w);
            grown.y -= anchor.y * (grown.h - rect.h);
            frame.nodes.resize(first);
            // One more pass with the grown size; children sized by scale may grow it again, which Roblox
            // also doesn't chase.
            const GuiObject* saved = frame.growing;
            frame.grownSizes[object] = {grown.w, grown.h};
            frame.growing = object;
            // Descendants already measured their own growth. Reuse those
            // sizes during this pass instead of recursively reflowing each
            // subtree again at every ancestor.
            frame.reflowing = true;
            layoutObject(object, grown, clip, parent, scale, frame, depth);
            frame.reflowing = false;
            frame.growing = saved;
        }
    }
}

// --- Drawing ---------------------------------------------------------------------------

void GuiEngine::draw(DrawList& list) const
{
    const auto base = static_cast<std::uint32_t>(list.layers.size());
    list.layers.insert(list.layers.end(), layers_.begin(), layers_.end());
    for (const Node& node : nodes_) {
        const std::size_t first = list.quads.size();
        if (node.groupOpacity <= 0.0f)
            continue;
        drawNode(list, node);
        if (node.layer != 0)
            for (std::size_t i = first; i < list.quads.size(); ++i)
                list.quads[i].layer = base + node.layer;
        const Color3 tint = node.groupTint;
        if (node.groupOpacity < 1.0f || tint.r != 1.0f || tint.g != 1.0f || tint.b != 1.0f)
            for (std::size_t i = first; i < list.quads.size(); ++i) {
                Color4& c = list.quads[i].color;
                c = {c.r * tint.r, c.g * tint.g, c.b * tint.b, c.a * node.groupOpacity};
            }
    }
}

void GuiEngine::drawImage(DrawList& list, const Node& node, const RectF& rect, const Quad& base) const
{
    const auto* label = instanceCast<const ImageLabel>(node.object);
    const auto* button = instanceCast<const ImageButton>(node.object);
    const auto* bindingLabel = instanceCast<const InputActionLabel>(node.object);
    if ((!label && !button && !bindingLabel) || !images_)
        return;
    ImageProps p;
    if (label)
        p = imagePropsOf(*label);
    else if (button)
        p = imagePropsOf(*button);
    else {
        p.image = &bindingLabel->resolvedImageContent();
        p.color = bindingLabel->imageColor3();
        p.transparency = bindingLabel->imageTransparency();
        p.scaleType = Enum::ScaleType::Fit;
    }
    std::string_view id = *p.image;
    if (button) {
        const auto self = node.object->shared_from_this();
        if (!pressed_.expired() && pressed_.lock() == self && !button->pressedImage().empty())
            id = button->pressedImage();
        else if (!hovered_.expired() && hovered_.lock() == self && !button->hoverImage().empty())
            id = button->hoverImage();
    }
    if (id.empty() || p.transparency >= 1.0f)
        return;
    auto image = images_(id);
    if (!image || image->empty())
        return;
    const auto iw = static_cast<float>(image->width);
    const auto ih = static_cast<float>(image->height);
    // ImageRectOffset/Size pick a part of the image (all of it when the size is zero).
    float sx = p.rectOffset.x;
    float sy = p.rectOffset.y;
    float sw = p.rectSize.x > 0 ? p.rectSize.x : iw - sx;
    float sh = p.rectSize.y > 0 ? p.rectSize.y : ih - sy;
    Quad q = base;
    q.kind = QuadKind::Image;
    q.texture = list.texture(image);
    q.color = Color4::from(p.color, p.transparency);
    q.pixelated = p.resample == Enum::ResamplerMode::Pixelated;
    q.stroke = 0;
    auto emit = [&](RectF r, float u0, float v0, float u1, float v1) {
        if (r.w <= 0 || r.h <= 0)
            return;
        Quad piece = q;
        piece.rect = r;
        piece.uv = {u0 / iw, v0 / ih, u1 / iw, v1 / ih};
        list.push(piece);
    };

    switch (p.scaleType) {
    case Enum::ScaleType::Stretch:
        emit(rect, sx, sy, sx + sw, sy + sh);
        break;
    case Enum::ScaleType::Fit: {
        const float s = std::min(rect.w / sw, rect.h / sh);
        const float w = sw * s;
        const float h = sh * s;
        emit({rect.x + (rect.w - w) / 2, rect.y + (rect.h - h) / 2, w, h}, sx, sy, sx + sw, sy + sh);
        break;
    }
    case Enum::ScaleType::Crop: {
        const float s = std::max(rect.w / sw, rect.h / sh);
        const float cw = rect.w / s;
        const float ch = rect.h / s;
        const float cx = sx + (sw - cw) / 2;
        const float cy = sy + (sh - ch) / 2;
        emit(rect, cx, cy, cx + cw, cy + ch);
        break;
    }
    case Enum::ScaleType::Slice: {
        // Nine pieces: SliceCenter is the stretchy middle, in image pixels.
        const Rect c = p.sliceCenter;
        const float x0 = c.min.x, y0 = c.min.y;
        const float x1 = c.max.x > 0 ? c.max.x : sw, y1 = c.max.y > 0 ? c.max.y : sh;
        const float k = p.sliceScale * node.scale;
        float left = x0 * k, top = y0 * k, right = (sw - x1) * k, bottom = (sh - y1) * k;
        // Margins never overlap; they shrink together when the box is small.
        if (left + right > rect.w) {
            const float f = rect.w / (left + right);
            left *= f;
            right *= f;
        }
        if (top + bottom > rect.h) {
            const float f = rect.h / (top + bottom);
            top *= f;
            bottom *= f;
        }
        const float xs[4] = {rect.x, rect.x + left, rect.right() - right, rect.right()};
        const float ys[4] = {rect.y, rect.y + top, rect.bottom() - bottom, rect.bottom()};
        const float us[4] = {sx, sx + x0, sx + x1, sx + sw};
        const float vs[4] = {sy, sy + y0, sy + y1, sy + sh};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                emit({xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j]}, us[i], vs[j], us[i + 1], vs[j + 1]);
        break;
    }
    case Enum::ScaleType::Tile: {
        const float tw = std::max(resolveScaled(p.tileSize.x, rect.w, node.scale), 1.0f);
        const float th = std::max(resolveScaled(p.tileSize.y, rect.h, node.scale), 1.0f);
        q.clip = q.clip.intersect(rect);
        const int columns = std::min(static_cast<int>(std::ceil(rect.w / tw)), 64);
        const int rows = std::min(static_cast<int>(std::ceil(rect.h / th)), 64);
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < columns; ++col)
                emit({rect.x + static_cast<float>(col) * tw, rect.y + static_cast<float>(row) * th, tw, th}, sx, sy,
                     sx + sw, sy + sh);
        break;
    }
    }
}

void GuiEngine::drawViewport(DrawList& list, const Node& node, const RectF& rect, const Quad& base) const
{
    auto* frame = instanceCast<ViewportFrame>(node.object);
    if (!frame || !viewports_ || frame->imageTransparency() >= 1.0f)
        return;
    // Scrolled out of its list, or off the screen: not asked for at all.
    RectF shown = rect.intersect(node.clip);
    if (node.layer == 0)
        shown = shown.intersect({0.0f, 0.0f, drawnScreen_.x, drawnScreen_.y});
    if (shown.w <= 0 || shown.h <= 0)
        return;
    auto image = viewports_(*frame, static_cast<std::uint32_t>(std::lround(std::max(rect.w, 1.0f))),
                            static_cast<std::uint32_t>(std::lround(std::max(rect.h, 1.0f))));
    if (!image)
        return;
    Quad q = base;
    q.kind = QuadKind::Image;
    q.texture = list.texture(image);
    q.color = Color4::from(frame->imageColor3(), frame->imageTransparency());
    q.premultiplied = true;
    q.stroke = 0;
    q.rect = rect;
    list.push(q);
}

void GuiEngine::drawNode(DrawList& list, const Node& node) const
{
    GuiObject* object = node.object;
    const RectF& r = node.rect;
    Quad base;
    base.clip = node.clip;
    if (node.sinA != 0.0f) {
        // p' = R p + t is a rotation about the pivot (I - R)^-1 t.
        base.rotation = std::atan2(node.sinA, node.cosA);
        const float c = node.cosA, s = node.sinA;
        const float det = (1 - c) * (1 - c) + s * s;
        base.pivotX = ((1 - c) * node.tx - s * node.ty) / det;
        base.pivotY = (s * node.tx + (1 - c) * node.ty) / det;
    }

    if (node.scrollbarOf) {
        const ScrollingFrame* sf = node.scrollbarOf;
        const Vector2 canvas = sf->absoluteCanvasSize();
        const Vector2 window = sf->absoluteWindowSize();
        const float pixel = settings_.pixelScale;
        const float thickness = static_cast<float>(sf->scrollBarThickness()) * node.scale;
        if (thickness <= 0 || sf->scrollBarImageTransparency() >= 1.0f)
            return;
        Quad bar = base;
        bar.color = Color4::from(sf->scrollBarImageColor3(), sf->scrollBarImageTransparency());
        // The default scroll bar images are a plain dark grey bar, drawn here as that colour (guess).
        bar.color.r *= 0.33f;
        bar.color.g *= 0.33f;
        bar.color.b *= 0.33f;
        if (canvas.y > window.y + 0.5f) {
            const float track = r.h;
            const float length = std::max(track * window.y / canvas.y, thickness);
            const float offset = (track - length) * sf->canvasPosition().y / std::max(canvas.y - window.y, 1.0f);
            const float x = sf->verticalScrollBarPosition() == Enum::VerticalScrollBarPosition::Left
                                ? r.x
                                : r.right() - thickness;
            bar.rect = {x, r.y + offset, thickness, length};
            list.push(bar);
        }
        if (canvas.x > window.x + 0.5f) {
            const float track = r.w;
            const float length = std::max(track * window.x / canvas.x, thickness);
            const float offset = (track - length) * sf->canvasPosition().x / std::max(canvas.x - window.x, 1.0f);
            bar.rect = {r.x + offset, r.bottom() - thickness, length, thickness};
            list.push(bar);
        }
        (void)pixel;
        return;
    }

    // UICorner: CornerRadius scale is relative to the shorter side (guess from
    // UDim(0.5, 0) making circles of squares).
    std::array<float, 4> radii{};
    const UICorner* corner = childOf<UICorner>(object);
    if (corner) {
        const float radius = resolveScaled(corner->cornerRadius(), std::min(r.w, r.h), node.scale);
        radii = {radius, radius, radius, radius};
    }
    const UIGradient* gradient = childOf<UIGradient>(object);
    if (gradient && !gradient->enabled())
        gradient = nullptr;
    // One ramp for the object, however many quads it has (text has one a glyph).
    std::uint32_t ramp = 0;
    auto applyGradient = [&](Quad& q) {
        if (!gradient)
            return;
        if (ramp == 0)
            ramp = list.gradient(GradientRamp::from(gradient->color(), gradient->transparency()));
        q.gradient = ramp;
        q.gradientRotation = gradient->rotation() * degrees;
        q.gradientOffsetX = gradient->offset().x;
        q.gradientOffsetY = gradient->offset().y;
    };

    // UIShadow: a blurred copy of the box behind it, grown by Spread and
    // moved by Offset. GUESS: BlurRadius's scale is of the box's shorter
    // side, Spread's and Offset's of its size; Inset and text shadows aren't drawn.
    if (const auto* shadow = childOf<UIShadow>(object);
        shadow && shadow->enabled() && !shadow->inset() && shadow->transparency() < 1.0f) {
        const float shorter = std::min(r.w, r.h);
        const float blur = std::max(resolveScaled(shadow->blurRadius(), shorter, node.scale), 0.0f);
        const float growX = resolveScaled(shadow->spread().x, r.w, node.scale) / 2.0f + blur / 2.0f;
        const float growY = resolveScaled(shadow->spread().y, r.h, node.scale) / 2.0f + blur / 2.0f;
        const float dx = resolveScaled(shadow->offset().x, r.w, node.scale);
        const float dy = resolveScaled(shadow->offset().y, r.h, node.scale);
        Quad q = base;
        q.rect = {r.x + dx - growX, r.y + dy - growY, r.w + 2 * growX, r.h + 2 * growY};
        q.kind = QuadKind::Shadow;
        q.uv = {blur, 0.0f, 0.0f, 0.0f};
        q.color = Color4::from(shadow->color(), shadow->transparency());
        for (std::size_t i = 0; i < 4; ++i)
            q.radii[i] = radii[i];
        list.push(q);
    }

    // Background, with AutoButtonColor's hover and press tints.
    Color3 background = object->backgroundColor3();
    if (auto* button = instanceCast<GuiButton>(object); button && button->autoButtonColor()) {
        const auto self = object->shared_from_this();
        // 0.742's custom-button path blends 30% toward black on hover
        // and toward white while pressed (0x103ec3464).
        if (!pressed_.expired() && pressed_.lock() == self)
            background = {background.r * 0.7f + 0.3f, background.g * 0.7f + 0.3f, background.b * 0.7f + 0.3f};
        else if (!hovered_.expired() && hovered_.lock() == self)
            background = darken(background, 0.7f);
    }
    const auto skin = legacySkin(object, hovered_.lock().get() == object, pressed_.lock().get() == object);
    const float bgTransparency = object->backgroundTransparency();
    if (skin) {
        const auto first = list.quads.size();
        drawLegacySkin(list, images_, *skin, r, node.scale, base);
        for (std::size_t i = first; i < list.quads.size(); ++i)
            applyGradient(list.quads[i]);
    } else if (bgTransparency < 1.0f) {
        const int border = object->borderSizePixel();
        if (border > 0 && !corner) {
            // BorderMode: Outline outside the box, Middle across its edge, Inset inside.
            const float b = static_cast<float>(border) * node.scale;
            float grow = b;
            if (object->borderMode() == Enum::BorderMode::Middle)
                grow = b / 2;
            else if (object->borderMode() == Enum::BorderMode::Inset)
                grow = 0;
            Quad q = base;
            q.rect = {r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow};
            q.color = Color4::from(object->borderColor3(), bgTransparency);
            q.stroke = b;
            list.push(q);
        }
        Quad q = base;
        q.rect = r;
        q.color = Color4::from(background, bgTransparency);
        q.radii = radii;
        applyGradient(q);
        list.push(q);
    }

    // UIStroke on the border (outside the box, across its edge or inside,
    // rounded joins round the corners) or round the text. Thickness is in
    // pixels, or with ScaledSize a fraction of the box's shorter side (the
    // text's size for text). GUESS: those bases.
    auto* stroke = childOf<UIStroke>(object);
    if (stroke && !stroke->enabled())
        stroke = nullptr;
    const bool textStroke = stroke && node.text && stroke->applyStrokeMode() == Enum::ApplyStrokeMode::Contextual;
    auto strokeWidth = [&](float basis) {
        return stroke->strokeSizingMode() == Enum::StrokeSizingMode::ScaledSize ? stroke->thickness() * basis
                                                                                : stroke->thickness() * node.scale;
    };
    if (stroke && !textStroke && stroke->transparency() < 1.0f && stroke->thickness() > 0.0f) {
        const float t = strokeWidth(std::min(r.w, r.h));
        float grow = t;
        if (stroke->borderStrokePosition() == Enum::BorderStrokePosition::Center)
            grow = t / 2;
        else if (stroke->borderStrokePosition() == Enum::BorderStrokePosition::Inner)
            grow = 0;
        Quad q = base;
        q.rect = {r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow};
        q.color = Color4::from(stroke->color(), stroke->transparency());
        q.stroke = t;
        const float joinRadius = stroke->lineJoinMode() == Enum::LineJoinMode::Round ? grow : 0.0f;
        for (std::size_t i = 0; i < 4; ++i)
            q.radii[i] = radii[i] > 0 ? radii[i] + grow : joinRadius;
        list.push(q);
    }

    drawImage(list, node, r, base);
    drawViewport(list, node, r, base);

    if (node.text) {
        const auto props = textProps(object);
        Color4 strokeColor{0, 0, 0, 0};
        if (props && props->strokeTransparency < 1.0f)
            strokeColor = Color4::from(props->strokeColor, props->strokeTransparency);
        if (textStroke && stroke->transparency() < 1.0f)
            strokeColor = Color4::from(stroke->color(), stroke->transparency());
        const std::size_t firstQuad = list.quads.size();
        RectF clip = node.clip;
        if (object->clipsDescendants() || instanceCast<TextBox>(object))
            clip = clip.intersect(r);
        list.text(*atlas_, *node.text, node.textRect.x, node.textRect.y, clip, strokeColor, base.rotation, base.pivotX,
                  base.pivotY, textStroke ? strokeWidth(node.text->textSize) : 1.0f);
        if (gradient)
            for (std::size_t i = firstQuad; i < list.quads.size(); ++i)
                applyGradient(list.quads[i]);

        // The caret of the focused TextBox, blinking.
        if (object == focused_ && std::fmod(time_ - caretTime_, 1.0) < 0.5) {
            const TextLayout& layout = *node.text;
            float x = node.textRect.x;
            float top = node.textRect.y;
            float height = layout.textSize;
            if (!layout.lines.empty()) {
                const TextLine* line = &layout.lines.front();
                x += line->x;
                top += line->top;
                for (const TextLine& l : layout.lines) {
                    for (std::uint32_t g = l.firstGlyph; g < l.firstGlyph + l.glyphCount; ++g) {
                        if (layout.glyphs[g].source < editor_.caret) {
                            line = &l;
                            x = node.textRect.x + layout.glyphs[g].x;
                            top = node.textRect.y + l.top;
                            if (layout.glyphs[g].face)
                                x += static_cast<float>(layout.glyphs[g].face->advance(layout.glyphs[g].glyph))
                                     * layout.glyphs[g].pixelsPerUnit;
                        }
                    }
                }
                if (editor_.caret == 0)
                    x = node.textRect.x + layout.lines.front().x;
                height = line->height;
            } else {
                // Empty text: where the alignment puts it.
                if (props && props->xAlignment == Enum::TextXAlignment::Center)
                    x += node.textRect.w / 2;
                else if (props && props->xAlignment == Enum::TextXAlignment::Right)
                    x += node.textRect.w;
                if (props && props->yAlignment == Enum::TextYAlignment::Center)
                    top += (node.textRect.h - height) / 2;
                else if (props && props->yAlignment == Enum::TextYAlignment::Bottom)
                    top += node.textRect.h - height;
            }
            Quad caret = base;
            caret.rect = {std::round(x), std::round(top), std::max(1.0f, std::round(node.scale)), height};
            caret.color = Color4::from(props ? props->color : Color3{}, 0.0f);
            caret.clip = clip;
            list.push(caret);
        }
    }
}

// --- Input -----------------------------------------------------------------------------

namespace {

// Whether a laid out node is under (x, y), in the space it was laid out in:
// the screen's, or its surface's canvas.
bool covers(const auto& node, float x, float y, bool inputOnly)
{
    if (node.scrollbarOf || !node.clip.contains(x, y))
        return false;
    // Into the node's unrotated frame: p = R^T (p' - t).
    const float dx = x - node.tx;
    const float dy = y - node.ty;
    const float lx = node.cosA * dx + node.sinA * dy;
    const float ly = -node.sinA * dx + node.cosA * dy;
    if (!node.rect.contains(lx, ly))
        return false;
    const GuiObject* o = node.object;
    return !inputOnly
           || ((o->active() || instanceCast<const GuiButton>(o) || instanceCast<const TextBox>(o))
               && o->interactable());
}

} // namespace

const GuiEngine::Node* GuiEngine::hit(float x, float y, bool inputOnly) const
{
    for (const Node& node : std::views::reverse(nodes_)) {
        // A surface GUI's nodes are on its canvas, not the screen: they're
        // under everything on it.
        if (node.layer != 0 && layers_[node.layer - 1].kind == WorldLayer::Kind::Surface)
            continue;
        if (covers(node, x, y, inputOnly))
            return &node;
    }
    return surfaceHit(x, y, inputOnly);
}

// Whether the wheel does something for `start` or what it sits in: a
// scrolling frame with more canvas than window (even one scrolled to its
// end), a page layout that turns pages by it.
static bool wheelScrolls(const Instance* start)
{
    for (const Instance* object = start; object; object = object->parent()) {
        if (auto* sf = instanceCast<const ScrollingFrame>(object); sf && sf->scrollingEnabled()) {
            const Vector2 canvas = sf->absoluteCanvasSize(), window = sf->absoluteWindowSize();
            if ((sf->scrollingDirection() != Enum::ScrollingDirection::X && canvas.y > window.y)
                || (sf->scrollingDirection() != Enum::ScrollingDirection::Y && canvas.x > window.x))
                return true;
        }
        if (auto* pages = childOf<UIPageLayout>(object); pages && pages->scrollWheelInputEnabled())
            return true;
    }
    return false;
}

bool GuiEngine::wheelTaken() const
{
    const float x = mouseX_, y = mouseY_;
    // As handle's ScrollEvent: from the top down, a button, an active frame
    // or a text box stops the search.
    for (const Node& node : std::views::reverse(nodes_)) {
        if (node.layer != 0 && layers_[node.layer - 1].kind == WorldLayer::Kind::Surface)
            continue;
        if (!covers(node, x, y, false))
            continue;
        if (wheelScrolls(node.object))
            return true;
        const GuiObject* o = node.object;
        if ((o->active() || instanceCast<const GuiButton>(o) || instanceCast<const TextBox>(o)) && o->interactable())
            return false;
    }
    const Node* pointer = surfaceHit(x, y, false);
    return pointer && wheelScrolls(pointer->object);
}

const GuiEngine::Node* GuiEngine::surfaceHit(float x, float y, bool inputOnly) const
{
    if (!view_ || surfaces_.empty())
        return nullptr;
    // The ray through the pixel, as placeBillboard projects.
    const View& view = *view_;
    const float aspect = view.screen.x / view.screen.y;
    const Vector3 along{(2.0f * x / view.screen.x - 1.0f) * view.tanHalf * aspect,
                        (1.0f - 2.0f * y / view.screen.y) * view.tanHalf, -1.0f};
    const Vector3 from = view.cframe.position;
    const Vector3 direction = view.cframe.vectorToWorldSpace(along).unit();
    // Where it crosses each canvas (surfaces are laid out only facing the camera).
    struct Met {
        const SurfaceInput* surface;
        float distance;
        Vector2 at; // canvas pixels
    };
    std::vector<Met> met;
    for (const SurfaceInput& surface : surfaces_) {
        const WorldLayer& layer = layers_[surface.layer - 1];
        const Vector3 normal = layer.axisX.cross(layer.axisY);
        const float speed = direction.dot(normal);
        if (std::abs(speed) < 1e-9f)
            continue;
        const float t = (layer.origin - from).dot(normal) / speed;
        if (t <= 0.0f)
            continue;
        const Vector3 d = from + direction * t - layer.origin;
        const Vector2 at{d.dot(layer.axisX) / layer.axisX.dot(layer.axisX),
                         d.dot(layer.axisY) / layer.axisY.dot(layer.axisY)};
        if (at.x >= 0.0f && at.y >= 0.0f && at.x <= surface.canvas.x && at.y <= surface.canvas.y)
            met.push_back({&surface, t, at});
    }
    if (met.empty())
        return nullptr;
    // The nearest one's part stops the ray, and so may the world before it.
    const Met& first = *std::ranges::min_element(met, {}, &Met::distance);
    if (pick_)
        if (const auto blocker = pick_(from, direction * (first.distance + 1.0f));
            blocker && blocker->part != first.surface->part && blocker->distance < first.distance - 0.05f)
            return nullptr;
    // Every GUI on that part's face, the last laid out on top.
    for (const Met& m : std::views::reverse(met)) {
        if (m.surface->part != first.surface->part || !m.surface->active)
            continue;
        for (const Node& node : std::views::reverse(nodes_))
            if (node.layer == m.surface->layer && covers(node, m.at.x, m.at.y, inputOnly))
                return &node;
    }
    return nullptr;
}

GuiObject* GuiEngine::objectAt(float x, float y) const
{
    const Node* node = hit(x * settings_.pixelScale, y * settings_.pixelScale, true);
    return node ? node->object : nullptr;
}

std::vector<InstancePtr> GuiEngine::objectsAt(float x, float y) const
{
    x *= settings_.pixelScale;
    y *= settings_.pixelScale;
    std::vector<InstancePtr> found;
    for (const Node& node : std::views::reverse(nodes_))
        if (node.layer == 0 && covers(node, x, y, false)) {
            InstancePtr object = node.object->shared_from_this();
            if (std::ranges::find(found, object) == found.end())
                found.push_back(std::move(object));
        }
    return found;
}

std::optional<RectF> GuiEngine::rectOf(const GuiObject* object) const
{
    for (const Node& node : nodes_)
        if (node.object == object && !node.scrollbarOf)
            return node.rect;
    return std::nullopt;
}

void GuiEngine::setHover(GuiObject* object, float x, float y)
{
    auto previous = hovered_.lock();
    if (previous.get() == object)
        return;
    const int ix = static_cast<int>(x / settings_.pixelScale - coordinateOrigin_.x);
    const int iy = static_cast<int>(y / settings_.pixelScale - coordinateOrigin_.y);
    if (auto* old = instanceCast<GuiObject>(previous.get()))
        old->fireMouseLeave(ix, iy);
    hovered_.reset();
    if (object) {
        hovered_ = object->shared_from_this();
        object->fireMouseEnter(ix, iy);
    }
}

void GuiEngine::focus(TextBox* box)
{
    if (box == focused_)
        return;
    if (focused_)
        releaseFocus(false);
    if (!box)
        return;
    focused_ = box;
    focusedRef_ = box->shared_from_this();
    if (box->clearTextOnFocus())
        box->setText("");
    editor_.multiLine = box->multiLine();
    editor_.setText(box->text());
    box->setCursorPosition(static_cast<int>(editor_.caret) + 1);
    caretTime_ = time_;
    box->fireFocused();
}

void GuiEngine::releaseFocus(bool enterPressed)
{
    TextBox* box = focused_;
    if (!box || focusedRef_.expired()) {
        focused_ = nullptr;
        return;
    }
    focused_ = nullptr;
    if (box->cursorPosition() < 0)
        return;
    box->setCursorPosition(-1);
    box->fireFocusLost(enterPressed);
}

void GuiEngine::routeInput(const platform::Event& event, bool capturedOnly)
{
    Enum::UserInputType type = Enum::UserInputType::None;
    Enum::UserInputState state = Enum::UserInputState::Change;
    Enum::KeyCode key = Enum::KeyCode::None;
    float x = mouseX_, y = mouseY_;
    Vector3 delta;
    bool keyboard = false;
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        if (button->button != platform::MouseButton::Left && button->button != platform::MouseButton::Right
            && button->button != platform::MouseButton::Middle)
            return;
        type = button->button == platform::MouseButton::Left    ? Enum::UserInputType::MouseButton1
               : button->button == platform::MouseButton::Right ? Enum::UserInputType::MouseButton2
                                                                : Enum::UserInputType::MouseButton3;
        state = button->pressed ? Enum::UserInputState::Begin : Enum::UserInputState::End;
        x = static_cast<float>(button->x) * settings_.pixelScale;
        y = static_cast<float>(button->y) * settings_.pixelScale;
    } else if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        type = Enum::UserInputType::MouseMovement;
        x = static_cast<float>(move->x) * settings_.pixelScale;
        y = static_cast<float>(move->y) * settings_.pixelScale;
        delta = {(x - mouseX_) / settings_.pixelScale, (y - mouseY_) / settings_.pixelScale, 0};
    } else if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        type = Enum::UserInputType::MouseWheel;
        delta.z = static_cast<float>(scroll->dy);
    } else if (const auto* keyEvent = std::get_if<platform::KeyEvent>(&event)) {
        if (keyEvent->repeat)
            return;
        keyboard = true;
        key = keyCodeFor(keyEvent->key).value_or(Enum::KeyCode::None);
        type = Enum::UserInputType::Keyboard;
        state = keyEvent->pressed ? Enum::UserInputState::Begin : Enum::UserInputState::End;
    } else
        return;
    if (capturedOnly && state != Enum::UserInputState::End)
        return;
    InputTargets targets;
    auto& held = keyboard ? keyTargets_[key] : buttonTargets_[type];
    if (state == Enum::UserInputState::End) {
        targets = std::move(held);
        if (keyboard)
            keyTargets_.erase(key);
        else
            buttonTargets_.erase(type);
    } else {
        const Node* node = hit(x, y, false);
        Instance* object = keyboard && focused_ ? focused_ : node ? node->object : nullptr;
        while (object && !object->isA("LayerCollector")) {
            if (object->isA("GuiObject"))
                targets.push_back(object->weak_from_this());
            object = object->parent();
        }
        if (state == Enum::UserInputState::Begin)
            held = targets;
    }
    if (targets.empty())
        return;
    const std::string name = state == Enum::UserInputState::Begin ? "InputBegan"
                             : state == Enum::UserInputState::End ? "InputEnded"
                                                                  : "InputChanged";
    inputDispatch_ = [targets = std::move(targets), name](const InstancePtr& object) {
        const std::array<Value, 1> args{object};
        for (const auto& target : targets)
            if (auto instance = target.lock(); instance && !instance->destroyed())
                instance->fireEvent(name, args);
    };
    if (deferredInput_)
        return;
    InstancePtr object;
    if (keyboard) {
        auto& current = localKeys_[key];
        if (!current)
            current = std::make_shared<InputObject>();
        object = current;
        if (state == Enum::UserInputState::End)
            localKeys_.erase(key);
    } else if (type == Enum::UserInputType::MouseMovement || type == Enum::UserInputType::MouseWheel) {
        auto& current = type == Enum::UserInputType::MouseMovement ? localMovement_ : localWheel_;
        if (!current)
            current = std::make_shared<InputObject>();
        object = current;
    } else {
        auto& current = localButtons_[type];
        if (!current)
            current = std::make_shared<InputObject>();
        object = current;
        if (state == Enum::UserInputState::End)
            localButtons_.erase(type);
    }
    auto* input = static_cast<InputObject*>(object.get());
    input->setKeyCode(key);
    input->setUserInputType(type);
    input->setUserInputState(state);
    input->setDelta(delta);
    input->setPosition(keyboard ? Vector3{}
                                : Vector3{x / settings_.pixelScale - coordinateOrigin_.x,
                                          y / settings_.pixelScale - coordinateOrigin_.y,
                                          type == Enum::UserInputType::MouseWheel ? delta.z : 0});
    auto dispatch = takeInputDispatch();
    dispatch(object);
}

void GuiEngine::finishInputEvent(const platform::Event& event)
{
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event);
        button && button->button == platform::MouseButton::Left && !button->pressed) {
        // A core modal may take the release after the game button took the
        // press. Release its mouse/InputBinding state without activating it.
        const auto held = pressed_.lock();
        pressed_.reset();
        if (auto* target = instanceCast<GuiButton>(held.get()); target && !target->destroyed())
            target->fireRelease(static_cast<int>(button->x - static_cast<double>(coordinateOrigin_.x)),
                                static_cast<int>(button->y - static_cast<double>(coordinateOrigin_.y)), false);
    }
    if (!inputDispatch_)
        routeInput(event, true);
}

bool GuiEngine::handle(const platform::Event& event)
{
    if (focused_ && focusedRef_.expired())
        focused_ = nullptr;
    clearInputDispatch();
    routeInput(event);
    const float k = settings_.pixelScale;
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * k;
        mouseY_ = static_cast<float>(move->y) * k;
        const Node* node = hit(mouseX_, mouseY_, true);
        setHover(node ? node->object : nullptr, mouseX_, mouseY_);
        if (node)
            node->object->fireMouseMoved(static_cast<int>(move->x - static_cast<double>(coordinateOrigin_.x)),
                                         static_cast<int>(move->y - static_cast<double>(coordinateOrigin_.y)));
        return false;
    }
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        const int eventX = static_cast<int>(button->x - static_cast<double>(coordinateOrigin_.x));
        const int eventY = static_cast<int>(button->y - static_cast<double>(coordinateOrigin_.y));
        mouseX_ = static_cast<float>(button->x) * k;
        mouseY_ = static_cast<float>(button->y) * k;
        const Node* node = hit(mouseX_, mouseY_, true);
        GuiObject* target = node ? node->object : nullptr;
        if (button->button == platform::MouseButton::Left) {
            if (button->pressed) {
                auto* box = instanceCast<TextBox>(target);
                if (box != focused_)
                    focus(box);
                if (auto* b = instanceCast<GuiButton>(target)) {
                    pressed_ = b->shared_from_this();
                    b->firePress(eventX, eventY);
                }
                return target != nullptr;
            }
            auto held = pressed_.lock();
            pressed_.reset();
            if (auto* b = instanceCast<GuiButton>(held.get())) {
                b->fireRelease(eventX, eventY, held.get() == target);
                return true;
            }
            return target != nullptr;
        }
        if (button->button == platform::MouseButton::Right && !button->pressed)
            if (auto* b = instanceCast<GuiButton>(target))
                b->fireRightClick(eventX, eventY);
        return target != nullptr;
    }
    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        // What the wheel does for the object under the pointer, or for what it
        // sits in: a scrolling frame scrolls, a page layout turns a page.
        const auto scrollFrom = [&](Instance* start) {
            for (Instance* object = start; object; object = object->parent()) {
                if (auto* sf = instanceCast<ScrollingFrame>(object); sf && sf->scrollingEnabled()) {
                    const Vector2 canvas = sf->absoluteCanvasSize(), window = sf->absoluteWindowSize();
                    Vector2 pos = sf->canvasPosition();
                    const bool vertical = sf->scrollingDirection() != Enum::ScrollingDirection::X && canvas.y > window.y;
                    const bool horizontal = sf->scrollingDirection() != Enum::ScrollingDirection::Y && canvas.x > window.x;
                    if (vertical)
                        pos.y = std::clamp(pos.y - static_cast<float>(scroll->dy) * 40, 0.0f, canvas.y - window.y);
                    else if (horizontal)
                        pos.x = std::clamp(pos.x - static_cast<float>(scroll->dy) * 40, 0.0f, canvas.x - window.x);
                    if (vertical || horizontal) {
                        sf->setCanvasPosition(pos);
                        return true;
                    }
                }
                if (auto* pages = childOf<UIPageLayout>(object);
                    pages && pages->scrollWheelInputEnabled() && scroll->dy != 0) {
                    const int step = scroll->dy > 0 ? -1 : 1;
                    if (onPageStep)
                        onPageStep(*pages, step);
                    else {
                        const auto all = pages->pages();
                        if (!all.empty()) {
                            auto index
                                = static_cast<int>(std::ranges::find(all, pages->currentPage()) - all.begin()) + step;
                            const int count = static_cast<int>(all.size());
                            index = pages->circular() ? ((index % count) + count) % count : std::clamp(index, 0, count - 1);
                            if (all[static_cast<std::size_t>(index)] != pages->currentPage()) {
                                const std::array<Value, 1> old{
                                    pages->currentPage() ? Value(pages->currentPage()->shared_from_this()) : Value{}};
                                pages->fireEvent("PageLeave", old);
                                pages->assignCurrentPage(all[static_cast<std::size_t>(index)]);
                                pages->assignSlide(nullptr, 1);
                                const std::array<Value, 1> now{all[static_cast<std::size_t>(index)]->shared_from_this()};
                                pages->fireEvent("PageEnter", now);
                                pages->fireEvent("Stopped", now);
                            }
                        }
                    }
                    return true;
                }
            }
            return false;
        };
        // From the top down: a transparent frame laid over the menu (a fade, a
        // dimmer) lets the wheel through, as it does clicks that nothing
        // listens to; a button, an active frame or a text box takes it.
        for (const Node& node : std::views::reverse(nodes_)) {
            if (node.layer != 0 && layers_[node.layer - 1].kind == WorldLayer::Kind::Surface)
                continue;
            if (!covers(node, mouseX_, mouseY_, false))
                continue;
            if (scrollFrom(node.object))
                return true;
            const GuiObject* o = node.object;
            if ((o->active() || instanceCast<const GuiButton>(o) || instanceCast<const TextBox>(o)) && o->interactable())
                return false;
        }
        const Node* pointer = surfaceHit(mouseX_, mouseY_, false);
        return pointer && scrollFrom(pointer->object);
    }
    if (!focused_)
        return false;
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (!key->pressed)
            return true;
        TextBox* box = focused_;
        const TextEditor::Result result = box->textEditable() ? editor_.key(*key) : TextEditor::Result::None;
        if (result == TextEditor::Result::Submitted || (key->key == platform::Key::Enter && !box->textEditable())) {
            releaseFocus(true);
        } else if (result == TextEditor::Result::Cancelled) {
            releaseFocus(false);
        } else if (result == TextEditor::Result::Edited) {
            box->setText(editor_.text);
            box->setCursorPosition(static_cast<int>(editor_.caret) + 1);
            caretTime_ = time_;
        }
        return true;
    }
    if (const auto* text = std::get_if<platform::TextEvent>(&event)) {
        if (focused_->textEditable()) {
            editor_.insert(text->text);
            focused_->setText(editor_.text);
            focused_->setCursorPosition(static_cast<int>(editor_.caret) + 1);
            caretTime_ = time_;
        }
        return true;
    }
    return false;
}

} // namespace lunar::ui
