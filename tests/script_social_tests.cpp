#include "fake_web.hpp"
#include "framework.hpp"

#include <lunar/assets/Json.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

using namespace lunar;

namespace {
const std::string permission
    = "https://apis.roblox.com/notifications/v1/can-prompt-experience-invite?PlaceId=123&Trigger=DeveloperMultiple";
const std::string recipientPermission
    = "https://apis.roblox.com/notifications/v1/"
      "can-prompt-experience-invite?PlaceId=123&Trigger=DeveloperSingle&RecipientUserId=7";
const std::string sendUrl = "https://apis.roblox.com/notifications/v1/send-experience-invite";

struct SocialFixture {
    std::shared_ptr<fakeweb::FakeServer> serverOwner = std::make_shared<fakeweb::FakeServer>();
    fakeweb::FakeServer& server = *serverOwner;
    std::shared_ptr<DataModel> game = DataModel::create();
    std::unique_ptr<ScriptContext> context = std::make_unique<ScriptContext>(game, ScriptRole::Client);
    std::shared_ptr<Player> me = std::make_shared<Player>();
    std::shared_ptr<GameInviteSession> prompt;
    std::vector<std::string> lines;
    SocialFixture()
    {
        game->assignJob("test-job", 123);
        me->setName("Me");
        me->setUserId(42);
        me->setParent(game->getService<Players>());
        game->getService<Players>()->assignLocalPlayer(me.get());
        context->setWeb(std::make_shared<assets::RobloxWeb>(
            [keep = serverOwner, transport = server.transport()](const auto& request) { return transport(request); },
            fakeweb::fakeLogin()));
        context->setOutput([&](MessageType type, std::string_view message) {
            if (type != MessageType::Info)
                lines.emplace_back(message);
        });
        context->setGameInvitePromptHandler([&](auto session) {
            prompt = std::move(session);
            return true;
        });
        server.signedInAnswers[permission] = {200, R"({"canPrompt":true})"};
        server.signedInAnswers[recipientPermission] = {200, R"({"canPrompt":true})"};
        server.signedInAnswers["https://friends.roblox.com/v1/users/42/friends"]
            = {200, R"({"data":[{"id":7,"name":"Friend","displayName":"Buddy","isOnline":true}]})"};
        server.signedInAnswers["https://friends.roblox.com/v1/users/42/friends/online?origin=getFriendsOnline"] = {
            200,
            R"({"data":[{"id":7,"name":"Friend","displayName":"Buddy","userPresence":{"UserLocationType":"Game","placeId":123,"gameInstanceId":"friend-job","lastLocation":"The game"}}]})"};
        server.signedInAnswers[sendUrl] = {200, R"({"conversationId":"fake-conversation"})"};
        server.answers["https://users.roblox.com/v1/users/7"]
            = {200, R"({"id":7,"name":"Friend","displayName":"Buddy"})"};
    }
    ~SocialFixture()
    {
        context.reset();
        prompt.reset();
    }
    void run(std::string_view source)
    {
        const auto result = context->run(source, "SocialTest");
        CHECK(result.has_value());
    }
    template <class Predicate>
    void until(Predicate finished)
    {
        for (int i = 0; i < 2000 && !finished(); ++i) {
            context->step(0.01);
            if (prompt)
                prompt->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(finished());
    }
    void open()
    {
        run(R"(game:GetService("SocialService"):PromptGameInvite(game.Players.LocalPlayer))");
        CHECK(prompt != nullptr);
        until([&] { return !prompt->loading(); });
    }
    std::size_t posts() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(server.seen, [](const auto& request) { return request.url == sendUrl; }));
    }
};
} // namespace

TEST(social_sdk_online_friends_are_real_independent_tables_with_a_shared_fetch)
{
    SocialFixture f;
    f.run(R"(
        task.spawn(function()
            local friends = game.Players.LocalPlayer:GetFriendsOnline(200)
            assert(#friends == 1 and friends[1].VisitorId == 7)
            assert(friends[1].UserName == "Friend" and friends[1].LocationType == 4)
            assert(friends[1].PlaceId == 123 and friends[1].GameId == "friend-job")
            friends[1].UserName = "changed locally"
            print("first ready")
        end)
        task.spawn(function()
            local friends = game.Players.LocalPlayer:GetFriendsOnlineAsync(1)
            assert(friends[1].UserName == "Friend" and friends[1].IsOnline)
            print("second ready")
        end)
    )");
    f.until([&] { return f.lines.size() == 2; });
    CHECK_EQ(f.server.seen.size(), 1u);
    f.run(R"(assert(game.Players.LocalPlayer:GetFriendsOnline()[1].UserName == "Friend"))");
    CHECK_EQ(f.server.seen.size(), 1u);
    f.context->step(31);
    f.run(R"(game.Players.LocalPlayer:GetFriendsOnline(); print("refreshed"))");
    f.until([&] { return f.lines.size() == 3; });
    CHECK_EQ(f.server.seen.size(), 2u);
}

TEST(social_sdk_permission_denials_and_malformed_replies_do_not_open_a_prompt)
{
    SocialFixture f;
    f.server.signedInAnswers[permission] = {200, R"({"canPrompt":false})"};
    f.run(R"(
        if game:GetService("SocialService"):CanSendGameInviteAsync(game.Players.LocalPlayer) then
            game.SocialService:PromptGameInvite(game.Players.LocalPlayer)
        end
        print("checked")
    )");
    f.until([&] { return !f.lines.empty(); });
    CHECK_EQ(f.lines, std::vector<std::string>{"checked"});
    CHECK(!f.prompt);
    f.server.signedInAnswers[permission] = {200, "{}"};
    f.lines.clear();
    f.run(R"(print(pcall(function() return game.SocialService:CanSendGameInviteAsync(game.Players.LocalPlayer) end)))");
    f.until([&] { return !f.lines.empty(); });
    CHECK(f.lines[0].starts_with("false"));
    CHECK(!f.prompt);
    CHECK_EQ(f.posts(), 0u);
}

TEST(social_sdk_invitation_requires_an_action_and_closes_once_with_confirmed_ids)
{
    SocialFixture f;
    f.run(R"(
        game:GetService("SocialService").GameInvitePromptClosed:Connect(function(player, ids)
            assert(player == game.Players.LocalPlayer)
            print("closed", #ids, ids[1])
        end)
    )");
    f.open();
    CHECK(f.prompt->error().empty());
    CHECK_EQ(f.prompt->friends().size(), 1u);
    CHECK_EQ(f.posts(), 0u);
    f.prompt->invite(99); // absent from this prompt
    CHECK_EQ(f.posts(), 0u);
    f.prompt->invite(7);
    f.until([&] { return !f.prompt->sending(); });
    CHECK(f.prompt->status(7) == GameInviteStatus::Sent);
    CHECK_EQ(f.posts(), 1u);
    f.prompt->invite(7);
    CHECK_EQ(f.posts(), 1u);
    f.prompt->close();
    f.prompt->close();
    CHECK_EQ(f.lines, std::vector<std::string>{"closed 1 7"});
}

TEST(social_sdk_recipient_permission_changes_prevent_submission_and_allow_retry)
{
    SocialFixture f;
    f.open();
    f.server.signedInAnswers[recipientPermission] = {200, R"({"canPrompt":false})"};
    f.prompt->invite(7);
    f.until([&] { return !f.prompt->sending(); });
    CHECK(f.prompt->status(7) == GameInviteStatus::Failed);
    CHECK_EQ(f.posts(), 0u);
    f.server.signedInAnswers[recipientPermission] = {200, R"({"canPrompt":true})"};
    f.prompt->invite(7);
    f.until([&] { return !f.prompt->sending(); });
    CHECK(f.prompt->status(7) == GameInviteStatus::Sent);
    CHECK_EQ(f.posts(), 1u);
}

TEST(social_sdk_single_recipient_options_reach_the_prompt_and_submission)
{
    SocialFixture f;
    f.run(R"(
        local options = Instance.new("ExperienceInviteOptions")
        options.InviteUser = 7
        options.PromptMessage = "Join our round"
        options.InviteMessageId = "message-id"
        options.LaunchData = "private-server-choice"
        game:GetService("SocialService"):PromptGameInvite(game.Players.LocalPlayer, options)
    )");
    CHECK(f.prompt);
    f.until([&] { return !f.prompt->loading(); });
    CHECK_EQ(f.prompt->options().promptMessage, "Join our round");
    CHECK_EQ(f.prompt->options().inviteUser, 7);
    CHECK_EQ(f.prompt->friends().size(), 1u);
    f.prompt->invite(7);
    f.until([&] { return !f.prompt->sending(); });
    CHECK(f.prompt->status(7) == GameInviteStatus::Sent);
    auto body = assets::json::parse(f.server.seen.back().body);
    CHECK(body.has_value());
    CHECK_EQ(*(*body)["launchData"]->string(), "private-server-choice");
    CHECK_EQ(*(*body)["trigger"]->string(), "DeveloperSingle");
}

TEST(social_sdk_session_teardown_prevents_later_invitation_actions)
{
    SocialFixture f;
    f.open();
    f.context.reset();
    const auto before = f.server.seen.size();
    f.prompt->invite(7);
    CHECK(f.prompt->closed());
    CHECK_EQ(f.server.seen.size(), before);
    CHECK_EQ(f.posts(), 0u);
}

TEST(social_sdk_delegates_to_a_registered_core_frontend)
{
    SocialFixture f;
    f.context->setCoreScriptAccess();
    f.run(R"(
        local social = game:GetService("SocialService")
        social.PromptInviteRequested:Connect(function(player, options)
            assert(player == game.Players.LocalPlayer and options == nil)
            print("core prompt")
        end)
        social:PromptGameInvite(game.Players.LocalPlayer)
        social.GameInvitePromptClosed:Connect(function(player, ids) print("closed", #ids) end)
        social:InvokeGameInvitePromptClosed(game.Players.LocalPlayer, {})
    )");
    CHECK_EQ(f.lines, (std::vector<std::string>{"core prompt", "closed 0"}));
    CHECK(!f.prompt);
    CHECK(f.server.seen.empty());
}
