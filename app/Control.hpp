#pragma once

#include <lunar/platform/Window.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// --control <file>: play the window from a file instead of the keyboard and
// mouse, for whoever isn't at the keyboard. Lines appended to the file are
// taken (the file is emptied once read) and run in order, each as the input
// the compositor would have sent:
//
//     key W 1.5        hold a key that long (default a tap); names as Key's: W, Space, Escape, Num1, LeftShift
//     click 640 360    the left button at a point (logical pixels); "click 640 360 right" for the right
//     move 640 360     the pointer to a point
//     look 200 -40     turn: relative mouse motion, as with the pointer locked
//     scroll -3        the wheel, notches (positive zooms in)
//     type hello       text, as typed (into the chat or a TextBox that has focus)
//     wait 2           the next line in that many seconds
//     shot out.png     a picture of the next frame
//     quit             close the window
//
// While it's on, the window counts as focused and its pointer as locked.
class ControlFile {
public:
    explicit ControlFile(std::filesystem::path path);

    // Called right after the window polled its own events: reads new
    // lines (a few times a second) and injects what's due.
    void step(lunar::platform::Window& window);
    // A picture asked for, once: where to save it.
    std::optional<std::filesystem::path> takeScreenshot();
    // What it did, a line each (for the log).
    std::function<void(const std::string&)> note;

private:
    using Clock = std::chrono::steady_clock;
    struct Release {
        Clock::time_point at;
        lunar::platform::Event event;
    };
    void run(lunar::platform::Window& window, const std::string& line);

    std::filesystem::path path_;
    std::deque<std::string> lines_;
    std::vector<Release> releases_;
    Clock::time_point readAt_ = Clock::now();
    Clock::time_point resumeAt_ = Clock::now();
    lunar::platform::Vec2d pointer_{};
    std::optional<std::filesystem::path> screenshot_;
};

// A key by its Key name ("W", "Space", "Num1"); nullopt for none.
std::optional<lunar::platform::Key> keyNamed(std::string_view name);
