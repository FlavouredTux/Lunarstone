#include "BenchReplay.hpp"

#include "BenchStats.hpp"
#include "LiveClient.hpp"
#include "LiveScene.hpp"
#include "NetTools.hpp"
#include "OffscreenUi.hpp"
#include "PlaceScene.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/net/Capture.hpp>
#include <lunar/net/CaptureDecoder.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/RayTracing.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
namespace render = lunar::render;
using namespace lunar::bench;

double milliseconds(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Only the datagrams of the session the client talked to most (a capture
// holds the whole journey, hops and all), in the order the server sent them.
std::vector<lunar::net::CapturedDatagram> sessionOf(std::vector<lunar::net::CapturedDatagram> datagrams)
{
    const auto pairOf = [](const lunar::net::CapturedDatagram& d) {
        const std::string a = d.from.toString(), b = d.to.toString();
        return std::min(a, b) + "|" + std::max(a, b);
    };
    std::map<std::string, std::size_t> counts;
    for (const auto& d : datagrams)
        ++counts[pairOf(d)];
    const std::string session = std::ranges::max_element(counts, {}, [](const auto& c) { return c.second; })->first;
    std::erase_if(datagrams, [&](const lunar::net::CapturedDatagram& d) { return pairOf(d) != session; });
    std::ranges::stable_sort(datagrams, {}, &lunar::net::CapturedDatagram::time);
    return datagrams;
}

} // namespace

int benchReplay(const BenchOptions& options)
{
    const std::string& prefix = options.recording;
    lunar::bench::Results results;
    results.recording = prefix;
    results.commit = lunar::bench::gitCommit();
    results.build = lunar::bench::buildDescription();
    results.preset = options.preset;
    results.width = options.width;
    results.height = options.height;
    results.fps = options.fps;
    results.rayTracing = options.graphics.rayTracing;

    // The capture, and what a session recorded with --out leaves beside it.
    const auto entered = Clock::now();
    auto capture = lunar::net::readCapture(prefix + ".pcap");
    if (!capture) {
        std::println(stderr, "{}", capture.error());
        return 1;
    }
    if (capture->empty()) {
        std::println(stderr, "replay: {} has no datagrams", prefix + ".pcap");
        return 1;
    }
    std::vector<lunar::net::CapturedDatagram> datagrams = sessionOf(std::move(*capture));
    const double first = datagrams.front().time;

    lunar::net::CaptureDecoder decoder(lunar::net::DescribeOptions{.quiet = true});
    decoder.loadKeylog(prefix + ".keys");
    // The first datagram makes the session's flow and its replica.
    std::size_t next = 0;
    while (next < datagrams.size() && decoder.replicas().empty())
        decoder.decode(datagrams[next], next);
    if (decoder.replicas().empty()) {
        std::println(stderr, "replay: no session in {}.pcap", prefix);
        return 1;
    }
    std::shared_ptr<lunar::net::Replica> replica = decoder.replicas().front();
    const RecordedJoin join = recordedJoin(prefix);
    replica->game()->assignJob(join.jobId, join.placeId);
    lunar::assets::AssetCache::setSessionPlaceId(static_cast<std::uint64_t>(join.placeId));
    results.loadIn.capture = milliseconds(entered, Clock::now());

    // No window: every frame is drawn into an image nobody looks at, with the
    // GUI laid out over it as OffscreenUi does for --render.
    auto context = render::Context::create({.rayTracing = options.graphics.rayTracing});
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 1;
    }
    if (options.graphics.rayTracing && !(*context)->rayTracing())
        std::println(stderr, "this GPU can't ray trace (no ray queries): shadow maps instead");
    // What the frames below were really drawn with, which is not what was
    // asked for on a device that cannot ray trace.
    results.rayTracing = (*context)->rayTracing();
    results.gpu = (*context)->properties().deviceName;
    auto offscreen = render::OffscreenRenderer::create(**context, options.width, options.height);
    if (!offscreen) {
        std::println(stderr, "Vulkan: {}", offscreen.error());
        return 1;
    }
    render::MeshRenderer& renderer = (*offscreen)->renderer();
    renderer.setGraphics(options.graphics);
    if (!(*offscreen)->setTimestamps(true))
        std::println("bench: this GPU can't time its frames: no timestamp queries");
    // What each of the renderer's passes took on the GPU, which is a second
    // set of stamps on a device that can write them.
    const bool passTimes = (*offscreen)->setPassTimestamps(true);
    if (!passTimes)
        std::println("bench: this GPU can't time its passes: no timestamp queries");
    results.rayTracingSkip = render::rayTracingSkipText();

    std::shared_ptr<lunar::DataModel> game = replica->game();
    lunar::Workspace& workspace = *game->workspace();
    // What the scripts print and the remotes log would otherwise be a line or
    // two of stdout a frame, which is a cost the client at this rate doesn't
    // pay; warnings and errors are the lines still worth reading.
    std::size_t lines = 0;
    auto note = [&lines](std::string_view line) {
        if (++lines <= 4 || line.starts_with("[client] error:") || line.starts_with("[client] warn:"))
            std::println("replay: {}", line);
    };
    LiveClient live(replica, LiveClient::Options{.userId = join.userId,
                                                 .platform = options.scriptPlatform,
                                                 .note = note,
                                                 .simulateCharacter = false});

    // The scene the frames draw, the camera that watches it and the GUI over
    // it: --net-play's, without the window (LiveScene, PlaceScene).
    Wardrobe wardrobe(workspace);
    PlaceScene scene(*game);
    scene.setTerrainDistance(options.graphics.terrainDistance);
    scene.wrapTextures(
        [&](render::TextureProvider textures) { return wardrobe.clothing().provider(std::move(textures)); });
    auto touch = [&scene](lunar::Instance* instance) { scene.touch(*instance); };
    const lunar::Connection added = workspace.descendantAdded.connect(touch);
    const lunar::Connection removing = workspace.descendantRemoving.connect(touch);
    renderer.setMaterialTextures(scene.materialTextures());
    renderer.setTerrainMaterials(scene.terrainMaterials());
    renderer.addEffects(&scene.ribbons());
    renderer.addEffects(&scene.adornments());
    renderer.setLights(&scene.lights());
    render::ParticleSystem particles(workspace, scene.textures());
    particles.setSettings({.density = options.graphics.particleDensity});
    renderer.addEffects(&particles);
    LiveCamera camera(workspace);
    auto made = OffscreenUi::create(**context, **offscreen, *game);
    if (!made) {
        std::println(stderr, "Vulkan: {}", made.error());
        return 1;
    }
    const std::unique_ptr<OffscreenUi> gui = std::move(*made);
    gui->ui().viewports()->setAssets(scene.viewportAssets());
    // A window doesn't wait for the GUI's images to load; nor do the frames timed here.
    gui->setWaitForImages(false);
    live.setConfigureClient([ui = gui.get()](lunar::ClientSession& clientSession) { ui->bind(clientSession); });
    const lunar::Vector2 framebuffer{static_cast<float>(options.width), static_cast<float>(options.height)};

    const double dt = 1.0 / options.fps;
    results.warmupFrames = static_cast<std::size_t>(std::max(options.warmup, 0));
    const std::size_t wanted = results.warmupFrames + static_cast<std::size_t>(std::max(options.frames, 0));
    std::print("bench: {} at {}x{}, {} graphics, {:.0f} Hz: {} frames after {} of warm-up, from {:.0f} s in, on {}",
               prefix, options.width, options.height, options.preset, options.fps, std::max(options.frames, 0),
               results.warmupFrames, options.at, results.gpu);
    // What the ray tracing did without, so a run that skipped something says
    // so where the rest of what it was is said.
    std::print(", LUNAR_RT_SKIP: {}\n", results.rayTracingSkip);
    std::fflush(stdout);

    // Where the scripts spent the time inside their frame, one sample a
    // frame, as the renderer's own phases are.
    std::vector<std::vector<double>> scriptSamples(lunar::SimulationProfile::PhaseCount);
    std::vector<double> luauSamples;
    results.scriptPhases.assign(lunar::SimulationProfile::phaseNames.begin(),
                                lunar::SimulationProfile::phaseNames.end());
    double peakRss = lunar::bench::residentMegabytes();
    double gameTime = 0.0, lightingAt = 0.0, nextReport = 0.0;
    render::Lighting lighting;
    bool scripting = false, clocked = false, loaded = false;
    std::size_t drawn = 0;
    const auto loadStart = Clock::now();
    double scriptsAt = 0.0, sceneAt = 0.0;
    // The workspace streams in over the first seconds of a session, so the
    // load-in runs until the replica stops filling up: the frames after that
    // are the ones a player would be looking at, not the ones that build the
    // place. Half a second of the capture that stays within half a percent
    // of the count it started at is settled: some games never stop
    // making and destroying their effects, and never hold the count still.
    constexpr std::size_t settleFrames = 30;
    std::size_t settled = 0, stepped = 0;
    std::size_t instances = 0, settling = 0;

    for (; drawn < wanted && next < datagrams.size();) {
        const auto frameStart = Clock::now();
        auto mark = frameStart;
        auto lap = [&mark] {
            const double ms = milliseconds(mark, Clock::now());
            mark = Clock::now();
            return ms;
        };
        const bool measuring = drawn >= results.warmupFrames;
        if (measuring)
            results.frames.resize(drawn - results.warmupFrames + 1);
        else
            results.warmup.resize(drawn + 1);
        lunar::bench::Frame& sample = measuring ? results.frames.back() : results.warmup.back();

        // What the server sent since the last frame, and what we owe it: a
        // recording has nobody on the other end, so the remotes' and the
        // tools' messages are taken and thrown away.
        // Once frames are drawn, frameMessageBudget of it a frame at most, as
        // a window's live feed takes it.
        const auto until = Clock::now() + frameMessageBudget;
        while (next < datagrams.size() && datagrams[next].time - first <= gameTime
               && (!loaded || Clock::now() < until)) {
            decoder.decode(datagrams[next], next);
            ++next;
        }
        replica->runtime.pendingPings.clear(); // nobody answers the server's pings here
        (void)live.remotes().take();
        if (lunar::net::ToolReplicator* tools = live.tools())
            (void)tools->take();
        if (live.start(next == datagrams.size())) {
            if (!clocked) {
                // The physics the server sent plays back on the capture's own
                // clock: on the wall clock a fixed timestep would run it
                // thousands of seconds ahead of the frames it is drawn in.
                const double from = first;
                live.animation()->setClock([from, &gameTime] { return from + gameTime; });
                clocked = true;
            }
            if (!scripting) {
                scripting = true;
                // What the script runtime's own phases cost, every frame.
                live.session()->setProfiling(true);
                scriptsAt = milliseconds(entered, Clock::now());
            }
        }
        sample[lunar::bench::Phase::Network] = lap();

        // The client's frame: the game's LocalScripts in RunService's order,
        // the physics in its phase, the animations in theirs (the other
        // players' rigs and the physics the server sent them included).
        if (lunar::ClientSession* session = live.session()) {
            session->setViewportSize(framebuffer);
            live.step(dt);
        }
        sample[lunar::bench::Phase::Scripts] = lap();

        // The CameraModule, after the scripts and the animations moved
        // things. Nothing is pressed at the keyboard in a benchmark.
        camera.update(static_cast<float>(dt), {}, ourPlayer(*replica, join.userId));
        sample[lunar::bench::Phase::Camera] = lap();

        // What the renderer draws: the parts as the replica has them, their
        // classic clothing, the terrain at the detail the camera asks for.
        render::Camera view;
        view.cframe = camera.camera().cframe();
        view.fieldOfView = camera.camera().fieldOfView();
        if (scripting)
            wardrobe.update([&](lunar::Model& model) {
                for (const lunar::InstancePtr& d : model.descendants())
                    scene.touch(*d);
            });
        const bool rebuilt = scene.refresh();
        if (rebuilt || gameTime - lightingAt >= 1.0) {
            lighting = scene.lighting();
            lightingAt = gameTime;
        }
        scene.followParts();
        scene.updateTerrain(view.cframe.position);
        particles.step(static_cast<float>(dt), view.cframe.position);
        sample[lunar::bench::Phase::Scene] = lap();

        // The GUI's layout and its draw list; the renderer records it with
        // the picture below.
        gui->update(dt);
        sample[lunar::bench::Phase::Gui] = lap();

        if (!loaded) {
            // The load-in: a line a second of the capture while it fills up,
            // then the meshes and textures of every part it brought, so that
            // the frames measured are frames of the game and not of the
            // downloads behind it. Nothing is drawn until then.
            const std::size_t now = replica->instanceCount();
            const std::size_t slack = settling / 200;
            settled = now <= settling + slack && now + slack >= settling ? settled + 1 : 0;
            if (settled == 0)
                settling = now;
            instances = now;
            if (gameTime >= nextReport) {
                std::print("bench: {:.1f} s of the capture: {} instances, {} parts, {}\n", gameTime, instances,
                           scene.partItemCount(), scripting ? "the client's scripts are running" : "no client yet");
                std::fflush(stdout);
                nextReport = gameTime + 1.0;
            }
            if (gameTime >= options.at && scripting && settled >= settleFrames) {
                const auto waiting = Clock::now();
                scene.waitForMeshes();
                sceneAt = milliseconds(waiting, Clock::now());
                lighting = scene.lighting();
                lightingAt = gameTime;
                loaded = true;
                results.loadIn.scripts = scriptsAt;
                results.loadIn.scene = sceneAt;
                std::print("bench: {:.1f} s of the capture stepped: {} instances, {} parts, {} ms of meshes\n",
                           gameTime, instances, scene.partItemCount(), sceneAt);
                std::fflush(stdout);
            }
            if (!loaded) {
                gameTime += dt;
                ++stepped;
                continue;
            }
        }

        // The picture: the renderer records it, the GPU draws it, and it is
        // copied back so the caller has an image, which the benchmark drops.
        const auto drawStart = Clock::now();
        (*offscreen)->render(view, scene.items(), lighting);
        const double draw = milliseconds(drawStart, Clock::now());
        const double record = renderer.stats().recordMilliseconds;
        const double readback = (*offscreen)->readbackMilliseconds();
        sample[lunar::bench::Phase::Record] = record;
        sample[lunar::bench::Phase::Readback] = readback;
        // What is left of the draw is the submission and the wait for the GPU,
        // which is where the GPU's own time below is spent.
        sample[lunar::bench::Phase::Submit] = std::max(draw - record - readback, 0.0);
        sample[lunar::bench::Phase::Gpu] = (*offscreen)->gpuMilliseconds();
        sample[lunar::bench::Phase::Frame] = milliseconds(frameStart, Clock::now());
        // What a windowed client spends on such a frame: everything above but
        // the wait for the GPU and the copy back, which a window has neither
        // of. Its frames are in flight, so that work overlaps the GPU's and
        // the frame is as long as whichever of the two takes more: an
        // estimate of the windowed client, not what this run measured.
        const double cpu = sample[lunar::bench::Phase::Network] + sample[lunar::bench::Phase::Scripts]
                           + sample[lunar::bench::Phase::Camera] + sample[lunar::bench::Phase::Scene]
                           + sample[lunar::bench::Phase::Gui] + sample[lunar::bench::Phase::Record];
        sample[lunar::bench::Phase::Cpu] = cpu;
        sample[lunar::bench::Phase::Pipelined] = std::max(cpu, sample[lunar::bench::Phase::Gpu]);

        // What each of the renderer's passes took on the GPU this frame, one
        // sample a frame as the phases above have: the warm-up frames' into
        // their own series, so the measured ones are the ones a player would
        // be looking at.
        if (passTimes) {
            std::vector<std::vector<double>>& series = measuring ? results.passSamples : results.warmupPassSamples;
            if (series.empty())
                series.resize(render::gpuPassCount);
            for (std::size_t i = 0; i < series.size(); ++i)
                series[i].push_back(renderer.passMilliseconds()[i]);
        }
        ++drawn;
        gameTime += dt;

        // The script runtime's own phases and the Luau in them, and the memory
        // the run is at: the benchmark's own work, so between the frames
        // rather than inside one.
        if (lunar::ClientSession* session = live.session()) {
            const lunar::SimulationProfile profile = session->takeProfile();
            if (measuring && profile.frames > 0) {
                for (std::size_t i = 0; i < profile.phases.size() && i < scriptSamples.size(); ++i)
                    scriptSamples[i].push_back(profile.phases[i] * 1000.0);
                luauSamples.push_back((profile.luau[0] + profile.luau[1] + profile.luau[2]) * 1000.0);
            }
        }
        peakRss = std::max(peakRss, lunar::bench::residentMegabytes());

        if (measuring) {
            const render::FrameStats& stats = renderer.stats();
            results.items = stats.items;
            results.visible = stats.visible;
            results.drawCalls = stats.drawCalls;
            results.triangles = stats.triangles;
            results.blasBuilds = stats.blasBuilds;
            results.tlasBuilds = stats.tlasBuilds;
            results.rayInstances = stats.rayInstances;
        }
        if (drawn == 1) {
            const render::FrameStats& stats = renderer.stats();
            results.loadIn.frames = stepped + 1;
            results.loadIn.firstFrame = milliseconds(loadStart, Clock::now());
            results.loadIn.total = results.loadIn.firstFrame + results.loadIn.capture;
            std::print("bench: first frame after {} frames ({:.1f} s of the capture, {:.1f} s of wall time)\n",
                       results.loadIn.frames, static_cast<double>(stepped) * dt, results.loadIn.firstFrame / 1000.0);
            std::print("bench: {} parts, {} draw items, {} draws, {} triangles\n", scene.partItemCount(), stats.items,
                       stats.drawCalls, stats.triangles);
        } else if (drawn == results.warmupFrames && measuring) {
            std::print("bench: {} frames of warm-up in {:.1f} s, {} to measure\n", results.warmupFrames,
                       milliseconds(loadStart, Clock::now()) / 1000.0, wanted - drawn);
        }
        std::fflush(stdout);
    }

    const double wall = milliseconds(loadStart, Clock::now());
    results.scriptSamples = std::move(scriptSamples);
    results.luauSamples = std::move(luauSamples);
    results.loadIn.scripts = scriptsAt;
    results.loadIn.scene = sceneAt;
    results.peakRssMb = peakRss;

    std::print("{}", lunar::bench::summaryText(results));
    const std::size_t all = results.warmup.size() + results.frames.size();
    std::println("bench: {} frames of the capture drawn ({:.1f} s of it), {} measured, in {:.1f} s of wall time", all,
                 static_cast<double>(all) * dt, results.frames.size(), wall / 1000.0);
    std::println("bench: {} lines of script output", lines);
    if (!options.out.empty()) {
        const std::filesystem::path path(options.out);
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream json(path, std::ios::binary);
        json << lunar::bench::toJson(results);
        if (!json)
            throw std::runtime_error(path.string() + ": can't write");
        std::println("bench: {}", path.string());
    }
    return 0;
}
