#pragma once

#include <vector>

namespace lunar::platform {

// The refresh rate of each display, in hertz, in the order the compositor
// lists them (each wl_output's current mode): what vsync paces frames to.
// Asks without opening a window; empty when there's no Wayland display.
std::vector<double> displayRefreshRates();

} // namespace lunar::platform
