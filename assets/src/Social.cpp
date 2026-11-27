#include <lunar/assets/Social.hpp>

#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>

#include <algorithm>
#include <format>
#include <set>

namespace lunar::assets {
namespace {

using Document = std::expected<json::Value, std::string>;

Document readResponse(std::expected<HttpResponse, std::string> response)
{
    if (!response)
        return std::unexpected(response.error());
    if (response->status != 200)
        return std::unexpected(std::format("HTTP {}", response->status));
    auto value = json::parse(response->body);
    if (!value || !value->object())
        return std::unexpected("invalid JSON response");
    return *value;
}

std::string stringOf(const json::Value& value, std::string_view field)
{
    const auto* member = value[field];
    return member && member->string() ? *member->string() : std::string{};
}

std::int64_t integerOf(const json::Value& value, std::string_view field)
{
    const auto* member = value[field];
    return member ? member->integer().value_or(0) : 0;
}

std::optional<GameInviteFriend> readFriend(const json::Value& value, bool withPresence)
{
    const auto id = integerOf(value, "id");
    if (id <= 0)
        return std::nullopt;
    GameInviteFriend result;
    result.userId = id;
    result.username = stringOf(value, "name");
    result.displayName = stringOf(value, "displayName");
    if (result.displayName.empty())
        result.displayName = result.username;
    if (const auto* online = value["isOnline"]; online && online->boolean())
        result.online = *online->boolean();
    if (withPresence) {
        if (const auto* presence = value["userPresence"]; presence && presence->object()) {
            // The authenticated online-friends endpoint supplies only online
            // entries. Native 0.742 parser 0x1041b3350 maps these location names.
            result.online = true;
            const std::string location = stringOf(*presence, "UserLocationType");
            if (location == "MobileWebsite")
                result.locationType = 0;
            else if (location == "iOS" || location == "Android")
                result.locationType = 1;
            else if (location == "Page")
                result.locationType = 2;
            else if (location == "Studio")
                result.locationType = 3;
            else if (location == "Game")
                result.locationType = 4;
            else if (location == "Xbox")
                result.locationType = 5;
            else if (location == "CloudEdit")
                result.locationType = 6;
            else
                return std::nullopt;
            result.placeId = integerOf(*presence, "placeId");
            result.universeId = integerOf(*presence, "universeId");
            result.gameId = stringOf(*presence, "gameInstanceId");
            result.lastLocation = stringOf(*presence, "lastLocation");
            result.lastOnline = stringOf(*presence, "lastOnline");
        } else
            return std::nullopt;
    }
    return result;
}

std::expected<std::vector<GameInviteFriend>, std::string> readFriends(Document document, bool withPresence)
{
    if (!document)
        return std::unexpected(document.error());
    const auto* data = (*document)["data"];
    if (!data || !data->array())
        return std::unexpected("friend response has no data array");
    std::vector<GameInviteFriend> result;
    std::set<std::int64_t> seen;
    for (const auto& item : *data->array()) {
        if (const auto* deleted = item["isDeleted"]; deleted && deleted->boolean().value_or(false))
            continue;
        auto person = readFriend(item, withPresence);
        if (!person)
            return std::unexpected("friend response contains an invalid user or presence");
        if (seen.insert(person->userId).second)
            result.push_back(std::move(*person));
        if (withPresence && result.size() == 200)
            break;
    }
    return result;
}

std::expected<std::vector<GameInviteFriend>, std::string>
fillNames(const RobloxWeb& web, std::expected<std::vector<GameInviteFriend>, std::string> result)
{
    if (!result)
        return result;
    std::vector<std::size_t> missing;
    for (std::size_t i = 0; i < result->size(); ++i)
        if ((*result)[i].username.empty())
            missing.push_back(i);
    for (std::size_t from = 0; from < missing.size(); from += 100) {
        const auto end = std::min(from + 100, missing.size());
        std::string ids;
        for (std::size_t i = from; i < end; ++i) {
            if (!ids.empty())
                ids += ',';
            ids += std::to_string((*result)[missing[i]].userId);
        }
        auto document
            = readResponse(web.anonymous({.method = "POST",
                                          .url = "https://users.roblox.com/v1/users",
                                          .headers = {"Content-Type: application/json"},
                                          .body = "{\"userIds\":[" + ids + "],\"excludeBannedUsers\":false}"}));
        if (!document)
            return std::unexpected(document.error());
        auto names = readFriends(std::move(document), false);
        if (!names)
            return std::unexpected(names.error());
        for (std::size_t i = from; i < end; ++i) {
            auto& person = (*result)[missing[i]];
            const auto found = std::ranges::find(*names, person.userId, &GameInviteFriend::userId);
            if (found == names->end() || found->username.empty())
                return std::unexpected("friend profile response is incomplete");
            person.username = found->username;
            person.displayName = found->displayName;
        }
    }
    return result;
}

std::string quote(std::string_view value)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const char character : value) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

} // namespace

std::expected<bool, std::string> canPromptGameInvite(const RobloxWeb& web, std::int64_t placeId, std::int64_t recipient)
{
    if (placeId <= 0 || recipient < 0)
        return std::unexpected("game invitation requires a valid place and recipient");
    // Native URL builder 0x100a7d300 (RobloxPlayer 0.742).
    std::string url
        = std::format("https://apis.roblox.com/notifications/v1/can-prompt-experience-invite?PlaceId={}&Trigger={}",
                      placeId, recipient ? "DeveloperSingle" : "DeveloperMultiple");
    if (recipient)
        url += std::format("&RecipientUserId={}", recipient);
    auto document = readResponse(web.asClientApp({.url = std::move(url)}));
    if (!document)
        return std::unexpected(document.error());
    const auto* permission = (*document)["canPrompt"];
    if (!permission || !permission->boolean())
        return std::unexpected("invitation permission response has no canPrompt boolean");
    return *permission->boolean();
}

std::expected<std::vector<GameInviteFriend>, std::string> getFriends(const RobloxWeb& web, std::int64_t userId)
{
    if (userId <= 0)
        return std::unexpected("friends require a positive user id");
    return fillNames(web,
                     readFriends(readResponse(web.asClientApp(
                                     {.url = std::format("https://friends.roblox.com/v1/users/{}/friends", userId)})),
                                 false));
}

std::expected<std::vector<GameInviteFriend>, std::string> getFriendsOnline(const RobloxWeb& web, std::int64_t userId)
{
    if (userId <= 0)
        return std::unexpected("online friends require a positive user id");
    return fillNames(
        web, readFriends(
                 readResponse(web.asClientApp(
                     {.url = std::format(
                          "https://friends.roblox.com/v1/users/{}/friends/online?origin=getFriendsOnline", userId)})),
                 true));
}

std::expected<GameInviteFriend, std::string> getInviteUser(const RobloxWeb& web, std::int64_t userId)
{
    if (userId <= 0)
        return std::unexpected("invite recipient requires a positive user id");
    auto value = readResponse(web.anonymous({.url = std::format("https://users.roblox.com/v1/users/{}", userId)}));
    if (!value)
        return std::unexpected(value.error());
    auto person = readFriend(*value, false);
    if (!person || person->userId != userId)
        return std::unexpected("user response did not match the invite recipient");
    return *person;
}

std::expected<void, std::string> sendGameInvite(const RobloxWeb& web, std::int64_t placeId, std::int64_t recipient,
                                                const GameInviteOptions& options, std::string_view idempotencyKey,
                                                std::function<bool()> cancelled)
{
    if (placeId <= 0 || recipient <= 0 || (options.inviteUser && options.inviteUser != recipient))
        return std::unexpected("invalid game invitation recipient");
    if (cancelled && cancelled())
        return std::unexpected("cancelled");
    auto allowed = canPromptGameInvite(web, placeId, recipient);
    if (!allowed)
        return std::unexpected(allowed.error());
    if (!*allowed)
        return std::unexpected("Invitations to this recipient are not available.");
    if (cancelled && cancelled())
        return std::unexpected("cancelled");
    // Shipped UniversalApp 0.742 module cVg. This function is called only
    // after the native prompt receives an explicit Invite button action.
    const std::string body
        = std::format("{{\"recipientUserId\":{},\"inviteMessageId\":{},\"placeId\":{}," "\"launchData\":{},\"trigger\":"
                                                                                        "{},\"idempotencyKey\":{}}}",
                      recipient, quote(options.inviteMessageId), placeId, quote(options.launchData),
                      quote(options.inviteUser ? "DeveloperSingle" : "DeveloperMultiple"), quote(idempotencyKey));
    auto reply = web.asClientApp({.method = "POST",
                                  .url = "https://apis.roblox.com/notifications/v1/send-experience-invite",
                                  .headers = {"Content-Type: application/json", "Accept: application/json"},
                                  .body = body});
    if (!reply)
        return std::unexpected(reply.error());
    if (reply->status < 200 || reply->status >= 300)
        return std::unexpected(std::format("HTTP {}", reply->status));
    return {};
}

} // namespace lunar::assets
