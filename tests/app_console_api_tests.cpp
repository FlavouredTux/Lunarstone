// The Luau tab's console over HTTP (app/ConsoleApi): real sockets to a
// server on a free port, a console whose run records the code it's given,
// and the test's own thread as the game's main thread, which pumps.

#include "framework.hpp"

#include "ConsoleApi.hpp"

#include <lunar/ui/DevConsole.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using lunar::ui::DevConsole;

namespace {

constexpr std::string_view token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string authorization = std::format("Authorization: Bearer {}\r\n", token);

std::unique_ptr<ConsoleApi> startApi()
{
    auto api = ConsoleApi::start(0, std::string(token));
    if (!api)
        lt::fail(api.error(), std::source_location::current());
    CHECK((*api)->port() != 0);
    return std::move(*api);
}

// A socket to the server, with timeouts so a server that doesn't answer
// fails the test instead of hanging it.
int connectTo(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const timeval timeout{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7f000001u);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        ::close(fd);
        lt::fail("can't connect to the api", std::source_location::current());
    }
    return fd;
}

void sendAll(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t put = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (put <= 0)
            return;
        bytes.remove_prefix(static_cast<std::size_t>(put));
    }
}

// What the server sends until it closes, or until `until` shows up.
std::string readFrom(int fd, std::string_view until = {})
{
    std::string got;
    std::array<char, 65536> buffer{};
    while (until.empty() || !got.contains(until)) {
        const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0)
            break;
        got.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return got;
}

struct Reply {
    int status = 0;
    std::string head; // the status line and the headers
    std::string body;
};

Reply roundTrip(std::uint16_t port, std::string_view request)
{
    const int fd = connectTo(port);
    sendAll(fd, request);
    const std::string raw = readFrom(fd);
    ::close(fd);
    Reply reply;
    const std::size_t end = raw.find("\r\n\r\n");
    reply.head = raw.substr(0, end);
    if (end != std::string::npos)
        reply.body = raw.substr(end + 4);
    if (raw.starts_with("HTTP/1.1 ") && raw.size() >= 12)
        reply.status = std::stoi(raw.substr(9, 3));
    return reply;
}

// A request as a browser's fetch() makes it, Host and all.
std::string request(std::uint16_t port, std::string_view method, std::string_view target, std::string_view headers = {},
                    std::string_view body = {})
{
    return std::format("{} {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n{}{}\r\n{}", method, target, port, headers,
                       method == "POST" ? std::format("Content-Length: {}\r\n", body.size()) : std::string(), body);
}

Reply get(std::uint16_t port, std::string_view target, std::string_view headers = authorization)
{
    return roundTrip(port, request(port, "GET", target, headers));
}

// The request from another thread while this one pumps, as the game's frames
// would.
Reply whilePumping(ConsoleApi& api, const std::function<Reply()>& ask)
{
    auto reply = std::async(std::launch::async, ask);
    while (reply.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
        api.pump();
    return reply.get();
}

// A console whose run keeps the code it's given, and on which thread, and
// fails code that calls error().
struct FakeGame {
    DevConsole console;
    std::vector<std::string> ran;
    std::thread::id ranOn;

    FakeGame()
    {
        console.run = [this](const std::string& code) -> std::string {
            ran.push_back(code);
            ranOn = std::this_thread::get_id();
            return code.contains("error(") ? "console:1: boom" : "";
        };
    }
};

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / std::format("lunarstone-api-{}-{}", stamp, next++);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

unsigned modeOf(const std::filesystem::path& path)
{
    struct stat status{};
    ::stat(path.c_str(), &status);
    return status.st_mode & 0777;
}

} // namespace

TEST(app_console_api_needs_the_token)
{
    const auto api = startApi();
    const std::uint16_t port = api->port();

    const Reply none = get(port, "/v1/status", "");
    CHECK_EQ(none.status, 401);
    CHECK(none.head.contains("WWW-Authenticate: Bearer"));
    CHECK(none.head.contains("Access-Control-Allow-Origin: *")); // a page can tell it's the token
    std::string wrong(token);
    wrong.back() = '0';
    CHECK_EQ(get(port, "/v1/status", std::format("Authorization: Bearer {}\r\n", wrong)).status, 401);
    CHECK_EQ(get(port, "/v1/status", std::format("Authorization: Bearer {}\r\n", token.substr(1))).status, 401);
    CHECK_EQ(get(port, "/v1/status", std::format("Authorization: Basic {}\r\n", token)).status, 401);
    CHECK_EQ(get(port, "/v1/status").status, 200);
    CHECK_EQ(get(port, "/v1/status", std::format("authorization: bearer {}\r\n", token)).status, 200);

    // ?token= only where EventSource needs it.
    CHECK_EQ(get(port, std::format("/v1/status?token={}", token), "").status, 401);
    CHECK_EQ(get(port, std::format("/v1/lines?token={}", token), "").status, 200);
    CHECK_EQ(get(port, std::format("/v1/output?token={}", wrong), "").status, 401);

    // What isn't there, only once the token's right.
    CHECK_EQ(get(port, "/v1/nothing", "").status, 401);
    CHECK_EQ(get(port, "/v1/nothing").status, 404);
    const Reply method = get(port, "/v1/run");
    CHECK_EQ(method.status, 405);
    CHECK(method.head.contains("Allow: POST"));
    CHECK(method.body.starts_with(R"({"ok":false,"error":)"));
}

TEST(app_console_api_checks_the_host)
{
    const auto api = startApi();
    const std::uint16_t port = api->port();
    auto withHost = [&](std::string_view host) {
        return roundTrip(port, std::format("GET /v1/status HTTP/1.1\r\n{}{}\r\n", host, authorization)).status;
    };
    CHECK_EQ(withHost(std::format("Host: 127.0.0.1:{}\r\n", port)), 200);
    CHECK_EQ(withHost(std::format("Host: localhost:{}\r\n", port)), 200);
    CHECK_EQ(withHost(std::format("Host: LocalHost:{}\r\n", port)), 200);
    // A name rebound to 127.0.0.1, another port, no Host at all, or two.
    CHECK_EQ(withHost(std::format("Host: evil.example:{}\r\n", port)), 403);
    CHECK_EQ(withHost(std::format("Host: localhost.evil.example:{}\r\n", port)), 403);
    CHECK_EQ(withHost(std::format("Host: 127.0.0.1:{}\r\n", port == 1 ? 2 : 1)), 403);
    CHECK_EQ(withHost("Host: 127.0.0.1\r\n"), 403);
    CHECK_EQ(withHost(""), 403);
    CHECK_EQ(withHost(std::format("Host: 127.0.0.1:{}\r\nHost: evil.example:{}\r\n", port, port)), 400);
    // Preflights too: the rebound page's would come with its name.
    CHECK_EQ(roundTrip(port, std::format("OPTIONS /v1/run HTTP/1.1\r\nHost: evil.example:{}\r\n\r\n", port)).status,
             403);
}

TEST(app_console_api_answers_preflights)
{
    const auto api = startApi();
    const std::uint16_t port = api->port();
    // As Chrome sends it for a page on a public origin: no token, of course.
    // clang-format off
    const std::string_view asks = "Origin: https://example.com\r\n"
                                  "Access-Control-Request-Method: POST\r\n"
                                  "Access-Control-Request-Headers: authorization,content-type\r\n"
                                  "Access-Control-Request-Private-Network: true\r\n";
    // clang-format on
    const Reply reply = roundTrip(port, request(port, "OPTIONS", "/v1/run", asks));
    CHECK_EQ(reply.status, 204);
    CHECK(reply.head.contains("Access-Control-Allow-Origin: *"));
    CHECK(reply.head.contains("Access-Control-Allow-Methods: GET, POST, OPTIONS"));
    CHECK(reply.head.contains("Access-Control-Allow-Headers: Authorization, Content-Type"));
    CHECK(reply.head.contains("Access-Control-Allow-Private-Network: true"));
    CHECK(reply.head.contains("Access-Control-Max-Age: 600"));
    CHECK(reply.body.empty());
}

TEST(app_console_api_reports_its_status)
{
    FakeGame game;
    const auto api = startApi();
    const std::uint16_t port = api->port();

    Reply status = get(port, "/v1/status");
    CHECK_EQ(status.status, 200);
    CHECK(status.head.contains("Content-Type: application/json"));
    CHECK(status.body.starts_with(R"({"executor":"Lunarstone","version":")"));
    CHECK(status.body.ends_with(R"(","ready":false,"seq":0})"));

    game.console.add(DevConsole::Kind::Output, "a");
    game.console.add(DevConsole::Kind::Output, "b");
    api->attach(&game.console);
    api->pump();
    CHECK(get(port, "/v1/status").body.ends_with(R"(","ready":true,"seq":2})"));
    // A game without a client to run in.
    game.console.run = nullptr;
    api->pump();
    CHECK(get(port, "/v1/status").body.ends_with(R"(","ready":false,"seq":2})"));
}

TEST(app_console_api_names_its_session)
{
    // 16 hex digits, the same for the whole run, another for the next one.
    auto session = [](std::uint16_t port) {
        const std::string body = get(port, "/v1/status").body;
        const std::string_view key = R"("session":")";
        const std::size_t at = body.find(key);
        return at == std::string::npos ? std::string() : body.substr(at + key.size(), 16);
    };
    const auto api = startApi();
    const std::string first = session(api->port());
    CHECK_EQ(first.size(), std::size_t{16});
    CHECK(std::ranges::all_of(first, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK(get(api->port(), "/v1/status").body.contains(std::format(R"("session":"{}","ready")", first)));
    CHECK_EQ(session(api->port()), first);
    const auto next = startApi();
    CHECK(session(next->port()) != first);
}

TEST(app_console_api_runs_code_on_the_main_thread)
{
    FakeGame game;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    api->attach(&game.console);
    auto run = [&](std::string_view headers, std::string_view body) {
        return whilePumping(*api, [&] { return roundTrip(port, request(port, "POST", "/v1/run", headers, body)); });
    };

    // The source as it is, echoed as typed code is, but kept out of the prompt's history.
    Reply ran = run(authorization + "Content-Type: text/plain;charset=UTF-8\r\n", "print(1)\nprint(2)");
    CHECK_EQ(ran.status, 200);
    CHECK_EQ(ran.body, std::string(R"({"ok":true})"));
    CHECK_EQ(game.ran.size(), std::size_t{1});
    CHECK_EQ(game.ran.back(), std::string("print(1)\nprint(2)"));
    CHECK(game.ranOn == std::this_thread::get_id());
    std::vector<DevConsole::Line> lines = game.console.lines();
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK(lines[0].kind == DevConsole::Kind::Command);
    CHECK_EQ(lines[0].text, std::string("> print(1)"));
    CHECK_EQ(lines[1].text, std::string("  print(2)"));
    CHECK(game.console.history().empty());

    // Or in JSON.
    ran = run(authorization + "Content-Type: application/json; charset=utf-8\r\n",
              R"({"code":"local x = \"hi\"\nprint(x) -- \u00e9"})");
    CHECK_EQ(ran.body, std::string(R"({"ok":true})"));
    CHECK_EQ(game.ran.back(), std::string("local x = \"hi\"\nprint(x) -- é"));

    // An error it hits right away comes back, and goes in the console as typed code's does.
    ran = run(authorization, "error('boom')");
    CHECK_EQ(ran.status, 200);
    CHECK_EQ(ran.body, std::string(R"({"ok":false,"error":"console:1: boom"})"));
    lines = game.console.lines();
    CHECK(lines.back().kind == DevConsole::Kind::Error);
    CHECK_EQ(lines.back().text, std::string("console:1: boom"));

    // curl's way with a body over a kilobyte: it waits for the go-ahead.
    const std::string padded = "print(3)" + std::string(2000, ' ');
    const int fd = connectTo(port);
    const std::string head = std::format("POST /v1/run HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n{}", port, authorization);
    sendAll(fd, head + std::format("Expect: 100-continue\r\nContent-Length: {}\r\n\r\n", padded.size()));
    CHECK_EQ(readFrom(fd, "\r\n\r\n"), std::string("HTTP/1.1 100 Continue\r\n\r\n"));
    auto rest = std::async(std::launch::async, [&] {
        sendAll(fd, padded);
        return readFrom(fd);
    });
    while (rest.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
        api->pump();
    CHECK(rest.get().starts_with("HTTP/1.1 200 OK\r\n"));
    ::close(fd);
    CHECK_EQ(game.ran.back(), padded);

    // What isn't code is answered without the game.
    const std::size_t count = game.ran.size();
    const std::string json = authorization + "Content-Type: application/json\r\n";
    CHECK_EQ(roundTrip(port, request(port, "POST", "/v1/run", json, R"json({"source":"print(1)"})json")).status, 400);
    CHECK_EQ(roundTrip(port, request(port, "POST", "/v1/run", json, "print(1)")).status, 400);
    CHECK_EQ(roundTrip(port, request(port, "POST", "/v1/run", authorization, "")).status, 400);
    CHECK_EQ(game.ran.size(), count);
}

TEST(app_console_api_without_a_client)
{
    FakeGame game;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    const std::string run = request(port, "POST", "/v1/run", authorization, "print(1)");

    // No console yet (the game is still joining): no need to wait for a frame.
    Reply reply = roundTrip(port, run);
    CHECK_EQ(reply.status, 503);
    CHECK(reply.body.starts_with(R"({"ok":false,"error":)"));

    // A console with nothing to run in: the frame says so, and the console shows nothing.
    game.console.run = nullptr;
    api->attach(&game.console);
    reply = whilePumping(*api, [&] { return roundTrip(port, run); });
    CHECK_EQ(reply.status, 503);
    CHECK(game.console.lines().empty());
}

TEST(app_console_api_refuses_what_is_too_big)
{
    FakeGame game;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    api->attach(&game.console);

    // The whole body sent, as fetch() would: the answer still gets through.
    const std::string over(1024 * 1024 + 1, '-');
    Reply reply = roundTrip(port, request(port, "POST", "/v1/run", authorization, over));
    CHECK_EQ(reply.status, 413);
    CHECK(reply.body.contains("1 MiB"));

    const std::string pad = std::format("X-Pad: {}\r\n", std::string(17 * 1024, 'a'));
    reply = roundTrip(port, request(port, "GET", "/v1/status", authorization + pad));
    CHECK_EQ(reply.status, 431);

    // A MiB exactly is fine.
    const std::string most(1024 * 1024, '-');
    reply = whilePumping(*api, [&] { return roundTrip(port, request(port, "POST", "/v1/run", authorization, most)); });
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(game.ran.back().size(), most.size());

    // A chunked body has no length to go by.
    const std::string chunked = std::format(
        "POST /v1/run HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n{}Transfer-Encoding: chunked\r\n\r\n", port, authorization);
    reply = roundTrip(port, chunked + "8\r\nprint(1)\r\n0\r\n\r\n");
    CHECK_EQ(reply.status, 501);
    CHECK(game.ran.size() == 1);
}

TEST(app_console_api_lists_lines_since)
{
    DevConsole console;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    api->attach(&console);
    auto lines = [&](std::string_view since) {
        return get(port, std::format("/v1/lines?since={}&token={}", since, token), "");
    };

    console.add(DevConsole::Kind::Output, "one");
    console.add(DevConsole::Kind::Warning, "two \"quoted\"\n\ttabbed \x01 \\ é");
    console.add(DevConsole::Kind::Error, "three");
    // Escaped as JSON has it, the UTF-8 left as it is.
    const std::string two = R"({"seq":2,"kind":"warning","text":"two \"quoted\"\n\ttabbed \u0001 \\ é"})";
    CHECK_EQ(lines("1").body, R"({"seq":3,"lines":[)" + two + R"(,{"seq":3,"kind":"error","text":"three"}]})");
    CHECK_EQ(lines("3").body, std::string(R"({"seq":3,"lines":[]})"));
    CHECK(get(port, "/v1/lines").body.starts_with(R"({"seq":3,"lines":[{"seq":1,"kind":"output","text":"one"},)"));
    // A number past the latest is an earlier session's: all of this one's lines.
    CHECK(lines("99").body.starts_with(R"({"seq":3,"lines":[{"seq":1,)"));
    CHECK_EQ(lines("x").status, 400);

    // Clear takes a number of its own.
    console.clear();
    console.add(DevConsole::Kind::Command, "> four");
    CHECK_EQ(lines("3").body, std::string(R"({"seq":5,"lines":[{"seq":5,"kind":"command","text":"> four"}]})"));

    // A teleport's new console goes on from there.
    DevConsole next;
    next.add(DevConsole::Kind::Info, "new game");
    api->attach(nullptr);
    CHECK_EQ(lines("0").body, std::string(R"({"seq":5,"lines":[]})"));
    api->attach(&next);
    CHECK_EQ(lines("5").body, std::string(R"({"seq":6,"lines":[{"seq":6,"kind":"info","text":"new game"}]})"));
    api->attach(nullptr);
}

TEST(app_console_api_keeps_the_numbers_of_a_console_that_comes_back)
{
    // --net-play's window keeps one console for every game it plays, so a
    // teleport detaches the same console and attaches it again: its lines
    // keep their numbers, and only what came in between is new to a page.
    DevConsole console;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    api->attach(&console);
    console.add(DevConsole::Kind::Output, "one");
    console.add(DevConsole::Kind::Output, "two");
    api->attach(nullptr);
    console.add(DevConsole::Kind::Info, "teleporting");
    api->attach(&console);
    CHECK_EQ(get(port, std::format("/v1/lines?since=2&token={}", token), "").body,
             std::string(R"({"seq":3,"lines":[{"seq":3,"kind":"info","text":"teleporting"}]})"));
    api->attach(nullptr);
}

TEST(app_console_api_streams_lines)
{
    DevConsole console;
    const auto api = startApi();
    const std::uint16_t port = api->port();
    api->attach(&console);
    console.add(DevConsole::Kind::Output, "old");

    const int stream = connectTo(port);
    sendAll(stream, request(port, "GET", std::format("/v1/output?token={}", token)));
    std::string got = readFrom(stream, "\"text\":\"old\"}\n\n");
    CHECK(got.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(got.contains("Content-Type: text/event-stream\r\n"));
    CHECK(got.contains("Access-Control-Allow-Origin: *\r\n"));
    CHECK(got.ends_with("\r\n\r\nid: 1\ndata: {\"seq\":1,\"kind\":\"output\",\"text\":\"old\"}\n\n"));

    // A line added once it's connected comes on its own.
    console.add(DevConsole::Kind::Error, "new\nline");
    got = readFrom(stream, R"("text":"new\nline"})");
    CHECK(got.contains("id: 2\ndata: {\"seq\":2,\"kind\":\"error\",\"text\":\"new\\nline\"}"));
    ::close(stream);

    // EventSource reconnecting says where it was, over the page's since.
    const int resumed = connectTo(port);
    sendAll(resumed, request(port, "GET", std::format("/v1/output?since=0&token={}", token), "Last-Event-ID: 1\r\n"));
    got = readFrom(resumed, R"("text":"new\nline"})");
    CHECK(got.contains("id: 2\n"));
    CHECK(!got.contains("old"));
    ::close(resumed);
}

TEST(app_console_api_keeps_its_token)
{
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "config/lunarstone/api-token";

    // Made the first time: 64 hex digits, only ours to read.
    const auto made = loadApiToken(file);
    CHECK(made.has_value());
    CHECK_EQ(made->size(), std::size_t{64});
    CHECK(std::ranges::all_of(*made, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK_EQ(modeOf(file), 0600u);
    CHECK_EQ(modeOf(file.parent_path()), 0700u);
    // And the same one after.
    const auto again = loadApiToken(file);
    CHECK(again.has_value());
    CHECK_EQ(*again, *made);

    // Not one others can read, nor one that isn't a token.
    ::chmod(file.c_str(), 0644);
    CHECK(!loadApiToken(file).has_value());
    {
        std::ofstream out(file, std::ios::trunc);
        out << "short\n";
    }
    ::chmod(file.c_str(), 0600);
    CHECK(!loadApiToken(file).has_value());
    // One of the user's own is theirs to pick.
    {
        std::ofstream out(file, std::ios::trunc);
        out << "my-own_token.with~all+the/chars=0123456789\n";
    }
    CHECK_EQ(loadApiToken(file).value_or(""), std::string("my-own_token.with~all+the/chars=0123456789"));

    // tests/main.cpp points XDG_CONFIG_HOME away from the real one.
    CHECK_EQ(apiTokenFile(), std::filesystem::path("/nonexistent/lunarstone-tests-config/lunarstone/api-token"));
    CHECK(!ConsoleApi::start(0, "").has_value());
}

TEST(app_console_api_flag)
{
    auto parse = [](std::vector<std::string_view> args, std::optional<std::uint16_t>& port, std::string& error) {
        std::size_t i = 0;
        const bool was = parseApiFlag(args, i, port, error);
        return std::pair{was, i};
    };
    std::optional<std::uint16_t> port;
    std::string error;
    CHECK(parse({"--api"}, port, error) == std::pair{true, std::size_t{0}});
    CHECK(port == ConsoleApi::defaultPort);
    CHECK(parse({"--api", "4000", "--mute"}, port, error) == std::pair{true, std::size_t{1}});
    CHECK(port == 4000);
    CHECK(parse({"--api", "--mute"}, port, error) == std::pair{true, std::size_t{0}});
    CHECK(port == ConsoleApi::defaultPort);
    CHECK(error.empty());
    parse({"--api", "70000"}, port, error);
    CHECK(!error.empty());
    port.reset();
    CHECK(!parse({"--mute"}, port, error).first);
    CHECK(!port);
}
