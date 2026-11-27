#include <lunar/ui/ClassicChat.hpp>

#include <lunar/DataTypes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace lunar::ui {

namespace {

// SourceSansBold: ChatSettings.DefaultFont and ChatBarFont.
lunar::Font chatFont()
{
    return fontFromEnum(Enum::Font::SourceSansBold);
}

// The chat bar's text: ChatBarTextColor black at TextTransparency 0.4.
constexpr float barTextTransparency = 0.4f;
// CreateBaseMessage: TextStrokeTransparency 0.75 in black.
constexpr float messageStrokeTransparency = 0.75f;

} // namespace

Color3 legacyNameColor(std::string_view name)
{
    // NAME_COLORS: the first three as literal RGB, the rest BrickColors.
    static const std::array<Color3, 8> colors = {
        Color3::fromRGB(253, 41, 67), // BrickColor.new("Bright red").Color
        Color3::fromRGB(1, 162, 255), // BrickColor.new("Bright blue").Color
        Color3::fromRGB(2, 184, 87),  // BrickColor.new("Earth green").Color
        BrickColor::fromName("Bright violet").color(),
        BrickColor::fromName("Bright orange").color(),
        BrickColor::fromName("Bright yellow").color(),
        BrickColor::fromName("Light reddish violet").color(),
        BrickColor::fromName("Brick yellow").color(),
    };
    // GetNameValue: bytes added or subtracted by their position from the end.
    const auto length = static_cast<long>(name.size());
    long value = 0;
    for (long index = 1; index <= length; ++index) {
        long c = static_cast<unsigned char>(name[static_cast<std::size_t>(index - 1)]);
        long reverseIndex = length - index + 1;
        if (length % 2 == 1)
            reverseIndex -= 1;
        if (reverseIndex % 4 >= 2)
            c = -c;
        value += c;
    }
    // Lua's % is floored, so negative sums still land in 0..7.
    const long slot = ((value % 8) + 8) % 8;
    return colors[static_cast<std::size_t>(slot)];
}

void LocalEchoChatSource::send(std::string_view text)
{
    if (!deliver)
        return;
    ChatMessage message;
    message.fromName = name_;
    message.displayName = displayName_.empty() ? name_ : displayName_;
    message.text = std::string(text);
    deliver(message);
}

ClassicChat::ClassicChat(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas)
{
    for (Channel& state : channels_)
        state.editor.maxLength = maximumMessageLength;
}

void ClassicChat::setSource(ChatSource* source)
{
    setSource(Tab::Game, source);
}

void ClassicChat::setSource(Tab tab, ChatSource* source)
{
    Channel& state = channel(tab);
    if (state.source) {
        state.source->deliver = nullptr;
        state.source->clearWindow = nullptr;
        state.source->removeUser = nullptr;
    }
    state.source = source;
    if (source) {
        source->deliver = [this, tab](const ChatMessage& message) { post(tab, message); };
        source->clearWindow = [this, tab] {
            Channel& target = channel(tab);
            target.messages.clear();
            target.lines.clear();
            target.scroll = 0;
            target.unread = 0;
        };
        source->removeUser = [this, tab](std::int64_t id) {
            Channel& target = channel(tab);
            std::erase_if(target.messages, [id](const auto& message) { return message.userId == id; });
            std::erase_if(target.lines, [id](const auto& line) { return line.message.userId == id; });
        };
    }
    if (!hasRtcTab() && selected_ == Tab::Rtc)
        selectTab(Tab::Game);
}

void ClassicChat::selectTab(Tab tab)
{
    if (tab == Tab::Rtc && !hasRtcTab())
        return;
    selected_ = tab;
    completions_.clear();
    completionPressed_.reset();
    completionPrefix_.clear();
    completionDismissed_.clear();
    presetsOpen_ = false;
    presetPressed_.reset();
    Channel& state = channel(tab);
    state.unread = 0;
    lastActivity_ = caretTime_ = time_;
    if (state.source)
        state.source->activate();
    syncFocus();
}

RectF ClassicChat::tabRect(Tab tab) const
{
    if (!hasRtcTab())
        return {};
    const float half = tabs_.w / 2.0f;
    return {tabs_.x + (tab == Tab::Rtc ? half : 0.0f), tabs_.y, half, tabs_.h};
}

void ClassicChat::post(ChatMessage message)
{
    post(Tab::Game, std::move(message));
}

void ClassicChat::post(Tab tab, ChatMessage message)
{
    Channel& state = channel(tab);
    if (!message.messageId.empty()) {
        for (std::size_t i = 0; i < state.lines.size(); ++i)
            if (state.lines[i].message.messageId == message.messageId) {
                state.messages[i] = message;
                state.lines[i] = {std::move(message), time_, {}, {}, 0.0f, -1.0f};
                state.lastMessage = time_;
                return;
            }
    }
    state.messages.push_back(message);
    if (state.messages.size() > historyLength)
        state.messages.pop_front();
    state.lines.push_back({std::move(message), time_, {}, {}, 0.0f, -1.0f});
    if (state.lines.size() > historyLength)
        state.lines.pop_front();
    state.lastMessage = time_;
    if (selected_ != tab)
        state.unread = std::min(state.unread + 1, historyLength);
}

void ClassicChat::postSystem(std::string text, std::optional<Color3> color)
{
    ChatMessage message;
    message.kind = ChatMessage::Kind::System;
    message.text = std::move(text);
    message.textColor = color;
    post(std::move(message));
}

void ClassicChat::focus()
{
    if (!visible)
        return;
    if (modern_ && !inputEnabled_)
        return;
    focused_ = true;
    caretTime_ = time_;
    lastActivity_ = time_;
    syncFocus();
}

void ClassicChat::releaseFocus(bool enterPressed)
{
    focused_ = false;
    completions_.clear();
    completionPressed_.reset();
    lastActivity_ = time_;
    syncFocus(enterPressed);
}

void ClassicChat::submit()
{
    Channel& state = channel(selected_);
    std::string text = state.editor.text;
    state.editor.setText("");
    releaseFocus(true);
    // Whitespace-only messages aren't sent.
    if (text.find_first_not_of(" \t") == std::string::npos)
        return;
    if (!modern_ && selected_ == Tab::Game && (text == "/?" || text == "/help")) {
        // A short version of the legacy help text.
        postSystem("These are the basic chat commands.");
        postSystem("/me <text> : roleplaying command for doing actions.");
        postSystem("/c <channel> : switch channel menu tabs.");
        postSystem("/mute <speaker> : mute a speaker.");
        postSystem("/unmute <speaker> : unmute a speaker.");
        return;
    }
    if (state.source)
        state.source->send(text);
}

bool ClassicChat::handle(const platform::Event& event)
{
    if (!visible)
        return false;
    Channel& state = channel(selected_);
    const float k = scale_;
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * k;
        mouseY_ = static_cast<float>(move->y) * k;
        hovered_ = window_.contains(mouseX_, mouseY_);
        if (hovered_)
            lastActivity_ = time_;
        return false;
    }
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        mouseX_ = static_cast<float>(button->x) * k;
        mouseY_ = static_cast<float>(button->y) * k;
        if (button->button != platform::MouseButton::Left)
            return false;
        if (modern_ && !button->pressed && completionPressed_) {
            const auto index = *completionPressed_;
            completionPressed_.reset();
            if (completionRect(index).contains(mouseX_, mouseY_))
                complete(index);
            return true;
        }
        if (modern_ && button->pressed && !completions_.empty()) {
            if (completionsMenu_.contains(mouseX_, mouseY_)) {
                for (std::size_t i = completionOffset_; i < completions_.size(); ++i)
                    if (completionRect(i).contains(mouseX_, mouseY_)) {
                        completionPressed_ = i;
                        break;
                    }
                return true;
            }
            completionDismissed_ = state.editor.text;
            completions_.clear();
        }
        if (modern_ && !button->pressed && presetPressed_) {
            const auto index = *presetPressed_;
            presetPressed_.reset();
            if (presetsOpen_ && presetChoiceRect(index).contains(mouseX_, mouseY_) && state.source) {
                state.source->sendPreset(index);
                presetsOpen_ = false;
            }
            return true;
        }
        if (modern_ && button->pressed && presetsButton_.w > 0 && presetsButton_.contains(mouseX_, mouseY_)) {
            presetsOpen_ = !presetsOpen_;
            if (presetsOpen_ && state.source)
                state.source->requestPresets();
            return true;
        }
        if (modern_ && button->pressed && presetsOpen_) {
            for (std::size_t i = presetOffset_; i < presets_.size(); ++i)
                if (presetChoiceRect(i).contains(mouseX_, mouseY_)) {
                    presetPressed_ = i;
                    return true;
                }
            if (presetsMenu_.contains(mouseX_, mouseY_))
                return true;
            presetsOpen_ = false;
        }
        if (modern_ && !button->pressed && sendPressed_) {
            sendPressed_ = false;
            if (inputEnabled_ && send_.contains(mouseX_, mouseY_))
                submit();
            return true;
        }
        if (!button->pressed)
            return false;
        if (modern_ && selected_ == Tab::Game && channelTabs_.contains(mouseX_, mouseY_)) {
            for (const auto& info : chatChannels_)
                if (channelRect(info.name).contains(mouseX_, mouseY_) && state.source
                    && state.source->selectChannel(info.name)) {
                    selectedChatChannel_ = info.name;
                    state.stickToBottom = true;
                    break;
                }
            return true;
        }
        if (modern_ && inputEnabled_ && send_.contains(mouseX_, mouseY_)) {
            sendPressed_ = true;
            return true;
        }
        if (hasRtcTab() && tabs_.contains(mouseX_, mouseY_)) {
            selectTab(tabRect(Tab::Rtc).contains(mouseX_, mouseY_) ? Tab::Rtc : Tab::Game);
            return true;
        }
        // Clicking the chat bar types into it; clicking anywhere else lets go.
        if (bar_.contains(mouseX_, mouseY_) && (!modern_ || inputEnabled_)) {
            focus();
            return true;
        }
        if (focused_)
            releaseFocus();
        return false;
    }
    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        if (modern_ && !completions_.empty() && completionsMenu_.contains(mouseX_, mouseY_)) {
            const long index = static_cast<long>(completionSelected_) - static_cast<long>(std::round(scroll->dy));
            completionSelected_
                = static_cast<std::size_t>(std::clamp(index, 0L, static_cast<long>(completions_.size() - 1)));
            return true;
        }
        if (modern_ && presetsOpen_ && presetsMenu_.contains(mouseX_, mouseY_)) {
            const auto amount = static_cast<long>(std::round(scroll->dy));
            presetOffset_ = static_cast<std::size_t>(
                std::clamp(static_cast<long>(presetOffset_) - amount, 0L, static_cast<long>(presets_.size())));
            return true;
        }
        if (!log_.contains(mouseX_, mouseY_))
            return false;
        state.scroll -= static_cast<float>(scroll->dy) * textSize * 2.0f * k;
        state.stickToBottom = false;
        lastActivity_ = time_;
        return true;
    }
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (modern_ && key->pressed && !completions_.empty()) {
            if (key->key == platform::Key::Tab) {
                complete(completionSelected_);
                return true;
            }
            if (key->key == platform::Key::Up || key->key == platform::Key::Down) {
                completionSelected_ = key->key == platform::Key::Up
                                          ? (completionSelected_ + completions_.size() - 1) % completions_.size()
                                          : (completionSelected_ + 1) % completions_.size();
                return true;
            }
            if (key->key == platform::Key::Escape) {
                completionDismissed_ = state.editor.text;
                completions_.clear();
                return true;
            }
        }
        if (modern_ && selected_ == Tab::Game && focusKey_ != Enum::KeyCode::Slash && focusKey_ != Enum::KeyCode::None
            && platform::keyName(key->key) == enums::item(focusKey_).name) {
            if (!key->pressed)
                skipFocusText_ = false;
            else if (!focused_ && inputEnabled_) {
                focus();
                skipFocusText_ = true;
                return true;
            }
        }
        if (presetsOpen_ && key->pressed && key->key == platform::Key::Escape) {
            presetsOpen_ = false;
            return true;
        }
        if (!focused_)
            return false; // "/" opens the chat bar as typed text, below

        switch (state.editor.key(*key)) {
        case TextEditor::Result::Submitted:
            submit();
            break;
        case TextEditor::Result::Cancelled:
            releaseFocus();
            break;
        default:
            caretTime_ = time_;
            break;
        }
        lastActivity_ = time_;
        return true;
    }
    if (const auto* text = std::get_if<platform::TextEvent>(&event)) {
        if (std::exchange(skipFocusText_, false))
            return true;
        if (!focused_) {
            // "/" opens the chat bar (ChatSettings: the slash key) and isn't
            // typed. Going by the character, not the key, finds it on every
            // layout: Shift+7 on Swiss and German keyboards.
            if (text->text != "/" || (modern_ && selected_ == Tab::Game && focusKey_ != Enum::KeyCode::Slash))
                return false;
            focus();
            return true;
        }
        state.editor.insert(text->text);
        caretTime_ = time_;
        lastActivity_ = time_;
        return true;
    }
    return false;
}

float ClassicChat::spaceWidth(const lunar::Font& font, float size) const
{
    const FontFace* face = fonts_->face(font);
    if (!face)
        return size / 4;
    const std::uint16_t g = face->glyphIndex(U' ');
    return static_cast<float>(face->advance(g)) * pixelsPerUnit(*face, size * familySizeScale(font.family));
}

void ClassicChat::layoutLine(Line& line, float width)
{
    const lunar::Font font = modern_ ? modernText_.font : chatFont();
    const float size = (modern_ ? modernText_.size : textSize) * scale_;
    TextParams params;
    params.style.font = font;
    params.style.size = size;
    if (modern_) {
        params.style.strokeColor = modernText_.strokeColor;
        params.style.strokeTransparency = modernText_.strokeTransparency;
    } else {
        params.style.strokeTransparency = 1.0f;
    }
    params.wrapped = true;
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Top;
    params.box = {width, 1e6f};
    params.richText = line.message.richText;

    std::string body = line.message.text;
    if (line.message.kind == ChatMessage::Kind::Player) {
        // DefaultChatMessage: "[Name]:" in the name colour, then the message
        // padded with enough spaces to start after it (GetNumberOfSpaces + 1).
        const std::string label = !line.message.prefix.empty() ? line.message.prefix
                                  : modern_                    ? line.message.displayName + ":"
                                                               : "[" + line.message.displayName + "]:";
        TextParams nameParams = params;
        nameParams.text = label;
        nameParams.wrapped = false;
        nameParams.style.color = line.message.nameColor.value_or(legacyNameColor(line.message.fromName));
        line.name = layoutText(*fonts_, nameParams);
        const float space = spaceWidth(font, size);
        const auto spaces = static_cast<std::size_t>(std::ceil(line.name.bounds.x / std::max(space, 0.01f))) + 1;
        body = std::string(spaces, ' ') + body;
    } else {
        line.name = {};
    }
    params.text = body;
    Color3 color = modern_ ? modernText_.color : Color3{1.0f, 1.0f, 1.0f};
    if (line.message.pending)
        color = {0.7f, 0.7f, 0.7f};
    if (line.message.kind == ChatMessage::Kind::Error)
        color = errorColor;
    if (line.message.textColor)
        color = *line.message.textColor;
    params.style.color = color;
    line.body = layoutText(*fonts_, params);
    line.height = std::max(line.body.bounds.y, size) + (modern_ ? 2 * scale_ : 0);
    line.laidOutWidth = width;
}

float ClassicChat::alphaAt(double since, double fadeOutAfter) const
{
    const double t = time_ - since - fadeOutAfter;
    if (t <= 0.0)
        return 1.0f;
    return static_cast<float>(std::max(0.0, 1.0 - t / fadeDuration));
}

void ClassicChat::update(double time, Vector2 screen, float topInset, float pixelScale)
{
    time_ = time;
    for (Channel& state : channels_)
        if (state.source)
            state.source->poll();
    if (scale_ != pixelScale)
        for (Channel& state : channels_)
            for (Line& line : state.lines)
                line.laidOutWidth = -1.0f;
    scale_ = pixelScale;
    if (modern_) {
        updateModern(screen, topInset);
        return;
    }
    const float k = pixelScale;
    // The Chat ScreenGui doesn't ignore the inset: the window starts under the top bar.
    const float top = topInset * k;
    const float width = std::round(screen.x * windowWidthScale);
    const float height = std::round((screen.y - top) * windowHeightScale + windowHeightOffset * k);
    window_ = {0.0f, top, width, height};
    const float barHeight = (textSize + windowHeightOffset) * k;
    bar_ = {0.0f, window_.bottom() - barHeight, width, barHeight};
    const float tabsHeight = hasRtcTab() ? (textSize + textPadding * 2.0f) * k : 0.0f;
    tabs_ = {0.0f, top, width, tabsHeight};
    log_ = {0.0f, top + tabsHeight, width, std::max(height - barHeight - gap * k - tabsHeight, 0.0f)};

    // MessageLogDisplay's Scroller: 3 px down, 4 px narrower, 6 px shorter; labels 8 px in, 14 px narrower.
    const float scrollerWidth = width - 4.0f * k;
    const float labelWidth = scrollerWidth - (messageBorder + 6.0f) * k;
    Channel& state = channel(selected_);
    state.contentHeight = 0.0f;
    for (Line& line : state.lines) {
        if (line.laidOutWidth != labelWidth)
            layoutLine(line, labelWidth);
        state.contentHeight += line.height;
    }
    const float viewHeight = log_.h - 6.0f * k;
    const float maxScroll = std::max(state.contentHeight - viewHeight, 0.0f);
    if (state.stickToBottom)
        state.scroll = maxScroll;
    state.scroll = std::clamp(state.scroll, 0.0f, maxScroll);
    if (state.scroll >= maxScroll - 0.5f)
        state.stickToBottom = true;
    if (focused_ || hovered_)
        lastActivity_ = time;
}

void ClassicChat::draw(DrawList& list)
{
    if (!visible)
        return;
    if (modern_) {
        drawModern(list);
        return;
    }
    Channel& state = channel(selected_);
    TextEditor& editor = state.editor;
    const float k = scale_;
    const RectF screenClip{-1e9f, -1e9f, 2e9f, 2e9f};
    // Background and chat bar fade after ChatWindowBackgroundFadeOutTime without hover or focus.
    const float bgAlpha = alphaAt(lastActivity_, backgroundFadeOutTime);
    // Messages fade after ChatWindowTextFadeOutTime without new ones (or hover).
    const float textAlpha = std::max(alphaAt(state.lastMessage, textFadeOutTime), bgAlpha);

    if (bgAlpha > 0.0f) {
        const float a = (1.0f - backgroundTransparency) * bgAlpha;
        if (hasRtcTab()) {
            for (Tab tab : {Tab::Game, Tab::Rtc}) {
                const RectF rect = tabRect(tab);
                list.rect(rect, {0, 0, 0, a}, screenClip);
                if (selected_ == tab)
                    list.rect(rect, {1, 1, 1, 0.12f * bgAlpha}, screenClip);
                std::string label = tab == Tab::Game ? "Game" : "RTC";
                if (const std::size_t count = unread(tab); count > 0)
                    label += std::format(" ({})", count);
                TextParams params;
                params.text = label;
                params.style.font = chatFont();
                params.style.size = textSize * k;
                params.style.color = {1, 1, 1};
                params.style.transparency = 1.0f - bgAlpha;
                params.xAlignment = Enum::TextXAlignment::Center;
                params.yAlignment = Enum::TextYAlignment::Center;
                params.box = {rect.w, rect.h};
                list.text(*atlas_, layoutText(*fonts_, params), rect.x, rect.y, rect);
            }
        }
        list.rect(log_, {0, 0, 0, a}, screenClip);
        list.rect(bar_, {0, 0, 0, a}, screenClip);
        // BoxFrame: ChatBarBoxColor white at 0.6 transparency, 7 px in.
        const RectF box{bar_.x + boxPadding * k, bar_.y + boxPadding * k, bar_.w - 2 * boxPadding * k,
                        bar_.h - 2 * boxPadding * k};
        list.rect(box, {1, 1, 1, a}, screenClip);
        const RectF textArea{box.x + textPadding * k, box.y + textPadding * k, box.w - 2 * textPadding * k,
                             box.h - 2 * textPadding * k};
        TextParams params;
        params.style.font = chatFont();
        params.style.size = textSize * k;
        params.style.color = {0, 0, 0};
        params.style.transparency = 1.0f - (1.0f - barTextTransparency) * bgAlpha;
        params.wrapped = false;
        params.xAlignment = Enum::TextXAlignment::Left;
        params.yAlignment = Enum::TextYAlignment::Top;
        params.box = {textArea.w, textArea.h};
        // The hint, from CoreScriptLocalization.csv (GameChat_ChatMain_ChatBarText).
        static const std::string hint = "To chat click here or press \"/\" key";
        const bool showHint = !focused_ && editor.text.empty();
        params.text = showHint ? std::string_view(hint) : std::string_view(editor.text);
        TextLayout layout = layoutText(*fonts_, params);
        // Long drafts scroll so the caret stays visible.
        float shift = 0.0f;
        if (!showHint && layout.bounds.x > textArea.w)
            shift = textArea.w - layout.bounds.x;
        // Where a byte offset into the draft sits on screen.
        auto xAt = [&](std::size_t pos) {
            float x = textArea.x + shift;
            for (const PlacedGlyph& g : layout.glyphs)
                if (g.source < pos && g.face)
                    x = textArea.x + shift + g.x + static_cast<float>(g.face->advance(g.glyph)) * g.pixelsPerUnit;
            // Spaces don't make glyphs; measure the text up to there instead when it ends in one.
            if (pos > 0 && editor.text[pos - 1] == ' ') {
                TextParams upTo = params;
                const std::string before = editor.text.substr(0, pos) + "|";
                upTo.text = before;
                const TextLayout probe = layoutText(*fonts_, upTo);
                if (!probe.glyphs.empty())
                    x = textArea.x + shift + probe.glyphs.back().x;
            }
            return x;
        };
        if (focused_ && editor.hasSelection()) {
            // GUESS: a translucent blue like the client's default text selection.
            const auto [first, last] = editor.selection();
            const float from = std::round(xAt(first));
            list.rect({from, textArea.y, std::round(xAt(last)) - from, textSize * k}, {0.2f, 0.5f, 1.0f, 0.4f},
                      textArea);
        }
        list.text(*atlas_, layout, textArea.x + shift, textArea.y, textArea);
        if (focused_ && std::fmod(time_ - caretTime_, 1.0) < 0.5) {
            const float caretX = xAt(editor.caret);
            list.rect({std::round(caretX), textArea.y, std::max(1.0f, std::round(k)), textSize * k},
                      {0, 0, 0, 1.0f - barTextTransparency}, textArea);
        }
    }

    if (textAlpha <= 0.0f)
        return;
    const RectF scroller{log_.x, log_.y + 3.0f * k, log_.w - 4.0f * k, log_.h - 6.0f * k};
    float y = scroller.y - state.scroll;
    const Color4 stroke{0, 0, 0, (1.0f - messageStrokeTransparency) * textAlpha};
    for (Line& line : state.lines) {
        if (y + line.height >= scroller.y && y <= scroller.bottom()) {
            const float x = scroller.x + messageBorder * k;
            // Fade by scaling each run's opacity.
            auto faded = [&](TextLayout layout) {
                for (TextStyle& style : layout.styles)
                    style.transparency = 1.0f - (1.0f - style.transparency) * textAlpha;
                return layout;
            };
            list.text(*atlas_, faded(line.body), x, y, scroller, stroke);
            if (!line.name.glyphs.empty())
                list.text(*atlas_, faded(line.name), x, y, scroller, stroke);
        }
        y += line.height;
    }
    // The scroll bar while the log is scrollable and the background shows.
    const float viewHeight = scroller.h;
    if (state.contentHeight > viewHeight && bgAlpha > 0.0f) {
        const float length = std::max(viewHeight * viewHeight / state.contentHeight, 8.0f * k);
        const float offset = (viewHeight - length) * state.scroll / (state.contentHeight - viewHeight);
        list.rect({scroller.right(), scroller.y + offset, scrollBarThickness * k, length}, {1, 1, 1, 0.5f * bgAlpha},
                  screenClip);
    }
}

} // namespace lunar::ui
