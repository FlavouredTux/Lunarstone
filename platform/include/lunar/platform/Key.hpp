#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lunar::platform {

// Layout-aware but shift-independent: on AZERTY the key labelled A is Key::A.
// Key::Count is a sentinel for sizing tables.
enum class Key : std::uint8_t {
    Unknown,
    // clang-format off
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // clang-format on
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Left,
    Right,
    Up,
    Down,
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    CapsLock,
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,
    // clang-format off
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    // clang-format on
    KeypadDecimal,
    KeypadDivide,
    KeypadMultiply,
    KeypadMinus,
    KeypadPlus,
    KeypadEnter,
    Count,
};

inline constexpr std::size_t keyCount = static_cast<std::size_t>(Key::Count);

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
    Back,
    Forward,
    Count,
};

inline constexpr std::size_t mouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

struct Modifiers {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool super = false;
    bool capsLock = false;
    bool numLock = false;

    bool operator==(const Modifiers&) const = default;
};

// XKB keysym (as in xkbcommon-keysyms.h) to Key; Key::Unknown if unmapped.
// Upper- and lowercase letters map to the same key.
Key keyFromKeysym(std::uint32_t keysym);

// Linux evdev button code (BTN_LEFT etc. from linux/input-event-codes.h), as
// wl_pointer reports it. Returns MouseButton::Count for buttons we don't name.
MouseButton mouseButtonFromEvdev(std::uint32_t button);

std::string_view keyName(Key key);
std::string_view mouseButtonName(MouseButton button);

} // namespace lunar::platform
