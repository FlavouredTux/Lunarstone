#include "fake_web.hpp"
#include "framework.hpp"

#include <lunar/assets/Json.hpp>
#include <lunar/assets/Social.hpp>

#include <algorithm>

using namespace lunar;
using namespace lunar::assets;

namespace {
constexpr auto permission
    = "https://apis.roblox.com/notifications/v1/can-prompt-experience-invite?PlaceId=123&Trigger=DeveloperMultiple";
constexpr auto recipientPermission
    = "https://apis.roblox.com/notifications/v1/"
      "can-prompt-experience-invite?PlaceId=123&Trigger=DeveloperSingle&RecipientUserId=7";
constexpr auto sendUrl = "https://apis.roblox.com/notifications/v1/send-experience-invite";
} // namespace

TEST(social_permission_uses_the_account_response_without_an_allowed_default)
{
    fakeweb::FakeServer server;
    RobloxWeb web(server.transport(), fakeweb::fakeLogin());
    for (const bool allowed : {false, true}) {
        server.signedInAnswers[permission] = {200, allowed ? R"({"canPrompt":true})" : R"({"canPrompt":false})"};
        auto result = canPromptGameInvite(web, 123);
        CHECK(result.has_value() && *result == allowed);
    }
    for (const auto body : {"{}", R"({"canPrompt":"true"})", "not JSON"}) {
        server.signedInAnswers[permission] = {200, body};
        CHECK(!canPromptGameInvite(web, 123));
    }
    server.signedInAnswers[permission] = {403, "{}"};
    CHECK(!canPromptGameInvite(web, 123));
    const auto before = server.seen.size();
    CHECK(!canPromptGameInvite(web, 0));
    CHECK(!canPromptGameInvite(web, 123, -1));
    CHECK_EQ(server.seen.size(), before);
    server.checkNoLeaks();
}

TEST(social_online_friends_map_presence_and_fill_names_from_public_profiles)
{
    fakeweb::FakeServer server;
    server.signedInAnswers["https://friends.roblox.com/v1/users/42/friends/online?origin=getFriendsOnline"] = {
        200,
        R"({"data":[{"id":7,"userPresence":{"UserLocationType":"Game","placeId":123,"universeId":456,"gameInstanceId":"job-7","lastLocation":"A game","lastOnline":"2026-10-31T00:00:00Z"}},
        {"id":8,"name":"MobileFriend","displayName":"Mobile","userPresence":{"UserLocationType":"Android"}},
        {"id":9,"name":"StudioFriend","displayName":"Studio","userPresence":{"UserLocationType":"CloudEdit"}}]})"};
    server.answers["https://users.roblox.com/v1/users"]
        = {200, R"({"data":[{"id":7,"name":"Friend7","displayName":"Friend Seven"}]})"};
    RobloxWeb web(server.transport(), fakeweb::fakeLogin());
    auto result = getFriendsOnline(web, 42);
    CHECK(result.has_value());
    CHECK_EQ(result->size(), 3u);
    CHECK_EQ((*result)[0].userId, 7);
    CHECK_EQ((*result)[0].username, "Friend7");
    CHECK_EQ((*result)[0].displayName, "Friend Seven");
    CHECK((*result)[0].online);
    CHECK_EQ((*result)[0].locationType, 4);
    CHECK_EQ((*result)[0].placeId, 123);
    CHECK_EQ((*result)[0].universeId, 456);
    CHECK_EQ((*result)[0].gameId, "job-7");
    CHECK_EQ((*result)[1].locationType, 1);
    CHECK_EQ((*result)[2].locationType, 6);
    CHECK_EQ(server.seen.size(), 2u);
    CHECK(fakeweb::hasHeader(server.seen[0], "Cookie: "));
    CHECK(!fakeweb::hasHeader(server.seen[1], "Cookie: "));
    auto namesRequest = json::parse(server.seen[1].body);
    CHECK(namesRequest && (*namesRequest)["userIds"]->array()->size() == 1);
    CHECK_EQ((*namesRequest)["userIds"]->array()->front().integer().value(), 7);
    server.signedInAnswers["https://friends.roblox.com/v1/users/42/friends/online?origin=getFriendsOnline"]
        = {200, R"({"data":[{"id":7,"name":"Friend","userPresence":{"UserLocationType":"unknown"}}]})"};
    CHECK(!getFriendsOnline(web, 42));
}

TEST(social_friend_failures_do_not_become_an_empty_friend_list)
{
    fakeweb::FakeServer server;
    RobloxWeb web(server.transport(), fakeweb::fakeLogin());
    const auto url = "https://friends.roblox.com/v1/users/42/friends";
    server.signedInAnswers[url] = {200, R"({"data":[]})"};
    auto empty = getFriends(web, 42);
    CHECK(empty && empty->empty());
    server.signedInAnswers[url] = {200, "{}"};
    CHECK(!getFriends(web, 42));
    server.signedInAnswers[url] = {503, "{}"};
    CHECK(!getFriends(web, 42));
}

TEST(social_denied_or_cancelled_invitations_never_post)
{
    fakeweb::FakeServer server;
    RobloxWeb web(server.transport(), fakeweb::fakeLogin());
    server.signedInAnswers[recipientPermission] = {200, R"({"canPrompt":false})"};
    CHECK(!sendGameInvite(web, 123, 7, {}, "test-key"));
    CHECK_EQ(server.seen.size(), 1u);
    CHECK_EQ(server.seen.front().method, "GET");
    server.signedInAnswers[recipientPermission] = {200, R"({"canPrompt":true})"};
    CHECK(!sendGameInvite(web, 123, 7, {}, "test-key", [] { return true; }));
    CHECK_EQ(server.seen.size(), 1u);
    int cancellationChecks = 0;
    CHECK(!sendGameInvite(web, 123, 7, {}, "test-key", [&] { return ++cancellationChecks == 2; }));
    CHECK_EQ(server.seen.size(), 2u);
    CHECK(std::ranges::all_of(server.seen, [](const auto& request) { return request.method == "GET"; }));
}

TEST(social_invitation_posts_the_requested_payload_with_the_normal_csrf_retry)
{
    std::vector<HttpRequest> seen;
    RobloxWeb web(
        [&](const HttpRequest& request) -> std::expected<HttpResponse, std::string> {
            seen.push_back(request);
            CHECK(fakeweb::hasHeader(request, "Cookie: "));
            CHECK(!request.followRedirects);
            if (request.url == recipientPermission)
                return HttpResponse{.status = 200, .body = R"({"canPrompt":true})"};
            CHECK_EQ(request.url, sendUrl);
            if (!fakeweb::hasHeader(request, "X-CSRF-TOKEN: fake-token"))
                return HttpResponse{.status = 403, .headers = {{"x-csrf-token", "fake-token"}}};
            return HttpResponse{.status = 200, .body = R"({"conversationId":"fake-conversation"})"};
        },
        fakeweb::fakeLogin());
    GameInviteOptions options{.inviteMessageId = "message-id", .launchData = "a\"b\\c\n雪"};
    CHECK(sendGameInvite(web, 123, 7, options, "stable-key"));
    CHECK_EQ(seen.size(), 3u);
    CHECK_EQ(seen[1].body, seen[2].body);
    auto body = json::parse(seen.back().body);
    CHECK(body.has_value());
    CHECK_EQ((*body)["recipientUserId"]->integer().value(), 7);
    CHECK_EQ(*(*body)["launchData"]->string(), options.launchData);
    CHECK_EQ(*(*body)["trigger"]->string(), "DeveloperMultiple");
    CHECK_EQ(*(*body)["idempotencyKey"]->string(), "stable-key");
}
