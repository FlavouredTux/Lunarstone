#include <lunar/ui/Notifications.hpp>

#include <algorithm>

namespace lunar::ui {

void Notifications::post(StarterGui::Notification notification)
{
    if (pending_.size() >= 50)
        pending_.pop_front();
    pending_.push_back({std::move(notification), -1, ++nextId_});
}

void Notifications::update(double time, Vector2 screen, float scale)
{
    time_ = time;
    screen_ = screen;
    scale_ = std::max(scale, 0.001f);
    std::erase_if(pending_, [&](const Entry& entry) {
        return entry.shown >= 0 && time - entry.shown >= entry.notification.duration;
    });
    for (std::size_t i = 0; i < std::min(pending_.size(), std::size_t{3}); ++i)
        if (pending_[i].shown < 0)
            pending_[i].shown = time;
}

RectF Notifications::rect(std::size_t index) const
{
    if (index >= std::min(pending_.size(), std::size_t{3}))
        return {};
    const float k = scale_;
    const float width = std::min(340 * k, std::max(screen_.x - 16 * k, 0.0f));
    float bottom = screen_.y - 8 * k;
    for (std::size_t i = 0; i <= index; ++i) {
        const float height = (pending_[i].notification.buttons.empty() ? 100 : 140) * k;
        if (i == index)
            return {screen_.x - width - 8 * k, bottom - height, width, height};
        bottom -= height + 8 * k;
    }
    return {};
}

RectF Notifications::buttonRect(std::size_t index, std::size_t button) const
{
    const RectF card = rect(index);
    if (card.w <= 0 || button >= pending_[index].notification.buttons.size())
        return {};
    const auto count = std::min(pending_[index].notification.buttons.size(), std::size_t{2});
    const float width = (card.w - 24 * scale_ - static_cast<float>(count - 1) * 8 * scale_) / static_cast<float>(count);
    return {card.x + 12 * scale_ + static_cast<float>(button) * (width + 8 * scale_), card.bottom() - 44 * scale_,
            width, 32 * scale_};
}

void Notifications::text(DrawList& list, std::string_view value, RectF box, float size, bool bold)
{
    TextParams params;
    params.text = value;
    params.style.font = fontFromEnum(bold ? Enum::Font::BuilderSansBold : Enum::Font::BuilderSans);
    params.style.color = {1, 1, 1};
    params.style.size = size * scale_;
    params.box = {box.w, box.h};
    params.wrapped = true;
    params.truncate = Enum::TextTruncate::AtEnd;
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Top;
    list.text(*atlas_, layoutText(*fonts_, params), box.x, box.y, box);
}

void Notifications::draw(DrawList& list)
{
    const float k = scale_;
    for (std::size_t i = 0; i < std::min(pending_.size(), std::size_t{3}); ++i) {
        const RectF card = rect(i);
        const auto& notification = pending_[i].notification;
        Quad background;
        background.rect = card;
        background.radii.fill(8 * k);
        background.color = {0.1f, 0.11f, 0.13f, 0.97f};
        list.push(background);
        float offset = 12 * k;
        if (images_ && !notification.icon.empty())
            if (auto icon = images_(notification.icon); icon && !icon->empty()) {
                Quad image;
                image.rect = {card.x + 12 * k, card.y + 14 * k, 48 * k, 48 * k};
                image.color = {1, 1, 1, 1};
                image.kind = QuadKind::Image;
                image.texture = list.texture(icon);
                list.push(image);
                offset = 72 * k;
            }
        text(list, notification.title, {card.x + offset, card.y + 12 * k, card.w - offset - 32 * k, 24 * k}, 16, true);
        text(list, notification.text, {card.x + offset, card.y + 39 * k, card.w - offset - 12 * k, 52 * k}, 14);
        text(list, "×", {card.right() - 26 * k, card.y + 9 * k, 20 * k, 24 * k}, 20);
        for (std::size_t j = 0; j < std::min(notification.buttons.size(), std::size_t{2}); ++j) {
            const RectF button = buttonRect(i, j);
            Quad quad;
            quad.rect = button;
            quad.radii.fill(6 * k);
            quad.color = {1, 1, 1, 0.12f};
            list.push(quad);
            text(list, notification.buttons[j],
                 {button.x + 10 * k, button.y + 6 * k, button.w - 20 * k, button.h - 6 * k}, 14);
        }
    }
}

bool Notifications::handle(const platform::Event& event)
{
    const auto* button = std::get_if<platform::MouseButtonEvent>(&event);
    if (!button || button->button != platform::MouseButton::Left)
        return false;
    const float x = static_cast<float>(button->x) * scale_;
    const float y = static_cast<float>(button->y) * scale_;
    for (std::size_t i = 0; i < std::min(pending_.size(), std::size_t{3}); ++i) {
        const RectF card = rect(i);
        if (!card.contains(x, y))
            continue;
        int target = -1;
        if (RectF{card.right() - 30 * scale_, card.y, 30 * scale_, 36 * scale_}.contains(x, y))
            target = -2;
        for (std::size_t j = 0; j < std::min(pending_[i].notification.buttons.size(), std::size_t{2}); ++j)
            if (buttonRect(i, j).contains(x, y))
                target = static_cast<int>(j);
        if (button->pressed) {
            pressed_ = Press{pending_[i].id, target};
            return true;
        }
        const auto pressed = std::exchange(pressed_, std::nullopt);
        if (!pressed || pressed->id != pending_[i].id || pressed->button != target)
            return true;
        const auto close = [&] { pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i)); };
        if (target == -2) {
            close();
            return true;
        }
        for (std::size_t j = 0; j < std::min(pending_[i].notification.buttons.size(), std::size_t{2}); ++j)
            if (buttonRect(i, j).contains(x, y)) {
                auto callback = std::move(pending_[i].notification.choose);
                std::string choice = pending_[i].notification.buttons[j];
                close();
                if (callback)
                    callback(std::move(choice));
                return true;
            }
        return true;
    }
    return !button->pressed && std::exchange(pressed_, std::nullopt).has_value();
}

} // namespace lunar::ui
