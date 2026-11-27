#pragma once

#include <cstdint>

namespace lunar::platform {

struct Extent {
    std::int32_t width = 0;
    std::int32_t height = 0;

    bool operator==(const Extent&) const = default;
};

// Size bookkeeping for an xdg_toplevel. The compositor suggests a logical size
// in xdg_toplevel.configure (0 meaning "your choice") and makes it current in
// the following xdg_surface.configure. The framebuffer is the logical size
// times the scale, which is kept in 120ths as wp_fractional_scale_v1 sends it
// (180 is 1.5x) and rounded half away from zero like the protocol says.
// Integer buffer scales are just multiples of 120.
class WindowGeometry {
public:
    explicit WindowGeometry(Extent initial);

    // xdg_toplevel.configure. constrained: maximized, fullscreen or tiled, where
    // the size is not ours to pick and shouldn't be remembered as the windowed one.
    void configure(Extent suggested, bool constrained);

    // xdg_surface.configure: applies the pending configure. True if the
    // framebuffer size changed.
    bool commit();

    // Both return true if the framebuffer size changed. Non-positive values
    // count as 1x.
    bool setScale(std::int32_t scale) { return setScale120(scale * 120); }
    bool setScale120(std::int32_t scale120);

    Extent logicalSize() const { return logical_; }
    Extent framebufferSize() const;
    double scale() const { return scale120_ / 120.0; }
    std::int32_t scale120() const { return scale120_; }

private:
    Extent logical_;
    Extent windowed_; // restored when leaving fullscreen/maximized with a 0x0 configure
    Extent pending_;
    bool pendingConstrained_ = false;
    std::int32_t scale120_ = 120;
};

} // namespace lunar::platform
