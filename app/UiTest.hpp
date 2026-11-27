#pragma once

#include <span>
#include <string_view>

// lunarstone --ui-test <place> <out.png> [width height] [--no-scene] [--chat-demo] [--menu] [--player] [--tools]
// Renders a place's StarterGui ScreenGuis (plus the classic chat and core
// GUI) over the place, offscreen, into a PNG.
int uiTest(std::span<const std::string_view> args);
