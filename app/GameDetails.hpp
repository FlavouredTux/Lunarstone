#pragma once

#include <lunar/assets/Web.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// What the loading screen says about a game: the experience's name, who
// made it (a user's or a group's name) and its universe, whose icon is
// rbxthumb://type=GameIcon.
struct GameDetails {
    std::uint64_t universeId = 0;
    std::string name;
    std::string creator;

    // The icon's content id at size x size (a size the thumbnails API makes:
    // 50, 128, 150, 256, 420 or 512).
    std::string icon(int size = 256) const;
};

// apis.roblox.com's answer for a place's universe ({"universeId": N}).
std::expected<std::uint64_t, std::string> parseUniverseReply(std::string_view text);
// games.roblox.com/v1/games?universeIds=N's answer: the first game in its data.
std::expected<GameDetails, std::string> parseGamesReply(std::string_view text);

// A place's game, asked anonymously: its universe, then the game's details.
// Blocks on the web, so the window asks from another thread.
std::expected<GameDetails, std::string> fetchGameDetails(const lunar::assets::RobloxWeb& web, std::uint64_t placeId);
