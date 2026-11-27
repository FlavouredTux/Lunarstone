#pragma once

#include <lunar/platform/Geometry.hpp>
#include <lunar/platform/Key.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lunar::platform {

struct KeyEvent {
    Key key = Key::Unknown;
    std::uint32_t scancode = 0; // evdev code, layout-independent
    std::uint32_t keysym = 0;   // XKB keysym with modifiers applied
    bool pressed = false;
    bool repeat = false; // synthesized from the compositor's repeat rate
    Modifiers modifiers{};
};

// Sent when the modifier or lock state changes, which on Wayland arrives
// after the key event that caused it.
struct ModifiersEvent {
    Modifiers modifiers{};
};

// UTF-8 text typed by the user, after compose/dead-key handling. Control
// characters (Enter, Backspace, Ctrl+letter) never show up here.
struct TextEvent {
    std::string text;
};

// Surface-local position in logical pixels.
struct MouseMoveEvent {
    double x = 0;
    double y = 0;
};

struct MouseButtonEvent {
    MouseButton button = MouseButton::Left;
    bool pressed = false;
    double x = 0;
    double y = 0;
    Modifiers modifiers{};
};

// In wheel notches. Positive dy scrolls up (away from the user), positive dx
// scrolls right. Touchpads give fractional values.
struct ScrollEvent {
    double dx = 0;
    double dy = 0;
};

// Mouse motion independent of the cursor, still reported while the pointer is
// locked. dx/dy include the compositor's acceleration, rawDx/rawDy don't.
struct RelativeMotionEvent {
    double dx = 0;
    double dy = 0;
    double rawDx = 0;
    double rawDy = 0;
};

struct MouseEnterEvent {
    bool entered = false;
};

struct FocusEvent {
    bool focused = false;
};

// The framebuffer size is what a swapchain should be created with.
struct ResizeEvent {
    Extent logicalSize{};
    Extent framebufferSize{};
    double scale = 1; // may be fractional
};

struct PointerLockEvent {
    bool locked = false;
};

struct CloseEvent {};

using Event = std::variant<KeyEvent, ModifiersEvent, TextEvent, MouseMoveEvent, MouseButtonEvent, ScrollEvent,
                           RelativeMotionEvent, MouseEnterEvent, FocusEvent, ResizeEvent, PointerLockEvent, CloseEvent>;

// FIFO of events between the Wayland callbacks and the application. Bounded so
// an application that never drains it can't grow it without limit: past the
// capacity the oldest events are dropped.
class EventQueue {
public:
    static constexpr std::size_t defaultCapacity = 4096;

    explicit EventQueue(std::size_t capacity = defaultCapacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    void push(Event event)
    {
        if (events_.size() == capacity_) {
            events_.pop_front();
            ++dropped_;
        }
        events_.push_back(std::move(event));
    }

    std::optional<Event> pop()
    {
        if (events_.empty())
            return std::nullopt;
        Event event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    std::vector<Event> drain()
    {
        std::vector<Event> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
        events_.clear();
        return out;
    }

    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }
    std::size_t capacity() const { return capacity_; }
    std::size_t dropped() const { return dropped_; } // total since construction

private:
    std::deque<Event> events_;
    std::size_t capacity_;
    std::size_t dropped_ = 0;
};

} // namespace lunar::platform
