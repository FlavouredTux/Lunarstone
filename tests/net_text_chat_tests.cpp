// TextChatService on a client: the text SendAsync sends, the network events
// it goes out and comes back as (through a RemoteBridge and a Replica, the
// way --net-join carries them), OnIncomingMessage, MessageReceived and the
// hidden messages of an account that may not read chat, and Play Solo's
// loopback without a server.

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/net/Remotes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/script/ScriptContext.hpp>
#include <lunar/script/TextChat.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

template <class T>
std::shared_ptr<T> make(const std::string& name, Instance* parent)
{
    auto instance = std::make_shared<T>();
    instance->setName(name);
    instance->setParent(parent);
    return instance;
}

// A game the way a server sets up chat: TextChatService.TextChannels with
// RBXGeneral, a TextSource per player in it, two players.
struct ChatGame {
    std::shared_ptr<DataModel> game = DataModel::create();
    Instance* service = game->getService("TextChatService");
    std::shared_ptr<Folder> channels = make<Folder>("TextChannels", service);
    std::shared_ptr<TextChannel> general = make<TextChannel>("RBXGeneral", channels.get());
    std::shared_ptr<Player> me = make<Player>("Me", game->getService("Players"));
    std::shared_ptr<Player> other = make<Player>("Other", game->getService("Players"));
    std::shared_ptr<TextSource> mySource = make<TextSource>("Me", general.get());
    std::shared_ptr<TextSource> otherSource = make<TextSource>("Other", general.get());

    ChatGame()
    {
        me->setUserId(42);
        me->setDisplayName("Myself");
        other->setUserId(7);
        mySource->setUserId(42);
        otherSource->setUserId(7);
    }
};

// A client of a ReplicationServer over a ChatGame, its remotes through a RemoteBridge.
struct ChatRig {
    ChatGame server;
    std::unique_ptr<ReplicationServer> replication;
    std::shared_ptr<Replica> replica = std::make_shared<Replica>();
    std::unique_ptr<RemoteBridge> bridge;
    std::unique_ptr<ScriptContext> client;
    std::vector<std::string> output;
    std::vector<ChatLine> chat;

    ChatRig()
    {
        replication =
            std::make_unique<ReplicationServer>(server.game, std::vector<std::string>{"Players", "TextChatService"});
        for (const auto& m : replication->joinMessages())
            replica->handle(m);
        auto* players = dynamic_cast<Players*>(replica->game()->findFirstChildOfClass("Players"));
        CHECK(players != nullptr);
        players->assignLocalPlayer(dynamic_cast<Player*>(players->findFirstChild("Me")));
        bridge = std::make_unique<RemoteBridge>(replica);
        client = std::make_unique<ScriptContext>(replica->game(), ScriptRole::Client);
        client->setOutput([this](MessageType type, std::string_view text) {
            if (type != MessageType::Info)
                output.emplace_back(text);
        });
        client->setRemoteSink([this](Instance& remote, std::string_view event, std::vector<RemoteValue> args) {
            bridge->send(remote, event, std::move(args));
        });
        bridge->setReceiver([this](Instance& remote, std::string_view event, std::span<const RemoteValue> args) {
            client->receiveRemote(remote, event, args);
        });
        client->setChatSink([this](const ChatLine& line) { chat.push_back(line); });
    }

    // What the server reads from the client: "ClientToServerMessageReplicateSignalV4(...)".
    std::vector<std::string> serverReads()
    {
        std::vector<std::string> out;
        for (auto& m : bridge->take()) {
            auto items = decodeData(m.message, replication->schema(), replication->context(), nullptr, {},
                                    ItemContext{false});
            CHECK(items.has_value());
            for (const Item& item : *items)
                if (const auto* e = std::get_if<EventInvocationItem>(&item))
                    out.push_back(std::format("{}{}", replication->schema().events[e->eventId].name,
                                              describeRemoteArguments(remoteArguments(*e), 1000)));
        }
        return out;
    }

    void fromServer(std::string_view event, std::vector<RemoteValue> args)
    {
        const Schema& schema = replication->schema();
        const auto eid = schema.eventId(*schema.classId("TextChatService"), event);
        CHECK(eid.has_value());
        const std::array<Item, 1> items{eventInvocation(replication->idOf(server.service), *eid, args)};
        CHECK(replica->handle(encodeData(items, schema, replication->context())));
    }

    void steps(int n)
    {
        for (int i = 0; i < n; ++i)
            client->step(1.0 / 60.0);
    }

    bool printed(std::string_view line) const { return std::ranges::find(output, line) != output.end(); }
};

RemoteValue str(std::string s)
{
    return remoteValue(Value(std::move(s)));
}

RemoteValue object(const std::shared_ptr<Instance>& instance)
{
    return remoteValue(Value(instance));
}

} // namespace

TEST(script_text_chat_prepares_text_like_send_async)
{
    CHECK_EQ(prepareChatText("  hi  \n"), std::string("hi"));
    CHECK_EQ(prepareChatText("a<b>&c"), std::string("a&lt;b&gt;&amp;c"));
    CHECK_EQ(prepareChatText("a\tb"), std::string("a   b"));
    CHECK_EQ(prepareChatText("a      b  c"), std::string("a   b  c"));
    CHECK_EQ(prepareChatText("a\n\n\n\n\nb"), std::string("a\n\n\nb"));
    CHECK_EQ(prepareChatText(" \t "), std::string());
    // Rich text back to plain: tags out, entities back.
    CHECK_EQ(plainChatText("<font color='#e31b1b'>[ OWNER ]</font> Bob &lt;3 &amp; a<br/>b"),
             std::string("[ OWNER ] Bob <3 & a\nb"));
    CHECK_EQ(plainChatText("1 < 2"), std::string("1 < 2"));
}

TEST(net_text_chat_send_goes_out_as_v4_and_comes_back)
{
    ChatRig rig;
    CHECK(rig.client
              ->run(R"(
        local TextChatService = game:GetService("TextChatService")
        local general = TextChatService.TextChannels.RBXGeneral
        TextChatService.OnIncomingMessage = function(message)
            local properties = Instance.new("TextChatMessageProperties")
            if message.TextSource then
                properties.PrefixText = "[VIP] " .. message.PrefixText
            end
            if message.Status == Enum.TextChatMessageStatus.Sending then
                properties.Text = 'on our screen only'
            end
            return properties
        end
        TextChatService.SendingMessage:Connect(function(m) print("sending", m.Text, m.Status.Name) end)
        general.MessageReceived:Connect(function(m)
            print("received", m.TextSource and m.TextSource.Name or "nil", m.Text, m.Status.Name, m.PrefixText)
        end)
    )",
                    "Test")
              .has_value());
    // What the chat bar sends: trimmed and escaped, from our TextSource in RBXGeneral.
    auto id = rig.client->sendChat("  hi <3  ");
    CHECK(id.has_value());
    CHECK(id->starts_with("42-{") && id->ends_with("}") && id->size() == 4 + 36 + 1);
    CHECK(rig.printed("sending hi &lt;3 Sending"));
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{1});
    CHECK_EQ(reads[0], std::format("ClientToServerMessageReplicateSignalV4(\"hi &lt;3\", \"\", \"{}\", "
                                   "TextChatService.TextChannels.RBXGeneral.Me, "
                                   "TextChatService.TextChannels.RBXGeneral, true, false, \"\", \"\")",
                                   *id));
    // Our own line first shows as Sending, through OnIncomingMessage.
    CHECK_EQ(rig.chat.size(), std::size_t{1});
    CHECK(rig.chat[0].own && rig.chat[0].status == "Sending" && rig.chat[0].prefix == "[VIP] Myself:");

    // The server's answer, then another player's message.
    auto general = std::dynamic_pointer_cast<Instance>(rig.server.general);
    rig.fromServer("ServerToClientMessageReplicateSignal",
                   {str("hi &lt;3"), str("Myself:"), str(""), str(*id), object(rig.server.mySource), object(general),
                    remoteValue(Value(1.7e12)),
                    remoteValue(Value(enums::item(Enum::TextChatMessageStatus::Success)))});
    rig.fromServer("ServerToClientMessageReplicateSignal",
                   {str("yo"), str("Other:"), str(""), str("7-{x}"), object(rig.server.otherSource), object(general),
                    remoteValue(Value(1.7e12 + 5)),
                    remoteValue(Value(enums::item(Enum::TextChatMessageStatus::Success)))});
    rig.steps(2);
    CHECK(rig.printed("received Me hi &lt;3 Success [VIP] Myself:"));
    CHECK(rig.printed("received Other yo Success [VIP] Other:"));
    CHECK_EQ(rig.chat.size(), std::size_t{3});
    CHECK(rig.chat[1].own && rig.chat[1].status == "Success" && rig.chat[1].messageId == *id);
    CHECK_EQ(rig.chat[2].senderName, std::string("Other"));
    CHECK_EQ(rig.chat[2].userId, std::int64_t{7});
    CHECK_EQ(rig.chat[2].text, std::string("yo"));
    CHECK_EQ(rig.chat[2].timestamp, std::int64_t{1700000000005});
    CHECK(!rig.chat[2].own);
    CHECK_EQ(rig.bridge->received, std::size_t{2});
}

TEST(net_text_chat_hidden_messages_and_the_v2_dictionary)
{
    ChatRig rig;
    CHECK(rig.client
              ->run(R"(
        local TextChatService = game:GetService("TextChatService")
        TextChatService.UserMessageIntentSent:Connect(function(m)
            print("intent", m.TextSource and m.TextSource.Name or "nil", #m.Text)
        end)
        TextChatService.MessageReceived:Connect(function(m) print("message", m.Text, m.Status.Name) end)
    )",
                    "Test")
              .has_value());
    auto general = std::dynamic_pointer_cast<Instance>(rig.server.general);
    // An account that may not read chat hears that someone spoke, a dot a character.
    rig.fromServer("UserMessageIntentSentRemote", {str("....."), str(""), str(""), str("7-{a}"),
                                                   object(rig.server.otherSource), object(general), remoteValue(Value(5.0))});
    // One whose TextSource never reached us: the id's user id still names the sender.
    rig.fromServer("UserMessageIntentSentRemote", {str(".."), str(""), str(""), str("7-{c}"), remoteValue(Value()),
                                                   object(general), remoteValue(Value(6.0))});
    // The dictionary form.
    auto data = std::make_shared<RemoteTable>();
    data->dictionary = true;
    data->members = {{"text", str("from v2")},
                     {"messageId", str("7-{b}")},
                     {"textSource", object(rig.server.otherSource)},
                     {"textChannel", object(general)},
                     {"status", remoteValue(Value(std::string("Floodchecked")))}};
    rig.fromServer("ServerToClientMessageReplicateSignalV2", {RemoteValue{Value{}, data}});
    rig.steps(2);
    CHECK(rig.printed("intent Other 5"));
    CHECK(rig.printed("intent nil 2"));
    CHECK(rig.printed("message from v2 Floodchecked"));
    CHECK_EQ(rig.chat.size(), std::size_t{3});
    CHECK(rig.chat[0].kind == ChatLine::Kind::Hidden && rig.chat[0].senderName == "Other");
    CHECK(rig.chat[1].kind == ChatLine::Kind::Hidden && rig.chat[1].senderName == "Other" && rig.chat[1].userId == 7);
    CHECK(rig.chat[2].kind == ChatLine::Kind::Message && rig.chat[2].status == "Floodchecked");
}

TEST(net_text_chat_preset_chat)
{
    // An account without chat: the server enables preset chat, and a phrase
    // of the catalog goes up as V4 with its presetId and the catalog version.
    ChatRig rig;
    const ChatPreset luck{"04919e74-e8b9-5df5-8353-3afc56d062f2", "Good luck", "default-draft-v4"};
    CHECK(!rig.client->sendPresetChat(luck).has_value());
    rig.fromServer("ServerToClientPresetChatConfigChangedSignal",
                   {remoteValue(Value(true)), str(R"({"surface":"in-experience-chat","defaultVersion":"default-draft-v4"})")});
    rig.fromServer("ServerToClientPresetChatUserAccessChangedSignal", {str("Enabled")});
    const PresetChatAccess access = rig.client->presetChatAccess();
    CHECK(access.enabled && access.access == "Enabled" && access.config.contains("default-draft-v4"));
    auto id = rig.client->sendPresetChat(luck);
    CHECK(id.has_value());
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{1});
    CHECK_EQ(reads[0], std::format("ClientToServerMessageReplicateSignalV4(\"Good luck\", \"\", \"{}\", "
                                   "TextChatService.TextChannels.RBXGeneral.Me, "
                                   "TextChatService.TextChannels.RBXGeneral, true, false, "
                                   "\"04919e74-e8b9-5df5-8353-3afc56d062f2\", \"default-draft-v4\")",
                                   *id));
}

TEST(script_text_chat_play_solo_loops_back)
{
    // No server: SendAsync's message comes back next frame as delivered.
    ChatGame g;
    auto* players = dynamic_cast<Players*>(g.game->findFirstChildOfClass("Players"));
    CHECK(players != nullptr);
    players->assignLocalPlayer(g.me.get());
    ScriptContext client(g.game, ScriptRole::Client);
    std::vector<std::string> output;
    std::vector<ChatLine> chat;
    client.setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            output.emplace_back(text);
    });
    client.setChatSink([&](const ChatLine& line) { chat.push_back(line); });
    CHECK(client
              .run(R"(
        local TextChatService = game:GetService("TextChatService")
        local general = TextChatService.TextChannels.RBXGeneral
        general.OnIncomingMessage = function(m)
            local p = Instance.new("TextChatMessageProperties")
            p.Text = string.upper(m.Text)
            return p
        end
        general.MessageReceived:Connect(function(m) print("got", m.Text, m.Status.Name, m.Metadata) end)
        local sent = general:SendAsync("hello", "meta")
        print("returned", sent.Status.Name, sent.TextSource.UserId)
        task.wait(0.1)
        print("now", sent.Status.Name)
        general:DisplaySystemMessage("<b>welcome</b>")
        TextChatService:DisplayBubble(workspace, "over here")
    )",
                   "Test")
              .has_value());
    for (int i = 0; i < 12; ++i)
        client.step(1.0 / 60.0);
    CHECK(std::ranges::find(output, std::string_view("returned Sending 42")) != output.end());
    CHECK(std::ranges::find(output, std::string_view("got HELLO Success meta")) != output.end());
    CHECK(std::ranges::find(output, std::string_view("now Success")) != output.end());
    CHECK(std::ranges::find(output, std::string_view("got <B>WELCOME</B> Success ")) != output.end());
    CHECK_EQ(chat.size(), std::size_t{4});
    CHECK(chat[0].status == "Sending" && chat[1].status == "Success" && chat[1].text == "HELLO");
    CHECK(chat[2].kind == ChatLine::Kind::System);
    CHECK(chat[3].kind == ChatLine::Kind::Bubble && chat[3].adornee.lock() != nullptr);
    // No TextSource of ours: nothing goes.
    g.mySource->setParent(nullptr);
    CHECK(!client.sendChat("hi").has_value());
}
