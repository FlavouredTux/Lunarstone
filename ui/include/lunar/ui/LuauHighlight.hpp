#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lunar::ui {

// What a piece of Luau is, so the editor can colour it. Enough of the
// language to tell these apart, not a parser.
enum class LuauToken : std::uint8_t { Code, Keyword, String, Number, Comment };

// A run of one kind of token in a line of Luau: [begin, end) byte offsets
// into that line. The spans cover it whole, in order.
struct LuauSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    LuauToken token = LuauToken::Code;
};

// Splits one line of Luau into the runs its colouring is drawn from. A string
// or a comment still open when the line ends runs to the end of it: nothing is
// carried to the next line (GUESS: Roblox's own colouring, which the client
// draws itself, isn't known to us).
std::vector<LuauSpan> highlightLuau(std::string_view line);

} // namespace lunar::ui