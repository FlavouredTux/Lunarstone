#include <lunar/ui/CoreGui.hpp>

#include <lunar/ui/TextLayout.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace lunar::ui {

Color3 healthColor(float fraction)
{
    constexpr float min = 0.1f;
    constexpr float max = 0.8f;
    if (fraction < min)
        return CoreGui::healthRed;
    if (fraction > max)
        return CoreGui::healthGreen;
    const std::array<std::pair<Color3, float>, 3> samples = {{
        {CoreGui::healthRed, 0.1f},
        {CoreGui::healthYellow, 0.5f},
        {CoreGui::healthGreen, 0.8f},
    }};
    float r = 0, g = 0, b = 0, weights = 0;
    for (const auto& [color, at] : samples) {
        const float distance = fraction - at;
        if (distance == 0.0f)
            return color;
        const float w = 1.0f / (distance * distance);
        r += w * color.r;
        g += w * color.g;
        b += w * color.b;
        weights += w;
    }
    return {r / weights, g / weights, b / weights};
}

CoreGui::CoreGui(FontLibrary& fonts, GlyphAtlas& atlas)
    : fonts_(&fonts), atlas_(&atlas), backpack_(fonts, atlas), menu_(*this, fonts, atlas), healthBars_(fonts, atlas),
      topbar_(fonts, atlas)
{
    topbar_.onMenu = [this] { menu_.setOpen(!menu_.open(), true); };
    topbar_.onSettings = [this] {
        menu_.setOpen(true, true);
        menu_.selectTab(GameMenu::Tab::Settings);
    };
    topbar_.onChat = [this] {
        if (onToggleChat)
            onToggleChat();
    };
    topbar_.onPlayerList = [this] { playerListOpen_ = !playerListOpen_; };
}

void CoreGui::setGame(DataModel* game)
{
    game_ = game ? game->shared_from_this() : nullptr;
    backpack_.setGame(game);
    menu_.setGame(game);
}

void CoreGui::textAt(DrawList& list, std::string_view text, RectF box, float size, Enum::Font font, Color4 color,
                     Enum::TextXAlignment x, Enum::TextYAlignment y, std::optional<RectF> clip)
{
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(font);
    params.style.size = size * scale_;
    params.style.color = {color.r, color.g, color.b};
    params.style.transparency = 1.0f - color.a;
    params.xAlignment = x;
    params.yAlignment = y;
    params.box = {box.w, box.h};
    params.truncate = Enum::TextTruncate::AtEnd;
    list.text(*atlas_, layoutText(*fonts_, params), box.x, box.y, clip ? box.intersect(*clip) : box);
}

bool CoreGui::handle(const platform::Event& event)
{
    struct ConsoleSync {
        CoreGui& core;
        ~ConsoleSync()
        {
            const bool visible = core.menu_.open() && core.menu_.tab() == GameMenu::Tab::Luau;
            if (visible != core.lastConsoleVisible_) {
                core.lastConsoleVisible_ = visible;
                if (auto game = std::static_pointer_cast<DataModel>(core.game_.lock()))
                    game->getService<StarterGui>()->coreSettings().devConsoleVisible = visible;
            }
        }
    } sync{*this};
    const float k = scale_;
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * k;
        mouseY_ = static_cast<float>(move->y) * k;
        backpack_.handle(event);
        menu_.handle(event);
        if (modern_)
            topbar_.handle(event);
        return false;
    }
    if (const auto* key = std::get_if<platform::KeyEvent>(&event);
        key && key->pressed && !key->repeat && key->key == platform::Key::F9) {
        const bool close = menu_.open() && menu_.tab() == GameMenu::Tab::Luau;
        menu_.setOpen(!close, true);
        if (!close)
            menu_.selectTab(GameMenu::Tab::Luau);
        return true;
    }
    // The menu is modal: while it's open it has every key, click and wheel.
    if (menu_.open())
        return menu_.handle(event);
    if (modern_ && topbar_.handle(event))
        return true;
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (!key->pressed || key->repeat)
            return false;
        if (key->key == platform::Key::Escape)
            return menu_.handle(event); // opens it
        if (key->key == platform::Key::Tab && coreGuiEnabled(Enum::CoreGuiType::PlayerList)) {
            playerListOpen_ = !playerListOpen_;
            return true;
        }
        if (key->key == healthBarsKey && !key->modifiers.ctrl && !key->modifiers.alt) {
            menuSettings.healthBars = !menuSettings.healthBars;
            return true;
        }
        return backpack_.handle(event);
    }
    if (std::holds_alternative<platform::MouseButtonEvent>(event))
        return backpack_.handle(event);
    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event); scroll && pointerOverPlayerList()) {
        playerListScroll_ -= static_cast<float>(scroll->dy) * 3 * 26 * scale_; // three rows a notch (GUESS)
        return true;
    }
    return false;
}

bool CoreGui::pointerOverPlayerList() const
{
    return playerListOpen_ && coreGuiEnabled(Enum::CoreGuiType::PlayerList) && !menu_.visible()
           && playerList_.contains(mouseX_, mouseY_);
}

bool CoreGui::coreGuiEnabled(Enum::CoreGuiType type) const
{
    const auto game = std::static_pointer_cast<DataModel>(game_.lock());
    const auto* starter = game ? instanceCast<const StarterGui>(game->findService("StarterGui")) : nullptr;
    return !starter || starter->coreGuiEnabled(type);
}

bool CoreGui::resetEnabled() const
{
    const auto game = std::static_pointer_cast<DataModel>(game_.lock());
    const auto* starter = game ? instanceCast<const StarterGui>(game->findService("StarterGui")) : nullptr;
    const auto* enabled = starter ? std::get_if<bool>(&starter->coreSettings().resetButtonCallback) : nullptr;
    return !enabled || *enabled;
}

void CoreGui::update(double time, Vector2 screen, float topInset, float pixelScale)
{
    if (auto game = std::static_pointer_cast<DataModel>(game_.lock())) {
        auto& state = game->getService<StarterGui>()->coreSettings();
        if (state.devConsoleVisible != lastConsoleVisible_) {
            if (state.devConsoleVisible) {
                menu_.setOpen(true);
                menu_.selectTab(GameMenu::Tab::Luau);
            } else if (menu_.tab() == GameMenu::Tab::Luau)
                menu_.setOpen(false);
        }
        lastConsoleVisible_ = menu_.open() && menu_.tab() == GameMenu::Tab::Luau;
        state.devConsoleVisible = lastConsoleVisible_;
    }
    screen_ = screen;
    top_ = topInset * pixelScale;
    scale_ = pixelScale;
    backpack_.update(screen, pixelScale);
    menu_.update(time, screen, topInset, pixelScale);
    if (const auto game = std::static_pointer_cast<DataModel>(game_.lock()))
        game->getService<GuiService>()->assignMenuIsOpen(menu_.open());
    if (modern_) {
        const auto game = std::static_pointer_cast<DataModel>(game_.lock());
        const auto* starter = game ? instanceCast<StarterGui>(game->findService("StarterGui")) : nullptr;
        topbar_.enabled = !starter || starter->coreSettings().topbarEnabled;
        topbar_.chatEnabled = coreGuiEnabled(Enum::CoreGuiType::Chat);
        topbar_.chatOpen = !starter || starter->coreSettings().chatActive;
        topbar_.playerListEnabled = coreGuiEnabled(Enum::CoreGuiType::PlayerList);
        topbar_.playerListOpen = playerListOpen_;
        topbar_.update(screen, pixelScale);
    }
}

void CoreGui::draw(DrawList& list)
{
    const float k = scale_;
    const RectF all{0, 0, screen_.x, screen_.y};
    auto game = std::static_pointer_cast<DataModel>(game_.lock());
    Players* players = game ? static_cast<Players*>(game->findService("Players")) : nullptr;
    Player* local = players ? players->localPlayer() : nullptr;

    // Health bar: top right inside the top bar, the player's name over a thin bar.
    if (local && coreGuiEnabled(Enum::CoreGuiType::Health)) {
        Humanoid* humanoid = nullptr;
        if (Model* character = local->character())
            humanoid = dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid"));
        const float width = 150 * k; // guess
        const float x = screen_.x - width - 10 * k;
        const std::string& name = local->displayName().empty() ? local->name() : local->displayName();
        if (!modern_)
            textAt(list, name, {x, 2 * k, width, 18 * k}, 14, Enum::Font::SourceSansBold, {1, 1, 1, 1},
                   Enum::TextXAlignment::Left, Enum::TextYAlignment::Center);
        if (humanoid) {
            const float fraction
                = humanoid->maxHealth() > 0 ? std::clamp(humanoid->health() / humanoid->maxHealth(), 0.0f, 1.0f) : 0.0f;
            if (!modern_ || fraction < 1.0f) {
                const RectF bar{x, (modern_ ? 16 : 22) * k, width, 6 * k};
                list.rect(bar, Color4::from(healthBackground, 0.5f), all);
                list.rect({bar.x, bar.y, std::round(bar.w * fraction), bar.h}, Color4::from(healthColor(fraction)),
                          all);
            }
        }
    }

    // Player list: right side under the top bar, one row per player, at most
    // half the screen tall (GUESS: the core list's cap) and scrolled with the
    // wheel past that, so it leaves the corner below it free.
    playerList_ = {};
    if (playerListOpen_ && players && coreGuiEnabled(Enum::CoreGuiType::PlayerList) && !menu_.visible()) {
        const float width = 170 * k; // guess
        const float row = 24 * k;
        const float x = screen_.x - width - 4 * k;
        float y = top_ + 4 * k;
        std::vector<Player*> list_;
        for (const InstancePtr& child : players->children())
            if (auto* p = dynamic_cast<Player*>(child.get()))
                list_.push_back(p);
        std::ranges::sort(list_, {}, [](const Player* p) { return p->name(); });
        if (!list_.empty()) {
            list.rect({x, y, width, row}, {0.12f, 0.12f, 0.12f, 0.75f}, all);
            textAt(list, "Players", {x + 8 * k, y, width - 16 * k, row}, 14, Enum::Font::SourceSansBold, {1, 1, 1, 1},
                   Enum::TextXAlignment::Left, Enum::TextYAlignment::Center);
            y += row + 2 * k;
        }
        const float step = row + 2 * k;
        const float content = static_cast<float>(list_.size()) * step;
        const float room = std::max(step, std::floor((screen_.y * 0.5f - (y - top_)) / step) * step);
        const RectF rows{x, y, width, std::min(content, room)};
        playerListScroll_ = std::clamp(playerListScroll_, 0.0f, std::max(0.0f, content - rows.h));
        playerList_ = {x, top_, width, rows.bottom() - top_};
        float at = y - playerListScroll_;
        for (const Player* p : list_) {
            if (at + row > rows.y && at < rows.bottom()) {
                const bool self = p == local;
                list.rect({x, at, width, row}, self ? Color4{0.2f, 0.2f, 0.2f, 0.6f} : Color4{0, 0, 0, 0.5f}, rows);
                textAt(list, p->displayName().empty() ? p->name() : p->displayName(),
                       {x + 8 * k, at, width - 16 * k, row}, 14, Enum::Font::SourceSans, {1, 1, 1, 1},
                       Enum::TextXAlignment::Left, Enum::TextYAlignment::Center, rows);
            }
            at += step;
        }
        if (content > rows.h) { // the scroll bar
            const float bar = std::max(12 * k, rows.h * rows.h / content);
            const float offset = (rows.h - bar) * playerListScroll_ / (content - rows.h);
            list.rect({x + width - 3 * k, rows.y + offset, 3 * k, bar}, {1, 1, 1, 0.6f}, all);
        }
    }

    backpack_.draw(list);

    drawStats(list);
    if (modern_ && !menu_.visible())
        topbar_.draw(list);
    menu_.draw(list);
}

void CoreGui::drawWorld(DrawList& list)
{
    if (menuSettings.healthBars) {
        auto game = std::static_pointer_cast<DataModel>(game_.lock());
        healthBars_.update(game.get(), screen_, scale_);
        healthBars_.draw(list);
    } else if (!healthBars_.bars().empty()) {
        healthBars_.update(nullptr, screen_, scale_); // forgets what it kept, once
    }
}

void CoreGui::drawStats(DrawList& list)
{
    if (!menuSettings.performanceStats || !stats_)
        return;
    // One line at the top in the middle, clear of the chat and the health bar.
    const float k = scale_;
    std::string line = std::format("{:.0f} fps   {:.1f} ms   {} draws   ", stats_->fps, stats_->frameMilliseconds,
                                   stats_->drawCalls);
    line += stats_->triangles >= 1'000'000
                ? std::format("{:.2f}M triangles", static_cast<double>(stats_->triangles) / 1e6)
                : std::format("{}k triangles", stats_->triangles / 1000);
    const lunar::Font font = fontFromEnum(Enum::Font::BuilderSansMedium);
    const float w = measureText(*fonts_, line, font, 15 * k).x + 24 * k;
    const RectF box{std::round((screen_.x - w) / 2), top_ + 6 * k, std::round(w), 26 * k};
    Quad q;
    q.rect = box;
    q.color = {0.05f, 0.05f, 0.06f, 0.7f};
    q.radii = {13 * k, 13 * k, 13 * k, 13 * k};
    list.push(q);
    textAt(list, line, box, 15, Enum::Font::BuilderSansMedium, {1, 1, 1, 0.95f}, Enum::TextXAlignment::Center,
           Enum::TextYAlignment::Center);
}

} // namespace lunar::ui
