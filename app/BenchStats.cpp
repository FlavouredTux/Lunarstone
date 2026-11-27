#include "BenchStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

// The numbers behind a benchmark run: what the phases of the frames cost, the
// scripts' own phases, the join and the memory -- as a table for a person and
// as JSON for tools/bench_compare.py to read back.

namespace lunar::bench {

namespace render = lunar::render;

namespace {

// One sample's worth of a phase, out of the frames that had one.
std::vector<double> samplesOf(std::span<const Frame> frames, Phase phase)
{
    std::vector<double> out;
    out.reserve(frames.size());
    for (const Frame& frame : frames)
        out.push_back(frame[phase]);
    return out;
}

// The renderer's pass of a run's samples, which is empty for a pass the run
// never timed -- and for an index that is not a pass at all.
Summary summarisePass(const std::vector<std::vector<double>>& passes, std::size_t pass)
{
    return pass < passes.size() ? summarise(passes[pass]) : Summary{};
}

// A count with its noun, one of them singular: "1 TLAS build".
std::string counted(std::uint32_t count, std::string_view noun)
{
    return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
}

// Nearest rank into samples that are already sorted.
double percentileOfSorted(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double share = std::clamp(p, 0.0, 1.0) * static_cast<double>(sorted.size());
    // ceil(0) - 1 is -1, which is the smallest of them.
    const auto index = share <= 0.0 ? std::size_t{0} : static_cast<std::size_t>(std::ceil(share)) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

// A number for the JSON: at most three decimals, no exponent, nothing that
// isn't a number (a NaN or an infinity from a broken run reads as 0).
std::string number(double value)
{
    if (!std::isfinite(value) || value == 0.0)
        return "0";
    std::string text = std::format("{:.3f}", value);
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    return text;
}

// A JSON string, with what a recording's name or a GPU's can hold in it
// escaped: the quotes and the backslashes, the newline and the tab, and the
// rest of the control characters as \u00XX.
void appendString(std::string& out, std::string_view text)
{
    out += '"';
    for (char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default: {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned int>(byte));
            else
                out += c;
            break;
        }
        }
    }
    out += '"';
}

void appendNumber(std::string& out, double value)
{
    out += number(value);
}

// {"frames":N,"mean":..,"p50":..,"p95":..,"p99":..,"max":..}
void appendSummary(std::string& out, const Summary& summary)
{
    out += std::format(R"({{"frames":{},"mean":{},"p50":{},"p95":{},"p99":{},"max":{}}})", summary.frames,
                       number(summary.mean), number(summary.p50), number(summary.p95), number(summary.p99),
                       number(summary.max));
}

// "frame":{"frames":N,...},"network":{...},...
void appendPhaseObject(std::string& out, const Results& results, bool warmup)
{
    out += '{';
    for (std::size_t i = 0; i < phaseCount; ++i) {
        if (i != 0)
            out += ',';
        appendString(out, phaseNames[i]);
        out += ':';
        const auto phase = static_cast<Phase>(i);
        appendSummary(out, warmup ? results.warmupSummary(phase) : results.summary(phase));
    }
    out += '}';
}

// "frame":[12.3,12.4,...],...
void appendSamples(std::string& out, const Results& results)
{
    out += '{';
    for (std::size_t i = 0; i < phaseCount; ++i) {
        if (i != 0)
            out += ',';
        appendString(out, phaseNames[i]);
        out += ":[";
        const auto phase = static_cast<Phase>(i);
        for (std::size_t f = 0; f < results.frames.size(); ++f) {
            if (f != 0)
                out += ',';
            appendNumber(out, results.frames[f][phase]);
        }
        out += ']';
    }
    out += '}';
}

// "opaque":{"frames":N,...}, for the passes that have samples: a run whose
// GPU could not time its passes has none, so its object is empty.
void appendPassObject(std::string& out, const Results& results, bool warmup)
{
    out += '{';
    bool first = true;
    for (std::size_t i = 0; i < render::gpuPassCount; ++i) {
        const Summary pass = warmup ? results.warmupPassSummary(i) : results.passSummary(i);
        if (pass.frames == 0)
            continue;
        if (!first)
            out += ',';
        first = false;
        appendString(out, render::gpuPassNames[i]);
        out += ':';
        appendSummary(out, pass);
    }
    out += '}';
}

// "opaque":[1.2,1.3,...], one a measured frame, for the passes timed at all.
void appendPassSamples(std::string& out, const Results& results)
{
    out += '{';
    bool first = true;
    for (std::size_t i = 0; i < render::gpuPassCount; ++i) {
        if (i >= results.passSamples.size() || results.passSamples[i].empty())
            continue;
        if (!first)
            out += ',';
        first = false;
        appendString(out, render::gpuPassNames[i]);
        out += ":[";
        const std::vector<double>& samples = results.passSamples[i];
        for (std::size_t f = 0; f < samples.size(); ++f) {
            if (f != 0)
                out += ',';
            appendNumber(out, samples[f]);
        }
        out += ']';
    }
    out += '}';
}

// Whether any pass of a run's samples has any at all: a run with no pass
// timestamps is reported without a passes block rather than as 19 zeros.
bool anyPassSampled(const std::vector<std::vector<double>>& passes)
{
    for (const std::vector<double>& pass : passes) {
        if (!pass.empty())
            return true;
    }
    return false;
}

// A summary-shaped object, as the table's and the JSON's line for a phase. The
// decimals have to be asked for by name: a bare precision is significant
// digits, and a phase would read "1e+03".
std::string phaseLine(std::string_view name, const Summary& summary, std::string_view suffix = {})
{
    return std::format("{}: mean {:.2f} ms, p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, max {:.2f}{}\n", name, summary.mean,
                       summary.p50, summary.p95, summary.p99, summary.max, suffix);
}

// The frames a second a mean of milliseconds is; 0 where there is no mean
// (no frames measured yet).
double framesPerSecond(double milliseconds)
{
    return milliseconds > 0.0 ? 1000.0 / milliseconds : 0.0;
}

} // namespace

double percentile(std::span<const double> samples, double p)
{
    if (samples.empty())
        return 0.0;
    if (!std::isfinite(p))
        p = 0.0;
    std::vector<double> sorted(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    return percentileOfSorted(sorted, p);
}

Summary summarise(std::span<const double> samples)
{
    Summary summary;
    summary.frames = samples.size();
    if (samples.empty())
        return summary;

    std::vector<double> sorted(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    double total = 0.0;
    for (double sample : sorted)
        total += sample;
    summary.mean = total / static_cast<double>(sorted.size());
    summary.p50 = percentileOfSorted(sorted, 0.50);
    summary.p95 = percentileOfSorted(sorted, 0.95);
    summary.p99 = percentileOfSorted(sorted, 0.99);
    summary.max = sorted.back();
    return summary;
}

Summary Results::summary(Phase phase) const
{
    return summarise(samplesOf(std::span<const Frame>(frames), phase));
}

Summary Results::warmupSummary(Phase phase) const
{
    return summarise(samplesOf(std::span<const Frame>(warmup), phase));
}

Summary Results::passSummary(std::size_t pass) const
{
    return summarisePass(passSamples, pass);
}

Summary Results::warmupPassSummary(std::size_t pass) const
{
    return summarisePass(warmupPassSamples, pass);
}

double Results::fpsEquivalent() const
{
    return framesPerSecond(summary(Phase::Frame).mean);
}

double Results::fpsEquivalent(Phase phase) const
{
    return framesPerSecond(summary(phase).mean);
}

std::string summaryText(const Results& results)
{
    std::string out = std::format("benchmark: {} frames at {} Hz, {}x{}, {}, {}\n", results.frames.size(), results.fps,
                                  results.width, results.height, results.preset,
                                  results.rayTracing ? "ray tracing" : "no ray tracing");
    out += std::format("  {} on {}, {} build at {}\n", results.commit, results.gpu, results.build, results.recording);

    for (std::size_t i = 0; i < phaseCount; ++i) {
        const auto which = static_cast<Phase>(i);
        const Summary phase = results.summary(which);
        if (phase.frames == 0)
            continue;
        // The frame itself and the two estimates of a windowed client's are
        // the numbers anyone quotes, so they carry their frames a second.
        const bool quoted = which == Phase::Frame || which == Phase::Cpu || which == Phase::Pipelined;
        out += phaseLine(phaseNames[i], phase,
                         quoted ? std::format(" ({:.1f} fps)", results.fpsEquivalent(which)) : std::string());
    }

    // The renderer's passes, in the order it records them, and what the ray
    // tracing side of the frame built or left out.
    bool anyPass = false;
    for (std::size_t i = 0; i < render::gpuPassCount; ++i)
        anyPass = anyPass || results.passSummary(i).frames > 0;
    if (anyPass) {
        out += "passes:\n";
        for (std::size_t i = 0; i < render::gpuPassCount; ++i) {
            const Summary pass = results.passSummary(i);
            if (pass.frames == 0)
                continue;
            out += phaseLine("  " + std::string(render::gpuPassNames[i]), pass);
        }
    }
    out += std::format("ray tracing: {}, {}, {}, LUNAR_RT_SKIP: {}\n", counted(results.blasBuilds, "BLAS build"),
                       counted(results.tlasBuilds, "TLAS build"), counted(results.rayInstances, "instance"),
                       results.rayTracingSkip);

    if (!results.scriptPhases.empty()) {
        out += "scripts:\n";
        for (std::size_t i = 0; i < results.scriptPhases.size() && i < results.scriptSamples.size(); ++i)
            out += phaseLine("  " + std::string(results.scriptPhases[i]), summarise(results.scriptSamples[i]));
        if (!results.luauSamples.empty())
            out += phaseLine("  luau", summarise(results.luauSamples));
    }

    out += std::format("load-in: {:.0f} ms to the first frame drawn (capture {:.0f}, scene {:.0f}, scripts {:.0f},",
                       results.loadIn.total, results.loadIn.capture, results.loadIn.scene, results.loadIn.scripts);
    out += std::format(" {:.0f} of it from the replay's first frame) over {} frames\n", results.loadIn.firstFrame,
                       results.loadIn.frames);
    out += std::format("peak RSS: {:.1f} MB\n", results.peakRssMb);
    return out;
}

std::string toJson(const Results& results)
{
    std::string out = "{\"version\":1";

    out += ",\"recording\":";
    appendString(out, results.recording);
    out += ",\"commit\":";
    appendString(out, results.commit);
    out += ",\"build\":";
    appendString(out, results.build);
    out += ",\"gpu\":";
    appendString(out, results.gpu);
    out += ",\"preset\":";
    appendString(out, results.preset);
    out += std::format(R"(,"resolution":{{"width":{},"height":{}}})", results.width, results.height);
    out += ",\"fps\":";
    appendNumber(out, results.fps);
    out += std::format(R"(,"rayTracing":{})", results.rayTracing);
    out += std::format(R"(,"warmupFrames":{},"frames":{})", results.warmupFrames, results.frames.size());

    out += ",\"phaseNames\":[";
    for (std::size_t i = 0; i < phaseCount; ++i) {
        if (i != 0)
            out += ',';
        appendString(out, phaseNames[i]);
    }
    out += ']';

    out += ",\"phases\":";
    appendPhaseObject(out, results, false);

    // A run whose GPU could not time its passes has none of them, and says so
    // by having no passes at all rather than 19 passes of nothing.
    const bool timed = anyPassSampled(results.passSamples);
    if (timed) {
        out += ",\"passes\":";
        appendPassObject(out, results, false);
    }

    out += ",\"passNames\":[";
    for (std::size_t i = 0; i < render::gpuPassCount; ++i) {
        if (i != 0)
            out += ',';
        appendString(out, render::gpuPassNames[i]);
    }
    out += ']';

    if (timed && !results.frames.empty()) {
        out += ",\"passSamples\":";
        appendPassSamples(out, results);
    }
    if (anyPassSampled(results.warmupPassSamples)) {
        out += ",\"warmupPasses\":";
        appendPassObject(out, results, true);
    }

    // "rayTracing" at the top is the setting; this is what it did, under a
    // name of its own so a parser never has two of the same key.
    out += std::format(R"(,"rayTracingStats":{{"blasBuilds":{},"tlasBuilds":{},"instances":{},"skip":)", results.blasBuilds,
                       results.tlasBuilds, results.rayInstances);
    appendString(out, results.rayTracingSkip);
    out += '}';

    out += std::format(R"(,"warmup":{{"frames":{},"phases":)", results.warmup.size());
    appendPhaseObject(out, results, true);
    out += '}';

    out += R"(,"scripts":{"phases":{)";
    for (std::size_t i = 0; i < results.scriptPhases.size() && i < results.scriptSamples.size(); ++i) {
        if (i != 0)
            out += ',';
        appendString(out, results.scriptPhases[i]);
        out += ':';
        appendSummary(out, summarise(results.scriptSamples[i]));
    }
    out += R"(},"luau":)";
    appendSummary(out, summarise(results.luauSamples));
    out += '}';

    out += std::format(
        R"(,"loadIn":{{"totalMs":{},"captureMs":{},"sceneMs":{},"scriptsMs":{},"fromFirstFrameMs":{},"frames":{}}})",
        number(results.loadIn.total), number(results.loadIn.capture), number(results.loadIn.scene),
        number(results.loadIn.scripts), number(results.loadIn.firstFrame), results.loadIn.frames);

    out += ",\"peakRssMb\":";
    appendNumber(out, results.peakRssMb);
    out += ",\"fpsEquivalent\":";
    appendNumber(out, results.fpsEquivalent());

    out += std::format(R"(,"renderer":{{"items":{},"visible":{},"drawCalls":{},"triangles":{}}})", results.items,
                       results.visible, results.drawCalls, results.triangles);

    if (!results.frames.empty()) {
        out += ",\"samples\":";
        appendSamples(out, results);
    }

    out += '}';
    return out;
}

double residentMegabytes()
{
    double megabytes = 0.0;
    if (std::FILE* file = std::fopen("/proc/self/statm", "r")) {
        char line[128] = {};
        // "size resident shared ..." in pages, as Stats:GetTotalMemoryUsageMb reads it.
        if (std::fgets(line, sizeof(line), file)) {
            std::uint64_t size = 0, resident = 0;
            std::istringstream numbers(line);
            if ((numbers >> size >> resident) && resident > 0) {
                const long pageSize = sysconf(_SC_PAGESIZE);
                if (pageSize > 0)
                    megabytes = static_cast<double>(resident) * static_cast<double>(pageSize) / (1024.0 * 1024.0);
            }
        }
        std::fclose(file);
    }
    return megabytes;
}

std::string buildDescription()
{
#ifdef NDEBUG
    std::string text = "release";
#else
    std::string text = "debug";
#endif
#if defined(__SANITIZE_ADDRESS__)
    text += " (asan)";
#endif
#if defined(__SANITIZE_THREAD__)
    text += " (tsan)";
#endif
    return text;
}

std::string gitCommit()
{
    // A fixed command line, so there is nothing in it to inject.
    // A fixed command line with nothing of the caller's in it, so there is
    // nothing for the shell to be asked.
    std::FILE* pipe = ::popen("git rev-parse --short HEAD 2>/dev/null", "r"); // NOLINT(bugprone-command-processor)
    if (!pipe)
        return "unknown";

    std::string out;
    char line[128] = {};
    while (std::fgets(line, sizeof(line), pipe))
        out += line;
    const int status = ::pclose(pipe);

    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    if (status != 0 || out.empty())
        return "unknown";
    return out;
}

} // namespace lunar::bench
