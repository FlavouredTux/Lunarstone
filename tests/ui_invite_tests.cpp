#include "fake_web.hpp"
#include "framework.hpp"
#include "render_gpu.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/script/ScriptContext.hpp>
#include <lunar/ui/GameInvitePrompt.hpp>
#include <lunar/ui/GameUi.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

using namespace lunar;
using namespace lunar::ui;

namespace {
struct InviteSession final : GameInviteSession {
    GameInviteOptions configuration;
    std::vector<GameInviteFriend> people{
        {.userId = 7, .username = "FriendUser", .displayName = "Buddy", .online = true},
        {.userId = 8, .username = "OtherUser", .displayName = "Other"}};
    std::vector<std::int64_t> actions;
    std::map<std::int64_t, GameInviteStatus> statuses;
    std::string failure;
    bool pending = false, finished = false;
    int closes = 0, retries = 0;
    const GameInviteOptions& options() const override { return configuration; }
    void poll() override {}
    bool loading() const override { return pending; }
    std::string_view error() const override { return failure; }
    const std::vector<GameInviteFriend>& friends() const override { return people; }
    GameInviteStatus status(std::int64_t id) const override
    {
        const auto found = statuses.find(id);
        return found == statuses.end() ? GameInviteStatus::Available : found->second;
    }
    bool sending() const override
    {
        return std::ranges::any_of(statuses, [](const auto& item) { return item.second == GameInviteStatus::Sending; });
    }
    void invite(std::int64_t id) override
    {
        actions.push_back(id);
        statuses[id] = GameInviteStatus::Sending;
    }
    void retry() override { ++retries; }
    void close() override
    {
        if (!finished)
            ++closes;
        finished = true;
    }
    bool closed() const override { return finished; }
};

struct Fixture {
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    std::shared_ptr<InviteSession> session = std::make_shared<InviteSession>();
    GameInvitePrompt prompt{fonts, atlas};
    float scale = 1;
    Fixture()
    {
        prompt.open(session);
        frame();
    }
    void frame() { prompt.update(0, Vector2{1280, 720} * scale, scale); }
    void click(RectF rect)
    {
        const double x = (rect.x + rect.w / 2) / scale, y = (rect.y + rect.h / 2) / scale;
        CHECK(prompt.handle(platform::MouseButtonEvent{.pressed = true, .x = x, .y = y}));
        CHECK(prompt.handle(platform::MouseButtonEvent{.pressed = false, .x = x, .y = y}));
    }
};
} // namespace

TEST(ui_invite_search_and_rendering_never_send_without_an_action)
{
    Fixture f;
    DrawList list;
    f.prompt.draw(list);
    CHECK(!list.quads.empty());
    CHECK(f.session->actions.empty());
    f.prompt.handle(platform::TextEvent{"buddy"});
    f.frame();
    CHECK_EQ(f.prompt.query(), "buddy");
    CHECK(f.prompt.inviteRect(7).w > 0);
    CHECK_EQ(f.prompt.inviteRect(8).w, 0.0f);
    f.prompt.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK(f.session->actions.empty());
    f.click(f.prompt.inviteRect(7));
    CHECK_EQ(f.session->actions, std::vector<std::int64_t>{7});
    f.click(f.prompt.inviteRect(7));
    CHECK_EQ(f.session->actions.size(), 1u);
    f.session->statuses[7] = GameInviteStatus::Sent;
    f.click(f.prompt.inviteRect(7));
    CHECK_EQ(f.session->actions.size(), 1u);
}

TEST(ui_invite_keyboard_activation_high_dpi_and_escape)
{
    Fixture f;
    f.scale = 2;
    f.frame();
    f.prompt.handle(platform::KeyEvent{.key = platform::Key::Tab, .pressed = true});
    f.prompt.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true, .repeat = true});
    CHECK(f.session->actions.empty());
    f.prompt.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK_EQ(f.session->actions, std::vector<std::int64_t>{7});
    f.session->statuses[7] = GameInviteStatus::Available;
    f.click(f.prompt.inviteRect(8));
    CHECK_EQ(f.session->actions, (std::vector<std::int64_t>{7, 8}));
    f.prompt.handle(platform::KeyEvent{.key = platform::Key::Escape, .pressed = true});
    CHECK(!f.prompt.visible());
    CHECK_EQ(f.session->closes, 1);
}

TEST(ui_invite_errors_close_and_changing_search_do_not_activate_a_different_row)
{
    Fixture f;
    const auto button = f.prompt.inviteRect(7);
    const double x = button.x + button.w / 2, y = button.y + button.h / 2;
    f.prompt.handle(platform::MouseButtonEvent{.pressed = true, .x = x, .y = y});
    f.prompt.handle(platform::MouseButtonEvent{.pressed = false, .x = x - 200, .y = y});
    CHECK(f.session->actions.empty());
    f.session->people.clear();
    f.session->failure = "The account cannot be prompted.";
    f.frame();
    DrawList list;
    f.prompt.draw(list);
    CHECK(!list.quads.empty());
    f.click(f.prompt.closeRect());
    CHECK(!f.prompt.visible());
    CHECK_EQ(f.session->closes, 1);
}

GPU_TEST(ui_invite_sdk_prompt_blocks_game_controls_and_renders_at_both_pixel_scales)
{
    constexpr std::string_view permission
        = "https://apis.roblox.com/notifications/v1/" "can-prompt-experience-invite?PlaceId=123&Trigger=" "DeveloperMul"
                                                                                                          "tiple";
    constexpr std::string_view sendUrl = "https://apis.roblox.com/notifications/v1/send-experience-invite";
    for (const float scale : {1.0f, 2.0f}) {
        // All account data and submissions in this check use this local fake.
        auto server = std::make_shared<fakeweb::FakeServer>();
        server->signedInAnswers[std::string(permission)] = {200, R"({"canPrompt":true})"};
        server->signedInAnswers
            ["https://apis.roblox.com/notifications/v1/" "can-prompt-experience-invite?PlaceId=123&" "Trigger="
                                                                                                     "DeveloperSingle&"
                                                                                                     "RecipientUserId="
                                                                                                     "7"]
            = {200, R"({"canPrompt":true})"};
        std::string friends = R"({"data":[)";
        for (int i = 0; i < 24; ++i) {
            if (i)
                friends += ',';
            friends += std::format(
                R"({{"id":{},"name":"ExampleFriend{:02}","displayName":"Example friend {:02}","isOnline":{}}})", i + 7,
                i, i, i < 4);
        }
        friends += "]}";
        server->signedInAnswers["https://friends.roblox.com/v1/users/42/friends"] = {200, friends};
        server->signedInAnswers["https://friends.roblox.com/v1/users/42/friends/online?origin=getFriendsOnline"]
            = {200, R"({"data":[]})"};
        server->signedInAnswers[std::string(sendUrl)] = {200, "{}"};
        auto game = DataModel::create();
        game->assignJob("test-job", 123);
        auto me = std::make_shared<Player>();
        me->setUserId(42);
        me->setName("LocalTester");
        me->setParent(game->getService<Players>());
        game->getService<Players>()->assignLocalPlayer(me.get());
        auto screen = std::make_shared<ScreenGui>();
        screen->setIgnoreGuiInset(true);
        screen->setParent(game->getService<StarterGui>());
        auto gameButton = std::make_shared<TextButton>();
        gameButton->setText("");
        gameButton->setPosition(UDim2::fromOffset(20, 300));
        gameButton->setSize(UDim2::fromOffset(100, 100));
        gameButton->setBorderSizePixel(0);
        gameButton->setAutoButtonColor(false);
        gameButton->setBackgroundColor3({1, 0, 0});
        gameButton->setParent(screen);
        int clicks = 0, releases = 0;
        gameButton->activated.connect([&] { ++clicks; });
        gameButton->mouseButton1Up.connect([&] { ++releases; });

        const auto width = static_cast<std::uint32_t>(1280 * scale);
        const auto height = static_cast<std::uint32_t>(720 * scale);
        auto offscreen = render::OffscreenRenderer::create(context, width, height);
        CHECK(offscreen.has_value());
        GameUi::Options options;
        options.contentDir = assets::AssetConfig::fromEnvironment().contentDir;
        options.map.minimap = false;
        auto made = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
        CHECK(made.has_value());
        auto ui = std::move(*made);
        ui->attach((*offscreen)->renderer());
        const auto frame = [&] { ui->update(0.01, {static_cast<float>(width), static_cast<float>(height)}, scale); };
        frame();
        auto scripts = std::make_unique<ScriptContext>(game, ScriptRole::Client);
        scripts->setWeb(std::make_shared<assets::RobloxWeb>(
            [server, transport = server->transport()](const auto& request) { return transport(request); },
            fakeweb::fakeLogin()));
        std::vector<std::string> errors;
        scripts->setOutput([&](MessageType type, std::string_view line) {
            if (type == MessageType::Error)
                errors.emplace_back(line);
        });
        const auto run = [&](std::string_view source) {
            if (const auto result = scripts->run(source, "InviteIntegration"); !result)
                throw lt::Failure{result.error().message};
        };
        auto handler = ui->gameInviteHandler();
        std::shared_ptr<GameInviteSession> active;
        scripts->setGameInvitePromptHandler([&](auto session) {
            active = session;
            return handler(std::move(session));
        });
        const auto wait = [&](auto finished) {
            for (int i = 0; i < 2000 && !finished(); ++i) {
                scripts->step(0.01);
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(finished());
        };
        const auto posts = [&] {
            const std::scoped_lock lock(server->mutex);
            return std::ranges::count_if(server->seen, [&](const auto& request) { return request.url == sendUrl; });
        };
        const auto capture = [&](std::string_view name) {
            frame();
            render::Lighting lighting;
            lighting.sky = {0, 0, 0};
            auto image = (*offscreen)->render({}, {}, lighting);
            if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP"))
                std::ofstream(std::filesystem::path(dir) / std::format("{}-{}x.png", name, scale), std::ios::binary)
                    << render::encodePng(image);
            return image;
        };
        run(R"(
            game:GetService("SocialService").GameInvitePromptClosed:Connect(function(player, ids)
                assert(player == game.Players.LocalPlayer)
                local count = workspace:GetAttribute("ClosedCount") or 0
                if count == 0 then assert(#ids == 1 and ids[1] == 7) else assert(#ids == 0) end
                workspace:SetAttribute("ClosedCount", count + 1)
            end)
        )");
        CHECK(ui->handle(platform::MouseButtonEvent{.pressed = true, .x = 50, .y = 350}));
        run(R"(game.SocialService:PromptGameInvite(game.Players.LocalPlayer))");
        CHECK(active);
        wait([&] { return !active->loading(); });
        CHECK(active->error().empty());
        CHECK_EQ(active->friends().size(), 24u);
        CHECK(ui->menuOpen() && ui->keyboardCaptured() && ui->wheelCaptured());
        CHECK(ui->handle(platform::MouseButtonEvent{.pressed = false, .x = 50, .y = 350}));
        CHECK_EQ(clicks, 0);
        CHECK_EQ(releases, 1);
        CHECK_EQ(posts(), 0);
        const auto image = capture("ui-invite");
        const auto covered = image.at(static_cast<std::uint32_t>(50 * scale), static_cast<std::uint32_t>(350 * scale));
        CHECK(covered.r > 80 && covered.r < 110 && covered.g < 3 && covered.b < 3);

        ui->handle(platform::TextEvent{"friend 00"});
        frame();
        CHECK(ui->gameInvites().inviteRect(7).w > 0);
        CHECK_EQ(ui->gameInvites().inviteRect(8).w, 0.0f);
        ui->handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
        CHECK_EQ(posts(), 0);
        const auto button = ui->gameInvites().inviteRect(7);
        const double x = (button.x + button.w / 2) / scale, y = (button.y + button.h / 2) / scale;
        ui->handle(platform::MouseButtonEvent{.pressed = true, .x = x, .y = y});
        ui->handle(platform::MouseButtonEvent{.pressed = false, .x = x, .y = y});
        wait([&] { return !active->sending(); });
        CHECK(active->status(7) == GameInviteStatus::Sent);
        CHECK_EQ(posts(), 1);
        capture("ui-invite-sent");
        ui->handle(platform::KeyEvent{.key = platform::Key::Escape, .pressed = true});
        run(R"(assert(workspace:GetAttribute("ClosedCount") == 1))");
        CHECK(!ui->gameInvites().visible());

        server->signedInAnswers[std::string(permission)] = {200, R"({"canPrompt":false})"};
        run(R"(game.SocialService:PromptGameInvite(game.Players.LocalPlayer))");
        wait([&] { return !active->loading(); });
        CHECK(!active->error().empty());
        CHECK(active->friends().empty());
        capture("ui-invite-denied");
        ui->handle(platform::KeyEvent{.key = platform::Key::Escape, .pressed = true});
        run(R"(assert(workspace:GetAttribute("ClosedCount") == 2))");
        CHECK_EQ(posts(), 1);
        CHECK(errors.empty());
        ui.reset();
        CHECK(!handler(active)); // The context may outlive a window.
    }
}
