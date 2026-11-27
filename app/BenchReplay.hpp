#pragma once

#include <lunar/render/Graphics.hpp>

#include <cstdint>
#include <string>

// --net-replay <prefix> --bench-replay <frames>: a recorded session played
// back at a fixed timestep with the client's own scripts running and every
// frame drawn offscreen, timed phase by phase: what the network brought, the
// scripts (with their own phases), the camera, the scene, the GUI, the
// renderer's CPU and the GPU's own time. A summary on stdout and, with
// --bench-out, the same numbers as JSON for tools/bench_compare.py.
struct BenchOptions {
    std::string recording;      // the capture's prefix, without .pcap
    std::uint32_t width = 1920; // --bench-size
    std::uint32_t height = 1080;
    int frames = 600;                     // --bench-replay: frames to measure
    int warmup = 120;                     // --bench-warmup: frames before them, timed but left out
    double at = 0.0;                      // --bench-at: seconds of the capture to step before the first frame drawn
    double fps = 60.0;                    // --fps: the fixed timestep they are stepped at
    std::string preset = "max";           // the --graphics preset, as the results name it
    std::string out;                      // --bench-out: the JSON to write, empty for none
    std::string scriptPlatform = "Linux"; // what --net-join says by default
    lunar::render::GraphicsSettings graphics;
};

// Runs the benchmark; the exit code is the one to hand back.
int benchReplay(const BenchOptions& options);