#include "Control.hpp"

#include <lunar/platform/Key.hpp>

#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

using lunar::platform::Event;
using lunar::platform::Key;

std::optional<Key> keyNamed(std::string_view name)
{
    for (int i = 1; i < static_cast<int>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        const std::string_view known = lunar::platform::keyName(key);
        if (known.size() == name.size()
            && std::equal(known.begin(), known.end(), name.begin(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
                                                      == std::tolower(static_cast<unsigned char>(b)); }))
            return key;
    }
    return std::nullopt;
}

ControlFile::ControlFile(std::filesystem::path path) : path_(std::move(path))
{
    std::error_code ec;
    std::filesystem::remove(path_, ec); // only what's written from now on
}

std::optional<std::filesystem::path> ControlFile::takeScreenshot()
{
    return std::exchange(screenshot_, std::nullopt);
}

void ControlFile::step(lunar::platform::Window& window)
{
    const auto now = Clock::now();
    // The window counts as focused while it's played from the file.
    if (!window.input().focused())
        window.inject(lunar::platform::FocusEvent{.focused = true});
    // Keys and buttons held long enough go up.
    std::erase_if(releases_, [&](const Release& r) {
        if (r.at > now)
            return false;
        window.inject(r.event);
        return true;
    });
    if (now - readAt_ >= std::chrono::milliseconds(200)) {
        readAt_ = now;
        std::error_code ec;
        if (std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) > 0) {
            std::ifstream in(path_);
            for (std::string line; std::getline(in, line);)
                if (!line.empty() && line[0] != '#')
                    lines_.push_back(line);
            in.close();
            std::ofstream(path_, std::ios::trunc); // taken
        }
    }
    while (!lines_.empty() && Clock::now() >= resumeAt_) {
        const std::string line = std::move(lines_.front());
        lines_.pop_front();
        run(window, line);
    }
}

void ControlFile::run(lunar::platform::Window& window, const std::string& line)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    const auto now = Clock::now();
    auto later = [&](double seconds) {
        return now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    };
    auto say = [&](std::string text) {
        if (note)
            note(std::move(text));
    };
    if (verb == "key") {
        std::string name;
        double seconds = 0.08;
        in >> name >> seconds;
        const auto key = keyNamed(name);
        if (!key) {
            say(std::format("control: no key {}", name));
            return;
        }
        window.inject(lunar::platform::KeyEvent{.key = *key, .pressed = true});
        releases_.push_back({later(seconds), lunar::platform::KeyEvent{.key = *key, .pressed = false}});
    } else if (verb == "click") {
        double x = 0, y = 0;
        std::string which;
        in >> x >> y >> which;
        const auto button = which == "right" ? lunar::platform::MouseButton::Right : lunar::platform::MouseButton::Left;
        pointer_ = {x, y};
        window.inject(lunar::platform::MouseMoveEvent{.x = x, .y = y});
        window.inject(lunar::platform::MouseButtonEvent{.button = button, .pressed = true, .x = x, .y = y});
        releases_.push_back(
            {later(0.06), lunar::platform::MouseButtonEvent{.button = button, .pressed = false, .x = x, .y = y}});
    } else if (verb == "move") {
        double x = 0, y = 0;
        in >> x >> y;
        pointer_ = {x, y};
        window.inject(lunar::platform::MouseMoveEvent{.x = x, .y = y});
    } else if (verb == "look") {
        double dx = 0, dy = 0;
        in >> dx >> dy;
        window.inject(lunar::platform::RelativeMotionEvent{.dx = dx, .dy = dy, .rawDx = dx, .rawDy = dy});
    } else if (verb == "scroll") {
        double dy = 0;
        in >> dy;
        window.inject(lunar::platform::ScrollEvent{.dx = 0, .dy = dy});
    } else if (verb == "type") {
        std::string text;
        std::getline(in, text);
        if (!text.empty() && text.front() == ' ')
            text.erase(0, 1);
        window.inject(lunar::platform::TextEvent{.text = text});
    } else if (verb == "wait") {
        double seconds = 0;
        in >> seconds;
        resumeAt_ = later(seconds);
    } else if (verb == "shot") {
        std::string out;
        in >> out;
        screenshot_ = out;
    } else if (verb == "quit") {
        window.setShouldClose(true);
    } else {
        say(std::format("control: what's \"{}\"?", line));
        return;
    }
    say("control: " + line);
}
