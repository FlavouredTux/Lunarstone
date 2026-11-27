#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace lunar::assets {

// A text WebSocket used from one worker thread. receive() is nonblocking;
// nullopt means there is no complete message yet. A close is an error.
// No Roblox login, cookies, or other HTTP client state is shared with it.
class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual std::expected<void, std::string> send(std::string_view text, std::stop_token stop) = 0;
    virtual std::expected<std::optional<std::string>, std::string> receive() = 0;
};

std::expected<std::unique_ptr<WebSocket>, std::string> connectWebSocket(std::string_view url, std::stop_token stop);

} // namespace lunar::assets
