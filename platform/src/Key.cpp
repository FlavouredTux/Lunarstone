#include <lunar/platform/Key.hpp>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <array>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar::platform {

namespace {

constexpr Key offset(Key first, std::uint32_t by)
{
    return static_cast<Key>(static_cast<std::uint32_t>(first) + by);
}

#if __cpp_impl_reflection
// Enumerator identifiers indexed by value, so names never drift from the enum.
template <class E>
consteval auto enumNames()
{
    std::array<std::string_view, static_cast<std::size_t>(E::Count)> names{};
    for (std::meta::info e : std::meta::enumerators_of(^^E))
        if (E value = std::meta::extract<E>(e); value != E::Count)
            names[static_cast<std::size_t>(value)] = std::define_static_string(std::meta::identifier_of(e));
    return names;
}
#endif

template <class E>
std::string_view enumName([[maybe_unused]] E value)
{
#if __cpp_impl_reflection
    static constexpr auto names = enumNames<E>();
    auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "Unknown";
#else
    return {};
#endif
}

} // namespace

Key keyFromKeysym(std::uint32_t keysym)
{
    if (keysym >= XKB_KEY_a && keysym <= XKB_KEY_z)
        return offset(Key::A, keysym - XKB_KEY_a);
    if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z)
        return offset(Key::A, keysym - XKB_KEY_A);
    if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9)
        return offset(Key::Num0, keysym - XKB_KEY_0);
    if (keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F12)
        return offset(Key::F1, keysym - XKB_KEY_F1);
    if (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_9)
        return offset(Key::Keypad0, keysym - XKB_KEY_KP_0);

    switch (keysym) {
    case XKB_KEY_space:
        return Key::Space;
    case XKB_KEY_Return:
        return Key::Enter;
    case XKB_KEY_Escape:
        return Key::Escape;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        return Key::Tab;
    case XKB_KEY_BackSpace:
        return Key::Backspace;
    case XKB_KEY_Insert:
        return Key::Insert;
    case XKB_KEY_Delete:
        return Key::Delete;
    case XKB_KEY_Home:
        return Key::Home;
    case XKB_KEY_End:
        return Key::End;
    case XKB_KEY_Prior:
        return Key::PageUp;
    case XKB_KEY_Next:
        return Key::PageDown;
    case XKB_KEY_Left:
        return Key::Left;
    case XKB_KEY_Right:
        return Key::Right;
    case XKB_KEY_Up:
        return Key::Up;
    case XKB_KEY_Down:
        return Key::Down;
    case XKB_KEY_Shift_L:
        return Key::LeftShift;
    case XKB_KEY_Shift_R:
        return Key::RightShift;
    case XKB_KEY_Control_L:
        return Key::LeftCtrl;
    case XKB_KEY_Control_R:
        return Key::RightCtrl;
    case XKB_KEY_Alt_L:
        return Key::LeftAlt;
    case XKB_KEY_Alt_R:
    case XKB_KEY_ISO_Level3_Shift:
        return Key::RightAlt; // AltGr
    case XKB_KEY_Super_L:
        return Key::LeftSuper;
    case XKB_KEY_Super_R:
        return Key::RightSuper;
    case XKB_KEY_Caps_Lock:
        return Key::CapsLock;
    case XKB_KEY_minus:
        return Key::Minus;
    case XKB_KEY_equal:
        return Key::Equals;
    case XKB_KEY_bracketleft:
        return Key::LeftBracket;
    case XKB_KEY_bracketright:
        return Key::RightBracket;
    case XKB_KEY_backslash:
        return Key::Backslash;
    case XKB_KEY_semicolon:
        return Key::Semicolon;
    case XKB_KEY_apostrophe:
        return Key::Apostrophe;
    case XKB_KEY_grave:
        return Key::Grave;
    case XKB_KEY_comma:
        return Key::Comma;
    case XKB_KEY_period:
        return Key::Period;
    case XKB_KEY_slash:
        return Key::Slash;
    // Keypad keys produce their navigation keysyms at the base level (no Num Lock).
    case XKB_KEY_KP_Insert:
        return Key::Keypad0;
    case XKB_KEY_KP_End:
        return Key::Keypad1;
    case XKB_KEY_KP_Down:
        return Key::Keypad2;
    case XKB_KEY_KP_Next:
        return Key::Keypad3;
    case XKB_KEY_KP_Left:
        return Key::Keypad4;
    case XKB_KEY_KP_Begin:
        return Key::Keypad5;
    case XKB_KEY_KP_Right:
        return Key::Keypad6;
    case XKB_KEY_KP_Home:
        return Key::Keypad7;
    case XKB_KEY_KP_Up:
        return Key::Keypad8;
    case XKB_KEY_KP_Prior:
        return Key::Keypad9;
    case XKB_KEY_KP_Delete:
    case XKB_KEY_KP_Decimal:
    case XKB_KEY_KP_Separator:
        return Key::KeypadDecimal;
    case XKB_KEY_KP_Divide:
        return Key::KeypadDivide;
    case XKB_KEY_KP_Multiply:
        return Key::KeypadMultiply;
    case XKB_KEY_KP_Subtract:
        return Key::KeypadMinus;
    case XKB_KEY_KP_Add:
        return Key::KeypadPlus;
    case XKB_KEY_KP_Enter:
        return Key::KeypadEnter;
    default:
        return Key::Unknown;
    }
}

MouseButton mouseButtonFromEvdev(std::uint32_t button)
{
    switch (button) {
    case BTN_LEFT:
        return MouseButton::Left;
    case BTN_RIGHT:
        return MouseButton::Right;
    case BTN_MIDDLE:
        return MouseButton::Middle;
    case BTN_SIDE:
    case BTN_BACK:
        return MouseButton::Back;
    case BTN_EXTRA:
    case BTN_FORWARD:
        return MouseButton::Forward;
    default:
        return MouseButton::Count;
    }
}

std::string_view keyName(Key key)
{
    return enumName(key);
}

std::string_view mouseButtonName(MouseButton button)
{
    return enumName(button);
}

} // namespace lunar::platform
