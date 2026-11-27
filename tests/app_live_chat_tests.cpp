// What a client of a game shows of its chat (app/LiveChat): the join log's
// lines, the chat window's, the bubbles over heads, and preset chat's
// catalog from the web.

#include "fake_web.hpp"
#include "framework.hpp"

#include "LiveChat.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace lunar;

namespace {

ChatLine message(std::string name, std::string text, std::string status = "Success")
{
    ChatLine line;
    line.channel = "RBXGeneral";
    line.senderName = name;
    line.senderDisplayName = name;
    line.prefix = name + ":";
    line.text = std::move(text);
    line.status = std::move(status);
    line.userId = 7;
    return line;
}

} // namespace

TEST(app_chat_lines_for_the_log)
{
    CHECK_EQ(describeChatLine(message("Bob", "hi &lt;3")), std::string("chat: Bob: hi <3"));
    ChatLine own = message("Me", "hi", "Sending");
    CHECK_EQ(describeChatLine(own), std::string("chat: Me: hi (Sending)"));
    ChatLine team = message("Bob", "go");
    team.channel = "RBXTeamBright red";
    team.prefix = "<font color='#e31b1b'>[ OWNER ]</font> Bob:";
    CHECK_EQ(describeChatLine(team), std::string("chat: Bob: go [RBXTeamBright red] (shown as \"[ OWNER ] Bob\")"));
    ChatLine hidden = message("", "...");
    hidden.kind = ChatLine::Kind::Hidden;
    CHECK_EQ(describeChatLine(hidden),
             std::string("chat: user 7: ... (hidden: 3 characters this account may not read)"));
    ChatLine system;
    system.kind = ChatLine::Kind::System;
    system.channel = "RBXGeneral";
    system.text = "<b>Welcome</b>";
    CHECK_EQ(describeChatLine(system), std::string("chat: [RBXGeneral] Welcome"));
}

TEST(app_chat_lines_for_the_window)
{
    auto shown = windowMessage(message("Bob", "hi &amp; bye"));
    CHECK(shown.has_value());
    CHECK(shown->kind == ui::ChatMessage::Kind::Player && shown->displayName == "Bob" && shown->text == "hi &amp; bye");
    CHECK(shown->richText);
    // Pending messages update in place when the server confirms or refuses them.
    ChatLine mine = message("Me", "hi", "Sending");
    mine.own = true;
    shown = windowMessage(mine);
    CHECK(shown.has_value() && shown->pending);
    mine.status = "InvalidPrivacySettings";
    shown = windowMessage(mine);
    CHECK(shown.has_value() && shown->kind == ui::ChatMessage::Kind::Error);
    CHECK_EQ(shown->text, std::string("Your chat settings prevent you from sending messages."));
    CHECK(!windowMessage(message("Bob", "x", "Floodchecked")).has_value());
    // A callback's prefix is the name shown.
    ChatLine tagged = message("Bob", "yo");
    tagged.prefix = "<font color='#f4e800'>[FAN]</font> Bob:";
    shown = windowMessage(tagged);
    CHECK(shown.has_value() && shown->displayName == "[FAN] Bob" && shown->fromName == "Bob");
}

TEST(app_chat_bubbles_over_heads)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setName("Bob");
    player->setUserId(7);
    player->setParent(game->getService("Players"));
    auto character = std::make_shared<Model>();
    character->setName("Bob");
    auto head = std::make_shared<Part>();
    head->setName("Head");
    head->setParent(character.get());
    character->setParent(game->workspace());
    player->setCharacter(character.get());
    {
        ChatBubbles bubbles(game);
        bubbles.add(message("Bob", "hello"), 0.0);
        CHECK_EQ(bubbles.count(), std::size_t{1});
        Instance* folder = game->getService("CoreGui")->findFirstChild("BubbleChat");
        CHECK(folder != nullptr);
        auto* gui = dynamic_cast<BillboardGui*>(folder->findFirstChildOfClass("BillboardGui"));
        CHECK(gui != nullptr);
        CHECK(gui->adornee() == head.get());
        const Instance* frame = gui->findFirstChild("Bubble");
        CHECK(frame != nullptr);
        const auto* label = dynamic_cast<const TextLabel*>(frame->findFirstChild("Text"));
        CHECK(label != nullptr);
        CHECK_EQ(label->text(), std::string("hello"));
        // Three at most; Sending and system lines make none.
        for (int i = 1; i <= 3; ++i)
            bubbles.add(message("Bob", std::format("line {}", i)), i);
        bubbles.add(message("Bob", "not yet", "Sending"), 4.0);
        CHECK_EQ(bubbles.count(), std::size_t{3});
        CHECK_EQ(folder->children().size(), std::size_t{1});
        // Each goes after its 15 s.
        bubbles.update(16.5);
        CHECK_EQ(bubbles.count(), std::size_t{2});
        bubbles.update(20.0);
        CHECK_EQ(bubbles.count(), std::size_t{0});
        CHECK(folder->children().empty());
    }
    // Gone with the bubbles.
    CHECK(game->getService("CoreGui")->findFirstChild("BubbleChat") == nullptr);
}

TEST(app_preset_chat_catalog)
{
    PresetChatAccess access;
    access.config = R"({"surface":"in-experience-chat","defaultVersion":"default-draft-v4","customVersion":""})";
    CHECK_EQ(presetCatalogVersion(access), std::string("default-draft-v4"));
    fakeweb::FakeServer server;
    server.signedInAnswers
        ["https://apis.roblox.com/preset-chat/v1/preset-chat-options?version=default-draft-v4" "&locale=en-us"]
        = {200, R"({"version":"default-draft-v4","categoryGroups":[{"name":"Quick Words","categories":[
            {"name":"General","messages":[
                {"presetId":"04919e74-e8b9-5df5-8353-3afc56d062f2","presetType":"Text","value":"Good luck"},
                {"presetId":"x","presetType":"Emote","value":"wave"}]}]}]})",
           ""};
    const assets::RobloxWeb web(server.transport(), fakeweb::fakeLogin());
    auto presets = fetchChatPresets(web, "default-draft-v4");
    CHECK(presets.has_value());
    CHECK_EQ(presets->size(), std::size_t{1});
    CHECK_EQ((*presets)[0].id, std::string("04919e74-e8b9-5df5-8353-3afc56d062f2"));
    CHECK_EQ((*presets)[0].text, std::string("Good luck"));
    CHECK_EQ((*presets)[0].version, std::string("default-draft-v4"));
    CHECK(!fetchChatPresets(web, "other").has_value());
}

TEST(app_chat_commands_help_channels_and_unavailable_whispers_stay_local)
{
    auto game = DataModel::create();
    auto* players = game->getService<Players>();
    auto me = std::make_shared<Player>();
    me->setName("Me");
    me->setUserId(42);
    me->setParent(players);
    players->assignLocalPlayer(me.get());
    auto* service = game->getService<TextChatService>();
    auto channels = std::make_shared<Folder>();
    channels->setName("TextChannels");
    channels->setParent(service);
    for (const char* name : {"RBXGeneral", "RBXTeamBlue"}) {
        auto channel = std::make_shared<TextChannel>();
        channel->setName(name);
        channel->setParent(channels);
        auto source = std::make_shared<TextSource>();
        source->setUserId(42);
        source->setParent(channel);
    }
    ScriptContext context(game, ScriptRole::Client);
    int sent = 0;
    context.setRemoteSink([&](Instance&, std::string_view, std::vector<RemoteValue>) { ++sent; });
    TextChatSource source(context);
    std::vector<ui::ChatMessage> delivered;
    source.deliver = [&](const auto& message) { delivered.push_back(message); };
    source.send("/help");
    source.send("/w Nobody private message");
    CHECK_EQ(sent, 0);
    CHECK_EQ(delivered.size(), 2u);
    CHECK(delivered[0].kind == ui::ChatMessage::Kind::System);
    CHECK(delivered[1].kind == ui::ChatMessage::Kind::Error);
    source.send("/team");
    CHECK_EQ(source.selectedChannel(), "RBXTeamBlue");
    source.send("/team hello");
    CHECK_EQ(sent, 1);
    auto command = std::make_shared<TextChatCommand>();
    command->setPrimaryAlias("/inventory");
    command->setParent(service);
    CHECK(context
              .run(R"(
        game.TextChatService.TextChatCommand.Triggered:Connect(function(source, text)
            assert(source.UserId == 42 and text == "/inventory open")
            game.TextChatService.TextChatCommand:SetAttribute("Called", true)
        end)
    )",
                   "Command")
              .has_value());
    source.send("/inventory open");
    context.step(0);
    CHECK_EQ(sent, 1);
    CHECK(std::get<bool>(command->getAttribute("Called")));
}

TEST(app_preset_chat_picker_loads_on_request_and_sends_the_catalog_entry)
{
    fakeweb::FakeServer server;
    server.signedInAnswers["https://apis.roblox.com/preset-chat/v1/preset-chat-options?version=test&locale=en-us"]
        = {200, R"({"version":"test","categoryGroups":[{"categories":[{"messages":[
            {"presetId":"preset-1","presetType":"Text","value":"Good luck"}]}]}]})",
           ""};
    auto web = std::make_shared<assets::RobloxWeb>(server.transport(), fakeweb::fakeLogin());
    auto game = DataModel::create();
    auto* service = game->getService<TextChatService>();
    ScriptContext context(game, ScriptRole::Client);
    TextChatSource source(context, web);
    CHECK(!source.hasPresets());
    std::vector<RemoteValue> config{{.value = true}, {.value = std::string(R"({"defaultVersion":"test"})")}};
    std::vector<RemoteValue> access{{.value = std::string("Enabled")}};
    context.receiveRemote(*service, "ServerToClientPresetChatConfigChangedSignal", config);
    context.receiveRemote(*service, "ServerToClientPresetChatUserAccessChangedSignal", access);
    CHECK(source.hasPresets());
    CHECK(server.seen.empty());
    source.requestPresets();
    for (int i = 0; i < 1000 && source.presetsLoading(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    source.poll();
    CHECK_EQ(source.presetChoices(), std::vector<std::string>{"Good luck"});
    auto* players = game->getService<Players>();
    auto me = std::make_shared<Player>();
    me->setUserId(42);
    me->setParent(players);
    players->assignLocalPlayer(me.get());
    auto channels = std::make_shared<Folder>();
    channels->setName("TextChannels");
    channels->setParent(service);
    auto general = std::make_shared<TextChannel>();
    general->setName("RBXGeneral");
    general->setParent(channels);
    auto member = std::make_shared<TextSource>();
    member->setUserId(42);
    member->setParent(general);
    std::vector<RemoteValue> sent;
    context.setRemoteSink([&](Instance&, std::string_view event, std::vector<RemoteValue> arguments) {
        CHECK_EQ(event, "ClientToServerMessageReplicateSignalV4");
        sent = std::move(arguments);
    });
    source.sendPreset(0);
    CHECK_EQ(sent.size(), 9u);
    CHECK_EQ(std::get<std::string>(sent[0].value), "Good luck");
    CHECK_EQ(std::get<std::string>(sent[7].value), "preset-1");
    CHECK_EQ(std::get<std::string>(sent[8].value), "test");
    server.checkNoLeaks();
}

namespace {
struct WhisperFixture {
    std::shared_ptr<DataModel> game = DataModel::create();
    ScriptContext client{game, ScriptRole::Client};
    std::shared_ptr<Player> me = std::make_shared<Player>();
    std::shared_ptr<Player> peer = std::make_shared<Player>();
    std::shared_ptr<Folder> channels = std::make_shared<Folder>();
    std::shared_ptr<RemoteFunction> request = std::make_shared<RemoteFunction>();
    struct Call {
        Instance* remote;
        std::string event;
        std::vector<RemoteValue> args;
    };
    std::vector<Call> calls;
    std::vector<ui::ChatMessage> displayed;
    std::unique_ptr<TextChatSource> source;
    WhisperFixture()
    {
        auto* players = game->getService<Players>();
        me->setName("Me");
        me->setDisplayName("Myself");
        me->setUserId(42);
        me->setParent(players);
        peer->setName("FriendUser");
        peer->setDisplayName("Friend");
        peer->setUserId(7);
        peer->setParent(players);
        players->assignLocalPlayer(me.get());
        channels->setName("TextChannels");
        channels->setParent(game->getService<TextChatService>());
        channel("RBXGeneral", {42, 7});
        auto replicated = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("RobloxReplicatedStorage"));
        replicated->setParent(game);
        auto chat = std::make_shared<Folder>();
        chat->setName("ExperienceChat");
        chat->setParent(replicated);
        request->setName("WhisperChat");
        request->setParent(chat);
        client.setRemoteSink([&](Instance& remote, std::string_view event, std::vector<RemoteValue> args) {
            calls.push_back({&remote, std::string(event), std::move(args)});
        });
        source = std::make_unique<TextChatSource>(client);
        source->deliver = [&](const auto& line) { displayed.push_back(line); };
    }
    std::shared_ptr<TextChannel> channel(std::string name, std::initializer_list<std::int64_t> members)
    {
        auto value = std::make_shared<TextChannel>();
        value->setName(std::move(name));
        value->setParent(channels);
        for (auto id : members) {
            auto member = std::make_shared<TextSource>();
            member->setUserId(id);
            member->setParent(value);
        }
        return value;
    }
    void reply(InstancePtr value)
    {
        CHECK(!calls.empty());
        std::vector<RemoteValue> args{calls.front().args[0], {.value = std::move(value)}};
        client.receiveRemote(*request, "RemoteOnInvokeSuccess", args);
        client.step(0);
    }
};
} // namespace

TEST(app_whisper_requests_the_server_channel_before_sending_any_text)
{
    WhisperFixture f;
    f.source->send("/w @FriendUser private hello");
    CHECK_EQ(f.calls.size(), 1u);
    CHECK(f.calls[0].remote == f.request.get());
    CHECK_EQ(f.calls[0].event, "RemoteOnInvokeServer");
    CHECK_EQ(std::get<std::string>(f.calls[0].args[2].value), "7");
    CHECK_EQ(f.source->selectedChannel(), "RBXGeneral");
    const auto privateChannel = f.channel("RBXWhisper:7_42", {7, 42});
    f.reply(privateChannel);
    CHECK_EQ(f.source->selectedChannel(), "RBXWhisper:7_42");
    CHECK_EQ(f.calls.size(), 2u);
    CHECK_EQ(f.calls[1].event, "ClientToServerMessageReplicateSignalV4");
    CHECK_EQ(std::get<std::string>(f.calls[1].args[0].value), "private hello");
    CHECK(std::get<InstancePtr>(f.calls[1].args[4].value) == privateChannel);
    const auto available = f.source->chatChannels();
    CHECK(std::ranges::any_of(available, [](const auto& info) { return info.label == "To Friend"; }));
}

TEST(app_whisper_refusal_and_wrong_channel_never_fall_back_to_public_chat)
{
    for (int response = 0; response < 3; ++response) {
        WhisperFixture f;
        f.source->send("/w Friend private hello");
        if (response == 0)
            f.reply(nullptr);
        else if (response == 1)
            f.reply(f.channels->findFirstChild("RBXGeneral")->shared_from_this());
        else
            f.reply(f.channel("RBXWhisper:7_42", {7, 42, 99}));
        CHECK_EQ(f.calls.size(), 1u);
        CHECK_EQ(f.source->selectedChannel(), "RBXGeneral");
        CHECK_EQ(f.displayed.size(), 1u);
        CHECK(f.displayed[0].kind == ui::ChatMessage::Kind::Error);
    }
}

TEST(app_whisper_timeout_and_source_destruction_do_not_send_drafts)
{
    WhisperFixture f;
    f.source->send("/w @FriendUser private hello");
    f.client.step(16);
    CHECK_EQ(f.calls.size(), 1u);
    CHECK_EQ(f.displayed.size(), 1u);
    CHECK(f.displayed[0].text.find("timed out") != std::string::npos);
    f.reply(f.channel("RBXWhisper:7_42", {7, 42}));
    CHECK_EQ(f.calls.size(), 1u);
    WhisperFixture removed;
    removed.source->send("/w @FriendUser private hello");
    removed.source.reset();
    removed.reply(removed.channel("RBXWhisper:7_42", {7, 42}));
    CHECK_EQ(removed.calls.size(), 1u);
}

TEST(app_whisper_existing_channel_still_enforces_send_permissions)
{
    WhisperFixture f;
    auto channel = f.channel("RBXWhisper:7_42", {7, 42});
    for (const auto& child : channel->children())
        if (auto* member = instanceCast<TextSource>(child.get()); member && member->userId() == 42)
            member->setCanSend(false);
    f.source->send("/w @FriendUser private hello");
    CHECK(f.calls.empty());
    CHECK_EQ(f.displayed.size(), 1u);
    CHECK(f.displayed[0].kind == ui::ChatMessage::Kind::Error);
}

TEST(app_builtin_command_aliases_and_disabled_whispers_keep_drafts_private)
{
    WhisperFixture f;
    auto command = std::make_shared<TextChatCommand>();
    command->setName("RBXWhisperCommand");
    command->setPrimaryAlias("private");
    command->setSecondaryAlias("/p");
    command->setParent(f.game->getService<TextChatService>());
    f.source->send("/w @FriendUser secret");
    CHECK(f.calls.empty());
    command->setEnabled(false);
    f.source->send("/private @FriendUser secret");
    f.source->send("/p @FriendUser secret");
    CHECK(f.calls.empty());
    command->setEnabled(true);
    f.source->send("/P @FriendUser secret");
    CHECK_EQ(f.calls.size(), 1u);
    CHECK(f.calls.front().remote == f.request.get());
    f.reply(f.channel("RBXWhisper:7_42", {7, 42}));
    CHECK_EQ(f.calls.size(), 2u);
    CHECK_EQ(std::get<std::string>(f.calls.back().args[0].value), "secret");
}

TEST(app_clear_console_version_and_mute_commands_stay_local)
{
    WhisperFixture f;
    int cleared = 0;
    std::int64_t removed = 0;
    f.source->clearWindow = [&] { ++cleared; };
    f.source->removeUser = [&](auto id) { removed = id; };
    f.source->send("/cls");
    f.source->send("/console");
    CHECK(f.game->getService<StarterGui>()->coreSettings().devConsoleVisible);
    f.source->send("/console");
    CHECK(!f.game->getService<StarterGui>()->coreSettings().devConsoleVisible);
    f.source->send("/version");
    CHECK(f.displayed.back().text.starts_with("Lunarstone"));
    f.source->send("/m @FriendUser");
    CHECK_EQ(removed, 7);
    CHECK_EQ(cleared, 1);
    auto* service = f.game->getService<TextChatService>();
    const auto receive = [&](std::string text) {
        auto* general = f.channels->findFirstChild("RBXGeneral");
        lunar::InstancePtr speaker;
        for (const auto& child : general->children())
            if (const auto* participant = instanceCast<TextSource>(child.get());
                participant && participant->userId() == 7)
                speaker = child;
        std::vector<RemoteValue> args{{.value = std::move(text)},
                                      {.value = std::string("Other:")},
                                      {.value = std::string{}},
                                      {.value = std::string("id")},
                                      {.value = speaker},
                                      {.value = general->shared_from_this()},
                                      {.value = 0.0},
                                      {.value = enums::item(Enum::TextChatMessageStatus::Success)}};
        CHECK(f.client.receiveRemote(*service, "ServerToClientMessageReplicateSignal", args));
        f.client.step(0);
    };
    const auto before = f.displayed.size();
    receive("muted text");
    CHECK_EQ(f.displayed.size(), before);
    f.source->send("/um @FriendUser");
    receive("unmuted text");
    CHECK_EQ(f.displayed.back().text, "unmuted text");
    CHECK(f.calls.empty());
}
