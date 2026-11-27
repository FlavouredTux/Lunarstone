#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace lunar::platform {

// Wayland leaves key repeat to clients: the compositor only sends the rate and
// delay (wl_keyboard.repeat_info). Only the most recently pressed key repeats.
class KeyRepeat {
public:
    using Clock = std::chrono::steady_clock;

    // rate in repeats per second, 0 disables; delay before the first repeat.
    void setInfo(std::int32_t rate, std::int32_t delayMs);

    void press(std::uint32_t scancode, Clock::time_point now);
    void release(std::uint32_t scancode); // stops only if it's the repeating key
    void stop() { key_.reset(); }

    // Repeats due by now since the last call, capped at one second's worth so
    // a stalled application doesn't get a burst.
    std::int32_t due(Clock::time_point now);

    std::optional<Clock::time_point> nextDeadline() const;
    std::optional<std::uint32_t> key() const { return key_; }
    bool enabled() const { return rate_ > 0; }

private:
    std::int32_t rate_ = 25;
    Clock::duration delay_ = std::chrono::milliseconds(600);
    std::optional<std::uint32_t> key_;
    Clock::time_point next_;
};

} // namespace lunar::platform
