#include <lunar/ui/LoadingScreen.hpp>

#include <lunar/ui/TextLayout.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace lunar::ui {

namespace {

constexpr RectF everywhere{-1e9f, -1e9f, 2e9f, 2e9f};
constexpr int spinnerDots = 8;
constexpr double spinnerTurn = 1.0; // seconds a turn

} // namespace

LoadingScreen::LoadingScreen(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}

void LoadingScreen::show()
{
    shown_ = true;
    dismissedAt_.reset();
    name_.clear();
    creator_.clear();
    icon_.clear();
    status_.clear();
    error_ = false;
    pressedLeave_ = false;
}

void LoadingScreen::setGame(std::string name, std::string creator, std::string icon)
{
    name_ = std::move(name);
    creator_ = std::move(creator);
    icon_ = std::move(icon);
}

void LoadingScreen::setStatus(std::string status, bool error)
{
    status_ = std::move(status);
    error_ = error;
}

void LoadingScreen::dismiss()
{
    if (shown_ && !dismissedAt_)
        dismissedAt_ = time_;
}

void LoadingScreen::remove()
{
    shown_ = false;
    dismissedAt_.reset();
}

float LoadingScreen::opacity() const
{
    if (!shown_)
        return 0.0f;
    if (!dismissedAt_)
        return 1.0f;
    const double t = (time_ - *dismissedAt_) / static_cast<double>(fadeSeconds);
    return static_cast<float>(std::clamp(1.0 - t, 0.0, 1.0));
}

bool LoadingScreen::visible() const
{
    return opacity() > 0.0f;
}

bool LoadingScreen::handle(const platform::Event& event)
{
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * scale_;
        mouseY_ = static_cast<float>(move->y) * scale_;
        return false; // everyone tracks the pointer
    }
    if (!blocking())
        return false;
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event);
        button && button->button == platform::MouseButton::Left) {
        const float x = static_cast<float>(button->x) * scale_, y = static_cast<float>(button->y) * scale_;
        const bool over = leave_.contains(x, y);
        if (button->pressed)
            pressedLeave_ = over;
        else if (std::exchange(pressedLeave_, false) && over && onLeave)
            onLeave();
    }
    return true;
}

void LoadingScreen::update(double time, Vector2 screen, float pixelScale)
{
    time_ = time;
    screen_ = screen;
    scale_ = pixelScale;
    if (dismissedAt_ && time_ - *dismissedAt_ >= static_cast<double>(fadeSeconds))
        remove();
    const float k = scale_;
    leave_ = {screen_.x - 52 * k, 16 * k, 36 * k, 36 * k};
}

void LoadingScreen::textAt(DrawList& list, std::string_view text, RectF box, float size, Enum::Font font,
                           Color3 color, float alpha, Enum::TextYAlignment y)
{
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(font);
    params.style.size = size * scale_;
    params.style.color = color;
    params.style.transparency = 1.0f - alpha;
    params.xAlignment = Enum::TextXAlignment::Center;
    params.yAlignment = y;
    params.box = {box.w, box.h};
    params.truncate = Enum::TextTruncate::AtEnd;
    list.text(*atlas_, layoutText(*fonts_, params), box.x, box.y, box);
}

void LoadingScreen::draw(DrawList& list)
{
    const float alpha = opacity();
    if (alpha <= 0.0f)
        return;
    const float k = scale_;
    const RectF screen{0, 0, screen_.x, screen_.y};
    list.rect(screen, Color4::from(background, 1.0f - alpha), everywhere);

    // The icon, its name and "By ..." as one block, a little above the middle.
    const float icon = iconSize * k;
    const float block = icon + (name_.empty() ? 0 : 52 * k) + (creator_.empty() ? 0 : 28 * k);
    const float top = std::round(screen_.y * 0.45f - block / 2);
    const RectF iconRect{std::round((screen_.x - icon) / 2), top, icon, icon};
    const float corner = 20 * k;
    Quad base;
    base.rect = iconRect;
    base.color = Color4::from(placeholder, 1.0f - alpha);
    base.radii = {corner, corner, corner, corner};
    list.push(base);
    if (auto image = images_ && !icon_.empty() ? images_(icon_) : nullptr; image && !image->empty()) {
        Quad q = base;
        q.kind = QuadKind::Image;
        q.texture = list.texture(image);
        q.color = {1.0f, 1.0f, 1.0f, alpha};
        list.push(q);
    }
    float y = iconRect.bottom() + 16 * k;
    const float textWidth = std::min(screen_.x - 64 * k, 720 * k);
    const float textX = std::round((screen_.x - textWidth) / 2);
    if (!name_.empty()) {
        textAt(list, name_, {textX, y, textWidth, 40 * k}, 32.0f, Enum::Font::BuilderSansBold, titleColor, alpha);
        y += 52 * k;
    }
    if (!creator_.empty())
        textAt(list, "By " + creator_, {textX, y - 8 * k, textWidth, 28 * k}, 20.0f, Enum::Font::BuilderSansMedium,
               detailColor, alpha);

    // The spinner: dots round a circle, the brightest one going round.
    const float cx = screen_.x / 2, cy = screen_.y - 112 * k;
    if (!error_) {
        const double turn = std::fmod(time_ / spinnerTurn, 1.0);
        const float radius = 14 * k, dot = 6 * k;
        for (int i = 0; i < spinnerDots; ++i) {
            const double at = static_cast<double>(i) / spinnerDots;
            const double angle = at * 2 * std::numbers::pi;
            const double behind = std::fmod(turn - at + 1.0, 1.0); // 0 for the leading dot
            Quad q;
            q.rect = {cx + radius * static_cast<float>(std::sin(angle)) - dot / 2,
                      cy - radius * static_cast<float>(std::cos(angle)) - dot / 2, dot, dot};
            q.radii = {dot / 2, dot / 2, dot / 2, dot / 2};
            q.color = Color4::from(titleColor, 1.0f - alpha * static_cast<float>(1.0 - 0.8 * behind));
            list.push(q);
        }
    }
    if (!status_.empty())
        textAt(list, status_, {textX, cy + 28 * k, textWidth, 24 * k}, 18.0f, Enum::Font::BuilderSansMedium,
               error_ ? errorColor : detailColor, alpha);

    // Leave, a cross in the top right corner.
    if (blocking() && leave_.contains(mouseX_, mouseY_)) {
        Quad hover;
        hover.rect = leave_;
        hover.color = {1.0f, 1.0f, 1.0f, pressedLeave_ ? 0.2f : 0.1f};
        hover.radii = {10 * k, 10 * k, 10 * k, 10 * k};
        list.push(hover);
    }
    const float lx = leave_.x + leave_.w / 2, ly = leave_.y + leave_.h / 2;
    for (const float angle : {0.25f, -0.25f}) {
        Quad q;
        q.rect = {lx - 9 * k, ly - k, 18 * k, 2 * k};
        q.color = Color4::from(titleColor, 1.0f - alpha);
        q.radii = {k, k, k, k};
        q.rotation = angle * std::numbers::pi_v<float>;
        q.pivotX = lx;
        q.pivotY = ly;
        list.push(q);
    }
}

} // namespace lunar::ui
