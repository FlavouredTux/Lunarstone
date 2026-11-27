#pragma once

#include <lunar/assets/WebSocket.hpp>
#include <lunar/ui/ClassicChat.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Asteroide's public RTC protocol, as used by its web and native clients:
// https://rtc.asteroide.cc/
// https://github.com/asteroide-development/Asteroide/blob/master/src/main/java/spigey/asteroide/utils/ws.java
std::string rtcInit(std::string_view username);
std::string rtcMessage(std::string_view text);
std::string rtcPlainText(std::string_view text);
// The 30-second heartbeat has no args; a users-event reply includes a nonce.
std::string rtcPing(bool available, std::optional<std::uint32_t> nonce = std::nullopt);

// Connects only when the RTC tab is selected. Network work stays on its own
// thread; poll() delivers messages on the UI thread. This source has no access
// to the game's ScriptContext or Roblox credentials.
class RtcChatSource final : public lunar::ui::ChatSource {
public:
    struct Options {
        std::string url = "wss://rtc.asteroide.fun/asws?version=0.2.4";
        // Optional custom name; otherwise the joined Roblox username wins,
        // with a guest name as fallback. rtcInit marks it as unverified.
        std::string username;
    };
    using Connector = std::function<std::expected<std::unique_ptr<lunar::assets::WebSocket>, std::string>(
        std::string_view, std::stop_token)>;

    RtcChatSource();
    explicit RtcChatSource(Options options, Connector connect = lunar::assets::connectWebSocket);
    ~RtcChatSource() override;
    RtcChatSource(const RtcChatSource&) = delete;
    RtcChatSource& operator=(const RtcChatSource&) = delete;

    void activate() override;
    void poll() override;
    void send(std::string_view text) override;
    bool connected() const;
    // The joined Player.Name, supplied when replication has made it available.
    // An explicitly chosen name takes priority until /name reset.
    void setRobloxUsername(std::string_view username);
    std::string_view username() const;
    // The game is loaded, the chat is shown, and its window has focus.
    void setAvailable(bool available);

private:
    void restart();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
