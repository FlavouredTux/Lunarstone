#include <lunar/ui/LuauHighlight.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace lunar::ui {

namespace {

// Luau's reserved words, the ones a reader notices straight away.
constexpr std::array<std::string_view, 22> keywords{
    "and",  "break", "continue", "do",  "else",  "elseif", "end",     "false", "for", "function", "if",
    "in",   "local", "nil",      "not", "or",    "repeat", "return",  "then",  "true", "until",   "while",
};

bool digit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool wordStart(char c)
{
    return c == '_' || (std::isalpha(static_cast<unsigned char>(c)) != 0);
}

bool wordChar(char c)
{
    return c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// From the quote at `at` to the one that closes it, or to the end of the line
// when it never does. A backslash takes the next character with it.
std::size_t quoted(std::string_view line, std::size_t at, char quote)
{
    for (std::size_t i = at + 1; i < line.size(); ++i) {
        if (line[i] == '\\')
            ++i;
        else if (line[i] == quote)
            return i + 1;
    }
    return line.size();
}

// A [[long string]], or a [[long comment]] -- both run to its closing ]].
std::size_t bracketed(std::string_view line, std::size_t at)
{
    const std::size_t close = line.find("]]", at + 2);
    return close == std::string_view::npos ? line.size() : close + 2;
}

// A number: its digits, and the letters of a hex or an exponent with them.
std::size_t numbered(std::string_view line, std::size_t at)
{
    std::size_t i = at;
    while (i < line.size() && (wordChar(line[i]) || line[i] == '.')) {
        if ((line[i] == 'e' || line[i] == 'E') && i + 1 < line.size() && (line[i + 1] == '+' || line[i + 1] == '-'))
            ++i; // the exponent's sign belongs to it
        ++i;
    }
    return i;
}

} // namespace

std::vector<LuauSpan> highlightLuau(std::string_view line)
{
    std::vector<LuauSpan> spans;
    std::size_t plain = 0; // where the Code run being kept begins
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        LuauToken token = LuauToken::Code;
        std::size_t end = i + 1;
        if (c == '-' && line.substr(i, 4) == "--[[") {
            token = LuauToken::Comment;
            end = bracketed(line, i);
        } else if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            token = LuauToken::Comment;
            end = line.size(); // a comment runs to the end of its line
        } else if (c == '"' || c == '\'') {
            token = LuauToken::String;
            end = quoted(line, i, c);
        } else if (c == '[' && line.substr(i, 2) == "[[") {
            token = LuauToken::String;
            end = bracketed(line, i);
        } else if (digit(c) || (c == '.' && i + 1 < line.size() && digit(line[i + 1]))) {
            token = LuauToken::Number;
            end = numbered(line, i);
        } else if (wordStart(c)) {
            while (end < line.size() && wordChar(line[end]))
                ++end;
            const std::string_view word = line.substr(i, end - i);
            // Anything else is a name, and stays in the plain run.
            if (!std::ranges::contains(keywords, word)) {
                i = end;
                continue;
            }
            token = LuauToken::Keyword;
        }
        if (token != LuauToken::Code && end > i) {
            if (i > plain)
                spans.push_back({plain, i, LuauToken::Code});
            spans.push_back({i, end, token});
            plain = end;
        }
        i = end;
    }
    if (plain < line.size())
        spans.push_back({plain, line.size(), LuauToken::Code});
    return spans;
}

} // namespace lunar::ui