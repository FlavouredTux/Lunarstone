#include "AvatarRender.hpp"
#include "ConsoleApi.hpp"
#include "Look.hpp"
#include "LuaApp.hpp"
#include "NetJoin.hpp"
#include "NetTools.hpp"
#include "OffscreenUi.hpp"
#include "Online.hpp"
#include "PlaceScene.hpp"
#include "PlaySession.hpp"
#include "Playtest.hpp"
#include "TerrainExport.hpp"
#include "UiTest.hpp"
#include "Viewer.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/audio/Decode.hpp>
#include <lunar/assets/ImageDecode.hpp>
#include <lunar/assets/MeshFile.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/avatar/Clothing.hpp>
#include <lunar/physics/PlayerCamera.hpp>
#include <lunar/physics/World.hpp>
#include <lunar/platform/Window.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/LightGrid.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Lod.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace {

void printTree(const lunar::Instance& instance, int depth = 0)
{
    std::println("{}{} ({})", std::string(static_cast<std::size_t>(depth) * 2, ' '), instance.name(),
                 instance.className());
    for (const auto& child : instance.children())
        printTree(*child, depth + 1);
}

void printWarnings(const std::vector<std::string>& warnings)
{
    if (!warnings.empty())
        std::println("\n{} warnings:", warnings.size());
    for (const std::string& warning : warnings)
        std::println("  {}", warning);
}

// .rbxm/.rbxmx files load as models, anything else as a place.
void printProperties(const lunar::Instance& instance)
{
    std::println("{} ({})", instance.fullName(), instance.className());
    for (const lunar::Property* property : instance.properties())
        std::println("  {:<24} {:<14} {}", property->name, property->type, lunar::toString(property->get(instance)));
}

// The properties of the first instance with that name and of its
// descendants, for poking at a place's contents.
int showProperties(const std::filesystem::path& path, std::string_view name)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    // Services first, so "Lighting" isn't some folder by that name.
    std::vector<lunar::InstancePtr> candidates(place->game->children().begin(), place->game->children().end());
    std::ranges::copy(place->game->descendants(), std::back_inserter(candidates));
    for (const lunar::InstancePtr& found : candidates) {
        if (found->name() != name)
            continue;
        printProperties(*found);
        for (const lunar::InstancePtr& d : found->descendants())
            printProperties(*d);
        return 0;
    }
    std::println(stderr, "no instance named {}", name);
    return 1;
}

int showFile(const std::filesystem::path& path)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }

    if (path.extension() == ".rbxm" || path.extension() == ".rbxmx") {
        auto model = lunar::loadModel(*bytes);
        if (!model) {
            std::println(stderr, "{}: {}", path.string(), model.error());
            return 1;
        }
        for (const auto& root : model->roots)
            printTree(*root);
        printWarnings(model->warnings);
        return 0;
    }

    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    printTree(*place->game);
    printWarnings(place->warnings);
    return 0;
}

template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

std::string describe(const lunar::platform::Event& event)
{
    using namespace lunar::platform;
    auto mods = [](Modifiers m) {
        return std::format("{}{}{}{}", m.ctrl ? " ctrl" : "", m.shift ? " shift" : "", m.alt ? " alt" : "",
                           m.super ? " super" : "");
    };
    return std::visit(
        Overloaded{
            [&](const KeyEvent& e) {
                return std::format("key {} {} (scancode {}, keysym {:#x}){}", keyName(e.key),
                                   e.repeat ? "repeat" : (e.pressed ? "down" : "up"), e.scancode, e.keysym,
                                   mods(e.modifiers));
            },
            [&](const ModifiersEvent& e) { return std::format("modifiers:{}", mods(e.modifiers)); },
            [](const TextEvent& e) { return std::format("text \"{}\"", e.text); },
            [](const MouseMoveEvent& e) { return std::format("mouse at {:.1f}, {:.1f}", e.x, e.y); },
            [&](const MouseButtonEvent& e) {
                return std::format("mouse {} {} at {:.1f}, {:.1f}{}", mouseButtonName(e.button),
                                   e.pressed ? "down" : "up", e.x, e.y, mods(e.modifiers));
            },
            [](const ScrollEvent& e) { return std::format("scroll {:+.2f}, {:+.2f}", e.dx, e.dy); },
            [](const RelativeMotionEvent& e) {
                return std::format("relative motion {:+.2f}, {:+.2f} (raw {:+.2f}, {:+.2f})", e.dx, e.dy, e.rawDx,
                                   e.rawDy);
            },
            [](const MouseEnterEvent& e) { return std::string(e.entered ? "pointer entered" : "pointer left"); },
            [](const FocusEvent& e) { return std::string(e.focused ? "focused" : "unfocused"); },
            [](const ResizeEvent& e) {
                return std::format("resized to {}x{} (framebuffer {}x{}, scale {})", e.logicalSize.width,
                                   e.logicalSize.height, e.framebufferSize.width, e.framebufferSize.height, e.scale);
            },
            [](const PointerLockEvent& e) { return std::string(e.locked ? "pointer locked" : "pointer unlocked"); },
            [](const CloseEvent&) { return std::string("close requested"); },
        },
        event);
}

// Opens a window and prints its input. Escape quits, F11 toggles fullscreen,
// Tab toggles pointer lock.
int showWindow()
{
    using namespace lunar::platform;
    auto created = Window::create({.title = "Lunarstone"});
    if (!created) {
        std::println(stderr, "{}", created.error());
        return 1;
    }
    Window& window = *created;
    Extent size = window.framebufferSize();
    std::println("window {}x{} (scale {}), {} decorations", size.width, size.height, window.scale(),
                 window.hasServerDecorations() ? "server-side" : "no");
    std::println("Escape quits, F11 toggles fullscreen, Tab toggles pointer lock");

    // Nothing renders yet, so give the surface some content to be shown at all.
    if (auto filled = window.fillSolid(0xff1b2230); !filled) {
        std::println(stderr, "{}", filled.error());
        return 1;
    }

    while (!window.shouldClose()) {
        window.waitEvents();
        while (auto event = window.events().pop()) {
            std::println("{}", describe(*event));
            const auto* key = std::get_if<KeyEvent>(&*event);
            if (!key || !key->pressed || key->repeat)
                continue;
            if (key->key == Key::Escape)
                window.setShouldClose(true);
            else if (key->key == Key::F11)
                window.toggleFullscreen();
            else if (key->key == Key::Tab && !window.setPointerLocked(!window.pointerLockRequested()))
                std::println("pointer lock isn't supported by this compositor");
        }
        std::fflush(stdout);
    }
    if (window.error()) {
        std::println(stderr, "{}", *window.error());
        return 1;
    }
    return 0;
}

struct RenderOptions {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::optional<std::pair<lunar::Vector3, lunar::Vector3>> look; // eye, target
    std::optional<double> clockTime;
    double seconds = 0.0; // game time, for animated water
    int benchFrames = 0;
    std::optional<int> qualityLevel; // graphics quality 1 to 21: how far parts are drawn
    lunar::render::GraphicsSettings graphics;
    bool gui = false;                // the local player's GUI and the core GUI on top (OffscreenUi)
};

int renderGame(const std::shared_ptr<lunar::DataModel>& game, const std::filesystem::path& outPath,
               const RenderOptions& options);

// Renders a place offscreen to a PNG, or a PPM if the name ends in .ppm.
// With benchFrames, renders that many more frames first and reports timings.
// LUNAR_POST: post effects to look at in place of the game's, as
// "bloom=intensity,size,threshold blur=size rays=intensity,spread
// dof=focus,radius,near,far" (any of them; "none" for none).
lunar::render::PostLook parsePostLook(std::string_view text)
{
    lunar::render::PostLook look;
    auto numbers = [](std::string_view list) {
        std::vector<float> out;
        while (!list.empty()) {
            const std::size_t comma = list.find(',');
            out.push_back(std::stof(std::string(list.substr(0, comma))));
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        }
        return out;
    };
    for (const auto part : std::views::split(text, ' ')) {
        const std::string_view item(part.begin(), part.end());
        const std::size_t equals = item.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view name = item.substr(0, equals);
        const std::vector<float> values = numbers(item.substr(equals + 1));
        auto at = [&](std::size_t i, float fallback) { return i < values.size() ? values[i] : fallback; };
        if (name == "bloom") {
            look.bloom = true;
            look.bloomIntensity = at(0, look.bloomIntensity);
            look.bloomSize = at(1, look.bloomSize);
            look.bloomThreshold = at(2, look.bloomThreshold);
        } else if (name == "blur") {
            look.blur = at(0, 24.0f);
        } else if (name == "rays") {
            look.sunRays = true;
            look.sunRaysIntensity = at(0, look.sunRaysIntensity);
            look.sunRaysSpread = at(1, look.sunRaysSpread);
        } else if (name == "dof") {
            look.depthOfField = true;
            look.focusDistance = at(0, look.focusDistance);
            look.inFocusRadius = at(1, look.inFocusRadius);
            look.nearIntensity = at(2, look.nearIntensity);
            look.farIntensity = at(3, look.farIntensity);
        }
    }
    return look;
}

std::string describePostLook(const lunar::render::PostLook& look)
{
    std::string out;
    if (look.bloom)
        out += std::format(" bloom (Intensity {}, Size {}, Threshold {})", look.bloomIntensity, look.bloomSize,
                           look.bloomThreshold);
    if (look.blur > 0.0f)
        out += std::format(" blur (Size {})", look.blur);
    if (look.sunRays)
        out += std::format(" sun rays (Intensity {}, Spread {})", look.sunRaysIntensity, look.sunRaysSpread);
    if (look.depthOfField)
        out += std::format(" depth of field (FocusDistance {}, InFocusRadius {}, Near {}, Far {})",
                           look.focusDistance, look.inFocusRadius, look.nearIntensity, look.farIntensity);
    return out;
}

int renderFile(const std::filesystem::path& placePath, const std::filesystem::path& outPath,
               const RenderOptions& options)
{
    auto bytes = lunar::readFile(placePath);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", placePath.string(), place.error());
        return 1;
    }
    return renderGame(place->game, outPath, options);
}

// Renders an already loaded game (a place file's, or a network replica's).
int renderGame(const std::shared_ptr<lunar::DataModel>& game, const std::filesystem::path& outPath,
               const RenderOptions& options)
{
    struct {
        std::shared_ptr<lunar::DataModel> game;
    } holder{game}, *place = &holder;
    using Clock = std::chrono::steady_clock;
    const auto extractStart = Clock::now();
    // Classic clothing: every Humanoid's Shirt and Pants composited onto its
    // body first (the client dresses NPCs too), as --play does.
    lunar::avatar::ClothingCompositor clothing(std::make_shared<lunar::assets::AssetCache>());
    if (lunar::Workspace* workspace = place->game->workspace()) {
        int dressed = 0;
        for (const lunar::InstancePtr& instance : workspace->descendants())
            if (auto* model = dynamic_cast<lunar::Model*>(instance.get());
                model && model->findFirstChildOfClass("Humanoid") && clothing.apply(*model))
                ++dressed;
        if (dressed > 0)
            std::println("clothing: {} characters dressed", dressed);
    }
    PlaceScene scene(*place->game);
    scene.setTerrainDistance(options.graphics.terrainDistance);
    scene.wrapTextures([&](lunar::render::TextureProvider next) { return clothing.provider(std::move(next)); });
    scene.waitForMeshes();
    const auto& items = scene.items();
    // The whole terrain at its coarsest to frame the view, then what the camera wants.
    scene.settleTerrain(std::nullopt);
    auto camera
        = lunar::render::framingCamera(items, static_cast<float>(options.width) / static_cast<float>(options.height));
    if (options.look)
        camera.cframe = lunar::render::Camera::lookAt(options.look->first, options.look->second);
    scene.settleTerrain(camera.cframe.position);
    scene.followParts(); // Highlights' looks, as a frame in play mode has them
    const auto extractTime = std::chrono::duration<double, std::milli>(Clock::now() - extractStart).count();
    if (options.clockTime)
        place->game->getService<lunar::Lighting>()->setClockTime(*options.clockTime);
    lunar::render::Lighting lighting = scene.lighting();
    lighting.time = options.seconds;
    if (const char* post = std::getenv("LUNAR_POST"))
        lighting.post = parsePostLook(post);

    // Particles as they'd be after the game's first seconds (--seconds, or
    // five: most emitters have filled out by then), their textures loaded.
    const float aspect = static_cast<float>(options.width) / static_cast<float>(options.height);
    lunar::render::ParticleSystem particles(*place->game->workspace(), scene.textures());
    particles.setSettings({.density = options.graphics.particleDensity});
    particles.advance(options.seconds > 0.0 ? static_cast<float>(options.seconds) : 5.0f, camera.cframe.position);
    {
        // What the view shows asks for its textures.
        lunar::render::EffectGeometry unused;
        particles.collect({.camera = camera, .viewProjection = camera.viewProjection(aspect)}, unused);
        scene.waitForMeshes();
    }

    auto context = lunar::render::Context::create({.rayTracing = options.graphics.rayTracing});
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 1;
    }
    if (options.graphics.rayTracing && !(*context)->rayTracing())
        std::println(stderr, "this GPU can't ray trace (no ray queries): shadow maps instead");

    auto offscreen = lunar::render::OffscreenRenderer::create(**context, options.width, options.height);
    if (!offscreen) {
        std::println(stderr, "Vulkan: {}", offscreen.error());
        return 1;
    }
    (*offscreen)->renderer().setGraphics(options.graphics);
    (*offscreen)->renderer().addEffects(&particles);
    (*offscreen)->renderer().setMaterialTextures(scene.materialTextures());
    (*offscreen)->renderer().setTerrainMaterials(scene.terrainMaterials());
    (*offscreen)->renderer().addEffects(&scene.ribbons());
    (*offscreen)->renderer().addEffects(&scene.adornments());
    (*offscreen)->renderer().setLights(&scene.lights());
    if (options.qualityLevel)
        (*offscreen)->renderer().setDrawDistance(lunar::render::qualityDrawDistance(*options.qualityLevel));

    if (options.benchFrames > 0) {
        (*offscreen)->render(camera, items, lighting); // uploads every mesh
        const auto start = Clock::now();
        double recordMs = 0.0;
        // Medians too: the averages swing with whatever else the machine runs.
        std::vector<double> frameTimes;
        std::vector<double> recordTimes;
        for (int i = 0; i < options.benchFrames; ++i) {
            const auto frameStart = Clock::now();
            (*offscreen)->render(camera, items, lighting);
            frameTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
            recordTimes.push_back((*offscreen)->renderer().stats().recordMilliseconds);
            recordMs += recordTimes.back();
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto median = [](std::vector<double>& values) {
            std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
            return values[values.size() / 2];
        };
        const auto& stats = (*offscreen)->renderer().stats();
        std::println("extract {:.1f} ms; {:.2f} ms/frame ({:.2f} ms recording) over {} frames", extractTime,
                     ms / options.benchFrames, recordMs / options.benchFrames, options.benchFrames);
        std::println("median {:.2f} ms/frame ({:.2f} ms recording)", median(frameTimes), median(recordTimes));
        std::println("{} items, {} visible, {} draws, {} triangles, {} meshes, {} textures", stats.items, stats.visible,
                     stats.drawCalls, stats.triangles, stats.meshesResident, stats.texturesResident);
        std::println("levels of detail drawn {}, {} past the draw distance", stats.levels, stats.tooFar);
        std::println("GPU: {} MiB of meshes, {} MiB allocated", stats.meshBytes >> 20, stats.deviceBytes >> 20);
        const lunar::terrain::TerrainLod::Stats terrain = scene.terrainStats();
        std::println("terrain: {} triangles in {} MiB ({} MiB cached), nodes per level {}, distance scale {:.2f}",
                     terrain.triangles, terrain.drawnBytes >> 20, terrain.cachedBytes >> 20,
                     std::span(terrain.nodes).first(5), terrain.distanceScale);
    }
    std::unique_ptr<OffscreenUi> ui;
    if (options.gui) {
        // Billboards face the game's camera: make it the picture's.
        if (lunar::Workspace* workspace = place->game->workspace()) {
            lunar::Camera* current = workspace->ensureCurrentCamera();
            current->setCFrame(camera.cframe);
            current->setFieldOfView(camera.fieldOfView);
        }
        auto made = OffscreenUi::create(**context, **offscreen, *place->game);
        if (!made) {
            std::println(stderr, "Vulkan: {}", made.error());
            return 1;
        }
        ui = std::move(*made);
        ui->ui().viewports()->setAssets(scene.viewportAssets());
    }
    // Textures past one frame's upload budget arrive over the next frames
    // (many more of them under a small LUNAR_GPU_BUDGET_MB).
    if (ui)
        ui->update();
    lunar::render::PixelImage image = (*offscreen)->render(camera, items, lighting);
    for (int i = 0; i < 1000 && (*offscreen)->renderer().stats().texturesPending > 0; ++i) {
        if (ui)
            ui->update();
        image = (*offscreen)->render(camera, items, lighting);
    }

    const std::string encoded
        = outPath.extension() == ".ppm" ? lunar::render::encodePpm(image) : lunar::render::encodePng(image);
    std::ofstream out(outPath, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!out) {
        std::println(stderr, "{}: can't write", outPath.string());
        return 1;
    }
    std::println("{} parts, {}x{} -> {}", items.size(), options.width, options.height, outPath.string());
    if (ui)
        std::println("GUI: {} quads", ui->quads());
    if (const auto counts = particles.stats(); counts.particles > 0)
        std::println("particles: {} alive from {} of {} emitters, {} drawn in {} batches; {:.3f} ms a step, {:.3f} ms "
                     "to draw",
                     counts.particles, counts.active, counts.emitters, counts.drawn, counts.batches,
                     counts.stepMilliseconds, counts.collectMilliseconds);
    const auto& stats = (*offscreen)->renderer().stats();
    if (std::getenv("LUNAR_LIGHT_PROBE")) {
        // research: the light grid's sky and sun (0-255) in a column of
        // voxels through the camera, as a grid built from these items sees them.
        lunar::render::LightGrid grid;
        grid.build(items, camera.cframe.position, {.sunDirection = lighting.sunDirection, .shadows = true});
        const lunar::render::GridPoint at = lunar::render::LightGrid::voxelAt(camera.cframe.position);
        for (int dy = 3; dy >= -3; --dy) {
            const lunar::render::GridPoint v{at.x, at.y + dy, at.z};
            std::println("light probe: voxel y{:+d}: sky {}, sun {}, occupancy {}/{}/{}", dy, grid.sky(v), grid.sun(v),
                         grid.occupancy(0, v), grid.occupancy(1, v), grid.occupancy(2, v));
        }
        // LUNAR_LIGHT_PROBE=slice: sky (and the y occupancy under it) across x
        // and y at the camera's z, the highest row first.
        if (std::string_view(std::getenv("LUNAR_LIGHT_PROBE")) == "slice")
            for (int dy = 6; dy >= -3; --dy) {
                std::string row;
                for (int dx = -14; dx <= 14; ++dx) {
                    const lunar::render::GridPoint v{at.x + dx, at.y + dy, at.z};
                    row += std::format(" {:3}/{:<3}", grid.sky(v), grid.occupancy(1, v));
                }
                std::println("light slice y{:+d}:{}", dy, row);
            }
    }
    if (scene.ribbons().beamCount() + scene.ribbons().trailCount() > 0)
        std::println("ribbons: {} beams and {} trails, {} and {} drawn, {} triangles", scene.ribbons().beamCount(),
                     scene.ribbons().trailCount(), scene.ribbons().beamsDrawn(), scene.ribbons().trailsDrawn(),
                     stats.effectTriangles);
    if (scene.lights().count() > 0)
        std::println("local lights: {} of {} shining, {} per pixel in {} cluster entries (Technology {})",
                     stats.localLights, scene.lights().count(), stats.pixelLights, stats.clusteredLights,
                     static_cast<int>(lighting.technology));
    if (lighting.post.any())
        std::println("post effects:{}{}", describePostLook(lighting.post), stats.postEffects ? "" : " (off)");
    std::println("GPU memory: {} MiB of a {} MiB budget, {} MiB of meshes", stats.deviceLocalBytes >> 20,
                 stats.memoryBudget >> 20, stats.meshBytes >> 20);
    std::println("textures: {} MiB of {} MiB, {} resident, quality level {}; {} allocations found the GPU full",
                 stats.textureBytes >> 20, stats.textureBudget >> 20, stats.texturesResident, stats.textureQuality,
                 stats.outOfMemory);
    if (const auto bounds = lunar::render::sceneBounds(items))
        std::println("scene from {:.0f},{:.0f},{:.0f} to {:.0f},{:.0f},{:.0f}", bounds->min.x, bounds->min.y,
                     bounds->min.z, bounds->max.x, bounds->max.y, bounds->max.z);
    return 0;
}

// Steps the scheduler at 60 Hz until nothing is waiting or time runs out.
void simulate(lunar::ScriptContext& context, double seconds)
{
    constexpr double frame = 1.0 / 60.0;
    while (context.time() < seconds && !context.idle())
        context.step(frame);
}

// Runs scripts against a game, printing their output. Fails if any script errored.
class ScriptRun {
public:
    explicit ScriptRun(std::shared_ptr<lunar::DataModel> game) : context_(std::move(game))
    {
        context_.setOutput([this](lunar::MessageType type, std::string_view message) {
            if (type == lunar::MessageType::Error)
                failed_ = true;
            const bool toStderr = type == lunar::MessageType::Warning || type == lunar::MessageType::Error;
            std::println(toStderr ? stderr : stdout, "{}", message);
        });
    }

    void run(std::string_view source, std::string_view name, lunar::Instance* script = nullptr)
    {
        report(context_.run(source, name, script));
    }

    void runScript(lunar::Instance& script) { report(context_.runScript(script)); }

    int finish(double seconds)
    {
        simulate(context_, seconds);
        return failed_ ? 1 : 0;
    }

private:
    void report(const std::expected<void, lunar::ScriptError>& result)
    {
        if (result)
            return;
        failed_ = true;
        std::println(stderr, "{}", result.error().message);
        if (!result.error().traceback.empty())
            std::print(stderr, "Stack Begin\n{}Stack End\n", result.error().traceback);
    }

    lunar::ScriptContext context_;
    bool failed_ = false;
};

int runFile(const std::filesystem::path& path, double seconds)
{
    auto source = lunar::readFile(path);
    if (!source) {
        std::println(stderr, "{}", source.error());
        return 1;
    }
    ScriptRun run(lunar::DataModel::create());
    run.run(*source, path.filename().string());
    return run.finish(seconds);
}

// Play Solo with the place's server and client scripts, a local player, and
// frames at 60 Hz for the given simulated time, then what `console` ran with
// the Luau tab's access and globals (ScriptContext::runConsole) once the place
// has settled. --console-run: how a console script is tried without a window.
// `realtime` paces the frames to the clock instead of running them flat out,
// for code that waits on something outside (game:HttpGet, request).
int consolePlace(const std::filesystem::path& path, const std::filesystem::path& scriptPath, double seconds,
                 bool realtime)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    auto code = lunar::readFile(scriptPath);
    if (!code) {
        std::println(stderr, "{}", code.error());
        return 1;
    }
    std::println("Playing {} for {} s, console running {}", path.filename().string(), seconds,
                 scriptPath.filename().string());
    lunar::PlaySession session(place->game);
    int errors = 0;
    session.setOutput([&errors](lunar::ScriptRole role, lunar::MessageType type, std::string_view message) {
        const char* side = role == lunar::ScriptRole::Server ? "server" : "client";
        if (type == lunar::MessageType::Error)
            ++errors;
        const bool toStderr = type == lunar::MessageType::Warning || type == lunar::MessageType::Error;
        std::println(toStderr ? stderr : stdout, "[{}] {}", side, message);
    });
    session.start();
    // The scripts of the place run first, so the console sees the game as it
    // runs it: Characters.Animate and CoreGui's modules are already there.
    constexpr double settle = 1.0;
    while (session.time() < settle)
        session.step(1.0 / 60.0);
    const std::string codeText(*code);
    if (auto result = session.client().runConsole(codeText, &session.server()); !result) {
        std::println(stderr, "{}", result.error().message);
        if (!result.error().traceback.empty())
            std::print(stderr, "Stack Begin\n{}Stack End\n", result.error().traceback);
        ++errors;
    }
    const auto frame = std::chrono::microseconds(16667);
    auto next = std::chrono::steady_clock::now();
    while (session.time() < seconds) {
        session.step(1.0 / 60.0);
        if (realtime) {
            next += frame;
            std::this_thread::sleep_until(next);
        }
    }
    std::println("{} error(s) after {} s", errors, session.time());
    return errors > 0 ? 1 : 0;
}

// Play Solo: the place's server and client scripts, a local player, and
// frames at 60 Hz for the given simulated time.
int runPlace(const std::filesystem::path& path, double seconds)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    std::println("Playing {} for {} s", path.filename().string(), seconds);
    lunar::PlaySession session(place->game);
    int errors = 0;
    session.setOutput([&errors](lunar::ScriptRole role, lunar::MessageType type, std::string_view message) {
        const char* side = role == lunar::ScriptRole::Server ? "server" : "client";
        if (type == lunar::MessageType::Error)
            ++errors;
        const bool toStderr = type == lunar::MessageType::Warning || type == lunar::MessageType::Error;
        std::println(toStderr ? stderr : stdout, "[{}] {}", side, message);
    });
    session.start();
    constexpr double frame = 1.0 / 60.0;
    while (session.time() < seconds)
        session.step(frame);
    std::println("{} error(s) after {} s", errors, session.time());
    return errors > 0 ? 1 : 0;
}

// A mesh from a file path or any MeshId form (rbxassetid://, rbxasset://, ...).
std::expected<lunar::assets::MeshFile, std::string> loadMesh(std::string_view source)
{
    lunar::assets::AssetCache cache;
    auto bytes = cache.fetch(source);
    if (!bytes)
        return std::unexpected(bytes.error());
    return lunar::assets::parseMesh(**bytes);
}

int meshInfo(std::string_view source)
{
    auto mesh = loadMesh(source);
    if (!mesh) {
        std::println(stderr, "{}: {}", source, mesh.error());
        return 1;
    }
    const auto v3 = [](lunar::Vector3 v) { return std::format("({:.4g}, {:.4g}, {:.4g})", v.x, v.y, v.z); };
    std::println("version {}", mesh->version);
    std::println("{} vertices, {} triangles", mesh->mesh.vertices.size(), mesh->mesh.indices.size() / 3);
    std::println("{} LOD(s), {} bone(s), {}", mesh->lodCount, mesh->boneCount, mesh->hasFacs ? "FACS" : "no FACS");
    for (const lunar::assets::UnionOperand& operand : mesh->operands) {
        static constexpr const char* kinds[] = {"block", "wedge", "corner wedge", "cylinder", "ball", "other"};
        const lunar::Vector3 p = operand.cframe.position, s = operand.size;
        std::println("  operand: {}{} at ({:.2f}, {:.2f}, {:.2f}), size ({:.2f}, {:.2f}, {:.2f})",
                     operand.negate ? "negate " : "", kinds[static_cast<int>(operand.kind)], p.x, p.y, p.z, s.x, s.y, s.z);
    }
    std::println("bounds {} to {}, size {}", v3(mesh->boundsMin), v3(mesh->boundsMax), v3(mesh->size()));
    return 0;
}

// Wavefront OBJ with vertex colours, for looking at a mesh in Blender & co.
int meshObj(std::string_view source, const std::filesystem::path& outPath)
{
    auto mesh = loadMesh(source);
    if (!mesh) {
        std::println(stderr, "{}: {}", source, mesh.error());
        return 1;
    }
    std::string obj = std::format("# {} (mesh version {})\n", source, mesh->version);
    for (const lunar::MeshVertex& v : mesh->mesh.vertices)
        obj += std::format("v {} {} {} {} {} {}\n", v.position.x, v.position.y, v.position.z, v.color.r, v.color.g,
                           v.color.b);
    // Roblox's V runs down the texture, OBJ's up.
    for (const lunar::MeshVertex& v : mesh->mesh.vertices)
        obj += std::format("vt {} {}\n", v.u, 1.0f - v.v);
    for (const lunar::MeshVertex& v : mesh->mesh.vertices)
        obj += std::format("vn {} {} {}\n", v.normal.x, v.normal.y, v.normal.z);
    const auto& indices = mesh->mesh.indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        obj += std::format("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\n", indices[i] + 1, indices[i + 1] + 1,
                           indices[i + 2] + 1);
    std::ofstream out(outPath, std::ios::binary);
    out.write(obj.data(), static_cast<std::streamsize>(obj.size()));
    if (!out) {
        std::println(stderr, "{}: can't write", outPath.string());
        return 1;
    }
    std::println("{} vertices, {} triangles -> {}", mesh->mesh.vertices.size(), indices.size() / 3, outPath.string());
    return 0;
}

// A texture from a file path or any content id form.
std::expected<lunar::assets::TextureFile, std::string> loadTexture(std::string_view source)
{
    lunar::assets::AssetCache cache;
    std::string id(source);
    for (int hop = 0; hop < 4; ++hop) {
        auto bytes = cache.fetch(id);
        if (!bytes)
            return std::unexpected(bytes.error());
        // Decal assets and TexturePacks point at the image.
        std::string next = lunar::assets::followImageIndirection(**bytes);
        if (next.empty())
            return lunar::assets::decodeTexture(**bytes, {.allMips = true, .allLayers = true});
        std::println("{} -> {}", id, next);
        id = std::move(next);
    }
    return std::unexpected("too many indirections");
}

int imageInfo(std::string_view source)
{
    auto texture = loadTexture(source);
    if (!texture) {
        std::println(stderr, "{}: {}", source, texture.error());
        return 1;
    }
    const lunar::Image& image = texture->image();
    std::println("{} {}, {} x {}", lunar::assets::imageFormatName(texture->format), texture->pixelFormat, image.width,
                 image.height);
    if (texture->layers > 1 || texture->fileMips > 1)
        std::println("{} {}{}, {} mip level(s)", texture->layers, texture->volume ? "depth slice(s)" : "layer(s)",
                     texture->cubemap ? " (cubemap)" : "", texture->fileMips);
    std::size_t opaque = 0;
    std::size_t transparent = 0;
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        opaque += image.rgba[i] == 255 ? 1u : 0u;
        transparent += image.rgba[i] == 0 ? 1u : 0u;
    }
    const std::size_t pixels = std::size_t{image.width} * image.height;
    std::println("alpha: {} opaque, {} transparent, {} partial", opaque, transparent, pixels - opaque - transparent);
    return 0;
}

// A sound's format and length, fetched (a file or an asset uri, for a place
// when one's given) and decoded.
int soundInfo(std::string_view source, std::uint64_t placeId)
{
    lunar::assets::AssetCache cache;
    cache.setPlaceId(placeId);
    auto bytes = std::filesystem::exists(source) ? lunar::readFile(std::filesystem::path(source))
                                                     .transform([](std::string b) {
                                                         return std::make_shared<const std::string>(std::move(b));
                                                     })
                                               : cache.fetch(source);
    if (!bytes) {
        std::println(stderr, "{}: {}", source, bytes.error());
        return 1;
    }
    auto clip = lunar::audio::decodeAudio(**bytes);
    if (!clip) {
        std::println(stderr, "{}: {}", source, clip.error());
        return 1;
    }
    float peak = 0.0f;
    for (const std::int16_t s : clip->samples)
        peak = std::max(peak, std::abs(static_cast<float>(s)) / 32768.0f);
    std::println("{} Hz, {} channel(s), {:.2f} s, peak {:.2f}, {} bytes", clip->sampleRate, clip->channels,
                 clip->seconds(), peak, (*bytes)->size());
    return 0;
}

// Re-encoded as PNG for looking at; layers and mips go side by side.
// Writes every script's Source under outDir, one file per script named
// after its full name, for reading what a place does.
int dumpScripts(const std::filesystem::path& path, const std::filesystem::path& outDir)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(outDir, error);
    int written = 0;
    for (const lunar::InstancePtr& instance : place->game->descendants()) {
        const auto* script = dynamic_cast<const lunar::LuaSourceContainer*>(instance.get());
        if (!script)
            continue;
        std::string name = std::format("{}.{}.luau", instance->fullName(), instance->className());
        std::ranges::replace_if(name, [](char c) { return c == '/' || c == '\\'; }, '_');
        std::ofstream out(outDir / name, std::ios::binary);
        out << script->source();
        if (!out) {
            std::println(stderr, "{}: can't write", (outDir / name).string());
            return 1;
        }
        ++written;
    }
    std::println("{} scripts -> {}", written, outDir.string());
    return 0;
}

int imagePng(std::string_view source, const std::filesystem::path& outPath)
{
    auto texture = loadTexture(source);
    if (!texture) {
        std::println(stderr, "{}: {}", source, texture.error());
        return 1;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::uint32_t layer = 0; layer < texture->layers; ++layer) {
        std::uint32_t layerWidth = 0;
        for (std::uint32_t mip = 0; mip < texture->mips; ++mip)
            layerWidth += texture->image(layer, mip).width;
        width = std::max(width, layerWidth);
        height += texture->image(layer).height;
    }
    lunar::render::PixelImage out;
    out.width = width;
    out.height = height;
    out.pixels.resize(std::size_t{width} * height);
    std::uint32_t y0 = 0;
    for (std::uint32_t layer = 0; layer < texture->layers; ++layer) {
        std::uint32_t x0 = 0;
        for (std::uint32_t mip = 0; mip < texture->mips; ++mip) {
            const lunar::Image& image = texture->image(layer, mip);
            for (std::uint32_t y = 0; y < image.height; ++y)
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    const std::uint8_t* p = image.rgba.data() + (std::size_t{y} * image.width + x) * 4;
                    out.pixels[std::size_t{y0 + y} * width + x0 + x] = {p[0], p[1], p[2], p[3]};
                }
            x0 += image.width;
        }
        y0 += texture->image(layer).height;
    }
    const std::string png = lunar::render::encodePng(out);
    std::ofstream file(outPath, std::ios::binary);
    file.write(png.data(), static_cast<std::streamsize>(png.size()));
    if (!file) {
        std::println(stderr, "{}: can't write", outPath.string());
        return 1;
    }
    std::println("{} x {} -> {}", width, height, outPath.string());
    return 0;
}


// --play's map flags (see usage); true when args[i] was one, i past its value.
bool parseMapFlag(std::span<const std::string_view> args, std::size_t& i, lunar::ui::MapSettings& map)
{
    const std::string_view arg = args[i];
    const bool hasValue = i + 1 < args.size();
    if (arg == "--no-minimap") {
        map.minimap = false;
    } else if (arg == "--minimap-corner" && hasValue) {
        const std::string_view corner = args[++i];
        using Corner = lunar::ui::MapSettings::Corner;
        map.corner = corner == "tl"   ? Corner::TopLeft
                     : corner == "tr" ? Corner::TopRight
                     : corner == "bl" ? Corner::BottomLeft
                                      : Corner::BottomRight;
    } else if (arg == "--minimap-size" && hasValue) {
        map.minimapSize = std::clamp(std::stof(std::string(args[++i])), 60.0f, 600.0f);
    } else if (arg == "--minimap-span" && hasValue) {
        map.minimapSpan = std::max(std::stof(std::string(args[++i])), lunar::ui::WorldMap::minMinimapSpan);
    } else if (arg == "--map-key" && hasValue) {
        const std::string_view name = args[++i];
        map.fullMapKey.reset();
        for (int k = 0; k < 256 && name != "none"; ++k) {
            const auto key = static_cast<lunar::platform::Key>(k);
            if (lunar::platform::keyName(key) == name) {
                map.fullMapKey = key;
                break;
            }
        }
    } else {
        return false;
    }
    return true;
}

int usage()
{
    std::println(stderr, "usage: lunarstone [place.rbxl(x) | model.rbxm(x)]");
    std::println(stderr, "       lunarstone --window");
    std::println(stderr, "       lunarstone --view <place>");
    std::println(stderr, "       lunarstone --play <place> [--no-scripts] [--shift-lock] [--players N]");
    std::println(stderr, "                  [--graphics potato|minimal|average|max] [--render-scale 25-100] [--raytracing]\n"
                 "                  [--reflect-everything] [--ambient-occlusion] [--shadow-all-lights]");
    std::println(stderr, "                  [--no-vsync] [--fps-cap N]");
    std::println(stderr, "                  [--seconds N] [--screenshot seconds:out.png]...");
    std::println(stderr, "                  [--no-minimap] [--minimap-corner tl|tr|bl|br] [--minimap-size px]");
    std::println(stderr, "                  [--minimap-span studs] [--map-key KEY|none]");
    std::println(stderr, "                  [--user id|name | --me]");
    std::println(stderr, "                  [--api [port]] (the Luau tab over HTTP on 127.0.0.1:34890 or that port;");
    std::println(stderr, "                  its token is in ~/.config/lunarstone/api-token; see app/ConsoleApi.hpp)");
    std::println(stderr, "       lunarstone --simulate <place> [--seconds N] [--scripts] [--idle] [--players N]");
    std::println(stderr, "                  [--user id|name | --me]");
    std::println(stderr, "                  [--give tool] [--gogogo] [--drive] [--map]");
    std::println(stderr, "       lunarstone --tree-test <place> [part name prefix]");
    std::println(stderr, "       lunarstone --props <place> <instance name>");
    std::println(stderr,
                 "       lunarstone --render <place> <out.png|out.ppm> [width height] [--look ex,ey,ez,tx,ty,tz]");
    std::println(stderr, "                  [--clock hours] [--seconds game time] [--bench frames] [--quality 1-21]");
    std::println(stderr, "                  [--graphics potato|minimal|average|max] [--render-scale 25-100] [--raytracing]\n"
                 "                  [--reflect-everything] [--ambient-occlusion] [--shadow-all-lights]");
    std::println(stderr, "       lunarstone --ui-test <place> <out.png> [width height]");
    std::println(stderr, "                  [--no-scene] [--chat-demo] [--menu] [--player] [--tools]");
    std::println(stderr, "                  [--look ex,ey,ez,tx,ty,tz]");
    std::println(stderr, "       lunarstone --export-terrain <place> <out.obj> [--preview out.png]");
    std::println(stderr, "       lunarstone --run place.rbxl(x) [--seconds N]");
    std::println(stderr, "       lunarstone --run-file script.luau [--seconds N]");
    std::println(stderr, "       lunarstone --console-run <place> <file.luau> [--seconds N] [--realtime]");
    std::println(stderr, "                  (plays the place headless, runs the file in the Luau tab's console;");
    std::println(stderr, "                  --realtime paces the frames to the clock, for code that waits on the web)");
    std::println(stderr, "       lunarstone --playtest <place> <test.luau> [--out <folder>] [--size <w> <h>]");
    std::println(stderr, "                  [--timeout <s>] [--players <n>] [--seed <n>] [--allow-errors]");
    std::println(stderr, "                  [--graphics ...] (Play Solo without a window, driven by a test in Luau)");
    std::println(stderr, "       lunarstone --dump-scripts <place> <out dir>");
    std::println(stderr, "       lunarstone --lua-app <client resources folder> [--seconds N] [--screenshot seconds:out.png]... [--control <file>]\n"
                 "                  [--game-seconds N] [--game-screenshot seconds:out.png]...\n"
                 "                  [--headless | --render out.png [--size w h] [--click seconds:x,y]...]");
    std::println(stderr, "                  (Roblox's own Lua app, its home screen, from the client's ExtraContent;\n"
                 "                  --game-* go to the games it launches)");
    std::println(stderr, "       lunarstone --download-place <place id> <out.rbxl>");
    std::println(stderr, "       lunarstone --mesh-info <file | asset uri>");
    std::println(stderr, "       lunarstone --mesh-obj <file | asset uri> <out.obj>");
    std::println(stderr, "       lunarstone --image-info <file | asset uri>");
    std::println(stderr, "       lunarstone --sound-info <file | asset uri> [place id]");
    std::println(stderr, "       lunarstone --image-png <file | asset uri> <out.png>");
    std::println(
        stderr,
        "       lunarstone --avatar-render <out.png> [--r6 | --r15] [--anim name | --speed studs/s] [--time t]");
    std::println(stderr, "                  [--yaw degrees] [--size width height] [--user id|name | --me]");
    std::println(stderr, "       lunarstone --avatar-info <user id | username | --me>");
    std::println(stderr,
                 "       lunarstone --net-decode <capture.pcap|.pcapng|hex dump> [--keylog file] [--hex] [--plain]");
    std::println(stderr, "       lunarstone --net-server <place> [--port N] [--seconds N]");
    std::println(stderr, "       lunarstone --net-client <host:port> [--seconds N]");
    std::println(stderr, "       lunarstone --net-loopback <place> [--seconds N]");
    std::println(stderr, "       lunarstone --net-join <placeId | --private <link>> [--seconds N] [--out <prefix>] [--attempts N] [--platform linux|mac] [--scripts [--script-platform <Enum.Platform name>] [--fps N|vsync] [--equip <slot> [--activate] [--unequip] [--tool-at seconds]] [--say text | --say-preset phrase [--say-after seconds]]] [--walk square|line|n3,e3,j,...] [--render out.png [--graphics ...] [--raytracing]] [--focus x,y,z[@seconds] ...] [--stream-out studs]");
    std::println(stderr, "                  (LUNAR_NET_LOG=1|2 prints the traffic, LUNAR_NET_PCAP=<file> captures it,");
    std::println(stderr, "                  LUNAR_NET_KEYLOG=<file> logs the session keys for --net-decode)");
    std::println(stderr, "       lunarstone --net-play <placeId | --private <link> | --replay <prefix>> [--job <JobId>] [--seconds N] [--fps N|vsync]");
    std::println(stderr, "                  [--graphics potato|minimal|average|max] [--render-scale 25-100] [--raytracing]");
    std::println(stderr, "                  [--attempts N] [--platform linux|mac] [--script-platform <Enum.Platform name>]");
    std::println(stderr, "                  [--out <prefix>] [--minimap] [--screenshot seconds:out.png]... [--screenshot-on <file>]");
    std::println(stderr, "                  [--control <file>] (input lines from a file: key W 1.5, click x y, look dx dy, ...)");
    std::println(stderr, "                  [--bot <file.luau>] (console Luau that plays: keypress, mouse1click, mousemoverel, ...)");
    std::println(stderr, "                  [--shift-lock] (the menu's Shift Lock Switch on)");
    std::println(stderr, "                  [--api [port]] (the Luau tab over HTTP, as for --play)");
    std::println(stderr, "                  [--headless] (no window, GPU or sound output: the scripts, the GUI's "
                         "layout, the sounds and the network, for --bot)");
    std::println(stderr, "                  (plays the game in a window: joins like --net-join --scripts, or plays a");
    std::println(stderr, "                  recording made with --out back in real time; Esc's Leave or closing it leaves)");
    std::println(stderr, "       lunarstone --net-replay <prefix> [--run seconds] [--timed [--sound [--mute]]] [--render out.png]");
    std::println(stderr, "                  [--bench-replay frames [--bench-warmup frames] [--bench-size WxH] [--bench-at seconds]");
    std::println(stderr, "                  [--bench-out file.json]] [--fps N]");
    std::println(stderr, "                  [--graphics potato|minimal|average|max] [--render-scale 25-100] [--raytracing]");
    std::println(stderr, "                  (--bench-replay plays a recording back at --fps with the client's scripts and");
    std::println(stderr, "                  draws and times every frame offscreen; the run before the measurement is reported apart)");
    std::println(stderr, "any of them: --cookie-file <path> signs in with the .ROBLOSECURITY cookie in that file");
    std::println(stderr,
                 "             (mode 0600 or stricter; default ~/.config/lunarstone/cookie or LUNAR_ROBLOSECURITY)");
    return 2;
}

// The local player for --play/--simulate: a real user's name, id and
// avatar when --user or --me was given.
std::expected<LocalUser, std::string> localUser(const UserChoice& choice)
{
    if (!choice.chosen())
        return LocalUser{};
    auto chosen = resolveUser(choice);
    if (!chosen)
        return std::unexpected(chosen.error());
    std::println("playing as {} (@{}, user {})", chosen->user.displayName, chosen->user.name, chosen->user.id);
    return LocalUser{.name = chosen->user.name,
                     .displayName = chosen->user.displayName,
                     .userId = chosen->user.id,
                     .avatar = chosen->avatar};
}

int run(int argc, char** argv)
{
    std::vector<std::string_view> args(argv + 1, argv + argc);
    // --cookie-file goes with any command; the file is checked now, so a
    // bad one stops here rather than at the first download that needs it.
    if (auto flag = std::ranges::find(args, std::string_view("--cookie-file")); flag != args.end()) {
        if (flag + 1 == args.end())
            return usage();
        const std::filesystem::path cookieFile(*(flag + 1));
        args.erase(flag, flag + 2);
        if (auto login = lunar::assets::configureLogin(cookieFile); !login) {
            std::println(stderr, "{}", login.error());
            return 1;
        }
    }
    if (!args.empty() && args[0] == "--window")
        return showWindow();
    if (!args.empty() && args[0].starts_with("--net-")) {
        const int code = runNetCommand(args);
        return code < 0 ? usage() : code;
    }
    if (args.size() == 2 && args[0] == "--view")
        return viewPlace(args[1]);
    if (args.size() >= 2 && args[0] == "--play") {
        PlayOptions options;
        UserChoice user;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (parseUserFlag(args, i, user))
                continue;
            std::string graphicsError;
            if (parseGraphicsFlag(args, i, options.graphics, graphicsError)) {
                if (!graphicsError.empty()) {
                    std::println(stderr, "{}", graphicsError);
                    return 2;
                }
                continue;
            }
            if (std::string apiError; parseApiFlag(args, i, options.api, apiError)) {
                if (!apiError.empty()) {
                    std::println(stderr, "{}", apiError);
                    return 2;
                }
                continue;
            }
            if (args[i] == "--no-scripts")
                options.runScripts = false;
            else if (args[i] == "--shift-lock")
                options.shiftLock = true;
            else if (args[i] == "--no-vsync")
                options.vsync = false;
            else if (args[i] == "--mute")
                options.mute = true; // no sound output
            else if (args[i] == "--fps-cap" && i + 1 < args.size())
                options.fpsCap = std::max(0, std::stoi(std::string(args[++i])));
            else if (args[i] == "--players" && i + 1 < args.size())
                options.players = std::max(1, std::stoi(std::string(args[++i])));
            else if (args[i] == "--seconds" && i + 1 < args.size())
                options.seconds = std::stod(std::string(args[++i])); // closes the window after that long
            else if (args[i] == "--screenshot" && i + 1 < args.size()) {
                auto shot = parseScreenshot(args[++i]); // a picture of the window, "seconds:out.png"
                if (!shot)
                    return usage();
                options.screenshots.push_back(std::move(*shot));
            } else if (!parseMapFlag(args, i, options.map))
                return usage();
        }
        auto local = localUser(user);
        if (!local) {
            std::println(stderr, "{}", local.error());
            return 1;
        }
        options.user = std::move(*local);
        return playPlace(args[1], options);
    }
    if (args.size() >= 2 && args[0] == "--simulate") {
        SimulateOptions options;
        UserChoice user;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (parseUserFlag(args, i, user))
                continue;
            if (args[i] == "--seconds" && i + 1 < args.size())
                options.seconds = std::stof(std::string(args[++i]));
            else if (args[i] == "--scripts")
                options.scripts = true;
            else if (args[i] == "--idle")
                options.idle = true;
            else if (args[i] == "--players" && i + 1 < args.size())
                options.players = std::max(1, std::stoi(std::string(args[++i])));
            else if (args[i] == "--give" && i + 1 < args.size())
                options.give = args[++i];
            else if (args[i] == "--map")
                options.map = true;
            else if (args[i] == "--gogogo")
                options.goGoGo = true;
            else if (args[i] == "--drive")
                options.drive = true;
            else
                return usage();
        }
        auto local = localUser(user);
        if (!local) {
            std::println(stderr, "{}", local.error());
            return 1;
        }
        options.user = std::move(*local);
        return simulatePlace(args[1], options);
    }
    if (args.size() == 3 && args[0] == "--download-place")
        return downloadPlace(args[1], args[2]);
    if (args.size() == 3 && args[0] == "--dump-scripts")
        return dumpScripts(args[1], args[2]);
    if ((args.size() == 2 || args.size() == 3) && args[0] == "--tree-test")
        return treeTest(args[1], args.size() == 3 ? args[2] : "tree");
    if (args.size() == 3 && args[0] == "--props")
        return showProperties(args[1], args[2]);
    if (args.size() == 2 && args[0] == "--mesh-info")
        return meshInfo(args[1]);
    if (args.size() == 3 && args[0] == "--mesh-obj")
        return meshObj(args[1], args[2]);
    if (args.size() == 2 && args[0] == "--image-info")
        return imageInfo(args[1]);
    if ((args.size() == 2 || args.size() == 3) && args[0] == "--sound-info")
        return soundInfo(args[1], args.size() == 3 ? std::stoull(std::string(args[2])) : 0);
    if (args.size() == 3 && args[0] == "--image-png")
        return imagePng(args[1], args[2]);
    if (args.size() >= 2 && args[0] == "--avatar-info") {
        UserChoice user;
        for (std::size_t i = 1; i < args.size(); ++i)
            if (!parseUserFlag(args, i, user))
                user.user = args[i];
        if (!user.chosen())
            return usage();
        return avatarInfo(user);
    }
    if (args.size() >= 2 && args[0] == "--avatar-render") {
        AvatarRenderOptions options;
        UserChoice user;
        for (std::size_t i = 2; i < args.size(); ++i) {
            const bool hasValue = i + 1 < args.size();
            if (parseUserFlag(args, i, user))
                continue;
            if (args[i] == "--r6")
                options.rig = lunar::Enum::HumanoidRigType::R6;
            else if (args[i] == "--r15")
                options.rig = lunar::Enum::HumanoidRigType::R15;
            else if (args[i] == "--anim" && hasValue)
                options.anim = args[++i];
            else if (args[i] == "--speed" && hasValue)
                options.speed = std::stof(std::string(args[++i]));
            else if (args[i] == "--time" && hasValue)
                options.time = std::stof(std::string(args[++i]));
            else if (args[i] == "--yaw" && hasValue)
                options.yaw = std::stof(std::string(args[++i]));
            else if (args[i] == "--size" && i + 2 < args.size()) {
                options.width = static_cast<std::uint32_t>(std::stoul(std::string(args[++i])));
                options.height = static_cast<std::uint32_t>(std::stoul(std::string(args[++i])));
            } else
                return usage();
        }
        if (user.chosen()) {
            auto chosen = resolveUser(user);
            if (!chosen) {
                std::println(stderr, "{}", chosen.error());
                return 1;
            }
            std::println("{} (@{}, user {})", chosen->user.displayName, chosen->user.name, chosen->user.id);
            options.avatar = chosen->avatar;
        }
        return renderAvatar(args[1], options);
    }
    if (!args.empty() && args[0] == "--render") {
        std::vector<std::string_view> positional;
        RenderOptions options;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const bool hasValue = i + 1 < args.size();
            std::string graphicsError;
            if (parseGraphicsFlag(args, i, options.graphics, graphicsError)) {
                if (!graphicsError.empty()) {
                    std::println(stderr, "{}", graphicsError);
                    return 2;
                }
                continue;
            }
            if (args[i] == "--look" && hasValue) {
                options.look = parseLook(args[++i]);
                if (!options.look)
                    return usage();
            } else if (args[i] == "--clock" && hasValue) {
                options.clockTime = std::stod(std::string(args[++i]));
            } else if (args[i] == "--seconds" && hasValue) {
                options.seconds = std::stod(std::string(args[++i]));
            } else if (args[i] == "--bench" && hasValue) {
                options.benchFrames = std::stoi(std::string(args[++i]));
            } else if (args[i] == "--quality" && hasValue) {
                options.qualityLevel = std::stoi(std::string(args[++i]));
            } else if (args[i] == "--gui") {
                options.gui = true; // StarterGui, billboards and surface GUIs on top
            } else {
                positional.push_back(args[i]);
            }
        }
        if (positional.size() != 2 && positional.size() != 4)
            return usage();
        if (positional.size() == 4) {
            options.width = static_cast<std::uint32_t>(std::stoul(std::string(positional[2])));
            options.height = static_cast<std::uint32_t>(std::stoul(std::string(positional[3])));
        }
        return renderFile(positional[0], positional[1], options);
    }
    if (!args.empty() && args[0] == "--ui-test")
        return uiTest(std::span(args).subspan(1));
    if (!args.empty() && args[0] == "--export-terrain") {
        const bool withPreview = args.size() == 5 && args[3] == "--preview";
        if (args.size() != 3 && !withPreview)
            return usage();
        return exportTerrain(args[1], args[2],
                             withPreview ? std::optional<std::filesystem::path>(args[4]) : std::nullopt);
    }
    if (!args.empty() && (args[0] == "--run" || args[0] == "--run-file")) {
        const bool withSeconds = args.size() == 4 && args[2] == "--seconds";
        if (args.size() != 2 && !withSeconds)
            return usage();
        const double seconds = withSeconds ? std::stod(std::string(args[3])) : 5.0;
        return args[0] == "--run" ? runPlace(args[1], seconds) : runFile(args[1], seconds);
    }
    if (!args.empty() && args[0] == "--lua-app") {
        const int code = runLuaApp(args);
        return code < 0 ? usage() : code;
    }
    if (args.size() >= 3 && args[0] == "--playtest") {
        // A test in Luau plays the place without a window (Playtest.hpp).
        PlaytestOptions options;
        options.test = args[2];
        UserChoice user;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (parseUserFlag(args, i, user) || parsePlaytestFlag(args, i, options))
                continue;
            std::string graphicsError;
            if (parseGraphicsFlag(args, i, options.graphics, graphicsError)) {
                if (!graphicsError.empty()) {
                    std::println(stderr, "{}", graphicsError);
                    return 2;
                }
                continue;
            }
            return usage();
        }
        auto local = localUser(user);
        if (!local) {
            std::println(stderr, "{}", local.error());
            return 2;
        }
        options.user = std::move(*local);
        return playtestPlace(args[1], options);
    }
    if (!args.empty() && args[0] == "--console-run") {
        // --console-run <place> <file.luau> [--seconds N] [--realtime]
        const bool realtime = args.size() > 3 && args.back() == "--realtime";
        const std::size_t count = args.size() - (realtime ? 1 : 0);
        const bool withSeconds = count == 5 && args[3] == "--seconds";
        if (count != 3 && !withSeconds)
            return usage();
        const double seconds = withSeconds ? std::stod(std::string(args[4])) : 3.0;
        return consolePlace(args[1], args[2], seconds, realtime);
    }
    if (!args.empty())
        return showFile(args[0]);

    auto game = lunar::DataModel::create();

    auto tower = lunar::ClassRegistry::create("Model");
    tower->setName("Tower");
    tower->setParent(game->workspace());

    for (int floor = 0; floor < 3; ++floor) {
        auto part = std::make_shared<lunar::Part>();
        part->setName(std::format("Floor{}", floor + 1));
        part->setPosition({0.0f, static_cast<float>(floor), 0.0f});
        part->setAnchored(true);
        part->setParent(tower);
    }

    printTree(*game);

    std::println("\nProperties of {}:", tower->findFirstChild("Floor2")->fullName());
    const lunar::Instance& floor = *tower->findFirstChild("Floor2");
    for (const lunar::Property* property : floor.properties())
        std::println("  {:<13} {:<9} {}{}", property->name, property->type, lunar::toString(property->get(floor)),
                     property->readOnly() ? "  (read-only)" : "");
    return 0;
}

} // namespace

// Usage: lunarstone [place.rbxl(x) | model.rbxm(x)]
//        lunarstone --window
//        lunarstone --view <place>
//        lunarstone --play <place> [--no-scripts] [--shift-lock] [--players N] [--user id|name | --me]
//        lunarstone --simulate <place> [--seconds N] [--scripts] [--idle] [--players N] [--gogogo] [--drive] [--map]
//                   [--user id|name | --me]
//        lunarstone --tree-test <place> [part name prefix]
//        lunarstone --props <place> <instance name>
//        lunarstone --render <place> <out.png|out.ppm> [width height] [--look ...] [--clock h] [--bench n] [--gui]
//                   [--quality level]
//        lunarstone --export-terrain <place> <out.obj> [--preview out.png]
//        lunarstone --run place.rbxl(x) [--seconds N]
//        lunarstone --run-file script.luau [--seconds N]
//        lunarstone --download-place <place id> <out.rbxl>
//        lunarstone --mesh-info <file | asset uri>
//        lunarstone --mesh-obj <file | asset uri> <out.obj>
//        lunarstone --image-info <file | asset uri>
//        lunarstone --image-png <file | asset uri> <out.png>
//        lunarstone --avatar-render <out.png> [--r6] [--anim name | --speed s] [--time t] [--yaw deg] [--size w h]
//                   [--user id|name | --me]
//        lunarstone --avatar-info <user id | username | --me>
// Any of them takes --cookie-file <path> to sign in (see lunar/assets/Web.hpp).
int main(int argc, char** argv)
{
    try {
        // Every enum of the client for the decoders, before any capture or
        // join decodes a value of one the engine doesn't model.
        lunar::installEnumCatalog();
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}

namespace {

// Where Roblox's default camera sits for the local player right after the
// character spawns: PlayerCamera (the PlayerModule's ClassicCamera, Poppercam
// included) with the character's Humanoid as the subject, 12.5 studs behind
// and 15 degrees down. Nothing when the player has no character.
std::optional<std::pair<lunar::Vector3, lunar::Vector3>> playerCameraLook(lunar::DataModel& game,
                                                                          std::int64_t userId)
{
    auto* players = game.findService("Players");
    lunar::Workspace* workspace = game.workspace();
    if (!players || !workspace)
        return std::nullopt;
    lunar::Instance* character = nullptr;
    for (const auto& child : players->children())
        if (auto* player = dynamic_cast<lunar::Player*>(child.get()); player && player->userId() == userId) {
            character = player->character();
            if (!character)
                character = workspace->findFirstChild(player->name());
            break;
        }
    auto* humanoid = character ? static_cast<lunar::Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (!humanoid)
        return std::nullopt;
    lunar::physics::World world(*workspace);
    world.refresh();
    lunar::Camera& camera = *workspace->ensureCurrentCamera();
    lunar::physics::PlayerCamera playerCamera(world, camera);
    playerCamera.setSubject(humanoid);
    playerCamera.update(1.0f / 60.0f, {});
    const lunar::CFrame frame = camera.cframe();
    std::println("render: through {}'s camera", character->name());
    return std::pair{frame.position, frame.position + frame.lookVector()};
}

} // namespace

// --graphics potato|minimal|average|max, --render-scale percent, --raytracing,
// --reflect-everything, --ambient-occlusion and --shadow-all-lights, for --play, --render, --net-join and --net-replay: true when args[i] was one (i past its value); error says what's
// wrong with a bad value.
bool parseGraphicsFlag(std::span<const std::string_view> args, std::size_t& i,
                       lunar::render::GraphicsSettings& graphics, std::string& error)
{
    const std::string_view arg = args[i];
    if (arg == "--raytracing") {
        graphics.rayTracing = true;
        return true;
    }
    if (arg == "--reflect-everything") {
        graphics.rayTracing = true;
        graphics.reflectEverything = true;
        return true;
    }
    if (arg == "--ambient-occlusion") {
        graphics.rayTracing = true;
        graphics.ambientOcclusion = true;
        return true;
    }
    if (arg == "--shadow-all-lights") {
        graphics.rayTracing = true;
        graphics.shadowAllLights = true;
        return true;
    }
    if (i + 1 >= args.size() || (arg != "--graphics" && arg != "--render-scale"))
        return false;
    const std::string_view value = args[++i];
    if (arg == "--graphics") {
        if (value == "raytracing") {
            error = "ray tracing is a flag of its own: --raytracing, with any --graphics";
            return true;
        }
        const auto preset = lunar::render::parseGraphicsPreset(value);
        if (!preset) {
            error = std::format("--graphics {}: minimal, average, max or potato", value);
            return true;
        }
        // The render scale and ray tracing are settings of their own; a preset keeps them.
        const float scale = graphics.renderScale;
        const bool rayTracing = graphics.rayTracing;
        const bool reflectEverything = graphics.reflectEverything;
        const bool ambientOcclusion = graphics.ambientOcclusion;
        const bool shadowAllLights = graphics.shadowAllLights;
        graphics = lunar::render::graphicsSettings(*preset);
        graphics.renderScale = scale;
        graphics.rayTracing = rayTracing;
        graphics.reflectEverything = reflectEverything;
        graphics.ambientOcclusion = ambientOcclusion;
        graphics.shadowAllLights = shadowAllLights;
        return true;
    }
    int percent = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), percent);
    if (ec != std::errc{} || end != value.data() + value.size() || percent < 25 || percent > 100) {
        error = std::format("--render-scale {}: a percentage from 25 to 100", value);
        return true;
    }
    graphics.renderScale = static_cast<float>(percent) / 100.0f;
    return true;
}

// For --net-join --render: the replica through the local player's camera,
// or framed whole like --render frames a place when there's no character.
int renderReplica(const std::shared_ptr<lunar::DataModel>& game, const std::filesystem::path& outPath,
                  std::int64_t localUserId, const lunar::render::GraphicsSettings& graphics)
{
    // The player with our UserId is the local one also without --scripts:
    // the GUI drawn over the picture is its PlayerGui.
    if (auto* players = dynamic_cast<lunar::Players*>(game->findService("Players")); players && !players->localPlayer())
        for (const auto& child : players->children())
            if (auto* player = dynamic_cast<lunar::Player*>(child.get()); player && player->userId() == localUserId)
                players->assignLocalPlayer(player);
    // research: LUNAR_NET_LOOK="ex,ey,ez,tx,ty,tz" aims the camera, as --render's --look does;
    // several looks separated by '|' give one image each (out_1.png, out_2.png ...).
    // "@camera" is the default view, "@overview" the whole replica, "@moving"
    // the fastest of the characters the server moves, "@sun" towards the sun.
    const char* env = std::getenv("LUNAR_NET_LOOK");
    if (!env || !*env)
        return renderGame(game, outPath,
                          RenderOptions{.look = playerCameraLook(*game, localUserId), .graphics = graphics, .gui = true});
    std::vector<std::string> looks;
    for (std::string_view rest(env); !rest.empty();) {
        const auto bar = rest.find('|');
        looks.emplace_back(rest.substr(0, bar));
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    int code = 0;
    // "@character": a few studs from the first character (a Workspace model with a
    // Humanoid and a HumanoidRootPart), looking at it.
    auto characterLook = [&]() -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        auto* ws = game->findFirstChildOfClass("Workspace");
        if (!ws)
            return std::nullopt;
        for (const auto& c : ws->children())
            if (c->isA("Model") && c->findFirstChildOfClass("Humanoid"))
                if (auto* root = c->findFirstChild("HumanoidRootPart")) {
                    const auto p = std::get<lunar::Vector3>(root->get("Position"));
                    std::println("render: aiming at {}", c->name());
                    return std::pair{p + lunar::Vector3{7, 3, 9}, p};
                }
        return std::nullopt;
    };
    // "@union": the first union in Workspace, from a distance its size picks.
    auto unionLook = [&]() -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        auto* ws = game->findFirstChildOfClass("Workspace");
        if (!ws)
            return std::nullopt;
        for (const auto& d : ws->descendants())
            if (d->isA("PartOperation")) {
                const auto p = std::get<lunar::Vector3>(d->get("Position"));
                const auto size = std::get<lunar::Vector3>(d->get("Size"));
                const float r = std::max({size.x, size.y, size.z, 2.0f});
                std::println("render: aiming at {}", d->fullName());
                return std::pair{p + lunar::Vector3{1.2f * r, 0.8f * r, 1.5f * r}, p};
            }
        return std::nullopt;
    };
    // "@beam" (or "@beam3", the fourth): a Beam that shows, both its
    // attachments in Workspace, from the side at a distance its length picks.
    auto beamLook = [&](std::string_view which) -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        auto* ws = game->workspace();
        if (!ws)
            return std::nullopt;
        int skip = 0;
        std::from_chars(which.data(), which.data() + which.size(), skip);
        for (const auto& d : ws->descendants()) {
            const auto* beam = dynamic_cast<const lunar::Beam*>(d.get());
            if (!beam || !beam->enabled() || !beam->attachment0() || !beam->attachment1()
                || !beam->attachment0()->isDescendantOf(ws) || !beam->attachment1()->isDescendantOf(ws))
                continue;
            if (skip-- > 0)
                continue;
            const lunar::Vector3 a = beam->attachment0()->worldPosition();
            const lunar::Vector3 b = beam->attachment1()->worldPosition();
            const lunar::Vector3 middle = (a + b) * 0.5f;
            const float r = std::max({(b - a).magnitude() * 0.5f, beam->width0(), beam->width1(), 2.0f});
            std::println("render: aiming at {}", beam->fullName());
            return std::pair{middle + lunar::Vector3{1.2f * r, 0.8f * r, 1.5f * r}, middle};
        }
        return std::nullopt;
    };
    // "@head": the first character's head, face on.
    auto headLook = [&]() -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        auto* ws = game->findFirstChildOfClass("Workspace");
        if (!ws)
            return std::nullopt;
        for (const auto& c : ws->children())
            if (c->isA("Model") && c->findFirstChildOfClass("Humanoid"))
                if (auto* head = c->findFirstChild("Head")) {
                    const auto frame = std::get<lunar::CFrame>(head->get("CFrame"));
                    std::println("render: aiming at {}", head->fullName());
                    return std::pair{frame.position + frame.lookVector() * 4.0f + lunar::Vector3{0, 0.5f, 0},
                                     frame.position};
                }
        return std::nullopt;
    };
    // "@moving": the fastest moving character but ours (the server's physics
    // moves the others), a few studs from its side.
    auto movingLook = [&]() -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        auto* ws = game->findFirstChildOfClass("Workspace");
        auto* players = dynamic_cast<lunar::Players*>(game->findFirstChildOfClass("Players"));
        const lunar::Model* ours = players && players->localPlayer() ? players->localPlayer()->character() : nullptr;
        const lunar::BasePart* best = nullptr;
        float fastest = -1.0f;
        for (const auto& c : ws ? ws->children() : std::vector<lunar::InstancePtr>{})
            if (c.get() != ours && c->isA("Model") && c->findFirstChildOfClass("Humanoid"))
                if (const auto* root = dynamic_cast<const lunar::BasePart*>(c->findFirstChild("HumanoidRootPart"));
                    root && root->assemblyLinearVelocity().magnitude() > fastest) {
                    best = root;
                    fastest = root->assemblyLinearVelocity().magnitude();
                }
        if (!best)
            return std::nullopt;
        std::println("render: aiming at {}, {:.1f} studs/s", best->parent()->name(), fastest);
        const lunar::CFrame frame = best->cframe();
        return std::pair{frame.position + frame.rightVector() * 9.0f + lunar::Vector3{0, 3, 0}, frame.position};
    };
    // From where the player's camera is, towards the sun (for sun rays).
    auto sunLook = [&]() -> std::optional<std::pair<lunar::Vector3, lunar::Vector3>> {
        const auto from = playerCameraLook(*game, localUserId);
        const auto* lighting = game->getService<lunar::Lighting>();
        if (!from || !lighting)
            return std::nullopt;
        const lunar::Vector3 toSun = lunar::render::sunDirection(lighting->clockTime(), lighting->geographicLatitude());
        std::println("render: towards the sun, {:.2f},{:.2f},{:.2f}", toSun.x, toSun.y, toSun.z);
        return std::pair{from->first, from->first + toSun * 100.0f};
    };
    for (std::size_t i = 0; i < looks.size(); ++i) {
        RenderOptions options;
        options.graphics = graphics;
        options.look = looks[i] == "@overview"  ? std::nullopt
                       : looks[i] == "@camera"  ? playerCameraLook(*game, localUserId)
                       : looks[i] == "@character" ? characterLook()
                       : looks[i] == "@moving"  ? movingLook()
                       : looks[i] == "@union"   ? unionLook()
                       : looks[i] == "@head"    ? headLook()
                       : looks[i].starts_with("@beam") ? beamLook(std::string_view(looks[i]).substr(5))
                       : looks[i] == "@sun"     ? sunLook()
                                                : parseLook(looks[i]);
        options.gui = looks[i] == "@camera"; // what the player sees
        // research: LUNAR_NET_BENCH=<frames> times that many frames, as --render's --bench does.
        if (const char* bench = std::getenv("LUNAR_NET_BENCH"); bench && *bench)
            options.benchFrames = std::atoi(bench);
        // research: LUNAR_NET_SIZE=<width>x<height>, the picture's size (1280x720 without).
        if (const char* size = std::getenv("LUNAR_NET_SIZE"); size && *size) {
            unsigned w = 0, h = 0;
            if (std::sscanf(size, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
                options.width = w;
                options.height = h;
            }
        }
        std::filesystem::path out = outPath;
        if (looks.size() > 1)
            out.replace_filename(std::format("{}_{}{}", outPath.stem().string(), i + 1, outPath.extension().string()));
        code |= renderGame(game, out, options);
    }
    return code;
}
