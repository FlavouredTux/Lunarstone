#include "LivePlay.hpp"

#include "ConsoleApi.hpp"
#include "GameDetails.hpp"
#include "GameWindow.hpp"
#include "LiveChat.hpp"
#include "LiveClient.hpp"
#include "LiveScene.hpp"
#include "MapScene.hpp"
#include "MusicVisualizer.hpp"
#include "NetJoin.hpp"
#include "NetTools.hpp"
#include "PlaceScene.hpp"
#include "PlaySession.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Image.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/audio/SoundSystem.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/ui/GameUi.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <malloc.h>

namespace {

using Clock = std::chrono::steady_clock;
namespace physics = lunar::physics;
namespace render = lunar::render;

// While the window's thread waits (for its frame's turn on screen, which a
// hidden window can hold for seconds; for the window to open), another
// thread keeps the connection up through GameFeed::service, so the server
// doesn't time us out. Nothing arrives at the replica meanwhile: the main
// thread takes it in at the next pump.
class KeepAlive {
public:
    explicit KeepAlive(GameFeed& feed) : thread_([this, &feed](const std::stop_token& stop) { run(stop, feed); }) {}
    ~KeepAlive()
    {
        thread_.request_stop();
        thread_.join();
    }
    KeepAlive(const KeepAlive&) = delete;
    KeepAlive& operator=(const KeepAlive&) = delete;

    // From now on until end() the feed is the other thread's.
    void begin()
    {
        const std::scoped_lock lock(mutex_);
        active_ = true;
        wake_.notify_all();
    }
    // Back to the main thread, once the other is done with the feed.
    void end()
    {
        std::unique_lock lock(mutex_);
        active_ = false;
        wake_.notify_all();
        idle_.wait(lock, [this] { return !busy_; });
    }

private:
    void run(const std::stop_token& stop, GameFeed& feed)
    {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            if (!active_) {
                wake_.wait(lock, stop, [this] { return active_; });
                continue;
            }
            busy_ = true;
            lock.unlock();
            feed.service();
            lock.lock();
            busy_ = false;
            idle_.notify_all();
            wake_.wait_for(lock, stop, std::chrono::milliseconds(2), [this] { return !active_; });
        }
    }

    std::mutex mutex_;
    std::condition_variable_any wake_, idle_;
    bool active_ = false;
    bool busy_ = false;
    std::jthread thread_; // last: it starts running once the rest is made
};

// The game's name, creator and icon for the loading screen, asked of the
// web on a thread of its own so the window never waits for them. Let go of
// (a teleport, the window closing), it gives up on its request at once.
class DetailsLookup {
public:
    explicit DetailsLookup(std::uint64_t placeId)
        : thread_([this, placeId](const std::stop_token& stop) {
              const lunar::assets::HttpStopScope downloads(stop);
              auto found = fetchGameDetails(lunar::assets::RobloxWeb(), placeId);
              const std::scoped_lock lock(mutex_);
              result_ = std::move(found);
          })
    {
    }

    // The answer, once, when it's in.
    std::optional<std::expected<GameDetails, std::string>> take()
    {
        const std::scoped_lock lock(mutex_);
        return std::exchange(result_, std::nullopt);
    }

private:
    std::mutex mutex_;
    std::optional<std::expected<GameDetails, std::string>> result_;
    std::jthread thread_; // last: it starts running once the rest is made
};

// The seconds a game that spawns no character for us keeps the loading
// screen up once its scene is there.
constexpr double characterWait = 5.0;

// Where each frame's time goes, over the seconds between log lines, and
// what took the longest in the slowest frame.
struct FrameTimes {
    double network = 0, scripts = 0, camera = 0, scene = 0, ui = 0, draw = 0, worst = 0;
    std::string worstIn;
    int frames = 0;
};

struct NetPlayOptions {
    std::optional<std::uint64_t> placeId;
    std::string replay; // a recording's prefix instead of a place
    LiveJoinOptions join;
    std::string scriptPlatform;
    double fps = 0; // 0: vsync
    render::GraphicsSettings graphics;
    bool graphicsFromFlags = false; // --graphics and the like: they win over the player's kept settings
    std::vector<Screenshot> screenshots;
    std::filesystem::path screenshotRequest;
    std::filesystem::path control; // --control: input from a file (ControlFile)
    std::filesystem::path bot;     // --bot: Luau run in the console once the client starts, playing the window
    bool shiftLock = false;        // --shift-lock: the menu's Shift Lock Switch on to start with
    double seconds = 0;
    bool minimap = false;             // --minimap: Lunarstone's map, our own marker only
    bool mute = false;                // --mute: no sound output (the sounds still load, for IsLoaded and Ended)
    std::optional<std::uint16_t> api; // --api: the Luau tab over HTTP on this port (ConsoleApi)
    bool headless = false;            // --headless: no window, GPU or sound output (playHeadless)
};

// `api` (--api, else null) goes to each game's console as the window's GUI
// brings it.
int play(GameFeed& feed, const NetPlayOptions& options, const std::string& title, ConsoleApi* api)
{
    KeepAlive keepAlive(feed);

    GameWindow::Options windowOptions{.title = "Lunarstone - " + title,
                                      .graphics = options.graphics,
                                      .vsync = options.fps <= 0,
                                      .fpsCap = options.fps > 0 ? static_cast<int>(options.fps) : 0,
                                      .shiftLock = options.shiftLock,
                                      .screenshots = options.screenshots,
                                      .seconds = options.seconds,
                                      .screenshotRequest = options.screenshotRequest,
                                      .control = options.control,
                                      .automated = !options.bot.empty()};
    // The player's kept settings (Settings.hpp), unless a tool plays or
    // pictures the window (--control, --bot, --screenshot): those runs must
    // not depend on anyone's settings.
    if (options.control.empty() && options.bot.empty() && options.screenshots.empty()) {
        windowOptions.settingsFile = playerSettingsFile();
        windowOptions.graphicsFromFlags = options.graphicsFromFlags;
    }
    // The real client has no minimap or full-screen map; --minimap adds
    // Lunarstone's, which on a real server never shows the other players.
    if (!options.minimap) {
        windowOptions.map.minimap = false;
        windowOptions.map.fullMapKey.reset();
    }
    windowOptions.map.showOtherPlayers = false;
    keepAlive.begin();
    auto made = GameWindow::create(*feed.replica()->game(), windowOptions);
    keepAlive.end();
    if (!made) {
        std::println(stderr, "{}", made.error());
        feed.leave();
        return 1;
    }
    GameWindow& window = **made;
    feed.configureClient = [&window](lunar::ClientSession& clientSession) { window.bindGui(clientSession); };
    const ConsoleApi::Detach detachApi(api); // before the window's GUI and its console go
    lunar::platform::Window& platformWindow = window.window();
    render::Presenter& presenter = window.presenter();
    // The Music tab's beat ring, for the whole session (teleports included).
    MusicVisualizer music(presenter.renderer());
    std::string facingWhy; // why our character faces the camera, for the log
    std::println(
        "play: WASD walks, Space jumps, right mouse turns the camera, the wheel zooms, / chats, 1-9 and 0 " "equip " "t"
                                                                                                                     "o"
                                                                                                                     "o"
                                                                                                                     "l"
                                                                                                                     "s"
                                                                                                                     ","
                                                                                                                     " "
                                                                                                                     "a"
                                                                                                                     " " "left " "click " "uses " "them, " "Backspace " "drops " "them, Esc " "opens the " "menu " "(Leave " "quits)");
    std::println("play: {}", options.fps > 0 ? std::format("{:.0f} frames a second at most", options.fps)
                                             : "a frame each time the display shows one (vsync)");

    // What keeps the mouse off a SurfaceGui: anything nearer in our world of
    // the game, our own character aside.
    window.ui().gui().setWorldPick(
        [&feed](lunar::Vector3 origin, lunar::Vector3 direction) -> std::optional<lunar::ui::GuiEngine::WorldHit> {
            LiveClient* client = feed.client();
            const LiveCharacter* self = client ? client->character() : nullptr;
            const physics::World* world = self ? self->world() : nullptr;
            if (!world)
                return std::nullopt;
            physics::RaycastParams params;
            if (lunar::Model* body = self->character())
                params.filterDescendantsInstances.push_back(body);
            const auto hit = world->raycast(origin, direction, params);
            if (!hit || !hit->instance)
                return std::nullopt;
            return lunar::ui::GuiEngine::WorldHit{hit->instance, hit->distance};
        });
    // The first game's place, known before its join is in (not a private
    // server's: its link says which place once the join has resolved it).
    std::uint64_t askedPlace = options.placeId.value_or(0);
    // The game ended before it got in (the join failed, or the server let us
    // go while loading): the exit code says so.
    bool joinFailed = false;

    // One pass a game: a teleport ends it, and the window goes on with the
    // next server's game (feed.generation()).
    for (;;) {
        // The last pass's scene, chat and GUI are gone: so can the game they showed.
        feed.releasePrevious();
        const std::uint64_t generation = feed.generation();
        const std::shared_ptr<lunar::net::Replica> replica = feed.replica();
        const std::shared_ptr<lunar::DataModel> game = replica->game();
        lunar::Workspace& workspace = *game->workspace();
        lunar::ui::GameUi& ui = window.ui();

        // Built once the join is in: the parts and terrain as they are then,
        // and every instance that comes or goes after. The renderer draws with
        // the scene's providers, so the scene outlives the window.
        Wardrobe wardrobe(workspace);
        std::unique_ptr<PlaceScene> scene;
        std::unique_ptr<render::ParticleSystem> particles; // the Workspace's ParticleEmitters, with the scene
        std::unique_ptr<MapScene> map;                     // --minimap's picture of the place, from the scene
        std::unique_ptr<lunar::audio::SoundSystem> sounds; // the game's Sounds, heard from the camera
        std::unique_ptr<LiveCamera> camera;
        lunar::Connection added, removing;
        // The chat window over the client's TextChatService: what we type goes
        // to the server, what the server says comes in (and over the heads).
        std::unique_ptr<TextChatSource> chat;
        // A graphics preset picked in the menu: what the renderer doesn't hold
        // itself, once the scene is there (it's made with the window's then).
        window.onGraphics = [&scene, &particles, terrainDistance = window.graphics().terrainDistance](
                                const render::GraphicsSettings& graphics) mutable {
            if (scene && graphics.terrainDistance != terrainDistance)
                scene->setTerrainDistance(graphics.terrainDistance);
            terrainDistance = graphics.terrainDistance;
            if (particles)
                particles->setSettings({.density = graphics.particleDensity});
        };
        ui.core().setTitle(title);
        std::string gameTitle = title; // the window's, the game's name once the web has said it
        bool teleported = false;
        std::optional<double> serverAt; // when the join had found its server and the session began

        // The loading screen, from the join until the game is there (or the
        // game's own ReplicatedFirst loader takes it down); its icon, name
        // and creator once the web has answered.
        lunar::ui::LoadingScreen& loading = ui.loading();
        loading.show();
        loading.setStatus("Joining server");
        std::unique_ptr<DetailsLookup> details;
        bool detailsAsked = false;
        std::optional<double> sceneAt; // when the scene was built
        bool teleportShown = false;    // the loading screen went up for a teleport, and has been drawn
        bool joined = false;           // our client has started: the game got in

        render::Lighting lighting;
        double gameTime = 0.0, lightingAt = 0.0;
        lunar::Vector2 mouse;
        bool toldEnd = false;
        std::optional<double> endedAt; // when the game ended, by the window's clock
        FrameTimes times;
        auto lastLog = Clock::now();
        std::string titleStatus = "joining";
        // The menu's Luau tab: this game's client, once there is one. A
        // teleport drops it before the client goes.
        lunar::ui::DevConsole& console = ui.core().console();
        LiveClient* consoleClient = nullptr;
        LiveClient* botClient = nullptr;
        console.run = nullptr;
        if (api)
            api->attach(&console);

        while (!platformWindow.shouldClose()) {
            const auto frameStart = Clock::now();
            const float dt = window.beginFrame([&](const lunar::platform::Event& event) {
                LiveClient* client = feed.client();
                if (!client)
                    return;
                // The tool in hand, as the client's ToolMouseCommand and drop key
                // work it (the hotbar's keys and slots are the backpack GUI's).
                if (const auto* button = std::get_if<lunar::platform::MouseButtonEvent>(&event);
                    button && button->button == lunar::platform::MouseButton::Left)
                    client->session()->toolButton(button->pressed);
                else if (const auto* key = std::get_if<lunar::platform::KeyEvent>(&event);
                         key && key->pressed && !key->repeat && key->key == lunar::platform::Key::Backspace)
                    client->session()->dropTools();
            }, [&](const lunar::platform::Event& event, bool processed) {
                if (LiveClient* client = feed.client()) {
                    const bool locked = (platformWindow.pointerLockRequested() && platformWindow.pointerLocked()) || window.controlled();
                    forwardInput(client->session()->input(), event, mouse, locked,
                                 ui.menuOpen() || ui.mapOpen() || ui.loadingScreenUp(), processed, ui.gui().takeInputDispatch());
                }
            });
            auto mark = Clock::now();
            std::pair<double, const char*> longest{0.0, ""};
            auto lap = [&](double& into, const char* what) {
                const auto t = Clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t - mark).count();
                into += ms;
                longest = std::max(longest, std::pair{ms, what});
                mark = t;
            };

            // The network first: what the server sent since the last frame, and
            // what we owe it, without waiting. When the server has sent us
            // elsewhere, the pumps leave it and join the next server behind
            // the loading screen (from the frame after it went up), and the
            // pass ends once the feed is there.
            if (!feed.teleporting() || teleportShown)
                feed.pump();
            if (feed.generation() != generation) {
                teleported = true; // the feed is on another server now
                break;
            }
            if (feed.teleporting()) {
                if (!teleportShown) {
                    loading.show();
                    teleportShown = true;
                }
                loading.setStatus(feed.joining().value_or("Teleporting"));
            }
            if (!serverAt && !feed.joining())
                serverAt = window.elapsed();
            // A per-frame step of the replica itself (say the server's physics
            // for the other characters, moved on to this frame) goes here: after
            // what arrived, before the scripts and the camera see the frame.
            lap(times.network, "network");

            // The client's frame: the game's LocalScripts in RunService's order
            // with this frame's time, our character's physics and the animations
            // in their phases.
            if (LiveClient* client = feed.client()) {
                client->session()->setViewportSize(window.logicalSize());
                if (const auto* players = static_cast<const lunar::Players*>(game->findService("Players")))
                    if (const lunar::Player* local = players->localPlayer())
                        window.setRtcRobloxUsername(local->name());
                if (!chat) {
                    chat = std::make_unique<TextChatSource>(client->session()->client());
                    chat->observe = [client](const lunar::ChatLine& line) {
                        std::println("{}", describeChatLine(line));
                        if (ChatBubbles* bubbles = client->bubbles())
                            bubbles->add(line, client->time());
                    };
                    ui.setChatSource(chat.get());
                }
            }
            // The ControlModule: WASD (and the arrows) walk our character, turned
            // by the camera's heading, and Space jumps; nothing while the GUI has
            // the keyboard. The client's frame simulates it and sends it on.
            if (LiveClient* client = feed.client(); client && client->character()) {
                const lunar::platform::InputState& keys = platformWindow.input();
                const bool controls = keys.focused() && !window.uiHasKeys();
                auto held = [&](lunar::platform::Key key) { return controls && keys.isDown(key); };
                using lunar::platform::Key;
                const float x = (held(Key::D) ? 1.0f : 0.0f) - (held(Key::A) ? 1.0f : 0.0f);
                const float z
                    = (held(Key::S) || held(Key::Down) ? 1.0f : 0.0f) - (held(Key::W) || held(Key::Up) ? 1.0f : 0.0f);
                client->character()->setMoveVector({x, 0.0f, z}, true);
                client->character()->setJump(held(Key::Space));
            }
            feed.frame(dt);
            lap(times.scripts, "scripts");
            if (LiveClient* client = feed.client(); client != consoleClient) {
                consoleClient = client;
                console.run = nullptr;
                if (client) {
                    client->setOutputSink([&console](lunar::MessageType type, std::string_view text) {
                        console.add(consoleKind(type), text);
                    });
                    // The window's focus, its clipboard and its frame cap, for
                    // the console's isrbxactive, setclipboard and setfpscap.
                    lunar::ScriptContext& scripts = client->session()->client();
                    scripts.setActive([&platformWindow] { return platformWindow.focused(); });
                    scripts.setClipboard([&platformWindow](std::string_view text) {
                        platformWindow.setClipboardText(std::string(text));
                    });
                    scripts.setFrameCap([&window](int fps) { window.setFrameCap(fps); });
                    // keypress, mouse1press, mousemoverel: input of the window's own.
                    scripts.setInput([&platformWindow](const lunar::ScriptContext::HostInput& input) {
                        using Kind = lunar::ScriptContext::HostInput::Kind;
                        namespace pf = lunar::platform;
                        const pf::Vec2d at = platformWindow.input().mousePosition();
                        switch (input.kind) {
                        case Kind::Key:
                            if (const auto key = platformKey(input.key))
                                platformWindow.inject(pf::KeyEvent{.key = *key, .pressed = input.pressed});
                            break;
                        case Kind::Button:
                            platformWindow.inject(
                                pf::MouseButtonEvent{.button = input.button == 2   ? pf::MouseButton::Right
                                                               : input.button == 3 ? pf::MouseButton::Middle
                                                                                   : pf::MouseButton::Left,
                                                     .pressed = input.pressed,
                                                     .x = at.x,
                                                     .y = at.y});
                            break;
                        case Kind::MoveBy:
                            platformWindow.inject(pf::RelativeMotionEvent{
                                .dx = input.x, .dy = input.y, .rawDx = input.x, .rawDy = input.y});
                            break;
                        case Kind::MoveTo:
                            platformWindow.inject(pf::MouseMoveEvent{.x = input.x, .y = input.y});
                            break;
                        case Kind::Scroll:
                            platformWindow.inject(pf::ScrollEvent{.dy = input.y});
                            break;
                        }
                    });
                    console.run = [client](const std::string& code) -> std::string {
                        if (!client->started())
                            return "The client's scripts haven't started yet.";
                        // Full access, as on a local play; a --!server line has
                        // no server here, and runConsole says so.
                        auto ran = client->session()->client().runConsole(code);
                        return ran ? std::string() : ran.error().message;
                    };
                }
            }
            // --bot: its script runs in the console once the client's scripts
            // have started, and again in the client after a teleport.
            if (LiveClient* client = feed.client();
                !options.bot.empty() && client && client->started() && client != botClient) {
                botClient = client;
                if (auto source = lunar::readFile(options.bot); !source)
                    std::println(stderr, "bot: {}", source.error());
                else if (auto ran = client->session()->client().runConsole(*source); !ran)
                    std::println(stderr, "bot: {}", ran.error().message);
                else
                    std::println("bot: running {}", options.bot.string());
            }
            // Code that came in over --api runs where typed code would.
            if (api)
                api->pump();
            if (const auto end = feed.ended(); end && !toldEnd) {
                toldEnd = true;
                endedAt = window.elapsed();
                std::println("play: {:.1f} s: disconnected: {}", window.elapsed(), *end);
                ui.chat().postSystem("Disconnected: " + *end, lunar::Color3::fromRGB(255, 80, 80));
                titleStatus = "disconnected";
            }
            // A window a tool plays (--bot, --control) leaves once its game is
            // over, as --headless does: nobody is at it to read why, and a
            // join turned away (an age check, say) would hold it open for
            // good. A player's window keeps saying why until they close it.
            if (endedAt && window.controlled() && window.elapsed() - *endedAt > 2.0)
                platformWindow.setShouldClose(true);

            if (!scene && serverAt && (replica->joinDataDone || window.elapsed() - *serverAt > 20.0)) {
                scene = std::make_unique<PlaceScene>(*game);
                scene->setTerrainDistance(window.graphics().terrainDistance);
                scene->wrapTextures(
                    [&](render::TextureProvider next) { return wardrobe.clothing().provider(std::move(next)); });
                auto touch = [s = scene.get()](lunar::Instance* instance) { s->touch(*instance); };
                added = workspace.descendantAdded.connect(touch);
                removing = workspace.descendantRemoving.connect(touch);
                presenter.renderer().setMaterialTextures(scene->materialTextures());
                presenter.renderer().setTerrainMaterials(scene->terrainMaterials());
                ui.viewports()->setAssets(scene->viewportAssets());
                presenter.renderer().addEffects(&scene->ribbons());
                presenter.renderer().addEffects(&scene->adornments());
                presenter.renderer().setLights(&scene->lights());
                particles = std::make_unique<render::ParticleSystem>(workspace, scene->textures());
                particles->setSettings({.density = window.graphics().particleDensity});
                presenter.renderer().addEffects(particles.get());
                lighting = scene->lighting();
                camera = std::make_unique<LiveCamera>(workspace);
                // Most of a game's audio is shared with its place only: asked for as the place.
                auto soundCache = std::make_shared<lunar::assets::AssetCache>();
                soundCache->setPlaceId(static_cast<std::uint64_t>(game->placeId()));
                sounds = std::make_unique<lunar::audio::SoundSystem>(*game, std::move(soundCache), !options.mute);
                if (options.minimap)
                    map = std::make_unique<MapScene>(*scene);
                std::println("play: {:.1f} s: the scene, {} parts and {} draw items{}", window.elapsed(),
                             scene->partItemCount(), scene->items().size(), scene->terrain() ? ", with terrain" : "");
                if (!toldEnd)
                    titleStatus.clear();
                sceneAt = window.elapsed();
            }

            // The loading screen: what the game is, how far the join has got,
            // and down once the scene and our character are there. The game's
            // details are asked for once its place is known: the one we were
            // asked to join, else a private server's (or a teleport's) once
            // the join has it.
            if (!detailsAsked) {
                const auto placeId = game->placeId() > 0 ? static_cast<std::uint64_t>(game->placeId()) : askedPlace;
                if (placeId > 0) {
                    details = std::make_unique<DetailsLookup>(placeId);
                    detailsAsked = true;
                }
            }
            if (details)
                if (auto found = details->take()) {
                    if (*found) {
                        loading.setGame((*found)->name, (*found)->creator, (*found)->icon());
                        ui.core().setTitle((*found)->name);
                        gameTitle = (*found)->name;
                    } else {
                        std::println(stderr, "play: what the game is: {}", found->error());
                    }
                    details.reset();
                }
            if (loading.blocking() && !feed.teleporting()) {
                LiveClient* client = feed.client();
                joined = joined || client != nullptr;
                auto* players = dynamic_cast<lunar::Players*>(game->findFirstChildOfClass("Players"));
                const lunar::Player* self = players ? players->localPlayer() : nullptr;
                if (client && client->session()->client().defaultLoadingScreenRemoved()) {
                    loading.remove();
                    std::println("play: {:.1f} s: the game took the loading screen down", window.elapsed());
                } else if (const auto end = feed.ended()) {
                    loading.setStatus((joined ? "Disconnected: " : "Couldn't join: ") + *end, true);
                    joinFailed = joinFailed || !joined;
                } else if (const auto joining = feed.joining()) {
                    loading.setStatus(*joining);
                } else if (!sceneAt) {
                    loading.setStatus(replica->instanceCount() == 0 ? "Joining server" : "Loading the game");
                } else if (client && ((self && self->character()) || window.elapsed() - *sceneAt >= characterWait)) {
                    loading.dismiss();
                    std::println("play: {:.1f} s: loaded", window.elapsed());
                } else {
                    loading.setStatus("Spawning");
                }
            }

            // The CameraModule, after the scripts and animations moved things.
            const lunar::platform::InputState& input = platformWindow.input();
            auto* players = dynamic_cast<lunar::Players*>(game->findFirstChildOfClass("Players"));
            lunar::Player* local = players ? players->localPlayer() : nullptr;
            if (camera) {
                const bool active = input.focused() && !window.uiHasKeys();
                camera->setMouseLockAllowed(window.shiftLock());
                camera->update(dt,
                               cameraInput(input, active, platformWindow.pointerLocked() || window.controlled(),
                                           !ui.wheelCaptured(), window.aspect(), window.cameraSensitivity()),
                               local);
                platformWindow.setPointerLocked(input.focused() && !ui.menuOpen() && !ui.mapOpen()
                                                && !ui.loadingScreenUp() && camera->wantsPointerLock(*game));
                if (LiveClient* client = feed.client(); client && client->character()) {
                    const bool scripts = client->session()->client().cameraRelativeRotation();
                    const std::string why = camera->facingReason(scripts);
                    if (why != facingWhy) {
                        std::println("play: {:.1f} s: our character {}", window.elapsed(),
                                     why.empty() ? "faces the way it walks" : "faces the camera (" + why + ")");
                        facingWhy = why;
                    }
                    client->character()->setFacing(camera->facing(scripts));
                }
            }
            // The beat ring at our character's feet, while the Music tab has it on.
            {
                lunar::ui::MenuSettings& settings = ui.core().menuSettings;
                music.setEnabled(settings.beatRing);
                std::optional<lunar::Vector3> feet;
                lunar::Model* body = local ? local->character() : nullptr;
                auto* humanoid
                    = body ? dynamic_cast<lunar::Humanoid*>(body->findFirstChildOfClass("Humanoid")) : nullptr;
                if (const lunar::BasePart* root = humanoid ? humanoid->rootPart() : nullptr) {
                    // R6 legs hang 2 studs below the root's bottom at HipHeight 0.
                    const float legs
                        = humanoid->rigType() == lunar::Enum::HumanoidRigType::R6 ? 2.0f : humanoid->hipHeight();
                    feet = root->position() - lunar::Vector3{0.0f, root->size().y / 2 + legs, 0.0f};
                }
                music.update(feet, dt);
                settings.musicStatus = music.status();
            }
            lap(times.camera, "camera");

            render::Camera view;
            std::span<const render::DrawItem> items;
            if (scene) {
                wardrobe.update([&](lunar::Model& model) {
                    for (const lunar::InstancePtr& d : model.descendants())
                        scene->touch(*d);
                });
                const bool rebuilt = scene->refresh();
                // More of the sky may have loaded, or the time of day moved on.
                if (rebuilt || gameTime - lightingAt >= 1.0) {
                    lighting = scene->lighting();
                    lightingAt = gameTime;
                }
                scene->followParts();
                if (map)
                    map->update(gameTime, ui.map());
                view.cframe = camera->camera().cframe();
                view.fieldOfView = camera->camera().fieldOfView();
                scene->updateTerrain(view.cframe.position);
                sounds->setMasterVolume(window.volume());
                sounds->update(view.cframe);
                items = scene->items();
                particles->step(std::min(dt, 0.1f), view.cframe.position);
            }
            lap(times.scene, "scene");
            // The game's mouse icon: at the pointer, or the screen's centre while it's locked.
            if (LiveClient* client = feed.client(); client && client->session()) {
                const lunar::ScriptContext& scripts = client->session()->client();
                const bool locked = platformWindow.pointerLocked() || window.controlled();
                ui.setCursor(scripts.mouseIconShown() ? scripts.mouseIcon() : std::string(),
                             locked ? window.size() * 0.5f : mouse * static_cast<float>(platformWindow.scale()));
            }
            const double guiFrom = times.ui;
            window.updateUi(dt);
            lap(times.ui, "gui");
            // LUNAR_PROFILE: a frame whose gui took long, and where.
            if (static const int slowGui = scriptProfileLevel(); slowGui > 0 && times.ui - guiFrom > 10.0) {
                const lunar::ui::GameUi::UpdateTimes& g = ui.lastUpdateTimes();
                std::println("play: slow gui frame {:.1f} ms: styles {:.1f}, core {:.1f}, layout {:.1f}, others {:.1f}, "
                             "draw lists {:.1f}, submit {:.1f}, {} glyphs rasterized in {:.1f} ms (slowest {:.1f} ms: "
                             "{:.0f} px em, stroke {}, bitmap {}x{})",
                             times.ui - guiFrom, g.styles, g.core, g.layout, g.others, g.draw, g.submit,
                             g.glyphsRasterized, g.raster.milliseconds, g.raster.worstMilliseconds,
                             g.raster.worstPixelsPerUnit, g.raster.worstStroke, g.raster.worstWidth,
                             g.raster.worstHeight);
            }
            // The network again before the frame waits for its turn on screen
            // (a teleport's join is the first pump's).
            if (!feed.teleporting())
                feed.pump();
            lap(times.network, "network");
            gameTime += static_cast<double>(dt);
            lighting.time = gameTime; // animates the water
            keepAlive.begin();
            if (!window.hidden()) {
                platformWindow.requestFrameCallback();
                presenter.drawFrame(view, items, lighting);
            }
            lap(times.draw, "draw");

            if (const double ms = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
                ms > times.worst) {
                times.worst = ms;
                times.worstIn = longest.second;
            }
            ++times.frames;
            const auto fps = window.endFrame(); // a screenshot takes a while to write
            keepAlive.end();
            if (fps)
                platformWindow.setTitle(std::format("Lunarstone - {} - {} fps{}", gameTitle, *fps,
                                                    titleStatus.empty() ? "" : " - " + titleStatus));
            if (Clock::now() - lastLog >= std::chrono::seconds(5)) {
                const double seconds = std::chrono::duration<double>(Clock::now() - lastLog).count();
                const double n = std::max(times.frames, 1);
                std::println(
                    "play: {:.1f} s: {:.0f} fps ({:.2f} ms a frame, worst {:.1f} mostly {}); ms a frame: " "network" " "
                                                                                                                     "{"
                                                                                                                     ":"
                                                                                                                     "."
                                                                                                                     "2"
                                                                                                                     "f"
                                                                                                                     "}" ", " "scripts" " {:.2f}" ", " "camera " "{:.2f}," " scene " "{:.2f}," " gui " "{:.2f}," " draw " "{:.2f};" " " "{} " "ins" "tan" "ces" ", " "{} " "par" "ts " "dra" "wn," " {}" " ch" "ara" "cte" "rs " "dre" "sse" "d",
                    window.elapsed(), times.frames / seconds, seconds * 1000.0 / n, times.worst, times.worstIn,
                    times.network / n, times.scripts / n, times.camera / n, times.scene / n, times.ui / n,
                    times.draw / n, replica->instanceCount(), scene ? scene->partItemCount() : 0,
                    wardrobe.dressedCount());
                if (LiveClient* client = feed.client(); client && client->character())
                    std::println("play: self: {}", client->character()->describe());
                // LUNAR_PROFILE: drawFrame's time by stage.
                if (static const int drawProfiling = scriptProfileLevel(); drawProfiling > 0) {
                    const render::Presenter::Timings t = presenter.takeTimings();
                    const double frames = std::max(t.frames, 1);
                    std::println("play: draw split, ms a frame: fence wait {:.2f}, next image {:.2f}, record {:.2f}, "
                                 "submit {:.2f}, present {:.2f}",
                                 t.fenceWait / frames, t.acquire / frames, t.record / frames, t.submit / frames,
                                 t.present / frames);
                    static std::uint64_t rasterizedSoFar = 0;
                    const std::uint64_t rasterizedNow = ui.glyphsRasterized();
                    std::println("play: glyph bitmaps rasterized in these seconds: {} ({:.1f} a frame)",
                                 rasterizedNow - rasterizedSoFar,
                                 static_cast<double>(rasterizedNow - rasterizedSoFar) / frames);
                    rasterizedSoFar = rasterizedNow;
                    const render::FrameStats& r = presenter.renderer().stats();
                    std::println("play: last frame: {} items, {} visible, {} too far, {} draw calls, {} shadow casters, "
                                 "{:.2f} M triangles, uploaded {} meshes and {} textures ({} textures waiting), "
                                 "lod {}/{}/{}/{}, {} effect batches, {} local lights",
                                 r.items, r.visible, r.tooFar, r.drawCalls, r.shadowCasters,
                                 static_cast<double>(r.triangles) / 1e6, r.meshesUploaded, r.texturesUploaded,
                                 r.texturesPending, r.levels[0], r.levels[1], r.levels[2], r.levels[3],
                                 r.effectBatches, r.localLights);
                    const auto& p = r.phases;
                    std::println("play: record phases of the last frame, ms: setup {:.2f}, cull and sort {:.2f}, "
                                 "lights and uploads {:.2f}, sun shadow {:.2f}, local shadows {:.2f}, solid {:.2f}, "
                                 "the rest {:.2f}",
                                 p.setup, p.build, p.prepare, p.sun, p.local, p.solid, p.rest);
                    std::println("play:   cull and sort in steps, ms: cull {:.2f}, effects {:.2f}, textures {:.2f}, "
                                 "sort blended {:.2f}, order effects {:.2f}, sort solid {:.2f}, write instances {:.2f}",
                                 p.steps[0], p.steps[1], p.steps[2], p.steps[3], p.steps[4], p.steps[5], p.steps[6]);
                }
                // LUNAR_PROFILE: where the scripts' time went these five
                // seconds, by frame phase and (2) by script.
                if (static const int profiling = scriptProfileLevel(); profiling > 0)
                    if (LiveClient* client = feed.client(); client && client->session()) {
                        lunar::ClientSession* session = client->session();
                        if (session->profiling())
                            printScriptProfile(session->takeProfile(), profiling);
                        session->setProfiling(true);
                    }
                if (sounds) {
                    const auto s = sounds->stats();
                    std::println("play: sound: {} playing of {}, {} loaded ({:.0f} MB), {} failed", s.playing, s.sounds,
                                 s.clips, static_cast<double>(s.bytes) / 1e6, s.failed);
                }
                times = {};
                lastLog = Clock::now();
            }
        }
        added.disconnect();
        removing.disconnect();
        // The renderer lets go of this game's scene before it goes.
        window.onGraphics = nullptr;
        if (scene) {
            presenter.renderer().removeEffects(&scene->ribbons());
            presenter.renderer().removeEffects(&scene->adornments());
            presenter.renderer().setLights(nullptr);
            presenter.renderer().setMaterialTextures({});
            presenter.renderer().setTerrainMaterials({});
            ui.viewports()->setAssets({});
        }
        if (particles)
            presenter.renderer().removeEffects(particles.get());
        console.run = nullptr; // the client goes with this game
        if (api)
            api->attach(nullptr); // and the console with the window's GUI, on a teleport
        if (!teleported)
            break;
        std::println("play: {:.1f} s: teleported; the window goes on with the new server's game", window.elapsed());
        askedPlace = 0; // the next game's place is its join's
        keepAlive.begin();
        auto rebound = window.rebind(*feed.replica()->game());
        keepAlive.end();
        if (!rebound) {
            std::println(stderr, "play: {}", rebound.error());
            feed.leave();
            return 1;
        }
        // The teleport has happened: whatever the console queued for it runs
        // now, in the console, as it was queued (ScriptContext::runTeleportCode).
        // The new client's context gets it below, when its scripts start.
        if (LiveClient* client = feed.client(); client && client->session())
            client->session()->client().runTeleportCode();
    } // one pass a game
    std::println("play: {:.1f} s, the window closed", window.elapsed());
    feed.leave();
    if (platformWindow.error()) {
        std::println(stderr, "{}", *platformWindow.error());
        return 1;
    }
    return joinFailed ? 1 : 0;
}

// --headless: the game without a window, a GPU or sound, for a bot. The
// client's scripts, physics and network run as in play(); the GUI is laid out
// (for the scripts' AbsolutePosition and the bot's clicks) but never drawn,
// and the place is never turned into a scene. Sounds load and play as they
// do there, mixed in step with the clock and thrown away, so a script sees
// one's TimePosition go on and its Loaded and Ended come. Input comes only from the
// console's keypress, mouse1click and the like. Images count as loaded at
// once and never download. Leaves when the game ends (or --seconds).
int playHeadless(GameFeed& feed, const NetPlayOptions& options, ConsoleApi* api)
{
    namespace pf = lunar::platform;
    const lunar::Vector2 viewport{1920.0f, 1080.0f};
    const double fps = options.fps > 0 ? options.fps : 30.0;
    const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / fps));
    const auto started = Clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - started).count(); };

    // Every image "loaded" at once, as a pixel: nothing downloads or decodes.
    auto pixel = std::make_shared<lunar::Image>();
    pixel->width = pixel->height = 1;
    pixel->rgba = {255, 255, 255, 255};
    lunar::ui::GameUi::Options uiOptions;
    uiOptions.contentDir = lunar::assets::AssetConfig::fromEnvironment().contentDir;
    uiOptions.images = [pixel](std::string_view) -> std::shared_ptr<const lunar::Image> { return pixel; };
    uiOptions.map.minimap = false;
    uiOptions.map.fullMapKey.reset();

    std::unique_ptr<lunar::ui::GameUi> ui;
    bool leave = false;
    const auto makeUi = [&](lunar::DataModel& game) {
        ui = lunar::ui::GameUi::createHeadless(game, uiOptions);
        ui->onLeave = [&leave] { leave = true; };
        ui->update(0.0, viewport, 1.0f);
    };
    makeUi(*feed.replica()->game());
    // What GameWindow::bindGui gives a client, over the headless GUI.
    feed.configureClient = [&ui, viewport](lunar::ClientSession& session) {
        session.setViewportSize(viewport);
        lunar::ScriptContext& context = session.client();
        lunar::ui::GameUi* gui = ui.get();
        gui->gui().setDeferredInput(true);
        gui->update(0.0, viewport, 1.0f);
        context.setTextMeasure([gui](std::string_view text, const lunar::Font& font, float size, float width,
                                     bool rich) { return gui->measureText(text, font, size, width, rich); });
        context.setImageLoadedChecker([gui](std::string_view id) { return gui->gui().imageLoaded(id); });
        context.setGuiHitTest([gui](float x, float y) { return gui->guiObjectsAt(x, y); });
        context.setGuiFocus(
            [gui](lunar::TextBox* box, bool capture, bool entered) { gui->focusTextBox(box, capture, entered); });
        context.setGuiLayoutQuery([gui](lunar::Instance* object) { gui->ensureGuiGeometry(object); });
        context.setGameInvitePromptHandler(gui->gameInviteHandler());
        gui->gui().onPageStep = context.guiPageStepper();
    };
    const ConsoleApi::Detach detachApi(api);
    std::println("play: headless, {:.0f} frames a second at most, a {}x{} screen", fps, viewport.x, viewport.y);

    bool joinFailed = false;
    // The game before a teleport, to say whether it went (or what keeps it).
    std::weak_ptr<lunar::DataModel> lastGame;
    double lastGameAt = 0.0;
    for (;;) {
        // The last pass's scene, chat and GUI are gone: so can the game they showed.
        feed.releasePrevious();
        const std::uint64_t generation = feed.generation();
        const std::shared_ptr<lunar::net::Replica> replica = feed.replica();
        const std::shared_ptr<lunar::DataModel> game = replica->game();
        lunar::Workspace& workspace = *game->workspace();
        bool teleported = false;
        std::optional<double> serverAt, readyAt, endedAt;
        bool joined = false;

        std::unique_ptr<LiveCamera> camera;
        std::unique_ptr<lunar::audio::SoundSystem> sounds;
        double soundsAt = 0.0, mixed = 0.0; // when they started, and how much has been mixed since
        std::vector<float> block;
        std::unique_ptr<TextChatSource> chat;
        // The console's input: events of a window that isn't there.
        pf::InputState input;
        input.apply(pf::FocusEvent{.focused = true});
        std::vector<pf::Event> queued;
        lunar::Vector2 mouse;
        bool pointerLocked = false; // as the camera last wanted it
        lunar::ui::LoadingScreen& loading = ui->loading();
        loading.show();
        loading.setStatus("Joining server");
        lunar::ui::DevConsole& console = ui->core().console();
        console.run = nullptr;
        LiveClient* consoleClient = nullptr;
        LiveClient* botClient = nullptr;
        if (api)
            api->attach(&console);
        FrameTimes times;
        auto lastLog = Clock::now(), last = Clock::now(), nextFrame = Clock::now();
        auto lastTrim = Clock::now();
        // The GUI is laid out ten times a second, not every frame: nobody
        // sees it. What reads it lays it out first if anything changed: a
        // script's AbsolutePosition, the bot's clicks (below), a hit test.
        constexpr auto uiPeriod = std::chrono::milliseconds(100);
        auto nextUi = Clock::now();
        double uiDt = 0.0;

        while (!leave && !(options.seconds > 0 && elapsed() >= options.seconds)) {
            std::this_thread::sleep_until(nextFrame);
            nextFrame = std::max(nextFrame + period, Clock::now());
            const auto frameStart = Clock::now();
            const float dt = std::min(std::chrono::duration<float>(frameStart - last).count(), 0.1f);
            last = frameStart;

            input.beginFrame();
            if (!queued.empty())
                ui->ensureGuiGeometry(nullptr);
            for (pf::Event& event : std::exchange(queued, {})) {
                input.apply(event);
                const bool usedByUi = ui->handle(event);
                LiveClient* client = feed.client();
                if (!client)
                    continue;
                const bool keys = ui->keyboardCaptured() || ui->menuOpen() || ui->loadingScreenUp();
                forwardInput(client->session()->input(), event, mouse, pointerLocked,
                             ui->menuOpen() || ui->loadingScreenUp(), usedByUi || keys, ui->gui().takeInputDispatch());
                if (usedByUi)
                    continue;
                if (const auto* button = std::get_if<pf::MouseButtonEvent>(&event);
                    button && button->button == pf::MouseButton::Left)
                    client->session()->toolButton(button->pressed);
                else if (const auto* key = std::get_if<pf::KeyEvent>(&event);
                         key && key->pressed && key->key == pf::Key::Backspace)
                    client->session()->dropTools();
            }

            if (!feed.teleporting() || loading.blocking())
                feed.pump();
            if (feed.generation() != generation) {
                teleported = true;
                break;
            }
            if (feed.teleporting()) {
                loading.show();
                loading.setStatus(feed.joining().value_or("Teleporting"));
            }
            if (!serverAt && !feed.joining())
                serverAt = elapsed();
            auto mark = Clock::now();
            times.network += std::chrono::duration<double, std::milli>(mark - frameStart).count();

            if (LiveClient* client = feed.client()) {
                client->session()->setViewportSize(viewport);
                if (!chat) {
                    chat = std::make_unique<TextChatSource>(client->session()->client());
                    chat->observe = [](const lunar::ChatLine& line) { std::println("{}", describeChatLine(line)); };
                    ui->setChatSource(chat.get());
                }
                if (client->character()) {
                    const bool controls = !ui->keyboardCaptured() && !ui->menuOpen() && !ui->loadingScreenUp();
                    auto held = [&](pf::Key key) { return controls && input.isDown(key); };
                    using pf::Key;
                    const float x = (held(Key::D) ? 1.0f : 0.0f) - (held(Key::A) ? 1.0f : 0.0f);
                    const float z = (held(Key::S) || held(Key::Down) ? 1.0f : 0.0f)
                                    - (held(Key::W) || held(Key::Up) ? 1.0f : 0.0f);
                    client->character()->setMoveVector({x, 0.0f, z}, true);
                    client->character()->setJump(held(Key::Space));
                }
            }
            feed.frame(dt);
            if (LiveClient* client = feed.client(); client != consoleClient) {
                consoleClient = client;
                console.run = nullptr;
                if (client) {
                    client->setOutputSink([&console](lunar::MessageType type, std::string_view text) {
                        console.add(consoleKind(type), text);
                    });
                    lunar::ScriptContext& scripts = client->session()->client();
                    scripts.setActive([] { return true; });
                    scripts.setClipboard([](std::string_view) {});
                    scripts.setFrameCap([](int) {});
                    scripts.setInput([&queued, &input](const lunar::ScriptContext::HostInput& in) {
                        using Kind = lunar::ScriptContext::HostInput::Kind;
                        const pf::Vec2d at = input.mousePosition();
                        switch (in.kind) {
                        case Kind::Key:
                            if (const auto key = platformKey(in.key))
                                queued.push_back(pf::KeyEvent{.key = *key, .pressed = in.pressed});
                            break;
                        case Kind::Button:
                            queued.push_back(pf::MouseButtonEvent{.button = in.button == 2   ? pf::MouseButton::Right
                                                                            : in.button == 3 ? pf::MouseButton::Middle
                                                                                             : pf::MouseButton::Left,
                                                                  .pressed = in.pressed,
                                                                  .x = at.x,
                                                                  .y = at.y});
                            break;
                        case Kind::MoveBy:
                            queued.push_back(
                                pf::RelativeMotionEvent{.dx = in.x, .dy = in.y, .rawDx = in.x, .rawDy = in.y});
                            break;
                        case Kind::MoveTo:
                            // Applied at once: a click right after the move lands where it went.
                            input.apply(pf::MouseMoveEvent{.x = in.x, .y = in.y});
                            queued.push_back(pf::MouseMoveEvent{.x = in.x, .y = in.y});
                            break;
                        case Kind::Scroll:
                            queued.push_back(pf::ScrollEvent{.dy = in.y});
                            break;
                        }
                    });
                    console.run = [client](const std::string& code) -> std::string {
                        if (!client->started())
                            return "The client's scripts haven't started yet.";
                        auto ran = client->session()->client().runConsole(code);
                        return ran ? std::string() : ran.error().message;
                    };
                }
            }
            if (LiveClient* client = feed.client();
                !options.bot.empty() && client && client->started() && client != botClient) {
                botClient = client;
                if (auto source = lunar::readFile(options.bot); !source)
                    std::println(stderr, "bot: {}", source.error());
                else if (auto ran = client->session()->client().runConsole(*source); !ran)
                    std::println(stderr, "bot: {}", ran.error().message);
                else
                    std::println("bot: running {}", options.bot.string());
            }
            if (api)
                api->pump();
            times.scripts += std::chrono::duration<double, std::milli>(Clock::now() - mark).count();

            if (const auto end = feed.ended(); end && !endedAt) {
                endedAt = elapsed();
                std::println("play: {:.1f} s: disconnected: {}", elapsed(), *end);
                joinFailed = joinFailed || !joined;
            }
            if (endedAt && elapsed() - *endedAt > 2.0)
                break;
            // Where the windowed client builds its scene, the game counts as there.
            if (!readyAt && serverAt && (replica->joinDataDone || elapsed() - *serverAt > 20.0)) {
                readyAt = elapsed();
                camera = std::make_unique<LiveCamera>(workspace);
                // As play() has them, asked for as the place, with no output.
                auto soundCache = std::make_shared<lunar::assets::AssetCache>();
                soundCache->setPlaceId(static_cast<std::uint64_t>(game->placeId()));
                sounds = std::make_unique<lunar::audio::SoundSystem>(*game, std::move(soundCache), false);
                soundsAt = elapsed();
                std::println("play: {:.1f} s: the game is in, {} instances", elapsed(), replica->instanceCount());
            }
            if (loading.blocking() && !feed.teleporting()) {
                LiveClient* client = feed.client();
                joined = joined || client != nullptr;
                auto* players = dynamic_cast<lunar::Players*>(game->findFirstChildOfClass("Players"));
                const lunar::Player* self = players ? players->localPlayer() : nullptr;
                if (client && client->session()->client().defaultLoadingScreenRemoved())
                    loading.remove();
                else if (const auto joining = feed.joining())
                    loading.setStatus(*joining);
                else if (readyAt && client && ((self && self->character()) || elapsed() - *readyAt >= characterWait)) {
                    loading.dismiss();
                    std::println("play: {:.1f} s: loaded", elapsed());
                }
            }
            if (camera) {
                auto* players = dynamic_cast<lunar::Players*>(game->findFirstChildOfClass("Players"));
                lunar::Player* local = players ? players->localPlayer() : nullptr;
                // The bot's input turns it as a player's would: Shift with
                // --shift-lock locks the mouse, and locked, its motion turns
                // the camera (a window's pointer lock, with no window).
                camera->setMouseLockAllowed(options.shiftLock);
                pointerLocked = camera->wantsPointerLock(*game);
                camera->update(dt, cameraInput(input, true, pointerLocked, true, viewport.x / viewport.y, 1.0f), local);
                if (LiveClient* client = feed.client(); client && client->character())
                    client->character()->setFacing(
                        camera->facing(client->session()->client().cameraRelativeRotation()));
            }
            if (sounds) {
                const lunar::Camera* view = workspace.currentCamera();
                sounds->update(view ? view->cframe() : lunar::CFrame());
                // No device pulls the mix: it's made here, 10 ms at a time.
                for (const double now = elapsed() - soundsAt; mixed + 0.01 <= now; mixed += 0.01) {
                    block.assign(std::size_t{lunar::audio::Mixer::rate / 100} * 2, 0.0f);
                    sounds->mixer().mix(block);
                }
            }
            uiDt += dt;
            if (frameStart >= nextUi) {
                mark = Clock::now();
                ui->update(uiDt, viewport, 1.0f);
                times.ui += std::chrono::duration<double, std::milli>(Clock::now() - mark).count();
                uiDt = 0.0;
                nextUi = frameStart + uiPeriod;
            }
            if (!feed.teleporting())
                feed.pump();
            ++times.frames;

            if (lastGameAt > 0.0 && elapsed() - lastGameAt > 10.0) {
                std::println("play: the game before the teleport {}",
                             lastGame.expired() ? "is gone"
                                                : std::format("is still held ({} owners)", lastGame.use_count()));
                lastGameAt = 0.0;
            }
            if (Clock::now() - lastLog >= std::chrono::seconds(5)) {
                const double seconds = std::chrono::duration<double>(Clock::now() - lastLog).count();
                const double n = std::max(times.frames, 1);
                std::println(
                    "play: {:.1f} s: {:.0f} fps; ms a frame: network {:.2f}, scripts {:.2f}, gui {:.2f}; {} " "instance"
                                                                                                              "s",
                    elapsed(), times.frames / seconds, times.network / n, times.scripts / n, times.ui / n,
                    replica->instanceCount());
                times = {};
                lastLog = Clock::now();
            }
            // Freed memory goes back to the system, not glibc's arenas.
            if (Clock::now() - lastTrim >= std::chrono::seconds(30)) {
                malloc_trim(0);
                lastTrim = Clock::now();
            }
        }
        console.run = nullptr;
        if (api)
            api->attach(nullptr);
        if (!teleported)
            break;
        std::println("play: {:.1f} s: teleported; going on with the new server's game", elapsed());
        lastGame = game;
        lastGameAt = elapsed();
        ui.reset();
        makeUi(*feed.replica()->game());
        if (LiveClient* client = feed.client(); client && client->session())
            client->session()->client().runTeleportCode();
    }
    std::println("play: {:.1f} s, headless, done", elapsed());
    feed.leave();
    return joinFailed ? 1 : 0;
}

} // namespace

int runNetPlay(std::span<const std::string_view> args)
{
    NetPlayOptions options;
    std::size_t i = 1;
    if (args.size() >= 3 && args[1] == "--replay") {
        options.replay = args[2];
        i = 3;
    } else if (args.size() >= 3 && args[1] == "--private") {
        options.join.privateLink = args[2]; // a private server's link: its place, and joined by its code
        options.placeId = 0;
        i = 3;
    } else if (args.size() >= 2) {
        try {
            options.placeId = std::stoull(std::string(args[1]));
        } catch (...) {
            return -1;
        }
        i = 2;
    } else {
        return -1;
    }
    std::string graphicsError;
    for (; i < args.size(); ++i) {
        const bool hasValue = i + 1 < args.size();
        if (parseGraphicsFlag(args, i, options.graphics, graphicsError)) {
            options.graphicsFromFlags = true;
            if (!graphicsError.empty()) {
                std::println(stderr, "play: {}", graphicsError);
                return 1;
            }
        } else if (args[i] == "--seconds" && hasValue) {
            options.seconds = std::stod(std::string(args[++i])); // leaves after that long
        } else if (args[i] == "--screenshot" && hasValue) {
            auto shot = parseScreenshot(args[++i]); // a picture of the window, "seconds:out.png"
            if (!shot)
                return -1;
            options.screenshots.push_back(std::move(*shot));
        } else if (args[i] == "--minimap") {
            options.minimap = true; // Lunarstone's minimap, and M for the full map
        } else if (args[i] == "--headless") {
            options.headless = true; // no window, GPU or sound: scripts, GUI layout and the network only
        } else if (args[i] == "--mute") {
            options.mute = true; // no sound output
        } else if (args[i] == "--screenshot-on" && hasValue) {
            options.screenshotRequest = args[++i]; // a file that asks for a picture when it appears
        } else if (args[i] == "--bot" && hasValue) {
            options.bot = args[++i]; // Luau that plays the window from the console (keypress, mouse1click, ...)
        } else if (args[i] == "--shift-lock") {
            options.shiftLock = true; // the Shift Lock Switch on, so Shift locks the mouse
        } else if (args[i] == "--control" && hasValue) {
            options.control = args[++i]; // a file of input lines (Control.hpp), on top of the keyboard and mouse
        } else if (args[i] == "--fps" && hasValue) {
            const std::string_view value = args[++i]; // frames a second at most, or vsync (the default)
            options.fps = value == "vsync" ? 0.0 : std::stod(std::string(value));
            if (options.fps < 0)
                return -1;
        } else if (args[i] == "--attempts" && hasValue) {
            options.join.attempts = std::stoi(std::string(args[++i]));
        } else if (args[i] == "--job" && hasValue) {
            options.join.gameId = args[++i]; // that server of the place (its JobId), as joining a friend does
        } else if (args[i] == "--platform" && hasValue) {
            options.join.platform = args[++i]; // linux or mac: what the join says we run on
        } else if (args[i] == "--script-platform" && hasValue) {
            options.scriptPlatform = args[++i]; // an Enum.Platform name for UserInputService:GetPlatform()
        } else if (args[i] == "--out" && hasValue) {
            options.join.out = args[++i]; // a capture of the session, for --net-play --replay
        } else if (std::string apiError; parseApiFlag(args, i, options.api, apiError)) {
            if (!apiError.empty()) {
                std::println(stderr, "play: {}", apiError);
                return 1;
            }
        } else {
            return -1;
        }
    }

    // --api listens before the join: a port that's taken stops it here.
    std::unique_ptr<ConsoleApi> api;
    if (options.api) {
        auto started = startConsoleApi(*options.api);
        if (!started) {
            std::println(stderr, "play: {}", started.error());
            return 1;
        }
        api = std::move(*started);
    }

    std::expected<std::unique_ptr<GameFeed>, std::string> feed;
    std::string title;
    if (!options.replay.empty()) {
        feed = replayLive(options.replay, options.scriptPlatform);
        title = "replay of " + std::filesystem::path(options.replay).filename().string();
    } else {
        options.join.placeId = *options.placeId;
        options.join.scriptPlatform = options.scriptPlatform;
        feed = joinLive(options.join);
        title = options.join.privateLink.empty() ? std::format("place {}", *options.placeId)
                                                 : std::string("a private server");
    }
    if (!feed) {
        std::println(stderr, "play: {}", feed.error());
        return 1;
    }
    if (options.headless)
        return playHeadless(**feed, options, api.get());
    return play(**feed, options, title, api.get());
}
