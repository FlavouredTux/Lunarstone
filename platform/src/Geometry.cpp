#include <lunar/platform/Geometry.hpp>

#include <algorithm>

namespace lunar::platform {

WindowGeometry::WindowGeometry(Extent initial)
    : logical_{std::max(initial.width, 1), std::max(initial.height, 1)}, windowed_(logical_), pending_(logical_)
{
}

void WindowGeometry::configure(Extent suggested, bool constrained)
{
    pending_ = suggested;
    pendingConstrained_ = constrained;
}

bool WindowGeometry::commit()
{
    // A zero dimension means we choose: keep the current size while
    // constrained, otherwise go back to the last windowed size (e.g. after
    // leaving fullscreen).
    Extent fallback = pendingConstrained_ ? logical_ : windowed_;
    Extent next{
        .width = pending_.width > 0 ? pending_.width : fallback.width,
        .height = pending_.height > 0 ? pending_.height : fallback.height,
    };
    if (!pendingConstrained_)
        windowed_ = next;

    Extent before = framebufferSize();
    logical_ = next;
    return framebufferSize() != before;
}

bool WindowGeometry::setScale120(std::int32_t scale120)
{
    Extent before = framebufferSize();
    scale120_ = scale120 > 0 ? scale120 : 120;
    return framebufferSize() != before;
}

Extent WindowGeometry::framebufferSize() const
{
    auto scaled
        = [&](std::int32_t length) { return static_cast<std::int32_t>((std::int64_t{length} * scale120_ + 60) / 120); };
    return {scaled(logical_.width), scaled(logical_.height)};
}

} // namespace lunar::platform
