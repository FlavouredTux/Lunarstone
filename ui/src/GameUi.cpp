#include <lunar/ui/GameUi.hpp>

#include <lunar/render/Context.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <chrono>
#include <cmath>

namespace lunar::ui {

std::expected<std::unique_ptr<GameUi>, std::string>
GameUi::create(render::Context& context, VkFormat colorFormat, std::uint32_t slots, DataModel& game, Options options)
{
    std::unique_ptr<GameUi> ui(new GameUi(game, std::move(options)));
    auto renderer = UiRenderer::create(context, colorFormat, slots);
    if (!renderer)
        return std::unexpected(renderer.error());
    ui->renderer_ = std::move(*renderer);
    auto map = WorldMap::create(context, slots, ui->fonts_, ui->atlas_);
    if (!map)
        return std::unexpected(map.error());
    ui->map_ = std::move(*map);
    ui->map_->settings() = ui->options_.map;
    ui->map_->setGame(&game);
    ui->map_->attach(*ui->renderer_);
    ui->viewports_ = std::make_unique<ViewportFrames>(context, *ui->renderer_, slots);
    ui->gui_.setViewportProvider([viewports = ui->viewports_.get()](ViewportFrame& frame, std::uint32_t width,
                                                                    std::uint32_t height) {
        return viewports->show(frame, width, height);
    });
    return ui;
}

std::unique_ptr<GameUi> GameUi::createHeadless(DataModel& game, Options options)
{
    return std::unique_ptr<GameUi>(new GameUi(game, std::move(options)));
}

GameUi::GameUi(DataModel& game, Options options)
    : game_(&game), options_(std::move(options)), fonts_(options_.contentDir), gui_(fonts_, atlas_),
      chat_(fonts_, atlas_), core_(fonts_, atlas_), loading_(fonts_, atlas_), notifications_(fonts_, atlas_),
      invites_(fonts_, atlas_)
{
    gui_.setImageProvider(options_.images);
    gui_.settings().topInset = options_.topInset;
    core_.setGame(&game);
    core_.setModern(options_.modernUi);
    chat_.setGame(options_.modernUi ? &game : nullptr);
    core_.onToggleChat = [this] {
        auto& settings = game_->getService<StarterGui>()->coreSettings();
        settings.chatActive = !settings.chatActive;
        if (!settings.chatActive)
            chat_.releaseFocus();
    };
    core_.setImageProvider(options_.images);
    core_.onLeave = [this] {
        if (onLeave)
            onLeave();
    };
    loading_.setImageProvider(options_.images);
    notifications_.setImageProvider(options_.images);
    invites_.setImageProvider(options_.images);
    loading_.onLeave = [this] {
        if (onLeave)
            onLeave();
    };
    // Reset Character: the local Humanoid's Health to 0, as the CoreScripts'
    // default ResetButtonCallback does. The character dies like any other
    // death: the session respawns it, a server hears its Dead state.
    core_.onReset = [this] {
        if (const auto* starter = instanceCast<StarterGui>(game_->findService("StarterGui"))) {
            const Value& callback = starter->coreSettings().resetButtonCallback;
            if (const auto* enabled = std::get_if<bool>(&callback); enabled && !*enabled)
                return;
            if (const auto* event = std::get_if<InstancePtr>(&callback); event && *event) {
                (*event)->fireEvent("Event");
                return;
            }
        }
        auto* players = static_cast<Players*>(game_->findService("Players"));
        Player* local = players ? players->localPlayer() : nullptr;
        Model* character = local ? local->character() : nullptr;
        if (auto* humanoid
            = character ? dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr)
            humanoid->setHealth(0.0f);
    };
    chat_.visible = options_.showChat;

    // Offline chat: the local player (or a stand-in) hears their own messages.
    std::string name = "Player1";
    std::string displayName = name;
    if (auto* players = static_cast<Players*>(game.findService("Players")))
        if (Player* local = players->localPlayer()) {
            name = local->name();
            displayName = local->displayName().empty() ? name : local->displayName();
        }
    echo_ = std::make_unique<LocalEchoChatSource>(name, displayName);
    chat_.setSource(echo_.get());
    chat_.postSystem("Chat '/?' or '/help' for a list of chat commands.");
    refreshRoots();
}

GameUi::~GameUi()
{
    alive_.reset();
    detach();
    if (map_ && renderer_)
        map_->detach(*renderer_);
}

void GameUi::attach(render::MeshRenderer& renderer)
{
    detach();
    renderer.setOverlay(renderer_.get());
    attached_ = &renderer;
}

void GameUi::detach()
{
    if (attached_ && attached_->overlay() == renderer_.get())
        attached_->setOverlay(nullptr);
    attached_ = nullptr;
}

void GameUi::refreshRoots()
{
    // The local player's PlayerGui while playing; StarterGui otherwise, like Studio's edit view.
    std::vector<Instance*> roots;
    if (auto* players = static_cast<Players*>(game_->findService("Players")))
        if (Player* local = players->localPlayer())
            if (Instance* playerGui = local->findFirstChildOfClass("PlayerGui"))
                roots.push_back(playerGui);
    if (roots.empty())
        if (Instance* starter = game_->findService("StarterGui"))
            roots.push_back(starter);
    if (Instance* core = game_->findService("CoreGui"))
        roots.push_back(core);
    gui_.setWorld(game_->workspace());
    gui_.setRoots(roots);
    roots_ = std::move(roots);
}

Vector2 GameUi::measureText(std::string_view text, const Font& font, float size, float width, bool richText)
{
    TextParams params;
    params.text = text;
    params.style.font = font;
    params.style.size = size;
    params.wrapped = width > 0.0f;
    params.richText = richText;
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Top;
    params.box = {width > 0.0f ? width : 1e6f, 1e6f};
    const TextLayout layout = layoutText(fonts_, params);
    return {std::ceil(layout.bounds.x), std::ceil(layout.bounds.y)};
}

bool GameUi::keyboardCaptured() const
{
    return invites_.visible() || chat_.focused() || gui_.textBoxFocused();
}

std::function<bool(std::shared_ptr<GameInviteSession>)> GameUi::gameInviteHandler()
{
    return [this, weak = std::weak_ptr<int>(alive_)](std::shared_ptr<GameInviteSession> session) {
        if (weak.expired())
            return false;
        chat_.releaseFocus();
        gui_.releaseFocus(false);
        core_.setMenuOpen(false);
        if (map_)
            map_->setFullOpen(false);
        invites_.open(std::move(session));
        invites_.update(time_, framebufferSize_, gui_.settings().pixelScale);
        return true;
    };
}

bool GameUi::minimapShown() const
{
    if (!map_ || !options_.showCoreGui || !map_->settings().minimap)
        return false;
    // Games hide the whole core HUD with SetCoreGuiEnabled(All, false); the
    // minimap counts as part of it then (one type alone leaves it).
    if (const auto* starter = dynamic_cast<const StarterGui*>(game_->findService("StarterGui")))
        if (!starter->coreGuiEnabled(Enum::CoreGuiType::PlayerList)
            && !starter->coreGuiEnabled(Enum::CoreGuiType::Health)
            && !starter->coreGuiEnabled(Enum::CoreGuiType::Backpack)
            && !starter->coreGuiEnabled(Enum::CoreGuiType::Chat))
            return false;
    return true;
}

bool GameUi::wheelCaptured() const
{
    return invites_.visible() || chat_.pointerOver() || (map_ && (map_->fullOpen() || map_->pointerOverMinimap()))
           || core_.pointerOverPlayerList() || (options_.modernUi && core_.topbar().pointerOver())
           || gui_.wheelTaken();
}

std::vector<InstancePtr> GameUi::guiObjectsAt(float x, float y)
{
    // Scripts can clone or move a button and query it in the same resumption.
    refreshRoots();
    styles_.apply(roots_);
    gui_.update(framebufferSize_, time_);
    return gui_.objectsAt(x, y);
}

void GameUi::ensureGuiGeometry(Instance* object)
{
    if (ensuringGeometry_ || gui_.layoutInProgress())
        return;
    ensuringGeometry_ = true;
    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{ensuringGeometry_};
    refreshRoots();
    const auto* layer
        = object ? (object->isA("LayerCollector") ? object : object->findFirstAncestorWhichIsA("LayerCollector"))
                 : nullptr;
    const bool world = layer && (layer->isA("SurfaceGui") || layer->isA("BillboardGui"));
    if (!world && !gui_.geometryQueryNeedsUpdate())
        return;
    styles_.apply(roots_);
    if (world)
        gui_.update(framebufferSize_, time_, true);
    else
        gui_.ensureGeometry(framebufferSize_, time_);
}

void GameUi::focusTextBox(TextBox* box, bool capture, bool enterPressed)
{
    if (chat_.ownsTextBox(box)) {
        if (capture) {
            chat_.selectTab(ClassicChat::Tab::Game);
            chat_.focus();
        } else if (chat_.selectedTab() == ClassicChat::Tab::Game)
            chat_.releaseFocus(enterPressed);
        return;
    }
    if (capture) {
        chat_.releaseFocus();
        guiObjectsAt(0, 0); // current geometry, including a box created this resumption
        gui_.focus(box);
    } else if (game_->getService<UserInputService>()->focusedTextBox() == box) {
        gui_.releaseFocus(enterPressed);
    }
}

bool GameUi::handle(const platform::Event& event)
{
    gui_.clearInputDispatch();
    struct InputGuard {
        GuiEngine& gui;
        const platform::Event& event;
        ~InputGuard() { gui.finishInputEvent(event); }
    } guard{gui_, event};
    if (!loading_.blocking() && invites_.visible())
        return invites_.handle(event);
    const bool pointer = std::holds_alternative<platform::MouseMoveEvent>(event);
    if (pointer) {
        // Everyone tracks the pointer; moving it never belongs to one of them.
        loading_.handle(event);
        core_.handle(event);
        chat_.handle(event);
        gui_.handle(event);
        if (!map_)
            return false;
        if (map_->fullOpen())
            map_->handleFull(event, map_->fullArea());
        else
            map_->handleMinimap(event);
        return false;
    }
    // The loading screen covers the rest: nothing under it gets input.
    if (loading_.blocking())
        return loading_.handle(event);
    if (options_.showCoreGui && !core_.menuOpen() && notifications_.handle(event))
        return true;
    if (options_.showChat && chat_.focused()) {
        if (chat_.handle(event) || std::holds_alternative<platform::KeyEvent>(event))
            return true;
    }
    if (gui_.textBoxFocused() && !std::holds_alternative<platform::MouseButtonEvent>(event))
        return gui_.handle(event);
    // The full map is modal; the hotkey opens it when the menu isn't up.
    if (map_ && map_->fullOpen())
        return map_->handleFull(event, map_->fullArea());
    if (map_ && !core_.menuOpen() && map_->handleHotkey(event))
        return true;
    if (map_ && !core_.menuOpen() && map_->handleMinimap(event))
        return true;
    if (options_.showCoreGui && core_.handle(event))
        return true;
    if (const auto* text = std::get_if<platform::TextEvent>(&event);
        text && text->text == "/" && options_.showChat && !chat_.visible) {
        auto* starter = game_->getService<StarterGui>();
        if (starter->coreGuiEnabled(Enum::CoreGuiType::Chat)) {
            starter->coreSettings().chatActive = true;
            chat_.visible = true;
        }
    }
    if (options_.showChat && chat_.handle(event))
        return true;
    return gui_.handle(event);
}

void GameUi::update(double dt, Vector2 framebufferSize, float pixelScale)
{
    const bool wasEnsuring = ensuringGeometry_;
    ensuringGeometry_ = true;
    struct Guard {
        bool& value;
        bool previous;
        ~Guard() { value = previous; }
    } guard{ensuringGeometry_, wasEnsuring};
    using Clock = std::chrono::steady_clock;
    UpdateTimes times;
    Clock::time_point mark = Clock::now();
    const auto lap = [&mark](double& into) {
        const Clock::time_point now = Clock::now();
        into += std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
    };
    const std::uint64_t rasterizedBefore = atlas_.rasterized();
    time_ += dt;
    framebufferSize_ = framebufferSize;
    refreshRoots();
    styles_.apply(roots_); // styled properties in place before the layout reads them
    lap(times.styles);
    chat_.visible = options_.showChat;
    if (auto* starter = instanceCast<StarterGui>(game_->findService("StarterGui"))) {
        chat_.visible
            = chat_.visible && starter->coreGuiEnabled(Enum::CoreGuiType::Chat) && starter->coreSettings().chatActive;
        for (auto& message : starter->takeSystemChat())
            chat_.postSystem(std::move(message.text), message.color);
        for (auto& notification : starter->takeNotifications())
            notifications_.post(std::move(notification));
    }
    if (!chat_.visible && chat_.focused())
        chat_.releaseFocus();
    float topInset = options_.topInset;
    if (const auto* starter = instanceCast<StarterGui>(game_->findService("StarterGui"));
        starter && options_.showCoreGui && options_.modernUi && !starter->coreSettings().topbarEnabled)
        topInset = 0.0f;
    if (map_)
        core_.setMapKey(map_->settings().fullMapKey);
    core_.update(time_, framebufferSize, topInset, pixelScale);
    lap(times.core);
    if (options_.showCoreGui && options_.modernUi) {
        gui_.settings().topbarSafeArea = core_.topbar().safeArea();
        if (!core_.topbar().enabled)
            topInset = 0.0f;
    }
    gui_.settings().topInset = topInset;
    gui_.settings().pixelScale = pixelScale;
    mark = Clock::now();
    gui_.update(framebufferSize, time_);
    lap(times.layout);
    chat_.update(time_, framebufferSize, topInset, pixelScale);
    if (chat_.focused() && gui_.textBoxFocused())
        gui_.releaseFocus(false);
    if (map_) {
        if (core_.menuOpen())
            map_->setFullOpen(false);
        map_->setMinimapHidden(!minimapShown() || core_.menuVisible());
        map_->update(time_, framebufferSize, pixelScale);
    }
    loading_.update(time_, framebufferSize, pixelScale);
    notifications_.update(time_, framebufferSize, pixelScale);
    invites_.update(time_, framebufferSize, pixelScale);

    lap(times.others);
    if (!renderer_) { // headless: nothing to draw
        lastTimes_ = times;
        return;
    }
    list_.clear();
    atlas_.beginFrame();
    if (attached_)
        viewports_->setGraphics(attached_->graphics());
    viewports_->beginFrame();
    if (options_.showCoreGui)
        core_.drawWorld(list_);
    gui_.draw(list_);
    chat_.draw(list_);
    map_->drawMinimap(list_);
    if (options_.showCoreGui)
        core_.draw(list_);
    if (options_.showCoreGui && !core_.menuVisible())
        notifications_.draw(list_);
    map_->drawFull(list_, {0, 0, framebufferSize.x, framebufferSize.y});
    invites_.draw(list_);
    loading_.draw(list_);
    if (!cursorIcon_.empty() && options_.images && !menuOpen() && !map_->fullOpen() && !loading_.visible())
        if (auto image = options_.images(cursorIcon_); image && !image->empty()) {
            const float w = static_cast<float>(image->width) * pixelScale;
            const float h = static_cast<float>(image->height) * pixelScale;
            Quad q;
            q.rect = {cursorAt_.x - w * 0.5f, cursorAt_.y - h * 0.5f, w, h};
            q.kind = QuadKind::Image;
            q.texture = list_.texture(image);
            q.color = {1.0f, 1.0f, 1.0f, 1.0f};
            list_.push(q);
        }
    lap(times.draw);
    renderer_->submit(list_, atlas_);
    lap(times.submit);
    times.glyphsRasterized = atlas_.rasterized() - rasterizedBefore;
    times.raster = atlas_.takeRasterStats();
    lastTimes_ = times;
}

} // namespace lunar::ui
