#include <lunar/platform/KeyRepeat.hpp>

#include <algorithm>

namespace lunar::platform {

namespace {

KeyRepeat::Clock::duration interval(std::int32_t rate)
{
    return std::chrono::duration_cast<KeyRepeat::Clock::duration>(std::chrono::seconds(1)) / rate;
}

} // namespace

void KeyRepeat::setInfo(std::int32_t rate, std::int32_t delayMs)
{
    rate_ = std::clamp(rate, 0, 1000); // keeps the interval nonzero
    delay_ = std::chrono::milliseconds(std::max(delayMs, 0));
    if (!enabled())
        stop();
}

void KeyRepeat::press(std::uint32_t scancode, Clock::time_point now)
{
    if (!enabled())
        return;
    key_ = scancode;
    next_ = now + delay_;
}

void KeyRepeat::release(std::uint32_t scancode)
{
    if (key_ == scancode)
        stop();
}

std::int32_t KeyRepeat::due(Clock::time_point now)
{
    if (!key_ || now < next_)
        return 0;
    Clock::duration step = interval(rate_);
    auto count = ((now - next_) / step) + 1;
    next_ += step * count;
    return static_cast<std::int32_t>(std::min<decltype(count)>(count, rate_));
}

std::optional<KeyRepeat::Clock::time_point> KeyRepeat::nextDeadline() const
{
    if (!key_)
        return std::nullopt;
    return next_;
}

} // namespace lunar::platform
