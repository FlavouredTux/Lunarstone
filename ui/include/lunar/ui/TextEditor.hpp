#pragma once

#include <lunar/platform/Events.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::ui {

// How text editors reach the system clipboard; the app connects it to its
// window. Without one, copy and paste do nothing.
struct Clipboard {
    std::function<std::string()> read;
    std::function<void(std::string)> write;
};
void setClipboard(Clipboard value);

// Single-line (or multi-line) text editing state shared by TextBox and the
// chat bar: UTF-8 text and a caret as a byte offset on a code point boundary.
class TextEditor {
public:
    enum class Result : std::uint8_t {
        None,      // not an editing key
        Edited,    // text or caret changed
        Submitted, // Enter on a single-line editor
        Cancelled, // Escape
    };

    std::string text;
    std::size_t caret = 0;
    // The other end of the selection (Shift+arrows, Ctrl+A), when there is one.
    std::optional<std::size_t> anchor;
    std::size_t maxLength = std::string::npos; // in code points
    bool multiLine = false;

    void setText(std::string value);
    void insert(
        std::string_view typed); // replaces the selection; control characters dropped (newlines too unless multiLine)
    // Editing and caret keys, plus Ctrl+A/C/X/V. On a multi-line editor the
    // arrows and Home and End work within its lines; Enter only submits it
    // with Ctrl.
    Result key(const platform::KeyEvent& event);
    std::size_t length() const; // code points

    // The lines, split on the newlines: byte ranges the caller holds on to
    // until the text changes again. There is always one.
    std::vector<std::string_view> lines() const;
    std::size_t lineCount() const;
    // Where the caret's line begins and ends, and which line it is on
    // (from 0). All byte offsets; the newline between lines is neither.
    std::size_t lineStart() const;
    std::size_t lineEnd() const;
    std::size_t caretLine() const;

    // The selected byte range [first, second), empty without a selection.
    std::pair<std::size_t, std::size_t> selection() const;
    bool hasSelection() const;

private:
    void eraseSelection();
    void moveCaret(std::size_t to, bool extend);
    std::size_t previous(std::size_t pos) const;
    std::size_t next(std::size_t pos) const;
    // Where the line holding pos begins, and the caret a line up or down.
    std::size_t lineStartAt(std::size_t pos) const;
    std::size_t vertical(std::size_t from, int by) const;
};

} // namespace lunar::ui
