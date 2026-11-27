#include "LuaApp.hpp"

#include "GameWindow.hpp"
#include "LivePlay.hpp"
#include "OffscreenUi.hpp"

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/RobloxVersion.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/avatar/UserAvatar.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

// What the client puts before each script's bytecode in its own models.
constexpr std::string_view sourceBytecodeHeader{"\x52\x7a\x10\x4a\x5b\x32\x09\xe0", 8};

// Moves the patch's children into `target`. A folder the target already has
// a child of that name for is merged into it instead, so the patch's
// CoreGui/RobloxGui/Modules lands in the RobloxGui already there.
void mergePatch(lunar::Instance& patch, lunar::Instance& target)
{
    for (const lunar::InstancePtr& child : std::vector(patch.children().begin(), patch.children().end())) {
        lunar::Instance* existing = target.findFirstChild(child->name());
        if (existing && child->isA("Folder"))
            mergePatch(*child, *existing);
        else
            child->setParent(&target);
    }
}

// Roblox's client settings for the Mac client (which the 0.742 resources
// come from): fetched at most once a day into the cache, the cached copy
// when the fetch fails, nothing when there's neither.
std::optional<std::string> clientSettings()
{
    namespace fs = std::filesystem;
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path dir = cacheHome && *cacheHome ? fs::path(cacheHome) / "lunarstone"
                         : home                  ? fs::path(home) / ".cache/lunarstone"
                                                 : fs::path(".");
    const fs::path file = dir / "clientsettings-MacDesktopClient.json";
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (!ec && fs::file_time_type::clock::now() - written < std::chrono::hours(24))
        if (auto cached = lunar::readFile(file))
            return std::move(*cached);
    const lunar::assets::RobloxWeb web;
    auto response
        = web.anonymous({.url = "https://clientsettingscdn.roblox.com/v2/settings/application/MacDesktopClient"});
    if (response && response->status == 200) {
        fs::create_directories(dir, ec);
        std::ofstream(file, std::ios::binary) << response->body;
        return std::move(response->body);
    }
    std::println(stderr, "client settings: {}",
                 response ? lunar::assets::describeHttpFailure(*response) : response.error());
    if (auto cached = lunar::readFile(file))
        return std::move(*cached);
    return std::nullopt;
}

// The native shell puts GUAC's app-policy JSON into MemStorageService before
// starting Lua. Its shipped GlobalDist policy is also the offline fallback.
std::optional<std::string> appPolicy(const std::filesystem::path& resources)
{
    auto valid = [](std::string_view text) {
        const auto value = lunar::assets::json::parse(text);
        return value && value->object() && !value->object()->empty();
    };
    if (lunar::assets::AssetConfig::fromEnvironment().download) {
        const lunar::assets::RobloxWeb web;
        auto reply = web.asClientApp(
            {.url = "https://apis.roblox.com/universal-app-configuration/v1/behaviors/app-policy/content",
             .headers = {std::format("User-Agent: {}", lunar::robloxMacUserAgent), "Accept: application/json"}});
        if (reply && reply->status == 200 && valid(reply->body))
            return std::move(reply->body);
        std::println(stderr, "app policy: using shipped defaults ({})",
                     reply ? std::format("HTTP {}", reply->status) : reply.error());
    }
    auto defaults = lunar::readFile(resources / "content/guac/defaultConfigs/GuacDefaultPolicy-GlobalDist.json");
    if (defaults && valid(*defaults))
        return std::move(*defaults);
    std::println(stderr, "app policy: no valid shipped defaults");
    return std::nullopt;
}

// How many of each class under `root`, for the report.
void countClasses(const lunar::Instance& root, std::vector<std::pair<std::string, int>>& counts)
{
    for (const lunar::InstancePtr& d : root.descendants()) {
        auto it = std::ranges::find(counts, d->className(), &std::pair<std::string, int>::first);
        if (it == counts.end())
            counts.emplace_back(std::string(d->className()), 1);
        else
            ++it->second;
    }
}

// The app in a window: its GUI is all there is (no Lunarstone chat or core
// GUI, nothing 3D), laid out for the window, the keyboard and mouse its
// input. Closing the window quits; `seconds` closes it sooner (0: never).
// One window of the app: until it closes (true) or the app launches a game
// (`launch` set, false).
bool runWindow(lunar::DataModel& game, lunar::ScriptContext& context, double seconds, std::vector<Screenshot> shots,
               const std::filesystem::path& control, const std::optional<lunar::ScriptContext::LaunchRequest>& launch)
{
    GameWindow::Options options{
        .title = "Roblox - Lunarstone", .screenshots = std::move(shots), .seconds = seconds, .control = control};
    options.gameOverlay = false;
    options.map.minimap = false;
    options.map.fullMapKey.reset();
    auto made = GameWindow::create(game, options);
    if (!made) {
        std::println(stderr, "{}", made.error());
        return true;
    }
    GameWindow& window = **made;
    // The app measures text as this window's GUI lays it out.
    context.setTextMeasure([&window](std::string_view text, const lunar::Font& font, float size, float width,
                                     bool richText) { return window.ui().measureText(text, font, size, width, richText); });
    lunar::ClientInput input(context);
    window.ui().gui().setDeferredInput(true);
    context.setImageLoadedChecker([&window](std::string_view id) { return window.ui().gui().imageLoaded(id); });
    context.setGuiHitTest([&window](float x, float y) { return window.ui().guiObjectsAt(x, y); });
    context.setGuiFocus([&window](lunar::TextBox* box, bool capture, bool entered) { window.ui().focusTextBox(box, capture, entered); });
    context.setGuiLayoutQuery([&window](lunar::Instance* object) { window.ui().ensureGuiGeometry(object); });
    context.setGameInvitePromptHandler(window.ui().gameInviteHandler());
    window.ui().gui().onPageStep = context.guiPageStepper();
    lunar::Vector2 mouse;
    const lunar::render::Lighting lighting;
    while (!window.window().shouldClose() && !launch) {
        const float dt = window.beginFrame({}, [&](const lunar::platform::Event& event, bool processed) {
            forwardInput(input, event, mouse, false, window.uiHasKeys(), processed, window.ui().gui().takeInputDispatch());
        });
        if (lunar::Camera* camera = game.workspace()->currentCamera())
            camera->updateViewportSize(window.logicalSize());
        input.process();
        context.step(dt);
        window.updateUi(dt);
        window.presenter().drawFrame(lunar::render::Camera{}, {}, lighting);
        if (const auto frames = window.endFrame())
            window.window().setTitle(std::format("Roblox - Lunarstone - {} fps", *frames));
    }
    context.setTextMeasure(nullptr);
    context.setImageLoadedChecker(nullptr);
    context.setGuiHitTest(nullptr);
    context.setGuiFocus(nullptr);
    context.setGuiLayoutQuery(nullptr);
    return !launch;
}

// The game the app launched, played as --net-play plays one (its own window
// for now, with `options`: --seconds, --screenshot), under the live-join
// lock tools/play.sh takes: the account is shared with the other tools. The
// app hears the join and the leave.
void playLaunched(lunar::DataModel& game, const lunar::ScriptContext::LaunchRequest& launch,
                  std::span<const std::string> options)
{
    // The app's ExperienceService: a placeholder its scripts made (the
    // engine doesn't model the class), none if they never asked for it.
    lunar::Instance* experiences = nullptr;
    for (const lunar::InstancePtr& child : game.children())
        if (child->className() == "ExperienceService")
            experiences = child.get();
    if (launch.placeId == 0) {
        std::println(stderr, "the app launched no place (user {}, server {}); only places join yet", launch.userId,
                     launch.gameInstanceId);
        return;
    }
    const int lock = ::open("/tmp/lunarstone-live-join.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        std::println("waiting for another live join to finish (it holds /tmp/lunarstone-live-join.lock)");
        ::flock(lock, LOCK_EX);
    }
    if (experiences)
        experiences->fireEvent("ExperienceJoin");
    const std::string place = std::to_string(launch.placeId);
    std::vector<std::string_view> args{"--net-play", place};
    args.insert(args.end(), options.begin(), options.end());
    std::println("joining place {}", place);
    if (const int code = runNetPlay(args); code != 0)
        std::println(stderr, "the game ended with code {}", code);
    if (experiences)
        experiences->fireEvent("ExperienceLeave");
    if (lock >= 0)
        ::close(lock);
}

} // namespace

int runLuaApp(std::span<const std::string_view> args)
{
    if (args.size() < 2)
        return -1;
    const std::filesystem::path resources(args[1]);
    double seconds = 0.0; // 0: until the window closes (headless: 5)
    std::filesystem::path picture;
    std::uint32_t width = 1600, height = 900;
    bool headless = false;
    std::vector<Screenshot> screenshots;
    std::filesystem::path control; // --control: input from a file (ControlFile)
    struct Click {
        double at;
        double x, y;
    };
    std::vector<Click> clicks; // --click seconds:x,y for a picture
    std::vector<std::string> gameOptions; // --game-seconds, --game-screenshot: for the games it launches
    auto number = [](std::string_view text, auto& out) {
        return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc();
    };
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size()) {
            if (!number(args[++i], seconds))
                return -1;
        } else if (args[i] == "--render" && i + 1 < args.size()) {
            picture = args[++i];
            headless = true;
        } else if (args[i] == "--control" && i + 1 < args.size()) {
            control = args[++i];
        } else if (args[i] == "--click" && i + 1 < args.size()) {
            // seconds:x,y
            const std::string_view text = args[++i];
            const std::size_t colon = text.find(':'), comma = text.find(',');
            Click click{};
            if (colon == std::string_view::npos || comma == std::string_view::npos || comma < colon
                || !number(text.substr(0, colon), click.at) || !number(text.substr(colon + 1, comma - colon - 1), click.x)
                || !number(text.substr(comma + 1), click.y))
                return -1;
            clicks.push_back(click);
        } else if (args[i] == "--game-seconds" && i + 1 < args.size()) {
            if (double limit = 0; !number(args[i + 1], limit) || limit <= 0.0)
                return -1;
            gameOptions.insert(gameOptions.end(), {"--seconds", std::string(args[++i])});
        } else if (args[i] == "--game-screenshot" && i + 1 < args.size()) {
            if (!parseScreenshot(args[i + 1]))
                return -1;
            gameOptions.insert(gameOptions.end(), {"--screenshot", std::string(args[++i])});
        } else if (args[i] == "--headless") {
            headless = true;
        } else if (args[i] == "--screenshot" && i + 1 < args.size()) {
            auto shot = parseScreenshot(args[++i]);
            if (!shot)
                return -1;
            screenshots.push_back(std::move(*shot));
        } else if (args[i] == "--size" && i + 2 < args.size()) {
            if (!number(args[i + 1], width) || !number(args[i + 2], height) || width == 0 || height == 0)
                return -1;
            i += 2;
        } else {
            return -1;
        }
    }

    // Fonts and images must come from the same bundle as its scripts. Keep
    // an explicit LUNAR_CONTENT_DIR override, as the asset loaders do.
    const std::string contentDir = (resources / "content").string();
    ::setenv("LUNAR_CONTENT_DIR", contentDir.c_str(), 0);

    const auto placePath = resources / "ExtraContent/places/Mobile.rbxl";
    const auto appPath = resources / "ExtraContent/models/UniversalApp/UniversalApp.rbxm";
    auto placeBytes = lunar::readFile(placePath);
    auto appBytes = placeBytes ? lunar::readFile(appPath) : placeBytes;
    if (!placeBytes || !appBytes) {
        std::println(stderr, "{}", (!placeBytes ? placeBytes : appBytes).error());
        return 1;
    }
    auto place = lunar::loadPlace(*placeBytes);
    if (!place) {
        std::println(stderr, "{}: {}", placePath.string(), place.error());
        return 1;
    }
    auto app = lunar::loadModel(*appBytes);
    if (!app) {
        std::println(stderr, "{}: {}", appPath.string(), app.error());
        return 1;
    }
    for (const std::string& warning : app->warnings)
        std::println(stderr, "UniversalApp.rbxm: {}", warning);
    const std::shared_ptr<lunar::DataModel>& game = place->game;

    // PatchRoot: DataModelInstances/<service>/... into the services,
    // CoreScripts kept apart (the client runs them; they aren't in the tree).
    lunar::Instance* patchRoot = nullptr;
    for (const lunar::InstancePtr& root : app->roots)
        if (root->name() == "PatchRoot")
            patchRoot = root.get();
    lunar::Instance* instances = patchRoot ? patchRoot->findFirstChild("DataModelInstances") : nullptr;
    lunar::Instance* coreScripts = patchRoot ? patchRoot->findFirstChild("CoreScripts") : nullptr;
    lunar::Instance* starter = coreScripts ? coreScripts->findFirstChild("LuaAppStarterScript") : nullptr;
    if (!instances || !starter) {
        std::println(stderr, "{}: no PatchRoot with DataModelInstances and CoreScripts.LuaAppStarterScript",
                     appPath.string());
        return 1;
    }
    for (const lunar::InstancePtr& service : std::vector(instances->children().begin(), instances->children().end())) {
        lunar::Instance* target = game->getService(service->name());
        if (!target) {
            std::println(stderr, "UniversalApp.rbxm: no service {} for its patch", service->name());
            continue;
        }
        mergePatch(*service, *target);
    }

    if (auto settings = clientSettings()) {
        if (auto count = lunar::ScriptContext::loadClientSettings(*settings))
            std::println("client settings: {} flags", *count);
        else
            std::println(stderr, "client settings: not the JSON Roblox serves; flags keep their defaults");
    }

    // The signed-in user as the local player, which the client makes before
    // the app starts (LUAUFORGE_MAIN waits for Players.LocalPlayer): whoever
    // the cookie belongs to, else a guest.
    {
        const lunar::assets::RobloxWeb web;
        auto user = lunar::avatar::authenticatedUser(web);
        if (!user) {
            std::println(stderr, "signed-in user: {}; the app runs as a guest", user.error());
            user = lunar::avatar::RobloxUser{.id = 0, .name = "Guest", .displayName = "Guest"};
        }
        auto* players = game->getService<lunar::Players>();
        auto player = std::make_shared<lunar::Player>();
        player->setName(user->name);
        player->setDisplayName(user->displayName);
        player->setUserId(user->id);
        player->setParent(players);
        players->assignLocalPlayer(player.get());
        std::println("signed in as {} (@{}, user {})", user->displayName, user->name, user->id);
        // What the native shell keeps for the app once someone signs in (the
        // keys of the app's LocalStorageKey table), which it reads at start
        // to know who that is. GUESS: membership, age and country defaults;
        // the tracker and installation ids are made up and fixed.
        if (user->id > 0) {
            using lunar::ScriptContext;
            ScriptContext::setAppStorageItem("UserId", std::to_string(user->id));
            ScriptContext::setAppStorageItem("Username", user->name);
            ScriptContext::setAppStorageItem("DisplayName", user->displayName);
            ScriptContext::setAppStorageItem("Membership", "0");
            ScriptContext::setAppStorageItem("IsUnder13", "false");
            ScriptContext::setAppStorageItem("HasRobloxSubscription", "false");
            ScriptContext::setAppStorageItem("CountryCode", "US");
            ScriptContext::setAppStorageItem("RobloxLocaleId", "en-us");
            ScriptContext::setAppStorageItem("BrowserTrackerId", "174281629301");
            ScriptContext::setAppStorageItem("AppInstallationId", "8a7c1f62-5d3e-4b0a-9f41-2e6b8d9c0a17");
        }
    }

    // The app lays itself out for the camera's viewport: the picture's size.
    if (lunar::Camera* camera = game->workspace()->currentCamera())
        camera->updateViewportSize({static_cast<float>(width), static_cast<float>(height)});

    lunar::ScriptContext context(game, lunar::ScriptRole::Client);
    int errors = 0;
    context.setOutput([&errors](lunar::MessageType type, std::string_view message) {
        if (type == lunar::MessageType::Error)
            ++errors;
        const bool toStderr = type == lunar::MessageType::Warning || type == lunar::MessageType::Error;
        std::println(toStderr ? stderr : stdout, "{}", message);
    });
    context.setCoreScriptAccess();
    if (auto policy = appPolicy(resources)) {
        context.setMemStorageItem("GUAC:-1:app-policy", *policy);
        const auto* player = game->getService<lunar::Players>()->localPlayer();
        if (player && player->userId() > 0)
            context.setMemStorageItem(std::format("GUAC:{}:app-policy", player->userId()), *policy);
    }
    // The app's Source is bytecode behind the client's header; strip it and
    // the runtime unscrambles the opcodes as it does a server's.
    context.setNetworkBytecode([](std::string_view source) -> std::optional<std::string> {
        if (!source.starts_with(sourceBytecodeHeader))
            return std::nullopt;
        return std::string(source.substr(sourceBytecodeHeader.size()));
    });

    // A picture's GUI is made first, so the app measures text by it as it runs.
    std::unique_ptr<lunar::render::Context> renderContext;
    std::unique_ptr<lunar::render::OffscreenRenderer> offscreen;
    std::unique_ptr<OffscreenUi> offscreenUi;
    if (!picture.empty()) {
        auto madeContext = lunar::render::Context::create({});
        auto madeOffscreen = madeContext ? lunar::render::OffscreenRenderer::create(**madeContext, width, height)
                                         : std::unexpected(madeContext.error());
        auto madeUi = madeOffscreen ? OffscreenUi::create(**madeContext, **madeOffscreen, *game, false)
                                    : std::unexpected(madeOffscreen.error());
        if (!madeUi) {
            std::println(stderr, "Vulkan: {}", madeUi.error());
            return 2;
        }
        renderContext = std::move(*madeContext);
        offscreen = std::move(*madeOffscreen);
        offscreenUi = std::move(*madeUi);
        offscreenUi->bind(context);
        context.setImageLoadedChecker(
            [ui = offscreenUi.get()](std::string_view id) { return ui->ui().gui().imageLoaded(id); });
        context.setTextMeasure([ui = offscreenUi.get()](std::string_view text, const lunar::Font& font, float size,
                                                        float w, bool richText) {
            return ui->ui().measureText(text, font, size, w, richText);
        });
    }

    std::println("Starting the Lua app from {}", resources.string());
    // A ModuleScript the client runs as a script: it returns nothing.
    if (auto started = context.runScript(*starter); !started) {
        std::println(stderr, "{}", started.error().message);
        if (!started.error().traceback.empty())
            std::print(stderr, "Stack Begin\n{}Stack End\n", started.error().traceback);
        ++errors;
    }
    if (!headless) {
        // The app until its window closes; a game it launches is played in
        // between, then the app comes back.
        std::optional<lunar::ScriptContext::LaunchRequest> launch;
        context.setLaunchHandler([&launch](const lunar::ScriptContext::LaunchRequest& request) { launch = request; });
        while (!runWindow(*game, context, seconds, std::move(screenshots), control, launch)) {
            const lunar::ScriptContext::LaunchRequest request = *launch;
            launch.reset();
            screenshots.clear();
            playLaunched(*game, request, gameOptions);
        }
        context.setLaunchHandler(nullptr);
        return errors > 0 ? 1 : 0;
    }

    // Frames at 60 Hz in real time: the app waits on the web, which takes
    // the time it takes.
    if (seconds <= 0.0)
        seconds = 5.0;
    constexpr double frame = 1.0 / 60.0;
    auto next = std::chrono::steady_clock::now();
    std::ranges::sort(clicks, {}, &Click::at);
    std::size_t clicked = 0;
    if (offscreenUi)
        offscreenUi->setWaitForImages(false);
    lunar::ClientInput headlessInput(context);
    lunar::Vector2 headlessMouse;
    while (context.time() < seconds) {
        headlessInput.process();
        context.step(frame);
        // A picture's GUI is laid out as it runs, so clicks land where the
        // app's buttons are (and the app reads its sizes as a window has them).
        if (offscreenUi) {
            offscreenUi->update(frame);
            for (; clicked < clicks.size() && clicks[clicked].at <= context.time(); ++clicked) {
                const Click& c = clicks[clicked];
                lunar::ui::GameUi& gui = offscreenUi->ui();
                const std::array<lunar::platform::Event, 3> events{
                    lunar::platform::MouseMoveEvent{.x=c.x, .y=c.y},
                    lunar::platform::MouseButtonEvent{.pressed=true, .x=c.x, .y=c.y},
                    lunar::platform::MouseButtonEvent{.pressed=false, .x=c.x, .y=c.y}};
                for (const auto& event : events) {
                    const bool used = gui.handle(event);
                    forwardInput(headlessInput, event, headlessMouse, false, gui.keyboardCaptured(), used, gui.gui().takeInputDispatch());
                    headlessInput.process();
                }
                std::println("click {:.0f},{:.0f} at {:.1f} s", c.x, c.y, context.time());
            }
        }
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    if (offscreenUi)
        offscreenUi->setWaitForImages(true);

    if (offscreenUi) {
        // CoreGui as the GUI engine lays it out, over nothing. Twice: style
        // queries go by the layout before them.
        offscreenUi->update();
        // The image wait may have completed textures since the last script
        // frame. Let IsLoaded listeners remove their loading placeholders.
        context.step(0.0);
        offscreenUi->update();
        const lunar::render::PixelImage image = offscreen->render(lunar::render::Camera{}, {});
        const std::string encoded = lunar::render::encodePng(image);
        std::ofstream(picture, std::ios::binary).write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        std::println("{} quads, {}x{} -> {}", offscreenUi->quads(), width, height, picture.string());
    }
    context.setTextMeasure(nullptr); // the picture's GUI goes before the context
    context.setImageLoadedChecker(nullptr);
    context.setGuiHitTest(nullptr);
    context.setGuiFocus(nullptr);
    context.setGuiLayoutQuery(nullptr);

    if (std::getenv("LUNAR_LUA_APP_TREE")) {
        // The app's GUI as it stands: each GuiObject's class, name, size and
        // whether it shows, indented by depth (for finding what's missing).
        auto dump = [](auto& self, const lunar::Instance& node, int depth) -> void {
            if (depth > 60)
                return;
            std::string line(static_cast<std::size_t>(depth) * 2, ' ');
            line += std::format("{} {}", node.className(), node.name());
            if (const auto* gui = dynamic_cast<const lunar::GuiObject*>(&node)) {
                const lunar::UDim2 size = gui->size();
                line += std::format(" abs={}x{}@{},{} size=({},{},{},{}) auto={} order={}{}", gui->absoluteSize().x,
                                    gui->absoluteSize().y, gui->absolutePosition().x, gui->absolutePosition().y,
                                    size.x.scale, size.x.offset, size.y.scale, size.y.offset,
                                    static_cast<int>(gui->automaticSize()), gui->layoutOrder(),
                                    gui->visible() ? "" : " hidden");
                if (gui->backgroundTransparency() < 1.0f) {
                    const lunar::Color3 c = gui->backgroundColor3();
                    line += std::format(" bg={},{},{}/{}", std::lround(c.r * 255.0f), std::lround(c.g * 255.0f),
                                        std::lround(c.b * 255.0f), gui->backgroundTransparency());
                }
                if (!node.tags().empty())
                    line += " tags:";
                for (const std::string& tag : node.tags())
                    line += " " + tag;
            }
            std::println("{}", line);
            for (const lunar::InstancePtr& child : node.children())
                if (!child->isA("ModuleScript"))
                    self(self, *child, depth + 1);
        };
        if (lunar::Instance* coreGui = game->findService("CoreGui"))
            for (const lunar::InstancePtr& child : coreGui->children())
                if (child->isA("ScreenGui"))
                    dump(dump, *child, 0);
    }


    std::vector<std::pair<std::string, int>> counts;
    if (lunar::Instance* coreGui = game->findService("CoreGui"))
        countClasses(*coreGui, counts);
    std::ranges::sort(counts, std::ranges::greater{}, &std::pair<std::string, int>::second);
    std::print("CoreGui after {:.1f} s:", context.time());
    for (std::size_t i = 0; i < std::min<std::size_t>(counts.size(), 12); ++i)
        std::print(" {} {}", counts[i].second, counts[i].first);
    std::println("\n{} error(s)", errors);
    return errors > 0 ? 1 : 0;
}
