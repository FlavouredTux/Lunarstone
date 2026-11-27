#include <lunar/ui/Topbar.hpp>

#include <algorithm>
#include <numbers>
#include <utility>

namespace lunar::ui {
namespace {

void rounded(DrawList& list, RectF rect, Color4 color, float radius, float stroke = 0.0f)
{
    Quad quad;
    quad.rect = rect;
    quad.color = color;
    quad.radii.fill(radius);
    quad.stroke = stroke;
    list.push(quad);
}

} // namespace

void Topbar::update(Vector2 screen, float scale)
{
    screen_ = screen;
    scale_ = scale;
    if (!enabled)
        open_ = pressed_ = false;
}

RectF Topbar::buttonRect(Button button) const
{
    if (!enabled)
        return {};
    const float k = scale_;
    switch (button) {
    case Button::Roblox:
        return {16 * k, 8 * k, 44 * k, 44 * k};
    case Button::Controls:
        return {72 * k, 8 * k, 44 * k, 44 * k};
    case Button::Chat:
        return chatEnabled ? RectF{116 * k, 8 * k, 44 * k, 44 * k} : RectF{};
    case Button::PlayerList:
        return {72 * k, 60 * k, 192 * k, 40 * k};
    case Button::Settings:
        return {72 * k, 140 * k, 192 * k, 40 * k};
    }
    return {};
}

Rect Topbar::safeArea() const
{
    const float right = enabled ? (chatEnabled ? 172.0f : 128.0f) : 0.0f;
    const float width = std::max(screen_.x / scale_ - 16.0f, right);
    return {{right, 0}, {width, enabled ? height : 0.0f}};
}

bool Topbar::pointerOver() const
{
    if (!enabled)
        return false;
    return buttonRect(Button::Roblox).contains(mouseX_, mouseY_)
           || buttonRect(Button::Controls).contains(mouseX_, mouseY_)
           || buttonRect(Button::Chat).contains(mouseX_, mouseY_)
           || (open_ && RectF{72 * scale_, 60 * scale_, 192 * scale_, 120 * scale_}.contains(mouseX_, mouseY_));
}

bool Topbar::handle(const platform::Event& event)
{
    if (!enabled)
        return false;
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * scale_;
        mouseY_ = static_cast<float>(move->y) * scale_;
        return false;
    }
    if (const auto* key = std::get_if<platform::KeyEvent>(&event);
        key && key->pressed && key->key == platform::Key::Escape && open_) {
        open_ = false;
        return true;
    }
    const auto* button = std::get_if<platform::MouseButtonEvent>(&event);
    if (!button || button->button != platform::MouseButton::Left)
        return std::holds_alternative<platform::ScrollEvent>(event) && open_ && pointerOver();
    mouseX_ = static_cast<float>(button->x) * scale_;
    mouseY_ = static_cast<float>(button->y) * scale_;
    if (!button->pressed)
        return std::exchange(pressed_, false);
    if (buttonRect(Button::Roblox).contains(mouseX_, mouseY_)) {
        open_ = false;
        if (onMenu)
            onMenu();
    } else if (buttonRect(Button::Controls).contains(mouseX_, mouseY_)) {
        open_ = !open_;
    } else if (buttonRect(Button::Chat).contains(mouseX_, mouseY_)) {
        open_ = false;
        if (onChat)
            onChat();
    } else if (open_) {
        const RectF list = buttonRect(Button::PlayerList);
        const RectF chat{list.x, list.bottom(), list.w, list.h};
        if (list.contains(mouseX_, mouseY_) && playerListEnabled && onPlayerList)
            onPlayerList();
        else if (chat.contains(mouseX_, mouseY_) && chatEnabled && onChat)
            onChat();
        else if (buttonRect(Button::Settings).contains(mouseX_, mouseY_) && onSettings)
            onSettings();
        open_ = false;
    } else {
        return false;
    }
    pressed_ = true;
    return true;
}

void Topbar::label(DrawList& list, std::string_view text, RectF rect)
{
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(Enum::Font::BuilderSansMedium);
    params.style.size = 16 * scale_;
    params.style.color = {1, 1, 1};
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Center;
    params.box = {rect.w, rect.h};
    list.text(*atlas_, layoutText(*fonts_, params), rect.x, rect.y, rect);
}

void Topbar::draw(DrawList& list)
{
    if (!enabled)
        return;
    const float k = scale_;
    const Color4 background{0.07f, 0.08f, 0.1f, 0.9f};
    const Color4 white{1, 1, 1, 1};
    const RectF roblox = buttonRect(Button::Roblox);
    const RectF controls = buttonRect(Button::Controls);
    rounded(list, roblox, background, 22 * k);
    rounded(list, {controls.x, controls.y, (chatEnabled ? 88 : 44) * k, controls.h}, background, 22 * k);
    Quad logo;
    logo.rect = {roblox.x + 11 * k, roblox.y + 11 * k, 22 * k, 22 * k};
    logo.color = white;
    logo.stroke = 7 * k;
    logo.rotation = 0.25f;
    logo.pivotX = roblox.x + roblox.w / 2;
    logo.pivotY = roblox.y + roblox.h / 2;
    list.push(logo);
    for (int line = 0; line < 3; ++line)
        rounded(list, {controls.x + 12 * k, controls.y + (14.0f + static_cast<float>(line) * 6) * k, 20 * k, 2 * k},
                white, k);
    if (chatEnabled) {
        const RectF chat = buttonRect(Button::Chat);
        if (chatOpen)
            rounded(list, {chat.x + 4 * k, chat.y + 4 * k, 36 * k, 36 * k}, {1, 1, 1, 0.1f}, 18 * k);
        rounded(list, {chat.x + 11 * k, chat.y + 12 * k, 22 * k, 17 * k}, white, 4 * k, 2 * k);
        Quad tail;
        tail.rect = {chat.x + 14 * k, chat.y + 25 * k, 6 * k, 6 * k};
        tail.color = white;
        tail.rotation = std::numbers::pi_v<float> / 4;
        tail.pivotX = tail.rect.x + 3 * k;
        tail.pivotY = tail.rect.y + 3 * k;
        list.push(tail);
        for (int line = 0; line < 2; ++line)
            rounded(list, {chat.x + 16 * k, chat.y + (17.0f + static_cast<float>(line) * 5) * k, 12 * k, k}, white, k);
    }
    if (open_) {
        const RectF first = buttonRect(Button::PlayerList);
        rounded(list, {first.x, first.y, first.w, 120 * k}, {0.1f, 0.11f, 0.13f, 0.98f}, 8 * k);
        label(list, playerListOpen ? "Hide player list" : "Show player list",
              {first.x + 14 * k, first.y, first.w - 28 * k, first.h});
        label(list, chatOpen ? "Hide chat" : "Show chat",
              {first.x + 14 * k, first.bottom(), first.w - 28 * k, first.h});
        label(list, "Settings", {first.x + 14 * k, first.y + 80 * k, first.w - 28 * k, first.h});
    }
}

} // namespace lunar::ui
