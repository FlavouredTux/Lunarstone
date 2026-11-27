// The player's kept settings (app/Settings.cpp): what the in-game menu
// writes, read back the next session.

#include "framework.hpp"

#include "Settings.hpp"

#include <filesystem>
#include <format>
#include <fstream>

#include <unistd.h>

namespace {

std::filesystem::path scratch()
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("lunar-settings-test-{}", ::getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST(settings_round_trip)
{
    const auto dir = scratch();
    const auto file = dir / "sub" / "settings";
    const PlayerSettings chosen{.graphics = lunar::render::GraphicsPreset::Max,
                                .rayTracing = true,
                                .fullscreen = true,
                                .cameraSensitivity = 2.5f,
                                .volume = 0.3f,
                                .shiftLock = true,
                                .performanceStats = true,
                                .healthBars = false,
                                .beatRing = true};
    CHECK(savePlayerSettings(file, chosen).has_value());
    CHECK(loadPlayerSettings(file) == std::optional<PlayerSettings>(chosen));
    // Never picked a preset: none comes back.
    CHECK(savePlayerSettings(file, PlayerSettings{}).has_value());
    CHECK(loadPlayerSettings(file) == std::optional<PlayerSettings>(PlayerSettings{}));
    std::filesystem::remove_all(dir);
}

TEST(settings_missing_or_odd_files)
{
    const auto dir = scratch();
    CHECK(!loadPlayerSettings(dir / "none").has_value());
    std::ofstream(dir / "garbled") << "not json";
    CHECK(!loadPlayerSettings(dir / "garbled").has_value());
    // What isn't sensible keeps its default; the rest is read.
    std::ofstream(dir / "odd") << R"({"graphics": "ultra", "volume": 7, "cameraSensitivity": -1,
                                      "rayTracing": "yes", "shiftLock": true})";
    const auto odd = loadPlayerSettings(dir / "odd");
    CHECK(odd.has_value());
    if (odd) {
        CHECK(!odd->graphics.has_value());
        CHECK_EQ(odd->volume, 1.0f);
        CHECK_EQ(odd->cameraSensitivity, 1.0f);
        CHECK(!odd->rayTracing);
        CHECK(odd->shiftLock);
    }
    std::filesystem::remove_all(dir);
}
