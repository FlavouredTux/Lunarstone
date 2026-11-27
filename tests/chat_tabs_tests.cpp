#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/ClassicChat.hpp>

#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::ui;

namespace {

struct Source final : ChatSource {
    std::vector<std::string> sent;
    int activations = 0;
    int polls = 0;
    void send(std::string_view text) override { sent.emplace_back(text); }
    void activate() override { ++activations; }
    void poll() override { ++polls; }
    void receive(std::string text)
    {
        if (deliver)
            deliver({.fromName = "Friend", .displayName = "Friend", .text = std::move(text)});
    }
};

struct Chat {
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    Source game;
    Source rtc;
    ClassicChat chat{fonts, atlas};

    Chat()
    {
        chat.setSource(&game);
        chat.setSource(ClassicChat::Tab::Rtc, &rtc);
        chat.update(0.0, {1280, 720}, 36.0f, 1.0f);
    }
    void type(std::string text)
    {
        chat.focus();
        CHECK(chat.handle(platform::TextEvent{std::move(text)}));
    }
    void enter() { CHECK(chat.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true})); }
    void click(ClassicChat::Tab tab, float scale = 1.0f)
    {
        const RectF rect = chat.tabRect(tab);
        CHECK(chat.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left,
                                                     .pressed = true,
                                                     .x = (rect.x + rect.w / 2.0f) / scale,
                                                     .y = (rect.y + rect.h / 2.0f) / scale}));
    }
};

} // namespace

TEST(chat_tabs_clear_and_mute_only_the_source_history)
{
    Chat f;
    f.game.receive("game line");
    f.rtc.receive("RTC line");
    CHECK(f.game.clearWindow && f.game.removeUser);
    f.game.clearWindow();
    CHECK(f.chat.messages(ClassicChat::Tab::Game).empty());
    CHECK_EQ(f.chat.messages(ClassicChat::Tab::Rtc).size(), 1u);
    ChatMessage other{.text = "peer", .userId = 7};
    f.game.deliver(other);
    f.game.receive("system");
    f.game.removeUser(7);
    CHECK_EQ(f.chat.messages(ClassicChat::Tab::Game).size(), 1u);
    CHECK_EQ(f.chat.messages(ClassicChat::Tab::Game).front().text, "system");
}

TEST(chat_tabs_keep_messages_and_drafts_separate)
{
    Chat f;
    f.type("game draft");
    f.click(ClassicChat::Tab::Rtc);
    CHECK_EQ(f.rtc.activations, 1);
    CHECK(f.chat.focused());
    CHECK(f.chat.draft().empty());
    f.type("rtc draft");
    f.game.receive("in game");
    f.rtc.receive("in RTC");
    f.chat.postSystem("game system message");
    CHECK_EQ(f.chat.messages().size(), 2u);
    CHECK_EQ(f.chat.messages(ClassicChat::Tab::Rtc).size(), 1u);
    CHECK_EQ(f.chat.unread(ClassicChat::Tab::Game), 2u);
    f.click(ClassicChat::Tab::Game);
    CHECK_EQ(f.chat.draft(), "game draft");
    CHECK_EQ(f.chat.unread(ClassicChat::Tab::Game), 0u);
    f.enter();
    CHECK_EQ(f.game.sent, std::vector<std::string>{"game draft"});
    CHECK(f.rtc.sent.empty());
    f.click(ClassicChat::Tab::Rtc);
    CHECK_EQ(f.chat.draft(), "rtc draft");
    f.chat.focus();
    f.enter();
    CHECK_EQ(f.rtc.sent, std::vector<std::string>{"rtc draft"});
    CHECK_EQ(f.game.sent.size(), 1u);
}

TEST(chat_tabs_receive_in_background_and_limit_each_history)
{
    Chat f;
    for (int i = 0; i < 80; ++i)
        f.rtc.receive(std::to_string(i));
    CHECK(f.chat.messages().empty());
    CHECK_EQ(f.chat.messages(ClassicChat::Tab::Rtc).size(), ClassicChat::historyLength);
    CHECK_EQ(f.chat.unread(ClassicChat::Tab::Rtc), ClassicChat::historyLength);
    f.chat.update(1.0, {1280, 720}, 36.0f, 1.0f);
    CHECK_EQ(f.rtc.polls, 2);
    f.click(ClassicChat::Tab::Rtc);
    CHECK_EQ(f.chat.unread(ClassicChat::Tab::Rtc), 0u);
    DrawList draw;
    f.chat.draw(draw);
    CHECK(!draw.quads.empty());
}

TEST(chat_tabs_click_at_display_scale_and_detach_rtc)
{
    Chat f;
    f.chat.update(1.0, {2560, 1440}, 36.0f, 2.0f);
    f.click(ClassicChat::Tab::Rtc, 2.0f);
    CHECK(f.chat.selectedTab() == ClassicChat::Tab::Rtc);
    f.chat.setSource(ClassicChat::Tab::Rtc, nullptr);
    CHECK(!f.rtc.deliver);
    CHECK(!f.chat.hasRtcTab());
    CHECK(f.chat.selectedTab() == ClassicChat::Tab::Game);
    f.chat.selectTab(ClassicChat::Tab::Rtc);
    CHECK(f.chat.selectedTab() == ClassicChat::Tab::Game);
}

TEST(chat_tabs_help_stays_local_to_game)
{
    Chat f;
    f.type("/help");
    f.enter();
    CHECK(f.game.sent.empty());
    CHECK(!f.chat.messages().empty());
    CHECK(f.chat.messages(ClassicChat::Tab::Rtc).empty());
    f.click(ClassicChat::Tab::Rtc);
    f.type("/help");
    f.enter();
    CHECK_EQ(f.rtc.sent, std::vector<std::string>{"/help"});
}
