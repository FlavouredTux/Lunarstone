#include <lunar/net/Socket.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>

namespace lunar::net {

namespace {

sockaddr_in toSockaddr(const SystemAddress& a)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    std::memcpy(&sa.sin_addr, a.ip.data(), 4);
    sa.sin_port = htons(a.port);
    return sa;
}

SystemAddress fromSockaddr(const sockaddr_in& sa)
{
    SystemAddress a;
    std::memcpy(a.ip.data(), &sa.sin_addr, 4);
    a.port = ntohs(sa.sin_port);
    return a;
}

} // namespace

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

UdpSocket::~UdpSocket()
{
    if (fd_ >= 0)
        ::close(fd_);
}

std::expected<UdpSocket, std::string> UdpSocket::bind(std::string_view host, std::uint16_t port)
{
    UdpSocket s;
    s.fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s.fd_ < 0)
        return std::unexpected(std::format("socket: {}", std::strerror(errno)));
    // The default receive buffer (208 KiB on Linux) holds about a hundred
    // datagrams: a server's burst while joining (the game's map,
    // 140k instances) or a long frame overflowed it, the kernel dropped
    // thousands, and the server's resends and congestion window slowed the
    // join to a trickle. Ask for more; the kernel caps it at net.core.rmem_max.
    constexpr int receiveBuffer = 16 << 20;
    ::setsockopt(s.fd_, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof receiveBuffer);
    SystemAddress local;
    if (!host.empty()) {
        auto a = resolve(std::format("{}:{}", host, port));
        if (!a)
            return std::unexpected(a.error());
        local = *a;
    }
    local.port = port;
    sockaddr_in sa = toSockaddr(local);
    if (::bind(s.fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0)
        return std::unexpected(std::format("bind {}: {}", local.toString(), std::strerror(errno)));
    return s;
}

bool UdpSocket::sendTo(const SystemAddress& to, std::span<const std::uint8_t> data) const
{
    sockaddr_in sa = toSockaddr(to);
    return ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof sa)
           == static_cast<ssize_t>(data.size());
}

std::optional<std::pair<SystemAddress, std::vector<std::uint8_t>>> UdpSocket::receive() const
{
    std::vector<std::uint8_t> buf(65536);
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0)
        return std::nullopt;
    buf.resize(static_cast<std::size_t>(n));
    return std::pair{fromSockaddr(sa), std::move(buf)};
}

bool UdpSocket::wait(int ms) const
{
    pollfd p{fd_, POLLIN, 0};
    return ::poll(&p, 1, ms) > 0;
}

SystemAddress UdpSocket::localAddress() const
{
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    return fromSockaddr(sa);
}

std::expected<SystemAddress, std::string> resolve(std::string_view hostPort, std::uint16_t defaultPort)
{
    std::string host(hostPort);
    std::uint16_t port = defaultPort;
    if (auto colon = host.rfind(':'); colon != std::string::npos) {
        const std::string_view p = std::string_view(host).substr(colon + 1);
        auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), port);
        if (ec != std::errc{} || ptr != p.data() + p.size())
            return std::unexpected(std::format("bad port in '{}'", hostPort));
        host.resize(colon);
    }
    if (host.empty())
        host = "127.0.0.1";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return std::unexpected(std::format("can't resolve '{}'", host));
    sockaddr_in sin{};
    std::memcpy(&sin, res->ai_addr, sizeof sin);
    SystemAddress a = fromSockaddr(sin);
    ::freeaddrinfo(res);
    a.port = port;
    return a;
}

} // namespace lunar::net
