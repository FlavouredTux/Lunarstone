#include <lunar/platform/Keyboard.hpp>

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <utility>

namespace lunar::platform {

namespace detail {
void XkbDeleter::operator()(xkb_context* p) const
{
    xkb_context_unref(p);
}
void XkbDeleter::operator()(xkb_keymap* p) const
{
    xkb_keymap_unref(p);
}
void XkbDeleter::operator()(xkb_state* p) const
{
    xkb_state_unref(p);
}
void XkbDeleter::operator()(xkb_compose_table* p) const
{
    xkb_compose_table_unref(p);
}
void XkbDeleter::operator()(xkb_compose_state* p) const
{
    xkb_compose_state_unref(p);
}
} // namespace detail

namespace {

using detail::XkbPtr;

// Evdev scancodes are offset by 8 in XKB, a leftover from X11.
constexpr xkb_keycode_t xkbKeycode(std::uint32_t scancode)
{
    return scancode + 8;
}

std::string localeFromEnvironment()
{
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"})
        if (const char* value = std::getenv(name); value && *value) // NOLINT(concurrency-mt-unsafe)
            return value;
    return "C";
}

XkbPtr<xkb_context> newContext()
{
    return XkbPtr<xkb_context>(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
}

bool isControl(const std::string& text)
{
    return text.size() == 1 && (static_cast<unsigned char>(text[0]) < 0x20 || text[0] == 0x7f);
}

} // namespace

std::expected<Keyboard, std::string> Keyboard::fromString(std::string_view keymap, std::string_view locale)
{
    auto context = newContext();
    if (!context)
        return std::unexpected("xkb_context_new failed");
    XkbPtr<xkb_keymap> map(xkb_keymap_new_from_buffer(context.get(), keymap.data(), keymap.size(),
                                                      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!map)
        return std::unexpected("could not compile the compositor's keymap");
    return finish(std::move(context), std::move(map), locale);
}

std::expected<Keyboard, std::string> Keyboard::fromNames(std::string_view layout, std::string_view variant,
                                                         std::string_view locale)
{
    auto context = newContext();
    if (!context)
        return std::unexpected("xkb_context_new failed");
    std::string layoutName(layout);
    std::string variantName(variant);
    xkb_rule_names names{
        .rules = nullptr,
        .model = nullptr,
        .layout = layoutName.c_str(),
        .variant = variantName.c_str(),
        .options = nullptr,
    };
    XkbPtr<xkb_keymap> map(xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!map)
        return std::unexpected("no XKB keymap for layout '" + layoutName + "'");
    return finish(std::move(context), std::move(map), locale);
}

std::expected<Keyboard, std::string> Keyboard::finish(XkbPtr<xkb_context> context, XkbPtr<xkb_keymap> keymap,
                                                      std::string_view locale)
{
    Keyboard keyboard;
    keyboard.state_.reset(xkb_state_new(keymap.get()));
    if (!keyboard.state_)
        return std::unexpected("xkb_state_new failed");

    // No compose table just means no dead keys.
    std::string localeName = locale.empty() ? localeFromEnvironment() : std::string(locale);
    keyboard.composeTable_.reset(
        xkb_compose_table_new_from_locale(context.get(), localeName.c_str(), XKB_COMPOSE_COMPILE_NO_FLAGS));
    if (keyboard.composeTable_)
        keyboard.compose_.reset(xkb_compose_state_new(keyboard.composeTable_.get(), XKB_COMPOSE_STATE_NO_FLAGS));

    keyboard.context_ = std::move(context);
    keyboard.keymap_ = std::move(keymap);
    return keyboard;
}

KeyPress Keyboard::press(std::uint32_t scancode)
{
    xkb_keycode_t code = xkbKeycode(scancode);
    KeyPress result{
        .key = key(scancode),
        .keysym = xkb_state_key_get_one_sym(state_.get(), code),
        .text = {},
        .repeats = repeats(scancode),
    };

    auto plainText = [&] {
        auto size = xkb_state_key_get_utf8(state_.get(), code, nullptr, 0);
        if (size <= 0)
            return;
        result.text.resize(static_cast<std::size_t>(size) + 1);
        xkb_state_key_get_utf8(state_.get(), code, result.text.data(), result.text.size());
        result.text.resize(static_cast<std::size_t>(size));
    };

    if (!compose_ || xkb_compose_state_feed(compose_.get(), result.keysym) != XKB_COMPOSE_FEED_ACCEPTED) {
        plainText();
    } else {
        switch (xkb_compose_state_get_status(compose_.get())) {
        case XKB_COMPOSE_NOTHING:
            plainText();
            break;
        case XKB_COMPOSE_COMPOSING:
            break;
        case XKB_COMPOSE_COMPOSED: {
            auto size = xkb_compose_state_get_utf8(compose_.get(), nullptr, 0);
            if (size > 0) {
                result.text.resize(static_cast<std::size_t>(size) + 1);
                xkb_compose_state_get_utf8(compose_.get(), result.text.data(), result.text.size());
                result.text.resize(static_cast<std::size_t>(size));
            }
            xkb_compose_state_reset(compose_.get());
            break;
        }
        case XKB_COMPOSE_CANCELLED:
            xkb_compose_state_reset(compose_.get());
            break;
        }
    }

    if (isControl(result.text))
        result.text.clear();
    return result;
}

Key Keyboard::key(std::uint32_t scancode) const
{
    xkb_keycode_t code = xkbKeycode(scancode);
    xkb_layout_index_t layout = xkb_state_key_get_layout(state_.get(), code);
    if (layout == XKB_LAYOUT_INVALID)
        return Key::Unknown;
    for (xkb_level_index_t level : {0U, 1U}) {
        const xkb_keysym_t* syms = nullptr;
        int count = xkb_keymap_key_get_syms_by_level(keymap_.get(), code, layout, level, &syms);
        if (count == 1)
            if (Key key = keyFromKeysym(syms[0]); key != Key::Unknown)
                return key;
    }
    return Key::Unknown;
}

bool Keyboard::repeats(std::uint32_t scancode) const
{
    return xkb_keymap_key_repeats(keymap_.get(), xkbKeycode(scancode)) != 0;
}

bool Keyboard::updateModifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                               std::uint32_t group)
{
    xkb_state_update_mask(state_.get(), depressed, latched, locked, 0, 0, group);
    auto active = [&](const char* name) {
        return xkb_state_mod_name_is_active(state_.get(), name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    Modifiers next{
        .shift = active(XKB_MOD_NAME_SHIFT),
        .ctrl = active(XKB_MOD_NAME_CTRL),
        .alt = active(XKB_MOD_NAME_ALT),
        .super = active(XKB_MOD_NAME_LOGO),
        .capsLock = active(XKB_MOD_NAME_CAPS),
        .numLock = active(XKB_MOD_NAME_NUM),
    };
    return std::exchange(modifiers_, next) != next;
}

Modifiers Keyboard::modifiers() const
{
    return modifiers_;
}

std::uint32_t Keyboard::modifierMask(std::string_view name) const
{
    xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap_.get(), std::string(name).c_str());
    return index == XKB_MOD_INVALID || index >= 32 ? 0 : 1U << index;
}

} // namespace lunar::platform
