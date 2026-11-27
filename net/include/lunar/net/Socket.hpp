#pragma once

#include <lunar/net/Offline.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// A non-blocking IPv4 UDP socket.
class UdpSocket {
public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    ~UdpSocket();

    // Binds to `host` (empty: any) and `port` (0: the system picks).
    static std::expected<UdpSocket, std::string> bind(std::string_view host, std::uint16_t port);

    bool sendTo(const SystemAddress& to, std::span<const std::uint8_t> data) const;
    // One datagram, or nullopt when none is waiting.
    std::optional<std::pair<SystemAddress, std::vector<std::uint8_t>>> receive() const;
    // Waits up to `ms` for something to read.
    bool wait(int ms) const;
    SystemAddress localAddress() const;
    bool valid() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// "host:port" (host a dotted quad or a name) to an address.
std::expected<SystemAddress, std::string> resolve(std::string_view hostPort, std::uint16_t defaultPort = 0);

} // namespace lunar::net
