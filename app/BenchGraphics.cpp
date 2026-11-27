#include "BenchGraphics.hpp"

#include <charconv>
#include <format>
#include <system_error>

bool GraphicsChoice::parse(std::span<const std::string_view> args, std::size_t i, std::string& error)
{
    const std::string_view flag = args[i];
    if (flag == "--raytracing") {
        rayTracing = true;
        return true;
    }
    if (flag == "--reflect-everything") {
        rayTracing = true;
        reflectEverything = true;
        return true;
    }
    if (flag == "--ambient-occlusion") {
        rayTracing = true;
        ambientOcclusion = true;
        return true;
    }
    if (flag == "--shadow-all-lights") {
        rayTracing = true;
        shadowAllLights = true;
        return true;
    }
    // A flag of its own at the end of a command line has no value to read;
    // whoever parses it next says the usage, as it always has.
    if (i + 1 >= args.size() || (flag != "--graphics" && flag != "--render-scale"))
        return false;
    const std::string_view value = args[i + 1];
    if (flag == "--graphics") {
        if (value == "raytracing") {
            error = "ray tracing is a flag of its own: --raytracing, with any --graphics";
            return true;
        }
        const auto named = lunar::render::parseGraphicsPreset(value);
        if (!named) {
            error = std::format("--graphics {}: minimal, average, max or potato", value);
            return true;
        }
        preset = *named;
        return true;
    }
    int percent = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), percent);
    if (ec != std::errc{} || end != value.data() + value.size() || percent < 25 || percent > 100) {
        error = std::format("--render-scale {}: a percentage from 25 to 100", value);
        return true;
    }
    renderScale = static_cast<float>(percent) / 100.0f;
    return true;
}

lunar::render::GraphicsSettings GraphicsChoice::settings(lunar::render::GraphicsPreset without) const
{
    // The preset first, then everything else here: an order the flags came in
    // cannot change what the picture is drawn with.
    lunar::render::GraphicsSettings graphics = lunar::render::graphicsSettings(preset.value_or(without));
    if (renderScale)
        graphics.renderScale = *renderScale;
    graphics.rayTracing = rayTracing;
    graphics.reflectEverything = reflectEverything;
    graphics.ambientOcclusion = ambientOcclusion;
    graphics.shadowAllLights = shadowAllLights;
    return graphics;
}

std::string GraphicsChoice::name(lunar::render::GraphicsPreset without) const
{
    switch (preset.value_or(without)) {
    case lunar::render::GraphicsPreset::Minimal:
        return "minimal";
    case lunar::render::GraphicsPreset::Average:
        return "average";
    case lunar::render::GraphicsPreset::Max:
        return "max";
    case lunar::render::GraphicsPreset::Potato:
        return "potato";
    }
    return "unknown";
}

bool graphicsChoice(std::span<const std::string_view> args, std::size_t from, GraphicsChoice& choice,
                    std::string& error)
{
    for (std::size_t i = from; i < args.size(); ++i)
        if (choice.parse(args, i, error) && !error.empty())
            return false;
    return true;
}