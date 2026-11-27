#include <lunar/ui/TextEditor.hpp>

#include <lunar/ui/Font.hpp>

#include <algorithm>

namespace lunar::ui {

namespace {

bool continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

Clipboard& clipboard()
{
    static Clipboard current;
    return current;
}

std::size_t newlines(std::string_view text)
{
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

} // namespace

void setClipboard(Clipboard value)
{
    clipboard() = std::move(value);
}

void TextEditor::setText(std::string value)
{
    text = std::move(value);
    caret = text.size();
    anchor.reset();
}

std::pair<std::size_t, std::size_t> TextEditor::selection() const
{
    if (!anchor)
        return {caret, caret};
    const std::size_t a = std::min(*anchor, text.size());
    const std::size_t b = std::min(caret, text.size());
    return {std::min(a, b), std::max(a, b)};
}

bool TextEditor::hasSelection() const
{
    const auto [first, last] = selection();
    return first != last;
}

void TextEditor::eraseSelection()
{
    const auto [first, last] = selection();
    text.erase(first, last - first);
    caret = first;
    anchor.reset();
}

// Shift extends the selection from where the caret was; without it any
// selection goes away.
void TextEditor::moveCaret(std::size_t to, bool extend)
{
    if (extend && !anchor)
        anchor = caret;
    else if (!extend)
        anchor.reset();
    caret = to;
    if (anchor && *anchor == caret)
        anchor.reset();
}

std::size_t TextEditor::length() const
{
    std::size_t n = 0;
    for (char c : text)
        n += continuation(c) ? 0u : 1u;
    return n;
}

std::vector<std::string_view> TextEditor::lines() const
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\n') {
            out.push_back(std::string_view(text).substr(start, i - start));
            start = i + 1;
        }
    return out;
}

std::size_t TextEditor::lineCount() const
{
    return newlines(text) + 1;
}

std::size_t TextEditor::lineStart() const
{
    return lineStartAt(caret);
}

std::size_t TextEditor::lineStartAt(std::size_t pos) const
{
    if (pos == 0)
        return 0;
    const std::size_t newline = text.rfind('\n', pos - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

std::size_t TextEditor::lineEnd() const
{
    const std::size_t newline = text.find('\n', caret);
    return newline == std::string::npos ? text.size() : newline;
}

std::size_t TextEditor::caretLine() const
{
    return newlines(std::string_view(text).substr(0, std::min(caret, text.size())));
}

// A line up or down, as far along it as the column was.
std::size_t TextEditor::vertical(std::size_t from, int by) const
{
    const std::string_view view(text);
    const std::size_t here = lineStart();
    const std::size_t column = from - here;
    const std::size_t edge = by < 0 ? here : lineEnd();
    if (by < 0) {
        if (edge == 0)
            return 0;
        if (edge == 1 || view[edge - 2] == '\n') // the newline above it ends here
            return edge - 1;
    } else {
        if (edge >= text.size())
            return text.size();
        if (view[edge + 1] == '\n') // and the one below it starts here
            return edge + 1;
    }
    const std::size_t start = lineStartAt(by < 0 ? edge - 1 : edge + 1);
    const std::size_t newline = text.find('\n', start);
    const std::size_t end = newline == std::string::npos ? text.size() : newline;
    return start + std::min(column, end - start);
}

std::size_t TextEditor::previous(std::size_t pos) const
{
    if (pos == 0)
        return 0;
    --pos;
    while (pos > 0 && continuation(text[pos]))
        --pos;
    return pos;
}

std::size_t TextEditor::next(std::size_t pos) const
{
    if (pos >= text.size())
        return text.size();
    ++pos;
    while (pos < text.size() && continuation(text[pos]))
        ++pos;
    return pos;
}

void TextEditor::insert(std::string_view typed)
{
    std::string clean;
    std::size_t pos = 0;
    while (pos < typed.size()) {
        const char32_t cp = decodeUtf8(typed, pos);
        if (cp == U'\n' || cp == U'\r') {
            if (multiLine)
                clean += '\n';
            continue;
        }
        if (cp < 0x20 || cp == 0x7F)
            continue;
        appendUtf8(clean, cp);
    }
    // What the selection frees counts toward the room.
    const auto [first, last] = selection();
    std::size_t selected = 0;
    for (std::size_t i = first; i < last; ++i)
        selected += continuation(text[i]) ? 0u : 1u;
    std::size_t room
        = maxLength == std::string::npos ? std::string::npos : maxLength - std::min(maxLength, length() - selected);
    std::size_t cut = 0;
    std::size_t points = 0;
    while (cut < clean.size() && points < room) {
        cut = cut + 1;
        while (cut < clean.size() && continuation(clean[cut]))
            ++cut;
        ++points;
    }
    if (room != std::string::npos)
        clean.resize(cut);
    eraseSelection();
    caret = std::min(caret, text.size());
    text.insert(caret, clean);
    caret += clean.size();
}

TextEditor::Result TextEditor::key(const platform::KeyEvent& event)
{
    using platform::Key;
    if (!event.pressed)
        return Result::None;
    caret = std::min(caret, text.size());
    const bool shift = event.modifiers.shift;
    if (event.modifiers.ctrl && !event.modifiers.alt) {
        switch (event.key) {
        case Key::A:
            anchor = 0;
            caret = text.size();
            if (caret == 0)
                anchor.reset();
            return Result::Edited;
        case Key::C:
        case Key::X: {
            // Without a selection, the whole line.
            const auto [first, last]
                = hasSelection() ? selection() : std::pair<std::size_t, std::size_t>{0, text.size()};
            if (first != last && clipboard().write)
                clipboard().write(text.substr(first, last - first));
            if (event.key == Key::X) {
                anchor = first;
                caret = last;
                eraseSelection();
            }
            return Result::Edited;
        }
        case Key::V:
            if (clipboard().read)
                insert(clipboard().read());
            return Result::Edited;
        default:
            break;
        }
    }
    switch (event.key) {
    case Key::Enter:
    case Key::KeypadEnter:
        if (multiLine && !event.modifiers.ctrl) {
            insert("\n");
            return Result::Edited;
        }
        return Result::Submitted;
    case Key::Escape:
        return Result::Cancelled;
    case Key::Backspace:
        if (hasSelection()) {
            eraseSelection();
        } else if (caret > 0) {
            std::size_t from = previous(caret);
            if (event.modifiers.ctrl) {
                while (from > 0 && text[from] == ' ')
                    from = previous(from);
                while (from > 0 && text[previous(from)] != ' ')
                    from = previous(from);
            }
            text.erase(from, caret - from);
            caret = from;
        }
        return Result::Edited;
    case Key::Delete:
        if (hasSelection())
            eraseSelection();
        else if (caret < text.size())
            text.erase(caret, next(caret) - caret);
        return Result::Edited;
    case Key::Left:
        // A plain arrow collapses a selection to its edge.
        moveCaret(!shift && hasSelection() ? selection().first : previous(caret), shift);
        return Result::Edited;
    case Key::Right:
        moveCaret(!shift && hasSelection() ? selection().second : next(caret), shift);
        return Result::Edited;
    case Key::Home:
        moveCaret(multiLine ? lineStart() : 0, shift);
        return Result::Edited;
    case Key::End:
        moveCaret(multiLine ? lineEnd() : text.size(), shift);
        return Result::Edited;
    case Key::Up:
    case Key::Down:
        if (!multiLine)
            return Result::None;
        // A plain arrow collapses a selection to the edge it moves from.
        moveCaret(vertical(!shift && hasSelection() ? (event.key == Key::Up ? selection().first : selection().second)
                                                     : caret,
                           event.key == Key::Up ? -1 : 1),
                  shift);
        return Result::Edited;
    default:
        return Result::None;
    }
}

} // namespace lunar::ui
