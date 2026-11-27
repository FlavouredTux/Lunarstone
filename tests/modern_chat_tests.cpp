#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/ClassicChat.hpp>

using namespace lunar;
using namespace lunar::ui;

namespace {

struct Source final : ChatSource {
    std::vector<std::string> sent;
    std::vector<ChatChannelInfo> channels;
    std::string selected;
    bool presetsEnabled = false;
    int requested = 0;
    std::vector<std::string> choices;
    std::vector<std::size_t> presetsSent;
    bool hasPresets() const override { return presetsEnabled; }
    void requestPresets() override { ++requested; }
    std::vector<std::string> presetChoices() const override { return choices; }
    void sendPreset(std::size_t index) override { presetsSent.push_back(index); }
    void send(std::string_view text) override { sent.emplace_back(text); }
    std::vector<ChatChannelInfo> chatChannels() const override { return channels; }
    std::string selectedChannel() const override { return selected; }
    bool selectChannel(std::string_view name) override
    {
        selected = name;
        return true;
    }
};

struct ModernFixture {
    std::shared_ptr<DataModel> game = DataModel::create();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    Source source;
    ClassicChat chat{fonts, atlas};
    std::shared_ptr<ChatWindowConfiguration> window = std::make_shared<ChatWindowConfiguration>();
    std::shared_ptr<ChatInputBarConfiguration> input = std::make_shared<ChatInputBarConfiguration>();
    ModernFixture()
    {
        auto* service = game->getService<TextChatService>();
        window->setParent(service);
        input->setParent(service);
        chat.setGame(game.get());
        chat.setSource(&source);
        frame();
    }
    void frame() { chat.update(1.0, {1280, 720}, 58.0f, 1.0f); }
};

} // namespace

TEST(modern_chat_configuration_and_core_layout_overrides)
{
    ModernFixture f;
    auto& settings = f.game->getService<StarterGui>()->coreSettings();
    settings.chatWindowSize = UDim2::fromOffset(400, 240);
    settings.chatWindowPosition = UDim2::fromOffset(30, 20);
    f.frame();
    CHECK(f.chat.windowRect() == (RectF{30, 78, 400, 240}));
    CHECK_EQ(f.window->absolutePosition().x, 30.0f);
    CHECK_EQ(f.window->absoluteSize().x, 400.0f);
    CHECK_EQ(f.input->absolutePosition().y, f.chat.barRect().y);
    f.window->setHorizontalAlignment(Enum::HorizontalAlignment::Right);
    settings.chatWindowPosition.reset();
    f.frame();
    CHECK(f.chat.windowRect().right() == 1272.0f);
}

TEST(modern_chat_focus_disabled_input_and_send_button)
{
    ModernFixture f;
    f.input->requestFocus();
    f.frame();
    CHECK(f.chat.focused() && f.input->isFocused());
    CHECK(f.chat.handle(platform::TextEvent{"hello"}));
    const auto send = f.chat.sendRect();
    const double x = send.x + send.w / 2, y = send.y + send.h / 2;
    CHECK(f.chat.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y}));
    CHECK(f.chat.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y}));
    CHECK_EQ(f.source.sent, std::vector<std::string>{"hello"});
    CHECK(!f.chat.focused() && !f.input->isFocused());
    f.input->setEnabled(false);
    f.input->requestFocus();
    f.frame();
    CHECK(!f.chat.focused());
    CHECK_EQ(f.chat.barRect().h, 0.0f);
}

TEST(modern_chat_pending_message_updates_without_duplicate_history)
{
    ModernFixture f;
    ChatMessage pending{.fromName = "Player", .displayName = "Player", .text = "hello"};
    pending.messageId = "message-1";
    pending.pending = true;
    f.chat.post(pending);
    pending.pending = false;
    pending.text = "confirmed";
    pending.prefix = "<font color='#ff0000'>[VIP]</font> Player:";
    pending.richText = true;
    f.chat.post(pending);
    f.frame();
    CHECK_EQ(f.chat.messages().size(), 1u);
    CHECK_EQ(f.chat.messages().front().text, "confirmed");
    CHECK(!f.chat.messages().front().pending);
    DrawList list;
    f.chat.draw(list);
    CHECK(!list.quads.empty());
}

TEST(modern_chat_channel_tabs_change_the_game_target_without_touching_rtc)
{
    ModernFixture f;
    f.source.channels = {{"RBXGeneral", "General"}, {"RBXTeamBlue", "Team"}};
    f.source.selected = "RBXGeneral";
    f.frame();
    const auto tab = f.chat.channelRect("RBXTeamBlue");
    CHECK(tab.w > 0 && tab.h > 0);
    CHECK(f.chat.handle(platform::MouseButtonEvent{
        .button = platform::MouseButton::Left, .pressed = true, .x = tab.x + tab.w / 2, .y = tab.y + tab.h / 2}));
    CHECK_EQ(f.source.selected, "RBXTeamBlue");
    f.chat.focus();
    f.chat.handle(platform::TextEvent{"team hello"});
    f.chat.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK_EQ(f.source.sent, std::vector<std::string>{"team hello"});
}

TEST(modern_chat_preset_picker_routes_only_the_selected_phrase)
{
    ModernFixture f;
    f.source.presetsEnabled = true;
    f.source.choices = {"Hello", "Good luck", "Well played"};
    f.frame();
    auto click = [&](RectF rect) {
        const double x = rect.x + rect.w / 2, y = rect.y + rect.h / 2;
        CHECK(f.chat.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y}));
        f.chat.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y});
    };
    click(f.chat.presetsRect());
    CHECK_EQ(f.source.requested, 1);
    f.frame();
    click(f.chat.presetChoiceRect(1));
    CHECK_EQ(f.source.presetsSent, std::vector<std::size_t>{1});
    CHECK(f.source.sent.empty());
    f.source.presetsEnabled = false;
    f.frame();
    CHECK_EQ(f.chat.presetsRect().w, 0.0f);
}

TEST(modern_chat_custom_focus_key_is_not_inserted_in_the_draft)
{
    ModernFixture f;
    f.input->setKeyboardKeyCode(Enum::KeyCode::T);
    f.frame();
    CHECK(!f.chat.handle(platform::TextEvent{"/"}));
    CHECK(f.chat.handle(platform::KeyEvent{.key = platform::Key::T, .pressed = true}));
    CHECK(f.chat.handle(platform::TextEvent{"t"}));
    CHECK(f.chat.focused());
    CHECK(f.chat.draft().empty());
    CHECK(f.chat.handle(platform::TextEvent{"hello"}));
    CHECK_EQ(f.chat.draft(), "hello");
}

TEST(modern_chat_game_focus_is_visible_to_scripts_and_rtc_focus_is_separate)
{
    ModernFixture f;
    auto* input = f.game->getService<UserInputService>();
    f.chat.focus();
    CHECK(input->focusedTextBox() != nullptr);
    CHECK(f.chat.ownsTextBox(input->focusedTextBox()));
    f.chat.handle(platform::TextEvent{"game draft"});
    f.frame();
    CHECK_EQ(input->focusedTextBox()->text(), "game draft");
    Source rtc;
    f.chat.setSource(ClassicChat::Tab::Rtc, &rtc);
    f.chat.selectTab(ClassicChat::Tab::Rtc);
    CHECK(f.chat.focused());
    CHECK(input->focusedTextBox() == nullptr);
    f.chat.handle(platform::TextEvent{"private draft"});
    f.frame();
    CHECK(input->focusedTextBox() == nullptr);
    f.chat.setSource(ClassicChat::Tab::Rtc, nullptr);
    f.chat.releaseFocus();
}

TEST(modern_chat_autocomplete_obeys_configuration_and_uses_tab_without_sending)
{
    ModernFixture f;
    auto command = std::make_shared<TextChatCommand>();
    command->setPrimaryAlias("/inventory");
    command->setParent(f.game->getService<TextChatService>());
    f.chat.focus();
    f.chat.handle(platform::TextEvent{"/inv"});
    f.frame();
    CHECK_EQ(f.chat.completions(), std::vector<std::string>{"/inventory "});
    command->setAutocompleteVisible(false);
    f.frame();
    CHECK(f.chat.completions().empty());
    command->setAutocompleteVisible(true);
    f.frame();
    CHECK(f.chat.handle(platform::KeyEvent{.key = platform::Key::Tab, .pressed = true}));
    CHECK_EQ(f.chat.draft(), "/inventory ");
    CHECK(f.source.sent.empty());
    f.chat.handle(platform::KeyEvent{.key = platform::Key::A, .pressed = true, .modifiers = {.ctrl = true}});
    f.chat.handle(platform::TextEvent{"/he"});
    f.frame();
    CHECK_EQ(f.chat.completions(), std::vector<std::string>{"/help "});
    f.input->setAutocompleteEnabled(false);
    f.frame();
    CHECK(f.chat.completions().empty());
    f.input->setAutocompleteEnabled(true);
    f.frame();
    CHECK(!f.chat.completions().empty());
    f.chat.handle(platform::KeyEvent{.key = platform::Key::Escape, .pressed = true});
    f.frame();
    CHECK(f.chat.focused() && f.chat.completions().empty());
}

TEST(modern_chat_builtin_completion_reads_current_aliases_and_flags)
{
    ModernFixture f;
    auto command = std::make_shared<TextChatCommand>();
    command->setName("RBXWhisperCommand");
    command->setPrimaryAlias("private");
    command->setSecondaryAlias("/pm");
    command->setParent(f.game->getService<TextChatService>());
    auto me = std::make_shared<Player>();
    auto peer = std::make_shared<Player>();
    me->setParent(f.game->getService<Players>());
    f.game->getService<Players>()->assignLocalPlayer(me.get());
    peer->setName("FriendUser");
    peer->setDisplayName("Friend");
    peer->setParent(f.game->getService<Players>());
    f.chat.focus();
    const auto type = [&](std::string text) {
        f.chat.handle(platform::KeyEvent{.key = platform::Key::A, .pressed = true, .modifiers = {.ctrl = true}});
        f.chat.handle(platform::TextEvent{std::move(text)});
        f.frame();
    };
    type("/w");
    CHECK(f.chat.completions().empty());
    type("/p");
    CHECK_EQ(f.chat.completions(), (std::vector<std::string>{"/pm ", "/private "}));
    type("/pm Fri");
    CHECK_EQ(f.chat.completions(), (std::vector<std::string>{"/pm @FriendUser "}));
    command->setAutocompleteVisible(false);
    f.frame();
    CHECK(f.chat.completions().empty());
    command->setAutocompleteVisible(true);
    command->setEnabled(false);
    type("/p");
    CHECK(f.chat.completions().empty());
}
