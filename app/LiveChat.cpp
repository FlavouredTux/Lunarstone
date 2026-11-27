#include "LiveChat.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Instance.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/ui/ChatCommands.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>

namespace {

// What the core chat says when the server turns a message of ours down.
// GUESS: the wording, from memory of the in-experience chat's strings.
std::string_view refusal(std::string_view status)
{
    if (status == "Floodchecked")
        return "You must wait before sending another message.";
    if (status == "MessageTooLong")
        return "Your message exceeds the maximum message length.";
    if (status == "InvalidPrivacySettings")
        return "Your chat settings prevent you from sending messages.";
    if (status == "InvalidTextChannelPermissions")
        return "You do not have permission to chat in this channel.";
    if (status == "TextFilterFailed")
        return "Your message could not be sent.";
    if (status == "ModerationTimeout")
        return "You are temporarily unable to chat.";
    return "Your message could not be sent.";
}

std::string displayedName(const lunar::ChatLine& line)
{
    std::string prefix = lunar::plainChatText(line.prefix);
    while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == ':'))
        prefix.pop_back();
    return prefix.empty() ? line.senderDisplayName : prefix;
}

} // namespace

std::string describeChatLine(const lunar::ChatLine& line)
{
    using Kind = lunar::ChatLine::Kind;
    const std::string text = lunar::plainChatText(line.text);
    switch (line.kind) {
    case Kind::System:
        return std::format("chat: [{}] {}", line.channel.empty() ? "system" : line.channel, text);
    case Kind::Bubble: {
        const auto adornee = line.adornee.lock();
        return std::format("chat: bubble over {}: {}", adornee ? adornee->fullName() : "nothing", text);
    }
    case Kind::Hidden:
        return std::format("chat: {}: {} (hidden: {} characters this account may not read)",
                           !line.senderName.empty() ? line.senderName
                           : line.userId != 0       ? std::format("user {}", line.userId)
                                                    : std::string("?"),
                           text, text.size());
    case Kind::Message:
        break;
    }
    const std::string name = !line.senderName.empty() ? line.senderName
                             : line.userId != 0       ? std::format("user {}", line.userId)
                                                      : std::string("?");
    std::string out = std::format("chat: {}: {}", name, text);
    if (line.status != "Success")
        out += std::format(" ({})", line.status);
    if (!line.channel.empty() && line.channel != "RBXGeneral")
        out += std::format(" [{}]", line.channel);
    if (const std::string shown = displayedName(line); shown != line.senderName && shown != line.senderDisplayName)
        out += std::format(" (shown as \"{}\")", shown);
    else if (line.senderDisplayName != line.senderName)
        out += std::format(" (display name \"{}\")", line.senderDisplayName);
    return out;
}

std::optional<lunar::ui::ChatMessage> windowMessage(const lunar::ChatLine& line)
{
    using Kind = lunar::ChatLine::Kind;
    lunar::ui::ChatMessage message;
    message.text = line.translation.empty() ? line.text : line.translation;
    message.richText = true;
    message.prefix = line.prefix;
    message.messageId = line.messageId;
    message.channel = line.channel;
    message.userId = line.userId;
    switch (line.kind) {
    case Kind::Bubble:
        return std::nullopt;
    case Kind::System:
        message.kind = lunar::ui::ChatMessage::Kind::System;
        return message;
    case Kind::Hidden:
        // A message this account can't read: the dots it came as, greyed.
        message.textColor = lunar::Color3::fromRGB(170, 170, 170);
        break;
    case Kind::Message:
        if (line.status == "Sending")
            message.pending = true;
        else if (line.status != "Success") {
            if (!line.own)
                return std::nullopt;
            message.kind = lunar::ui::ChatMessage::Kind::Error;
            message.text = refusal(line.status);
            return message;
        }
        break;
    }
    message.kind = lunar::ui::ChatMessage::Kind::Player;
    message.fromName = line.senderName;
    message.displayName = displayedName(line);
    return message;
}

std::string presetCatalogVersion(const lunar::PresetChatAccess& access)
{
    const auto doc = lunar::assets::json::parse(access.config);
    if (!doc)
        return {};
    for (const char* key : {"customVersion", "defaultVersion"})
        if (const auto* v = (*doc)[key]; v && v->string() && !v->string()->empty())
            return *v->string();
    return {};
}

std::expected<std::vector<lunar::ChatPreset>, std::string>
fetchChatPresets(const lunar::assets::RobloxWeb& web, std::string_view version, std::string_view locale)
{
    lunar::assets::HttpRequest request;
    request.url = std::format("https://apis.roblox.com/preset-chat/v1/preset-chat-options?version={}&locale={}",
                              version, locale);
    auto response = web.withCookie(std::move(request));
    if (!response)
        return std::unexpected("preset-chat-options: " + response.error());
    if (response->status != 200)
        return std::unexpected("preset-chat-options: " + lunar::assets::describeHttpFailure(*response));
    // {"version", "categoryGroups": [{"name", "categories": [{"name", "messages": [{"presetId", "presetType",
    // "value"}]}]}]}
    using lunar::assets::json::Array;
    using lunar::assets::json::Value;
    const auto doc = lunar::assets::json::parse(response->body);
    if (!doc)
        return std::unexpected("preset-chat-options: the answer isn't JSON");
    const auto arrayOf = [](const Value& v, std::string_view key) -> const Array* {
        const Value* member = v[key];
        return member ? member->array() : nullptr;
    };
    const auto stringOf = [](const Value& v, std::string_view key) -> std::string {
        const Value* member = v[key];
        const std::string* s = member ? member->string() : nullptr;
        return s ? *s : std::string();
    };
    const Array* groups = arrayOf(*doc, "categoryGroups");
    if (!groups)
        return std::unexpected("preset-chat-options: no categoryGroups in the answer");
    std::string catalog = stringOf(*doc, "version");
    if (catalog.empty())
        catalog = version;
    std::vector<lunar::ChatPreset> presets;
    for (const Value& group : *groups) {
        const Array* categories = arrayOf(group, "categories");
        for (std::size_t c = 0; categories && c < categories->size(); ++c) {
            const Array* messages = arrayOf((*categories)[c], "messages");
            for (std::size_t m = 0; messages && m < messages->size(); ++m) {
                const Value& message = (*messages)[m];
                const std::string type = stringOf(message, "presetType");
                lunar::ChatPreset preset{stringOf(message, "presetId"), stringOf(message, "value"), catalog};
                if (!preset.id.empty() && !preset.text.empty() && (type.empty() || type == "Text"))
                    presets.push_back(std::move(preset));
            }
        }
    }
    return presets;
}

std::expected<std::string, std::string> sayPresetPhrase(lunar::ScriptContext& client,
                                                        const lunar::assets::RobloxWeb& web, std::string_view phrase)
{
    const lunar::PresetChatAccess access = client.presetChatAccess();
    const std::string version = presetCatalogVersion(access);
    if (!access.enabled || access.access != "Enabled" || version.empty())
        return std::unexpected(std::format("no preset chat here (enabled {}, our access \"{}\", catalog \"{}\")",
                                           access.enabled, access.access, version));
    auto presets = fetchChatPresets(web, version);
    if (!presets)
        return std::unexpected(presets.error());
    const auto same = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b,
                                  [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    };
    const auto it = std::ranges::find_if(*presets, [&](const lunar::ChatPreset& p) { return same(p.text, phrase); });
    if (it == presets->end())
        return std::unexpected(std::format("catalog {} ({} phrases) has no \"{}\"", version, presets->size(), phrase));
    auto sent = client.sendPresetChat(*it);
    if (!sent)
        return std::unexpected(sent.error());
    return std::format("said preset \"{}\" ({}, catalog {}, message id {})", it->text, it->id, version, *sent);
}

namespace {

// What a bubble hangs over: the speaker's character's Head, or the part (or
// a model's Head or PrimaryPart) DisplayBubble named.
lunar::Instance* bubbleAdornee(const lunar::DataModel& game, const lunar::ChatLine& line)
{
    lunar::Instance* target = nullptr;
    if (line.kind == lunar::ChatLine::Kind::Bubble) {
        target = line.adornee.lock().get();
    } else if (line.userId != 0) {
        if (const lunar::Instance* players = game.findFirstChildOfClass("Players"))
            for (const auto& child : players->children())
                if (auto* player = dynamic_cast<lunar::Player*>(child.get()); player && player->userId() == line.userId)
                    target = player->character();
    }
    if (target && !target->isA("BasePart")) {
        if (lunar::Instance* head = target->findFirstChild("Head"); head && head->isA("BasePart"))
            return head;
        if (auto* model = dynamic_cast<lunar::Model*>(target))
            return model->primaryPart();
        return nullptr;
    }
    return target;
}

// UDim2.new(xScale, xOffset, yScale, yOffset).
lunar::UDim2 box(float xScale, int xOffset, float yScale, int yOffset)
{
    return {{xScale, xOffset}, {yScale, yOffset}};
}

} // namespace

ChatBubbles::ChatBubbles(std::shared_ptr<lunar::DataModel> game) : game_(std::move(game)) {}

ChatBubbles::~ChatBubbles()
{
    if (auto f = folder_.lock())
        f->destroy();
}

lunar::Instance* ChatBubbles::folder()
{
    if (auto f = folder_.lock(); f && f->parent())
        return f.get();
    lunar::Instance* core = game_->getService("CoreGui");
    auto f = std::make_shared<lunar::Folder>();
    f->setName("BubbleChat");
    f->setParent(core);
    folder_ = f;
    return f.get();
}

void ChatBubbles::add(const lunar::ChatLine& line, double now)
{
    using Kind = lunar::ChatLine::Kind;
    // Delivered messages (ours once the server has it), hidden ones' dots, DisplayBubble.
    if (line.kind == Kind::System || (line.kind == Kind::Message && line.status != "Success"))
        return;
    const std::string text = lunar::plainChatText(line.text);
    if (text.empty())
        return;
    lunar::Instance* adornee = bubbleAdornee(*game_, line);
    if (!adornee)
        return;
    Speaker& speaker = speakers_[adornee];
    speaker.adornee = adornee->weak_from_this();
    speaker.bubbles.push_back({text, now});
    if (speaker.bubbles.size() > maxBubbles)
        speaker.bubbles.erase(speaker.bubbles.begin());
    rebuild(speaker);
}

void ChatBubbles::update(double now)
{
    for (auto it = speakers_.begin(); it != speakers_.end();) {
        Speaker& speaker = it->second;
        const auto before = speaker.bubbles.size();
        std::erase_if(speaker.bubbles, [&](const Bubble& b) { return now - b.shownAt >= duration; });
        if (speaker.bubbles.empty() || speaker.adornee.expired()) {
            if (auto gui = speaker.gui.lock())
                gui->destroy();
            it = speakers_.erase(it);
            continue;
        }
        if (speaker.bubbles.size() != before)
            rebuild(speaker);
        ++it;
    }
}

std::size_t ChatBubbles::count() const
{
    std::size_t n = 0;
    for (const auto& [adornee, speaker] : speakers_)
        n += speaker.bubbles.size();
    return n;
}

void ChatBubbles::rebuild(Speaker& speaker)
{
    using namespace lunar;
    if (auto old = speaker.gui.lock())
        old->destroy();
    auto adornee = speaker.adornee.lock();
    if (!adornee)
        return;
    // BubbleChatConfiguration's defaults in Roblox's API: TextSize 16, text
    // (57, 59, 61) on (250, 250, 250) at 0.1 transparency, rounded 12 px,
    // padding 8 by 12. Widths are estimated at 8.5 px a character.
    constexpr float textSize = 16.0f, lineHeight = 19.0f, padX = 12.0f, padY = 8.0f, charWidth = 8.5f;
    struct Laid {
        const Bubble* bubble;
        float w, h;
    };
    std::vector<Laid> laid;
    float total = 0.0f;
    for (const Bubble& b : speaker.bubbles) {
        const float textWidth = static_cast<float>(b.text.size()) * charWidth;
        const float inner = maxWidth - 2.0f * padX;
        const float lines = std::max(1.0f, std::ceil(textWidth / inner));
        const float w = std::clamp(textWidth + 2.0f * padX, 32.0f, maxWidth);
        const float h = lines * lineHeight + 2.0f * padY;
        laid.push_back({&b, w, h});
        total += h;
    }
    total += spacing * static_cast<float>(laid.size() - 1);
    auto gui = std::make_shared<BillboardGui>();
    gui->setName("BubbleChat_" + std::string(adornee->name()));
    gui->setAdornee(adornee.get());
    gui->setAlwaysOnTop(true);
    gui->setMaxDistance(100.0f); // MaxDistance
    gui->setSize(box(0.0f, static_cast<int>(maxWidth), 0.0f, static_cast<int>(total)));
    gui->setSizeOffset(Vector2{0.0f, 0.5f});        // the bottom edge at the anchor
    gui->setStudsOffset(Vector3{0.0f, 1.5f, 0.0f}); // over the head
    float y = 0.0f;
    for (const Laid& l : laid) {
        auto frame = std::make_shared<Frame>();
        frame->setName("Bubble");
        frame->setBackgroundColor3(Color3::fromRGB(250, 250, 250));
        frame->setBackgroundTransparency(0.1f);
        frame->setBorderSizePixel(0);
        frame->setPosition(box(0.5f, static_cast<int>(-l.w / 2.0f), 0.0f, static_cast<int>(y)));
        frame->setSize(box(0.0f, static_cast<int>(l.w), 0.0f, static_cast<int>(l.h)));
        auto corner = std::make_shared<UICorner>();
        corner->setCornerRadius(UDim{0.0f, 12});
        corner->setParent(frame.get());
        auto label = std::make_shared<TextLabel>();
        label->setName("Text");
        label->setBackgroundTransparency(1.0f);
        label->setPosition(box(0.0f, static_cast<int>(padX), 0.0f, static_cast<int>(padY)));
        label->setSize(box(1.0f, static_cast<int>(-2.0f * padX), 1.0f, static_cast<int>(-2.0f * padY)));
        label->setText(l.bubble->text);
        label->setTextColor3(Color3::fromRGB(57, 59, 61));
        label->setTextSize(textSize);
        label->setTextWrapped(true);
        label->setFont(Enum::Font::GothamMedium);
        label->setParent(frame.get());
        frame->setParent(gui.get());
        y += l.h + spacing;
    }
    gui->setParent(folder());
    speaker.gui = gui;
}

struct TextChatSource::PresetState {
    std::shared_ptr<const lunar::assets::RobloxWeb> web;
    std::mutex mutex;
    std::vector<lunar::ChatPreset> choices;
    std::string error;
    bool loading = false;
    bool finished = false;
    std::jthread worker;
};

TextChatSource::TextChatSource(lunar::ScriptContext& client, std::shared_ptr<const lunar::assets::RobloxWeb> web)
    : client_(client), presets_(std::make_unique<PresetState>())
{
    presets_->web = web ? std::move(web) : std::make_shared<lunar::assets::RobloxWeb>();
    client_.setChatSink([this](const lunar::ChatLine& line) {
        if (line.userId != 0 && muted_.contains(line.userId))
            return;
        if (observe)
            observe(line);
        if (deliver)
            if (auto message = windowMessage(line))
                deliver(*message);
    });
}

TextChatSource::~TextChatSource()
{
    alive_.reset();
    client_.setChatSink(nullptr);
}

void TextChatSource::send(std::string_view text)
{
    auto report = [&](std::string message, bool error = true) {
        if (!deliver)
            return;
        lunar::ui::ChatMessage line;
        line.kind = error ? lunar::ui::ChatMessage::Kind::Error : lunar::ui::ChatMessage::Kind::System;
        line.text = std::move(message);
        deliver(line);
    };
    const auto split = [](std::string_view input) {
        const auto end = input.find_first_of(" \t");
        const auto rest = end == input.npos ? input.npos : input.find_first_not_of(" \t", end);
        return std::pair{input.substr(0, end), rest == input.npos ? std::string_view{} : input.substr(rest)};
    };
    const auto lower = [](std::string_view value) {
        std::string out(value);
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const auto [alias, arguments] = split(text);
    const auto* service = lunar::instanceCast<lunar::TextChatService>(client_.game().findService("TextChatService"));
    const auto commands = lunar::ui::chatCommands(service);
    const std::string typedAlias = lower(alias);
    const lunar::ui::ChatCommandInfo* match = nullptr;
    for (const auto& command : commands)
        if ((command.enabled || command.kind != lunar::ui::ChatCommandKind::Custom)
            && std::ranges::find(command.aliases, typedAlias) != command.aliases.end()) {
            match = &command;
            break;
        }
    // A retired or disabled private alias must never turn its draft public.
    if (!match && (typedAlias == "/w" || typedAlias == "/whisper")) {
        report("That whisper command is not available.");
        return;
    }
    using K = lunar::ui::ChatCommandKind;
    if (match && match->kind != K::Custom) {
        if (!match->enabled) {
            report("That chat command is disabled.");
            return;
        }
        if (match->instance && service) {
            const auto* players = lunar::instanceCast<lunar::Players>(client_.game().findService("Players"));
            const auto* local = players ? players->localPlayer() : nullptr;
            lunar::InstancePtr source;
            if (local)
                for (const auto& child : service->descendants())
                    if (const auto* participant = lunar::instanceCast<lunar::TextSource>(child.get());
                        participant && participant->userId() == local->userId()) {
                        source = child;
                        break;
                    }
            match->instance->fireEvent("Triggered", std::array<lunar::Value, 2>{source, std::string(text)});
        }
        const auto command = match->kind;
        if (command == K::Help) {
            std::string help = "Chat commands:";
            for (const auto& info : commands)
                if (info.enabled && info.visible && !info.aliases.empty())
                    help += " " + info.aliases.front();
            report(std::move(help), false);
            return;
        }
        if (command == K::Clear) {
            if (clearWindow)
                clearWindow();
            return;
        }
        if (command == K::Console) {
            auto& core = client_.game().getService<lunar::StarterGui>()->coreSettings();
            core.devConsoleVisible = !core.devConsoleVisible;
            return;
        }
        if (command == K::Version) {
#ifdef LUNAR_VERSION
            report("Lunarstone " LUNAR_VERSION " native chat", false);
#else
            report("Lunarstone native chat", false);
#endif
            return;
        }
        if (command == K::Emote) {
            client_.playChatEmote(std::string(arguments), [this, weak = std::weak_ptr<int>(alive_)](auto result) {
                if (weak.expired() || result || !deliver)
                    return;
                lunar::ui::ChatMessage error;
                error.kind = lunar::ui::ChatMessage::Kind::Error;
                error.text = result.error();
                deliver(error);
            });
            return;
        }
        if (command == K::Mute || command == K::Unmute) {
            const auto [name, unused] = split(arguments);
            const bool username = name.starts_with("@");
            const std::string wanted = lower(username ? name.substr(1) : name);
            const auto* players = client_.game().getService<lunar::Players>();
            std::vector<const lunar::Player*> matches;
            for (const auto& child : players->children())
                if (const auto* player = lunar::instanceCast<lunar::Player>(child.get()); player && !wanted.empty()) {
                    const std::string& label
                        = username || player->displayName().empty() ? player->name() : player->displayName();
                    if (lower(label) == wanted)
                        matches.push_back(player);
                }
            if (matches.size() != 1 || matches.front() == players->localPlayer()) {
                report(matches.size() > 1 ? "That name matches more than one player; use @username."
                                          : "Choose another player's display name or @username.");
                return;
            }
            const auto id = matches.front()->userId();
            if (command == K::Mute) {
                muted_.insert(id);
                if (removeUser)
                    removeUser(id);
            } else
                muted_.erase(id);
            report(std::format("{} {}.", command == K::Mute ? "Muted" : "Unmuted", name), false);
            return;
        }
        if (command == K::Channel || command == K::Team) {
            const bool team = command == K::Team;
            const auto [name, message] = team ? std::pair{std::string_view("Team"), arguments} : split(arguments);
            bool selected = false;
            for (const auto& channel : chatChannels())
                if (lower(channel.name) == lower(name) || lower(channel.label) == lower(name)) {
                    selected = selectChannel(channel.name);
                    break;
                }
            if (!selected) {
                report("That chat channel is not available.");
                return;
            }
            if (message.empty())
                return;
            text = message;
        } else if (command == K::Whisper) {
            const auto [name, message] = split(arguments);
            const auto* players = lunar::instanceCast<lunar::Players>(client_.game().findService("Players"));
            const bool username = name.starts_with("@");
            const std::string wanted = lower(username ? name.substr(1) : name);
            if (wanted.empty()) {
                report("Choose a display name or @username to whisper to.");
                return;
            }
            std::vector<const lunar::Player*> matches;
            if (players)
                for (const auto& child : players->children()) {
                    const auto* player = lunar::instanceCast<lunar::Player>(child.get());
                    if (!player || player == players->localPlayer())
                        continue;
                    const std::string label
                        = username || player->displayName().empty() ? player->name() : player->displayName();
                    if (lower(label).starts_with(wanted))
                        matches.push_back(player);
                }
            if (username && matches.size() > 1)
                for (const auto* player : matches)
                    if (lower(player->name()) == wanted) {
                        matches = {player};
                        break;
                    }
            // Keep exact usernames accepted by the existing client as well.
            if (matches.empty() && !username && players)
                for (const auto& child : players->children())
                    if (const auto* player = lunar::instanceCast<lunar::Player>(child.get());
                        player && player != players->localPlayer() && lower(player->name()) == wanted)
                        matches.push_back(player);
            if (matches.size() != 1) {
                report(matches.empty() ? "That player is not available for a whisper."
                                       : "That name matches more than one player; use @username.");
                return;
            }
            const auto generation = ++whisperGeneration_;
            client_.requestWhisperChannel(
                matches.front()->userId(),
                [this, weak = std::weak_ptr<int>(alive_), generation,
                 body = std::string(message)](std::expected<std::string, std::string> channel) {
                    if (weak.expired())
                        return;
                    auto failed = [&](std::string reason) {
                        if (!deliver)
                            return;
                        lunar::ui::ChatMessage error;
                        error.kind = lunar::ui::ChatMessage::Kind::Error;
                        error.text = std::move(reason);
                        deliver(error);
                    };
                    if (!channel) {
                        failed(channel.error());
                        return;
                    }
                    if (generation == whisperGeneration_ && !selectChannel(*channel)) {
                        failed("The whisper channel is no longer available.");
                        return;
                    }
                    if (!body.empty())
                        if (auto sent = client_.sendChat(body, *channel); !sent)
                            failed("Your whisper could not be sent: " + sent.error());
                });
            return;
        }
    }
    if (auto sent = client_.sendChat(text, selectedChannel()); !sent && deliver) {
        lunar::ui::ChatMessage error;
        error.kind = lunar::ui::ChatMessage::Kind::Error;
        error.text = std::format("Your message could not be sent: {}.", sent.error());
        deliver(error);
    }
}

std::vector<lunar::ui::ChatChannelInfo> TextChatSource::chatChannels() const
{
    std::vector<lunar::ui::ChatChannelInfo> found;
    const lunar::Instance* service = client_.game().findService("TextChatService");
    const lunar::Instance* folder = service ? service->findFirstChild("TextChannels") : nullptr;
    if (!folder)
        return found;
    const auto* players = lunar::instanceCast<lunar::Players>(client_.game().findService("Players"));
    const lunar::Player* local = players ? players->localPlayer() : nullptr;
    for (const auto& instance : folder->children()) {
        const auto* channel = lunar::instanceCast<lunar::TextChannel>(instance.get());
        if (!channel || channel->name() == "RBXSystem")
            continue;
        bool canSend = false;
        bool member = false;
        for (const auto& child : channel->children())
            if (const auto* source = lunar::instanceCast<lunar::TextSource>(child.get());
                source && local && source->userId() == local->userId()) {
                member = true;
                canSend = source->canSend();
            }
        if (!member)
            continue;
        const std::string& name = channel->name();
        std::string label = name == "RBXGeneral" ? "General" : name;
        if (name.starts_with("RBXTeam"))
            label = "Team";
        else if (name.starts_with("RBXWhisper")) {
            label = "Whisper";
            if (players && local)
                for (const auto& child : channel->children())
                    if (const auto* source = lunar::instanceCast<lunar::TextSource>(child.get());
                        source && source->userId() != local->userId())
                        for (const auto& person : players->children())
                            if (const auto* peer = lunar::instanceCast<lunar::Player>(person.get());
                                peer && peer->userId() == source->userId())
                                label = "To " + (peer->displayName().empty() ? peer->name() : peer->displayName());
        }
        found.push_back({name, std::move(label), canSend});
    }
    std::ranges::sort(found, {},
                      [](const auto& channel) { return channel.name == "RBXGeneral" ? std::string() : channel.name; });
    return found;
}

std::string TextChatSource::selectedChannel() const
{
    const lunar::Instance* service = client_.game().findService("TextChatService");
    const auto* input = service ? lunar::instanceCast<lunar::ChatInputBarConfiguration>(
                                      service->findFirstChildOfClass("ChatInputBarConfiguration"))
                                : nullptr;
    if (input) {
        const lunar::Instance* target = input->targetTextChannel();
        return target ? target->name() : "RBXGeneral";
    }
    return selectedChannel_.empty() ? "RBXGeneral" : selectedChannel_;
}

bool TextChatSource::selectChannel(std::string_view name)
{
    const auto channels = chatChannels();
    if (std::ranges::none_of(channels, [&](const auto& info) { return info.name == name; }))
        return false;
    lunar::Instance* service = client_.game().findService("TextChatService");
    auto* input = service ? lunar::instanceCast<lunar::ChatInputBarConfiguration>(
                                service->findFirstChildOfClass("ChatInputBarConfiguration"))
                          : nullptr;
    if (input)
        input->setTargetTextChannel(service->findFirstChild("TextChannels")->findFirstChild(name));
    selectedChannel_ = name;
    return true;
}

bool TextChatSource::hasPresets() const
{
    const auto access = client_.presetChatAccess();
    return access.enabled && access.access == "Enabled";
}

void TextChatSource::requestPresets()
{
    if (!hasPresets())
        return;
    auto& state = *presets_;
    {
        const std::scoped_lock lock(state.mutex);
        if (state.loading || !state.choices.empty())
            return;
        state.loading = true;
        state.finished = false;
        state.error.clear();
    }
    const std::string version = presetCatalogVersion(client_.presetChatAccess());
    const auto* localization
        = lunar::instanceCast<lunar::LocalizationService>(client_.game().findService("LocalizationService"));
    const std::string locale
        = localization && !localization->robloxLocaleId().empty() ? localization->robloxLocaleId() : "en-us";
    state.worker = std::jthread([&state, version, locale](std::stop_token stop) {
        const lunar::assets::HttpStopScope cancellation(stop);
        auto catalog = fetchChatPresets(*state.web, version, locale);
        const std::scoped_lock lock(state.mutex);
        if (catalog)
            state.choices = std::move(*catalog);
        else
            state.error = catalog.error();
        state.loading = false;
        state.finished = true;
    });
}

bool TextChatSource::presetsLoading() const
{
    const std::scoped_lock lock(presets_->mutex);
    return presets_->loading;
}

std::vector<std::string> TextChatSource::presetChoices() const
{
    const std::scoped_lock lock(presets_->mutex);
    std::vector<std::string> choices;
    for (const auto& preset : presets_->choices)
        choices.push_back(preset.text);
    return choices;
}

void TextChatSource::sendPreset(std::size_t index)
{
    lunar::ChatPreset preset;
    {
        const std::scoped_lock lock(presets_->mutex);
        if (index >= presets_->choices.size())
            return;
        preset = presets_->choices[index];
    }
    if (auto sent = client_.sendPresetChat(preset); !sent && deliver) {
        lunar::ui::ChatMessage error;
        error.kind = lunar::ui::ChatMessage::Kind::Error;
        error.text = "Your preset could not be sent: " + sent.error();
        deliver(error);
    }
}

void TextChatSource::poll()
{
    std::string error;
    {
        const std::scoped_lock lock(presets_->mutex);
        if (!presets_->finished)
            return;
        presets_->finished = false;
        error = presets_->error;
    }
    if (!error.empty() && deliver) {
        lunar::ui::ChatMessage message;
        message.kind = lunar::ui::ChatMessage::Kind::Error;
        message.text = "Preset chat unavailable: " + error;
        deliver(message);
    }
}
