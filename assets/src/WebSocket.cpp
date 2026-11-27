#include <lunar/assets/WebSocket.hpp>

#if LUNAR_WITH_CURL
#include <curl/curl.h>
#endif

#if LUNAR_WITH_CURL && LIBCURL_VERSION_NUM >= 0x075600

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <poll.h>

namespace lunar::assets {
namespace {

struct EasyDeleter {
    void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
};
struct MultiDeleter {
    void operator()(CURLM* multi) const { curl_multi_cleanup(multi); }
};

class CurlWebSocket final : public WebSocket {
public:
    std::unique_ptr<CURL, EasyDeleter> easy{curl_easy_init()};
    std::unique_ptr<CURLM, MultiDeleter> multi{curl_multi_init()};
    bool added = false;

    ~CurlWebSocket() override
    {
        if (added)
            curl_multi_remove_handle(multi.get(), easy.get());
    }

    std::expected<void, std::string> send(std::string_view text, std::stop_token stop) override
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::size_t offset = 0;
        do {
            if (stop.stop_requested())
                return std::unexpected("connection stopped");
            std::size_t sent = 0;
            const CURLcode result
                = curl_ws_send(easy.get(), text.data() + offset, text.size() - offset, &sent, 0, CURLWS_TEXT);
            if (result != CURLE_OK && result != CURLE_AGAIN)
                return std::unexpected(curl_easy_strerror(result));
            offset += sent;
            if (result == CURLE_OK && offset == text.size())
                return {};
            if (std::chrono::steady_clock::now() >= deadline)
                return std::unexpected("sending timed out");
            curl_socket_t socket = CURL_SOCKET_BAD;
            curl_easy_getinfo(easy.get(), CURLINFO_ACTIVESOCKET, &socket);
            pollfd descriptor{socket, POLLOUT, 0};
            (void)::poll(&descriptor, 1, 50);
        } while (true);
    }

    std::expected<std::optional<std::string>, std::string> receive() override
    {
        // Each frame can arrive in chunks, and a text message can span frames.
        // Control frames between fragments must not end or corrupt the text.
        std::array<char, 4096> buffer{};
        for (int reads = 0; reads < 64; ++reads) {
            std::size_t count = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode result = curl_ws_recv(easy.get(), buffer.data(), buffer.size(), &count, &meta);
            if (result == CURLE_AGAIN)
                return std::nullopt;
            if (result != CURLE_OK)
                return std::unexpected(curl_easy_strerror(result));
            if (!meta)
                return std::unexpected("missing WebSocket frame metadata");
            if ((meta->flags & CURLWS_CLOSE) != 0)
                return std::unexpected("server closed the connection");
            if ((meta->flags & (CURLWS_PING | CURLWS_PONG)) != 0)
                continue; // libcurl answers PING automatically
            if ((meta->flags & CURLWS_BINARY) != 0)
                return std::unexpected("expected a text WebSocket message");
            if (pending_.size() + count > 64u * 1024u)
                return std::unexpected("RTC message is too large");
            pending_.append(buffer.data(), count);
            if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
                std::string message;
                message.swap(pending_);
                return message;
            }
        }
        return std::nullopt;
    }

private:
    std::string pending_;
};

} // namespace

namespace {

// The Location header of the handshake's answer, if it had one: curl doesn't
// keep a redirect's for a WebSocket it couldn't upgrade.
std::size_t readLocation(char* data, std::size_t size, std::size_t count, void* user)
{
    const std::string_view line(data, size * count);
    constexpr std::string_view name = "location:";
    if (line.size() > name.size()
        && std::ranges::equal(line.substr(0, name.size()), name,
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) {
        std::string_view value = line.substr(name.size());
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.remove_suffix(1);
        *static_cast<std::string*>(user) = value;
    }
    return size * count;
}

// One handshake with the address. When the server answered with a redirect
// instead, `redirect` is where it pointed.
std::expected<std::unique_ptr<WebSocket>, std::string> handshake(const std::string& address, std::stop_token stop,
                                                                 std::string& redirect)
{
    auto connection = std::make_unique<CurlWebSocket>();
    if (!connection->easy || !connection->multi)
        return std::unexpected("cannot create a WebSocket connection");
    CURL* easy = connection->easy.get();
    curl_easy_setopt(easy, CURLOPT_URL, address.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "ws,wss");
    curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(easy, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);
    std::string location;
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &readLocation);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &location);
    // The multi handle stays attached for the lifetime of the connection.
    if (curl_multi_add_handle(connection->multi.get(), easy) != CURLM_OK)
        return std::unexpected("cannot start a WebSocket connection");
    connection->added = true;
    for (int running = 1; running > 0;) {
        if (stop.stop_requested())
            return std::unexpected("connection stopped");
        if (curl_multi_perform(connection->multi.get(), &running) != CURLM_OK
            || (running > 0 && curl_multi_poll(connection->multi.get(), nullptr, 0, 100, nullptr) != CURLM_OK))
            return std::unexpected("WebSocket connection failed");
    }
    CURLcode result = CURLE_FAILED_INIT;
    int left = 0;
    while (CURLMsg* message = curl_multi_info_read(connection->multi.get(), &left))
        if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
            result = message->data.result;
    // The header callback's string goes with this frame: no more of them.
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, nullptr);
    if (result != CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 300 && status < 400)
            redirect = std::move(location);
        return std::unexpected(std::format("WebSocket: {}", curl_easy_strerror(result)));
    }
    return connection;
}

// A redirect's location as a WebSocket address: a server moved elsewhere
// names the new place by its https:// (or http://) URL.
std::optional<std::string> asWebSocket(std::string_view location)
{
    for (const auto& [from, to] : {std::pair{"https://", "wss://"}, std::pair{"http://", "ws://"},
                                   std::pair{"wss://", "wss://"}, std::pair{"ws://", "ws://"}})
        if (location.starts_with(from))
            return std::string(to) + std::string(location.substr(std::string_view(from).size()));
    return std::nullopt;
}

} // namespace

// Follows a server's redirects, as browsers' and the executors' WebSockets do
// (sUNC's suncws.numelon.com answers 301 to its new host), up to 5 of them.
std::expected<std::unique_ptr<WebSocket>, std::string> connectWebSocket(std::string_view url, std::stop_token stop)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::string address(url);
    for (int redirects = 0;; ++redirects) {
        std::string redirect;
        auto connected = handshake(address, stop, redirect);
        if (connected || redirect.empty())
            return connected;
        const std::optional<std::string> next = asWebSocket(redirect);
        if (!next)
            return std::unexpected(std::format("WebSocket: redirected to {}, which isn't a WebSocket", redirect));
        if (redirects == 5)
            return std::unexpected("WebSocket: too many redirects");
        address = *next;
    }
}

} // namespace lunar::assets

#else

namespace lunar::assets {

std::expected<std::unique_ptr<WebSocket>, std::string> connectWebSocket(std::string_view, std::stop_token)
{
    return std::unexpected("RTC needs libcurl 7.86 or newer with WebSocket support");
}

} // namespace lunar::assets

#endif
