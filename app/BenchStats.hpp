#pragma once

#include <lunar/render/Passes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Where the frames of a replayed session spend their time: what each phase
// of a frame cost, and what each of the renderer's passes cost inside the
// GPU's half of it, over a run, as a table and as JSON for
// tools/bench_compare.py to compare two runs with.

namespace lunar::bench {

// The phases a replayed frame is split into. Frame is the whole of it, and
// the ones after it add up to it: the network's, the scripts' (which is
// where the animation, the physics and the Luau below are spent), the
// camera's, the scene's, the GUI's, and the renderer's -- recording the
// command buffer, submitting it and waiting for the GPU, and the copy of
// the picture back. Gpu is the GPU's own time, which the wait overlaps.
//
// Cpu is what a windowed client spends on a frame: everything above but
// the wait for the GPU and the copy back, which a window has neither of.
// Pipelined is what such a frame costs: a window has frames in flight, so
// the CPU work above overlaps the GPU's, and the frame is as long as
// whichever of the two takes more. Both are estimates of the windowed
// client, not something this run measured.
enum class Phase : std::uint8_t {
    Frame,
    Cpu,
    Pipelined,
    Network,
    Scripts,
    Camera,
    Scene,
    Gui,
    Record,
    Submit,
    Readback,
    Gpu,
    Count,
};
inline constexpr std::size_t phaseCount = static_cast<std::size_t>(Phase::Count);
inline constexpr std::array<std::string_view, phaseCount> phaseNames{
    "frame", "cpu", "pipelined", "network", "scripts", "camera", "scene", "gui", "record", "submit", "readback", "gpu",
};

// One frame's time, in milliseconds, in each phase.
struct Frame {
    std::array<double, phaseCount> phase{};

    double& operator[](Phase which) { return phase[static_cast<std::size_t>(which)]; }
    double operator[](Phase which) const { return phase[static_cast<std::size_t>(which)]; }
};

// How long a phase took over a run, in milliseconds.
struct Summary {
    std::size_t frames = 0; // the frames that had a time for it
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// The p'th percentile of samples (0 to 1), nearest rank: the smallest
// sample at or above that share of them. No samples gives 0.
double percentile(std::span<const double> samples, double p);

// Every sample's mean and percentiles; no samples gives an empty summary.
Summary summarise(std::span<const double> samples);

// One run of the benchmark: the frames it measured, the frames it left out
// as warm-up, what the scripts did with their own phases, how long the join
// took and how much memory the peak of the run needed.
struct Results {
    std::string recording; // the capture's prefix, without its extensions
    std::string commit;    // the commit it was built from, "unknown" outside a checkout
    std::string build;     // "release", "debug", "debug (asan)"...
    std::string gpu;       // the Vulkan device the frames were drawn on
    std::string preset;    // the graphics preset: "max" and the rest
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double fps = 60.0; // the fixed timestep the frames were stepped at
    bool rayTracing = false;

    std::size_t warmupFrames = 0;
    std::vector<Frame> frames; // the measured frames, in order
    std::vector<Frame> warmup; // the frames before them, reported apart

    // The script runtime's own phases (SimulationProfile::phaseNames), one
    // sample a frame like the phases above: where the time inside Scripts
    // went. Its profiler instruments every script that resumes, so these are
    // the numbers with that on, and Scripts carries what it costs.
    std::vector<std::string> scriptPhases;
    std::vector<std::vector<double>> scriptSamples; // milliseconds a frame, per phase, in that order
    std::vector<double> luauSamples;                // all the Luau of a frame, out of those

    // The renderer's passes, one sample a frame like the phases above: what
    // each pass took on the GPU in the frames measured.
    std::vector<std::vector<double>> passSamples;       // milliseconds a frame, per lunar::render::gpuPassNames entry
    std::vector<std::vector<double>> warmupPassSamples; // the frames before them, in the same order

    // What the ray tracing side of the last frame measured did: the
    // acceleration structures it built and the instances in the top level
    // one, and what LUNAR_RT_SKIP left out.
    std::uint32_t blasBuilds = 0, tlasBuilds = 0, rayInstances = 0;
    std::string rayTracingSkip = "none";

    // Joining: what it took to reach the first frame drawn.
    struct LoadIn {
        double total = 0.0;      // from starting to the first frame drawn
        double capture = 0.0;    // reading the capture and decoding the join
        double scene = 0.0;      // the parts, and the meshes and textures under them
        double scripts = 0.0;    // starting the client's own scripts
        double firstFrame = 0.0; // the first frame drawn, from the start again
        std::size_t frames = 0;  // the frames stepped before the client started
    } loadIn;
    double peakRssMb = 0.0;

    // What the renderer drew in the last frame measured, for context.
    std::uint32_t items = 0;
    std::uint32_t visible = 0;
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;

    Summary summary(Phase phase) const;
    Summary warmupSummary(Phase phase) const;
    // What one of the renderer's passes took in the frames measured, and in
    // the warm-up ones: pass is an index into lunar::render::gpuPassNames, and
    // one the run never timed is an empty summary.
    Summary passSummary(std::size_t pass) const;
    Summary warmupPassSummary(std::size_t pass) const;
    // The frames a second the mean frame time is: of the whole frame as it
    // was measured, or of one phase (the windowed client's estimates).
    double fpsEquivalent() const;
    double fpsEquivalent(Phase phase) const;
};

// What the measured frames cost, phase by phase and pass by pass, and what
// the run was: a table for a person to read, ending with the join and the
// memory.
std::string summaryText(const Results& results);

// The same numbers as the JSON tools/bench_compare.py reads. The version is
// for the format, so a comparison can refuse a file it doesn't know.
std::string toJson(const Results& results);

// This process's resident memory in MiB, as Stats:GetTotalMemoryUsageMb reads
// it out of /proc/self/statm; 0 where there is no such file.
double residentMegabytes();

// What the binary was built as: "release", "debug", or either with the
// sanitizers it has on.
std::string buildDescription();

// The commit the source tree is at (git rev-parse --short HEAD), or
// "unknown" outside a checkout or without git.
std::string gitCommit();

} // namespace lunar::bench