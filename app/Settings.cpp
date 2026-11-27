#include "Settings.hpp"

#include <lunar/assets/Json.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr std::string_view presetNames[] = {"minimal", "average", "max", "potato"};

std::string_view presetName(lunar::render::GraphicsPreset preset)
{
    return presetNames[static_cast<std::size_t>(preset)];
}

} // namespace

std::filesystem::path playerSettingsFile()
{
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = config && *config ? std::filesystem::path(config)
                                       : home && *home    ? std::filesystem::path(home) / ".config"
                                                          : std::filesystem::path(".");
    return base / "lunarstone" / "settings";
}

std::optional<PlayerSettings> loadPlayerSettings(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    const auto doc = lunar::assets::json::parse(text.str());
    if (!doc || !doc->object())
        return std::nullopt;
    PlayerSettings s;
    const auto flag = [&](std::string_view name, bool& out) {
        if (const auto* v = (*doc)[name])
            if (const auto b = v->boolean())
                out = *b;
    };
    const auto number = [&](std::string_view name, float& out, float lo, float hi) {
        if (const auto* v = (*doc)[name])
            if (const auto n = v->number(); n && *n >= lo && *n <= hi)
                out = static_cast<float>(*n);
    };
    if (const auto* v = (*doc)["graphics"])
        if (const auto* name = v->string())
            s.graphics = lunar::render::parseGraphicsPreset(*name);
    flag("rayTracing", s.rayTracing);
    flag("fullscreen", s.fullscreen);
    number("cameraSensitivity", s.cameraSensitivity, 0.1f, 10.0f);
    number("volume", s.volume, 0.0f, 1.0f);
    flag("shiftLock", s.shiftLock);
    flag("performanceStats", s.performanceStats);
    flag("healthBars", s.healthBars);
    flag("beatRing", s.beatRing);
    return s;
}

std::expected<void, std::string> savePlayerSettings(const std::filesystem::path& file, const PlayerSettings& s)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string body = "{\n";
    if (s.graphics)
        body += std::format("  \"graphics\": \"{}\",\n", presetName(*s.graphics));
    body += std::format("  \"rayTracing\": {},\n  \"fullscreen\": {},\n  \"cameraSensitivity\": {},\n"
                        "  \"volume\": {},\n  \"shiftLock\": {},\n  \"performanceStats\": {},\n"
                        "  \"healthBars\": {},\n  \"beatRing\": {}\n}}\n",
                        b(s.rayTracing), b(s.fullscreen), s.cameraSensitivity, s.volume, b(s.shiftLock),
                        b(s.performanceStats), b(s.healthBars), b(s.beatRing));
    const std::filesystem::path temporary = file.string() + ".new";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(std::format("can't write {}", temporary.string()));
    const bool written = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!written || ::rename(temporary.c_str(), file.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return std::unexpected(std::format("can't write {}", file.string()));
    }
    return {};
}
