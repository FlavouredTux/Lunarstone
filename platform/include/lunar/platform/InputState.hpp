#pragma once

#include <lunar/platform/Events.hpp>
#include <lunar/platform/Key.hpp>

#include <bitset>
#include <string>

namespace lunar::platform {

struct Vec2d {
    double x = 0;
    double y = 0;

    bool operator==(const Vec2d&) const = default;
};

// Polled view of the input, built by feeding it every event. "This frame"
// means since the last beginFrame(); Window::pollEvents calls it for you.
class InputState {
public:
    void beginFrame();
    void apply(const Event& event);

    bool isDown(Key key) const { return valid(key) && down_[index(key)]; }
    bool wasPressed(Key key) const { return valid(key) && pressed_[index(key)]; } // repeats excluded
    bool wasReleased(Key key) const { return valid(key) && released_[index(key)]; }

    bool isDown(MouseButton button) const { return valid(button) && buttonsDown_[index(button)]; }
    bool wasPressed(MouseButton button) const { return valid(button) && buttonsPressed_[index(button)]; }
    bool wasReleased(MouseButton button) const { return valid(button) && buttonsReleased_[index(button)]; }

    Modifiers modifiers() const { return modifiers_; }
    Vec2d mousePosition() const { return mousePosition_; }
    Vec2d mouseDelta() const { return mouseDelta_; } // unaccelerated relative motion this frame
    Vec2d scrollDelta() const { return scrollDelta_; }
    const std::string& text() const { return text_; } // typed this frame
    bool focused() const { return focused_; }
    bool pointerInside() const { return pointerInside_; }
    bool pointerLocked() const { return pointerLocked_; }

private:
    template <class E>
    static constexpr std::size_t index(E e)
    {
        return static_cast<std::size_t>(e);
    }
    static constexpr bool valid(Key key) { return index(key) < keyCount; }
    static constexpr bool valid(MouseButton button) { return index(button) < mouseButtonCount; }

    void releaseKeys();
    void releaseButtons();

    std::bitset<keyCount> down_;
    std::bitset<keyCount> pressed_;
    std::bitset<keyCount> released_;
    std::bitset<mouseButtonCount> buttonsDown_;
    std::bitset<mouseButtonCount> buttonsPressed_;
    std::bitset<mouseButtonCount> buttonsReleased_;
    Modifiers modifiers_;
    Vec2d mousePosition_;
    Vec2d mouseDelta_;
    Vec2d scrollDelta_;
    std::string text_;
    bool focused_ = false;
    bool pointerInside_ = false;
    bool pointerLocked_ = false;
};

} // namespace lunar::platform
