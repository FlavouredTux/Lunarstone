#include "GameDetails.hpp"

#include <lunar/assets/Json.hpp>

#include <format>

namespace json = lunar::assets::json;

std::string GameDetails::icon(int size) const
{
    return std::format("rbxthumb://type=GameIcon&id={}&w={}&h={}", universeId, size, size);
}

std::expected<std::uint64_t, std::string> parseUniverseReply(std::string_view text)
{
    const auto root = json::parse(text);
    const json::Value* id = root ? (*root)["universeId"] : nullptr;
    const auto universe = id ? id->integer() : std::nullopt;
    if (!universe || *universe <= 0)
        return std::unexpected("no universe in the reply");
    return static_cast<std::uint64_t>(*universe);
}

std::expected<GameDetails, std::string> parseGamesReply(std::string_view text)
{
    const auto root = json::parse(text);
    const json::Value* data = root ? (*root)["data"] : nullptr;
    const json::Array* games = data ? data->array() : nullptr;
    if (!games || games->empty())
        return std::unexpected("no game in the reply");
    const json::Value& game = games->front();
    GameDetails details;
    if (const json::Value* id = game["id"])
        details.universeId = static_cast<std::uint64_t>(id->integer().value_or(0));
    if (const json::Value* name = game["name"]; name && name->string())
        details.name = *name->string();
    if (const json::Value* creator = game["creator"])
        if (const json::Value* name = (*creator)["name"]; name && name->string())
            details.creator = *name->string();
    if (details.name.empty())
        return std::unexpected("the game has no name in the reply");
    return details;
}

std::expected<GameDetails, std::string> fetchGameDetails(const lunar::assets::RobloxWeb& web, std::uint64_t placeId)
{
    auto get = [&web](std::string url) -> std::expected<std::string, std::string> {
        auto reply = web.anonymous({.url = std::move(url), .headers = {"Accept: application/json"}});
        if (!reply)
            return std::unexpected(reply.error());
        if (reply->status != 200)
            return std::unexpected(std::format("HTTP {}", reply->status));
        return std::move(reply->body);
    };
    const auto universeReply = get(std::format("https://apis.roblox.com/universes/v1/places/{}/universe", placeId));
    if (!universeReply)
        return std::unexpected("the place's universe: " + universeReply.error());
    const auto universe = parseUniverseReply(*universeReply);
    if (!universe)
        return std::unexpected("the place's universe: " + universe.error());
    const auto gamesReply = get(std::format("https://games.roblox.com/v1/games?universeIds={}", *universe));
    if (!gamesReply)
        return std::unexpected("the game: " + gamesReply.error());
    auto details = parseGamesReply(*gamesReply);
    if (!details)
        return std::unexpected("the game: " + details.error());
    if (details->universeId == 0)
        details->universeId = *universe;
    return details;
}
