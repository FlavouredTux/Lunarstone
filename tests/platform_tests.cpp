#include "framework.hpp"

#include <lunar/platform/Events.hpp>
#include <lunar/platform/Geometry.hpp>
#include <lunar/platform/InputState.hpp>
#include <lunar/platform/Key.hpp>
#include <lunar/platform/KeyRepeat.hpp>
#include <lunar/platform/Keyboard.hpp>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <chrono>
#include <string>
#include <variant>

using namespace lunar::platform;
using namespace std::chrono_literals;

TEST(keysym_maps_letters_case_insensitively)
{
    CHECK(keyFromKeysym(XKB_KEY_a) == Key::A);
    CHECK(keyFromKeysym(XKB_KEY_A) == Key::A);
    CHECK(keyFromKeysym(XKB_KEY_z) == Key::Z);
    CHECK(keyFromKeysym(XKB_KEY_W) == Key::W);
}

TEST(keysym_maps_digits_function_keys_and_named_keys)
{
    CHECK(keyFromKeysym(XKB_KEY_0) == Key::Num0);
    CHECK(keyFromKeysym(XKB_KEY_9) == Key::Num9);
    CHECK(keyFromKeysym(XKB_KEY_F1) == Key::F1);
    CHECK(keyFromKeysym(XKB_KEY_F12) == Key::F12);
    CHECK(keyFromKeysym(XKB_KEY_space) == Key::Space);
    CHECK(keyFromKeysym(XKB_KEY_Return) == Key::Enter);
    CHECK(keyFromKeysym(XKB_KEY_Escape) == Key::Escape);
    CHECK(keyFromKeysym(XKB_KEY_ISO_Left_Tab) == Key::Tab);
    CHECK(keyFromKeysym(XKB_KEY_Up) == Key::Up);
    CHECK(keyFromKeysym(XKB_KEY_Shift_R) == Key::RightShift);
    CHECK(keyFromKeysym(XKB_KEY_Control_L) == Key::LeftCtrl);
    CHECK(keyFromKeysym(XKB_KEY_Alt_L) == Key::LeftAlt);
    CHECK(keyFromKeysym(XKB_KEY_KP_Home) == Key::Keypad7);
    CHECK(keyFromKeysym(XKB_KEY_KP_7) == Key::Keypad7);
    CHECK(keyFromKeysym(XKB_KEY_eacute) == Key::Unknown);
}

TEST(key_names_come_from_the_enum)
{
    CHECK_EQ(keyName(Key::A), "A");
    CHECK_EQ(keyName(Key::F12), "F12");
    CHECK_EQ(keyName(Key::LeftShift), "LeftShift");
    CHECK_EQ(keyName(Key::KeypadEnter), "KeypadEnter");
    CHECK_EQ(mouseButtonName(MouseButton::Middle), "Middle");
}

TEST(evdev_buttons_map_to_mouse_buttons)
{
    CHECK(mouseButtonFromEvdev(BTN_LEFT) == MouseButton::Left);
    CHECK(mouseButtonFromEvdev(BTN_RIGHT) == MouseButton::Right);
    CHECK(mouseButtonFromEvdev(BTN_MIDDLE) == MouseButton::Middle);
    CHECK(mouseButtonFromEvdev(BTN_SIDE) == MouseButton::Back);
    CHECK(mouseButtonFromEvdev(BTN_EXTRA) == MouseButton::Forward);
    CHECK(mouseButtonFromEvdev(BTN_TASK) == MouseButton::Count);
}

TEST(event_queue_is_fifo)
{
    EventQueue queue;
    queue.push(KeyEvent{.key = Key::A, .pressed = true});
    queue.push(TextEvent{"a"});
    CHECK_EQ(queue.size(), 2u);
    auto first = queue.pop();
    CHECK(first && std::holds_alternative<KeyEvent>(*first));
    auto second = queue.pop();
    CHECK(second && std::get<TextEvent>(*second).text == "a");
    CHECK(!queue.pop());
}

TEST(event_queue_drops_oldest_past_capacity)
{
    EventQueue queue(2);
    queue.push(TextEvent{"1"});
    queue.push(TextEvent{"2"});
    queue.push(TextEvent{"3"});
    CHECK_EQ(queue.size(), 2u);
    CHECK_EQ(queue.dropped(), 1u);
    auto events = queue.drain();
    CHECK(queue.empty());
    CHECK_EQ(events.size(), 2u);
    CHECK_EQ(std::get<TextEvent>(events[0]).text, "2");
    CHECK_EQ(std::get<TextEvent>(events[1]).text, "3");
}

TEST(input_state_tracks_keys_per_frame)
{
    InputState input;
    input.apply(KeyEvent{.key = Key::W, .pressed = true});
    CHECK(input.isDown(Key::W));
    CHECK(input.wasPressed(Key::W));

    input.beginFrame();
    input.apply(KeyEvent{.key = Key::W, .pressed = true, .repeat = true});
    CHECK(input.isDown(Key::W));
    CHECK(!input.wasPressed(Key::W));

    input.beginFrame();
    input.apply(KeyEvent{.key = Key::W, .pressed = false});
    CHECK(!input.isDown(Key::W));
    CHECK(input.wasReleased(Key::W));

    input.beginFrame();
    CHECK(!input.wasReleased(Key::W));
}

TEST(input_state_press_and_release_in_one_frame_is_seen)
{
    InputState input;
    input.apply(KeyEvent{.key = Key::Space, .pressed = true});
    input.apply(KeyEvent{.key = Key::Space, .pressed = false});
    CHECK(input.wasPressed(Key::Space));
    CHECK(input.wasReleased(Key::Space));
    CHECK(!input.isDown(Key::Space));
}

TEST(input_state_accumulates_and_resets_mouse_delta)
{
    InputState input;
    input.apply(RelativeMotionEvent{.dx = 9, .dy = 9, .rawDx = 1.5, .rawDy = -2});
    input.apply(RelativeMotionEvent{.dx = 9, .dy = 9, .rawDx = 0.5, .rawDy = -1});
    input.apply(ScrollEvent{.dx = 0, .dy = 1});
    CHECK(input.mouseDelta() == (Vec2d{2, -3}));
    CHECK(input.scrollDelta() == (Vec2d{0, 1}));
    input.beginFrame();
    CHECK(input.mouseDelta() == Vec2d{});
    CHECK(input.scrollDelta() == Vec2d{});
}

TEST(input_state_tracks_buttons_and_position)
{
    InputState input;
    input.apply(MouseMoveEvent{.x = 10, .y = 20});
    CHECK(input.mousePosition() == (Vec2d{10, 20}));
    input.apply(MouseButtonEvent{.button = MouseButton::Right, .pressed = true, .x = 11, .y = 21});
    CHECK(input.isDown(MouseButton::Right));
    CHECK(input.wasPressed(MouseButton::Right));
    CHECK(input.mousePosition() == (Vec2d{11, 21}));
    input.beginFrame();
    input.apply(MouseEnterEvent{.entered = false});
    CHECK(!input.isDown(MouseButton::Right));
    CHECK(input.wasReleased(MouseButton::Right));
}

TEST(input_state_releases_keys_on_focus_loss)
{
    InputState input;
    input.apply(FocusEvent{.focused = true});
    input.apply(KeyEvent{.key = Key::A, .pressed = true});
    input.apply(ModifiersEvent{.modifiers = {.shift = true}});
    CHECK(input.modifiers().shift);
    input.apply(FocusEvent{.focused = false});
    CHECK(!input.focused());
    CHECK(!input.isDown(Key::A));
    CHECK(input.wasReleased(Key::A));
    CHECK(!input.modifiers().shift);
}

TEST(input_state_collects_text_and_ignores_bad_keys)
{
    InputState input;
    input.apply(TextEvent{"h"});
    input.apply(TextEvent{"\xc3\xa9"});
    CHECK_EQ(input.text(), "h\xc3\xa9");
    input.apply(KeyEvent{.key = Key::Count, .pressed = true});
    CHECK(!input.isDown(Key::Count));
    input.beginFrame();
    CHECK(input.text().empty());
}

TEST(geometry_applies_configure_on_commit)
{
    WindowGeometry geometry({800, 600});
    geometry.configure({1024, 768}, false);
    CHECK(geometry.logicalSize() == (Extent{800, 600}));
    CHECK(geometry.commit());
    CHECK(geometry.logicalSize() == (Extent{1024, 768}));
    CHECK(!geometry.commit());
}

TEST(geometry_zero_size_keeps_our_choice)
{
    WindowGeometry geometry({800, 600});
    geometry.configure({0, 0}, false);
    CHECK(!geometry.commit());
    CHECK(geometry.logicalSize() == (Extent{800, 600}));
    geometry.configure({0, 500}, false);
    geometry.commit();
    CHECK(geometry.logicalSize() == (Extent{800, 500}));
}

TEST(geometry_restores_windowed_size_after_fullscreen)
{
    WindowGeometry geometry({800, 600});
    geometry.configure({1920, 1080}, true);
    geometry.commit();
    CHECK(geometry.logicalSize() == (Extent{1920, 1080}));
    geometry.configure({0, 0}, false);
    CHECK(geometry.commit());
    CHECK(geometry.logicalSize() == (Extent{800, 600}));
}

TEST(geometry_scales_framebuffer)
{
    WindowGeometry geometry({800, 600});
    CHECK(geometry.framebufferSize() == (Extent{800, 600}));
    CHECK(geometry.setScale(2));
    CHECK(geometry.framebufferSize() == (Extent{1600, 1200}));
    CHECK(!geometry.setScale(2));
    CHECK(geometry.setScale(0));
    CHECK_EQ(geometry.scale120(), 120);
}

TEST(geometry_supports_fractional_scales)
{
    WindowGeometry geometry({2560, 1440});
    CHECK(geometry.setScale120(180));
    CHECK_EQ(geometry.scale(), 1.5);
    CHECK(geometry.framebufferSize() == (Extent{3840, 2160}));
    geometry.configure({1001, 3}, false);
    geometry.commit();
    CHECK(geometry.framebufferSize() == (Extent{1502, 5})); // 1501.5 and 4.5 round up
    geometry.setScale120(90);
    CHECK(geometry.framebufferSize() == (Extent{751, 2}));
}

TEST(geometry_clamps_bad_initial_size)
{
    WindowGeometry geometry({0, -5});
    CHECK(geometry.logicalSize() == (Extent{1, 1}));
}

TEST(key_repeat_waits_for_delay_then_repeats_at_rate)
{
    KeyRepeat repeat;
    repeat.setInfo(10, 500);
    auto t0 = KeyRepeat::Clock::time_point{} + 1h;
    repeat.press(KEY_A, t0);
    CHECK_EQ(repeat.due(t0 + 499ms), 0);
    CHECK_EQ(repeat.due(t0 + 500ms), 1);
    CHECK_EQ(repeat.due(t0 + 550ms), 0);
    CHECK_EQ(repeat.due(t0 + 600ms), 1);
    CHECK_EQ(repeat.due(t0 + 850ms), 2);
    CHECK(repeat.nextDeadline() == t0 + 900ms);
}

TEST(key_repeat_stops_on_release_of_that_key_only)
{
    KeyRepeat repeat;
    repeat.setInfo(10, 100);
    auto t0 = KeyRepeat::Clock::time_point{} + 1h;
    repeat.press(KEY_A, t0);
    repeat.press(KEY_E, t0);
    repeat.release(KEY_A);
    CHECK(repeat.key() == KEY_E);
    repeat.release(KEY_E);
    CHECK(!repeat.key());
    CHECK_EQ(repeat.due(t0 + 1s), 0);
    CHECK(!repeat.nextDeadline());
}

TEST(key_repeat_rate_zero_disables_and_bursts_are_capped)
{
    KeyRepeat repeat;
    repeat.setInfo(0, 100);
    auto t0 = KeyRepeat::Clock::time_point{} + 1h;
    repeat.press(KEY_A, t0);
    CHECK(!repeat.key());

    repeat.setInfo(20, 100);
    repeat.press(KEY_A, t0);
    CHECK_EQ(repeat.due(t0 + 1h), 20);
}

namespace {

Keyboard usKeyboard(std::string_view variant = {})
{
    auto keyboard = Keyboard::fromNames("us", variant, "en_US.UTF-8");
    if (!keyboard)
        lt::fail(keyboard.error(), std::source_location::current());
    return std::move(*keyboard);
}

} // namespace

TEST(keyboard_translates_scancodes)
{
    Keyboard keyboard = usKeyboard();
    KeyPress a = keyboard.press(KEY_A);
    CHECK(a.key == Key::A);
    CHECK_EQ(a.keysym, std::uint32_t{XKB_KEY_a});
    CHECK_EQ(a.text, "a");
    CHECK(a.repeats);
    CHECK(keyboard.key(KEY_1) == Key::Num1);
    CHECK(keyboard.key(KEY_LEFTSHIFT) == Key::LeftShift);
    CHECK(keyboard.key(KEY_KP7) == Key::Keypad7);
    CHECK(!keyboard.repeats(KEY_LEFTSHIFT));
}

TEST(keyboard_applies_modifiers)
{
    Keyboard keyboard = usKeyboard();
    std::uint32_t shift = keyboard.modifierMask("Shift");
    CHECK(shift != 0u);
    CHECK(keyboard.updateModifiers(shift, 0, 0, 0));
    CHECK(keyboard.modifiers().shift);
    CHECK(!keyboard.updateModifiers(shift, 0, 0, 0));
    KeyPress a = keyboard.press(KEY_A);
    CHECK(a.key == Key::A);
    CHECK_EQ(a.text, "A");
    CHECK_EQ(keyboard.press(KEY_1).text, "!");
    CHECK(keyboard.key(KEY_1) == Key::Num1);
}

TEST(keyboard_drops_control_characters_from_text)
{
    Keyboard keyboard = usKeyboard();
    CHECK(keyboard.press(KEY_ENTER).text.empty());
    CHECK(keyboard.press(KEY_BACKSPACE).text.empty());
    keyboard.updateModifiers(keyboard.modifierMask("Control"), 0, 0, 0);
    KeyPress ctrlA = keyboard.press(KEY_A);
    CHECK(ctrlA.key == Key::A);
    CHECK(ctrlA.text.empty());
}

TEST(keyboard_composes_dead_keys)
{
    Keyboard keyboard = usKeyboard("intl");
    KeyPress dead = keyboard.press(KEY_APOSTROPHE);
    CHECK(dead.text.empty());
    CHECK_EQ(keyboard.press(KEY_E).text, "\xc3\xa9"); // e acute
    CHECK_EQ(keyboard.press(KEY_E).text, "e");
}

TEST(keyboard_maps_azerty_by_label)
{
    auto keyboard = Keyboard::fromNames("fr", {}, "C");
    CHECK(keyboard.has_value());
    CHECK(keyboard->key(KEY_Q) == Key::A);
    CHECK(keyboard->key(KEY_A) == Key::Q);
    CHECK(keyboard->key(KEY_1) == Key::Num1); // shifted level of the & key
}
