#pragma once

// Chat on our client of a game (--net-join --scripts, --net-replay --run, a
// game window): the messages the client's TextChatService shows, as join-log
// lines and as the chat window's lines, and what the local player types going
// out the way the core chat sends it (ScriptContext::sendChat).
//
// A window wires it up like this (Play Solo has DevChatSource instead):
//
//     TextChatSource chat(session.client());   // the ClientSession's context
//     ui.setChatSource(&chat);                  // typed lines go out, messages come in
//     chat.observe = [&](const lunar::ChatLine& line) { ... bubbles, logs ... };

#include <lunar/script/ScriptContext.hpp>
#include <lunar/ui/ClassicChat.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::assets {
class RobloxWeb;
}

// "chat: Name: text" for the join log, with what the line is when it isn't a
// plain delivered message ("(Sending)", "(hidden ...)", "[RBXTeam...]").
std::string describeChatLine(const lunar::ChatLine& line);

// Chat-window presentation; bubble-only lines are rendered over the speaker.
// PrefixText and body rich text are preserved for the native window.
std::optional<lunar::ui::ChatMessage> windowMessage(const lunar::ChatLine& line);

// Preset chat's catalog, which an account without chat (no age check) picks
// its phrases from: the version the server's config names (customVersion,
// else defaultVersion; GUESS: that order), and the phrases of a version in a
// locale, asked with the login like the client's PresetChatManager does
// (GET apis.roblox.com/preset-chat/v1/preset-chat-options?version=&locale=).
std::string presetCatalogVersion(const lunar::PresetChatAccess& access);
std::expected<std::vector<lunar::ChatPreset>, std::string>
fetchChatPresets(const lunar::assets::RobloxWeb& web, std::string_view version, std::string_view locale = "en-us");

// A phrase of that catalog said in the chat (--say-preset "Good luck"): what
// was sent, or why not (no preset chat here yet, no such phrase).
std::expected<std::string, std::string> sayPresetPhrase(lunar::ScriptContext& client,
                                                        const lunar::assets::RobloxWeb& web, std::string_view phrase);

// Bubble chat as the core scripts draw it: a BillboardGui over the
// speaker's head (in CoreGui, which the game window and --render draw)
// holding their last few messages as rounded bubbles, each gone after
// BubbleChatConfiguration.BubbleDuration. Feed it the chat sink's lines and
// the time; hidden messages show their dots. GUESS: the look (the API's
// BubbleChatConfiguration defaults, AlwaysOnTop, the text width estimate).
class ChatBubbles {
public:
    explicit ChatBubbles(std::shared_ptr<lunar::DataModel> game);
    ~ChatBubbles();
    ChatBubbles(const ChatBubbles&) = delete;
    ChatBubbles& operator=(const ChatBubbles&) = delete;

    void add(const lunar::ChatLine& line, double now);
    // Drops bubbles past their time.
    void update(double now);
    // Bubbles showing now, over every head.
    std::size_t count() const;

    static constexpr double duration = 15.0;     // BubbleChatConfiguration.BubbleDuration
    static constexpr std::size_t maxBubbles = 3; // MaxBubbles
    static constexpr float maxWidth = 300.0f;    // pixels a bubble may grow to before wrapping (GUESS)
    static constexpr float spacing = 6.0f;       // BubblesSpacing

private:
    struct Bubble {
        std::string text;
        double shownAt = 0.0;
    };
    struct Speaker {
        std::weak_ptr<lunar::Instance> adornee;
        std::weak_ptr<lunar::Instance> gui; // the BillboardGui
        std::vector<Bubble> bubbles;        // oldest first
    };
    void rebuild(Speaker& speaker);
    lunar::Instance* folder();

    std::shared_ptr<lunar::DataModel> game_;
    std::weak_ptr<lunar::Instance> folder_;
    std::map<const lunar::Instance*, Speaker> speakers_; // by adornee
};

// The chat window's backend over a client's TextChatService. Takes over the
// context's chat sink while it lives.
class TextChatSource final : public lunar::ui::ChatSource {
public:
    explicit TextChatSource(lunar::ScriptContext& client,
                            std::shared_ptr<const lunar::assets::RobloxWeb> web = nullptr);
    ~TextChatSource() override;
    TextChatSource(const TextChatSource&) = delete;
    TextChatSource& operator=(const TextChatSource&) = delete;

    // A line typed into the chat bar; why it couldn't go shows as an error line.
    void send(std::string_view text) override;
    std::vector<lunar::ui::ChatChannelInfo> chatChannels() const override;
    std::string selectedChannel() const override;
    bool selectChannel(std::string_view channel) override;
    bool hasPresets() const override;
    void requestPresets() override;
    bool presetsLoading() const override;
    std::vector<std::string> presetChoices() const override;
    void sendPreset(std::size_t index) override;
    void poll() override;

    // Every line as it arrives, before the window gets it: bubbles, logs.
    std::function<void(const lunar::ChatLine&)> observe;

private:
    lunar::ScriptContext& client_;
    std::string selectedChannel_;
    struct PresetState;
    std::unique_ptr<PresetState> presets_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::uint64_t whisperGeneration_ = 0;
    std::set<std::int64_t> muted_;
};
