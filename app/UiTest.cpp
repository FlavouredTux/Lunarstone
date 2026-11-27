#include "UiTest.hpp"

#include "Look.hpp"

#include "MapScene.hpp"
#include "PlaceScene.hpp"
#include "PlaySession.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/ui/GameUi.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numbers>
#include <print>
#include <string>
#include <vector>

namespace {

// A backpack for the hotbar: a held sword, a tool with an icon and a tooltip,
// and a few named ones.
void giveTools(lunar::Player& player, lunar::Model& character)
{
    auto backpack = std::make_shared<lunar::Backpack>();
    backpack->setName("Backpack");
    backpack->setParent(&player);
    auto tool = [](std::string name, lunar::Instance& parent) {
        auto made = std::make_shared<lunar::Tool>();
        made->setName(std::move(name));
        made->setParent(&parent);
        return made;
    };
    tool("Sword", character);
    auto icon = tool("Backpack", *backpack);
    icon->setTextureId("rbxasset://textures/ui/Backpack/Backpack.png");
    icon->setToolTip("A tool with an icon");
    tool("Rocket Launcher", *backpack);
    tool("Slingshot", *backpack);
}

} // namespace

int uiTest(std::span<const std::string_view> args)
{
    std::vector<std::string_view> positional;
    bool scene = true;
    bool chatDemo = false;
    bool rtcDemo = false;
    bool menu = false;
    std::string menuTab;    // --menu-tab: home, players, settings, report, help, luau
    std::string menuDialog; // --menu-dialog: reset or leave
    std::vector<std::string_view> menuKeys; // --menu-keys: keys pressed in the menu, comma separated
    std::optional<std::pair<float, float>> menuHover;
    bool reportDemo = false;
    bool luauDemo = false; // --luau-demo: the Luau tab with a log and a line being typed
    bool showStats = false;
    int crowd = 0;
    float scale = 1.0f;
    bool player = false;
    bool tools = false;
    bool minimap = false;
    bool fullMap = false;
    std::optional<float> span;
    std::optional<float> mapYaw;
    std::optional<float> mapPitch;
    std::optional<float> mapDistance;
    bool map2d = false;
    bool mapCenter = false;
    int mapBench = 0;
    std::optional<std::pair<float, float>> mapHover; // the pointer, pixels
    std::optional<std::pair<lunar::Vector3, lunar::Vector3>> look;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--look" && i + 1 < args.size())
            look = parseLook(args[++i]);
        else if (arg == "--player")
            player = true;
        else if (arg == "--tools")
            tools = player = true;
        else if (arg == "--no-scene")
            scene = false;
        else if (arg == "--chat-demo")
            chatDemo = true;
        else if (arg == "--rtc-demo")
            rtcDemo = true;
        else if (arg == "--menu")
            menu = true;
        else if (arg == "--menu-tab" && i + 1 < args.size())
            menuTab = args[++i], menu = true;
        else if (arg == "--menu-dialog" && i + 1 < args.size())
            menuDialog = args[++i], menu = true;
        else if (arg == "--menu-keys" && i + 1 < args.size()) {
            for (std::string_view rest = args[++i]; !rest.empty();) {
                const std::size_t comma = rest.find(',');
                menuKeys.push_back(rest.substr(0, comma));
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            menu = true;
        } else if (arg == "--menu-hover" && i + 1 < args.size()) {
            const std::string_view spec = args[++i];
            const std::size_t comma = spec.find(',');
            if (comma != std::string_view::npos)
                menuHover
                    = {std::stof(std::string(spec.substr(0, comma))), std::stof(std::string(spec.substr(comma + 1)))};
        } else if (arg == "--report-demo")
            reportDemo = menu = player = true;
        else if (arg == "--luau-demo")
            luauDemo = menu = true;
        else if (arg == "--stats")
            showStats = true;
        else if (arg == "--crowd" && i + 1 < args.size())
            crowd = std::stoi(std::string(args[++i])), player = true;
        else if (arg == "--scale" && i + 1 < args.size())
            scale = std::stof(std::string(args[++i]));
        else if (arg == "--minimap")
            minimap = player = true;
        else if (arg == "--map")
            fullMap = player = true;
        else if (arg == "--span" && i + 1 < args.size())
            span = std::stof(std::string(args[++i]));
        else if (arg == "--map-yaw" && i + 1 < args.size())
            mapYaw = std::stof(std::string(args[++i]));
        else if (arg == "--map-pitch" && i + 1 < args.size())
            mapPitch = std::stof(std::string(args[++i]));
        else if (arg == "--map-distance" && i + 1 < args.size())
            mapDistance = std::stof(std::string(args[++i]));
        else if (arg == "--map-bench" && i + 1 < args.size())
            mapBench = std::stoi(std::string(args[++i]));
        else if (arg == "--map-center")
            mapCenter = fullMap = player = true;
        else if (arg == "--map-2d")
            map2d = fullMap = player = true;
        else if (arg == "--map-hover" && i + 1 < args.size()) {
            // "x,y" in pixels.
            const std::string_view spec = args[++i];
            const std::size_t comma = spec.find(',');
            if (comma != std::string_view::npos)
                mapHover
                    = {std::stof(std::string(spec.substr(0, comma))), std::stof(std::string(spec.substr(comma + 1)))};
        } else
            positional.push_back(arg);
    }
    if (positional.size() != 2 && positional.size() != 4) {
        std::println(stderr, "usage: lunarstone --ui-test <place> <out.png> [width height]");
        std::println(stderr, "           [--no-scene] [--chat-demo] [--rtc-demo] [--menu] [--player] [--tools] [--scale k] [--stats]");
        std::println(stderr, "           [--menu-tab home|players|settings|report|help] [--menu-dialog reset|leave]");
        std::println(stderr, "           [--menu-keys key,key...] [--menu-hover x,y] [--report-demo] [--crowd n]");
        std::println(stderr, "           [--minimap [--span studs]] [--map|--map-2d [--map-yaw deg] [--map-pitch deg]");
        std::println(stderr,
                     "            [--map-distance studs] [--map-center] [--map-hover x,y]] [--map-bench frames]");
        std::println(stderr, "           [--look ex,ey,ez,tx,ty,tz]");
        return 2;
    }
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    if (positional.size() == 4) {
        width = static_cast<std::uint32_t>(std::stoul(std::string(positional[2])));
        height = static_cast<std::uint32_t>(std::stoul(std::string(positional[3])));
    }

    auto bytes = lunar::readFile(positional[0]);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", positional[0], place.error());
        return 1;
    }
    if (player) {
        // A local player whose character has taken some damage, for the health bar and player list.
        auto* players = place->game->getService<lunar::Players>();
        // With the map, everyone gets a root part near the spawn for a marker.
        const lunar::CFrame spawn = spawnPoint(*place->game->workspace());
        if (minimap || fullMap)
            std::println("spawn at {:.1f},{:.1f},{:.1f}", spawn.position.x, spawn.position.y, spawn.position.z);
        int index = 0;
        for (const char* name : {"builderman", "Shedletsky", "Telamon"}) {
            auto p = std::make_shared<lunar::Player>();
            p->setName(name);
            p->setDisplayName(name);
            p->setParent(players);
            if (minimap || fullMap) {
                auto character = std::make_shared<lunar::Model>();
                character->setName(name);
                auto root = std::make_shared<lunar::Part>();
                root->setName("HumanoidRootPart");
                root->setSize({2, 2, 1});
                root->setTransparency(1.0f);
                const float angle = 0.9f * static_cast<float>(index);
                const lunar::Vector3 offset{index == 0 ? 0.0f : 40.0f * std::sin(angle * 2.0f), 3.0f,
                                            index == 0 ? 0.0f : -50.0f * std::cos(angle * 2.0f)};
                root->setCFrame(lunar::CFrame(spawn.position + offset)
                                * lunar::CFrame::angles(0.0f, angle + 0.6f, 0.0f));
                root->setParent(character);
                auto humanoid = std::make_shared<lunar::Humanoid>();
                humanoid->setParent(character);
                character->setParent(place->game->workspace());
                p->setCharacter(character.get());
                if (index == 0)
                    players->assignLocalPlayer(p.get());
                ++index;
                continue;
            }
            if (std::string_view(name) == "builderman") {
                auto character = std::make_shared<lunar::Model>();
                character->setName(name);
                auto humanoid = std::make_shared<lunar::Humanoid>();
                humanoid->setHealth(65.0f);
                humanoid->setParent(character);
                character->setParent(place->game->workspace());
                p->setCharacter(character.get());
                players->assignLocalPlayer(p.get());
                if (tools)
                    giveTools(*p, *character);
            }
        }
    }
    if (crowd > 0) {
        // More players, for a long player list.
        auto* players = place->game->getService<lunar::Players>();
        static constexpr std::array<std::string_view, 8> names{"Roblox",   "Stickmasterluke", "Merely", "Sorcus",
                                                                "Seranok", "Clockwork",       "Games",  "Loleris"};
        for (int i = 0; i < crowd; ++i) {
            auto p = std::make_shared<lunar::Player>();
            const std::string name = std::format("{}{}", names[static_cast<std::size_t>(i) % names.size()],
                                                 i < static_cast<int>(names.size()) ? "" : std::to_string(i));
            p->setName(name);
            p->setDisplayName(i % 3 == 1 ? "Builder Bob" : name);
            p->setParent(players);
        }
    }
    auto context = lunar::render::Context::create();
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 1;
    }
    auto offscreen = lunar::render::OffscreenRenderer::create(**context, width, height);
    if (!offscreen) {
        std::println(stderr, "Vulkan: {}", offscreen.error());
        return 1;
    }

    std::vector<lunar::render::DrawItem> items;
    std::unique_ptr<PlaceScene> placeScene;
    if (scene) {
        placeScene = std::make_unique<PlaceScene>(*place->game);
        placeScene->waitForMeshes();
        items = placeScene->items();
    }
    const lunar::render::Lighting lighting
        = placeScene ? placeScene->lighting() : lunar::render::lightingFrom(*place->game);
    lunar::render::Camera camera
        = lunar::render::framingCamera(items, static_cast<float>(width) / static_cast<float>(height));
    if (look)
        camera.cframe = lunar::render::Camera::lookAt(look->first, look->second);
    if (placeScene) {
        (*offscreen)->renderer().setMaterialTextures(placeScene->materialTextures());
        (*offscreen)->renderer().setTerrainMaterials(placeScene->terrainMaterials());
    }

    // Images load synchronously here: the picture should show them.
    auto config = lunar::assets::AssetConfig::fromEnvironment();
    auto textures = std::make_shared<lunar::assets::TextureLoader>(std::make_shared<lunar::assets::AssetCache>(config));
    lunar::ui::GameUi::Options options;
    options.contentDir = config.contentDir;
    options.images = [textures](std::string_view id) -> std::shared_ptr<const lunar::Image> {
        auto image = textures->load(id);
        return image ? *image : nullptr;
    };
    // A local-only source for previewing the RTC tab without joining a service.
    lunar::ui::LocalEchoChatSource rtc("Friend", "Friend");
    auto ui = lunar::ui::GameUi::create(**context, lunar::render::OffscreenRenderer::colorFormat, 1, *place->game,
                                        std::move(options));
    if (!ui) {
        std::println(stderr, "Vulkan: {}", ui.error());
        return 1;
    }
    (*ui)->attach((*offscreen)->renderer());
    if (placeScene)
        (*ui)->viewports()->setAssets(placeScene->viewportAssets());
    if (rtcDemo) {
        (*ui)->setRtcChatSource(&rtc);
        auto& chat = (*ui)->chat();
        chat.selectTab(lunar::ui::ClassicChat::Tab::Rtc);
        chat.post(lunar::ui::ClassicChat::Tab::Rtc,
                  {.kind = lunar::ui::ChatMessage::Kind::System, .text = "Asteroide RTC (preview)"});
        rtc.send("hello from RTC!");
        chat.focus();
        chat.handle(lunar::platform::TextEvent{"a reply to RTC"});
    }
    if (chatDemo) {
        auto& chat = (*ui)->chat();
        chat.post({.fromName = "Shedletsky", .displayName = "Shedletsky", .text = "hello everyone!"});
        chat.post({.fromName = "builderman", .displayName = "builderman", .text = "welcome to the game"});
        chat.post({.fromName = "Telamon",
                   .displayName = "Telamon",
                   .text = "this is a longer message that should wrap onto a second line in the classic chat window"});
        chat.postSystem("Your friend Guest joined the game.", lunar::Color3::fromRGB(255, 255, 255));
        lunar::ui::ChatMessage error;
        error.kind = lunar::ui::ChatMessage::Kind::Error;
        error.text = "You must wait before sending another message.";
        chat.post(error);
        chat.focus();
        chat.handle(lunar::platform::TextEvent{"typing a reply"});
    }
    // The menu's settings wired to nothing but a line here, so they show as they do in a game window.
    auto& core = (*ui)->core();
    core.setTitle(std::filesystem::path(positional[0]).stem().string());
    core.menuSettings.graphics = lunar::render::GraphicsPreset::Average;
    core.onGraphicsQuality = [](lunar::render::GraphicsPreset) { std::println("menu: graphics quality"); };
    core.onFullscreen = [](bool on) { std::println("menu: fullscreen {}", on); };
    core.onCameraSensitivity = [](float value) { std::println("menu: camera sensitivity {:.1f}", value); };
    core.onShiftLock = [](bool on) { std::println("menu: shift lock {}", on); };
    core.onReset = [] { std::println("menu: reset"); };
    if (showStats) {
        core.menuSettings.performanceStats = true;
        core.setPerformanceStats({.fps = 144, .frameMilliseconds = 6.9f, .drawCalls = 812, .triangles = 1'240'000});
    }
    core.setMenuOpen(menu);
    const lunar::Vector2 screenSize{static_cast<float>(width), static_cast<float>(height)};
    auto press = [&](lunar::platform::Key key, bool shift = false) {
        lunar::platform::KeyEvent down{.key = key, .pressed = true};
        down.modifiers.shift = shift;
        (*ui)->handle(down);
        (*ui)->handle(lunar::platform::KeyEvent{.key = key, .pressed = false});
    };
    auto click = [&](std::string_view id) {
        (*ui)->update(0.0, screenSize, scale);
        if (const auto r = core.menu().controlRect(id)) {
            const double x = (r->x + r->w / 2) / scale, y = (r->y + r->h / 2) / scale;
            (*ui)->handle(lunar::platform::MouseButtonEvent{.button = lunar::platform::MouseButton::Left, .pressed = true, .x = x, .y = y});
            (*ui)->handle(lunar::platform::MouseButtonEvent{.button = lunar::platform::MouseButton::Left, .pressed = false, .x = x, .y = y});
        } else {
            std::println(stderr, "menu: no control {}", id);
        }
    };
    if (menu) {
        (*ui)->update(0.0, screenSize, scale);
        if (!menuTab.empty())
            click("tab." + menuTab);
        if (reportDemo) {
            click("tab.report");
            click("report.kind.1");
            click("report.reason.next");
            click("report.reason.next");
            click("report.description");
            (*ui)->handle(lunar::platform::TextEvent{"kept saying mean things in chat"});
            press(lunar::platform::Key::Enter);
            press(lunar::platform::Key::Enter);
        }
        if (luauDemo) {
            using Kind = lunar::ui::DevConsole::Kind;
            lunar::ui::DevConsole& console = core.console();
            console.run = [&console](const std::string& code) -> std::string {
                if (code.starts_with("print"))
                    console.add(Kind::Output, "Hotel+ seed 59284469, room 0");
                return code.starts_with("game.Workspace.Nope") ? "Console:1: Nope is not a valid member of Workspace "
                                                                 "\"Workspace\""
                                                               : std::string();
            };
            console.add(Kind::Output, "look behind you");
            console.add(Kind::Output, "Preloading assets");
            console.add(Kind::Warning, "Infinite yield possible on 'ReplicatedStorage:WaitForChild(\"GameData\")'");
            console.add(Kind::Info, "Stack Begin");
            console.add(Kind::Error, "Players.SamplePlayer.PlayerGui.Hud.Status:54: attempt to "
                                     "compare number <= nil");
            console.submit("print(workspace:GetChildren()[1].Name)");
            console.submit("game.Workspace.Nope.Parent = nil");
            console.submit("local players = game.Players:GetPlayers()\nfor i, p in ipairs(players) do\n"
                           "    print(i, p.Name, p.UserId)\nend");
            click("tab.luau");
            click("luau.input");
            (*ui)->handle(lunar::platform::TextEvent{
                "-- everyone in this server\n"
                "local players = game.Players:GetPlayers()\n"
                "for i = 1, #players do\n"
                "    local p = players[i]\n"
                "    print(i, p.Name, p.UserId)  -- their id\n"
                "end\n"
                "local first = players[1]\n"
                "print(first and first.Name or \"no one here\")"});
        }
        for (const std::string_view name : menuKeys) {
            const bool shift = name.starts_with("Shift+");
            const std::string_view keyText = shift ? name.substr(6) : name;
            for (std::size_t k = 0; k < lunar::platform::keyCount; ++k)
                if (lunar::platform::keyName(static_cast<lunar::platform::Key>(k)) == keyText) {
                    (*ui)->update(0.0, screenSize, scale);
                    press(static_cast<lunar::platform::Key>(k), shift);
                    break;
                }
        }
        if (menuDialog == "reset")
            core.menu().openDialog(lunar::ui::GameMenu::Dialog::Reset);
        else if (menuDialog == "leave")
            core.menu().openDialog(lunar::ui::GameMenu::Dialog::Leave);
        if (menuHover) {
            lunar::platform::MouseMoveEvent hover;
            hover.x = menuHover->first / scale;
            hover.y = menuHover->second / scale;
            (*ui)->handle(hover);
        }
    }
    std::unique_ptr<MapScene> mapScene;
    if (placeScene && (minimap || fullMap)) {
        // The game camera, for the minimap's turn and the full map's first yaw.
        place->game->workspace()->ensureCurrentCamera()->setCFrame(camera.cframe);
        mapScene = std::make_unique<MapScene>(*placeScene);
        mapScene->buildNow((*ui)->map());
        if (const auto mesh = mapScene->mesh()) {
            const auto& st = mesh->stats;
            std::println("map: {} triangles drawn, {} vertices, {} instances of {} shapes, {:.1f} MiB", st.triangles,
                         mesh->vertices.size(), mesh->instances.size(), mesh->shapes.size(),
                         static_cast<double>(mesh->bytes()) / (1 << 20));
            std::println("  {} tiles, {} batches; built in {:.0f} ms", mesh->tiles.size(), mesh->batches.size(),
                         st.milliseconds);
            std::println("  parts {}, small {}, clear {}, boxed {}, foliage {}", st.parts, st.small, st.clear, st.boxes,
                         st.foliage);
            std::println("  terrain level {} {} triangles, water {}", mapScene->terrainLevel(), st.terrain, st.water);
        }
        if (span)
            (*ui)->map().settings().minimapSpan = *span;
        (*ui)->map().settings().showOtherPlayers = true; // a local test scene
        (*ui)->map().setTitle(std::filesystem::path(positional[0]).stem().string());
        (*ui)->map().setFullOpen(fullMap);
        if (mapCenter) {
            (*ui)->map().update(0.0, {static_cast<float>(width), static_cast<float>(height)}, 1.0f);
            (*ui)->map().findSelf();
        }
        if (mapYaw)
            (*ui)->map().orbit().yaw = *mapYaw * std::numbers::pi_v<float> / 180.0f;
        if (mapPitch)
            (*ui)->map().orbit().pitch = *mapPitch * std::numbers::pi_v<float> / 180.0f;
        if (mapDistance)
            (*ui)->map().orbit().distance = *mapDistance;
        if (map2d)
            (*ui)->map().setMode(lunar::ui::WorldMap::Mode::TwoD);
        if (mapHover)
            (*ui)->map().pointAt(mapHover->first, mapHover->second);
    }

    const lunar::Vector2 size = screenSize;
    (*ui)->update(0.0, size, scale);
    // Hover the second hotbar slot, for its tooltip.
    if (const auto slot = (*ui)->core().backpack().slotRect(1); tools && slot) {
        lunar::platform::MouseMoveEvent hover;
        hover.x = slot->x + slot->w / 2;
        hover.y = slot->y + slot->h / 2;
        (*ui)->handle(hover);
        (*ui)->update(0.0, size, scale);
    }
    lunar::render::PixelImage image = (*offscreen)->render(camera, items, lighting);
    for (int i = 0; i < 64 && (*offscreen)->renderer().stats().texturesPending > 0; ++i) {
        (*ui)->update(0.0, size, scale);
        image = (*offscreen)->render(camera, items, lighting);
    }

    // --map-bench: frames (no 3D scene) with a fresh minimap picture every
    // frame against frames that keep the last one; the difference is what
    // a picture costs, CPU and GPU, waited for.
    if (mapBench > 0 && mapScene) {
        using Clock = std::chrono::steady_clock;
        double gameTime = 1.0;
        auto run = [&](bool redraw, double& uiMs, double& sceneMs) {
            uiMs = sceneMs = 0.0;
            const auto start = Clock::now();
            for (int i = 0; i < mapBench; ++i) {
                const double dt = redraw ? 1.5 / static_cast<double>(lunar::ui::WorldMap::minimapRate) : 0.0;
                gameTime += dt;
                auto t0 = Clock::now();
                mapScene->update(gameTime, (*ui)->map());
                auto t1 = Clock::now();
                (*ui)->update(dt, size);
                auto t2 = Clock::now();
                sceneMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                uiMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
                (*offscreen)->render(camera, {}, lighting);
            }
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count() / mapBench;
        };
        double uiKeep = 0, sceneKeep = 0, uiRedraw = 0, sceneRedraw = 0;
        run(false, uiKeep, sceneKeep); // warm up
        const double keep = run(false, uiKeep, sceneKeep);
        const double redraw = run(true, uiRedraw, sceneRedraw);
        // The picture's triangles for the report: one more frame that draws it.
        (*ui)->update(1.0, size);
        (*offscreen)->render(camera, {}, lighting);
        const auto& stats = (*ui)->map().renderer().stats();
        std::println("map bench, ms/frame: keep {:.3f} (ui {:.3f}, map scene {:.3f})", keep, uiKeep / mapBench,
                     sceneKeep / mapBench);
        std::println("  redraw {:.3f} (ui {:.3f}, map scene {:.3f})", redraw, uiRedraw / mapBench,
                     sceneRedraw / mapBench);
        std::println("  a minimap picture costs {:.3f} ms, {:.3f} ms/frame at {} pictures/s and 60 fps", redraw - keep,
                     (redraw - keep) * static_cast<double>(lunar::ui::WorldMap::minimapRate) / 60.0,
                     lunar::ui::WorldMap::minimapRate);
        std::println(
            "  last picture {} draws, {} triangles; GPU: mesh {:.1f} MiB, moving {:.2f} MiB, images {:.2f} MiB",
            stats.draws, stats.triangles, static_cast<double>(stats.meshBytes) / (1 << 20),
            static_cast<double>(stats.movingBytes) / (1 << 20), static_cast<double>(stats.imageBytes) / (1 << 20));
    }

    const std::string encoded = lunar::render::encodePng(image);
    std::ofstream out(std::string(positional[1]), std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!out) {
        std::println(stderr, "{}: can't write", positional[1]);
        return 1;
    }
    std::println("{} quads, {}x{} -> {}", (*ui)->drawList().quads.size(), width, height, positional[1]);
    return 0;
}
