#include "ConsoleApi.hpp"

#include <lunar/assets/Json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <print>
#include <random>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using lunar::ui::DevConsole;

constexpr std::size_t maxHeaderBytes = 16 * 1024;
constexpr std::size_t maxBodyBytes = 1024 * 1024;
constexpr std::size_t maxConnections = 16;
// What an event stream may have waiting to go out before its client counts
// as gone: a few megabytes of lines it isn't reading.
constexpr std::size_t maxStreamBacklog = 4 * 1024 * 1024;
// What a closed connection reads (and throws away) of what its client still
// sends, at most.
constexpr std::size_t maxDrainBytes = 2 * 1024 * 1024;
constexpr auto requestTimeout = std::chrono::seconds(10); // the whole request, from the connect
constexpr auto writeTimeout = std::chrono::seconds(10);   // the whole answer
constexpr auto runTimeout = std::chrono::seconds(5);      // code waiting for a frame
constexpr auto lingerTimeout = std::chrono::seconds(2);
constexpr auto keepaliveEvery = std::chrono::seconds(15);
// How often an event stream looks for new lines: the console's add() has no
// hook (it's on every script's print), so this thread asks.
constexpr auto linePoll = std::chrono::milliseconds(50);

// --- JSON and HTTP text ---------------------------------------------------

// A JSON string: quotes, backslashes and control characters escaped, every
// other byte as it is (UTF-8 stays UTF-8; what isn't is the reader's to
// replace).
void appendJsonString(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            else
                out += c;
        }
    }
    out += '"';
}

std::string_view kindName(DevConsole::Kind kind)
{
    switch (kind) {
    case DevConsole::Kind::Output:
        return "output";
    case DevConsole::Kind::Info:
        return "info";
    case DevConsole::Kind::Warning:
        return "warning";
    case DevConsole::Kind::Error:
        return "error";
    case DevConsole::Kind::Command:
        return "command";
    }
    return "output";
}

void appendLineJson(std::string& out, std::uint64_t seq, const DevConsole::Line& line)
{
    out += std::format(R"({{"seq":{},"kind":"{}","text":)", seq, kindName(line.kind));
    appendJsonString(out, line.text);
    out += '}';
}

std::string_view reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Content Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

// Every answer is the whole of its connection, and any page may read it:
// the token, not the origin, is what lets a request in.
std::string response(int status, std::string_view body, std::string_view headers = {})
{
    // clang-format off
    return std::format("HTTP/1.1 {} {}\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: {}\r\n"
                       "Cache-Control: no-store\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "X-Content-Type-Options: nosniff\r\n"
                       "Connection: close\r\n"
                       "{}\r\n{}",
                       status, reason(status), body.size(), headers, body);
    // clang-format on
}

std::string failure(int status, std::string_view message, std::string_view headers = {})
{
    std::string body = R"({"ok":false,"error":)";
    appendJsonString(body, message);
    body += '}';
    return response(status, body, headers);
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameText(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

template <class T>
std::optional<T> parseNumber(std::string_view text)
{
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

// Compares all of a and b whatever they hold, so how long it takes says
// nothing of how much of a guessed token was right (only its length).
bool sameSecret(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = static_cast<unsigned char>(
            difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return difference == 0;
}

// The value of `name` in a query string, %XX and + decoded.
std::optional<std::string> queryValue(std::string_view query, std::string_view name)
{
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        const std::size_t equals = pair.find('=');
        if (pair.substr(0, equals) != name)
            continue;
        const std::string_view raw = equals == std::string_view::npos ? std::string_view() : pair.substr(equals + 1);
        std::string value;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '+') {
                value += ' ';
            } else if (raw[i] == '%' && i + 2 < raw.size()) {
                unsigned byte = 0;
                const auto [end, error] = std::from_chars(raw.data() + i + 1, raw.data() + i + 3, byte, 16);
                if (error != std::errc() || end != raw.data() + i + 3)
                    return std::nullopt;
                value += static_cast<char>(byte);
                i += 2;
            } else {
                value += raw[i];
            }
        }
        return value;
    }
    return std::nullopt;
}

struct Request {
    std::string method, path, query;
    std::optional<std::string> host, authorization, lastEventId;
    std::string contentType;
    std::optional<std::size_t> contentLength;
    bool chunked = false;
    bool expectContinue = false;
};

// The request line and the headers this server reads, from the head (what
// comes before the blank line); null for what isn't HTTP/1.x, or says one of
// them twice.
std::optional<Request> parseHead(std::string_view head)
{
    Request request;
    std::size_t lineEnd = head.find("\r\n");
    const std::string_view line = head.substr(0, lineEnd);
    const std::size_t space = line.find(' ');
    const std::size_t space2 = space == std::string_view::npos ? space : line.find(' ', space + 1);
    if (space2 == std::string_view::npos)
        return std::nullopt;
    const std::string_view target = line.substr(space + 1, space2 - space - 1);
    if (space == 0 || target.empty() || target.front() != '/' || !line.substr(space2 + 1).starts_with("HTTP/1."))
        return std::nullopt;
    request.method = line.substr(0, space);
    const std::size_t question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string_view::npos)
        request.query = target.substr(question + 1);

    auto once = [](std::optional<std::string>& field, std::string_view value) {
        if (field)
            return false;
        field = std::string(value);
        return true;
    };
    for (std::size_t at = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2; at < head.size();) {
        lineEnd = head.find("\r\n", at);
        const std::string_view field = head.substr(at, lineEnd - at);
        at = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
        const std::size_t colon = field.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;
        const std::string_view name = field.substr(0, colon);
        if (name.find_first_of(" \t") != std::string_view::npos) // folded lines too
            return std::nullopt;
        const std::string_view value = trim(field.substr(colon + 1));
        if (sameText(name, "Host")) {
            if (!once(request.host, value))
                return std::nullopt;
        } else if (sameText(name, "Authorization")) {
            if (!once(request.authorization, value))
                return std::nullopt;
        } else if (sameText(name, "Last-Event-ID")) {
            if (!once(request.lastEventId, value))
                return std::nullopt;
        } else if (sameText(name, "Content-Type")) {
            request.contentType = value;
        } else if (sameText(name, "Content-Length")) {
            const auto length = parseNumber<std::size_t>(value);
            if (!length || (request.contentLength && *request.contentLength != *length))
                return std::nullopt;
            request.contentLength = length;
        } else if (sameText(name, "Transfer-Encoding")) {
            request.chunked = true; // whatever it says, there's no Content-Length to go by
        } else if (sameText(name, "Expect")) {
            request.expectContinue = sameText(value, "100-continue");
        }
    }
    return request;
}

// "Bearer <token>", the scheme in any case.
std::optional<std::string_view> bearer(std::string_view value)
{
    constexpr std::string_view scheme = "Bearer ";
    if (value.size() <= scheme.size() || !sameText(value.substr(0, scheme.size()), scheme))
        return std::nullopt;
    return trim(value.substr(scheme.size()));
}

// --- Code for the main thread ---------------------------------------------

// Code a request brought, waiting for the main thread's pump, and the
// answer once it ran there.
struct RunRequest {
    enum class State : std::uint8_t { Queued, Taken, Answered, Abandoned };
    std::string code;
    State state = State::Queued;
    int status = 0;
    std::string body;
};

enum class Endpoint : std::uint8_t { None, Status, Run, Lines, Output };

Endpoint endpointOf(std::string_view path)
{
    if (path == "/v1/status")
        return Endpoint::Status;
    if (path == "/v1/run")
        return Endpoint::Run;
    if (path == "/v1/lines")
        return Endpoint::Lines;
    if (path == "/v1/output")
        return Endpoint::Output;
    return Endpoint::None;
}

struct Connection {
    enum class Phase : std::uint8_t {
        Request,   // reading it
        Running,   // its code waits for the main thread
        Answering, // writing the answer
        Streaming, // GET /v1/output, for as long as the client stays
        // Answered: reading what the client still sends until it closes, as
        // closing on unread input resets the connection, which can take the
        // answer with it before the client has read it (a 413, say).
        Lingering,
    };
    int fd = -1;
    Phase phase = Phase::Request;
    Clock::time_point deadline;
    std::string in;
    std::optional<Request> request; // once its head is in
    Endpoint endpoint = Endpoint::None;
    std::size_t bodyAt = 0; // where the body starts in `in`
    std::string out;
    std::size_t drained = 0; // Lingering: bytes read and dropped
    std::shared_ptr<RunRequest> run;
    std::uint64_t sent = 0; // Streaming: the last line number written
    Clock::time_point keepalive;
};

} // namespace

struct ConsoleApi::Server {
    int listenFd = -1;
    int wakeFd = -1; // an eventfd: stop, or an answer is ready
    std::uint16_t port = 0;
    std::string token;
    std::string session; // 16 hex digits, new each run

    // The console requests go to, and the numbers its predecessors used up.
    // The main thread changes them; this thread reads the console's lines.
    std::mutex consoleMutex;
    DevConsole* console = nullptr;
    std::uint64_t base = 0;
    // The console attached before this one, its base and how many lines it
    // had when it went: only ever compared, never read (it may be gone).
    const DevConsole* previous = nullptr;
    std::uint64_t previousBase = 0, previousAdded = 0;
    // Whether the console had code to run things with at the last pump: its
    // run belongs to the main thread.
    std::atomic<bool> ready = false;

    std::mutex runMutex;
    std::deque<std::shared_ptr<RunRequest>> queue;

    std::jthread thread; // last: it starts with the rest made

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server()
    {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
        if (listenFd >= 0)
            ::close(listenFd);
        if (wakeFd >= 0)
            ::close(wakeFd);
    }

    void wake() const
    {
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t written = ::write(wakeFd, &one, sizeof one);
    }

    std::uint64_t seq()
    {
        const std::scoped_lock lock(consoleMutex);
        return base + (console ? console->added() : 0);
    }

    // DevConsole::linesSince in this server's numbers.
    DevConsole::Recent linesSince(std::uint64_t since)
    {
        const std::scoped_lock lock(consoleMutex);
        DevConsole::Recent recent;
        if (console)
            recent = console->linesSince(since > base ? since - base : 0);
        recent.added += base;
        return recent;
    }

    // Answered from the main thread (pump, attach).
    static void finish(RunRequest& run, int status, std::string body)
    {
        run.status = status;
        run.body = std::move(body);
        run.state = RunRequest::State::Answered;
    }

    void loop(const std::stop_token& stop);
    void accept(std::vector<Connection>& connections) const;
    void receive(Connection& c);
    static void send(Connection& c);
    static void close(Connection& c);
    void answer(Connection& c, std::string text) const;
    void readRequest(Connection& c);
    bool admit(Connection& c);
    void dispatch(Connection& c);
    void checkRuns(std::vector<Connection>& connections);
    void stream(std::vector<Connection>& connections);
};

void ConsoleApi::Server::loop(const std::stop_token& stop)
{
    const std::stop_callback onStop(stop, [this] { wake(); });
    std::vector<Connection> connections;
    std::vector<pollfd> fds;
    while (!stop.stop_requested()) {
        fds.clear();
        fds.push_back({.fd = listenFd, .events = POLLIN, .revents = 0});
        fds.push_back({.fd = wakeFd, .events = POLLIN, .revents = 0});
        bool streaming = false;
        for (const Connection& c : connections) {
            short events = 0;
            switch (c.phase) {
            case Connection::Phase::Request:
                events = static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)); // a 100 Continue
                break;
            case Connection::Phase::Running:
                break; // hang-ups and errors come anyway
            case Connection::Phase::Answering:
                events = POLLOUT;
                break;
            case Connection::Phase::Streaming:
                events = static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT));
                streaming = true;
                break;
            case Connection::Phase::Lingering:
                events = POLLIN;
                break;
            }
            fds.push_back({.fd = c.fd, .events = events, .revents = 0});
        }
        // Event streams look for new lines every linePoll; otherwise only the
        // deadlines need the clock, and nothing at all with no one connected.
        const int timeout = streaming ? static_cast<int>(linePoll.count()) : connections.empty() ? -1 : 100;
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout) < 0 && errno != EINTR) {
            std::println(stderr, "api: poll: {}", std::strerror(errno));
            break;
        }
        if (stop.stop_requested())
            break;
        if (fds[1].revents & POLLIN) {
            std::uint64_t count = 0;
            [[maybe_unused]] const ssize_t got = ::read(wakeFd, &count, sizeof count);
        }
        for (std::size_t i = 0; i < connections.size(); ++i) {
            Connection& c = connections[i];
            const short revents = fds[i + 2].revents;
            // A hang-up while its code waits: the code may still run, but no
            // one is there for the answer.
            if ((revents & (POLLERR | POLLNVAL)) || ((revents & POLLHUP) && c.phase == Connection::Phase::Running)) {
                close(c);
                continue;
            }
            if (revents & (POLLIN | POLLHUP))
                receive(c);
            if (c.fd >= 0 && (revents & POLLOUT))
                send(c);
        }
        if (fds[0].revents & POLLIN)
            accept(connections);
        checkRuns(connections);
        stream(connections);
        const auto now = Clock::now();
        for (Connection& c : connections) {
            if (c.fd < 0 || now < c.deadline)
                continue;
            if (c.phase == Connection::Phase::Request)
                answer(c, failure(408, "the request took more than 10 seconds to arrive"));
            else if (c.phase == Connection::Phase::Answering || c.phase == Connection::Phase::Lingering)
                close(c);
        }
        std::erase_if(connections, [](const Connection& c) { return c.fd < 0; });
    }
    for (Connection& c : connections)
        close(c);
}

void ConsoleApi::Server::accept(std::vector<Connection>& connections) const
{
    for (;;) {
        const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return; // EAGAIN: no more for now; anything else is the client's own trouble
        if (connections.size() >= maxConnections) {
            const std::string busy = failure(503, "16 connections are open already");
            [[maybe_unused]] const ssize_t put = ::send(fd, busy.data(), busy.size(), MSG_NOSIGNAL);
            ::close(fd);
            continue;
        }
        Connection& c = connections.emplace_back();
        c.fd = fd;
        c.deadline = Clock::now() + requestTimeout;
    }
}

void ConsoleApi::Server::receive(Connection& c)
{
    std::array<char, 16384> buffer{};
    while (c.fd >= 0) {
        const ssize_t got = ::recv(c.fd, buffer.data(), buffer.size(), 0);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 && errno == EAGAIN)
            return;
        if (got == 0 && (c.phase == Connection::Phase::Running || c.phase == Connection::Phase::Answering))
            return;     // it's done sending, and still waits for the answer
        if (got <= 0) { // the client went, or broke
            close(c);
            return;
        }
        const auto n = static_cast<std::size_t>(got);
        switch (c.phase) {
        case Connection::Phase::Request:
            c.in.append(buffer.data(), n);
            readRequest(c);
            break;
        case Connection::Phase::Lingering:
            c.drained += n;
            if (c.drained > maxDrainBytes)
                close(c);
            break;
        case Connection::Phase::Streaming:
            break; // nothing to read in a stream: only its end matters
        case Connection::Phase::Running:
        case Connection::Phase::Answering:
            return; // the rest waits for after the answer
        }
    }
}

void ConsoleApi::Server::send(Connection& c)
{
    while (!c.out.empty()) {
        const ssize_t put = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (put < 0 && errno == EINTR)
            continue;
        if (put < 0 && errno == EAGAIN)
            return;
        if (put < 0) {
            close(c);
            return;
        }
        c.out.erase(0, static_cast<std::size_t>(put));
    }
    if (c.phase == Connection::Phase::Answering) {
        ::shutdown(c.fd, SHUT_WR);
        c.phase = Connection::Phase::Lingering;
        c.deadline = Clock::now() + lingerTimeout;
    }
}

void ConsoleApi::Server::close(Connection& c)
{
    if (c.fd >= 0)
        ::close(c.fd);
    c.fd = -1;
}

void ConsoleApi::Server::answer(Connection& c, std::string text) const
{
    c.out += text;
    c.phase = Connection::Phase::Answering;
    c.deadline = Clock::now() + writeTimeout;
    c.in = {};
    send(c);
}

void ConsoleApi::Server::readRequest(Connection& c)
{
    if (!c.request) {
        const std::size_t end = c.in.find("\r\n\r\n");
        if (end == std::string::npos ? c.in.size() > maxHeaderBytes : end + 4 > maxHeaderBytes) {
            answer(c, failure(431, "the request's headers are over 16 KiB"));
            return;
        }
        if (end == std::string::npos)
            return; // more to come
        auto request = parseHead(std::string_view(c.in).substr(0, end));
        if (!request) {
            answer(c, failure(400, "that isn't an HTTP/1.1 request this server reads"));
            return;
        }
        c.request = std::move(*request);
        c.bodyAt = end + 4;
        if (!admit(c))
            return;
    }
    // Only code comes with a body; one sent with anything else is let be.
    if (c.endpoint == Endpoint::Run && c.in.size() - c.bodyAt < c.request->contentLength.value_or(0))
        return; // more to come
    dispatch(c);
}

// What the head alone decides: whose request it is and whether it's one this
// server answers. False when that's answered it already.
bool ConsoleApi::Server::admit(Connection& c)
{
    const Request& request = *c.request;
    // A page on another name that resolves to 127.0.0.1 (DNS rebinding) is
    // same-origin with whatever it finds here, so the name has to be ours.
    const std::string_view host = request.host ? std::string_view(*request.host) : std::string_view();
    const std::string suffix = std::format(":{}", port);
    if (!host.ends_with(suffix)
        || (host.substr(0, host.size() - suffix.size()) != "127.0.0.1"
            && !sameText(host.substr(0, host.size() - suffix.size()), "localhost"))) {
        answer(c, failure(403, std::format("Host has to be 127.0.0.1:{} or localhost:{}", port, port)));
        return false;
    }
    // A CORS preflight, which can't carry the token: what the real request
    // may do. Private-Network is Chrome's for a public page calling localhost.
    if (request.method == "OPTIONS") {
        // clang-format off
        answer(c, "HTTP/1.1 204 No Content\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                  "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
                  "Access-Control-Allow-Private-Network: true\r\n"
                  "Access-Control-Max-Age: 600\r\n"
                  "Content-Length: 0\r\n"
                  "Connection: close\r\n\r\n");
        // clang-format on
        return false;
    }
    c.endpoint = endpointOf(request.path);
    // EventSource can't set headers: the two that a page reads that way take
    // the token in the query instead.
    std::optional<std::string_view> given;
    if (request.authorization)
        given = bearer(*request.authorization);
    std::optional<std::string> fromQuery;
    if (!request.authorization && (c.endpoint == Endpoint::Lines || c.endpoint == Endpoint::Output)) {
        fromQuery = queryValue(request.query, "token");
        given = fromQuery;
    }
    if (!given || !sameSecret(*given, token)) {
        answer(c, failure(401, "this needs Authorization: Bearer <the token in the api-token file>",
                          "WWW-Authenticate: Bearer\r\n"));
        return false;
    }
    if (c.endpoint == Endpoint::None) {
        answer(c, failure(404, std::format("no {} here: /v1/status, /v1/run, /v1/lines, /v1/output", request.path)));
        return false;
    }
    const std::string_view method = c.endpoint == Endpoint::Run ? "POST" : "GET";
    if (request.method != method) {
        answer(c, failure(405, std::format("{} takes {}", request.path, method), std::format("Allow: {}\r\n", method)));
        return false;
    }
    if (request.chunked) {
        answer(c, failure(501, "send the body with a Content-Length"));
        return false;
    }
    if (c.endpoint == Endpoint::Run) {
        if (request.contentLength.value_or(0) > maxBodyBytes) {
            answer(c, failure(413, "the code is over 1 MiB"));
            return false;
        }
        if (request.expectContinue) // curl's way with a body over a kilobyte
            c.out += "HTTP/1.1 100 Continue\r\n\r\n";
    }
    return true;
}

void ConsoleApi::Server::dispatch(Connection& c)
{
    const Request& request = *c.request;
    // Line numbers past the latest one are from an earlier Lunarstone's
    // console (a page that kept its place): this one's lines are all new.
    auto since = [&](std::optional<std::string> text) -> std::optional<std::uint64_t> {
        if (!text)
            return 0;
        const auto number = parseNumber<std::uint64_t>(*text);
        if (!number)
            return std::nullopt;
        return *number > seq() ? 0 : *number;
    };
    switch (c.endpoint) {
    case Endpoint::Status: {
        const std::string body
            = std::format(R"({{"executor":"Lunarstone","version":"{}","session":"{}","ready":{},"seq":{}}})",
                          LUNAR_VERSION, session, ready ? "true" : "false", seq());
        answer(c, response(200, body));
        return;
    }
    case Endpoint::Lines: {
        const auto from = since(queryValue(request.query, "since"));
        if (!from) {
            answer(c, failure(400, "since is a line number"));
            return;
        }
        const DevConsole::Recent recent = linesSince(*from);
        std::string body = std::format(R"({{"seq":{},"lines":[)", recent.added);
        const std::uint64_t first = recent.added - recent.lines.size() + 1;
        for (std::size_t i = 0; i < recent.lines.size(); ++i) {
            if (i > 0)
                body += ',';
            appendLineJson(body, first + i, recent.lines[i]);
        }
        body += "]}";
        answer(c, response(200, body));
        return;
    }
    case Endpoint::Output: {
        // Where EventSource left off when it reconnects, else where the page said.
        auto from = request.lastEventId ? since(request.lastEventId) : std::nullopt;
        if (!from)
            from = since(queryValue(request.query, "since"));
        if (!from) {
            answer(c, failure(400, "since is a line number"));
            return;
        }
        // clang-format off
        c.out += "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-store\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Connection: close\r\n\r\n";
        // clang-format on
        c.phase = Connection::Phase::Streaming;
        c.sent = *from;
        c.keepalive = Clock::now() + keepaliveEvery;
        c.in = {};
        send(c);
        return; // the lines go out with the next stream()
    }
    case Endpoint::Run: {
        std::string code = c.in.substr(c.bodyAt, request.contentLength.value_or(0));
        const std::string_view type
            = trim(std::string_view(request.contentType).substr(0, request.contentType.find(';')));
        if (sameText(type, "application/json")) {
            const auto json = lunar::assets::json::parse(code);
            const lunar::assets::json::Value* field = json ? (*json)["code"] : nullptr;
            if (!field || !field->string()) {
                answer(c, failure(400, R"(a JSON body is {"code":"..."})"));
                return;
            }
            code = *field->string();
        }
        if (code.empty()) {
            answer(c, failure(400, "there's no code in the request"));
            return;
        }
        {
            // With the console held, so a teleport's attach() either sees it
            // queued (and answers it) or comes first (and this says so).
            const std::scoped_lock lock(consoleMutex);
            if (console) {
                const std::scoped_lock queued(runMutex);
                c.run = std::make_shared<RunRequest>();
                c.run->code = std::move(code);
                queue.push_back(c.run);
            }
        }
        if (!c.run) {
            answer(c, failure(503, "There are no client scripts to run this in."));
            return;
        }
        c.phase = Connection::Phase::Running;
        c.deadline = Clock::now() + runTimeout;
        c.in = {};
        return;
    }
    case Endpoint::None:
        break;
    }
    answer(c, failure(404, "no such endpoint"));
}

void ConsoleApi::Server::checkRuns(std::vector<Connection>& connections)
{
    const auto now = Clock::now();
    for (Connection& c : connections) {
        if (c.fd < 0 || c.phase != Connection::Phase::Running)
            continue;
        std::optional<std::string> done;
        {
            const std::scoped_lock lock(runMutex);
            if (c.run->state == RunRequest::State::Answered) {
                done = response(c.run->status, c.run->body);
            } else if (c.run->state == RunRequest::State::Queued && now >= c.deadline) {
                // Not taken in time (a frame that long, or a window that
                // stopped drawing): it won't run now, whatever comes later.
                std::erase(queue, c.run);
                c.run->state = RunRequest::State::Abandoned;
                done = failure(504, "the game didn't get to it within 5 seconds; it didn't run");
            }
        }
        if (done) {
            c.run.reset();
            answer(c, std::move(*done));
        }
    }
}

void ConsoleApi::Server::stream(std::vector<Connection>& connections)
{
    std::uint64_t from = std::numeric_limits<std::uint64_t>::max();
    for (const Connection& c : connections)
        if (c.fd >= 0 && c.phase == Connection::Phase::Streaming)
            from = std::min(from, c.sent);
    if (from == std::numeric_limits<std::uint64_t>::max())
        return;
    const auto now = Clock::now();
    if (from < seq()) {
        // One copy of the lines for every stream, each line's event made once.
        const DevConsole::Recent recent = linesSince(from);
        const std::uint64_t first = recent.added - recent.lines.size() + 1;
        std::vector<std::string> events(recent.lines.size());
        for (std::size_t i = 0; i < recent.lines.size(); ++i) {
            events[i] = std::format("id: {}\ndata: ", first + i);
            appendLineJson(events[i], first + i, recent.lines[i]);
            events[i] += "\n\n";
        }
        for (Connection& c : connections) {
            if (c.fd < 0 || c.phase != Connection::Phase::Streaming || c.sent >= recent.added)
                continue;
            if (c.out.size() > maxStreamBacklog) { // not reading: gone, as far as this is concerned
                close(c);
                continue;
            }
            for (std::size_t i = c.sent >= first ? c.sent - first + 1 : 0; i < events.size(); ++i)
                c.out += events[i];
            c.sent = recent.added;
            c.keepalive = now + keepaliveEvery;
            send(c);
        }
    }
    for (Connection& c : connections) {
        if (c.fd >= 0 && c.phase == Connection::Phase::Streaming && now >= c.keepalive) {
            c.out += ": keepalive\n\n";
            c.keepalive = now + keepaliveEvery;
            send(c);
        }
    }
}

ConsoleApi::ConsoleApi(std::unique_ptr<Server> server) : server_(std::move(server)) {}

ConsoleApi::~ConsoleApi() = default;

std::expected<std::unique_ptr<ConsoleApi>, std::string> ConsoleApi::start(std::uint16_t port, std::string token)
{
    // An empty token would match an empty Bearer.
    if (token.size() < 16)
        return std::unexpected("api: the token is too short (16 characters at least)");
    auto server = std::make_unique<Server>();
    server->token = std::move(token);
    // A page that kept its place tells this run from the last one by it: the
    // line numbers start over at 1 and may well pass where it stopped.
    std::random_device random;
    server->session = std::format("{:08x}{:08x}", random(), random());
    server->listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server->listenFd < 0)
        return std::unexpected(std::format("api: socket: {}", std::strerror(errno)));
    // Another run's connections in TIME_WAIT don't hold the port; a listener
    // does still.
    const int on = 1;
    ::setsockopt(server->listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7f000001u); // 127.0.0.1 alone: nothing from another machine
    address.sin_port = htons(port);
    if (::bind(server->listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        const int error = errno;
        return std::unexpected(std::format("api: 127.0.0.1:{}: {}{}", port, std::strerror(error),
                                           error == EADDRINUSE ? " (another --api? --api <port> takes another)" : ""));
    }
    if (::listen(server->listenFd, static_cast<int>(maxConnections)) != 0)
        return std::unexpected(std::format("api: listen: {}", std::strerror(errno)));
    socklen_t length = sizeof address;
    if (::getsockname(server->listenFd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return std::unexpected(std::format("api: getsockname: {}", std::strerror(errno)));
    server->port = ntohs(address.sin_port);
    server->wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (server->wakeFd < 0)
        return std::unexpected(std::format("api: eventfd: {}", std::strerror(errno)));
    server->thread = std::jthread([s = server.get()](const std::stop_token& stop) { s->loop(stop); });
    return std::unique_ptr<ConsoleApi>(new ConsoleApi(std::move(server)));
}

std::uint16_t ConsoleApi::port() const
{
    return server_->port;
}

void ConsoleApi::attach(DevConsole* console)
{
    Server& s = *server_;
    bool orphans = false;
    {
        const std::scoped_lock lock(s.consoleMutex);
        if (s.console == console)
            return;
        const std::uint64_t used = s.base + (s.console ? s.console->added() : 0);
        if (s.console) {
            s.previous = s.console;
            s.previousBase = s.base;
            s.previousAdded = s.console->added();
        }
        // The console that went, back again (--net-play's window keeps one
        // for every game, so a teleport detaches it and attaches it again):
        // its lines keep their numbers and only what came in between is new.
        // Any other console's lines all come after the last number used.
        const bool returning = console && console == s.previous && console->added() >= s.previousAdded;
        s.base = returning ? s.previousBase : used;
        s.console = console;
        // Code sent to the game that's going isn't for the next one.
        const std::scoped_lock queued(s.runMutex);
        for (const auto& run : s.queue)
            Server::finish(*run, 503, R"({"ok":false,"error":"The game it was sent to has gone."})");
        orphans = !s.queue.empty();
        s.queue.clear();
    }
    s.ready = console && console->run;
    if (orphans)
        s.wake();
}

void ConsoleApi::pump()
{
    Server& s = *server_;
    s.ready = s.console && s.console->run;
    std::vector<std::shared_ptr<RunRequest>> taken;
    {
        const std::scoped_lock lock(s.runMutex);
        if (s.queue.empty())
            return;
        taken.assign(s.queue.begin(), s.queue.end());
        s.queue.clear();
        for (const auto& run : taken)
            run->state = RunRequest::State::Taken;
    }
    for (const auto& run : taken) {
        // As the console's prompt runs code (DevConsole::submit), but for
        // the history, which is the prompt's own.
        int status = 200;
        std::string body = R"({"ok":true})";
        if (!s.console || !s.console->run) {
            status = 503;
            body = R"({"ok":false,"error":"There are no client scripts to run this in."})";
        } else {
            s.console->echo(run->code);
            if (std::string error = s.console->run(run->code); !error.empty()) {
                s.console->add(DevConsole::Kind::Error, error);
                body = R"({"ok":false,"error":)";
                appendJsonString(body, error);
                body += '}';
            }
        }
        const std::scoped_lock lock(s.runMutex);
        Server::finish(*run, status, std::move(body));
    }
    s.wake();
}

// --- The token ------------------------------------------------------------

std::filesystem::path apiTokenFile()
{
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && config[0] == '/')
        return std::filesystem::path(config) / "lunarstone/api-token";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config/lunarstone/api-token";
    return {};
}

namespace {

// What a token may hold: RFC 6750's b64token, as it goes in a header as it is.
bool tokenCharacter(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || std::string_view("-._~+/=").contains(c);
}

// The token in a file that's there; null when there's no file.
std::expected<std::optional<std::string>, std::string> readTokenFile(const std::filesystem::path& file)
{
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return std::optional<std::string>();
        return std::unexpected(std::format("api: {}: {}", file.string(), std::strerror(errno)));
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(fd);
        return std::unexpected(std::format("api: {} isn't a file", file.string()));
    }
    if ((status.st_mode & 077) != 0) {
        ::close(fd);
        return std::unexpected(
            std::format("api: {} may be read by others: chmod 600 it (or delete it for a new token)", file.string()));
    }
    std::string text(256, '\0');
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t got = ::read(fd, text.data() + done, text.size() - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        done += static_cast<std::size_t>(got);
    }
    ::close(fd);
    text.resize(done);
    const std::string_view token = trim(std::string_view(text).substr(0, text.find_first_of("\r\n")));
    if (token.size() < 32 || !std::ranges::all_of(token, tokenCharacter))
        return std::unexpected(
            std::format("api: {} doesn't hold a token (64 hex digits): delete it for a new one", file.string()));
    return std::optional<std::string>(token);
}

} // namespace

std::expected<std::string, std::string> loadApiToken(const std::filesystem::path& file)
{
    auto existing = readTokenFile(file);
    if (!existing)
        return std::unexpected(existing.error());
    if (*existing)
        return std::move(**existing);

    // None yet: 32 random bytes, in a folder only we can open.
    const std::filesystem::path folder = file.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(folder.parent_path(), ec);
    if (::mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST)
        return std::unexpected(std::format("api: {}: {}", folder.string(), std::strerror(errno)));
    std::array<unsigned char, 32> bytes{};
    for (std::size_t done = 0; done < bytes.size();) {
        const ssize_t got = ::getrandom(bytes.data() + done, bytes.size() - done, 0);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return std::unexpected(std::format("api: getrandom: {}", std::strerror(errno)));
        done += static_cast<std::size_t>(got);
    }
    std::string token;
    for (const unsigned char byte : bytes)
        token += std::format("{:02x}", byte);

    // Written beside it and linked into place, so two windows starting at
    // once end up with the same one.
    std::string temporary = (folder / ".api-token.XXXXXX").string();
    const int fd = ::mkstemp(temporary.data()); // mode 0600
    if (fd < 0)
        return std::unexpected(std::format("api: {}: {}", folder.string(), std::strerror(errno)));
    const std::string line = token + "\n";
    const bool written
        = ::fchmod(fd, 0600) == 0 && ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    ::close(fd);
    const int linked = written ? ::link(temporary.c_str(), file.c_str()) : -1;
    const int error = errno;
    ::unlink(temporary.c_str());
    if (linked != 0 && written && error == EEXIST) { // the other window's
        auto theirs = readTokenFile(file);
        if (!theirs)
            return std::unexpected(theirs.error());
        if (*theirs)
            return std::move(**theirs);
    }
    if (linked != 0)
        return std::unexpected(std::format("api: {}: {}", file.string(), std::strerror(error)));
    return token;
}

std::expected<std::unique_ptr<ConsoleApi>, std::string> startConsoleApi(std::uint16_t port)
{
    const std::filesystem::path file = apiTokenFile();
    if (file.empty())
        return std::unexpected("api: no HOME (nor XDG_CONFIG_HOME) to keep the token in");
    auto token = loadApiToken(file);
    if (!token)
        return std::unexpected(token.error());
    auto api = ConsoleApi::start(port, std::move(*token));
    if (api)
        std::println("api: http://127.0.0.1:{} (token in {})", (*api)->port(), file.string());
    return api;
}

bool parseApiFlag(std::span<const std::string_view> args, std::size_t& i, std::optional<std::uint16_t>& port,
                  std::string& error)
{
    if (args[i] != "--api")
        return false;
    port = ConsoleApi::defaultPort;
    // The port is optional: a number after the flag is it.
    if (i + 1 < args.size() && !args[i + 1].empty()
        && std::ranges::all_of(args[i + 1], [](char c) { return c >= '0' && c <= '9'; })) {
        const std::string_view value = args[++i];
        const auto number = parseNumber<std::uint16_t>(value);
        if (!number)
            error = std::format("--api {}: a port, up to 65535 (0: any free one)", value);
        else
            port = *number;
    }
    return true;
}
