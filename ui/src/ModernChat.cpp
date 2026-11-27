#include <lunar/ui/ChatCommands.hpp>
#include <lunar/ui/ClassicChat.hpp>

#include <lunar/Classes.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace lunar::ui {
namespace {

void panel(DrawList& list, RectF rect, Color4 color, float radius)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    Quad quad;
    quad.rect = rect;
    quad.color = color;
    quad.radii.fill(radius);
    list.push(quad);
}

// The tabs, log and input bar stack into one card: a band rounds only the
// card's own corners.
void section(DrawList& list, RectF rect, RectF card, Color4 color, float radius)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    const float top = rect.y <= card.y + 0.5f ? radius : 0;
    const float bottom = rect.bottom() >= card.bottom() - 0.5f ? radius : 0;
    Quad quad;
    quad.rect = rect;
    quad.color = color;
    quad.radii = {top, top, bottom, bottom};
    list.push(quad);
}

TextStyle styleOf(const TextChatConfigurations* config)
{
    TextStyle style;
    style.font = config ? config->fontFace() : fontFromEnum(Enum::Font::BuilderSans);
    style.size = config ? std::clamp(static_cast<float>(config->textSize()), 1.0f, 100.0f) : 16.0f;
    style.color = config ? config->textColor3() : Color3{1, 1, 1};
    style.strokeColor = config ? config->textStrokeColor3() : Color3{};
    style.strokeTransparency = config ? static_cast<float>(config->textStrokeTransparency()) : 1.0f;
    return style;
}

} // namespace

void ClassicChat::setGame(DataModel* game)
{
    releaseFocus();
    if (nativeInputRoot_)
        nativeInputRoot_->destroy();
    nativeInputRoot_.reset();
    nativeInput_.reset();
    game_ = game ? std::static_pointer_cast<DataModel>(game->shared_from_this()) : nullptr;
    modern_ = game != nullptr;
    for (Channel& state : channels_)
        for (Line& line : state.lines)
            line.laidOutWidth = -1;
}

ClassicChat::~ClassicChat()
{
    releaseFocus();
    if (nativeInputRoot_)
        nativeInputRoot_->destroy();
}

void ClassicChat::syncFocus(bool enterPressed)
{
    if (const auto game = game_.lock()) {
        const bool gameFocus = focused_ && selected_ == Tab::Game;
        auto* inputService = game->getService<UserInputService>();
        if (gameFocus && !nativeInput_) {
            // The native renderer draws the bar. This hidden core container
            // supplies the TextBox identity used by UserInputService.
            nativeInputRoot_ = std::make_shared<ScreenGui>();
            nativeInputRoot_->setName("LunarstoneChatFocus");
            nativeInputRoot_->setEnabled(false);
            nativeInputRoot_->setArchivable(false);
            nativeInputRoot_->setParent(game->getService("CoreGui"));
            nativeInput_ = std::make_shared<TextBox>();
            nativeInput_->setName("ChatInputBar");
            nativeInput_->setClearTextOnFocus(false);
            nativeInput_->setParent(nativeInputRoot_);
        }
        if (gameFocus && nativeInput_) {
            auto& editor = channel(Tab::Game).editor;
            nativeInput_->setText(editor.text);
            nativeInput_->setCursorPosition(static_cast<int>(editor.caret) + 1);
            if (inputService->focusedTextBox() != nativeInput_.get()) {
                if (auto* previous = inputService->focusedTextBox()) {
                    previous->setCursorPosition(-1);
                    previous->fireFocusLost(false);
                }
                nativeInput_->fireFocused();
            }
        } else if (nativeInput_ && inputService->focusedTextBox() == nativeInput_.get()) {
            nativeInput_->setCursorPosition(-1);
            nativeInput_->fireFocusLost(enterPressed);
        }
        if (Instance* service = game->findService("TextChatService"))
            if (auto* config
                = instanceCast<ChatInputBarConfiguration>(service->findFirstChildOfClass("ChatInputBarConfiguration")))
                config->assignFocused(focused_ && selected_ == Tab::Game);
    }
}

RectF ClassicChat::channelRect(std::string_view name) const
{
    if (channelTabs_.h <= 0 || chatChannels_.empty())
        return {};
    for (std::size_t i = 0; i < chatChannels_.size(); ++i)
        if (chatChannels_[i].name == name) {
            const float width = channelTabs_.w / static_cast<float>(chatChannels_.size());
            return {channelTabs_.x + static_cast<float>(i) * width, channelTabs_.y, width, channelTabs_.h};
        }
    return {};
}

RectF ClassicChat::presetChoiceRect(std::size_t index) const
{
    if (!presetsOpen_ || index < presetOffset_ || index >= presets_.size())
        return {};
    const float y = presetsMenu_.y + static_cast<float>(index - presetOffset_) * 32 * scale_;
    if (y + 32 * scale_ > presetsMenu_.bottom() + 0.1f)
        return {};
    return {presetsMenu_.x, y, presetsMenu_.w, 32 * scale_};
}

RectF ClassicChat::completionRect(std::size_t index) const
{
    if (index >= completions_.size() || index < completionOffset_)
        return {};
    const float y = completionsMenu_.y + static_cast<float>(index - completionOffset_) * 32 * scale_;
    if (y + 32 * scale_ > completionsMenu_.bottom() + 0.1f)
        return {};
    return {completionsMenu_.x, y, completionsMenu_.w, 32 * scale_};
}

void ClassicChat::complete(std::size_t index)
{
    if (index >= completions_.size())
        return;
    channel(selected_).editor.setText(completions_[index]);
    completionDismissed_ = channel(selected_).editor.text;
    completionPrefix_ = completionDismissed_;
    completions_.clear();
    completionPressed_.reset();
    syncFocus();
}

bool ClassicChat::shows(const ChatMessage& message) const
{
    return selected_ == Tab::Rtc || selectedChatChannel_.empty() || message.channel.empty()
           || message.channel == "RBXSystem" || message.channel == selectedChatChannel_;
}

void ClassicChat::updateModern(Vector2 screen, float topInset)
{
    const float k = scale_;
    const auto game = game_.lock();
    Instance* service = game ? game->findService("TextChatService") : nullptr;
    auto* window
        = service ? instanceCast<ChatWindowConfiguration>(service->findFirstChildOfClass("ChatWindowConfiguration"))
                  : nullptr;
    auto* input
        = service ? instanceCast<ChatInputBarConfiguration>(service->findFirstChildOfClass("ChatInputBarConfiguration"))
                  : nullptr;
    const auto* starter = game ? instanceCast<StarterGui>(game->findService("StarterGui")) : nullptr;
    const auto* core = starter ? &starter->coreSettings() : nullptr;
    const auto* channelConfig
        = service ? instanceCast<ChannelTabsConfiguration>(service->findFirstChildOfClass("ChannelTabsConfiguration"))
                  : nullptr;
    if (ChatSource* source = channel(Tab::Game).source) {
        chatChannels_ = source->chatChannels();
        selectedChatChannel_ = source->selectedChannel();
    } else {
        chatChannels_.clear();
        selectedChatChannel_.clear();
    }
    const TextStyle text = styleOf(window);
    if (modernText_ != text)
        for (Channel& state : channels_)
            for (Line& line : state.lines)
                line.laidOutWidth = -1;
    modernText_ = text;
    modernInput_ = styleOf(input);
    modernInputBackground_ = input ? input->backgroundColor3() : Color3::fromRGB(25, 27, 29);
    modernInputTransparency_ = input ? static_cast<float>(input->backgroundTransparency()) : 0.3f;
    modernChannel_ = styleOf(channelConfig);
    modernChannelBackground_ = channelConfig ? channelConfig->backgroundColor3() : Color3::fromRGB(25, 27, 29);
    modernChannelTransparency_ = channelConfig ? static_cast<float>(channelConfig->backgroundTransparency()) : 0.3f;
    modernChannelSelected_ = channelConfig ? channelConfig->selectedTabTextColor3() : Color3{1, 1, 1};
    modernChannelHover_ = channelConfig ? channelConfig->hoverBackgroundColor3() : Color3::fromRGB(125, 125, 125);
    modernBackground_ = window ? window->backgroundColor3() : Color3::fromRGB(25, 27, 29);
    modernTransparency_ = window ? static_cast<float>(window->backgroundTransparency()) : 0.3f;
    modernPlaceholder_ = input ? input->placeholderColor3() : Color3::fromRGB(178, 178, 178);
    focusKey_ = input ? input->keyboardKeyCode() : Enum::KeyCode::Slash;
    inputEnabled_ = selected_ == Tab::Rtc || ((!input || input->enabled()) && (!core || !core->chatBarDisabled));
    windowEnabled_ = selected_ == Tab::Rtc || !window || window->enabled();
    if (!inputEnabled_ && focused_)
        releaseFocus();
    if (input && input->takeFocusRequest() && selected_ == Tab::Game && inputEnabled_)
        focus();

    const float margin = 8 * k;
    const float top = topInset * k;
    const float roomW = std::max(screen.x - margin * 2, 0.0f);
    const float roomH = std::max(screen.y - top - margin * 2, 0.0f);
    float width = std::clamp(screen.x * 0.3f, std::min(280 * k, roomW), std::min(480 * k, roomW));
    float height = (screen.y - top) * 0.25f + 24 * k;
    if (window) {
        width *= window->widthScale();
        height *= window->heightScale();
    }
    if (core && core->chatWindowSize) {
        const UDim2 size = *core->chatWindowSize;
        width = size.x.scale * screen.x + static_cast<float>(size.x.offset) * k;
        height = size.y.scale * (screen.y - top) + static_cast<float>(size.y.offset) * k;
    }
    width = std::clamp(width, 0.0f, roomW);
    height = std::clamp(height, 0.0f, roomH);
    float x = margin, y = top + margin;
    if (window) {
        if (window->horizontalAlignment() == Enum::HorizontalAlignment::Right)
            x = screen.x - margin - width;
        else if (window->horizontalAlignment() == Enum::HorizontalAlignment::Center)
            x = (screen.x - width) / 2;
        if (window->verticalAlignment() == Enum::VerticalAlignment::Bottom)
            y = screen.y - margin - height;
        else if (window->verticalAlignment() == Enum::VerticalAlignment::Center)
            y = top + (screen.y - top - height) / 2;
    }
    if (core && core->chatWindowPosition) {
        const UDim2 position = *core->chatWindowPosition;
        x = position.x.scale * screen.x + static_cast<float>(position.x.offset) * k;
        y = top + position.y.scale * (screen.y - top) + static_cast<float>(position.y.offset) * k;
    }
    const float tabsHeight = hasRtcTab() ? 28 * k : 0;
    const float channelsHeight
        = selected_ == Tab::Game && chatChannels_.size() > 1 && (!channelConfig || channelConfig->enabled()) ? 28 * k
                                                                                                             : 0;
    const float barHeight = inputEnabled_ ? 40 * k : 0;
    const float messagesHeight
        = windowEnabled_ ? std::max(height - tabsHeight - channelsHeight - barHeight, 0.0f) : 0;
    window_ = {x, y, width, tabsHeight + channelsHeight + messagesHeight + barHeight};
    tabs_ = {x, y, width, tabsHeight};
    channelTabs_ = {x, y + tabsHeight, width, channelsHeight};
    log_ = {x, y + tabsHeight + channelsHeight, width, messagesHeight};
    bar_ = {x, log_.bottom(), width, barHeight};
    send_ = inputEnabled_ ? RectF{bar_.right() - 32 * k, bar_.y + 8 * k, 24 * k, 24 * k} : RectF{};
    ChatSource* source = channel(selected_).source;
    const bool hasPresets = selected_ == Tab::Game && source && source->hasPresets() && inputEnabled_;
    presetsButton_ = hasPresets ? RectF{bar_.x + 8 * k, bar_.y + 8 * k, 24 * k, 24 * k} : RectF{};
    if (!hasPresets)
        presetsOpen_ = false;
    presets_ = hasPresets ? source->presetChoices() : std::vector<std::string>{};
    const auto rows = static_cast<std::size_t>(std::clamp(std::floor(log_.h / (32 * k)), 1.0f, 7.0f));
    presetOffset_ = std::min(presetOffset_, presets_.size() > rows ? presets_.size() - rows : 0);
    presetsMenu_
        = {bar_.x, bar_.y - static_cast<float>(rows) * 32 * k - 4 * k, bar_.w, static_cast<float>(rows) * 32 * k};
    if (window)
        window->assignChatGeometry({log_.x / k, log_.y / k}, {log_.w / k, log_.h / k});
    if (input)
        input->assignChatGeometry({bar_.x / k, bar_.y / k}, {bar_.w / k, bar_.h / k});

    Channel& state = channel(selected_);
    const std::string prefix = state.editor.text;
    if (completionPrefix_ != prefix) {
        completionSelected_ = completionOffset_ = 0;
        completionDismissed_.clear();
        completionPrefix_ = prefix;
    }
    completions_.clear();
    if (focused_ && selected_ == Tab::Game && !presetsOpen_ && inputEnabled_ && (!input || input->autocompleteEnabled())
        && state.editor.caret == prefix.size() && !state.editor.hasSelection() && prefix.starts_with("/")
        && prefix != completionDismissed_) {
        const auto lower = [](std::string value) {
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        };
        const std::string wanted = lower(prefix);
        auto add = [&](std::string value) {
            if (lower(value).starts_with(wanted) && value != prefix)
                completions_.push_back(std::move(value));
        };
        const auto commands = chatCommands(instanceCast<TextChatService>(service));
        const auto end = wanted.find(' ');
        ChatCommandKind argumentKind = ChatCommandKind::Custom;
        for (const auto& command : commands) {
            if (!command.enabled || !command.visible)
                continue;
            for (const auto& alias : command.aliases) {
                add(alias + " ");
                if (end != wanted.npos && alias == wanted.substr(0, end))
                    argumentKind = command.kind;
            }
        }
        if (end != wanted.npos) {
            if ((argumentKind == ChatCommandKind::Whisper || argumentKind == ChatCommandKind::Mute
                 || argumentKind == ChatCommandKind::Unmute)
                && wanted.find(' ', end + 1) == wanted.npos && game) {
                if (auto* players = instanceCast<Players>(game->findService("Players")))
                    for (const auto& child : players->children())
                        if (const auto* player = instanceCast<Player>(child.get());
                            player && player != players->localPlayer()) {
                            std::string needle = wanted.substr(end + 1);
                            const bool username = needle.starts_with("@");
                            if (username)
                                needle.erase(needle.begin());
                            const std::string label
                                = username || player->displayName().empty() ? player->name() : player->displayName();
                            if (lower(label).starts_with(needle)
                                || (!username && lower(player->name()).starts_with(needle)))
                                completions_.push_back(prefix.substr(0, end + 1) + "@" + player->name() + " ");
                        }
            } else if (argumentKind == ChatCommandKind::Channel) {
                for (const auto& info : chatChannels_)
                    add(prefix.substr(0, end + 1) + info.label + " ");
            }
        }
        std::ranges::sort(completions_);
        completions_.erase(std::unique(completions_.begin(), completions_.end()), completions_.end());
        if (completions_.size() > 20)
            completions_.resize(20);
    }
    completionSelected_ = std::min(completionSelected_, completions_.empty() ? 0 : completions_.size() - 1);
    const auto completionRows = std::min(
        completions_.size(), static_cast<std::size_t>(std::clamp(std::floor(log_.h / (32 * k)), 1.0f, 5.0f)));
    if (completionSelected_ < completionOffset_)
        completionOffset_ = completionSelected_;
    if (completionRows && completionSelected_ >= completionOffset_ + completionRows)
        completionOffset_ = completionSelected_ - completionRows + 1;
    completionsMenu_ = {bar_.x, bar_.y - static_cast<float>(completionRows) * 32 * k - 4 * k, bar_.w,
                        static_cast<float>(completionRows) * 32 * k};
    state.contentHeight = 0;
    const float textWidth = std::max(log_.w - 20 * k, 0.0f);
    for (Line& line : state.lines) {
        if (!shows(line.message))
            continue;
        if (line.laidOutWidth != textWidth)
            layoutLine(line, textWidth);
        state.contentHeight += line.height;
    }
    const float maximum = std::max(state.contentHeight - std::max(log_.h - 16 * k, 0.0f), 0.0f);
    if (state.stickToBottom)
        state.scroll = maximum;
    state.scroll = std::clamp(state.scroll, 0.0f, maximum);
    if (state.scroll >= maximum - 0.5f)
        state.stickToBottom = true;
    if (focused_ || hovered_)
        lastActivity_ = time_;
    syncFocus();
}

void ClassicChat::drawModern(DrawList& list)
{
    const float k = scale_;
    Channel& state = channel(selected_);
    const float radius = 12 * k;
    if (windowEnabled_)
        section(list, log_, window_, Color4::from(modernBackground_, modernTransparency_), radius);
    if (hasRtcTab()) {
        section(list, tabs_, window_, {0.09f, 0.1f, 0.12f, 0.9f}, radius);
        for (Tab tab : {Tab::Game, Tab::Rtc}) {
            const RectF rect = tabRect(tab);
            // A pill inset in its half, concentric with the card's corners.
            if (tab == selected_)
                panel(list,
                      {rect.x + (tab == Tab::Game ? 4.0f : 2.0f) * k, rect.y + 4 * k, rect.w - 6 * k, rect.h - 8 * k},
                      {1, 1, 1, 0.13f}, radius - 4 * k);
            std::string label = tab == Tab::Game ? "Game" : "RTC";
            if (unread(tab))
                label += std::format(" ({})", unread(tab));
            TextParams params;
            params.text = label;
            params.style = modernText_;
            params.style.size *= k;
            params.box = {rect.w, rect.h};
            list.text(*atlas_, layoutText(*fonts_, params), rect.x, rect.y, rect);
        }
    }
    if (channelTabs_.h > 0) {
        section(list, channelTabs_, window_, Color4::from(modernChannelBackground_, modernChannelTransparency_),
                radius);
        for (const auto& channel : chatChannels_) {
            const RectF rect = channelRect(channel.name);
            if (channel.name == selectedChatChannel_)
                panel(list, rect, {1, 1, 1, 0.16f}, 4 * k);
            else if (rect.contains(mouseX_, mouseY_))
                panel(list, rect, Color4::from(modernChannelHover_, modernChannelTransparency_), 4 * k);
            TextParams params;
            params.text = channel.label;
            params.style = modernChannel_;
            if (channel.name == selectedChatChannel_)
                params.style.color = modernChannelSelected_;
            params.style.size *= k;
            params.box = {rect.w, rect.h};
            list.text(*atlas_, layoutText(*fonts_, params), rect.x, rect.y, rect);
        }
    }
    const RectF clip{log_.x + 8 * k, log_.y + 8 * k, std::max(log_.w - 16 * k, 0.0f), std::max(log_.h - 16 * k, 0.0f)};
    float y = clip.y - state.scroll;
    if (windowEnabled_)
        for (const Line& line : state.lines) {
            if (!shows(line.message))
                continue;
            if (y + line.height >= clip.y && y < clip.bottom()) {
                list.text(*atlas_, line.body, clip.x, y, clip);
                if (!line.name.glyphs.empty())
                    list.text(*atlas_, line.name, clip.x, y, clip);
            }
            y += line.height;
        }
    if (state.contentHeight > clip.h && clip.h > 0) {
        const float length = std::max(8 * k, clip.h * clip.h / state.contentHeight);
        const float offset = (clip.h - length) * state.scroll / (state.contentHeight - clip.h);
        panel(list, {log_.right() - 5 * k, clip.y + offset, 3 * k, length}, {1, 1, 1, 0.45f}, 1.5f * k);
    }
    if (!inputEnabled_)
        return;
    // The bar is the card's bottom band, under a hairline; focus lifts it a little.
    section(list, bar_, window_, Color4::from(modernInputBackground_, modernInputTransparency_), radius);
    if (focused_)
        section(list, bar_, window_, {1, 1, 1, 0.05f}, radius);
    if (bar_.y > window_.y)
        list.rect({bar_.x, bar_.y, bar_.w, std::max(1.0f, std::round(k))}, {1, 1, 1, 0.1f}, bar_);
    TextEditor& editor = state.editor;
    const bool hint = !focused_ && editor.text.empty();
    TextParams params;
    const auto focusKey = selected_ == Tab::Rtc ? Enum::KeyCode::Slash : focusKey_;
    const std::string placeholder
        = focusKey == Enum::KeyCode::None
              ? "To chat click here"
              : "To chat click here or press '"
                    + (focusKey == Enum::KeyCode::Slash ? std::string("/") : inputKeyText(focusKey)) + "' key";
    params.text = hint ? std::string_view(placeholder) : std::string_view(editor.text);
    params.style = modernInput_;
    params.style.size *= k;
    if (hint)
        params.style.color = modernPlaceholder_;
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Center;
    const float presetSpace = presetsButton_.w > 0 ? 32 * k : 0;
    const RectF textArea{bar_.x + 12 * k + presetSpace, bar_.y, std::max(bar_.w - 52 * k - presetSpace, 0.0f), bar_.h};
    params.box = {textArea.w, textArea.h};
    const TextLayout text = layoutText(*fonts_, params);
    auto caretAt = [&](std::size_t offset) {
        if (offset == 0)
            return 0.0f;
        const std::string prefix = editor.text.substr(0, offset) + "|";
        TextParams probe = params;
        probe.text = prefix;
        const auto measured = layoutText(*fonts_, probe);
        return measured.glyphs.empty() ? measured.bounds.x : measured.glyphs.back().x;
    };
    const float cursor = hint ? 0.0f : caretAt(editor.caret);
    if (cursor < state.editorScroll)
        state.editorScroll = cursor;
    if (cursor > state.editorScroll + textArea.w - 2 * k)
        state.editorScroll = cursor - textArea.w + 2 * k;
    state.editorScroll = std::clamp(state.editorScroll, 0.0f, std::max(text.bounds.x - textArea.w + 2 * k, 0.0f));
    const float origin = textArea.x - state.editorScroll;
    if (focused_ && editor.hasSelection()) {
        const auto [first, last] = editor.selection();
        list.rect({origin + caretAt(first), textArea.y + 8 * k, caretAt(last) - caretAt(first), textArea.h - 16 * k},
                  {0.2f, 0.45f, 0.9f, 0.55f}, textArea);
    }
    list.text(*atlas_, text, origin, textArea.y, textArea);
    if (focused_ && std::fmod(time_ - caretTime_, 1.0) < 0.5)
        list.rect({origin + cursor, textArea.y + 10 * k, k, textArea.h - 20 * k}, {1, 1, 1, 1}, textArea);
    panel(list, send_, editor.text.empty() ? Color4{1, 1, 1, 0.08f} : Color4{0.2f, 0.37f, 1, 1}, send_.h / 2);
    TextParams arrow;
    arrow.text = "↑";
    arrow.style = modernInput_;
    arrow.style.size = 20 * k;
    arrow.box = {send_.w, send_.h};
    list.text(*atlas_, layoutText(*fonts_, arrow), send_.x, send_.y, send_);
    if (presetsButton_.w > 0) {
        panel(list, presetsButton_, {1, 1, 1, 0.12f}, presetsButton_.h / 2);
        arrow.text = "+";
        arrow.box = {presetsButton_.w, presetsButton_.h};
        list.text(*atlas_, layoutText(*fonts_, arrow), presetsButton_.x, presetsButton_.y, presetsButton_);
    }
    if (!completions_.empty()) {
        panel(list, completionsMenu_, {0.1f, 0.11f, 0.13f, 0.99f}, 8 * k);
        for (std::size_t i = completionOffset_; i < completions_.size(); ++i) {
            const RectF choice = completionRect(i);
            if (choice.h <= 0)
                break;
            if (i == completionSelected_ || choice.contains(mouseX_, mouseY_))
                panel(list, choice, {1, 1, 1, 0.14f}, 4 * k);
            TextParams suggestion;
            suggestion.text = completions_[i];
            suggestion.style = modernInput_;
            suggestion.style.size *= k;
            suggestion.xAlignment = Enum::TextXAlignment::Left;
            suggestion.truncate = Enum::TextTruncate::AtEnd;
            suggestion.box = {choice.w - 16 * k, choice.h};
            list.text(*atlas_, layoutText(*fonts_, suggestion), choice.x + 8 * k, choice.y, choice);
        }
    }
    if (presetsOpen_) {
        panel(list, presetsMenu_, {0.1f, 0.11f, 0.13f, 0.99f}, 8 * k);
        if (presets_.empty()) {
            TextParams status;
            status.text
                = state.source && state.source->presetsLoading() ? "Loading phrases..." : "No phrases available";
            status.style = modernInput_;
            status.style.size *= k;
            status.box = {presetsMenu_.w, 32 * k};
            list.text(*atlas_, layoutText(*fonts_, status), presetsMenu_.x, presetsMenu_.y, presetsMenu_);
        }
        for (std::size_t i = presetOffset_; i < presets_.size(); ++i) {
            const RectF choice = presetChoiceRect(i);
            if (choice.h <= 0)
                break;
            if (choice.contains(mouseX_, mouseY_))
                panel(list, choice, {1, 1, 1, 0.12f}, 4 * k);
            TextParams phrase;
            phrase.text = presets_[i];
            phrase.style = modernInput_;
            phrase.style.size *= k;
            phrase.xAlignment = Enum::TextXAlignment::Left;
            phrase.truncate = Enum::TextTruncate::AtEnd;
            phrase.box = {choice.w - 16 * k, choice.h};
            list.text(*atlas_, layoutText(*fonts_, phrase), choice.x + 8 * k, choice.y, choice);
        }
    }
}

} // namespace lunar::ui
