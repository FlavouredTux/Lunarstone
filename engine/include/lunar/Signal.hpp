#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace lunar {

namespace detail {
struct SlotBase {
    bool connected = true;
};
} // namespace detail

// Handle returned by Signal::connect. Copies refer to the same connection.
class Connection {
public:
    Connection() = default;

    void disconnect()
    {
        if (auto slot = slot_.lock())
            slot->connected = false;
        slot_.reset();
    }

    bool connected() const
    {
        auto slot = slot_.lock();
        return slot && slot->connected;
    }

private:
    template <class...>
    friend class Signal;

    explicit Connection(std::weak_ptr<detail::SlotBase> slot) : slot_(std::move(slot)) {}

    std::weak_ptr<detail::SlotBase> slot_;
};

// Roblox-style event (RBXScriptSignal). Handlers connected while firing run on
// the next fire; handlers disconnected while firing are skipped.
template <class... Args>
class Signal {
public:
    using Handler = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    ~Signal() { disconnectAll(); }

    Connection connect(Handler handler)
    {
        prune();
        auto slot = std::make_shared<Slot>(std::move(handler));
        slots_.push_back(slot);
        return Connection(slot);
    }

    void fire(const Args&... args) const
    {
        if (slots_.empty())
            return;
        auto snapshot = slots_;
        for (const auto& slot : snapshot)
            if (slot->connected)
                slot->handler(args...);
    }

    void disconnectAll()
    {
        for (const auto& slot : slots_)
            slot->connected = false;
        slots_.clear();
    }

    // Nothing connected (not even a handler since disconnected but not yet
    // pruned): firing it would run nothing.
    bool empty() const { return slots_.empty(); }

    std::size_t connectionCount() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(slots_, [](const auto& slot) { return slot->connected; }));
    }

private:
    struct Slot : detail::SlotBase {
        explicit Slot(Handler h) : handler(std::move(h)) {}
        Handler handler;
    };

    void prune()
    {
        std::erase_if(slots_, [](const auto& slot) { return !slot->connected; });
    }

    std::vector<std::shared_ptr<Slot>> slots_;
};

} // namespace lunar
