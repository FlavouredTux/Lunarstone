// What a benchmark run costs (app/BenchStats): the percentiles of a phase's
// samples, the summaries over a run, and the table and the JSON it is
// reported as.

#include "framework.hpp"

#include "BenchStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar::bench;

namespace render = lunar::render;

namespace {

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) < eps;
}

// 1 to 10, the series the percentile boundaries are worked out by hand on.
const std::vector<double>& oneToTen()
{
    static const std::vector<double> samples{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    return samples;
}

// Every phase filled in, so every phase of a summary has samples: a frame of
// `total` ms as the benchmark measures it, and the two estimates of a
// windowed client's the same frame gives.
Frame frame(double total)
{
    Frame out;
    out[Phase::Frame] = total;
    out[Phase::Network] = total / 10.0;
    out[Phase::Scripts] = total / 5.0;
    out[Phase::Camera] = 0.1;
    out[Phase::Scene] = 0.2;
    out[Phase::Gui] = 0.0;
    out[Phase::Record] = 0.3;
    out[Phase::Submit] = 0.05;
    out[Phase::Readback] = 0.15;
    out[Phase::Gpu] = total / 2.0;
    // What the client above spends without the wait for the GPU and the copy
    // back, and the frame that is as long as whichever of the two is.
    out[Phase::Cpu] = total / 10.0 + total / 5.0 + 0.1 + 0.2 + 0.0 + 0.3;
    out[Phase::Pipelined] = std::max(out[Phase::Cpu], out[Phase::Gpu]);
    return out;
}

Results fixture()
{
    Results results;
    results.recording = "example-session";
    results.commit = "abc1234";
    results.build = "release";
    results.gpu = "NVIDIA GeForce RTX 4070";
    results.preset = "max";
    results.width = 1920;
    results.height = 1080;
    results.fps = 60.0;
    results.rayTracing = false;
    results.warmupFrames = 2;
    results.warmup = {frame(50.0), frame(60.0)};
    for (double ms : {10.0, 12.0, 14.0, 16.0})
        results.frames.push_back(frame(ms));
    results.scriptPhases = {"render step", "simulation"};
    results.scriptSamples = {{0.4, 0.5}, {1.25, 2.5}};
    results.luauSamples = {1.5, 2.0};
    // The renderer's passes: two of them timed over the measured frames, and
    // one of those over the warm-up ones, the rest left empty as a frame that
    // did not do them has them.
    results.passSamples.assign(render::gpuPassCount, std::vector<double>{});
    results.passSamples[static_cast<std::size_t>(render::GpuPass::Opaque)] = {1.0, 1.5, 2.0, 2.5};
    results.passSamples[static_cast<std::size_t>(render::GpuPass::Post)] = {0.5, 0.5, 0.75, 0.75};
    results.warmupPassSamples.assign(render::gpuPassCount, std::vector<double>{});
    results.warmupPassSamples[static_cast<std::size_t>(render::GpuPass::Opaque)] = {3.0, 4.0};
    results.blasBuilds = 12;
    results.tlasBuilds = 1;
    results.rayInstances = 250;
    results.loadIn = {1234.5, 200.0, 900.2, 50.0, 84.3, std::size_t{30}};
    results.peakRssMb = 512.5;
    results.items = 1000;
    results.visible = 812;
    results.drawCalls = 400;
    results.triangles = 1234567;
    return results;
}

// Whether the braces and the brackets match and every string is closed.
bool balanced(const std::string& text)
{
    std::vector<char> open;
    bool inString = false;
    bool escaped = false;
    for (char c : text) {
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '{' || c == '[')
            open.push_back(c);
        else if (c == '}' || c == ']') {
            if (open.empty())
                return false;
            const char want = c == '}' ? '{' : '[';
            if (open.back() != want)
                return false;
            open.pop_back();
        }
    }
    return open.empty() && !inString;
}

} // namespace

TEST(bench_percentile_nearest_rank)
{
    const std::vector<double>& samples = oneToTen();
    // index = ceil(p * 10) - 1: the median is the fifth of ten.
    CHECK_EQ(percentile(samples, 0.5), 5.0);
    CHECK_EQ(percentile(samples, 0.95), 10.0);
    CHECK_EQ(percentile(samples, 0.99), 10.0);
    // Just past a tenth lands on the second sample, just under it on the first.
    CHECK_EQ(percentile(samples, 0.1), 1.0);
    CHECK_EQ(percentile(samples, 0.11), 2.0);
    CHECK_EQ(percentile(samples, 0.21), 3.0);
    // The ends.
    CHECK_EQ(percentile(samples, 0.0), 1.0);
    CHECK_EQ(percentile(samples, 1.0), 10.0);
    // The order of the samples doesn't matter to the rank.
    CHECK_EQ(percentile(std::vector<double>{10, 2, 7, 1, 5}, 0.5), 5.0);
}

TEST(bench_percentile_edges)
{
    CHECK_EQ(percentile({}, 0.5), 0.0);
    const std::vector<double>& samples = oneToTen();
    // Out of [0, 1] is clamped, not undefined.
    CHECK_EQ(percentile(samples, -5.0), 1.0);
    CHECK_EQ(percentile(samples, 42.0), 10.0);
    // The caller's span is left alone: a copy is sorted, not theirs.
    std::vector<double> mine{9.0, 1.0, 5.0};
    CHECK_EQ(percentile(mine, 0.5), 5.0);
    CHECK_EQ(mine[0], 9.0);
    CHECK_EQ(mine[1], 1.0);
    CHECK_EQ(mine[2], 5.0);
}

TEST(bench_summarise_known_series)
{
    const Summary summary = summarise(oneToTen());
    CHECK_EQ(summary.frames, std::size_t{10});
    CHECK_EQ(summary.mean, 5.5);
    CHECK_EQ(summary.p50, 5.0);
    CHECK_EQ(summary.p95, 10.0);
    CHECK_EQ(summary.p99, 10.0);
    CHECK_EQ(summary.max, 10.0);

    // 1, 3, 5: the median is the middle of the three.
    const Summary three = summarise(std::vector<double>{5, 1, 3});
    CHECK_EQ(three.frames, std::size_t{3});
    CHECK_EQ(three.mean, 3.0);
    CHECK_EQ(three.p50, 3.0);
    CHECK_EQ(three.p95, 5.0);
    CHECK_EQ(three.p99, 5.0);
    CHECK_EQ(three.max, 5.0);
}

TEST(bench_summarise_empty_and_single)
{
    const Summary empty = summarise({});
    CHECK_EQ(empty.frames, std::size_t{0});
    CHECK_EQ(empty.mean, 0.0);
    CHECK_EQ(empty.p50, 0.0);
    CHECK_EQ(empty.p95, 0.0);
    CHECK_EQ(empty.p99, 0.0);
    CHECK_EQ(empty.max, 0.0);

    const Summary one = summarise(std::vector<double>{4.25});
    CHECK_EQ(one.frames, std::size_t{1});
    CHECK_EQ(one.mean, 4.25);
    CHECK_EQ(one.p50, 4.25);
    CHECK_EQ(one.p95, 4.25);
    CHECK_EQ(one.p99, 4.25);
    CHECK_EQ(one.max, 4.25);
}

TEST(bench_results_summary_over_frames)
{
    const Results results = fixture();

    const Summary frames = results.summary(Phase::Frame);
    CHECK_EQ(frames.frames, std::size_t{4});
    CHECK_EQ(frames.mean, 13.0);
    CHECK_EQ(frames.p50, 12.0);
    CHECK_EQ(frames.p95, 16.0);
    CHECK_EQ(frames.p99, 16.0);
    CHECK_EQ(frames.max, 16.0);

    // A phase that costs nothing in every frame is still counted.
    const Summary gui = results.summary(Phase::Gui);
    CHECK_EQ(gui.frames, std::size_t{4});
    CHECK_EQ(gui.mean, 0.0);
    CHECK_EQ(gui.max, 0.0);

    // The warm-up frames are summarised apart, and don't reach the measured ones.
    const Summary warmup = results.warmupSummary(Phase::Frame);
    CHECK_EQ(warmup.frames, std::size_t{2});
    CHECK_EQ(warmup.mean, 55.0);
    CHECK_EQ(warmup.max, 60.0);
    CHECK_EQ(results.warmupSummary(Phase::Gui).max, 0.0);

    Results none;
    CHECK_EQ(none.summary(Phase::Frame).frames, std::size_t{0});
    CHECK_EQ(none.warmupSummary(Phase::Frame).frames, std::size_t{0});
}

TEST(bench_results_pass_summary_over_frames)
{
    const Results results = fixture();

    const Summary opaque = results.passSummary(static_cast<std::size_t>(render::GpuPass::Opaque));
    CHECK_EQ(opaque.frames, std::size_t{4});
    CHECK_EQ(opaque.mean, 1.75);
    CHECK_EQ(opaque.p50, 1.5);
    CHECK_EQ(opaque.p95, 2.5);
    CHECK_EQ(opaque.p99, 2.5);
    CHECK_EQ(opaque.max, 2.5);

    // The warm-up frames' passes are summarised apart from the measured ones.
    const Summary warmup = results.warmupPassSummary(static_cast<std::size_t>(render::GpuPass::Opaque));
    CHECK_EQ(warmup.frames, std::size_t{2});
    CHECK_EQ(warmup.mean, 3.5);
    CHECK_EQ(warmup.max, 4.0);

    // A pass the frames did not do, and an index that is no pass at all, are
    // empty summaries rather than anything to read.
    CHECK_EQ(results.passSummary(static_cast<std::size_t>(render::GpuPass::Water)).frames, std::size_t{0});
    CHECK_EQ(results.passSummary(render::gpuPassCount).frames, std::size_t{0});
    CHECK_EQ(results.warmupPassSummary(render::gpuPassCount).mean, 0.0);
    CHECK_EQ(results.warmupPassSummary(static_cast<std::size_t>(render::GpuPass::Post)).frames, std::size_t{0});

    Results none;
    CHECK_EQ(none.passSummary(static_cast<std::size_t>(render::GpuPass::Opaque)).frames, std::size_t{0});
}

TEST(bench_json_has_the_passes)
{
    const std::string json = toJson(fixture());
    CHECK(balanced(json));
    CHECK(json.find('\n') == std::string::npos);

    // Every pass by name, in the order the renderer records them.
    CHECK(json.find(R"("passNames":["texture upload","light grid","mesh upload","BLAS build","TLAS build",)")
          != std::string::npos);
    CHECK(
        json.find(
            R"(,"sun shadow map","local shadow atlas","depth prepass","decal layer","opaque","sky","decals","denoise","water",)")
        != std::string::npos);
    CHECK(json.find(R"(,"transparent","effects","highlights","post","upscale","GUI overlay","readback"])")
          != std::string::npos);

    // The passes that were timed, with their numbers, and none of the others.
    CHECK(
        json.find("\"passes\":{\"opaque\":{\"frames\":4,\"mean\":1.75,\"p50\":1.5,\"p95\":2.5,\"p99\":2.5,\"max\":2.5}")
        != std::string::npos);
    CHECK(json.find("\"post\":{\"frames\":4,\"mean\":0.625,\"p50\":0.5,\"p95\":0.75,\"p99\":0.75,\"max\":0.75}")
          != std::string::npos);
    CHECK(json.find("\"water\":") == std::string::npos);
    // The warm-up frames' passes, apart from the measured ones.
    CHECK(json.find("\"warmupPasses\":{\"opaque\":{\"frames\":2,\"mean\":3.5,\"p50\":3,\"p95\":4,\"p99\":4,\"max\":4}")
          != std::string::npos);

    // A sample a measured frame, so a comparison can look at the spread.
    CHECK(json.find("\"passSamples\":{\"opaque\":[1,1.5,2,2.5]") != std::string::npos);
    CHECK(json.find("\"post\":[0.5,0.5,0.75,0.75]") != std::string::npos);

    // The order the passes are written in: their summaries, their names, the
    // samples behind them, the warm-up's, and then the ray tracing.
    CHECK(json.find("\"passes\":") < json.find("\"passNames\":"));
    CHECK(json.find("\"passNames\":") < json.find("\"passSamples\":"));
    CHECK(json.find("\"passSamples\":") < json.find("\"warmupPasses\":"));
    CHECK(json.find("\"warmupPasses\":") < json.find("\"rayTracing\":{\"blasBuilds\""));

    // The acceleration structures the frame built and what was left out.
    CHECK(json.find(R"("rayTracingStats":{"blasBuilds":12,"tlasBuilds":1,"instances":250,"skip":"none"})")
          != std::string::npos);
}

TEST(bench_json_without_pass_timestamps_has_no_passes)
{
    // A run whose GPU could not time its passes names them and leaves the
    // numbers out, rather than reporting 19 passes of nothing.
    const std::string none = toJson(Results{});
    CHECK(balanced(none));
    CHECK(none.find("\"passNames\":") != std::string::npos);
    CHECK(none.find("\"passes\":") == std::string::npos);
    CHECK(none.find("\"passSamples\":") == std::string::npos);
    CHECK(none.find("\"warmupPasses\":") == std::string::npos);
    CHECK(none.find(R"("rayTracingStats":{"blasBuilds":0,"tlasBuilds":0,"instances":0,"skip":"none"})")
          != std::string::npos);

    // The same when the frames were measured but no pass among them was timed.
    Results untimed = fixture();
    untimed.passSamples.assign(render::gpuPassCount, std::vector<double>{});
    untimed.warmupPassSamples.assign(render::gpuPassCount, std::vector<double>{});
    const std::string blank = toJson(untimed);
    CHECK(balanced(blank));
    CHECK(blank.find("\"passes\":") == std::string::npos);
    CHECK(blank.find("\"passSamples\":") == std::string::npos);
    // The phases and their samples are still there: the frames were measured.
    CHECK(blank.find("\"phases\":{\"frame\":{\"frames\":4,") != std::string::npos);
    CHECK(blank.find("\"samples\":{\"frame\":[10,12,14,16]") != std::string::npos);
}

TEST(bench_summary_text_names_the_passes)
{
    const std::string text = summaryText(fixture());
    CHECK(text.find("passes:\n") != std::string::npos);
    CHECK(text.find("  opaque: mean 1.75 ms, p50 1.50, p95 2.50, p99 2.50, max 2.50") != std::string::npos);
    CHECK(text.find("  post: mean 0.62 ms, p50 0.50, p95 0.75, p99 0.75, max 0.75") != std::string::npos);

    // A pass the frames did not do is not named at all.
    CHECK(text.find("  water:") == std::string::npos);
    CHECK(text.find("  depth prepass:") == std::string::npos);

    // The passes come after the phases and before the scripts.
    CHECK(text.find("gpu: mean") < text.find("passes:\n"));
    CHECK(text.find("passes:\n") < text.find("scripts:\n"));

    // Nothing was timed, so there is no passes block to print.
    const std::string none = summaryText(Results{});
    CHECK(none.find("passes:") == std::string::npos);
    CHECK(none.find("  opaque:") == std::string::npos);
}

TEST(bench_summary_text_reports_the_acceleration_structures)
{
    Results results = fixture();
    // The fixture built 12 bottom levels, one top level and 250 instances.
    results.rayTracingSkip = "sun,ao";
    const std::string text = summaryText(results);
    CHECK(text.find("ray tracing: 12 BLAS builds, 1 TLAS build, 250 instances, LUNAR_RT_SKIP: sun,ao")
          != std::string::npos);
    // One of them reads in the singular.
    results.tlasBuilds = 0;
    results.rayInstances = 1;
    CHECK(summaryText(results).find("0 TLAS builds, 1 instance,") != std::string::npos);
    // A run that built nothing says so, and says that nothing was skipped.
    CHECK(summaryText(Results{}).find("ray tracing: 0 BLAS builds, 0 TLAS builds, 0 instances, LUNAR_RT_SKIP: none")
          != std::string::npos);
}

TEST(bench_results_fps_equivalent)
{
    Results none;
    CHECK_EQ(none.fpsEquivalent(), 0.0);

    Results ten;
    ten.frames = {frame(10.0), frame(10.0), frame(10.0)};
    CHECK(near(ten.fpsEquivalent(), 100.0));

    // The warm-up frames don't count towards it.
    const Results results = fixture();
    CHECK(near(results.fpsEquivalent(), 1000.0 / 13.0));
    CHECK(near(results.fpsEquivalent(), 76.923076923076923, 1e-9));
}

TEST(bench_window_estimate_is_its_own_phase)
{
    // The CPU a windowed client spends on a frame, and the frame that is as
    // long as the longer of that and the GPU: both are summarised and
    // compared like any other phase.
    const Results results = fixture();
    const Summary cpu = results.summary(Phase::Cpu);
    const Summary pipelined = results.summary(Phase::Pipelined);
    CHECK_EQ(cpu.frames, std::size_t{4});
    CHECK_EQ(pipelined.frames, std::size_t{4});
    // The four frames are 10, 12, 14 and 16 ms: each spends a tenth on the
    // network, a fifth on the scripts and 0.6 ms of camera, scene, GUI and
    // recording, so 3.6 to 5.4 ms on the CPU, while the GPU has half of the
    // frame every time, so the pipelined estimate is the GPU's.
    constexpr double eps = 1e-6; // the sums are the frames' own arithmetic
    CHECK(near(cpu.p50, 4.2, eps));
    CHECK(near(cpu.mean, 4.5, eps));
    CHECK(near(pipelined.p50, 6.0, eps));
    CHECK(near(results.fpsEquivalent(Phase::Cpu), 1000.0 / cpu.mean, 1e-9));
    CHECK(near(results.fpsEquivalent(Phase::Pipelined), 1000.0 / pipelined.mean, 1e-9));
}

TEST(bench_json_has_the_run_and_every_phase)
{
    const std::string json = toJson(fixture());
    CHECK(balanced(json));
    CHECK_EQ(json.front(), '{');
    CHECK_EQ(json.back(), '}');
    CHECK(json.find('\n') == std::string::npos);

    CHECK(json.find("\"version\":1") != std::string::npos);
    CHECK(json.find("\"recording\":\"example-session\"") != std::string::npos);
    CHECK(json.find("\"commit\":\"abc1234\"") != std::string::npos);
    CHECK(json.find("\"build\":\"release\"") != std::string::npos);
    CHECK(json.find("\"gpu\":\"NVIDIA GeForce RTX 4070\"") != std::string::npos);
    CHECK(json.find("\"preset\":\"max\"") != std::string::npos);
    CHECK(json.find("\"resolution\":{\"width\":1920,\"height\":1080}") != std::string::npos);
    CHECK(json.find("\"fps\":60") != std::string::npos);
    CHECK(json.find("\"rayTracing\":false") != std::string::npos);
    CHECK(json.find("\"warmupFrames\":2") != std::string::npos);
    CHECK(json.find("\"frames\":4") != std::string::npos);

    // Every phase, by name and with its numbers.
    CHECK(json.find(R"("phaseNames":["frame","cpu","pipelined","network","scripts","camera","scene","gui","record",)")
          != std::string::npos);
    CHECK(json.find(R"("submit","readback","gpu"])") != std::string::npos);
    CHECK(json.find("\"phases\":{\"frame\":{\"frames\":4,\"mean\":13,\"p50\":12,\"p95\":16,\"p99\":16,\"max\":16}")
          != std::string::npos);
    CHECK(json.find("\"gui\":{\"frames\":4,\"mean\":0,\"p50\":0,\"p95\":0,\"p99\":0,\"max\":0}") != std::string::npos);
    CHECK(json.find("\"gpu\":{\"frames\":4,\"mean\":6.5,\"p50\":6,\"p95\":8,\"p99\":8,\"max\":8}")
          != std::string::npos);

    // The warm-up apart from the measured frames.
    CHECK(json.find("\"warmup\":{\"frames\":2,\"phases\":{\"frame\":{\"frames\":2,\"mean\":55,") != std::string::npos);

    // The script runtime's own phases, a sample a frame, and the Luau under them.
    CHECK(json.find(R"("render step":{"frames":2,"mean":0.45,"p50":0.4,"p95":0.5,"p99":0.5,"max":0.5})")
          != std::string::npos);
    CHECK(json.find(R"("simulation":{"frames":2,"mean":1.875,"p50":1.25,"p95":2.5,"p99":2.5,"max":2.5})")
          != std::string::npos);
    CHECK(json.find(R"("luau":{"frames":2,"mean":1.75,"p50":1.5,"p95":2,"p99":2,"max":2}})") != std::string::npos);

    // The join, the memory, the FPS the mean frame time is, what was drawn.
    CHECK(
        json.find(
            R"("loadIn":{"totalMs":1234.5,"captureMs":200,"sceneMs":900.2,"scriptsMs":50,"fromFirstFrameMs":84.3,"frames":30})")
        != std::string::npos);
    CHECK(json.find("\"peakRssMb\":512.5") != std::string::npos);
    CHECK(json.find("\"fpsEquivalent\":76.923") != std::string::npos);
    CHECK(json.find("\"renderer\":{\"items\":1000,\"visible\":812,\"drawCalls\":400,\"triangles\":1234567}")
          != std::string::npos);

    // The samples themselves, so a comparison can look at the spread.
    CHECK(json.find("\"samples\":{\"frame\":[10,12,14,16]") != std::string::npos);
    CHECK(json.find("\"network\":[1,1.2,1.4,1.6]") != std::string::npos);
    CHECK(json.find("\"gui\":[0,0,0,0]") != std::string::npos);
}

TEST(bench_json_without_frames_or_a_broken_number)
{
    const std::string json = toJson(Results{});
    CHECK(balanced(json));
    // Nothing was measured, so there are no samples to report.
    CHECK(json.find("\"samples\":") == std::string::npos);
    CHECK(json.find("\"phases\":{\"frame\":{\"frames\":0,") != std::string::npos);
    CHECK(json.find("\"fpsEquivalent\":0") != std::string::npos);

    // A NaN or an infinity from a broken run is a 0, not invalid JSON.
    Results broken = fixture();
    broken.frames[0][Phase::Frame] = std::nan("");
    broken.peakRssMb = std::numeric_limits<double>::infinity();
    const std::string bad = toJson(broken);
    CHECK(balanced(bad));
    CHECK(bad.find("nan") == std::string::npos);
    CHECK(bad.find("inf") == std::string::npos);
    CHECK(bad.find("\"peakRssMb\":0") != std::string::npos);
}

TEST(bench_json_escapes_a_hostile_string)
{
    Results results = fixture();
    results.recording = "example \"session\"\tunder\\over\n";
    results.gpu = "N\"V\\i\ndia\t";
    results.rayTracingSkip = "sun\"ao\\tao\n";
    const std::string json = toJson(results);
    CHECK(balanced(json));

    // The quotes and the backslashes escaped, the newline and the tab as their
    // own escapes: the raw characters are nowhere in it.
    CHECK(json.find("\"recording\":\"example \\\"session\\\"\\tunder\\\\over\\n\"") != std::string::npos);
    CHECK(json.find("\"gpu\":\"N\\\"V\\\\i\\ndia\\t\"") != std::string::npos);
    CHECK(json.find(R"("skip":"sun\"ao\\tao\n")") != std::string::npos);
    CHECK(json.find('\n') == std::string::npos);
    CHECK(json.find('\t') == std::string::npos);

    // The rest of the control characters go out as \u00XX.
    results.gpu = std::string("bell\x07") + "end";
    const std::string control = toJson(results);
    CHECK(balanced(control));
    CHECK(control.find("\"gpu\":\"bell\\u0007end\"") != std::string::npos);
}

TEST(bench_summary_text_reads_like_a_table)
{
    const std::string text = summaryText(fixture());
    CHECK(text.find("benchmark: 4 frames at 60 Hz, 1920x1080, max, no ray tracing") != std::string::npos);

    // Every phase that has samples is named, with its numbers.
    for (std::size_t i = 0; i < phaseCount; ++i) {
        const std::string line = std::string(phaseNames[i]) + ": mean ";
        CHECK(text.find(line) != std::string::npos);
    }
    CHECK(text.find("frame: mean 13.00 ms, p50 12.00, p95 16.00, p99 16.00, max 16.00 (76.9 fps)")
          != std::string::npos);
    CHECK(text.find("network: mean 1.30 ms, p50 1.20, p95 1.60, p99 1.60, max 1.60") != std::string::npos);

    // The scripts, then the join and the memory.
    CHECK(text.find("  render step: mean 0.45 ms, p50 0.40, p95 0.50, p99 0.50, max 0.50") != std::string::npos);
    CHECK(text.find("  simulation: mean 1.88 ms, p50 1.25, p95 2.50, p99 2.50, max 2.50") != std::string::npos);
    CHECK(text.find("  luau: mean 1.75 ms") != std::string::npos);
    CHECK(text.find("load-in: 1234 ms to the first frame drawn") != std::string::npos);
    CHECK(text.find("capture 200, scene 900, scripts 50, 84 of it") != std::string::npos);
    CHECK(text.find("peak RSS: 512.5 MB") != std::string::npos);

    // Nothing was measured, so there are no phases to name.
    const std::string empty = summaryText(Results{});
    CHECK(empty.find("benchmark: 0 frames") != std::string::npos);
    CHECK(empty.find("network: mean") == std::string::npos);
    CHECK(empty.find("peak RSS: 0.0 MB") != std::string::npos);
}

TEST(bench_resident_megabytes_is_the_processes_own)
{
#ifdef __linux__
    const double megabytes = residentMegabytes();
    CHECK(megabytes > 0.0);
    CHECK(megabytes < 1024.0 * 1024.0 * 1024.0);
#endif
}

TEST(bench_build_description_says_what_it_is)
{
    const std::string build = buildDescription();
    CHECK(!build.empty());
    // "release", "debug", or either with the sanitizers it has on.
    CHECK(build.starts_with("release") || build.starts_with("debug"));
}

TEST(bench_git_commit_is_the_checkout_or_unknown)
{
    const std::string commit = gitCommit();
    CHECK(!commit.empty());
    // A checkout gives a short hash; anywhere else it says so.
    if (commit != "unknown")
        CHECK(commit.size() >= 6);
}
