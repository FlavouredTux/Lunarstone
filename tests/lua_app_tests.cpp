// What the client's own Lua app asks of the engine (script/src/AppApi.cpp,
// Localization.cpp): localization tables, legacy enum names, fast flags from
// the client settings, the message bus and MemStorageService.

#include "fake_web.hpp"
#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace lunar;

namespace {

void awaitOutput(Harness& h)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (h.lines.empty() && std::chrono::steady_clock::now() < deadline) {
        h.steps(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST(lua_app_avatar_rules_are_fetched_without_blocking_the_frame)
{
    fakeweb::FakeServer server;
    server.answers["https://avatar.roblox.com/v1/avatar-rules"]
        = {.body = R"({"scales":{"height":{"min":0.9,"max":1.05}},"wearableAssetTypes":[{"id":8,"maxNumber":3}],
                    "emotesEnabledForUser":false,"defaultClothingAssetLists":{"defaultShirtAssetIds":[123]}})"};
    Harness h;
    h.context.setWeb(std::make_shared<const assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
    h.run(R"(
        local rules = game:GetService("AvatarEditorService"):GetAvatarRulesAsync()
        assert(rules.Scales.Height.Min == 0.9)
        assert(rules.WearableAssetTypes[1].MaxNumber == 3)
        assert(rules.DefaultClothingAssetLists.DefaultShirtAssetIds[1] == 123)
        assert(rules.EmotesEnabledForUser == false)
        print("rules ready")
    )");
    CHECK(h.lines.empty());
    awaitOutput(h);
    CHECK_EQ(h.lines, (std::vector<std::string>{"rules ready"}));
    CHECK_EQ(server.seen.size(), 1u);
    CHECK(!fakeweb::hasHeader(server.seen[0], "Cookie: "));
    server.checkNoLeaks();
}

TEST(lua_app_profile_configuration_reads_the_selected_assets)
{
    fakeweb::FakeServer server;
    server
        .answers["https://avatar.roblox.com/v4/avatar/users/123/?selectionTypes=Background&selectionTypes=ProfileFrame"]
        = {.body = R"({"avatarConfigurations":{"background":{"backgroundAsset":{"id":9876543210}},
                                              "profileFrame":{"frameAsset":{"id":456}}}})"};
    server
        .answers["https://avatar.roblox.com/v4/avatar/users/124/?selectionTypes=Background&selectionTypes=ProfileFrame"]
        = {.body = R"({"avatarConfigurations":{"background":null,"profileFrame":null}})"};
    Harness h;
    h.context.setCoreScriptAccess();
    h.context.setWeb(std::make_shared<const assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
    h.run(R"(
        local players = game:GetService("Players")
        local profile = players:GetProfileConfigurationFromUserIdAsync(123)
        assert(profile.BackgroundAssetId == 9876543210 and profile.FrameAssetId == 456)
        local empty = players:GetProfileConfigurationFromUserIdAsync(124)
        assert(next(empty) == nil)
        print("profile ready")
    )");
    awaitOutput(h);
    CHECK_EQ(h.lines, (std::vector<std::string>{"profile ready"}));
    CHECK(h.error("game:GetService('Players'):GetProfileConfigurationFromUserIdAsync(-1)").contains("invalid userId"));
    server.checkNoLeaks();
}

TEST(lua_app_avatar_fetch_failures_reject_the_waiting_call)
{
    for (const auto& [status, body] : std::vector<std::pair<long, std::string>>{
             {503, "unavailable"}, {200, "not json"}, {200, R"({"unrelated":true})"}}) {
        fakeweb::FakeServer server;
        server.answers["https://avatar.roblox.com/v1/avatar-rules"] = {.status = status, .body = body};
        Harness h;
        h.context.setWeb(std::make_shared<const assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
        h.run(R"(
            local ok, err = pcall(function() return game:GetService("AvatarEditorService"):GetAvatarRulesAsync() end)
            assert(not ok and type(err) == "string")
            print("rejected")
        )");
        awaitOutput(h);
        CHECK_EQ(h.lines, (std::vector<std::string>{"rejected"}));
    }
}

TEST(lua_app_image_readiness_updates_loading_placeholders)
{
    Harness h;
    // The app can start observing images before its window is created.
    h.run(R"(
        local image = Instance.new("ImageLabel")
        image.Image = "rbxasset://ready.png"
        assert(not image.IsLoaded)
        local seen = {}
        local changed = image:GetPropertyChangedSignal("IsLoaded")
        changed:Connect(function() table.insert(seen, image.IsLoaded) end)
        assert(not pcall(function() image.IsLoaded = true end))
        changed:Wait()
        assert(image.IsLoaded and #seen == 1 and seen[1] == true)
        image.Image = "rbxasset://pending.png"
        assert(not image.IsLoaded)
        changed:Wait()
        assert(#seen == 2 and seen[2] == false)
        task.wait()
        assert(#seen == 2)
        print("images ready")
    )");
    bool ready = false;
    h.context.setImageLoadedChecker([&](std::string_view id) { return ready && id == "rbxasset://ready.png"; });
    h.steps(2);
    CHECK(h.lines.empty());
    ready = true;
    h.steps(4);
    CHECK_EQ(h.lines, (std::vector<std::string>{"images ready"}));
}

TEST(lua_app_catalog_search_and_pagination)
{
    fakeweb::FakeServer server;
    const std::string url
        = R"(https://catalog.roblox.com/v2/search/items/details?model.keyword=red%20%26%20blue&limit=10&model.sortType=Relevance&model.sortAggregation=AllTime&model.categoryFilter=Recommended&model.salesTypeFilter=All&model.includeNotForSale=false)";
    server.answers[url]
        = {.body = R"({"data":[{"id":123,"name":"Red Hat","itemType":"Asset"}],"nextPageCursor":"next/token="})"};
    server.answers[url + "&cursor=next%2Ftoken%3D"]
        = {.body = R"({"data":[{"id":456,"name":"Blue Hat","itemType":"Asset"}],"nextPageCursor":null})"};
    Harness h;
    h.context.setWeb(std::make_shared<const assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
    h.run(R"(
        local params = CatalogSearchParams.new()
        assert(typeof(params) == "CatalogSearchParams")
        assert(params.SortType == Enum.CatalogSortType.Relevance)
        params.SearchKeyword = "red & blue"
        params.CategoryFilter = Enum.CatalogCategoryFilter.Recommended
        params.Limit = 10
        assert(not pcall(function() params.SortType = Enum.Material.Plastic end))
        local pages = game:GetService("AvatarEditorService"):SearchCatalogAsync(params)
        assert(pages:IsA("Pages") and pages.ClassName == "CatalogPages")
        assert(not pages.IsFinished)
        local first = pages:GetCurrentPage()
        assert(first[1].Id == 123 and first[1].ItemType == "Asset")
        first[1].Name = "changed locally"
        assert(pages:GetCurrentPage()[1].Name == "Red Hat")
        pages:AdvanceToNextPageAsync()
        assert(pages.IsFinished and pages:GetCurrentPage()[1].Id == 456)
        assert(not pcall(function() pages:AdvanceToNextPageAsync() end))
        print("catalog ready")
    )");
    awaitOutput(h);
    CHECK_EQ(h.lines, (std::vector<std::string>{"catalog ready"}));
    CHECK_EQ(server.seen.size(), 2u);
    server.checkNoLeaks();
}

TEST(lua_app_avatar_description_accessories_preserve_rigid_slots)
{
    Harness h;
    h.run(R"(
        local description = Instance.new("HumanoidDescription")
        description.HatAccessory = "111"
        description:SetAccessories({
            {AssetId = 222, AccessoryType = Enum.AccessoryType.Shirt, Order = 1, Puffiness = 0.6}
        }, false)
        local layered = description:GetAccessories(false)
        assert(#layered == 1 and layered[1].AssetId == 222 and layered[1].IsLayered)
        assert(#description:GetAccessories(true) == 2 and description.HatAccessory == "111")
        description:SetAccessories({
            {AssetId = 333, AccessoryType = Enum.AccessoryType.Hair, IsLayered = false}
        }, true)
        assert(description.HatAccessory == "" and description.HairAccessory == "333")
        assert(#description:GetAccessories(true) == 1 and #description:GetAccessories(false) == 0)
        description:SetAccessories({}, false)
        assert(description.HairAccessory == "333")
        assert(not pcall(function()
            description:SetAccessories({{AssetId = -1, AccessoryType = Enum.AccessoryType.Hat}}, true)
        end))
        assert(description.HairAccessory == "333")
    )");
}

TEST(lua_app_avatar_models_apply_descriptions_and_return_independent_snapshots)
{
    const auto content = assets::AssetConfig::fromEnvironment().contentDir;
    if (!std::filesystem::exists(content / "avatar/character.rbxm"))
        // NOLINTNEXTLINE(bugprone-std-exception-baseclass): the test runner catches lt::Skipped.
        SKIP("client rig content is not installed");
    Harness h;
    h.run(R"(
        local description = Instance.new("HumanoidDescription")
        description.HeadColor = Color3.new(0.2, 0.3, 0.4)
        local model = game:GetService("Players"):CreateHumanoidModelFromDescriptionAsync(description, Enum.HumanoidRigType.R6)
        assert(model.Parent == nil and model.Humanoid.RigType == Enum.HumanoidRigType.R6)
        local snapshot = model.Humanoid:GetAppliedDescription()
        assert(snapshot.HeadColor == description.HeadColor)
        snapshot.HeadColor = Color3.new(0.8, 0.5, 0.2)
        assert(model.Humanoid:GetAppliedDescription().HeadColor == description.HeadColor)
        model.Humanoid:ApplyDescriptionAsync(snapshot)
        assert(model.Head.Color == snapshot.HeadColor)
        assert(model.Humanoid:GetAppliedDescription().HeadColor == snapshot.HeadColor)
        print("avatar ready")
    )");
    CHECK(h.lines.empty());
    awaitOutput(h);
    CHECK_EQ(h.lines, (std::vector<std::string>{"avatar ready"}));
}

TEST(lua_app_localization_tables_format_by_key)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local t = Instance.new("LocalizationTable")
        t:SetEntries({
            {Key = "Greet", Source = "Hello {name}", Values = {["en-us"] = "Hello {name}", de = "Hallo {name}"}},
            {Key = "Count", Values = {["en-us"] = "{1:int} items"}},
        })
        local en, de = t:GetTranslator("en-us"), t:GetTranslator("DE")
        print(en:FormatByKey("Greet", {name = "Ann"}), de:FormatByKey("Greet", {name = "Ann"}),
              en:FormatByKey("Count", {2.6}), #t:GetEntries(), (pcall(de.FormatByKey, de, "Count")),
              en:Translate(game, "Hello {name}"), t:GetString("de", "Greet"))
    )"),
             "Hello Ann Hallo Ann 3 items 2 false Hello {name} Hallo {name}");
}

TEST(lua_app_enum_legacy_names)
{
    Harness h;
    CHECK_EQ(
        h.print("print(Enum.Font.GothamSemibold == Enum.Font.GothamMedium, Enum.RaycastFilterType.Blacklist.Name)"),
        "true Exclude");
}

// Served values win over the Lua code's defaults; engine variables nobody
// serves keep the binary's.
TEST(lua_app_fast_flags_take_the_client_settings)
{
    CHECK_EQ(ScriptContext::loadClientSettings(
                 R"({"applicationSettings": {"FFlagLuaAppTestServed": "True", "DFIntLuaAppTestNumber": "42",
                     "FStringLuaAppTestText": "hi \"there\"", "FLogSkipped": 3}, "other": [1, {"a": "b"}]})"),
             std::optional<std::size_t>(3));
    CHECK(!ScriptContext::loadClientSettings("not json").has_value());
    Harness h;
    h.context.setCoreScriptAccess();
    CHECK_EQ(h.print(R"(
        print(game:DefineFastFlag("LuaAppTestServed", false), game:DefineFastInt("LuaAppTestNumber", 1),
              game:DefineFastString("LuaAppTestText", ""), game:DefineFastFlag("LuaAppTestUnserved", true),
              settings():GetFVariable("LuaAppErrorReporterRateLimitThreshold"))
    )"),
             "true 42 hi \"there\" true 10");
}

TEST(lua_app_message_bus_publishes_to_subscribers)
{
    Harness h;
    h.context.setCoreScriptAccess();
    CHECK_EQ(h.print(R"(
        local bus = game:GetService("MessageBusService")
        local id = bus:GetMessageId("Test", "ping")
        local heard = {}
        local connection = bus:Subscribe(id, function(params) table.insert(heard, params.n) end, false, false)
        bus:Publish(id, {n = 1})
        bus:Publish(id, {n = 2})
        connection:Disconnect()
        bus:Publish(id, {n = 3})
        print(id, table.concat(heard, ","), bus:GetLast(id).n)
    )"),
             "Test.ping 1,2 3");
}

TEST(lua_app_mem_storage_binds_keys)
{
    Harness h;
    h.context.setCoreScriptAccess();
    CHECK_EQ(h.print(R"(
        local mem = game:GetService("MemStorageService")
        mem:SetItem("LuaAppTestKey", "v1")
        local seen = {}
        mem:Bind("LuaAppTestKey", function(value) table.insert(seen, value) end)
        mem:SetItem("LuaAppTestKey", "v2")
        print(mem:HasItem("LuaAppTestKey"), mem:GetItem("LuaAppTestKey"), mem:GetItem("LuaAppTestMissing", "dflt"),
              table.concat(seen, ","), mem:RemoveItem("LuaAppTestKey"), mem:HasItem("LuaAppTestKey"))
    )"),
             "true v2 dflt v2 true false");
}

TEST(lua_app_native_configuration_reaches_policy_subscribers)
{
    Harness h;
    h.context.setCoreScriptAccess();
    h.context.setMemStorageItem("GUAC:123:app-policy", R"({"MorePageContent":{"Navigation":["Settings"]}})");
    h.run(R"(
        local mem = game:GetService("MemStorageService")
        local http = game:GetService("HttpService")
        local function readPolicy(value)
            print(http:JSONDecode(value).MorePageContent.Navigation[1])
        end
        readPolicy(mem:GetItem("GUAC:123:app-policy"))
        mem:Bind("GUAC:123:app-policy", readPolicy)
    )");
    h.context.setMemStorageItem("GUAC:123:app-policy", R"({"MorePageContent":{"Navigation":["Catalog"]}})");
    CHECK_EQ(h.lines, (std::vector<std::string>{"Settings", "Catalog"}));
}

// The app's navigation rail: SetGlobalGuiInset adds to GetGuiInset (under
// the harness's top bar) and turns away negative sides.
TEST(lua_app_global_gui_inset)
{
    Harness h;
    h.context.setCoreScriptAccess();
    CHECK_EQ(h.print(R"(
        local gui = game:GetService("GuiService")
        gui:SetGlobalGuiInset(64, 0, 0, 0)
        local topLeft, bottomRight = gui:GetGuiInset()
        print(topLeft, bottomRight, (pcall(gui.SetGlobalGuiInset, gui, -1, 0, 0, 0)))
    )"),
             "64, 36 0, 0 false");
}

// ResetPropertyToDefault: the class's default back, and the reset said
// (the StyleEngine listens); a property the class lacks is an error, which
// ReactRoblox catches.
TEST(lua_app_reset_property_to_default)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local button = Instance.new("ImageButton")
        button.BackgroundTransparency = 0.5
        button:ResetPropertyToDefault("BackgroundTransparency")
        print(button.BackgroundTransparency, (pcall(button.ResetPropertyToDefault, button, "Nope")))
    )"),
             "0 false");
}

// What the app launches reaches the host, by ExperienceService and by the bus.
TEST(lua_app_launches_reach_the_host)
{
    Harness h;
    h.context.setCoreScriptAccess();
    std::vector<ScriptContext::LaunchRequest> launched;
    h.context.setLaunchHandler([&](const ScriptContext::LaunchRequest& request) { launched.push_back(request); });
    h.run(R"(
        game:GetService("ExperienceService"):LaunchExperience({placeId = 1818, referralPage = "Home"})
        local bus = game:GetService("MessageBusService")
        bus:Publish(bus:GetMessageId("Game", "launch"), {placeId = 123456001, userId = 42, joinAttemptId = "abc"})
    )");
    CHECK_EQ(launched.size(), 2u);
    CHECK_EQ(launched[0].placeId, 1818u);
    CHECK_EQ(launched[1].placeId, 123456001u);
    CHECK_EQ(launched[1].userId, 42u);
    CHECK_EQ(launched[1].joinAttemptId, std::string("abc"));
}

TEST(lua_app_player_policy_uses_the_actual_account_response_and_caches_it)
{
    fakeweb::FakeServer server;
    const std::string url = "https://apis.roblox.com/player-policy-service/v1/player-policy-client";
    server.signedInAnswers[url] = {200, R"({"isPaidItemTradingAllowed":false,"arePaidRandomItemsRestricted":true,
        "areAdsAllowed":false,"allowedExternalLinkReferences":[]})", ""};
    Harness h;
    auto* players = h.game->getService<Players>();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(players);
    players->assignLocalPlayer(player.get());
    h.context.setWeb(std::make_shared<assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
    h.run(R"(
        local service = game:GetService("PolicyService")
        local policy = service:GetPolicyInfoForPlayerAsync(game.Players.LocalPlayer)
        assert(policy.IsPaidItemTradingAllowed == false)
        assert(policy.ArePaidRandomItemsRestricted == true)
        assert(policy.AreAdsAllowed == false and #policy.AllowedExternalLinkReferences == 0)
        local cached = service:GetPolicyInfoForPlayerAsync(game.Players.LocalPlayer)
        assert(cached.ArePaidRandomItemsRestricted == true)
        print("policy ready")
    )");
    awaitOutput(h);
    CHECK_EQ(h.lines, std::vector<std::string>{"policy ready"});
    CHECK_EQ(server.seen.size(), 1u);
    CHECK(fakeweb::hasHeader(server.seen.front(), "Cookie: "));
    server.checkNoLeaks();
    h.lines.clear();
    auto other = std::make_shared<Player>();
    other->setName("Other");
    other->setUserId(7);
    other->setParent(players);
    h.run(R"(
        local other = game.Players.Other
        assert(not pcall(function() game:GetService("PolicyService"):GetPolicyInfoForPlayerAsync(other) end))
        print("other rejected")
    )");
    CHECK_EQ(h.lines, std::vector<std::string>{"other rejected"});
    CHECK_EQ(server.seen.size(), 1u);
}

TEST(lua_app_player_policy_rejects_incomplete_responses)
{
    fakeweb::FakeServer server;
    server.signedInAnswers["https://apis.roblox.com/player-policy-service/v1/player-policy-client"] = {
        200, R"({"isPaidItemTradingAllowed":true})", ""};
    Harness h;
    auto* players = h.game->getService<Players>();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(players);
    players->assignLocalPlayer(player.get());
    h.context.setWeb(std::make_shared<assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin()));
    h.run(R"(
        local ok, problem = pcall(function()
            game:GetService("PolicyService"):GetPolicyInfoForPlayerAsync(game.Players.LocalPlayer)
        end)
        assert(not ok and problem:find("incomplete"))
        print("incomplete rejected")
    )");
    awaitOutput(h);
    CHECK_EQ(h.lines, std::vector<std::string>{"incomplete rejected"});
    server.checkNoLeaks();
}
