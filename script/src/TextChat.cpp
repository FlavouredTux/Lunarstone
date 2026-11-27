// TextChatService on a client: TextChannel:SendAsync and DisplaySystemMessage,
// TextChatService:DisplayBubble, the OnIncomingMessage callbacks, the
// MessageReceived / SendingMessage / UserMessageIntentSent events, and the
// server's replication signals. From the 0.742 client (RobloxPlayer
// x86_64, no slide; notes-chat.md in the research notes):
//
// - SendAsync (0x103cc7756) trims, escapes and tidies the text (prepareChatText),
//   makes the TextChatMessage with Status Sending and hands it to the sender
//   (0x103ce125a), which fires TextChatService's remote event
//   ClientToServerMessageReplicateSignalV4(text, metadata, messageId,
//   textSource, textChannel, verified, isEscapedFlagEnabled, presetId,
//   presetChatVersion) when DFFlagEnableClientToServerMessageReplicateSignalV4
//   is on (it is in the live client settings), else ...V3 without the last two.
// - The client listens to ServerToClientMessageReplicateSignal(text,
//   prefixText, metadata, messageId, textSource, textChannel, timestamp,
//   status) and ...V2(data), a dictionary of the same and more (0x103cf9058).
// - UserMessageIntentSentRemote(text, prefixText, metadata, messageId,
//   textSource, textChannel, timestamp) tells an account that may not read
//   chat (age check missing) that someone spoke: the text is a dot per character.
//
// GUESS: the order of the two OnIncomingMessage callbacks (the channel's,
// then TextChatService's), that the server's answer to our own message comes
// back as a fresh message, and what `verified` means (sent true for what the
// local player typed into the chat bar, false for a game script's SendAsync).

#include "InstanceApi.hpp"
#include "Services.hpp"

#include <lunar/script/TextChat.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>

namespace lunar {

std::string plainChatText(std::string_view richText)
{
    std::string out;
    out.reserve(richText.size());
    for (std::size_t i = 0; i < richText.size(); ++i) {
        const char c = richText[i];
        if (c == '<') {
            // A tag: <font ...>, </font>, <b>, <br /> ... (a lone '<' stays).
            const std::size_t close = richText.find('>', i);
            if (close != std::string_view::npos) {
                const std::string_view tag = richText.substr(i + 1, close - i - 1);
                if (tag == "br" || tag == "br/" || tag == "br /")
                    out += '\n';
                i = close;
                continue;
            }
        } else if (c == '&') {
            static constexpr std::pair<std::string_view, char> entities[] = {
                {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'},
            };
            bool replaced = false;
            for (const auto& [entity, character] : entities)
                if (richText.substr(i).starts_with(entity)) {
                    out += character;
                    i += entity.size() - 1;
                    replaced = true;
                    break;
                }
            if (replaced)
                continue;
        }
        out += c;
    }
    return out;
}

std::string prepareChatText(std::string_view text)
{
    // 0x103cc908a: surrounding " \t\n\v\f\r" trimmed.
    constexpr std::string_view space = " \t\n\v\f\r";
    const std::size_t first = text.find_first_not_of(space);
    if (first == std::string_view::npos)
        return {};
    text = text.substr(first, text.find_last_not_of(space) - first + 1);
    // 0x103cc8e5a: & < > as entities, & first.
    std::string escaped;
    for (const char c : text) {
        if (c == '&')
            escaped += "&amp;";
        else if (c == '<')
            escaped += "&lt;";
        else if (c == '>')
            escaped += "&gt;";
        else
            escaped += c;
    }
    // 0x103cc8a9e: "\t" -> three spaces, " {3,}" -> three spaces, "\n{3,}" -> three newlines.
    std::string out;
    std::size_t run = 0;
    char runOf = 0;
    for (const char c : escaped) {
        const char d = c == '\t' ? ' ' : c;
        const int count = c == '\t' ? 3 : 1;
        for (int k = 0; k < count; ++k) {
            run = d == runOf ? run + 1 : 1;
            runOf = d;
            if ((d == ' ' || d == '\n') && run > 3)
                continue;
            out += d;
        }
    }
    return out;
}

} // namespace lunar

namespace lunar::lua {

struct TextChatState {
    std::function<void(const ChatLine&)> sink;
    // Messages of ours waiting for the server's answer, by message id.
    std::map<std::string, std::weak_ptr<TextChatMessage>, std::less<>> sending;
    PresetChatAccess preset;
};

namespace {

TextChatState& state(Shared& shared)
{
    if (!shared.textChat)
        shared.textChat = std::make_shared<TextChatState>();
    return *shared.textChat;
}

PackedArgs instanceArgs(std::initializer_list<const Instance*> instances)
{
    PackedArgs args;
    for (const Instance* instance : instances)
        args.push_back(
            packed(instance ? Value(std::const_pointer_cast<Instance>(instance->shared_from_this())) : Value()));
    return args;
}

Instance* textChatService(const DataModel& game)
{
    return game.findFirstChildOfClass("TextChatService");
}

Player* localPlayer(const DataModel& game)
{
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

Player* playerByUserId(const DataModel& game, std::int64_t userId)
{
    if (userId == 0)
        return nullptr;
    if (const Instance* players = game.findFirstChildOfClass("Players"))
        for (const InstancePtr& child : players->children())
            if (auto* player = dynamic_cast<Player*>(child.get()); player && player->userId() == userId)
                return player;
    return nullptr;
}

// A player's TextSource in a channel; its CanSend says whether they may speak there.
TextSource* sourceOf(const TextChannel& channel, std::int64_t userId)
{
    for (const InstancePtr& child : channel.children())
        if (auto* source = dynamic_cast<TextSource*>(child.get()); source && source->userId() == userId)
            return source;
    return nullptr;
}

// Where the chat bar sends: ChatInputBarConfiguration.TargetTextChannel, else RBXGeneral.
TextChannel* chatBarChannel(const Instance& service)
{
    if (const auto* bar
        = dynamic_cast<const ChatInputBarConfiguration*>(service.findFirstChildOfClass("ChatInputBarConfiguration")))
        if (auto* target = dynamic_cast<TextChannel*>(bar->targetTextChannel()))
            return target;
    const Instance* channels = service.findFirstChild("TextChannels");
    return channels ? dynamic_cast<TextChannel*>(channels->findFirstChild("RBXGeneral")) : nullptr;
}

std::string newMessageId(std::int64_t userId)
{
    // "<userId>-{guid}", the shape other clients' ids arrive in (GUESS: the
    // lowercase hex, which most of them use).
    const std::uint64_t a = scriptEntropy(), b = scriptEntropy();
    return std::format("{}-{{{:08x}-{:04x}-4{:03x}-{:04x}-{:012x}}}", userId, static_cast<std::uint32_t>(a >> 32),
                       static_cast<std::uint32_t>((a >> 16) & 0xFFFF), static_cast<std::uint32_t>(a & 0xFFF),
                       static_cast<std::uint32_t>(0x8000 | ((b >> 48) & 0x3FFF)), b & 0xFFFFFFFFFFFFull);
}

std::int64_t nowMillis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string_view statusName(Enum::TextChatMessageStatus status)
{
    const EnumInfo& info = enums::info<Enum::TextChatMessageStatus>();
    const EnumItem* item = info.find(static_cast<std::uint32_t>(status));
    return item ? item->name : "Unknown";
}

// The line the chat window shows for a message as it stands now.
ChatLine lineOf(const DataModel& game, const TextChatMessage& message, ChatLine::Kind kind, std::int64_t timestamp)
{
    ChatLine line;
    line.kind = kind;
    if (const TextChannel* channel = message.textChannel())
        line.channel = channel->name();
    line.prefix = message.prefixText();
    line.text = message.text();
    line.translation = message.translation();
    line.messageId = message.messageId();
    line.status = std::string(statusName(message.status()));
    line.timestamp = timestamp;
    if (const TextSource* source = message.textSource()) {
        line.userId = source->userId();
        line.senderName = std::string(source->name());
        if (const Player* player = playerByUserId(game, source->userId())) {
            line.senderName = std::string(player->name());
            line.senderDisplayName = player->displayName();
        }
        if (const Player* me = localPlayer(game); me && me->userId() == source->userId())
            line.own = true;
    }
    if (line.userId == 0 && kind != ChatLine::Kind::System) {
        // A message can arrive before its TextSource replicates. Its message
        // ID starts with the sender's user ID followed by a hyphen.
        std::int64_t id = 0;
        const std::string& m = line.messageId;
        const auto [end, ec] = std::from_chars(m.data(), m.data() + m.size(), id);
        if (ec == std::errc{} && end != m.data() && end < m.data() + m.size() && *end == '-' && id > 0) {
            line.userId = id;
            if (const Player* player = playerByUserId(game, id)) {
                line.senderName = std::string(player->name());
                line.senderDisplayName = player->displayName();
            }
            if (const Player* me = localPlayer(game); me && me->userId() == id)
                line.own = true;
        }
    }
    if (line.senderDisplayName.empty())
        line.senderDisplayName = line.senderName;
    return line;
}

// The prefix the server gives a player's message is the display name followed by a colon.
std::string prefixOf(const Player& player)
{
    return std::format("{}:", player.displayName().empty() ? player.name() : player.displayName());
}

void deliverLine(Shared& shared, const ChatLine& line)
{
    if (const TextChatState& s = state(shared); s.sink)
        s.sink(line);
}

// TextChannel.OnIncomingMessage, then TextChatService.OnIncomingMessage: each
// may return TextChatMessageProperties whose non-empty PrefixText, Text and
// Translation replace the message's. A callback that yields has no say.
void runIncomingCallbacks(Shared& shared, const Instance* service, const std::shared_ptr<TextChatMessage>& message)
{
    for (const Instance* owner : {static_cast<const Instance*>(message->textChannel()), service}) {
        if (!owner)
            continue;
        const auto callback = shared.callback(*owner, "OnIncomingMessage");
        if (!callback || !callback->runtime || callback->ref == LUA_NOREF)
            continue;
        struct Outcome {
            bool finished = false;
            bool ok = false;
            PackedArgs results;
        };
        auto outcome = std::make_shared<Outcome>();
        invokeCallback(*callback, instanceArgs({message.get()}), [outcome](bool ok, PackedArgs results) {
            outcome->finished = true;
            outcome->ok = ok;
            outcome->results = std::move(results);
        });
        if (!outcome->finished || !outcome->ok || outcome->results.empty())
            continue;
        const auto* instance = std::get_if<InstancePtr>(&outcome->results.front().value);
        const auto* properties = instance ? dynamic_cast<const TextChatMessageProperties*>(instance->get()) : nullptr;
        if (!properties)
            continue;
        if (!properties->prefixText().empty())
            message->setPrefixText(properties->prefixText());
        if (!properties->text().empty())
            message->setText(properties->text());
        if (!properties->translation().empty())
            message->setTranslation(properties->translation());
    }
}

// A message as the server replicated it.
struct Incoming {
    ChatLine::Kind kind = ChatLine::Kind::Message;
    std::string text, prefixText, metadata, messageId, translation;
    std::weak_ptr<Instance> source, channel;
    std::int64_t timestamp = 0;
    Enum::TextChatMessageStatus status = Enum::TextChatMessageStatus::Success;
};

void receiveMessage(Shared& shared, const Incoming& in)
{
    Runtime* client = shared.runtimeFor(ScriptRole::Client);
    if (!client)
        client = shared.runtimeFor(ScriptRole::Standalone);
    auto* channel = dynamic_cast<TextChannel*>(in.channel.lock().get());
    if (!client || !channel)
        return; // a channel this client doesn't have (GUESS: the real client drops those too)
    Instance* service = textChatService(client->game());
    auto message = std::make_shared<TextChatMessage>();
    message->setMessageId(in.messageId);
    message->setText(in.text);
    message->setPrefixText(in.prefixText);
    message->setMetadata(in.metadata);
    message->setTranslation(in.translation);
    message->setStatus(in.status);
    message->setTextChannel(channel);
    message->setTextSource(dynamic_cast<TextSource*>(in.source.lock().get()));
    if (in.kind == ChatLine::Kind::Hidden) {
        // The client raises TextChatService.UserMessageIntentSent(userIntentMessage) (0x1005dce3a).
        if (service)
            shared.events.fire(*service, "UserMessageIntentSent", instanceArgs({message.get()}));
        deliverLine(shared, lineOf(client->game(), *message, ChatLine::Kind::Hidden, in.timestamp));
        return;
    }
    runIncomingCallbacks(shared, service, message);
    // The answer to one of ours: the message SendAsync returned learns how it went.
    TextChatState& s = state(shared);
    if (auto it = s.sending.find(in.messageId); it != s.sending.end()) {
        if (auto mine = it->second.lock())
            mine->setStatus(in.status);
        s.sending.erase(it);
    }
    shared.events.fire(*channel, "MessageReceived", instanceArgs({message.get()}));
    if (service)
        shared.events.fire(*service, "MessageReceived", instanceArgs({message.get()}));
    deliverLine(shared, lineOf(client->game(), *message, ChatLine::Kind::Message, in.timestamp));
}

std::string stringArg(std::span<const RemoteValue> args, std::size_t i)
{
    const auto* s = i < args.size() ? std::get_if<std::string>(&args[i].value) : nullptr;
    return s ? *s : std::string();
}

std::weak_ptr<Instance> instanceArg(std::span<const RemoteValue> args, std::size_t i)
{
    const auto* p = i < args.size() ? std::get_if<InstancePtr>(&args[i].value) : nullptr;
    return p ? std::weak_ptr<Instance>(*p) : std::weak_ptr<Instance>();
}

std::int64_t numberArg(std::span<const RemoteValue> args, std::size_t i)
{
    const auto* n = i < args.size() ? std::get_if<double>(&args[i].value) : nullptr;
    return n ? static_cast<std::int64_t>(*n) : 0;
}

// A status as the network has it: the EnumItem, its value, or its name.
Enum::TextChatMessageStatus statusOf(const Value& value)
{
    const EnumInfo& info = enums::info<Enum::TextChatMessageStatus>();
    if (const auto* item = std::get_if<EnumItem>(&value))
        return static_cast<Enum::TextChatMessageStatus>(item->value);
    if (const auto* number = std::get_if<double>(&value))
        return static_cast<Enum::TextChatMessageStatus>(static_cast<std::uint32_t>(*number));
    if (const auto* name = std::get_if<std::string>(&value))
        if (const EnumItem* item = info.find(*name))
            return static_cast<Enum::TextChatMessageStatus>(item->value);
    return Enum::TextChatMessageStatus::Unknown;
}

// ServerToClientMessageReplicateSignalV2's dictionary (keys from 0x103cf9058).
Incoming fromDictionary(const RemoteTable& data)
{
    Incoming in;
    for (const auto& [key, member] : data.members) {
        const Value& v = member.value;
        const auto* s = std::get_if<std::string>(&v);
        if (key == "text" && s)
            in.text = *s;
        else if (key == "prefixText" && s)
            in.prefixText = *s;
        else if (key == "metadata" && s)
            in.metadata = *s;
        else if (key == "messageId" && s)
            in.messageId = *s;
        else if (key == "translation" && s)
            in.translation = *s;
        else if (key == "textSource" && std::holds_alternative<InstancePtr>(v))
            in.source = std::get<InstancePtr>(v);
        else if (key == "textChannel" && std::holds_alternative<InstancePtr>(v))
            in.channel = std::get<InstancePtr>(v);
        else if (key == "timestamp" && std::holds_alternative<double>(v))
            in.timestamp = static_cast<std::int64_t>(std::get<double>(v));
        else if (key == "status")
            in.status = statusOf(v);
        else if (key == "isHiddenMessage" && std::holds_alternative<bool>(v) && std::get<bool>(v))
            in.kind = ChatLine::Kind::Hidden;
    }
    return in;
}

bool triggerCommand(Shared& shared, Instance& service, TextSource& source, std::string_view text)
{
    const auto end = text.find_first_of(" \t\n");
    std::string alias(text.substr(0, end));
    const auto lower = [](std::string value) {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    alias = lower(std::move(alias));
    for (const auto& descendant : service.descendants()) {
        auto* command = instanceCast<TextChatCommand>(descendant.get());
        if (!command || !command->enabled())
            continue;
        auto matches = [&](const std::string& configured) {
            if (configured.empty())
                return false;
            const auto value = lower(configured);
            return alias == value || (value.front() != '/' && alias == "/" + value);
        };
        if (matches(command->primaryAlias()) || matches(command->secondaryAlias())) {
            shared.events.fire(*command, "Triggered",
                               {packed(Value(source.shared_from_this())), packed(Value(std::string(text)))});
            return true;
        }
    }
    return false;
}

std::shared_ptr<TextChatMessage> sendMessage(Runtime& rt, TextChannel& channel, std::string_view typed,
                                             std::string metadata, bool verified, std::string& error,
                                             const ChatPreset* preset = nullptr)
{
    Shared& shared = rt.shared();
    Instance* service = textChatService(rt.game());
    if (!service || !channel.isDescendantOf(service)) {
        error = "SendAsync error: TextChannel not a descendant of TextChatService";
        return nullptr;
    }
    Player* me = localPlayer(rt.game());
    if (!me) {
        error = "SendAsync error: there is no local player";
        return nullptr;
    }
    auto message = std::make_shared<TextChatMessage>();
    message->setMessageId(newMessageId(me->userId()));
    // A preset goes as the catalog has it (SendPresetAsync 0x103cc91ae sets
    // the text it found without SendAsync's tidying).
    message->setText(preset ? preset->text : prepareChatText(typed));
    message->setMetadata(std::move(metadata));
    message->setPrefixText(prefixOf(*me));
    message->setTextChannel(&channel);
    TextSource* source = sourceOf(channel, me->userId());
    message->setTextSource(source);
    if (!source || !source->canSend() || message->text().empty()) {
        // GUESS: refused on the spot, the way the server would answer it.
        message->setStatus(message->text().empty() ? Enum::TextChatMessageStatus::Unknown
                                                   : Enum::TextChatMessageStatus::InvalidTextChannelPermissions);
        return message;
    }
    if (!preset && triggerCommand(shared, *service, *source, typed)) {
        message->setStatus(Enum::TextChatMessageStatus::Success);
        return message;
    }
    message->setStatus(Enum::TextChatMessageStatus::Sending);
    // What goes out is the text as sent, whatever OnIncomingMessage makes of it on our screen.
    const std::string text = message->text();
    const std::string metadataSent = message->metadata();
    shared.events.fire(*service, "SendingMessage", instanceArgs({message.get()}));
    runIncomingCallbacks(shared, service, message);
    deliverLine(shared, lineOf(rt.game(), *message, ChatLine::Kind::Message, nowMillis()));
    state(shared).sending[message->messageId()] = message;
    if (shared.remoteSink) {
        // isEscapedFlagEnabled is FFlag TextChatEscapePostFilterClient (off by
        // default, not in the live settings). The 0.742 client fills presetId
        // and presetChatVersion only with EnableTextChatPresets (off; the live
        // settings have newer clients' EnableTextChatPresets9 on), so a
        // preset is how a newer client sends it.
        std::vector<RemoteValue> args{
            remoteValue(Value(text)),
            remoteValue(Value(metadataSent)),
            remoteValue(Value(message->messageId())),
            remoteValue(Value(source->shared_from_this())),
            remoteValue(Value(channel.shared_from_this())),
            remoteValue(Value(verified)),
            remoteValue(Value(false)),
            remoteValue(Value(preset ? preset->id : std::string())),
            remoteValue(Value(preset ? preset->version : std::string())),
        };
        shared.remoteSink(*service, "ClientToServerMessageReplicateSignalV4", std::move(args));
    } else {
        // No server (Play Solo): it comes back next frame as the server would
        // send it, unfiltered.
        Incoming back;
        back.text = text;
        back.prefixText = prefixOf(*me);
        back.metadata = metadataSent;
        back.messageId = message->messageId();
        back.source = source->weak_from_this();
        back.channel = channel.weak_from_this();
        back.timestamp = nowMillis();
        shared.invocations.emplace_back([&shared, back = std::move(back)] { receiveMessage(shared, back); });
    }
    return message;
}

TextChannel& selfChannel(lua_State* L)
{
    auto* channel = dynamic_cast<TextChannel*>(&self(L));
    if (!channel)
        expectedColon(L);
    return *channel;
}

// TextChannel:SendAsync(message, metadata): the message, Sending until the
// server answers. Doesn't wait for the answer (GUESS: the client's yields
// only until the message is on its way).
int channelSendAsync(lua_State* L)
{
    TextChannel& channel = selfChannel(L);
    const std::string_view text = checkString(L, 2);
    std::string metadata = lua_isnoneornil(L, 3) ? std::string() : std::string(checkString(L, 3));
    Runtime& rt = Runtime::of(L);
    if (rt.role() == ScriptRole::Server)
        raise(L, "TextChannel:SendAsync can only be called on the client.");
    std::string error;
    auto message = sendMessage(rt, channel, text, std::move(metadata), false, error);
    if (!message)
        raise(L, error);
    return returnInstance(L, message.get());
}

// TextChannel:DisplaySystemMessage(text, metadata): a line only this client sees.
int channelDisplaySystemMessage(lua_State* L)
{
    TextChannel& channel = selfChannel(L);
    const std::string_view text = checkString(L, 2);
    std::string metadata = lua_isnoneornil(L, 3) ? std::string() : std::string(checkString(L, 3));
    Runtime& rt = Runtime::of(L);
    if (rt.role() == ScriptRole::Server)
        raise(L, "TextChannel:DisplaySystemMessage can only be called on the client.");
    Instance* service = textChatService(rt.game());
    if (!service || !channel.isDescendantOf(service))
        raise(L, "DisplaySystemMessage error: TextChannel not a descendant of TextChatService");
    auto message = std::make_shared<TextChatMessage>();
    message->setMessageId(newMessageId(0));
    message->setText(std::string(text));
    message->setMetadata(std::move(metadata));
    message->setStatus(Enum::TextChatMessageStatus::Success);
    message->setTextChannel(&channel);
    Shared& shared = rt.shared();
    runIncomingCallbacks(shared, service, message);
    shared.events.fire(channel, "MessageReceived", instanceArgs({message.get()}));
    shared.events.fire(*service, "MessageReceived", instanceArgs({message.get()}));
    deliverLine(shared, lineOf(rt.game(), *message, ChatLine::Kind::System, nowMillis()));
    return returnInstance(L, message.get());
}

// TextChatService:DisplayBubble(partOrCharacter, message): a bubble only this client sees.
int serviceDisplayBubble(lua_State* L)
{
    self(L);
    Instance& adornee = checkInstance(L, 2);
    const std::string_view text = checkString(L, 3);
    Runtime& rt = Runtime::of(L);
    if (rt.role() == ScriptRole::Server)
        raise(L, "TextChatService:DisplayBubble can only be called on the client.");
    ChatLine line;
    line.kind = ChatLine::Kind::Bubble;
    line.text = std::string(text);
    line.status = "Success";
    line.timestamp = nowMillis();
    line.adornee = adornee.weak_from_this();
    deliverLine(rt.shared(), line);
    return 0;
}

constexpr auto methods = std::to_array<Method>({
    {"SendAsync", "TextChannel", channelSendAsync},
    {"DisplaySystemMessage", "TextChannel", channelDisplaySystemMessage},
    {"DisplayBubble", "TextChatService", serviceDisplayBubble},
});

} // namespace

std::span<const Method> textChatMethods()
{
    return methods;
}

bool receiveTextChat(Shared& shared, Instance&, std::string_view event, std::span<const RemoteValue> arguments)
{
    Incoming in;
    if (event == "ServerToClientMessageReplicateSignal" || event == "UserMessageIntentSentRemote") {
        in.kind = event == "UserMessageIntentSentRemote" ? ChatLine::Kind::Hidden : ChatLine::Kind::Message;
        in.text = stringArg(arguments, 0);
        in.prefixText = stringArg(arguments, 1);
        in.metadata = stringArg(arguments, 2);
        in.messageId = stringArg(arguments, 3);
        in.source = instanceArg(arguments, 4);
        in.channel = instanceArg(arguments, 5);
        in.timestamp = numberArg(arguments, 6);
        if (in.kind == ChatLine::Kind::Message && arguments.size() > 7)
            in.status = statusOf(arguments[7].value);
    } else if (event == "ServerToClientPresetChatConfigChangedSignal") {
        // (enabled, config): [live] true, {"surface":"in-experience-chat","defaultVersion":"default-draft-v4",...}.
        PresetChatAccess& preset = state(shared).preset;
        const auto* enabled = arguments.empty() ? nullptr : std::get_if<bool>(&arguments[0].value);
        preset.enabled = enabled && *enabled;
        preset.config = stringArg(arguments, 1);
        return true;
    } else if (event == "ServerToClientPresetChatUserAccessChangedSignal") {
        state(shared).preset.access = stringArg(arguments, 0); // [live] "Enabled"
        return true;
    } else if (event == "ServerToClientMessageReplicateSignalV2") {
        if (arguments.empty() || !arguments[0].table)
            return true;
        in = fromDictionary(*arguments[0].table);
    } else {
        return false;
    }
    // Scripts see it from the next frame, like a remote's events.
    shared.invocations.emplace_back([&shared, in = std::move(in)] { receiveMessage(shared, in); });
    return true;
}

void setChatSink(Shared& shared, std::function<void(const ChatLine&)> sink)
{
    state(shared).sink = std::move(sink);
}

namespace {

// Where the chat bar sends and why it can't: the channel and our TextSource in it.
std::expected<TextChannel*, std::string> chatBarTarget(Runtime& client, std::string_view channelName = {})
{
    if (client.role() == ScriptRole::Server)
        return std::unexpected("the server has no chat bar");
    Instance* service = textChatService(client.game());
    if (!service)
        return std::unexpected("the game has no TextChatService");
    TextChannel* channel = nullptr;
    if (channelName.empty())
        channel = chatBarChannel(*service);
    else if (Instance* channels = service->findFirstChild("TextChannels"))
        channel = instanceCast<TextChannel>(channels->findFirstChild(channelName));
    if (!channel)
        return std::unexpected("there is no TextChatService.TextChannels.RBXGeneral to send to");
    const Player* me = localPlayer(client.game());
    if (!me)
        return std::unexpected("there is no local player");
    const TextSource* source = sourceOf(*channel, me->userId());
    if (!source)
        return std::unexpected(std::format("the server gave us no TextSource in {}", channel->fullName()));
    if (!source->canSend())
        return std::unexpected(
            std::format("our TextSource in {} may not send (CanSend is false)", channel->fullName()));
    return channel;
}

} // namespace

PresetChatAccess presetChatAccess(Shared& shared)
{
    return state(shared).preset;
}

std::expected<std::string, std::string> sendPresetChat(Runtime& client, const ChatPreset& preset)
{
    auto channel = chatBarTarget(client);
    if (!channel)
        return std::unexpected(channel.error());
    const PresetChatAccess& access = state(client.shared()).preset;
    // SendPresetAsync's "Preset chat is not enabled" (0x103cc92bb).
    if (!access.enabled || access.access != "Enabled")
        return std::unexpected(std::format("preset chat is not enabled here (enabled {}, our access \"{}\")",
                                           access.enabled, access.access));
    if (preset.id.empty() || preset.text.empty() || preset.version.empty())
        return std::unexpected("the preset needs an id, its text and the catalog version");
    std::string error;
    auto message = sendMessage(client, **channel, {}, {}, true, error, &preset);
    if (!message)
        return std::unexpected(error);
    return message->messageId();
}

void requestWhisper(Runtime& client, std::int64_t target,
                    std::function<void(std::expected<std::string, std::string>)> done)
{
    if (!done)
        return;
    const Player* local = localPlayer(client.game());
    if (!client.isClient() || !local || local->userId() <= 0) {
        done(std::unexpected("There is no local chat user"));
        return;
    }
    if (target == local->userId()) {
        done(std::unexpected("You cannot whisper to yourself"));
        return;
    }
    bool present = false;
    if (const auto* players = client.game().findService("Players"))
        for (const auto& child : players->children())
            if (const auto* player = instanceCast<Player>(child.get()); player && player->userId() == target)
                present = true;
    if (!present) {
        done(std::unexpected("That player is not in the experience"));
        return;
    }
    const std::string name
        = std::format("RBXWhisper:{}_{}", std::min(local->userId(), target), std::max(local->userId(), target));
    auto validate = [weak = client.game().weak_from_this(), ownId = local->userId(), target, name,
                     done = std::move(done)](bool ok, PackedArgs values) mutable {
        if (!ok) {
            std::string message = "The whisper channel request failed";
            if (!values.empty())
                if (const auto* text = std::get_if<std::string>(&values[0].value))
                    message = *text;
            done(std::unexpected(std::move(message)));
            return;
        }
        auto game = weak.lock();
        const auto* returned = values.empty() ? nullptr : std::get_if<InstancePtr>(&values[0].value);
        const auto* channel = returned ? instanceCast<TextChannel>(returned->get()) : nullptr;
        const Instance* service = game ? game->findFirstChildOfClass("TextChatService") : nullptr;
        if (!channel || !service || channel->destroyed() || channel->name() != name
            || !channel->isDescendantOf(service)) {
            done(std::unexpected("The server did not provide an authorized whisper channel"));
            return;
        }
        bool own = false, peer = false, other = false;
        for (const auto& child : channel->children())
            if (const auto* source = instanceCast<TextSource>(child.get())) {
                own = own || source->userId() == ownId;
                peer = peer || source->userId() == target;
                other = other || (source->userId() != ownId && source->userId() != target);
            }
        if (!own || !peer || other) {
            done(std::unexpected("The whisper channel participants are not ready"));
            return;
        }
        done(name);
    };
    if (Instance* service = textChatService(client.game()))
        if (Instance* existing = service->findFirstChild(name, true)) {
            validate(true, {packed(Value(existing->shared_from_this()))});
            return;
        }
    Instance* replicated = client.game().findFirstChildOfClass("RobloxReplicatedStorage");
    Instance* chat = replicated ? replicated->findFirstChild("ExperienceChat") : nullptr;
    Instance* request = chat ? chat->findFirstChild("WhisperChat") : nullptr;
    if (!request || !request->isA("RemoteFunction")) {
        validate(false, {packed(Value(std::string("Whisper chat is not available in this experience")))});
        return;
    }
    // Shipped 0.742 ExpChat eDQ requests a channel through this RemoteFunction
    // with the target's string user id. Only its server result can be selected.
    invokeServerForCore(client, *request, {packed(Value(std::to_string(target)))}, std::move(validate));
}

std::expected<std::string, std::string> sendChat(Runtime& client, std::string_view text, std::string_view channelName)
{
    auto channel = chatBarTarget(client, channelName);
    if (!channel)
        return std::unexpected(channel.error());
    if (prepareChatText(text).empty())
        return std::unexpected("there's nothing to send");
    std::string error;
    auto message = sendMessage(client, **channel, text, {}, true, error);
    if (!message)
        return std::unexpected(error);
    return message->messageId();
}

} // namespace lunar::lua
