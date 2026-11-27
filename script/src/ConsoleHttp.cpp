// The console's web requests (Console.cpp opens them): game:HttpGet and
// game:HttpPost, which the executors add to DataModel, and request
// (http_request, http.request), UNC's general one.
//
// The game's own scripts reach none of it. HttpService stays off for them as
// on the real client, and these DataModel members are members only to the
// console's threads (InstanceApi's consoleMethods), so a game probing for
// game.HttpGet finds what it would on Roblox: no such member. The requests
// carry no login: nothing here puts the .ROBLOSECURITY cookie on them or can
// read it, so code pasted into the console can't send it anywhere.
//
// A request goes on a thread of its own while the Luau thread that made it
// waits, so the game keeps drawing; it wakes on the first Heartbeat after the
// answer came. http:// as well as https://, as the executors fetch: sUNC's
// loadstring test fetches an API dump from http://setup.roblox.com.

#include "Console.hpp"
#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Services.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/HttpCache.hpp>
#include <lunar/assets/Web.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace lunar::lua {

namespace {

// What request() says it is when the code doesn't: identifyexecutor's name
// and version, as UNC asks.
constexpr std::string_view userAgent = "Lunarstone/" LUNAR_VERSION;
// GUESS: what game:HttpGet says it is. Roblox's own HttpGet sent
// Roblox/WinInet and the executors' copies kept it; some script hosts serve
// only that.
constexpr std::string_view httpGetAgent = "Roblox/WinInet";

using Result = std::expected<assets::HttpResponse, std::string>;

// research: LUNAR_TRACE_HTTP=1 says what each request fetched and how long
// it and the wake after it took, as a notice (LogService doesn't see those).
const bool traceHttp = std::getenv("LUNAR_TRACE_HTTP") != nullptr;

using Clock = std::chrono::steady_clock;

// What the request's thread fills in, read once `done` is set.
struct Fetch {
    std::atomic<bool> done = false;
    Result result = std::unexpected(std::string());
    std::string url;                  // for the trace
    Clock::time_point start, finished; // for the trace
};

// The waiting side. When it goes (the Luau thread woke, or was killed, or the
// game went and its timers with it), the download stops where it is.
struct Waiter {
    std::shared_ptr<Fetch> fetch = std::make_shared<Fetch>();
    std::stop_source stop;
    Waiter() = default;
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    ~Waiter() { stop.request_stop(); }
};

// Pushes what the call returns for an answer; returns how many.
using Answer = int (*)(lua_State* L, const assets::HttpResponse& response);

// Each Heartbeat until the answer is in, then the waiting thread resumes with
// it, or with the error raised where it called (a pcall around it catches it).
void poll(Runtime& rt, lua_State* thread, std::uint64_t token, std::shared_ptr<Waiter> waiter, Answer answer,
          std::string_view what)
{
    if (!rt.isSuspended(thread, token))
        return; // killed while it waited
    if (!waiter->fetch->done.load(std::memory_order_acquire)) {
        rt.after(0.0, [&rt, thread, token, waiter = std::move(waiter), answer, what]() mutable {
            poll(rt, thread, token, std::move(waiter), answer, what);
        });
        return;
    }
    const Result& result = waiter->fetch->result;
    if (traceHttp) {
        const Fetch& fetch = *waiter->fetch;
        const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
        rt.notice(MessageType::Info,
                  std::format("trace: {} {} -> {} in {:.3f} s, resumed {:.3f} s later", what, fetch.url,
                              result ? std::format("{}, {} bytes", result->status, result->body.size()) : result.error(),
                              seconds(fetch.finished - fetch.start), seconds(Clock::now() - fetch.finished)));
    }
    if (!result)
        rt.fail(thread, token, std::format("{} failed: {}", what, result.error()), what);
    else
        rt.wake(thread, token, [&](lua_State* t) { return answer(t, *result); }, what);
}

bool hasScheme(std::string_view url, std::string_view scheme)
{
    return url.size() > scheme.size() && std::ranges::equal(url.substr(0, scheme.size()), scheme, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

// The transport fetches the web alone (no file://, no ftp://); said here, so
// the message is plain.
bool web(std::string_view url)
{
    return hasScheme(url, "https://") || hasScheme(url, "http://");
}

int send(lua_State* L, const assets::HttpTransport& transport, assets::HttpRequest request, Answer answer,
         std::string_view what)
{
    if (!web(request.url))
        raise(L, std::format("{}: only http:// and https:// URLs are fetched", what));
    request.plainHttp = true;
    // Where the thread can't wait (inside a metamethod, say), the frame does
    // instead, for as long as the request takes.
    if (!lua_isyieldable(L)) {
        const Result result = transport(request);
        if (!result)
            raise(L, std::format("{} failed: {}", what, result.error()));
        return answer(L, *result);
    }
    Runtime& rt = Runtime::of(L);
    auto waiter = std::make_shared<Waiter>();
    if (traceHttp)
        waiter->fetch->url = request.url;
    waiter->fetch->start = Clock::now();
    try {
        std::thread([fetch = waiter->fetch, stop = waiter->stop.get_token(), transport, request = std::move(request)] {
            const assets::HttpStopScope scope(stop);
            fetch->result = transport(request);
            fetch->finished = Clock::now();
            fetch->done.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error& e) {
        raise(L, std::format("{} failed: {}", what, e.what()));
    }
    // The first look is on the next Heartbeat: never before this thread yields.
    const std::uint64_t token = rt.suspend(L);
    rt.after(0.0, [&rt, L, token, waiter = std::move(waiter), answer, what]() mutable {
        poll(rt, L, token, std::move(waiter), answer, what);
    });
    return lua_yield(L, 0);
}

int pushBody(lua_State* L, const assets::HttpResponse& response)
{
    pushString(L, response.body);
    return 1;
}

std::string_view statusMessage(long status)
{
    static constexpr std::array<std::pair<long, std::string_view>, 24> known{{
        {200, "OK"},
        {201, "Created"},
        {202, "Accepted"},
        {204, "No Content"},
        {206, "Partial Content"},
        {301, "Moved Permanently"},
        {302, "Found"},
        {303, "See Other"},
        {304, "Not Modified"},
        {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},
        {400, "Bad Request"},
        {401, "Unauthorized"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {409, "Conflict"},
        {413, "Payload Too Large"},
        {429, "Too Many Requests"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
    }};
    for (const auto& [code, message] : known)
        if (code == status)
            return message;
    return {};
}

// UNC's response: Success (a 2xx), StatusCode, StatusMessage, Headers (names
// lower-cased; repeats joined with ", ") and Body.
int pushResponse(lua_State* L, const assets::HttpResponse& response)
{
    lua_createtable(L, 0, 5);
    lua_pushboolean(L, response.status >= 200 && response.status < 300);
    lua_setfield(L, -2, "Success");
    lua_pushinteger(L, static_cast<int>(response.status));
    lua_setfield(L, -2, "StatusCode");
    pushString(L, statusMessage(response.status));
    lua_setfield(L, -2, "StatusMessage");
    lua_createtable(L, 0, static_cast<int>(response.headers.size()));
    for (const auto& [name, value] : response.headers) {
        pushString(L, name);
        if (lua_rawget(L, -2) == LUA_TSTRING) {
            std::string joined = lua_tostring(L, -1);
            joined += ", ";
            joined += value;
            lua_pop(L, 1);
            pushString(L, joined);
        } else {
            lua_pop(L, 1);
            pushString(L, value);
        }
        pushString(L, name);
        lua_insert(L, -2);
        lua_rawset(L, -3);
    }
    lua_setfield(L, -2, "Headers");
    pushString(L, response.body);
    lua_setfield(L, -2, "Body");
    return 1;
}

// A header the code gives can't end itself early and start another.
bool oneLine(std::string_view text)
{
    return text.find_first_of("\r\n") == std::string_view::npos;
}

// game:HttpGet(url [, nocache]): the body of what the URL answers. GUESS:
// whatever its status, a 404's page too, as the executors' does (it's why a
// dead link fails in loadstring and not here); an error only when nothing
// answered. Through the HTTP cache, as Roblox's went through WinInet's: an
// answer whose headers say it may be kept comes from there while it's
// fresh, unless nocache, which fetches it again (and keeps that).
int httpGet(lua_State* L)
{
    self(L);
    assets::HttpRequest request;
    request.url = checkString(L, 2);
    request.headers.push_back(std::format("User-Agent: {}", httpGetAgent));
    request.followRedirects = true;
    const bool reload = lua_toboolean(L, 3) != 0;
    assets::HttpTransport cached = [cache = assets::HttpCache(assets::AssetConfig::fromEnvironment().httpDir),
                                     transport = robloxWeb(L).transport(), reload](const assets::HttpRequest& r) {
        return cache.get(transport, r, reload);
    };
    return send(L, cached, std::move(request), pushBody, "HttpGet");
}

// game:HttpPost(url, data [, contentType]): data sent, the answer's body back,
// whatever its status as HttpGet. The content type is HttpPostAsync's default
// (*/*) unless given.
int httpPost(lua_State* L)
{
    self(L);
    assets::HttpRequest request;
    request.method = "POST";
    request.url = checkString(L, 2);
    request.body = checkString(L, 3);
    const std::string_view type = lua_type(L, 4) == LUA_TSTRING ? checkString(L, 4) : "*/*";
    if (!oneLine(type))
        raise(L, "HttpPost: the content type is one line");
    request.headers.push_back(std::format("User-Agent: {}", httpGetAgent));
    request.headers.push_back(std::format("Content-Type: {}", type));
    request.followRedirects = true;
    return send(L, robloxWeb(L).transport(), std::move(request), pushBody, "HttpPost");
}

// The string field of the options table on top, or nothing when it's nil.
std::optional<std::string> stringField(lua_State* L, const char* name)
{
    const int type = lua_getfield(L, 1, name);
    std::optional<std::string> value;
    if (type == LUA_TSTRING)
        value = lua_tostring(L, -1);
    else if (type != LUA_TNIL)
        raise(L, std::format("request: {} is a string, not a {}", name, lua_typename(L, type)));
    lua_pop(L, 1);
    return value;
}

// request({Url, Method, Headers, Cookies, Body}) (http_request, http.request):
// UNC's request. Method is GET unless given (POST, PUT, PATCH, DELETE, HEAD,
// OPTIONS too); Headers and Cookies are tables of strings. The answer is a
// table whatever the status (Success says whether it was a 2xx); an error only
// when nothing answered.
int request(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    assets::HttpRequest request;
    request.followRedirects = true;
    const std::optional<std::string> url = stringField(L, "Url");
    if (!url)
        raise(L, "request: Url is missing");
    request.url = *url;
    std::string method = stringField(L, "Method").value_or("GET");
    for (char& c : method)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    constexpr std::array methods{"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
    if (std::ranges::find(methods, std::string_view(method)) == methods.end())
        raise(L, std::format("request: {} isn't a method this sends", method));
    request.method = method;
    request.body = stringField(L, "Body").value_or("");

    bool agent = false;
    if (lua_getfield(L, 1, "Headers") == LUA_TTABLE) {
        for (lua_pushnil(L); lua_next(L, -2) != 0; lua_pop(L, 1)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
                raise(L, "request: Headers are strings, name = value");
            const std::string_view name = lua_tostring(L, -2);
            const std::string_view value = lua_tostring(L, -1);
            if (name.empty() || !oneLine(name) || name.contains(':') || !oneLine(value))
                raise(L, std::format("request: \"{}\" isn't a header this sends", name));
            std::string lowered(name);
            for (char& c : lowered)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            agent = agent || lowered == "user-agent";
            request.headers.push_back(std::format("{}: {}", name, value));
        }
    }
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "Cookies") == LUA_TTABLE) {
        std::string cookies;
        for (lua_pushnil(L); lua_next(L, -2) != 0; lua_pop(L, 1)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
                raise(L, "request: Cookies are strings, name = value");
            const std::string_view name = lua_tostring(L, -2);
            const std::string_view value = lua_tostring(L, -1);
            if (!oneLine(name) || !oneLine(value) || name.find_first_of("=;") != std::string_view::npos
                || value.contains(';'))
                raise(L, std::format("request: \"{}\" isn't a cookie this sends", name));
            cookies += std::format("{}{}={}", cookies.empty() ? "" : "; ", name, value);
        }
        if (!cookies.empty())
            request.headers.push_back(std::format("Cookie: {}", cookies));
    }
    lua_pop(L, 1);
    if (!agent)
        request.headers.push_back(std::format("User-Agent: {}", userAgent));
    return send(L, robloxWeb(L).transport(), std::move(request), pushResponse, "request");
}

constexpr auto dataModelMethods = std::to_array<Method>({
    {"HttpGet", "DataModel", httpGet},
    {"HttpGetAsync", "DataModel", httpGet},
    {"HttpPost", "DataModel", httpPost},
    {"HttpPostAsync", "DataModel", httpPost},
});

} // namespace

std::span<const Method> consoleMethods()
{
    return dataModelMethods;
}

void openConsoleHttp(lua_State* thread)
{
    // One function under all three names, as the executors' aliases are.
    lua_pushcfunction(thread, request, "request");
    lua_pushvalue(thread, -1);
    lua_setglobal(thread, "request");
    lua_pushvalue(thread, -1);
    lua_setglobal(thread, "http_request");
    lua_createtable(thread, 0, 1); // http.request
    lua_insert(thread, -2);
    lua_setfield(thread, -2, "request");
    lua_setglobal(thread, "http");
}

} // namespace lunar::lua
