#include <lunar/ui/GameInvitePrompt.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace lunar::ui {
namespace {

constexpr Color3 white{1, 1, 1};
constexpr Color3 muted{0.7f, 0.72f, 0.76f};

void panel(DrawList& list, RectF rect, Color4 color, float radius, RectF clip = {-1e9f, -1e9f, 2e9f, 2e9f},
           float stroke = 0)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    Quad quad;
    quad.rect = rect;
    quad.color = color;
    quad.radii.fill(radius);
    quad.clip = clip;
    quad.stroke = stroke;
    list.push(quad);
}

std::string lower(std::string_view value)
{
    std::string out(value);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

void GameInvitePrompt::open(std::shared_ptr<GameInviteSession> session)
{
    session_ = std::move(session);
    search_.setText("");
    search_.maxLength = 256;
    focused_ = -3;
    pressed_.reset();
    scroll_ = 0;
    filter();
}

void GameInvitePrompt::close()
{
    // Clear first: the public close signal may open a different prompt.
    auto session = std::exchange(session_, {});
    filtered_.clear();
    pressed_.reset();
    focused_ = -3;
    if (session)
        session->close();
}

void GameInvitePrompt::filter()
{
    filtered_.clear();
    if (!visible())
        return;
    const auto needle = lower(search_.text);
    const auto& friends = session_->friends();
    for (std::size_t i = 0; i < friends.size(); ++i)
        if (needle.empty() || lower(friends[i].username).contains(needle)
            || lower(friends[i].displayName).contains(needle))
            filtered_.push_back(i);
    const float extent = static_cast<float>(filtered_.size()) * 64 * scale_;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(extent - rows_.h, 0.0f));
}

void GameInvitePrompt::update(double time, Vector2 screen, float scale)
{
    time_ = time;
    screen_ = screen;
    scale_ = scale;
    if (!session_)
        return;
    session_->poll();
    if (session_->closed()) {
        session_.reset();
        filtered_.clear();
        return;
    }
    const float k = scale_;
    const float width = std::max(1.0f, std::min(620 * k, screen.x - 32 * k));
    const float height = std::max(1.0f, std::min(620 * k, screen.y - 32 * k));
    panel_ = {(screen.x - width) / 2, (screen.y - height) / 2, width, height};
    closeRect_ = {panel_.right() - 48 * k, panel_.y + 12 * k, 32 * k, 32 * k};
    searchRect_ = {panel_.x + 24 * k, panel_.y + 104 * k, std::max(0.0f, width - 48 * k), 42 * k};
    rows_ = {searchRect_.x, searchRect_.bottom() + 16 * k, searchRect_.w, std::max(0.0f, height - 234 * k)};
    retryRect_ = {panel_.x + 24 * k, panel_.bottom() - 54 * k, 104 * k, 34 * k};
    filter();
}

RectF GameInvitePrompt::rowRect(std::size_t index) const
{
    return {rows_.x, rows_.y + static_cast<float>(index) * 64 * scale_ - scroll_, rows_.w, 60 * scale_};
}

RectF GameInvitePrompt::inviteRect(std::int64_t userId) const
{
    if (!visible())
        return {};
    const auto& friends = session_->friends();
    for (std::size_t i = 0; i < filtered_.size(); ++i)
        if (friends[filtered_[i]].userId == userId) {
            const auto row = rowRect(i);
            return {row.right() - 106 * scale_, row.y + 12 * scale_, 98 * scale_, 36 * scale_};
        }
    return {};
}

std::int64_t GameInvitePrompt::targetAt(float x, float y) const
{
    if (closeRect_.contains(x, y))
        return -1;
    if (!session_->loading() && session_->friends().empty() && !session_->error().empty() && retryRect_.contains(x, y))
        return -2;
    if (searchRect_.contains(x, y))
        return -3;
    if (rows_.contains(x, y)) {
        const auto index = static_cast<std::size_t>((y - rows_.y + scroll_) / (64 * scale_));
        if (index < filtered_.size()) {
            const auto id = session_->friends()[filtered_[index]].userId;
            if (inviteRect(id).contains(x, y))
                return id;
        }
    }
    return 0;
}

void GameInvitePrompt::activate(std::int64_t target)
{
    if (!visible())
        return;
    if (target == -1)
        close();
    else if (target == -2)
        session_->retry();
    else if (target > 0 && !session_->sending() && session_->status(target) != GameInviteStatus::Sent)
        session_->invite(target);
}

bool GameInvitePrompt::handle(const platform::Event& event)
{
    if (!visible())
        return false;
    if (std::holds_alternative<platform::MouseMoveEvent>(event))
        return true;
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        if (button->button != platform::MouseButton::Left)
            return true;
        const auto target = targetAt(static_cast<float>(button->x) * scale_, static_cast<float>(button->y) * scale_);
        if (button->pressed) {
            pressed_ = target;
            focused_ = target == 0 ? focused_ : target;
            if (target == -3) {
                search_.caret = search_.text.size();
                search_.anchor.reset();
            }
        } else {
            const auto held = std::exchange(pressed_, {});
            if (held && *held == target)
                activate(target);
        }
        return true;
    }
    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        scroll_ -= static_cast<float>(scroll->dy) * 48 * scale_;
        filter();
        return true;
    }
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (!key->pressed)
            return true;
        if (key->key == platform::Key::Escape) {
            close();
            return true;
        }
        if (key->key == platform::Key::Tab) {
            std::vector<std::int64_t> targets{-3};
            for (const auto index : filtered_) {
                const auto id = session_->friends()[index].userId;
                if (session_->status(id) != GameInviteStatus::Sent)
                    targets.push_back(id);
            }
            if (session_->friends().empty() && !session_->error().empty())
                targets.push_back(-2);
            targets.push_back(-1);
            auto at = std::ranges::find(targets, focused_);
            const auto offset = at == targets.end() ? 0 : static_cast<int>(at - targets.begin());
            const int count = static_cast<int>(targets.size());
            focused_ = targets[static_cast<std::size_t>((offset + (key->modifiers.shift ? count - 1 : 1)) % count)];
            if (focused_ > 0) {
                for (std::size_t i = 0; i < filtered_.size(); ++i)
                    if (session_->friends()[filtered_[i]].userId == focused_) {
                        const auto row = rowRect(i);
                        if (row.y < rows_.y)
                            scroll_ -= rows_.y - row.y;
                        else if (row.bottom() > rows_.bottom())
                            scroll_ += row.bottom() - rows_.bottom();
                    }
            }
        } else if (focused_ == -3) {
            if (search_.key(*key) == TextEditor::Result::Edited) {
                scroll_ = 0;
                filter();
            }
        } else if (!key->repeat && (key->key == platform::Key::Enter || key->key == platform::Key::Space))
            activate(focused_);
        return true;
    }
    if (const auto* typed = std::get_if<platform::TextEvent>(&event)) {
        if (focused_ == -3) {
            search_.insert(typed->text);
            scroll_ = 0;
            filter();
        }
        return true;
    }
    return true;
}

void GameInvitePrompt::text(DrawList& list, std::string_view value, RectF rect, float size, Color3 color, bool bold,
                            std::optional<RectF> clip)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    TextParams params;
    params.text = value;
    params.style.font = fontFromEnum(bold ? Enum::Font::BuilderSansBold : Enum::Font::BuilderSans);
    params.style.size = size * scale_;
    params.style.color = color;
    params.box = {rect.w, rect.h};
    params.truncate = Enum::TextTruncate::AtEnd;
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Center;
    list.text(*atlas_, layoutText(*fonts_, params), rect.x, rect.y, clip.value_or(rect).intersect(rect));
}

void GameInvitePrompt::draw(DrawList& list)
{
    if (!visible())
        return;
    const float k = scale_;
    panel(list, {0, 0, screen_.x, screen_.y}, {0, 0, 0, 0.62f}, 0);
    panel(list, panel_, {0.1f, 0.11f, 0.13f, 1}, 14 * k);
    text(list, "Invite friends", {panel_.x + 24 * k, panel_.y + 18 * k, panel_.w - 96 * k, 30 * k}, 24, white, true);
    const auto& message = session_->options().promptMessage;
    text(list, message.empty() ? "Invite a friend to join this experience." : message,
         {panel_.x + 24 * k, panel_.y + 58 * k, panel_.w - 48 * k, 30 * k}, 15, muted);
    text(list, "×", closeRect_, 28, white);
    if (focused_ == -1)
        panel(list, closeRect_, {1, 1, 1, 0.6f}, 5 * k, panel_, k);
    panel(list, searchRect_, {0.18f, 0.19f, 0.22f, 1}, 7 * k);
    if (focused_ == -3)
        panel(list, searchRect_, {0.4f, 0.63f, 1, 1}, 7 * k, panel_, k);
    const RectF input{searchRect_.x + 12 * k, searchRect_.y, searchRect_.w - 24 * k, searchRect_.h};
    text(list, search_.text.empty() ? "Search friends" : search_.text, input, 16, search_.text.empty() ? muted : white);
    if (focused_ == -3 && std::fmod(time_, 1.0) < 0.5) {
        TextParams caret;
        caret.text = std::string_view(search_.text).substr(0, search_.caret);
        caret.style.font = fontFromEnum(Enum::Font::BuilderSans);
        caret.style.size = 16 * k;
        caret.box = {1e6f, input.h};
        const auto shaped = layoutText(*fonts_, caret);
        const float x = std::min(input.x + shaped.bounds.x, input.right() - k);
        panel(list, {x, input.y + 11 * k, k, 20 * k}, {1, 1, 1, 1}, 0, input);
    }
    const auto& friends = session_->friends();
    if (session_->loading())
        text(list, "Loading friends…", rows_, 17, muted);
    else if (friends.empty() && session_->error().empty())
        text(list, "No friends are available to invite.", rows_, 17, muted);
    else if (!friends.empty() && filtered_.empty())
        text(list, "No friends match your search.", rows_, 17, muted);
    for (std::size_t i = 0; i < filtered_.size(); ++i) {
        const auto row = rowRect(i);
        if (row.bottom() <= rows_.y || row.y >= rows_.bottom())
            continue;
        const auto& person = friends[filtered_[i]];
        const auto status = session_->status(person.userId);
        panel(list, row, {1, 1, 1, 0.035f}, 7 * k, rows_);
        const RectF avatar{row.x + 8 * k, row.y + 9 * k, 42 * k, 42 * k};
        panel(list, avatar, {0.25f, 0.26f, 0.3f, 1}, 21 * k, rows_);
        if (images_)
            if (auto image = images_(std::format("rbxthumb://type=AvatarHeadShot&id={}&w=150&h=150", person.userId));
                image && !image->empty()) {
                Quad quad;
                quad.rect = avatar;
                quad.clip = rows_;
                quad.radii.fill(21 * k);
                quad.kind = QuadKind::Image;
                quad.color = {1, 1, 1, 1};
                quad.texture = list.texture(image);
                list.push(quad);
            }
        const std::string name
            = person.displayName.empty() ? std::format("User {}", person.userId) : person.displayName;
        text(list, name, {row.x + 62 * k, row.y + 7 * k, row.w - 180 * k, 25 * k}, 17, white, true, rows_);
        const std::string detail
            = (person.online ? "Online · " : "") + (person.username.empty() ? std::string() : "@" + person.username);
        text(list, detail, {row.x + 62 * k, row.y + 30 * k, row.w - 180 * k, 23 * k}, 13, muted, false, rows_);
        const auto button = inviteRect(person.userId);
        const bool enabled = !session_->sending() && status != GameInviteStatus::Sent;
        panel(list, button,
              status == GameInviteStatus::Sent ? Color4{0.13f, 0.3f, 0.2f, 1}
              : enabled                        ? Color4{0.23f, 0.46f, 0.85f, 1}
                                               : Color4{0.25f, 0.27f, 0.31f, 1},
              6 * k, rows_);
        const char* label = status == GameInviteStatus::Sent      ? "Invited"
                            : status == GameInviteStatus::Sending ? "Sending…"
                            : status == GameInviteStatus::Failed  ? "Retry"
                                                                  : "Invite";
        text(list, label, {button.x + 12 * k, button.y, button.w - 20 * k, button.h}, 14, white, false, rows_);
        if (focused_ == person.userId)
            panel(list, button, {1, 1, 1, 0.8f}, 6 * k, rows_, k);
    }
    const float extent = static_cast<float>(filtered_.size()) * 64 * k;
    if (extent > rows_.h && rows_.h > 0) {
        const float length = std::max(20 * k, rows_.h * rows_.h / extent);
        const float at = scroll_ / std::max(extent - rows_.h, k) * (rows_.h - length);
        panel(list, {rows_.right() + 7 * k, rows_.y + at, 4 * k, length}, {1, 1, 1, 0.3f}, 2 * k);
    }
    if (!session_->error().empty()) {
        text(list, session_->error(),
             friends.empty() ? rows_ : RectF{panel_.x + 24 * k, panel_.bottom() - 72 * k, panel_.w - 48 * k, 26 * k},
             13, {1, 0.62f, 0.6f});
        if (friends.empty() && !session_->loading()) {
            panel(list, retryRect_, {0.25f, 0.27f, 0.31f, 1}, 6 * k);
            text(list, "Try again", {retryRect_.x + 12 * k, retryRect_.y, retryRect_.w - 20 * k, retryRect_.h}, 14,
                 white);
            if (focused_ == -2)
                panel(list, retryRect_, {1, 1, 1, 0.8f}, 6 * k, panel_, k);
        }
    } else
        text(list, "Choose Invite to send an invitation.",
             {panel_.x + 24 * k, panel_.bottom() - 50 * k, panel_.w - 48 * k, 28 * k}, 13, muted);
}

} // namespace lunar::ui
