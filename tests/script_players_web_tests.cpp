// Players' web lookups from scripts, answered by the fake transport, and the
// local player's identity in Play Solo.

#include "fake_web.hpp"
#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/assets/Web.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace lunar;
using namespace fakeweb;

namespace {

void answerUsers(FakeServer& server)
{
    server.answers["https://avatar.roblox.com/v2/avatar/avatar-fetch/?placeId=0&userId=1"] = {
        .body
        = R"({"resolvedAvatarType":"R15","assetAndAssetTypeIds":[{"assetId":607702162,"assetTypeId":8},{"assetId":12995020128,"assetTypeId":27}],"animationAssetIds":{"idle":2510235063},"bodyColor3s":{"torsoColor3":"0D69AC"},"scales":{"height":1.05},"emotes":[]})"};
    server.answers["https://avatar.roblox.com/v2/avatar/users/1/avatar"] = {
        .body
        = R"({"scales":{"height":1.0},"playerAvatarType":"R15","bodyColors":{"torsoColorId":23},"assets":[{"id":607702162,"name":"Roblox Baseball Cap","assetType":{"id":8,"name":"Hat"},"currentVersionId":1171146069}],"defaultShirtApplied":false,"emotes":[]})"};
    server.answers["https://users.roblox.com/v1/users/156"] = {.body = R"({"id":156,"name":"builderman"})"};
    server.answers["https://users.roblox.com/v1/usernames/users"]
        = {.body = R"({"data":[{"requestedUsername":"builderman","id":156,"name":"builderman"}]})"};
}

} // namespace

TEST(script_players_web_lookups)
{
    FakeServer server;
    answerUsers(server);
    Harness h;
    h.context.setWeb(std::make_shared<const assets::RobloxWeb>(server.transport(), fakeLogin()));

    CHECK_EQ(h.print(R"(
        local d = game:GetService("Players"):GetHumanoidDescriptionFromUserId(1)
        print(d.ClassName, d.Torso, d.HatAccessory, d.IdleAnimation, d.HeightScale)
    )"),
             "HumanoidDescription 12995020128 607702162 2510235063 1.0499999523162842");
    CHECK_EQ(h.print(R"(
        local info = game:GetService("Players"):GetCharacterAppearanceInfoAsync(1)
        print(info.playerAvatarType, info.assets[1].assetType.name, info.bodyColors.torsoColorId)
    )"),
             "R15 Hat 23");
    // The name lookups yield while a worker asks; two at once share one
    // request, and the answer is kept, so asking again doesn't yield.
    h.lines.clear();
    h.run(R"(
        local Players = game:GetService("Players")
        task.spawn(function() print("a", Players:GetNameFromUserIdAsync(156)) end)
        task.spawn(function() print("b", Players:GetNameFromUserIdAsync(156)) end)
        task.spawn(function() print("id", Players:GetUserIdFromNameAsync("builderman")) end)
        task.spawn(function()
            local ok, message = pcall(Players.GetNameFromUserIdAsync, Players, 999)
            print("missing", ok, message:find("Players:GetNameFromUserIdAsync%(%) failed") ~= nil)
        end)
        print("asked")
    )");
    CHECK_EQ(h.lines.size(), std::size_t{1});
    for (int i = 0; i < 2000 && h.lines.size() < 5; ++i) {
        h.steps(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::ranges::sort(h.lines);
    CHECK_EQ(h.lines.size(), std::size_t{5});
    if (h.lines.size() == 5) {
        CHECK_EQ(h.lines[0], "a builderman");
        CHECK_EQ(h.lines[1], "asked");
        CHECK_EQ(h.lines[2], "b builderman");
        CHECK_EQ(h.lines[3], "id 156");
        CHECK_EQ(h.lines[4], "missing false true");
    }
    {
        const std::scoped_lock lock(server.mutex);
        CHECK_EQ(std::ranges::count(server.seen, std::string("https://users.roblox.com/v1/users/156"),
                                    &assets::HttpRequest::url),
                 1);
    }
    CHECK_EQ(h.print(R"(print(game:GetService("Players"):GetNameFromUserIdAsync(156)))"), "builderman");

    CHECK(h.error(R"(game:GetService("Players"):GetHumanoidDescriptionFromUserId(-1))")
              .contains("Players:GetHumanoidDescriptionFromUserId() got invalid userId"));
    CHECK(h.error(R"(game:GetService("Players"):GetCharacterAppearanceInfoAsync(0))")
              .contains("must be called with a positive userId"));
    CHECK(h.error(R"(game:GetService("Players"):GetHumanoidDescriptionFromUserId(2))")
              .contains("Players:GetHumanoidDescriptionFromUserId() failed because"));
    // None of it carried the login.
    for (const assets::HttpRequest& request : server.seen)
        CHECK(!hasHeader(request, "Cookie: "));
    server.checkNoLeaks();
}

TEST(script_play_session_local_user)
{
    auto game = DataModel::create();
    PlaySession session(game);
    session.start("builderman", 156, "Builderman");
    Player* player = session.localPlayer();
    CHECK(player != nullptr);
    CHECK_EQ(player->name(), "builderman");
    CHECK_EQ(player->displayName(), "Builderman");
    CHECK_EQ(player->userId(), 156);
    CHECK_EQ(player->characterAppearanceId(), 156);
    // Test players with negative ids have no appearance to load.
    Player* other = session.addPlayer("Player2", -2);
    CHECK_EQ(other->characterAppearanceId(), 0);
    CHECK_EQ(other->displayName(), "Player2");
}
