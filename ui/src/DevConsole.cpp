#include <lunar/ui/DevConsole.hpp>

#include <algorithm>
#include <cstddef>

namespace lunar::ui {

void DevConsole::add(Kind kind, std::string_view text)
{
    const std::scoped_lock lock(mutex_);
    if (lines_.size() >= maxLines)
        lines_.pop_front();
    lines_.push_back({kind, std::string(text)});
    ++added_;
}

void DevConsole::clear()
{
    const std::scoped_lock lock(mutex_);
    lines_.clear();
    ++added_;
}

std::vector<DevConsole::Line> DevConsole::lines() const
{
    const std::scoped_lock lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

std::uint64_t DevConsole::added() const
{
    const std::scoped_lock lock(mutex_);
    return added_;
}

DevConsole::Recent DevConsole::linesSince(std::uint64_t since) const
{
    const std::scoped_lock lock(mutex_);
    Recent recent{.added = added_, .lines = {}};
    // The kept lines are numbers added_ - size + 1 through added_: a clear()
    // empties the log, so every line kept came after the last one.
    const std::uint64_t kept = lines_.size();
    const std::uint64_t first = added_ - kept + 1;
    const std::uint64_t skip = since < first ? 0 : std::min(since - first + 1, kept);
    recent.lines.assign(lines_.begin() + static_cast<std::ptrdiff_t>(skip), lines_.end());
    return recent;
}

void DevConsole::echo(const std::string& code)
{
    // The echo keeps the code's line breaks: the first line under the prompt,
    // the rest under it, as its own entry each so long ones still wrap.
    int n = 0;
    for (std::size_t at = 0; at <= code.size(); ++n) {
        const std::size_t newline = code.find('\n', at);
        const std::size_t end = newline == std::string::npos ? code.size() : newline;
        if (newline == std::string::npos && end == at)
            break; // nothing after a newline at the end
        add(Kind::Command, std::string(n == 0 ? "> " : "  ") + code.substr(at, end - at));
        if (newline == std::string::npos)
            break;
        at = newline + 1;
    }
}

void DevConsole::submit(const std::string& code)
{
    if (code.empty())
        return;
    echo(code);
    if (history_.empty() || history_.back() != code)
        history_.push_back(code);
    if (history_.size() > maxHistory)
        history_.erase(history_.begin());
    if (!run) {
        add(Kind::Error, "There are no client scripts to run this in.");
        return;
    }
    if (std::string error = run(code); !error.empty())
        add(Kind::Error, error);
}

} // namespace lunar::ui
