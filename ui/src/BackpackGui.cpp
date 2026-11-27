#include <lunar/ui/BackpackGui.hpp>

#include <lunar/Tools.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <cmath>
#include <format>
#include <vector>

namespace lunar::ui {

namespace {

// Hotkeys: 1-9 for the first nine slots, 0 for the tenth.
std::optional<int> slotForKey(platform::Key key)
{
    using platform::Key;
    constexpr std::array<Key, BackpackGui::slotCount> keys{Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5,
                                                           Key::Num6, Key::Num7, Key::Num8, Key::Num9, Key::Num0};
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key)
            return static_cast<int>(i);
    return std::nullopt;
}

} // namespace

BackpackGui::BackpackGui(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}

void BackpackGui::setGame(DataModel* game)
{
    game_ = game ? game->shared_from_this() : nullptr;
    hotbar_.clear();
    pressed_.reset();
}

Player* BackpackGui::localPlayer() const
{
    auto game = game_.lock();
    auto* players = game ? dynamic_cast<Players*>(game->findFirstChildOfClass("Players")) : nullptr;
    return players ? players->localPlayer() : nullptr;
}

bool BackpackGui::enabled() const
{
    auto game = game_.lock();
    const auto* starterGui
        = game ? dynamic_cast<const StarterGui*>(game->findFirstChildOfClass("StarterGui")) : nullptr;
    return !starterGui || starterGui->coreGuiEnabled(Enum::CoreGuiType::Backpack);
}

Tool* BackpackGui::slot(int index) const
{
    return hotbar_.slot(index);
}

std::optional<RectF> BackpackGui::slotRect(int index) const
{
    if (index < 0 || index >= slotCount)
        return std::nullopt;
    return rects_[static_cast<std::size_t>(index)];
}

void BackpackGui::sync()
{
    if (auto game = std::static_pointer_cast<DataModel>(game_.lock()))
        hotbar_.sync(*game);
    else
        hotbar_.clear();
}

void BackpackGui::select(int index)
{
    if (auto game = std::static_pointer_cast<DataModel>(game_.lock()))
        hotbar_.select(*game, index);
}

bool BackpackGui::handle(const platform::Event& event)
{
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * scale_;
        mouseY_ = static_cast<float>(move->y) * scale_;
        return false;
    }
    if (!enabled())
        return false;
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (!key->pressed || key->repeat)
            return false;
        if (const auto index = slotForKey(key->key)) {
            sync();
            select(*index);
        }
        return false; // UserInputService still sees the key
    }
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        if (button->button != platform::MouseButton::Left)
            return false;
        const float x = static_cast<float>(button->x) * scale_;
        const float y = static_cast<float>(button->y) * scale_;
        std::optional<int> over;
        for (int i = 0; i < slotCount; ++i)
            if (const auto& rect = rects_[static_cast<std::size_t>(i)]; rect && rect->contains(x, y))
                over = i;
        if (button->pressed) {
            pressed_ = over;
            return over.has_value();
        }
        // MouseButton1Click: released over the slot it was pressed on.
        const bool click = over && pressed_ == over;
        const bool used = pressed_.has_value();
        pressed_.reset();
        if (click)
            select(*over);
        return used;
    }
    return false;
}

void BackpackGui::update(Vector2 screen, float pixelScale)
{
    scale_ = pixelScale;
    sync();
    for (auto& rect : rects_)
        rect.reset();
    if (!enabled())
        return;
    // UpdateBackpackLayout and slot:Readjust: the hotbar frame is centred at
    // the bottom, and the filled slots are centred in it. Positions are UDim2
    // offsets, whole pixels: the halves get cut off.
    const float k = pixelScale;
    const float frameWidth = iconBuffer + slotCount * (iconSize + iconBuffer);
    const float frameHeight = iconBuffer + iconSize + iconBuffer;
    const float frameX = screen.x / 2 + std::trunc(-frameWidth / 2) * k;
    const float frameY = screen.y - frameHeight * k;
    int total = 0;
    for (int i = 0; i < slotCount; ++i)
        total += hotbar_.slot(i) ? 1 : 0;
    int visual = 0;
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        if (!hotbar_.slot(static_cast<int>(i)))
            continue;
        ++visual;
        const float factor = static_cast<float>(visual) - (static_cast<float>(total) / 2.0f + 0.5f);
        const float x = std::trunc(frameWidth / 2 - iconSize / 2 + (iconBuffer + iconSize) * factor);
        rects_[i] = RectF{frameX + x * k, frameY + iconBuffer * k, iconSize * k, iconSize * k};
    }
}

void BackpackGui::textAt(DrawList& list, std::string_view text, RectF box, Enum::TextXAlignment x,
                         Enum::TextYAlignment y, bool wrapped, const RectF& clip)
{
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(Enum::Font::SourceSans);
    params.style.size = 14.0f * scale_; // FontSize.Size14
    params.style.color = {1.0f, 1.0f, 1.0f};
    params.xAlignment = x;
    params.yAlignment = y;
    params.wrapped = wrapped;
    params.box = {box.w, box.h};
    list.text(*atlas_, layoutText(*fonts_, params), box.x, box.y, clip);
}

void BackpackGui::draw(DrawList& list)
{
    if (!enabled())
        return;
    const float k = scale_;
    const RectF all{-1e9f, -1e9f, 2e9f, 2e9f};
    Player* player = localPlayer();
    Model* character = player ? player->character() : nullptr;
    std::optional<std::size_t> hovered;
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        const Tool* tool = hotbar_.slot(static_cast<int>(i));
        const auto& rect = rects_[i];
        if (!tool || !rect)
            continue;
        const RectF r = *rect;
        if (r.contains(mouseX_, mouseY_))
            hovered = i;
        list.rect(r, Color4::from(background, backgroundFade), all);

        // The icon (80%, centred) or, without a TextureId, the name.
        if (!tool->textureId().empty()) {
            if (auto image = images_ ? images_(tool->textureId()) : nullptr; image && !image->empty()) {
                Quad q;
                q.rect = {r.x + r.w * 0.1f, r.y + r.h * 0.1f, r.w * 0.8f, r.h * 0.8f};
                q.kind = QuadKind::Image;
                q.texture = list.texture(image);
                q.color = {1, 1, 1, 1};
                q.clip = all;
                list.push(q);
            }
        } else {
            textAt(list, tool->name(), {r.x + 1 * k, r.y + 1 * k, r.w - 2 * k, r.h - 2 * k},
                   Enum::TextXAlignment::Center, Enum::TextYAlignment::Center, true, r);
        }

        // The hotkey in the top-left corner: a 15% box with the digit centred on it.
        const std::string number = std::format("{}", i < 9 ? i + 1 : 0);
        textAt(list, number, {r.x, r.y, r.w * 0.15f, r.h * 0.15f}, Enum::TextXAlignment::Center,
               Enum::TextYAlignment::Center, true, all);

        if (character && tool->parent() == character) {
            const float t = equipThickness;
            const Color4 edge = Color4::from(equipColor);
            list.rect({r.x, r.y, r.w * t, r.h}, edge, all);
            list.rect({r.x + r.w * t, r.y, r.w * (1 - 2 * t), r.h * t}, edge, all);
            list.rect({r.x + r.w * (1 - t), r.y, r.w * t, r.h}, edge, all);
            list.rect({r.x + r.w * t, r.y + r.h * (1 - t), r.w * (1 - 2 * t), r.h * t}, edge, all);
        }
    }

    // The hovered slot's ToolTip, TOOLTIP_OFFSET above it.
    if (hovered && !pressed_) {
        const Tool* tool = hotbar_.slot(static_cast<int>(*hovered));
        const std::optional<RectF>& rect = rects_[*hovered];
        if (tool && rect && !tool->toolTip().empty()) {
            const RectF r = *rect;
            const float width
                = measureText(*fonts_, tool->toolTip(), fontFromEnum(Enum::Font::SourceSans), 14.0f * k).x + 6 * k;
            const RectF tip{std::round(r.x + r.w / 2 - width / 2), r.y - 25 * k, width, 16 * k};
            list.rect(tip, Color4::from(tooltipColor), all);
            textAt(list, tool->toolTip(), tip, Enum::TextXAlignment::Center, Enum::TextYAlignment::Top, false, all);
        }
    }
}

} // namespace lunar::ui
