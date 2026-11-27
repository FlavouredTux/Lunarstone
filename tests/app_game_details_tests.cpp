#include "framework.hpp"
#include "GameDetails.hpp"

#include <lunar/assets/Web.hpp>

#include <expected>
#include <string>
#include <vector>

TEST(game_details_universe_reply)
{
    CHECK(parseUniverseReply(R"({"universeId":123456002})") == 123456002u);
    CHECK(!parseUniverseReply(R"({"universeId":null})"));
    CHECK(!parseUniverseReply(R"({"errors":[{"code":0,"message":"NotFound"}]})"));
    CHECK(!parseUniverseReply("not json"));
}

TEST(game_details_games_reply)
{
    const auto details = parseGamesReply(R"({"data":[{"id":123456002,"rootPlaceId":123456001,"name":"Sample Experience",
        "description":"...","sourceName":"Sample Experience","creator":{"id":12345,"name":"Sample Creator","type":"Group",
        "isRNVAccount":false,"hasVerifiedBadge":true},"playing":1000}]})");
    CHECK(details.has_value());
    CHECK(details->universeId == 123456002u);
    CHECK(details->name == "Sample Experience");
    CHECK(details->creator == "Sample Creator");
    CHECK(details->icon() == "rbxthumb://type=GameIcon&id=123456002&w=256&h=256");
    CHECK(details->icon(150) == "rbxthumb://type=GameIcon&id=123456002&w=150&h=150");
    CHECK(!parseGamesReply(R"({"data":[]})"));
    CHECK(!parseGamesReply(R"({"data":[{"id":1}]})"));
}

// The place's universe first, then the game; anonymous both times.
TEST(game_details_fetch_asks_for_the_universe_then_the_game)
{
    std::vector<std::string> urls;
    lunar::assets::RobloxWeb web([&urls](const lunar::assets::HttpRequest& request)
                                     -> std::expected<lunar::assets::HttpResponse, std::string> {
        urls.push_back(request.url);
        for (const std::string& header : request.headers)
            if (header.starts_with("Cookie"))
                return std::unexpected("sent a login");
        if (request.url.starts_with("https://apis.roblox.com/universes/v1/places/123456001/universe"))
            return lunar::assets::HttpResponse{.status = 200, .body = R"({"universeId":123456002})"};
        if (request.url == "https://games.roblox.com/v1/games?universeIds=123456002")
            return lunar::assets::HttpResponse{
                .status = 200, .body = R"({"data":[{"id":123456002,"name":"Sample Experience","creator":{"name":"Sample Creator"}}]})"};
        return lunar::assets::HttpResponse{.status = 404};
    });
    const auto details = fetchGameDetails(web, 123456001u);
    CHECK(details.has_value());
    CHECK(details->name == "Sample Experience");
    CHECK(urls.size() == 2u);
    CHECK(!fetchGameDetails(web, 1u)); // a 404 for the universe
}
