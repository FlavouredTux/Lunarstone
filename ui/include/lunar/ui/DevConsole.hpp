#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::ui {

// The menu's Luau tab, Lunarstone's stand-in for the F9 console: what the
// game's client scripts print, warn and raise, and Luau typed in to run in
// their context. The app feeds the log and says how to run code.
class DevConsole {
public:
    enum class Kind : std::uint8_t { Output, Info, Warning, Error, Command };
    struct Line {
        Kind kind = Kind::Output;
        std::string text;
    };
    static constexpr std::size_t maxLines = 2000;
    static constexpr std::size_t maxHistory = 100;

    // From any thread.
    void add(Kind kind, std::string_view text);
    void clear();
    std::vector<Line> lines() const;
    // How many lines were ever added: moves whenever one is.
    std::uint64_t added() const;
    // The lines still kept that came after line number `since`, and added()
    // as it was then, read together so another thread can follow the log
    // without missing a line or seeing one twice. Lines are numbered from 1
    // as they're added; a clear() takes a number of its own, so the last line
    // kept is always number `added`.
    struct Recent {
        std::uint64_t added = 0;
        std::vector<Line> lines;
    };
    Recent linesSince(std::uint64_t since) const;

    // Runs code in the client's context; errors it hits right away come back
    // as the result (empty when it ran). Null: there's nothing to run it in.
    std::function<std::string(const std::string& code)> run;
    // Echoes the code, keeps it in the history and runs it.
    void submit(const std::string& code);
    // The echo alone, as Command lines: for code that comes from elsewhere
    // than the prompt (app/ConsoleApi), which stays out of the history.
    void echo(const std::string& code);
    const std::vector<std::string>& history() const { return history_; }

private:
    mutable std::mutex mutex_;
    std::deque<Line> lines_;
    std::uint64_t added_ = 0;
    std::vector<std::string> history_;
};

} // namespace lunar::ui
