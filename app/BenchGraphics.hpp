#pragma once

#include <lunar/render/Graphics.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// What the graphics flags of a command line asked for, held apart from the
// settings they make: --graphics names a preset, --render-scale the scale of
// the picture, and the four others say what the ray tracing does. A preset
// is what the rest of them are laid over, so the settings never depend on
// which of the flags came first.
//
// This is the benchmark's own reading of those flags (--net-replay
// --bench-replay), which draws with the Max preset unless --graphics names
// another. It follows the same rules as parseGraphicsFlag, which reads the
// same flags for --play and --render and is left as it is.
struct GraphicsChoice {
    std::optional<lunar::render::GraphicsPreset> preset;
    std::optional<float> renderScale;
    bool rayTracing = false;
    bool reflectEverything = false;
    bool ambientOcclusion = false;
    bool shadowAllLights = false;

    // Whether args[i] is one of those flags, applied here. i is left where it
    // is, so whoever parses the command line after this still takes the flag's
    // value with it. error says what is wrong with a value that is not one.
    bool parse(std::span<const std::string_view> args, std::size_t i, std::string& error);

    // The settings a preset makes, everything else here on top of it.
    // Without a preset, `without`: what the caller draws with when the
    // command line named none.
    lunar::render::GraphicsSettings settings(lunar::render::GraphicsPreset without) const;

    // What was drawn, as a result names it: the preset's name, or the one
    // given for when the command line named none.
    std::string name(lunar::render::GraphicsPreset without) const;
};

// Every one of the flags, read from args[from] on, in whatever order they
// come in. Returns the first value that is not one, in error.
bool graphicsChoice(std::span<const std::string_view> args, std::size_t from, GraphicsChoice& choice,
                    std::string& error);