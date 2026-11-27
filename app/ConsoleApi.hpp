#pragma once

// --api [port] (on --play and --net-play): the menu's Luau tab over HTTP, so
// a browser page (or curl) can run code in the game and follow what the
// console prints; tools/console.html is that page. Off unless asked for: what
// it runs has the console's full access, and any page the browser opens can
// reach 127.0.0.1.
//
// Protocol, version 1. Plain HTTP/1.1 on 127.0.0.1 only (port 34890 unless
// --api says another), one request a connection (Connection: close), JSON
// answers in UTF-8.
//
//   The token. Every request but a CORS preflight carries
//       Authorization: Bearer <token>
//   GET /v1/lines and GET /v1/output take ?token=<token> in its place, since
//   EventSource can't set headers. The token is the one line in
//   $XDG_CONFIG_HOME/lunarstone/api-token (~/.config/lunarstone/api-token),
//   64 hex digits made on the first --api and kept (mode 0600, the folder
//   0700), so a page can remember it; delete the file for a new one. No
//   token or the wrong one: 401. The Host header has to be 127.0.0.1:<port>
//   or localhost:<port>, else 403: a page on a name that it points at
//   127.0.0.1 (DNS rebinding) doesn't get in.
//
//   GET /v1/status
//       {"executor":"Lunarstone","version":"0.1.0","session":"9f3c08d2a1b4e675",
//        "ready":true,"seq":42}
//       session: 16 hex digits, new each time Lunarstone starts (not on a
//       teleport), so a page that kept its place knows its line numbers are
//       from another run; ready: there's a client to run code in (not while
//       the game is still joining, nor between games on a teleport); seq:
//       the latest line's number (see the lines below).
//
//   POST /v1/run
//       The Luau source as the body (Content-Type text/plain, or none), or
//       {"code":"..."} with Content-Type: application/json. It runs in the
//       console on the game's next frame as code typed at its prompt does
//       (echoed as command lines, though not kept in the prompt's history):
//           200 {"ok":true}
//           200 {"ok":false,"error":"..."}  an error it hit right away (a
//               syntax error, say); what it raises later is a console line
//           503 {"ok":false,"error":"..."}  nothing to run it in (yet)
//           504 {"ok":false,"error":"..."}  the game didn't get to it within
//               5 seconds, and now it won't run
//
//   GET /v1/lines?since=N
//       {"seq":44,"lines":[{"seq":43,"kind":"output","text":"hi"},
//                          {"seq":44,"kind":"command","text":"> print(1)"}]}
//       The lines after number N (none or 0: all) that the console still
//       has: it keeps its last 2000. kind is output, info, warning, error or
//       command (the echo of code that ran, "> " before its first line and
//       "  " before the others). Poll with since = the seq of the last answer.
//
//   GET /v1/output
//       Server-Sent Events (text/event-stream): every line, as it comes, as
//           id: 43
//           data: {"seq":43,"kind":"output","text":"hi"}
//       starting after Last-Event-ID (which EventSource sends by itself when it
//       reconnects), else ?since=N, else with every line kept. A ": keepalive"
//       comment every 15 seconds. A client that falls megabytes behind is
//       dropped (EventSource reconnects and catches up from its last id).
//
//   Line numbers start at 1 with the first line the session's console had
//   and go up by one a line; the console's Clear takes a number of its own
//   that no line has, and after a teleport the new game's console carries on
//   from where the last one stopped.
//
//   Errors are {"ok":false,"error":"..."}: 400 a request that isn't one, 401,
//   403, 404, 405, 408 a request that took more than 10 seconds to arrive,
//   413 a body over 1 MiB, 431 headers over 16 KiB, 501 a chunked body (send
//   a Content-Length), 503 16 connections already open.
//
//   CORS: since the token is what lets a request in, every answer has
//   Access-Control-Allow-Origin: *. An OPTIONS preflight gets 204 with GET,
//   POST and OPTIONS, the Authorization and Content-Type headers, a Max-Age
//   of 600 and Access-Control-Allow-Private-Network: true (Chrome's preflight
//   for a page on a public origin calling localhost).

#include <lunar/ui/DevConsole.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class ConsoleApi {
public:
    static constexpr std::uint16_t defaultPort = 34890;

    // Listens on 127.0.0.1:port (0: any free one, see port()) on a thread of
    // its own, for requests that carry `token`. No console to begin with:
    // attach one.
    static std::expected<std::unique_ptr<ConsoleApi>, std::string> start(std::uint16_t port, std::string token);
    // Stops listening and drops every connection.
    ~ConsoleApi();
    ConsoleApi(const ConsoleApi&) = delete;
    ConsoleApi& operator=(const ConsoleApi&) = delete;

    std::uint16_t port() const;

    // The rest is for the main thread, the one that runs the game's scripts.

    // The console requests go to from now on; null for none for the time
    // being (a teleport's new window brings a console of its own), when
    // run requests get 503 straight away.
    void attach(lunar::ui::DevConsole* console);
    // Once a frame: runs the code that came in since the last one, in the
    // order it came, and answers its requests. Never waits on a client.
    void pump();

    // attach(nullptr) when it goes: made after what owns the console (the
    // window), it goes first, however the play loop ends.
    class Detach {
    public:
        explicit Detach(ConsoleApi* api) : api_(api) {}
        ~Detach()
        {
            if (api_)
                api_->attach(nullptr);
        }
        Detach(const Detach&) = delete;
        Detach& operator=(const Detach&) = delete;

    private:
        ConsoleApi* api_;
    };

private:
    struct Server;
    explicit ConsoleApi(std::unique_ptr<Server> server);
    std::unique_ptr<Server> server_;
};

// Where the token is kept: $XDG_CONFIG_HOME/lunarstone/api-token when that's
// an absolute path, else ~/.config/lunarstone/api-token; empty with neither.
std::filesystem::path apiTokenFile();

// The token in `file`, made (32 random bytes in hex, mode 0600, its folder
// made 0700) when there's no file yet. One that others may read is refused,
// as the cookie file is.
std::expected<std::string, std::string> loadApiToken(const std::filesystem::path& file);

// The token from apiTokenFile(), then start(), with the line that says where
// it listens (never the token itself).
std::expected<std::unique_ptr<ConsoleApi>, std::string> startConsoleApi(std::uint16_t port);

// --api [port] for --play and --net-play: true when args[i] was it (i past
// its port, when one follows). error says what's wrong with a bad port.
bool parseApiFlag(std::span<const std::string_view> args, std::size_t& i, std::optional<std::uint16_t>& port,
                  std::string& error);
