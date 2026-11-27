#pragma once

#include <lunar/Math.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/TextEditor.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {
class DataModel;
class TextBox;
class ScreenGui;
} // namespace lunar

namespace lunar::ui {

// The name colour of the legacy Lua chat (ExtraDataInitializer's
// ComputeNameColor): a signed sum of the name's bytes picks one of eight
// colours. Takes the player's Name (not DisplayName), like the original.
Color3 legacyNameColor(std::string_view name);

struct ChatMessage {
    enum class Kind : std::uint8_t {
        Player, // "[DisplayName]: text"
        System, // plain text
        Error,  // plain text in the error colour
    };
    Kind kind = Kind::Player;
    std::string fromName;    // Player.Name: picks the default name colour
    std::string displayName; // shown in the brackets
    std::string text;
    std::optional<Color3> nameColor; // team colour and the like; else legacyNameColor(fromName)
    std::optional<Color3> textColor;
    std::string prefix; // TextChatService PrefixText, including rich text
    bool richText = false;
    std::string messageId; // Sending/Success update the same visible message
    std::string channel;
    bool pending = false;
    std::int64_t userId = 0;
};

// Where chat messages come from and where typed ones go. One per chat
// backend: the legacy chat events, TextChatService (TextChannel
// MessageReceived / SendAsync), or offline echo. The window calls send()
// for what the local player types and shows what arrives through deliver().
struct ChatChannelInfo {
    std::string name;
    std::string label;
    bool canSend = true;
};

class ChatSource {
public:
    virtual ~ChatSource() = default;
    virtual void send(std::string_view text) = 0;
    // Called when a tab is opened, and each frame on the UI thread. A network
    // source can connect lazily and deliver queued messages from poll().
    virtual void activate() {}
    virtual void poll() {}
    virtual std::vector<ChatChannelInfo> chatChannels() const { return {}; }
    virtual std::string selectedChannel() const { return {}; }
    virtual bool selectChannel(std::string_view) { return false; }
    virtual bool hasPresets() const { return false; }
    virtual void requestPresets() {}
    virtual bool presetsLoading() const { return false; }
    virtual std::vector<std::string> presetChoices() const { return {}; }
    virtual void sendPreset(std::size_t) {}

    std::function<void(const ChatMessage&)> deliver; // set by the chat window
    std::function<void()> clearWindow;
    std::function<void(std::int64_t)> removeUser;
};

// Offline: every message typed comes straight back as the local player's.
class LocalEchoChatSource final : public ChatSource {
public:
    LocalEchoChatSource(std::string name, std::string displayName)
        : name_(std::move(name)), displayName_(std::move(displayName))
    {
    }
    void send(std::string_view text) override;

private:
    std::string name_;
    std::string displayName_;
};

// The pre-2023 Roblox chat window: a translucent black box in the top-left
// corner under the top bar with "[Name]: message" lines and a chat bar
// below, "/" to type, Enter to send. Drawn natively from the Lua chat's
// settings (ChatSettings.lua, ChatWindow.lua, ChatBar.lua and
// MessageLogDisplay.lua of Roblox's open-source CoreScripts, from memory;
// constants are commented where they come from and marked when guessed).
class ClassicChat {
public:
    enum class Tab : std::uint8_t { Game, Rtc };

    // ChatSettings.lua.
    static constexpr float textSize = 18.0f;                 // ChatWindowTextSize, ChatBarTextSize
    static constexpr float backgroundTransparency = 0.6f;    // ChatChannelParentFrame, ChatBar frames
    static constexpr double backgroundFadeOutTime = 3.5;     // ChatWindowBackgroundFadeOutTime
    static constexpr double textFadeOutTime = 30.0;          // ChatWindowTextFadeOutTime
    static constexpr double fadeDuration = 0.8;              // ChatDefaultFadeDuration
    static constexpr std::size_t maximumMessageLength = 200; // MaximumMessageLength
    static constexpr std::size_t historyLength = 50;         // MessageHistoryLengthPerChannel
    static constexpr float windowWidthScale = 0.3f;          // DefaultWindowSizeDesktop = UDim2(0.3, 0, 0.25, 24)
    static constexpr float windowHeightScale = 0.25f;
    static constexpr float windowHeightOffset = 7 * 2 + 5 * 2;         // extraOffset: chat bar box and text padding
    static constexpr float boxPadding = 7.0f;                          // ChatBar BoxFrame inset
    static constexpr float textPadding = 5.0f;                         // ChatBar TextBox holder inset
    static constexpr float messageBorder = 8.0f;                       // CreateBaseMessage: label at x = 8, width - 14
    static constexpr float scrollBarThickness = 4.0f;                  // MessageLogDisplay Scroller
    static constexpr float gap = 2.0f;                                 // between the log and the chat bar (guess)
    static constexpr Color3 errorColor = Color3::fromRGB(245, 50, 50); // ErrorMessageTextColor

    ClassicChat(FontLibrary& fonts, GlyphAtlas& atlas);
    ~ClassicChat();
    // GameUi uses the modern TextChatService appearance. Standalone users
    // without a game retain the classic appearance and geometry.
    void setGame(DataModel* game);
    bool modern() const { return modern_; }
    RectF sendRect() const { return send_; }
    RectF presetsRect() const { return presetsButton_; }
    RectF presetChoiceRect(std::size_t index) const;
    RectF completionRect(std::size_t index) const;
    const std::vector<std::string>& completions() const { return completions_; }
    RectF channelRect(std::string_view name) const;

    // The backend; null to disconnect. Not owned.
    void setSource(ChatSource* source);
    // An RTC source adds a second tab. Game messages and the existing post()
    // calls always go to Game, even while RTC is selected.
    void setSource(Tab tab, ChatSource* source);
    bool hasRtcTab() const { return channel(Tab::Rtc).source != nullptr; }
    Tab selectedTab() const { return selected_; }
    void selectTab(Tab tab);
    // Adds a message as if it arrived from the source.
    void post(ChatMessage message);
    void post(Tab tab, ChatMessage message);
    void postSystem(std::string text, std::optional<Color3> color = std::nullopt);

    // Input in window coordinates; true when the chat used the event. "/"
    // focuses the chat bar when nothing else has keyboard focus.
    bool handle(const platform::Event& event);
    bool focused() const { return focused_; }
    bool pointerOver() const { return visible && hovered_; }
    void focus();
    void releaseFocus(bool enterPressed = false);
    bool ownsTextBox(const TextBox* box) const { return nativeInput_.get() == box; }

    // screen: framebuffer pixels; topInset and scale as GuiEngine's settings.
    void update(double time, Vector2 screen, float topInset, float pixelScale);
    void draw(DrawList& list);

    bool visible = true;
    const std::deque<ChatMessage>& messages(Tab tab = Tab::Game) const { return channel(tab).messages; }
    const std::string& draft() const { return channel(selected_).editor.text; }
    std::size_t unread(Tab tab) const { return channel(tab).unread; }

    // Laid-out geometry, for tests and hit testing (framebuffer pixels).
    RectF windowRect() const { return window_; }
    RectF logRect() const { return log_; }
    RectF barRect() const { return bar_; }
    RectF tabRect(Tab tab) const;

private:
    struct Line {
        ChatMessage message;
        double time = 0.0;
        TextLayout name;
        TextLayout body;
        float height = 0.0f;
        float laidOutWidth = -1.0f;
    };
    struct Channel {
        ChatSource* source = nullptr;
        std::deque<ChatMessage> messages;
        std::deque<Line> lines;
        TextEditor editor;
        double lastMessage = -1e9;
        float scroll = 0.0f;
        bool stickToBottom = true;
        float contentHeight = 0.0f;
        std::size_t unread = 0;
        float editorScroll = 0.0f;
    };
    Channel& channel(Tab tab) { return channels_[static_cast<std::size_t>(tab)]; }
    const Channel& channel(Tab tab) const { return channels_[static_cast<std::size_t>(tab)]; }
    void layoutLine(Line& line, float width);
    float spaceWidth(const lunar::Font& font, float size) const;
    float alphaAt(double since, double fadeOutAfter) const;
    void submit();
    void updateModern(Vector2 screen, float topInset);
    void drawModern(DrawList& list);
    void syncFocus(bool enterPressed = false);
    bool shows(const ChatMessage& message) const;
    void complete(std::size_t index);

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    std::array<Channel, 2> channels_;
    Tab selected_ = Tab::Game;
    bool focused_ = false;
    bool hovered_ = false;
    double time_ = 0.0;
    double lastActivity_ = 0.0; // hover, focus or typing keeps the background shown; shown at the start
    double caretTime_ = 0.0;
    float scale_ = 1.0f;
    RectF window_, tabs_, log_, bar_;
    RectF send_;
    RectF presetsButton_;
    RectF presetsMenu_;
    std::vector<std::string> presets_;
    std::size_t presetOffset_ = 0;
    bool presetsOpen_ = false;
    std::optional<std::size_t> presetPressed_;
    std::vector<std::string> completions_;
    RectF completionsMenu_;
    std::size_t completionSelected_ = 0;
    std::size_t completionOffset_ = 0;
    std::optional<std::size_t> completionPressed_;
    std::string completionPrefix_;
    std::string completionDismissed_;
    RectF channelTabs_;
    std::vector<ChatChannelInfo> chatChannels_;
    std::string selectedChatChannel_;
    bool sendPressed_ = false;
    bool modern_ = false;
    bool inputEnabled_ = true;
    Enum::KeyCode focusKey_ = Enum::KeyCode::Slash;
    bool skipFocusText_ = false;
    bool windowEnabled_ = true;
    std::weak_ptr<DataModel> game_;
    std::shared_ptr<ScreenGui> nativeInputRoot_;
    std::shared_ptr<TextBox> nativeInput_;
    TextStyle modernText_;
    TextStyle modernInput_;
    TextStyle modernChannel_;
    Color3 modernInputBackground_;
    float modernInputTransparency_ = 0.3f;
    Color3 modernChannelBackground_;
    float modernChannelTransparency_ = 0.3f;
    Color3 modernChannelSelected_;
    Color3 modernChannelHover_;
    Color3 modernBackground_{25.0f / 255, 27.0f / 255, 29.0f / 255};
    float modernTransparency_ = 0.3f;
    Color3 modernPlaceholder_{0.7f, 0.7f, 0.7f};
    float mouseX_ = 0.0f, mouseY_ = 0.0f;
};

} // namespace lunar::ui
