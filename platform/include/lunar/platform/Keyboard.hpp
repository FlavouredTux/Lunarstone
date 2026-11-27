#pragma once

#include <lunar/platform/Key.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct xkb_compose_table;
struct xkb_compose_state;

namespace lunar::platform {

namespace detail {
struct XkbDeleter {
    void operator()(xkb_context* p) const;
    void operator()(xkb_keymap* p) const;
    void operator()(xkb_state* p) const;
    void operator()(xkb_compose_table* p) const;
    void operator()(xkb_compose_state* p) const;
};
template <class T>
using XkbPtr = std::unique_ptr<T, XkbDeleter>;
} // namespace detail

struct KeyPress {
    Key key = Key::Unknown;
    std::uint32_t keysym = 0;
    std::string text; // empty for control keys and while a compose sequence is pending
    bool repeats = false;
};

// xkbcommon keymap, modifier and compose (dead key) state for one keyboard.
// Knows nothing about Wayland, so it's testable with a keymap built from names.
// Scancodes are evdev codes, as wl_keyboard reports them.
class Keyboard {
public:
    // Keymap text as received from wl_keyboard.keymap. The locale picks the
    // compose table; empty means LC_ALL, LC_CTYPE or LANG.
    static std::expected<Keyboard, std::string> fromString(std::string_view keymap, std::string_view locale = {});

    // Keymap from RMLVO names, e.g. fromNames("us", "intl").
    static std::expected<Keyboard, std::string> fromNames(std::string_view layout, std::string_view variant = {},
                                                          std::string_view locale = {});

    // A key went down or repeated: advances compose state and returns the text.
    KeyPress press(std::uint32_t scancode);

    // The Key for a scancode, from the unshifted level of the active layout,
    // falling back to the shifted level (digits on AZERTY).
    Key key(std::uint32_t scancode) const;
    bool repeats(std::uint32_t scancode) const;

    // wl_keyboard.modifiers. Returns true if modifiers() changed.
    bool updateModifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked, std::uint32_t group);
    Modifiers modifiers() const;

    // Mask bit for an XKB modifier name like "Shift" or "Control", 0 if unknown.
    std::uint32_t modifierMask(std::string_view name) const;

private:
    Keyboard() = default;
    static std::expected<Keyboard, std::string> finish(detail::XkbPtr<xkb_context> context,
                                                       detail::XkbPtr<xkb_keymap> keymap, std::string_view locale);

    detail::XkbPtr<xkb_context> context_;
    detail::XkbPtr<xkb_keymap> keymap_;
    detail::XkbPtr<xkb_state> state_;
    detail::XkbPtr<xkb_compose_table> composeTable_;
    detail::XkbPtr<xkb_compose_state> compose_;
    Modifiers modifiers_;
};

} // namespace lunar::platform
