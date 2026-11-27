#include "framework.hpp"

#include <lunar/assets/WebSocket.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// An actual WebSocket peer on loopback. No RTC service or account is involved.
class Peer {
public:
    // With `redirectTo`, it answers the handshake with a 301 there instead.
    explicit Peer(bool stall = false, std::string redirectTo = {}) : redirectTo_(std::move(redirectTo))
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
        CHECK_EQ(::listen(listener_, 1), 0);
        socklen_t length = sizeof address;
        CHECK_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length), 0);
        url = std::format("ws://127.0.0.1:{}/chat", ntohs(address.sin_port));
        worker_ = std::jthread([this, stall](std::stop_token stop) { serve(stop, stall); });
    }
    ~Peer()
    {
        worker_.request_stop();
        worker_.join();
        ::close(listener_);
    }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::string url;
    std::atomic<bool> accepted = false;
    std::string sentText()
    {
        const std::scoped_lock lock(mutex_);
        return text_;
    }
    std::string handshake()
    {
        const std::scoped_lock lock(mutex_);
        return handshake_;
    }

private:
    bool read(int socket, std::string& out, std::size_t count, std::stop_token stop)
    {
        while (count > 0 && !stop.stop_requested()) {
            pollfd descriptor{socket, POLLIN, 0};
            if (::poll(&descriptor, 1, 50) <= 0)
                continue;
            std::array<char, 2048> buffer{};
            const auto size = ::recv(socket, buffer.data(), std::min(count, buffer.size()), 0);
            if (size <= 0)
                return false;
            out.append(buffer.data(), static_cast<std::size_t>(size));
            count -= static_cast<std::size_t>(size);
        }
        return count == 0;
    }

    void serve(std::stop_token stop, bool stall)
    {
        int socket = -1;
        while (!stop.stop_requested()) {
            pollfd descriptor{listener_, POLLIN, 0};
            if (::poll(&descriptor, 1, 50) <= 0)
                continue;
            socket = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            break;
        }
        if (socket < 0)
            return;
        accepted = true;
        if (stall) {
            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            respond(socket, stop);
        }
        ::close(socket);
    }

    void respond(int socket, std::stop_token stop)
    {
        std::string request;
        while (!request.ends_with("\r\n\r\n"))
            if (request.size() > 8192 || !read(socket, request, 1, stop))
                return;
        {
            const std::scoped_lock lock(mutex_);
            handshake_ = request;
        }
        if (!redirectTo_.empty()) {
            const std::string response
                = std::format("HTTP/1.1 301 Moved Permanently\r\nLocation: {}\r\nContent-Length: 0\r\n\r\n", redirectTo_);
            (void)::send(socket, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        const auto keyStart = request.find("Sec-WebSocket-Key: ");
        if (keyStart == std::string::npos)
            return;
        const auto valueStart = keyStart + std::string_view("Sec-WebSocket-Key: ").size();
        const std::string key = request.substr(valueStart, request.find("\r\n", valueStart) - valueStart)
                                + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::array<unsigned char, 20> hash{};
        std::size_t hashSize = hash.size();
        if (EVP_Q_digest(nullptr, "SHA1", nullptr, key.data(), key.size(), hash.data(), &hashSize) != 1)
            return;
        std::array<unsigned char, 32> accept{};
        const int acceptSize = EVP_EncodeBlock(accept.data(), hash.data(), static_cast<int>(hash.size()));
        std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
        response += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
        response += std::format(
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            std::string_view(reinterpret_cast<const char*>(accept.data()), static_cast<std::size_t>(acceptSize)));
        (void)::send(socket, response.data(), response.size(), MSG_NOSIGNAL);
        // Text split across frames with a PING between them.
        const std::string frames = std::string("\x01\x03hel\x89\x01?\x80\x02lo", 12);
        (void)::send(socket, frames.data(), frames.size(), MSG_NOSIGNAL);
        while (!stop.stop_requested()) {
            std::string header;
            if (!read(socket, header, 2, stop))
                return;
            const auto opcode = static_cast<unsigned char>(header[0]) & 0x0fu;
            const auto length = static_cast<unsigned char>(header[1]) & 0x7fu;
            if (length >= 126 || (static_cast<unsigned char>(header[1]) & 0x80u) == 0)
                return;
            std::string mask;
            std::string payload;
            if (!read(socket, mask, 4, stop) || !read(socket, payload, length, stop))
                return;
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i])
                                               ^ static_cast<unsigned char>(mask[i % 4]));
            if (opcode == 1) {
                const std::scoped_lock lock(mutex_);
                text_ = std::move(payload);
            }
        }
    }

    int listener_ = -1;
    std::string redirectTo_;
    std::mutex mutex_;
    std::string text_;
    std::string handshake_;
    std::jthread worker_;
};

} // namespace

TEST(websocket_sends_and_reassembles_fragmented_text_around_ping)
{
    Peer peer;
    auto socket = lunar::assets::connectWebSocket(peer.url, {});
    if (!socket && socket.error().contains("support"))
        SKIP(socket.error());
    CHECK(socket.has_value());
    std::string handshake = peer.handshake();
    std::ranges::transform(handshake, handshake.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(!handshake.contains("\r\nuser-agent:"));
    CHECK((*socket)->send("hello server", {}).has_value());
    std::optional<std::string> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!received || peer.sentText().empty()) && std::chrono::steady_clock::now() < deadline) {
        if (!received) {
            auto message = (*socket)->receive();
            CHECK(message.has_value());
            received = std::move(*message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_EQ(received.value_or(""), "hello");
    CHECK_EQ(peer.sentText(), "hello server");
}

TEST(websocket_connection_cancels_during_handshake)
{
    Peer peer(true);
    std::stop_source stop;
    std::expected<std::unique_ptr<lunar::assets::WebSocket>, std::string> result;
    std::jthread worker([&] { result = lunar::assets::connectWebSocket(peer.url, stop.get_token()); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!peer.accepted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto started = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    CHECK(!result);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

TEST(websocket_follows_a_redirect_to_the_new_host)
{
    // sUNC's suncws.numelon.com answers 301 with its new host's https:// URL.
    Peer target;
    std::string moved = target.url;
    moved.replace(0, std::string_view("ws://").size(), "http://");
    Peer old(false, moved);
    auto socket = lunar::assets::connectWebSocket(old.url, {});
    if (!socket && socket.error().contains("support"))
        SKIP(socket.error());
    CHECK(socket.has_value());
    CHECK(old.handshake().contains("Upgrade: websocket"));
    CHECK(target.handshake().contains("Upgrade: websocket"));
}
