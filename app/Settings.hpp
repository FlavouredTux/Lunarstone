#pragma once

#include <lunar/render/Graphics.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

// The player's settings, kept between sessions in
// ~/.config/lunarstone/settings ($XDG_CONFIG_HOME/lunarstone/settings): what
// the in-game menu's Settings tab changes. A game window that keeps them
// (GameWindow::Options::settingsFile) starts from them and writes them back
// when the menu changes one; command-line flags win for the session and are
// never written.
struct PlayerSettings {
    std::optional<lunar::render::GraphicsPreset> graphics; // none: never picked
    bool rayTracing = false; // wanted; a window ray traces from its start only
    bool fullscreen = false;
    float cameraSensitivity = 1.0f;
    float volume = 1.0f;
    bool shiftLock = false;
    bool performanceStats = false;
    bool healthBars = false;
    bool beatRing = false;

    friend bool operator==(const PlayerSettings&, const PlayerSettings&) = default;
};

std::filesystem::path playerSettingsFile();
// Nothing when there's no file or it isn't ours; fields it lacks or that
// aren't sensible keep their defaults.
std::optional<PlayerSettings> loadPlayerSettings(const std::filesystem::path& file);
// Written whole (a new file renamed over the old).
std::expected<void, std::string> savePlayerSettings(const std::filesystem::path& file, const PlayerSettings& settings);
