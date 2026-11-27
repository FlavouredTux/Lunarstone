#include <lunar/ui/GameMenu.hpp>

#include <lunar/ui/ClassicChat.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/LuauHighlight.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>
#include <string_view>
#include <utility>

namespace lunar::ui {

namespace {

using X = Enum::TextXAlignment;
using Y = Enum::TextYAlignment;

// The look: Roblox's current menu for the feel (a dark translucent panel,
// white type, the selected tab lit), Builder Sans like it.
constexpr Enum::Font regular = Enum::Font::BuilderSans;
constexpr Enum::Font medium = Enum::Font::BuilderSansMedium;
constexpr Enum::Font bold = Enum::Font::BuilderSansBold;

constexpr Color4 panelColor{0.075f, 0.08f, 0.095f, 0.95f};
constexpr Color4 dialogColor{0.13f, 0.135f, 0.155f, 1.0f};
constexpr Color4 white{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color4 secondary{0.72f, 0.74f, 0.78f, 1.0f};
constexpr Color4 disabled{1.0f, 1.0f, 1.0f, 0.32f};
constexpr Color4 fill{1.0f, 1.0f, 1.0f, 0.07f};
constexpr Color4 hoverFill{1.0f, 1.0f, 1.0f, 0.13f};
constexpr Color4 pressFill{1.0f, 1.0f, 1.0f, 0.19f};
constexpr Color4 line{1.0f, 1.0f, 1.0f, 0.08f};
constexpr Color4 accent{0.2f, 0.373f, 1.0f, 1.0f}; // Roblox's blue, #335FFF
constexpr Color4 dark{0.07f, 0.075f, 0.09f, 1.0f};
constexpr Color4 warning{1.0f, 0.74f, 0.3f, 1.0f};

// Luau as the editor colours it, to go with the log's.
constexpr Color4 codeKeyword{0.55f, 0.68f, 1.0f, 1.0f};
constexpr Color4 codeString{0.55f, 0.85f, 0.63f, 1.0f};
constexpr Color4 codeNumber{0.72f, 0.79f, 1.0f, 1.0f};
constexpr Color4 codeComment{0.5f, 0.53f, 0.58f, 1.0f};
constexpr float codeTextSize = 14.0f;   // the editor's type, the log's
constexpr float gutterTextSize = 12.0f; // and its line numbers smaller still

constexpr std::array<std::string_view, 7> tabNames{"Home", "Players", "Settings", "Report", "Help", "Music", "Luau"};
constexpr std::array<std::string_view, 7> tabIds{"tab.home", "tab.players", "tab.settings", "tab.report",
                                                 "tab.help", "tab.music",   "tab.luau"};
// The client's own icons; the Music and Luau tabs draw their own
// (GameMenu::musicIcon, GameMenu::luauIcon).
constexpr std::array<std::string_view, 7> tabIcons{"HomeTab", "PlayersTabIcon", "GameSettingsTab", "ReportAbuseTab",
                                                   "HelpTab", "",               ""};

// ReportAbuseMenu's abuse types, from memory (GUESS).
constexpr std::array<std::string_view, 8> playerReasons{
    "Swearing", "Inappropriate Username", "Bullying",          "Scamming",
    "Dating",   "Cheating/Exploiting",    "Personal Question", "Offsite Links",
};
constexpr std::array<std::string_view, 3> experienceReasons{"Inappropriate Content", "Bad Model or Script",
                                                            "Offsite Link"};

constexpr std::array<render::GraphicsPreset, 4> presets{render::GraphicsPreset::Potato, render::GraphicsPreset::Minimal,
                                                        render::GraphicsPreset::Average, render::GraphicsPreset::Max};
constexpr std::array<std::string_view, 4> presetNames{"Potato", "Minimal", "Average", "Max"};

Color4 faded(Color4 c, float alpha)
{
    c.a *= alpha;
    return c;
}

RectF grow(RectF r, float by)
{
    return {r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by};
}

const std::string& shownName(const Player& p)
{
    return p.displayName().empty() ? p.name() : p.displayName();
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The first character of a name, for its avatar badge.
std::string initial(std::string_view name)
{
    if (name.empty())
        return "?";
    const auto lead = static_cast<unsigned char>(name[0]);
    const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    std::string out(name.substr(0, std::min(length, name.size())));
    if (length == 1)
        out[0] = static_cast<char>(std::toupper(lead));
    return out;
}

std::string_view keyLabel(platform::Key key)
{
    return platform::keyName(key);
}

// UTF-8's continuation bytes: not a place to put a caret.
bool continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

Color4 tokenColor(LuauToken token)
{
    switch (token) {
    case LuauToken::Keyword:
        return codeKeyword;
    case LuauToken::String:
        return codeString;
    case LuauToken::Number:
        return codeNumber;
    case LuauToken::Comment:
        return codeComment;
    case LuauToken::Code:
        break;
    }
    return white;
}

} // namespace

std::span<const std::string_view> GameMenu::reasons(bool aboutPlayer)
{
    if (aboutPlayer)
        return playerReasons;
    return experienceReasons;
}

GameMenu::GameMenu(CoreGui& owner, FontLibrary& fonts, GlyphAtlas& atlas)
    : owner_(&owner), fonts_(&fonts), atlas_(&atlas)
{
    report_.description.maxLength = 200;
    luauInput_.multiLine = true; // the Luau tab's editor takes newlines
}

void GameMenu::setGame(DataModel* game)
{
    game_ = game ? game->shared_from_this() : nullptr;
}

void GameMenu::setOpen(bool open, bool animate)
{
    if (open && !open_) {
        // Each time it opens: the Home tab, nothing focused.
        tab_ = Tab::Home;
        focus_.reset();
        focusVisible_ = false;
        report_.notice = false;
    }
    if (!open) {
        dialog_ = Dialog::None;
        typing_ = false;
    }
    pressed_.reset();
    dragging_.reset();
    open_ = open;
    if (animate) {
        slideFrom_ = shown_;
        slideT_ = 0.0f;
    } else {
        shown_ = open ? 1.0f : 0.0f;
        slideT_ = 1e9f;
    }
}

void GameMenu::selectTab(Tab tab)
{
    if (tab_ != tab) {
        focus_.reset();
        typing_ = false;
        // The log opens at its newest lines.
        if (tab == Tab::Luau)
            scrollToEnd_ = true;
    }
    tab_ = tab;
}

void GameMenu::openDialog(Dialog dialog)
{
    if (dialog == Dialog::Reset && !owner_->resetEnabled())
        return;
    dialog_ = dialog;
    typing_ = false;
    pressed_.reset();
    dragging_.reset();
    // The confirmation is focused, so Enter goes ahead (the ring shows after keys).
    focus_ = dialog == Dialog::None ? std::nullopt : std::optional<std::string>("dialog.confirm");
}

void GameMenu::closeDialogOrMenu()
{
    if (dialog_ != Dialog::None) {
        openDialog(Dialog::None);
        return;
    }
    setOpen(false, true);
}

std::optional<RectF> GameMenu::controlRect(std::string_view id) const
{
    for (const Control& c : controls_)
        if (c.id == id)
            return c.rect;
    return std::nullopt;
}

std::optional<std::string> GameMenu::focused() const
{
    return focus_;
}

// --- Input ----------------------------------------------------------------------

bool GameMenu::handle(const platform::Event& event)
{
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * k_;
        mouseY_ = static_cast<float>(move->y) * k_;
        if (dragging_)
            for (const Control& c : controls_)
                if (c.id == *dragging_ && c.drag)
                    c.drag(std::clamp((mouseX_ - c.rect.x) / std::max(c.rect.w, 1.0f), 0.0f, 1.0f));
        return false;
    }
    if (!open_) {
        // Closed, it only hears the Escape that opens it.
        const auto* key = std::get_if<platform::KeyEvent>(&event);
        if (key && key->pressed && !key->repeat && key->key == platform::Key::Escape) {
            setOpen(true, true);
            return true;
        }
        return false;
    }

    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (typing_) {
            if (!key->pressed)
                return true;
            const bool luau = tab_ == Tab::Luau;
            // The editor's arrows move through its lines; only from the first
            // (Up) or the last (Down) do they step out of it, into the
            // history, the way a shell goes through what ran before.
            if (luau && (key->key == platform::Key::Up || key->key == platform::Key::Down)) {
                const std::size_t line = editing().caretLine();
                const std::size_t lines = editing().lineCount();
                if ((key->key == platform::Key::Up && line == 0)
                    || (key->key == platform::Key::Down && line + 1 == lines)) {
                    browseHistory(key->key == platform::Key::Up ? -1 : 1);
                    return true;
                }
            } else if (luau && key->key == platform::Key::Tab) {
                // Tab indents the code; Shift+Tab, and Tab in an empty editor,
                // still change tab the way they do over the rest of the menu.
                const int step = key->modifiers.shift ? tabCount - 1 : 1;
                if (key->modifiers.shift || editing().text.empty())
                    selectTab(static_cast<Tab>((static_cast<int>(tab_) + step) % tabCount));
                else {
                    editing().insert("    ");
                    caretTime_ = time_;
                    historyAt_ = -1;
                }
                return true;
            } else if (luau && key->key == platform::Key::L && key->modifiers.ctrl) {
                // Ctrl+L clears the log, like the console's own does.
                owner_->console().clear();
                scrollToEnd_ = true;
                return true;
            }
            switch (editing().key(*key)) {
            case TextEditor::Result::Submitted:
                if (tab_ == Tab::Luau) {
                    runLuau();
                } else {
                    typing_ = false;
                    focus_ = "report.submit";
                    focusVisible_ = true;
                }
                break;
            case TextEditor::Result::Cancelled:
                typing_ = false;
                break;
            case TextEditor::Result::Edited:
                caretTime_ = time_;
                report_.notice = false;
                if (luau)
                    historyAt_ = -1; // whatever is typed is a draft again
                break;
            case TextEditor::Result::None:
                break;
            }
            return true;
        }
        if (!key->pressed)
            return true;
        using platform::Key;
        const bool once = !key->repeat;
        switch (key->key) {
        case Key::Escape:
            if (once)
                closeDialogOrMenu();
            break;
        case Key::R:
            if (once && dialog_ == Dialog::None)
                openDialog(Dialog::Reset);
            focusVisible_ = true;
            break;
        case Key::L:
            if (once && dialog_ == Dialog::None)
                openDialog(Dialog::Leave);
            focusVisible_ = true;
            break;
        case Key::Tab:
            if (dialog_ != Dialog::None) {
                moveFocus(key->modifiers.shift ? -1 : 1);
            } else {
                const int next = (static_cast<int>(tab_) + (key->modifiers.shift ? tabCount - 1 : 1)) % tabCount;
                selectTab(static_cast<Tab>(next));
            }
            break;
        case Key::Up:
            moveFocus(-1);
            break;
        case Key::Down:
            moveFocus(1);
            break;
        case Key::Left:
            stepFocused(-1);
            break;
        case Key::Right:
            stepFocused(1);
            break;
        case Key::Enter:
        case Key::KeypadEnter:
        case Key::Space:
            if (once)
                pressFocused();
            break;
        default:
            break;
        }
        return true;
    }

    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        mouseX_ = static_cast<float>(button->x) * k_;
        mouseY_ = static_cast<float>(button->y) * k_;
        if (button->button != platform::MouseButton::Left)
            return true;
        // The control under the pointer: the last laid out wins (a
        // selector's arrows over the selector), a dialog's only.
        const Control* hit = nullptr;
        for (auto it = controls_.rbegin(); it != controls_.rend(); ++it)
            if (hovered(*it)) {
                hit = &*it;
                break;
            }
        if (button->pressed) {
            focusVisible_ = false;
            pressed_ = hit ? std::optional<std::string>(hit->id) : std::nullopt;
            if (typing_ && (!hit || hit->id != editingId()))
                typing_ = false;
            if (hit && hit->drag) {
                dragging_ = hit->id;
                hit->drag(std::clamp((mouseX_ - hit->rect.x) / std::max(hit->rect.w, 1.0f), 0.0f, 1.0f));
            }
            // A click on the dimmed screen beside the panel closes it (a dialog first).
            if (!hit && !panel_.contains(mouseX_, mouseY_))
                closeDialogOrMenu();
        } else {
            if (dragging_) {
                dragging_.reset();
            } else if (hit && pressed_ && *pressed_ == hit->id && hit->press) {
                if (hit->focusable)
                    focus_ = hit->id;
                const auto press = hit->press; // it may change the controls
                press();
            }
            pressed_.reset();
        }
        return true;
    }

    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        if (dialog_ == Dialog::None && content_.contains(mouseX_, mouseY_))
            this->scroll() -= static_cast<float>(scroll->dy) * 48.0f * k_;
        return true;
    }
    if (const auto* typed = std::get_if<platform::TextEvent>(&event)) {
        if (typing_) {
            editing().insert(typed->text);
            caretTime_ = time_;
            report_.notice = false;
            if (tab_ == Tab::Luau)
                historyAt_ = -1;
        }
        return true;
    }
    return false;
}

void GameMenu::moveFocus(int by)
{
    focusVisible_ = true;
    const bool inDialog = dialog_ != Dialog::None;
    std::vector<const Control*> ring;
    for (const Control& c : controls_)
        if (c.focusable && c.enabled && c.id.starts_with("dialog.") == inDialog)
            ring.push_back(&c);
    if (ring.empty()) {
        // Nothing to focus (the player list): the keys scroll.
        scroll() += static_cast<float>(by) * 48.0f * k_;
        return;
    }
    auto at = std::ranges::find_if(ring, [&](const Control* c) { return focus_ && c->id == *focus_; });
    int index = 0;
    if (at == ring.end())
        index = by > 0 ? 0 : static_cast<int>(ring.size()) - 1;
    else if (inDialog)
        index = (static_cast<int>(at - ring.begin()) + by + static_cast<int>(ring.size()))
                % static_cast<int>(ring.size());
    else
        index = std::clamp(static_cast<int>(at - ring.begin()) + by, 0, static_cast<int>(ring.size()) - 1);
    const Control& next = *ring[static_cast<std::size_t>(index)];
    focus_ = next.id;
    // Keep it in view.
    if (next.content) {
        const float pad = 12.0f * k_;
        if (next.rect.y < content_.y)
            scroll() -= content_.y - next.rect.y + pad;
        else if (next.rect.bottom() > content_.bottom())
            scroll() += next.rect.bottom() - content_.bottom() + pad;
    }
}

void GameMenu::pressFocused()
{
    focusVisible_ = true;
    for (const Control& c : controls_)
        if (focus_ && c.id == *focus_ && c.enabled && c.press) {
            const auto press = c.press;
            press();
            return;
        }
}

void GameMenu::stepFocused(int by)
{
    focusVisible_ = true;
    for (const Control& c : controls_)
        if (focus_ && c.id == *focus_ && c.enabled && c.step) {
            const auto step = c.step;
            step(by);
            return;
        }
    // Side by side (a dialog's buttons): the arrows move between them.
    if (dialog_ != Dialog::None)
        moveFocus(by);
}

// --- Layout and drawing ----------------------------------------------------------

void GameMenu::update(double time, Vector2 screen, float topInset, float pixelScale)
{
    const float dt = lastTime_ < 0.0 ? 0.0f : std::clamp(static_cast<float>(time - lastTime_), 0.0f, 0.1f);
    lastTime_ = time;
    time_ = time;
    screen_ = screen;
    top_ = topInset * pixelScale;
    k_ = pixelScale;
    // The slide: an ease-out cubic from wherever it was to where it's going.
    const float length = open_ ? openSeconds : closeSeconds;
    if (slideT_ < length) {
        slideT_ += dt;
        const float t = std::min(slideT_ / length, 1.0f);
        const float eased = 1.0f - std::pow(1.0f - t, 3.0f);
        shown_ = slideFrom_ + ((open_ ? 1.0f : 0.0f) - slideFrom_) * eased;
    }
    layout(nullptr);
}

void GameMenu::draw(DrawList& list)
{
    layout(&list);
}

void GameMenu::layout(DrawList* list)
{
    controls_.clear();
    if (!visible())
        return;
    const float k = k_;
    const RectF all{0, 0, screen_.x, screen_.y};
    clip_ = all;
    inContent_ = false;
    if (list)
        list->rect(all, {0, 0, 0, 0.38f * shown_}, all);

    // The panel: a card down the right edge under the top bar, slid off to
    // the right by what's left of the slide.
    const float margin = (screen_.x < 560 * k ? 8.0f : 12.0f) * k;
    const float width = std::min(panelWidth * k, screen_.x - 2 * margin);
    const float top = top_ + margin;
    const float height = std::max(screen_.y - top - margin, 120 * k);
    const float slide = (1.0f - shown_) * (width + margin + 48 * k);
    const RectF panel{std::round(screen_.x - margin - width + slide), top, std::round(width), std::round(height)};
    panel_ = panel;
    const float radius = 16 * k;
    if (list) {
        // A soft shadow: rings fading out round the card.
        for (int i = 1; i <= 6; ++i)
            box(list, grow(panel, static_cast<float>(i) * 3 * k), {0, 0, 0, 0.045f * shown_},
                radius + static_cast<float>(i) * 3 * k);
        box(list, panel, panelColor, radius);
        box(list, panel, line, radius, std::max(1.0f, std::round(k)));
    }
    clip_ = panel;
    drawHeader(list, panel);
    const RectF bar{panel.x + 12 * k, panel.y + 60 * k, panel.w - 24 * k, 62 * k};
    drawTabs(list, bar);
    if (list)
        list->rect({panel.x + 16 * k, bar.bottom() + 8 * k, panel.w - 32 * k, std::max(1.0f, std::round(k))}, line,
                   panel);

    // The tab's content, scrolled.
    const float contentTop = bar.bottom() + 9 * k;
    content_ = {panel.x + 20 * k, contentTop, panel.w - 40 * k, std::max(panel.bottom() - contentTop - 4 * k, 0.0f)};
    float& offset = scroll();
    clip_ = content_;
    inContent_ = true;
    const RectF area{content_.x, content_.y + 16 * k - std::round(offset), content_.w, content_.h};
    switch (tab_) {
    case Tab::Home: contentHeight_ = drawHome(list, area); break;
    case Tab::Players: contentHeight_ = drawPlayers(list, area); break;
    case Tab::Settings: contentHeight_ = drawSettings(list, area); break;
    case Tab::Report: contentHeight_ = drawReport(list, area); break;
    case Tab::Help: contentHeight_ = drawHelp(list, area); break;
    case Tab::Music: contentHeight_ = drawMusic(list, area); break;
    case Tab::Luau: contentHeight_ = drawLuau(list, area); break;
    }
    contentHeight_ += 32 * k; // the padding above and below
    inContent_ = false;
    clip_ = panel;
    if (std::exchange(scrollToEnd_, false))
        offset = contentHeight_;
    offset = std::clamp(offset, 0.0f, std::max(0.0f, contentHeight_ - content_.h));
    if (list && contentHeight_ > content_.h) {
        const float thumb = std::max(24 * k, content_.h * content_.h / contentHeight_);
        const float at = (content_.h - thumb) * offset / (contentHeight_ - content_.h);
        box(list, {panel.right() - 8 * k, content_.y + at, 3 * k, thumb}, {1, 1, 1, 0.3f}, 1.5f * k);
    }
    if (dialog_ != Dialog::None)
        drawDialog(list, panel);
}

void GameMenu::drawHeader(DrawList* list, const RectF& panel)
{
    const float k = k_;
    text(list, title_.empty() ? "Menu" : title_, {panel.x + 24 * k, panel.y + 8 * k, panel.w - 96 * k, 52 * k}, 24,
         bold, white);
    // The close button: an X in a rounded square.
    const RectF close{panel.right() - 52 * k, panel.y + 14 * k, 36 * k, 36 * k};
    Control& c = add({.id = "header.close", .rect = close, .focusable = false, .press = [this] { setOpen(false, true); }});
    if (!list)
        return;
    if (hovered(c))
        box(list, close, pressed_ == c.id ? pressFill : hoverFill, 10 * k);
    const float cx = close.x + close.w / 2, cy = close.y + close.h / 2;
    for (const float angle : {0.25f, -0.25f}) {
        Quad q;
        q.rect = {cx - 8 * k, cy - k, 16 * k, 2 * k};
        q.color = white;
        q.radii = {k, k, k, k};
        q.rotation = angle * std::numbers::pi_v<float>;
        q.pivotX = cx;
        q.pivotY = cy;
        q.clip = clip_;
        list->push(q);
    }
}

void GameMenu::drawTabs(DrawList* list, const RectF& bar)
{
    const float k = k_;
    const float cell = bar.w / tabCount;
    const bool hi = k >= 1.5f;
    for (int i = 0; i < tabCount; ++i) {
        const Tab tab = static_cast<Tab>(i);
        const RectF r{std::round(bar.x + static_cast<float>(i) * cell + 2 * k), bar.y, std::round(cell - 4 * k), bar.h};
        Control& c = add({.id = std::string(tabIds[static_cast<std::size_t>(i)]),
                          .rect = r,
                          .focusable = false,
                          .press = [this, tab] { selectTab(tab); }});
        if (!list)
            continue;
        const bool selected = tab_ == tab;
        if (selected)
            box(list, r, {1, 1, 1, 0.12f}, 12 * k);
        else if (hovered(c))
            box(list, r, {1, 1, 1, 0.06f}, 12 * k);
        const Color4 tint = selected ? white : faded(white, 0.6f);
        const RectF iconBox{r.x + r.w / 2 - 12 * k, r.y + 9 * k, 24 * k, 24 * k};
        if (tab == Tab::Luau) {
            luauIcon(list, iconBox, tint);
        } else if (tab == Tab::Music) {
            musicIcon(list, iconBox, tint);
        } else {
            const std::string icon = std::format("rbxasset://textures/ui/Settings/MenuBarIcons/{}{}.png",
                                                 tabIcons[static_cast<std::size_t>(i)], hi ? "@2x" : "");
            image(list, icon, iconBox, tint);
        }
        text(list, tabNames[static_cast<std::size_t>(i)], {r.x, r.y + 37 * k, r.w, 18 * k}, 15,
             selected ? bold : medium, tint, X::Center);
        if (selected)
            box(list, {r.x + r.w / 2 - 10 * k, r.bottom() - 4 * k, 20 * k, 3 * k}, accent, 1.5f * k);
    }
}

float GameMenu::drawHome(DrawList* list, RectF area)
{
    const float k = k_;
    float y = area.y;
    const float h = 56 * k, gap = 10 * k;
    button(list, "home.resume", {area.x, y, area.w, h}, "Resume", "Esc", ButtonStyle::Primary,
           [this] { setOpen(false, true); });
    y += h + gap;
    button(list, "home.reset", {area.x, y, area.w, h}, "Reset Character", "R", ButtonStyle::Secondary,
           [this] { openDialog(Dialog::Reset); }, owner_->resetEnabled());
    y += h + gap;
    button(list, "home.leave", {area.x, y, area.w, h}, "Leave", "L", ButtonStyle::Secondary,
           [this] { openDialog(Dialog::Leave); });
    y += h + 28 * k;

    // About this session.
    text(list, "This server", {area.x + 4 * k, y, area.w, 20 * k}, 15, bold, secondary);
    y += 28 * k;
    Player* local = nullptr;
    const std::vector<Player*> players = sortedPlayers(&local);
    const int minutes = static_cast<int>(time_ / 60.0);
    const int seconds = static_cast<int>(time_) % 60;
    std::vector<std::pair<std::string, std::string>> rows;
    if (local)
        rows.emplace_back("Playing as", local->displayName().empty() || local->displayName() == local->name()
                                            ? local->name()
                                            : std::format("{}  @{}", local->displayName(), local->name()));
    rows.emplace_back("Players", std::to_string(players.size()));
    rows.emplace_back("Session", minutes >= 60 ? std::format("{}:{:02}:{:02}", minutes / 60, minutes % 60, seconds)
                                               : std::format("{}:{:02}", minutes, seconds));
    const float row = 44 * k;
    const RectF card{area.x, y, area.w, row * static_cast<float>(rows.size())};
    if (list) {
        box(list, card, {1, 1, 1, 0.05f}, 12 * k);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float ry = y + row * static_cast<float>(i);
            if (i > 0)
                list->rect({card.x + 16 * k, ry, card.w - 32 * k, std::max(1.0f, std::round(k))}, line, clip_);
            text(list, rows[i].first, {card.x + 16 * k, ry, card.w * 0.4f, row}, 16, regular, secondary);
            text(list, rows[i].second, {card.x + card.w * 0.4f, ry, card.w * 0.6f - 16 * k, row}, 16, medium, white,
                 X::Right);
        }
    }
    return card.bottom() - area.y;
}

float GameMenu::drawPlayers(DrawList* list, RectF area)
{
    const float k = k_;
    Player* local = nullptr;
    const std::vector<Player*> players = sortedPlayers(&local);
    float y = area.y;
    text(list, players.size() == 1 ? std::string("1 player in this server")
                                   : std::format("{} players in this server", players.size()),
         {area.x + 4 * k, y, area.w, 20 * k}, 15, bold, secondary);
    y += 30 * k;
    const float h = 60 * k;
    for (Player* p : players) {
        const RectF r{area.x, y, area.w, h};
        y += h + 8 * k;
        if (r.bottom() < content_.y || r.y > content_.bottom())
            continue; // scrolled out
        const bool self = p == local;
        // Reporting someone fills in the Report tab (nothing is sent).
        if (!self) {
            const RectF flag{r.right() - 48 * k, r.y + 12 * k, 36 * k, 36 * k};
            const std::string name = p->name();
            Control& c = add({.id = "players.report." + name,
                              .rect = flag,
                              .focusable = true,
                              .press = [this, name] {
                                  report_.aboutPlayer = true;
                                  report_.player = name;
                                  report_.reason = -1;
                                  report_.notice = false;
                                  selectTab(Tab::Report);
                              }});
            if (list) {
                box(list, r, {1, 1, 1, 0.05f}, 12 * k);
                if (hovered(c))
                    box(list, flag, pressed_ == c.id ? pressFill : hoverFill, 10 * k);
                image(list, std::format("rbxasset://textures/ui/Settings/MenuBarIcons/ReportAbuseTab{}.png",
                                        k >= 1.5f ? "@2x" : ""),
                      grow(flag, -9 * k), faded(white, hovered(c) ? 0.9f : 0.45f));
                focusRing(list, c, 10 * k);
            }
        } else if (list) {
            box(list, r, {1, 1, 1, 0.1f}, 12 * k);
        }
        if (!list)
            continue;
        // A badge in the name's legacy chat colour with its first letter.
        const RectF avatar{r.x + 12 * k, r.y + 10 * k, 40 * k, 40 * k};
        box(list, avatar, Color4::from(legacyNameColor(p->name())), 20 * k);
        text(list, initial(shownName(*p)), avatar, 20, bold, white, X::Center);
        const float nameX = avatar.right() + 12 * k;
        float nameW = r.right() - nameX - 60 * k;
        if (self) {
            const float pill = textWidth("You", 14, bold) + 20 * k;
            const RectF you{r.right() - pill - 14 * k, r.y + (h - 24 * k) / 2, pill, 24 * k};
            box(list, you, accent, 12 * k);
            text(list, "You", you, 14, bold, white, X::Center);
            nameW = you.x - nameX - 8 * k;
        }
        text(list, shownName(*p), {nameX, r.y + 9 * k, nameW, 24 * k}, 18, bold, white);
        text(list, "@" + p->name(), {nameX, r.y + 31 * k, nameW, 20 * k}, 15, regular, secondary);
    }
    return y - 8 * k - area.y;
}

float GameMenu::drawSettings(DrawList* list, RectF area)
{
    const float k = k_;
    MenuSettings& settings = owner_->menuSettings;
    float y = area.y;
    const float rowH = 64 * k;
    const float controlW = std::min(220 * k, area.w * 0.55f);
    const float controlX = area.right() - controlW;
    const Color4 dividerColor = line;

    // A row: its label and a note under it on the left, the control's place on the right.
    auto row = [&](std::string_view label, std::string_view note, bool enabled, float control) {
        const RectF r{area.x, y, area.w, rowH};
        if (list) {
            const float labelW = area.w - control - 16 * k;
            const bool twoLines = !note.empty();
            text(list, label, {area.x, y + (twoLines ? 11 * k : 0), labelW, twoLines ? 22 * k : rowH}, 17, medium,
                 enabled ? white : disabled);
            if (twoLines)
                text(list, note, {area.x, y + 34 * k, labelW, 18 * k}, 14, regular,
                     enabled ? secondary : faded(secondary, 0.6f));
            list->rect({area.x, y + rowH - std::max(1.0f, std::round(k)), area.w, std::max(1.0f, std::round(k))},
                       dividerColor, clip_);
        }
        y += rowH;
        return r;
    };
    // Choices side by side, one lit.
    auto segmented = [&](std::string id, const RectF& r, std::span<const std::string_view> names,
                         std::optional<int> selected, bool enabled, std::function<void(int)> pick) {
        const int n = static_cast<int>(names.size());
        Control& whole = add({.id = id,
                              .rect = r,
                              .enabled = enabled,
                              .step = [selected, n, pick](int by) { pick(std::clamp(selected.value_or(1) + by, 0, n - 1)); }});
        whole.press = [selected, n, pick] { pick((selected.value_or(-1) + 1) % n); };
        if (list) {
            box(list, r, enabled ? fill : faded(fill, 0.5f), 10 * k);
            focusRing(list, whole, 10 * k);
        }
        const float w = (r.w - 6 * k) / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const RectF s{r.x + 3 * k + w * static_cast<float>(i), r.y + 3 * k, w, r.h - 6 * k};
            Control& c = add({.id = std::format("{}.{}", id, i),
                              .rect = s,
                              .enabled = enabled,
                              .focusable = false,
                              .press = [this, pick, i, id] {
                                  focus_ = id;
                                  pick(i);
                              }});
            if (!list)
                continue;
            const bool lit = selected == i;
            if (lit)
                box(list, s, enabled ? white : faded(white, 0.3f), 8 * k);
            else if (hovered(c))
                box(list, s, {1, 1, 1, 0.08f}, 8 * k);
            text(list, names[static_cast<std::size_t>(i)], s, 15, lit ? bold : medium,
                 lit ? dark : (enabled ? white : disabled), X::Center);
        }
    };
    // An on/off switch; the whole row is its button.
    auto toggle = [&](std::string id, const RectF& r, bool on, bool enabled, std::function<void(bool)> set) {
        toggleControl(list, std::move(id), r, on, enabled, std::move(set));
    };
    // A value along a track, dragged or stepped with the arrows.
    auto slider = [&](std::string id, const RectF& r, float value, float min, float max, float step, bool enabled,
                      std::string label, std::function<void(float)> set) {
        const float valueW = 44 * k;
        const RectF track{r.x + 8 * k, r.y, r.w - valueW - 16 * k, r.h};
        auto snap = [=](float v) { return std::clamp(std::round(v / step) * step, min, max); };
        Control& c = add({.id = std::move(id),
                          .rect = track,
                          .enabled = enabled,
                          .step = [=](int by) { set(snap(value + static_cast<float>(by) * step)); },
                          .drag = [=](float f) { set(snap(min + f * (max - min))); }});
        if (!list)
            return;
        const float f = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
        const float cy = track.y + track.h / 2;
        box(list, {track.x, cy - 3 * k, track.w, 6 * k}, {1, 1, 1, enabled ? 0.18f : 0.08f}, 3 * k);
        if (enabled)
            box(list, {track.x, cy - 3 * k, track.w * f, 6 * k}, accent, 3 * k);
        const float knob = (hovered(c) || dragging_ == c.id ? 22.0f : 20.0f) * k;
        const RectF dot{track.x + track.w * f - knob / 2, cy - knob / 2, knob, knob};
        box(list, dot, enabled ? white : faded(white, 0.35f), knob / 2);
        if (isFocused(c))
            box(list, grow(dot, 3 * k), accent, knob / 2 + 3 * k, 2 * k);
        text(list, label, {r.right() - valueW, r.y, valueW, r.h}, 16, medium, enabled ? white : disabled, X::Right);
    };
    // Roblox's selector: the value between arrows; the arrows (and Left/Right) step it.
    auto selector = [&](std::string id, const RectF& r, std::string_view value, bool enabled,
                        std::function<void(int)> step) {
        Control& whole = add({.id = id,
                              .rect = r,
                              .enabled = enabled,
                              .press = [step] { step(1); },
                              .step = step});
        if (list) {
            box(list, r, enabled ? fill : faded(fill, 0.5f), 10 * k);
            text(list, value, {r.x + 36 * k, r.y, r.w - 72 * k, r.h}, 16, medium, enabled ? white : disabled,
                 X::Center);
            focusRing(list, whole, 10 * k);
        }
        for (const int dir : {-1, 1}) {
            const RectF a{dir < 0 ? r.x : r.right() - 36 * k, r.y, 36 * k, r.h};
            Control& c = add({.id = std::format("{}.{}", id, dir < 0 ? "previous" : "next"),
                              .rect = a,
                              .enabled = enabled,
                              .focusable = false,
                              .press = [this, step, dir, id] {
                                  focus_ = id;
                                  step(dir);
                              }});
            if (!list)
                continue;
            if (hovered(c))
                box(list, grow(a, -3 * k), {1, 1, 1, 0.1f}, 8 * k);
            chevron(list, a.x + a.w / 2, a.y + a.h / 2, 5 * k, dir, enabled ? white : disabled);
        }
    };

    // What works.
    {
        const bool can = static_cast<bool>(owner_->onGraphicsQuality);
        RectF r = row("Graphics Quality", can ? (settings.graphics ? "Lunarstone's presets" : "Custom (command line)")
                                              : "Not available here",
                      can, controlW);
        std::optional<int> selected;
        for (int i = 0; i < static_cast<int>(presets.size()); ++i)
            if (settings.graphics == presets[static_cast<std::size_t>(i)])
                selected = i;
        segmented("settings.graphics", {controlX, r.y + 14 * k, controlW, 36 * k}, presetNames, selected, can,
                  [this](int i) {
                      owner_->menuSettings.graphics = presets[static_cast<std::size_t>(i)];
                      if (owner_->onGraphicsQuality)
                          owner_->onGraphicsQuality(presets[static_cast<std::size_t>(i)]);
                  });
    }
    {
        const bool can = settings.rayTracingAvailable && static_cast<bool>(owner_->onRayTracing);
        const std::string_view note = !settings.rayTracingAvailable ? "This GPU can't ray trace"
                                      : !owner_->onRayTracing        ? "Not available here"
                                      : settings.rayTracing != settings.rayTracingActive
                                          ? "From your next game"
                                          : "Ray-traced shadows and lighting";
        RectF r = row("Ray Tracing", note, can, 56 * k);
        toggle("settings.raytracing", r, settings.rayTracing, can, [this](bool on) {
            owner_->menuSettings.rayTracing = on;
            if (owner_->onRayTracing)
                owner_->onRayTracing(on);
        });
    }
    {
        const bool can = static_cast<bool>(owner_->onFullscreen);
        RectF r = row("Fullscreen", can ? "Also F11" : "Not available here", can, 56 * k);
        toggle("settings.fullscreen", r, settings.fullscreen, can, [this](bool on) {
            owner_->menuSettings.fullscreen = on;
            if (owner_->onFullscreen)
                owner_->onFullscreen(on);
        });
    }
    {
        const bool can = static_cast<bool>(owner_->onCameraSensitivity);
        RectF r = row("Camera Sensitivity", can ? "Mouse turning speed" : "Not available here", can, controlW);
        slider("settings.sensitivity", {controlX, r.y + 14 * k, controlW, 36 * k}, settings.cameraSensitivity,
               minSensitivity, maxSensitivity, 0.1f, can, std::format("{:.1f}", settings.cameraSensitivity),
               [this](float value) {
                   if (value == owner_->menuSettings.cameraSensitivity)
                       return;
                   owner_->menuSettings.cameraSensitivity = value;
                   if (owner_->onCameraSensitivity)
                       owner_->onCameraSensitivity(value);
               });
    }
    {
        const bool can = static_cast<bool>(owner_->onVolume);
        RectF r = row("Volume", can ? "How loud the game is" : "Not available here", can, controlW);
        slider("settings.volume", {controlX, r.y + 14 * k, controlW, 36 * k}, settings.volume, 0.0f, 1.0f, 0.1f, can,
               settings.volume <= 0.0f ? std::string("Off") : std::format("{}", std::lround(settings.volume * 10.0f)),
               [this](float value) {
                   if (value == owner_->menuSettings.volume)
                       return;
                   owner_->menuSettings.volume = value;
                   if (owner_->onVolume)
                       owner_->onVolume(value);
               });
    }
    {
        const bool can = static_cast<bool>(owner_->onShiftLock);
        RectF r = row("Shift Lock Switch", can ? "Shift locks the mouse to the camera" : "Not available here", can,
                      56 * k);
        toggle("settings.shiftlock", r, settings.shiftLock, can, [this](bool on) {
            owner_->menuSettings.shiftLock = on;
            if (owner_->onShiftLock)
                owner_->onShiftLock(on);
        });
    }
    {
        RectF r = row("Performance Stats", "Frame rate and what each frame draws", true, 56 * k);
        toggle("settings.stats", r, settings.performanceStats, true,
               [this](bool on) { owner_->menuSettings.performanceStats = on; });
    }
    {
        RectF r = row("Health Bars", "A health bar beside every other player (F7)", true, 56 * k);
        toggle("settings.healthbars", r, settings.healthBars, true,
               [this](bool on) { owner_->menuSettings.healthBars = on; });
    }

    // What Lunarstone can't do yet: shown, never faked.
    y += 22 * k;
    text(list, "Not available yet", {area.x, y, area.w, 20 * k}, 15, bold, secondary);
    y += 22 * k;
    {
        RectF r = row("Camera Mode", "Only the default camera so far", false, controlW);
        selector("settings.cameramode", {controlX, r.y + 14 * k, controlW, 36 * k}, "Default (Classic)", false,
                 [](int) {});
    }
    {
        RectF r = row("Movement Mode", "Click to move isn't in yet", false, controlW);
        selector("settings.movementmode", {controlX, r.y + 14 * k, controlW, 36 * k}, "Keyboard", false, [](int) {});
    }
    return y - area.y;
}

float GameMenu::drawReport(DrawList* list, RectF area)
{
    const float k = k_;
    float y = area.y;
    const std::string_view intro = "Report a player or this experience for breaking the rules.";
    const float introH = wrappedHeight(intro, 16, regular, area.w);
    text(list, intro, {area.x, y, area.w, introH}, 16, regular, secondary, X::Left, Y::Top, true);
    y += introH + 20 * k;

    auto label = [&](std::string_view what, bool enabled) {
        text(list, what, {area.x, y, area.w, 20 * k}, 15, bold, enabled ? secondary : faded(secondary, 0.5f));
        y += 26 * k;
    };
    const float fieldH = 44 * k;

    // Experience or player.
    label("What are you reporting?", true);
    {
        const RectF r{area.x, y, area.w, fieldH};
        static constexpr std::array<std::string_view, 2> kinds{"The experience", "A player"};
        const int selected = report_.aboutPlayer ? 1 : 0;
        auto pick = [this](int i) {
            const bool aboutPlayer = i == 1;
            if (aboutPlayer != report_.aboutPlayer)
                report_.reason = -1;
            report_.aboutPlayer = aboutPlayer;
            if (aboutPlayer && report_.player.empty())
                if (const auto others = otherPlayers(); !others.empty())
                    report_.player = others.front()->name();
            report_.notice = false;
        };
        Control& whole = add({.id = "report.kind",
                              .rect = r,
                              .press = [pick, selected] { pick(1 - selected); },
                              .step = [pick](int by) { pick(by > 0 ? 1 : 0); }});
        if (list) {
            box(list, r, fill, 10 * k);
            focusRing(list, whole, 10 * k);
        }
        const float w = (r.w - 6 * k) / 2;
        for (int i = 0; i < 2; ++i) {
            const RectF s{r.x + 3 * k + w * static_cast<float>(i), r.y + 3 * k, w, r.h - 6 * k};
            Control& c = add({.id = std::format("report.kind.{}", i),
                              .rect = s,
                              .focusable = false,
                              .press = [this, pick, i] {
                                  focus_ = "report.kind";
                                  pick(i);
                              }});
            if (!list)
                continue;
            if (selected == i)
                box(list, s, white, 8 * k);
            else if (hovered(c))
                box(list, s, {1, 1, 1, 0.08f}, 8 * k);
            text(list, kinds[static_cast<std::size_t>(i)], s, 16, selected == i ? bold : medium,
                 selected == i ? dark : white, X::Center);
        }
        y += fieldH + 18 * k;
    }

    // Selector rows: the value between arrows.
    auto selector = [&](std::string id, std::string_view value, bool enabled, std::function<void(int)> step) {
        const RectF r{area.x, y, area.w, fieldH};
        Control& whole = add({.id = id, .rect = r, .enabled = enabled, .press = [step] { step(1); }, .step = step});
        if (list) {
            box(list, r, enabled ? fill : faded(fill, 0.5f), 10 * k);
            text(list, value, {r.x + 44 * k, r.y, r.w - 88 * k, r.h}, 16, medium, enabled ? white : disabled,
                 X::Center);
            focusRing(list, whole, 10 * k);
        }
        for (const int dir : {-1, 1}) {
            const RectF a{dir < 0 ? r.x : r.right() - 44 * k, r.y, 44 * k, r.h};
            Control& c = add({.id = std::format("{}.{}", id, dir < 0 ? "previous" : "next"),
                              .rect = a,
                              .enabled = enabled,
                              .focusable = false,
                              .press = [this, step, dir, id] {
                                  focus_ = id;
                                  step(dir);
                              }});
            if (!list)
                continue;
            if (hovered(c))
                box(list, grow(a, -4 * k), {1, 1, 1, 0.1f}, 8 * k);
            chevron(list, a.x + a.w / 2, a.y + a.h / 2, 5 * k, dir, enabled ? white : disabled);
        }
        y += fieldH + 18 * k;
    };

    // Which player.
    const std::vector<Player*> others = otherPlayers();
    {
        const bool enabled = report_.aboutPlayer && !others.empty();
        label("Which player?", report_.aboutPlayer);
        std::string value = "Only for reporting a player";
        if (report_.aboutPlayer) {
            value = "No one else is here";
            for (const Player* p : others)
                if (p->name() == report_.player)
                    value = shownName(*p) == p->name() ? p->name() : std::format("{} (@{})", shownName(*p), p->name());
        }
        selector("report.player", value, enabled, [this](int by) {
            const auto candidates = otherPlayers();
            if (candidates.empty())
                return;
            int at = -1;
            for (std::size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i]->name() == report_.player)
                    at = static_cast<int>(i);
            const int n = static_cast<int>(candidates.size());
            at = at < 0 ? 0 : (at + by + n) % n;
            report_.player = candidates[static_cast<std::size_t>(at)]->name();
            report_.notice = false;
        });
    }

    // Why.
    {
        label("Reason", true);
        const auto choices = reasons(report_.aboutPlayer);
        const std::string value
            = report_.reason >= 0 && report_.reason < static_cast<int>(choices.size())
                  ? std::string(choices[static_cast<std::size_t>(report_.reason)])
                  : std::string("Choose a reason");
        selector("report.reason", value, true, [this](int by) {
            const int n = static_cast<int>(reasons(report_.aboutPlayer).size());
            report_.reason = report_.reason < 0 ? (by > 0 ? 0 : n - 1) : (report_.reason + by + n) % n;
            report_.notice = false;
        });
    }

    // What happened: a line of text.
    {
        label("Description (optional)", true);
        field(list, "report.description", {area.x, y, area.w, fieldH}, report_.description, "Tell us what happened",
              regular);
        y += fieldH + 24 * k;
    }

    const bool ready = report_.reason >= 0 && (!report_.aboutPlayer || !report_.player.empty());
    button(list, "report.submit", {area.x, y, area.w, 52 * k}, "Submit", {}, ButtonStyle::Primary,
           [this] { submitReport(); }, ready);
    y += 52 * k;

    if (report_.notice) {
        // Nothing is sent: say so plainly.
        y += 18 * k;
        const std::string_view body = "Lunarstone doesn't send reports yet, so this one stayed on your computer. "
                                      "To report for real, use the Roblox app or website.";
        const float bodyH = wrappedHeight(body, 15, regular, area.w - 32 * k);
        const RectF card{area.x, y, area.w, bodyH + 58 * k};
        if (list) {
            box(list, card, faded(warning, 0.1f), 12 * k);
            box(list, card, faded(warning, 0.6f), 12 * k, 1.5f * k);
            text(list, "Report not sent", {card.x + 16 * k, card.y + 12 * k, card.w - 32 * k, 22 * k}, 17, bold,
                 warning);
            text(list, body, {card.x + 16 * k, card.y + 40 * k, card.w - 32 * k, bodyH}, 15, regular, white, X::Left,
                 Y::Top, true);
        }
        y = card.bottom();
    }
    return y - area.y;
}

void GameMenu::submitReport()
{
    if (report_.reason < 0 || (report_.aboutPlayer && report_.player.empty()))
        return;
    // Deliberately goes nowhere: no web request, no callback.
    ++report_.submitted;
    report_.notice = true;
    typing_ = false;
    scrollToEnd_ = true; // down to the note
}

float GameMenu::drawHelp(DrawList* list, RectF area)
{
    const float k = k_;
    float y = area.y;
    struct Row {
        std::vector<std::string> keys;
        std::string what;
    };
    auto section = [&](std::string_view title, const std::vector<Row>& rows) {
        text(list, title, {area.x, y, area.w, 22 * k}, 15, bold, secondary);
        y += 30 * k;
        const float keysW = std::min(170 * k, area.w * 0.45f);
        for (const Row& row : rows) {
            const float h = 36 * k;
            if (list && y + h >= content_.y && y <= content_.bottom()) {
                float x = area.x;
                for (const std::string& key : row.keys) {
                    float w = 0;
                    keyChip(list, key, x, y + h / 2, white, &w);
                    x += w + 6 * k;
                }
                text(list, row.what, {area.x + keysW, y, area.w - keysW, h}, 16, regular, white);
            }
            y += h;
        }
        y += 18 * k;
    };
    std::vector<Row> play{
        {{"W", "A", "S", "D"}, "Walk"},
        {{"Space"}, "Jump"},
        {{"Right mouse"}, "Hold and drag to turn the camera"},
        {{"Left", "Right"}, "Turn the camera"},
        {{"Wheel"}, "Zoom (also I and O)"},
        {{"Shift"}, "Shift lock, when it's on in Settings"},
        {{"/"}, "Chat"},
        {{"1-9", "0"}, "Equip a tool"},
        {{"Click"}, "Use the tool in hand"},
        {{"Backspace"}, "Drop the tool in hand"},
        {{"Tab"}, "Show or hide the player list"},
        {{"F7"}, "Health bars beside the other players"},
    };
    if (mapKey_)
        play.push_back({{std::string(keyLabel(*mapKey_))}, "The map"});
    play.push_back({{"F11"}, "Fullscreen"});
    play.push_back({{"Esc"}, "This menu"});
    section("Keyboard and mouse", play);
    section("In this menu", {
                                {{"Tab"}, "Next tab (Shift for the one before)"},
                                {{"Up", "Down"}, "Move between controls"},
                                {{"Left", "Right"}, "Change a setting"},
                                {{"Enter"}, "Press"},
                                {{"R"}, "Reset your character"},
                                {{"L"}, "Leave"},
                            });
    if (tab_ == Tab::Luau)
        section("In the Luau tab", {
                                      {{"Enter"}, "A new line"},
                                      {{"Ctrl+Enter"}, "Run what is there"},
                                      {{"Tab"}, "Indent; Shift+Tab still changes tab"},
                                      {{"Up", "Down"}, "Between lines, then through what ran before"},
                                      {{"Ctrl+L"}, "Clear the log"},
                                  });
    return y - 18 * k - area.y;
}

void GameMenu::drawDialog(DrawList* list, const RectF& panel)
{
    const float k = k_;
    clip_ = panel;
    if (list)
        box(list, panel, {0, 0, 0, 0.55f}, 16 * k);
    const bool reset = dialog_ == Dialog::Reset;
    const std::string_view question
        = reset ? "Are you sure you want to reset your character?" : "Are you sure you want to leave the experience?";
    const float w = std::min(panel.w - 40 * k, 400 * k);
    const float pad = 28 * k;
    const float questionH = wrappedHeight(question, 24, bold, w - 2 * pad);
    const float buttonH = 50 * k;
    const float h = pad + questionH + 28 * k + buttonH + pad;
    const RectF card{std::round(panel.x + (panel.w - w) / 2), std::round(panel.y + (panel.h - h) / 2), w, h};
    if (list) {
        box(list, grow(card, 4 * k), {0, 0, 0, 0.12f}, 20 * k);
        box(list, card, dialogColor, 16 * k);
        box(list, card, line, 16 * k, std::max(1.0f, std::round(k)));
        text(list, question, {card.x + pad, card.y + pad, card.w - 2 * pad, questionH}, 24, bold, white, X::Center,
             Y::Top, true);
    }
    const float by = card.y + pad + questionH + 28 * k;
    const float bw = (card.w - 2 * pad - 12 * k) / 2;
    button(list, "dialog.confirm", {card.x + pad, by, bw, buttonH}, reset ? "Reset" : "Leave", {}, ButtonStyle::Primary,
           [this, reset] {
               openDialog(Dialog::None);
               if (reset) {
                   setOpen(false, true);
                   if (owner_->onReset)
                       owner_->onReset();
               } else if (owner_->onLeave) {
                   owner_->onLeave();
               }
           });
    button(list, "dialog.cancel", {card.x + pad + bw + 12 * k, by, bw, buttonH}, reset ? "Don't Reset" : "Don't Leave",
           {}, ButtonStyle::Secondary, [this] { openDialog(Dialog::None); });
}

// An on/off switch at the right of `r`, the whole row its target.
void GameMenu::toggleControl(DrawList* list, std::string id, const RectF& r, bool on, bool enabled,
                             std::function<void(bool)> set)
{
    const float k = k_;
    Control& c = add({.id = std::move(id),
                      .rect = r,
                      .enabled = enabled,
                      .press = [on, set] { set(!on); },
                      .step = [set](int by) { set(by > 0); }});
    if (!list)
        return;
    if (hovered(c))
        box(list, grow(r, -2 * k), {1, 1, 1, 0.04f}, 10 * k);
    const RectF track{r.right() - 50 * k, r.y + (r.h - 28 * k) / 2, 50 * k, 28 * k};
    const Color4 trackColor = on ? accent : Color4{1, 1, 1, 0.18f};
    box(list, track, enabled ? trackColor : faded(trackColor, 0.4f), 14 * k);
    const float knob = 22 * k;
    const RectF dot{on ? track.right() - knob - 3 * k : track.x + 3 * k, track.y + 3 * k, knob, knob};
    box(list, dot, enabled ? white : faded(white, 0.4f), knob / 2);
    if (isFocused(c))
        box(list, grow(track, 3 * k), accent, 17 * k, 2 * k);
}

// The music tab: the beat ring that dances to whatever plays (the app
// listens, MenuSettings::musicStatus says how that goes); the player comes later.
float GameMenu::drawMusic(DrawList* list, RectF area)
{
    const float k = k_;
    MenuSettings& settings = owner_->menuSettings;
    float y = area.y;
    const float rowH = 64 * k;
    const RectF r{area.x, y, area.w, rowH};
    if (list) {
        text(list, "Beat Ring", {area.x, y + 11 * k, area.w - 70 * k, 22 * k}, 17, medium, white);
        text(list, "A rainbow ring round you that moves to the music", {area.x, y + 34 * k, area.w - 70 * k, 18 * k},
             14, regular, secondary);
        list->rect({area.x, y + rowH - std::max(1.0f, std::round(k)), area.w, std::max(1.0f, std::round(k))}, line,
                   clip_);
    }
    toggleControl(list, "music.ring", r, settings.beatRing, true,
                  [this](bool on) { owner_->menuSettings.beatRing = on; });
    y += rowH + 14 * k;
    if (!settings.musicStatus.empty()) {
        const float h = wrappedHeight(settings.musicStatus, 15, regular, area.w);
        text(list, settings.musicStatus, {area.x, y, area.w, h}, 15, regular, secondary);
        y += h + 14 * k;
    }
    const std::string_view later = "A music player is coming here too.";
    const float h = wrappedHeight(later, 15, regular, area.w);
    text(list, later, {area.x, y, area.w, h}, 15, regular, faded(secondary, 0.7f));
    y += h;
    return y - area.y + 16 * k;
}

float GameMenu::drawLuau(DrawList* list, RectF area)
{
    const float k = k_;
    float y = area.y;
    DevConsole& console = owner_->console();
    const std::string_view intro = "What the game's client scripts print, warn and raise. Luau run below runs as one "
                                   "of them: what it fires really goes to the server.";
    const float introH = wrappedHeight(intro, 15, regular, area.w);
    text(list, intro, {area.x, y, area.w, introH}, 15, regular, secondary, X::Left, Y::Top, true);
    y += introH + 16 * k;

    // Which lines, and Clear.
    const float rowH = 36 * k;
    const float clearW = 76 * k;
    {
        static constexpr std::array<std::string_view, 4> filters{"All", "Output", "Warnings", "Errors"};
        const RectF r{area.x, y, area.w - clearW - 8 * k, rowH};
        if (list)
            box(list, r, fill, 9 * k);
        const float w = (r.w - 6 * k) / 4;
        for (int i = 0; i < 4; ++i) {
            const RectF s{r.x + 3 * k + w * static_cast<float>(i), r.y + 3 * k, w, r.h - 6 * k};
            Control& c = add({.id = std::format("luau.filter.{}", i), .rect = s, .press = [this, i] {
                                  logFilter_ = static_cast<LogFilter>(i);
                                  scrollToEnd_ = true;
                              }});
            if (!list)
                continue;
            const bool selected = static_cast<int>(logFilter_) == i;
            if (selected)
                box(list, s, white, 7 * k);
            else if (hovered(c))
                box(list, s, {1, 1, 1, 0.08f}, 7 * k);
            focusRing(list, c, 7 * k);
            text(list, filters[static_cast<std::size_t>(i)], s, 14, selected ? bold : medium,
                 selected ? dark : white, X::Center);
        }
        button(list, "luau.clear", {r.right() + 8 * k, y, clearW, rowH}, "Clear", {}, ButtonStyle::Secondary,
               [&console] { console.clear(); });
        y += rowH + 12 * k;
    }

    // The log, oldest first; the newest few hundred that pass the filter.
    {
        const std::vector<DevConsole::Line> lines = console.lines();
        auto shows = [this](DevConsole::Kind kind) {
            switch (logFilter_) {
            case LogFilter::All:
                return true;
            case LogFilter::Output:
                return kind == DevConsole::Kind::Output || kind == DevConsole::Kind::Command;
            case LogFilter::Warnings:
                return kind == DevConsole::Kind::Warning;
            case LogFilter::Errors:
                return kind == DevConsole::Kind::Error;
            }
            return true;
        };
        std::vector<const DevConsole::Line*> shown;
        for (auto it = lines.rbegin(); it != lines.rend() && shown.size() < 200; ++it)
            if (shows(it->kind))
                shown.push_back(&*it);
        std::ranges::reverse(shown);

        const float pad = 12 * k;
        const float iconW = 18 * k;
        const float textW = area.w - 2 * pad - iconW;
        float h = pad;
        std::vector<float> heights;
        heights.reserve(shown.size());
        for (const DevConsole::Line* line : shown) {
            heights.push_back(std::max(wrappedHeight(line->text, 14, Enum::Font::Code, textW), 18 * k));
            h += heights.back() + 4 * k;
        }
        if (shown.empty())
            h += 18 * k;
        h += pad;
        const RectF logBox{area.x, y, area.w, h};
        if (list) {
            box(list, logBox, {0, 0, 0, 0.35f}, 10 * k);
            float at = y + pad;
            if (shown.empty())
                text(list, lines.empty() ? "Nothing printed yet." : "Nothing here passes the filter.",
                     {area.x + pad, at, textW, 18 * k}, 14, regular, faded(secondary, 0.7f));
            for (std::size_t i = 0; i < shown.size(); ++i) {
                const DevConsole::Line& line = *shown[i];
                Color4 color{0.92f, 0.93f, 0.95f, 1.0f};
                std::string_view icon;
                switch (line.kind) {
                case DevConsole::Kind::Output:
                    break;
                case DevConsole::Kind::Info:
                    color = secondary;
                    break;
                case DevConsole::Kind::Warning:
                    color = warning;
                    icon = "rbxasset://textures/DevConsole/Warning.png";
                    break;
                case DevConsole::Kind::Error:
                    color = {1.0f, 0.45f, 0.42f, 1.0f};
                    icon = "rbxasset://textures/DevConsole/Error.png";
                    break;
                case DevConsole::Kind::Command:
                    color = {0.55f, 0.68f, 1.0f, 1.0f};
                    break;
                }
                if (!icon.empty())
                    image(list, icon, {area.x + pad, at + 2 * k, 14 * k, 14 * k}, white);
                text(list, line.text, {area.x + pad + iconW, at, textW, heights[i]}, 14, Enum::Font::Code, color,
                     X::Left, Y::Top, true);
                at += heights[i] + 4 * k;
            }
        }
        y += h + 12 * k;
    }

    // The command line: a line while it's short, as many as it holds up to
    // ten, then it scrolls inside itself.
    {
        const bool can = static_cast<bool>(console.run);
        const float runW = 76 * k;
        const float runH = 44 * k;
        const float fieldH = codeHeight();
        codeField(list, "luau.input", {area.x, y, area.w - runW - 8 * k, fieldH}, luauInput_,
                  can ? "print(\"hello\")  (Ctrl+Enter runs)" : "No client scripts to run in", can);
        // Run keeps its own height, at the editor's bottom.
        button(list, "luau.run", {area.right() - runW, y + fieldH - runH, runW, runH}, "Run", {},
               ButtonStyle::Primary, [this] { runLuau(); }, can && !luauInput_.text.empty());
        y += fieldH;
    }

    // New lines keep the log at its end, unless it's been scrolled up to read.
    const std::uint64_t added = console.added();
    if (added != logSeen_) {
        if (scroll() >= contentHeight_ - content_.h - 60 * k)
            scrollToEnd_ = true;
        logSeen_ = added;
    }
    return y - area.y;
}

void GameMenu::runLuau()
{
    const std::string code = luauInput_.text;
    if (code.empty())
        return;
    luauInput_.setText({});
    historyAt_ = -1;
    caretTime_ = time_;
    owner_->console().submit(code);
    scrollToEnd_ = true;
}

void GameMenu::browseHistory(int by)
{
    const std::vector<std::string>& history = owner_->console().history();
    if (history.empty())
        return;
    const int n = static_cast<int>(history.size());
    if (historyAt_ < 0)
        historyAt_ = by < 0 ? n - 1 : -1;
    else
        historyAt_ += by;
    if (historyAt_ >= n)
        historyAt_ = -1;
    historyAt_ = std::max(historyAt_, by < 0 ? 0 : -1);
    luauInput_.setText(historyAt_ < 0 ? std::string() : history[static_cast<std::size_t>(historyAt_)]);
    caretTime_ = time_;
}

void GameMenu::luauIcon(DrawList* list, RectF r, Color4 tint)
{
    if (!list)
        return;
    const float k = k_;
    // A window's outline, a prompt and a cursor inside it.
    box(list, {r.x + 1 * k, r.y + 3 * k, r.w - 2 * k, r.h - 6 * k}, tint, 4 * k, 1.5f * k);
    chevron(list, r.x + 8.5f * k, r.y + r.h / 2, 3.5f * k, 1, tint);
    box(list, {r.x + 12.5f * k, r.y + r.h / 2 + 2.5f * k, 6 * k, 1.5f * k}, tint, 0.75f * k);
}

void GameMenu::musicIcon(DrawList* list, RectF r, Color4 tint)
{
    if (!list)
        return;
    const float k = k_;
    // Two beamed eighth notes: round heads, stems up to a thick beam.
    const float head = 6.5f * k, stem = 1.75f * k;
    const float left = r.x + 3 * k, right = r.x + r.w - 3 * k - head;
    const float bottom = r.y + r.h - 3 * k;
    box(list, {left, bottom - head, head, head}, tint, head / 2);
    box(list, {right, bottom - head - 2 * k, head, head}, tint, head / 2);
    box(list, {left + head - stem, r.y + 5 * k, stem, bottom - head / 2 - r.y - 5 * k}, tint, stem / 2);
    box(list, {right + head - stem, r.y + 3 * k, stem, bottom - head / 2 - r.y - 5 * k}, tint, stem / 2);
    box(list, {left + head - stem, r.y + 3 * k, right - left + stem, 3 * k}, tint, 1 * k);
}

GameMenu::Control& GameMenu::field(DrawList* list, std::string id, RectF r, TextEditor& editor,
                                   std::string_view placeholder, Enum::Font font, bool enabled)
{
    const float k = k_;
    const bool active = typing_ && editingId() == id;
    Control& c = add({.id = std::move(id), .rect = r, .enabled = enabled, .press = [this] {
                          typing_ = true;
                          caretTime_ = time_;
                      }});
    if (!list)
        return c;
    box(list, r, {1, 1, 1, enabled ? 0.05f : 0.02f}, 10 * k);
    const bool hot = active || (enabled && hovered(c));
    box(list, r, active ? accent : Color4{1, 1, 1, hot ? 0.3f : 0.14f}, 10 * k, (active ? 2.0f : 1.5f) * k);
    focusRing(list, c, 10 * k);
    const RectF inner{r.x + 14 * k, r.y, r.w - 28 * k, r.h};
    const std::string& draft = editor.text;
    if (draft.empty() && !active) {
        text(list, placeholder, inner, 15, font == Enum::Font::Code ? regular : font,
             enabled ? faded(secondary, 0.7f) : disabled);
        return c;
    }
    TextParams params;
    params.text = draft;
    params.style.font = fontFromEnum(font);
    params.style.size = 16 * k;
    params.style.color = {1, 1, 1};
    params.xAlignment = X::Left;
    params.yAlignment = Y::Center;
    params.box = {inner.w, inner.h};
    const TextLayout layout = layoutText(*fonts_, params);
    // Where a byte offset sits: the pen at a bar typed after it (spaces
    // leave no glyph to measure by).
    auto xAt = [&](std::size_t pos) {
        TextParams upTo = params;
        const std::string before = draft.substr(0, pos) + "|";
        upTo.text = before;
        const TextLayout probe = layoutText(*fonts_, upTo);
        return probe.glyphs.empty() ? 0.0f : probe.glyphs.back().x;
    };
    // Long text scrolls so the caret stays in the box.
    const float caretAt = xAt(editor.caret);
    const float shift = std::min(0.0f, inner.w - 2 * k - caretAt);
    const RectF clip = inner.intersect(clip_);
    if (active && editor.hasSelection()) {
        const auto [first, last] = editor.selection();
        const float from = inner.x + shift + xAt(first);
        list->rect({from, r.y + 11 * k, inner.x + shift + xAt(last) - from, r.h - 22 * k}, {0.2f, 0.37f, 1.0f, 0.45f},
                   clip);
    }
    list->text(*atlas_, layout, inner.x + shift, inner.y, clip);
    if (active && std::fmod(time_ - caretTime_, 1.0) < 0.5)
        list->rect({std::round(inner.x + shift + caretAt), r.y + 11 * k, std::max(1.0f, std::round(1.5f * k)),
                    r.h - 22 * k},
                   white, clip);
    return c;
}

float GameMenu::codeHeight() const
{
    const float lines = static_cast<float>(std::clamp<std::size_t>(luauInput_.lineCount(), 1, luauMaxLines));
    return std::max(44.0f * k_, lines * codeLine() + 2 * codePad());
}

float GameMenu::codeTop(RectF r, std::size_t shown) const
{
    const float lines = static_cast<float>(std::clamp<std::size_t>(shown, 1, luauMaxLines));
    return r.y + (r.h - lines * codeLine()) / 2;
}

float GameMenu::codeWidth(std::size_t lines)
{
    // Wide enough for the biggest number the editor shows, so the gutter
    // doesn't move as the code grows.
    const std::size_t most = std::max(lines, static_cast<std::size_t>(luauMaxLines));
    return textWidth(std::to_string(most), gutterTextSize, Enum::Font::Code) + 10 * k_;
}

GameMenu::Control& GameMenu::codeField(DrawList* list, std::string id, RectF r, TextEditor& editor,
                                       std::string_view placeholder, bool enabled)
{
    const float k = k_;
    const bool active = typing_ && editingId() == id;
    Control& c = add({.id = std::move(id), .rect = r, .enabled = enabled, .press = [this, &editor, r] {
                          typing_ = true;
                          caretTime_ = time_;
                          caretInCode(editor, r);
                      }});
    const std::vector<std::string_view> lines = editor.lines();
    const float gutter = codeWidth(lines.size());
    const int shown = std::clamp(static_cast<int>((r.h - 2 * codePad()) / codeLine()), 1,
                                 std::max(1, static_cast<int>(lines.size())));
    // Which lines it shows: scrolled to keep the caret in view.
    int first = std::clamp(static_cast<int>(luauScroll_), 0, std::max(0, static_cast<int>(lines.size()) - shown));
    const int caretLine = static_cast<int>(editor.caretLine());
    if (caretLine < first)
        first = caretLine;
    else if (caretLine >= first + shown)
        first = caretLine - shown + 1;
    first = std::clamp(first, 0, std::max(0, static_cast<int>(lines.size()) - shown));
    luauScroll_ = static_cast<float>(first);
    const RectF inner{r.x + 14 * k + gutter, codeTop(r, static_cast<std::size_t>(shown)),
                      r.w - 28 * k - gutter, static_cast<float>(shown) * codeLine()};
    luauGutter_ = {r.x + 14 * k, inner.y, gutter - 8 * k, inner.h};
    if (!list)
        return c;
    box(list, r, {1, 1, 1, enabled ? 0.05f : 0.02f}, 10 * k);
    const bool hot = active || (enabled && hovered(c));
    box(list, r, active ? accent : Color4{1, 1, 1, hot ? 0.3f : 0.14f}, 10 * k, (active ? 2.0f : 1.5f) * k);
    focusRing(list, c, 10 * k);
    if (editor.text.empty() && !active) {
        text(list, placeholder, {r.x + 14 * k, r.y, r.w - 28 * k, r.h}, codeTextSize, regular,
             enabled ? faded(secondary, 0.7f) : disabled);
        return c;
    }
    // How far the lines are shifted to keep the caret in view: to the right when
    // it goes past, and back to it when it doesn't. All the lines shift
    // together, the way any code editor's do.
    const std::string_view caretOn = lines[static_cast<std::size_t>(caretLine)];
    const float keep = 8 * k;
    const float caretAt = codeX(caretOn, editor.caret - editor.lineStart());
    luauShift_ = caretAt > inner.w - keep ? caretAt - inner.w + keep : std::min(luauShift_, caretAt - keep);
    luauShift_ = std::max(0.0f, luauShift_);

    const bool selected = active && editor.hasSelection();
    const auto [selFirst, selLast] = editor.selection();
    std::size_t offset = 0;
    const RectF saved = clip_;
    for (std::size_t i = 0; i < lines.size(); offset += lines[i].size() + 1, ++i) {
        if (static_cast<int>(i) < first || static_cast<int>(i) >= first + shown)
            continue;
        const std::string_view line = lines[i];
        const float y = inner.y + static_cast<float>(static_cast<int>(i) - first) * codeLine();
        // The line's number in the gutter, where the whole field shows.
        clip_ = saved;
        text(list, std::to_string(i + 1), {r.x + 14 * k, y, gutter - 8 * k, codeLine()}, gutterTextSize,
             Enum::Font::Code, faded(secondary, 0.55f), X::Right);
        // The code itself, past the gutter and the lines shifted beside it.
        clip_ = inner.intersect(saved);
        const float x = inner.x - luauShift_;
        if (selected) {
            // What part of the selection is on this line.
            const std::size_t from = std::max(selFirst, offset);
            const std::size_t to = std::min(selLast, offset + line.size());
            if (from < to)
                list->rect({x + codeX(line, from - offset), y + 2 * k,
                            codeX(line, to - offset) - codeX(line, from - offset), codeLine() - 4 * k},
                           {0.2f, 0.37f, 1.0f, 0.45f}, clip_);
        }
        for (const LuauSpan& span : highlightLuau(line))
            // A wide box: a line past the edge is cut off, not ended in dots.
            text(list, line.substr(span.begin, span.end - span.begin),
                 {x + codeX(line, span.begin), y, r.w, codeLine()}, codeTextSize, Enum::Font::Code,
                 tokenColor(span.token));
        if (active && static_cast<int>(i) == caretLine && std::fmod(time_ - caretTime_, 1.0) < 0.5)
            list->rect({std::round(x + caretAt), y + 2 * k, std::max(1.0f, std::round(1.5f * k)), codeLine() - 4 * k},
                       white, clip_);
    }
    clip_ = saved;
    return c;
}

float GameMenu::codeX(std::string_view line, std::size_t column) const
{
    if (column == 0)
        return 0.0f;
    TextParams params;
    params.style.font = fontFromEnum(Enum::Font::Code);
    params.style.size = codeTextSize * k_;
    params.style.color = {1, 1, 1};
    params.xAlignment = X::Left;
    params.yAlignment = Y::Top;
    // A bar typed after it: spaces leave no glyph to measure by.
    const std::string before = std::string(line.substr(0, std::min(column, line.size()))) + "|";
    params.text = before;
    const TextLayout probe = layoutText(*fonts_, params);
    return probe.glyphs.empty() ? 0.0f : probe.glyphs.back().x;
}

// A click in the editor: the line under the pointer, as far along it as the
// pointer was. The last line's own shift is undone, the rest is at its start.
void GameMenu::caretInCode(TextEditor& editor, RectF r)
{
    const std::vector<std::string_view> lines = editor.lines();
    const float row = codeLine();
    const int index = std::clamp(static_cast<int>((mouseY_ - codeTop(r, lines.size())) / row)
                                     + static_cast<int>(luauScroll_),
                                 0, static_cast<int>(lines.size()) - 1);
    std::size_t offset = 0;
    for (int i = 0; i < index; ++i)
        offset += lines[static_cast<std::size_t>(i)].size() + 1;
    const float x = mouseX_ - (r.x + 14 * k_ + codeWidth(lines.size())) + luauShift_;
    const std::string_view here = lines[static_cast<std::size_t>(index)];
    std::size_t column = 0;
    while (column < here.size()) { // the character under the pointer
        std::size_t next = column + 1;
        while (next < here.size() && continuation(here[next]))
            ++next;
        if (codeX(here, next) > x)
            break;
        column = next;
    }
    editor.caret = offset + column;
    editor.anchor.reset();
}

// --- Helpers ------------------------------------------------------------------

void GameMenu::text(DrawList* list, std::string_view text, RectF box, float size, Enum::Font font, Color4 color, X x,
                    Y y, bool wrapped)
{
    if (!list || color.a <= 0.0f || text.empty())
        return;
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(font);
    params.style.size = size * k_;
    params.style.color = {color.r, color.g, color.b};
    params.style.transparency = 1.0f - color.a;
    params.xAlignment = x;
    params.yAlignment = y;
    params.box = {box.w, box.h};
    params.wrapped = wrapped;
    params.truncate = wrapped ? Enum::TextTruncate::None : Enum::TextTruncate::AtEnd;
    list->text(*atlas_, layoutText(*fonts_, params), std::round(box.x), std::round(box.y), box.intersect(clip_));
}

float GameMenu::textWidth(std::string_view text, float size, Enum::Font font)
{
    return measureText(*fonts_, text, fontFromEnum(font), size * k_).x;
}

float GameMenu::wrappedHeight(std::string_view text, float size, Enum::Font font, float width)
{
    return std::ceil(measureText(*fonts_, text, fontFromEnum(font), size * k_, width).y);
}

void GameMenu::box(DrawList* list, RectF rect, Color4 color, float radius, float stroke)
{
    if (!list || color.a <= 0.0f)
        return;
    Quad q;
    q.rect = rect;
    q.color = color;
    const float r = std::min(radius, std::min(rect.w, rect.h) / 2);
    q.radii = {r, r, r, r};
    q.stroke = stroke;
    q.clip = clip_;
    list->push(q);
}

void GameMenu::image(DrawList* list, std::string_view id, RectF box, Color4 tint)
{
    if (!list || !images_)
        return;
    const auto picture = images_(id);
    if (!picture || picture->empty())
        return;
    // Fitted into the box, its aspect kept.
    const float iw = static_cast<float>(picture->width), ih = static_cast<float>(picture->height);
    const float s = std::min(box.w / iw, box.h / ih);
    Quad q;
    q.rect = {std::round(box.x + (box.w - iw * s) / 2), std::round(box.y + (box.h - ih * s) / 2), iw * s, ih * s};
    q.kind = QuadKind::Image;
    q.texture = list->texture(picture);
    q.color = tint;
    q.clip = clip_;
    list->push(q);
}

void GameMenu::chevron(DrawList* list, float cx, float cy, float size, int direction, Color4 color)
{
    if (!list)
        return;
    // Two bars meeting at the point, a quarter turn apart.
    const float thickness = 2 * k_;
    const float length = size * std::numbers::sqrt2_v<float> + thickness / 2;
    const float d = static_cast<float>(direction);
    for (const float side : {-1.0f, 1.0f}) {
        const float mx = cx, my = cy + side * size / 2;
        Quad q;
        q.rect = {mx - length / 2, my - thickness / 2, length, thickness};
        q.color = color;
        q.radii = {thickness / 2, thickness / 2, thickness / 2, thickness / 2};
        q.rotation = -d * side * std::numbers::pi_v<float> / 4;
        q.pivotX = mx;
        q.pivotY = my;
        q.clip = clip_;
        list->push(q);
    }
}

void GameMenu::keyChip(DrawList* list, std::string_view key, float x, float centerY, Color4 color, float* width)
{
    const float k = k_;
    const float h = 24 * k;
    const float w = std::max(h, std::round(textWidth(key, 14, bold) + 14 * k));
    if (width)
        *width = w;
    if (!list)
        return;
    const RectF r{std::round(x), std::round(centerY - h / 2), w, h};
    box(list, r, faded(color, 0.08f), 6 * k);
    box(list, r, faded(color, 0.55f), 6 * k, std::max(1.0f, 1.5f * k));
    text(list, key, r, 14, bold, color, X::Center);
}

void GameMenu::focusRing(DrawList* list, const Control& control, float radius)
{
    if (list && isFocused(control))
        box(list, grow(control.rect, 3 * k_), accent, radius + 3 * k_, 2 * k_);
}

GameMenu::Control& GameMenu::button(DrawList* list, std::string id, RectF rect, std::string_view label,
                                    std::string_view key, ButtonStyle style, std::function<void()> press, bool enabled)
{
    Control& c = add({.id = std::move(id), .rect = rect, .enabled = enabled, .press = std::move(press)});
    if (!list)
        return c;
    const float k = k_;
    const bool hover = hovered(c);
    const bool down = hover && pressed_ == c.id;
    Color4 background = fill, foreground = white;
    if (style == ButtonStyle::Primary) {
        background = down ? Color4{0.8f, 0.82f, 0.86f, 1} : hover ? Color4{0.9f, 0.91f, 0.94f, 1} : white;
        foreground = dark;
        if (!enabled) {
            background = {1, 1, 1, 0.18f};
            foreground = {1, 1, 1, 0.45f};
        }
    } else {
        background = down ? pressFill : hover ? hoverFill : fill;
        if (!enabled)
            foreground = disabled;
    }
    box(list, rect, background, 10 * k);
    if (key.empty()) {
        text(list, label, rect, 18, bold, foreground, X::Center);
    } else {
        float w = 0;
        keyChip(list, key, rect.x + 16 * k, rect.y + rect.h / 2, foreground, &w);
        const float x = rect.x + 16 * k + std::max(w, 40 * k) + 14 * k;
        text(list, label, {x, rect.y, rect.right() - x - 12 * k, rect.h}, 19, bold, foreground);
    }
    focusRing(list, c, 10 * k);
    return c;
}

GameMenu::Control& GameMenu::add(Control control)
{
    control.clip = clip_;
    control.content = inContent_;
    controls_.push_back(std::move(control));
    return controls_.back();
}

bool GameMenu::hovered(const Control& control) const
{
    if (!open_ || !control.enabled)
        return false;
    // Under a dialog only its own controls answer.
    if (dialog_ != Dialog::None && !control.id.starts_with("dialog."))
        return false;
    return control.rect.intersect(control.clip).contains(mouseX_, mouseY_);
}

bool GameMenu::isFocused(const Control& control) const
{
    return focusVisible_ && open_ && focus_ && *focus_ == control.id && control.enabled;
}

std::vector<Player*> GameMenu::sortedPlayers(Player** local) const
{
    std::vector<Player*> out;
    auto game = std::static_pointer_cast<DataModel>(game_.lock());
    auto* players = game ? static_cast<Players*>(game->findService("Players")) : nullptr;
    if (!players)
        return out;
    Player* self = players->localPlayer();
    if (local)
        *local = self;
    for (const InstancePtr& child : players->children())
        if (auto* p = dynamic_cast<Player*>(child.get()))
            out.push_back(p);
    // The local player first, then everyone by name.
    std::ranges::sort(out, [self](const Player* a, const Player* b) {
        if ((a == self) != (b == self))
            return a == self;
        return lower(shownName(*a)) < lower(shownName(*b));
    });
    return out;
}

std::vector<Player*> GameMenu::otherPlayers() const
{
    Player* local = nullptr;
    std::vector<Player*> out = sortedPlayers(&local);
    std::erase(out, local);
    return out;
}

} // namespace lunar::ui
