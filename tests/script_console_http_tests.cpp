// The console's web requests: game:HttpGet and game:HttpPost, and request
// (http_request, http.request). The console's threads wait for the answer
// while the game goes on; the game's own scripts don't see any of it; and no
// request carries the login.

#include "fake_web.hpp"
#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/assets/Web.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace lunar;
using namespace std::chrono_literals;

namespace {

// What the console printed, warnings and errors left out.
std::vector<std::string> printed(const Harness& h)
{
    std::vector<std::string> lines;
    for (const std::string& line : h.lines)
        if (!line.starts_with("warn: ") && !line.starts_with("error: "))
            lines.push_back(line);
    return lines;
}

// Steps the game until it has printed `count` lines; the requests answer on
// threads of their own, so this may take a few frames.
void stepUntil(Harness& h, std::size_t count)
{
    const auto give = std::chrono::steady_clock::now() + 5s;
    while (printed(h).size() < count && std::chrono::steady_clock::now() < give) {
        h.steps(1);
        std::this_thread::sleep_for(1ms);
    }
}

// A fake server whose "down" host never answers, on the harness's web.
struct Web {
    fakeweb::FakeServer server;
    assets::HttpTransport transport()
    {
        return [inner = server.transport()](
                   const assets::HttpRequest& request) -> std::expected<assets::HttpResponse, std::string> {
            if (request.url.starts_with("https://down.test/"))
                return std::unexpected("couldn't connect to down.test");
            return inner(request);
        };
    }
    void attach(Harness& h)
    {
        h.context.setWeb(std::make_shared<const assets::RobloxWeb>(transport(), fakeweb::fakeLogin()));
    }
};

bool hasHeader(const assets::HttpRequest& request, std::string_view header)
{
    return std::ranges::contains(request.headers, header);
}

} // namespace

TEST(script_console_http_get_loads_a_script_while_the_game_goes_on)
{
    Web web;
    web.server.answers["https://scripts.test/hello.lua"] = {200, "return 'hello from the web'"};
    Harness h;
    web.attach(h);
    h.console(R"lua(
        print("asked")
        print(loadstring(game:HttpGet("https://scripts.test/hello.lua"))())
        -- A dead link's page comes back too: it's loadstring that fails on it.
        local missing = game:HttpGet("https://scripts.test/missing.lua")
        print(missing:sub(1, 10), (loadstring(missing)))
    )lua");
    // The run yielded at the first request instead of holding the frame.
    CHECK_EQ(printed(h).size(), std::size_t{1});
    stepUntil(h, 3);
    const std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(lines.at(1), std::string("hello from the web"));
    CHECK_EQ(lines.at(2), std::string(R"({"errors": nil)"));

    const std::scoped_lock lock(web.server.mutex);
    CHECK_EQ(web.server.seen.size(), std::size_t{2});
    for (const assets::HttpRequest& request : web.server.seen) {
        CHECK_EQ(request.method, std::string("GET"));
        CHECK(request.followRedirects);
        CHECK(hasHeader(request, "User-Agent: Roblox/WinInet"));
        CHECK(!fakeweb::hasHeader(request, "Cookie: "));
    }
    web.server.checkNoLeaks();
}

TEST(script_console_http_get_fetches_plain_http_too)
{
    // sUNC's loadstring test fetches an API dump from http://setup.roblox.com.
    Web web;
    web.server.answers["http://setup.test/API-Dump.json"] = {200, R"({"Classes": []})"};
    Harness h;
    web.attach(h);
    h.console(R"lua(
        local dump = game:GetService("HttpService"):JSONDecode(game:HttpGet("http://setup.test/API-Dump.json"))
        print(#dump.Classes)
    )lua");
    stepUntil(h, 1);
    CHECK_EQ(printed(h), std::vector<std::string>{"0"});
    const std::scoped_lock lock(web.server.mutex);
    CHECK_EQ(web.server.seen.size(), std::size_t{1});
    CHECK(web.server.seen.at(0).plainHttp);
    CHECK(!fakeweb::hasHeader(web.server.seen.at(0), "Cookie: "));
}

TEST(script_console_http_get_keeps_what_may_be_kept_until_nocache)
{
    // As WinInet did under Roblox's HttpGet: the API dump says it's good for
    // a year, so the second fetch is the cache's; nocache asks again.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lunarstone-console-http-cache";
    std::filesystem::remove_all(dir);
    ::setenv("LUNAR_HTTP_CACHE", dir.c_str(), 1);
    Web web;
    web.server.answers["http://setup.test/version-1-API-Dump.json"]
        = {200, R"({"Classes": []})", {}, {{"cache-control", "public, max-age=31536000"}}};
    web.server.answers["https://api.test/now"] = {200, "now", {}, {{"cache-control", "no-store"}}};
    Harness h;
    web.attach(h);
    h.console(R"lua(
        local url = "http://setup.test/version-1-API-Dump.json"
        print(game:HttpGet(url), game:HttpGet(url), game:HttpGetAsync(url))
        print(game:HttpGet(url, true))
        print(game:HttpGet("https://api.test/now"), game:HttpGet("https://api.test/now"))
    )lua");
    stepUntil(h, 3);
    ::setenv("LUNAR_HTTP_CACHE", "off", 1);
    std::filesystem::remove_all(dir);
    CHECK_EQ(printed(h), (std::vector<std::string>{R"({"Classes": []} {"Classes": []} {"Classes": []})",
                                                   R"({"Classes": []})", "now now"}));
    const std::scoped_lock lock(web.server.mutex);
    CHECK_EQ(web.server.seen.size(), std::size_t{4});
}

TEST(script_console_http_post_sends_the_data)
{
    Web web;
    web.server.answers["https://api.test/echo"] = {201, "made"};
    Harness h;
    web.attach(h);
    h.console(R"lua(
        print(game:HttpPost("https://api.test/echo", '{"a":1}', "application/json"))
        print(game:HttpPostAsync("https://api.test/echo", "plain"))
    )lua");
    stepUntil(h, 2);
    CHECK_EQ(printed(h), (std::vector<std::string>{"made", "made"}));
    const std::scoped_lock lock(web.server.mutex);
    CHECK_EQ(web.server.seen.size(), std::size_t{2});
    CHECK_EQ(web.server.seen.at(0).method, std::string("POST"));
    CHECK_EQ(web.server.seen.at(0).body, std::string(R"({"a":1})"));
    CHECK(hasHeader(web.server.seen.at(0), "Content-Type: application/json"));
    CHECK(hasHeader(web.server.seen.at(1), "Content-Type: */*"));
}

TEST(script_game_scripts_have_no_http_get)
{
    Web web;
    web.server.answers["https://scripts.test/hello.lua"] = {200, "return 1"};
    Harness h;
    web.attach(h);
    // As on Roblox: no such member to a game's script, which is what a game
    // probing for an executor checks; HttpGetAsync is a real member, out of
    // its reach; and none of the request globals.
    h.run(R"lua(
        local missing = 'HttpGet is not a valid member of DataModel "' .. game:GetFullName() .. '"'
        local ok, e = pcall(function() return game.HttpGet end)
        assert(not ok and e == missing, e)
        ok, e = pcall(function() return game:HttpGet("https://scripts.test/hello.lua") end)
        assert(not ok and e == missing, e)
        ok, e = pcall(function() return game:HttpGetAsync("https://scripts.test/hello.lua") end)
        assert(not ok and e:find("lacking capability", 1, true), e)
        ok, e = pcall(function() return game.HttpPost end)
        assert(not ok)
        assert(request == nil and http_request == nil and http == nil)
    )lua");
    h.steps(3);
    const std::scoped_lock lock(web.server.mutex);
    CHECK(web.server.seen.empty());
}

TEST(script_console_request_answers_in_uncs_shape)
{
    Web web;
    web.server.answers["https://api.test/thing"]
        = {200, R"({"ok":true})", {}, {{"content-type", "application/json"}, {"x-seen", "a"}, {"x-seen", "b"}}};
    // The fake answers a request with a Cookie header from its signed-in
    // table; this one's cookie is the code's own.
    web.server.signedInAnswers["https://api.test/thing"] = web.server.answers["https://api.test/thing"];
    Harness h;
    web.attach(h);
    h.console(R"lua(
        assert(http_request == request and http.request == request)
        local answer = request({
            Url = "https://api.test/thing",
            Method = "patch",
            Headers = {Authorization = "Bearer not-a-secret", ["Content-Type"] = "application/json"},
            Cookies = {session = "abc"},
            Body = '{"n":2}',
        })
        print(answer.Success, answer.StatusCode, answer.StatusMessage, answer.Body)
        print(answer.Headers["content-type"], answer.Headers["x-seen"])
        local missing = http_request({Url = "https://api.test/nothing"})
        print(missing.Success, missing.StatusCode, missing.StatusMessage)
    )lua");
    stepUntil(h, 3);
    CHECK_EQ(printed(h),
             (std::vector<std::string>{R"(true 200 OK {"ok":true})", "application/json a, b", "false 404 Not Found"}));
    const std::scoped_lock lock(web.server.mutex);
    CHECK_EQ(web.server.seen.size(), std::size_t{2});
    const assets::HttpRequest& sent = web.server.seen.at(0);
    CHECK_EQ(sent.method, std::string("PATCH"));
    CHECK_EQ(sent.body, std::string(R"({"n":2})"));
    // The code's own headers go as given; the login never does.
    CHECK(hasHeader(sent, "Authorization: Bearer not-a-secret"));
    CHECK(hasHeader(sent, "Content-Type: application/json"));
    CHECK(hasHeader(sent, "Cookie: session=abc"));
    CHECK(hasHeader(sent, std::format("User-Agent: Lunarstone/{}", LUNAR_VERSION)));
    CHECK(hasHeader(web.server.seen.at(1), std::format("User-Agent: Lunarstone/{}", LUNAR_VERSION)));
    for (const assets::HttpRequest& request : web.server.seen)
        for (const std::string& header : request.headers)
            CHECK(!header.contains(fakeweb::fakeCookie));
}

TEST(script_console_http_errors_are_raised_where_it_called)
{
    Web web;
    Harness h;
    web.attach(h);
    h.console(R"lua(
        print(pcall(game.HttpGet, game, "https://down.test/x.lua"))
        print(pcall(request, {Url = "https://down.test/x.lua"}))
        print(pcall(game.HttpGet, game, "ftp://scripts.test/plain.lua"))
        print(pcall(request, {Url = "https://api.test/", Method = "BREW"}))
        print(pcall(request, {Url = "https://api.test/", Headers = {["X-A"] = "1\r\nX-B: 2"}}))
        print(pcall(request, {Method = "GET"}))
    )lua");
    stepUntil(h, 6);
    CHECK_EQ(printed(h), (std::vector<std::string>{
                             "false HttpGet failed: couldn't connect to down.test",
                             "false request failed: couldn't connect to down.test",
                             "false HttpGet: only http:// and https:// URLs are fetched",
                             "false request: BREW isn't a method this sends",
                             R"(false request: "X-A" isn't a header this sends)",
                             "false request: Url is missing",
                         }));
    const std::scoped_lock lock(web.server.mutex);
    CHECK(web.server.seen.empty()); // down.test never reaches the server; the rest never went
}

TEST(script_console_http_get_blocks_where_the_thread_cant_wait)
{
    Web web;
    web.server.answers["https://scripts.test/hello.lua"] = {200, "hello"};
    Harness h;
    web.attach(h);
    // A metamethod's call can't yield: there it answers at once, holding the
    // frame for the request instead.
    h.console(R"lua(
        local fetch = setmetatable({}, {__index = function(_, url) return game:HttpGet(url) end})
        print(fetch["https://scripts.test/hello.lua"])
    )lua");
    CHECK_EQ(printed(h), (std::vector<std::string>{"hello"}));
}

TEST(script_console_http_get_outlived_by_its_request)
{
    // The game goes while a request is still out: the request's thread
    // finishes on its own afterwards and touches nothing that went.
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::promise<void> answered;
    std::future<void> done = answered.get_future();
    auto slow = [released, finished = std::make_shared<std::promise<void>>(std::move(answered))](
                    const assets::HttpRequest&) -> std::expected<assets::HttpResponse, std::string> {
        released.wait();
        finished->set_value();
        return assets::HttpResponse{.status = 200, .body = "late"};
    };
    {
        Harness h;
        h.context.setWeb(std::make_shared<const assets::RobloxWeb>(slow, fakeweb::fakeLogin()));
        h.console(R"lua(print(game:HttpGet("https://slow.test/")))lua");
        h.steps(3);
        CHECK(printed(h).empty());
    }
    release.set_value();
    CHECK(done.wait_for(5s) == std::future_status::ready);
    std::this_thread::sleep_for(20ms); // past the result's store, for ASan to see it
}
