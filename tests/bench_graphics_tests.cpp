#include "BenchGraphics.hpp"

#include "framework.hpp"

#include <lunar/render/Graphics.hpp>

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using lunar::render::GraphicsPreset;
using lunar::render::GraphicsSettings;

// What --net-replay --bench-replay draws with, from a command line's graphics
// flags: parseGraphicsFlag consumes a flag's value with args[++i], so it
// cannot be called twice over one span -- the second call reads the value and
// misses -- and the benchmark's own reading of them is here instead. What a
// flag says may not depend on which of them came first.

namespace {

// The graphics of a command line, the preset it was named and any error.
struct Read {
    GraphicsChoice choice;
    std::string error;
    GraphicsSettings settings() const { return choice.settings(GraphicsPreset::Max); }
    std::string name() const { return choice.name(GraphicsPreset::Max); }
};

Read readLine(std::vector<std::string_view> args, std::size_t from = 0)
{
    Read read;
    graphicsChoice(args, from, read.choice, read.error);
    return read;
}

const GraphicsSettings& max()
{
    static const GraphicsSettings settings = lunar::render::graphicsSettings(GraphicsPreset::Max);
    return settings;
}

} // namespace

TEST(bench_graphics_no_preset_is_max)
{
    // Without --graphics the benchmark draws with Max, and says so.
    const Read read = readLine({"--net-replay", "session.hop1", "--bench-replay", "600"});
    CHECK(read.error.empty());
    CHECK_EQ(read.name(), "max");
    CHECK_EQ(read.settings().shadowResolution, max().shadowResolution);
    CHECK_EQ(read.settings().terrainDistance, max().terrainDistance);
    CHECK(read.settings().shadows);
    CHECK(read.settings().postEffects);
    CHECK(!read.settings().rayTracing);
}

TEST(bench_graphics_names_the_preset_it_was_given)
{
    const Read minimal = readLine({"--net-replay", "p", "--bench-replay", "600", "--graphics", "minimal"});
    CHECK(minimal.error.empty());
    CHECK_EQ(minimal.name(), "minimal");
    const GraphicsSettings wanted = lunar::render::graphicsSettings(GraphicsPreset::Minimal);
    CHECK_EQ(minimal.settings().shadows, wanted.shadows);
    CHECK_EQ(minimal.settings().postEffects, wanted.postEffects);
    CHECK_EQ(minimal.settings().textureQuality, wanted.textureQuality);
    CHECK_EQ(minimal.settings().drawDistance, wanted.drawDistance);

    // Average is Average: a preset of its own, not the one the benchmark
    // defaults to.
    const GraphicsSettings averagePreset = lunar::render::graphicsSettings(GraphicsPreset::Average);
    const Read average = readLine({"--graphics", "average"});
    CHECK_EQ(average.name(), "average");
    CHECK_EQ(average.settings().shadowResolution, averagePreset.shadowResolution);
    CHECK_EQ(average.settings().terrainDistance, averagePreset.terrainDistance);
    CHECK(averagePreset.shadowResolution != max().shadowResolution);

    // The last --graphics is the one that counts, as it is for every command.
    const Read twice = readLine({"--graphics", "minimal", "--graphics", "max"});
    CHECK_EQ(twice.name(), "max");
    CHECK_EQ(twice.settings().shadowResolution, max().shadowResolution);
}

TEST(bench_graphics_render_scale_with_and_without_a_preset)
{
    // --render-scale lands on the preset it comes with, before it or after:
    // the order of the two flags is not the order of the settings.
    const GraphicsSettings minimal = lunar::render::graphicsSettings(GraphicsPreset::Minimal);
    const Read scaleFirst = readLine({"--render-scale", "75", "--graphics", "minimal"});
    const Read presetFirst = readLine({"--graphics", "minimal", "--render-scale", "75"});
    for (const Read* read : {&scaleFirst, &presetFirst}) {
        CHECK(read->error.empty());
        CHECK(std::abs(read->settings().renderScale - 0.75f) < 1e-6f);
        // The rest of the preset is still the one --graphics said.
        CHECK_EQ(read->settings().shadows, minimal.shadows);
        CHECK_EQ(read->settings().shadowResolution, minimal.shadowResolution);
        CHECK_EQ(read->name(), "minimal");
    }

    // And on the Max preset it is Max at half scale.
    const Read onMax = readLine({"--graphics", "max", "--render-scale", "50"});
    CHECK(std::abs(onMax.settings().renderScale - 0.5f) < 1e-6f);
    CHECK_EQ(onMax.settings().shadowResolution, max().shadowResolution);
    CHECK_EQ(onMax.name(), "max");

    // Without --graphics the scale is still the only thing that changed.
    const Read alone = readLine({"--render-scale", "75"});
    CHECK(std::abs(alone.settings().renderScale - 0.75f) < 1e-6f);
    CHECK_EQ(alone.settings().shadowResolution, max().shadowResolution);
    CHECK_EQ(alone.name(), "max");
}

TEST(bench_graphics_ray_tracing_flags_on_top_of_a_preset)
{
    const Read sun = readLine({"--graphics", "max", "--raytracing"});
    CHECK(sun.error.empty());
    CHECK(sun.settings().rayTracing);
    CHECK(!sun.settings().reflectEverything);
    CHECK(!sun.settings().ambientOcclusion);
    CHECK(!sun.settings().shadowAllLights);
    CHECK_EQ(sun.settings().shadowResolution, max().shadowResolution);

    // Each of the three others turns ray tracing on as well, and leaves the
    // rest of the preset alone.
    for (const std::string_view flag : {"--reflect-everything", "--ambient-occlusion", "--shadow-all-lights"}) {
        const Read read = readLine({"--graphics", "max", flag});
        CHECK(read.error.empty());
        CHECK(read.settings().rayTracing);
        CHECK_EQ(read.settings().shadowResolution, max().shadowResolution);
        if (flag == "--reflect-everything")
            CHECK(read.settings().reflectEverything);
        if (flag == "--ambient-occlusion")
            CHECK(read.settings().ambientOcclusion);
        if (flag == "--shadow-all-lights")
            CHECK(read.settings().shadowAllLights);
    }

    // And a preset after them keeps all three.
    const Read all = readLine({"--ambient-occlusion", "--shadow-all-lights", "--reflect-everything", "--graphics",
                           "minimal"});
    CHECK(all.settings().rayTracing && all.settings().ambientOcclusion && all.settings().shadowAllLights
          && all.settings().reflectEverything);
    CHECK(!all.settings().shadows);
    CHECK_EQ(all.name(), "minimal");
}

TEST(bench_graphics_bad_values_say_what_is_wrong)
{
    const Read raytracing = readLine({"--graphics", "raytracing"});
    CHECK(raytracing.error.find("--raytracing") != std::string::npos);

    const Read named = readLine({"--graphics", "ultra"});
    CHECK(named.error.find("minimal, average, max or potato") != std::string::npos);

    const Read small = readLine({"--render-scale", "10"});
    CHECK(small.error.find("a percentage from 25 to 100") != std::string::npos);

    const Read words = readLine({"--render-scale", "half"});
    CHECK(words.error.find("a percentage from 25 to 100") != std::string::npos);

    // A flag at the end of a command line has no value; the usage says so.
    const Read dangling = readLine({"--graphics"});
    CHECK(dangling.error.empty());
    CHECK_EQ(dangling.name(), "max");
}

TEST(bench_graphics_reads_only_from_where_it_is_told_to)
{
    // The prefix before `from` (the command and the capture) is none of its
    // business, and a value that looks like a flag is read as one only where
    // a flag is expected.
    Read read = readLine({"--graphics", "max", "--bench-render", "max", "--fps", "60"}, 2);
    CHECK(read.error.empty());
    CHECK_EQ(read.name(), "max");
    // parse reads the one flag at the index and leaves the index where it is,
    // so the caller can still take the value with the flag itself.
    const std::vector<std::string_view> more{"--graphics", "minimal", "--render-scale", "50"};
    CHECK(read.choice.parse(more, 0, read.error));
    CHECK_EQ(read.name(), "minimal");
    CHECK(!read.choice.renderScale);
    CHECK(graphicsChoice(more, 0, read.choice, read.error));
    CHECK_EQ(read.name(), "minimal");
    CHECK(std::abs(read.settings().renderScale - 0.5f) < 1e-6f);
}