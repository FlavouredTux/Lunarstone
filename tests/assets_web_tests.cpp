#include "fake_web.hpp"
#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/HttpCache.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>

#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <atomic>
#include <stop_token>
#include <thread>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace lunar;
using namespace lunar::assets;
using namespace fakeweb;

TEST(web_look_thumbnails_use_the_batch_endpoint_with_exact_ids)
{
    FakeServer server;
    server.answers["https://thumbnails.roblox.com/v1/batch"]
        = {.body = R"({"data":[{"state":"Completed","imageUrl":"https://tr.rbxcdn.com/look.png"}]})"};
    server.answers["https://tr.rbxcdn.com/look.png"] = {.body = "thumbnail-bytes"};
    RobloxWeb web(server.transport(), fakeLogin());
    const auto result = web.downloadThumbnail("Look", 4006883154366038243ULL, 300, 450, false);
    CHECK(result.has_value());
    CHECK_EQ(*result, "thumbnail-bytes");
    CHECK_EQ(server.seen.size(), 2u);
    CHECK_EQ(server.seen[0].method, "POST");
    const auto request = json::parse(server.seen[0].body);
    const auto* items = request ? request->array() : nullptr;
    CHECK(items != nullptr);
    CHECK_EQ(items->size(), 1u);
    const auto& item = items->front();
    const auto* id = item["targetId"];
    const auto* type = item["type"];
    CHECK(id != nullptr);
    CHECK(type != nullptr);
    const auto* name = type->string();
    CHECK(name != nullptr);
    CHECK_EQ(id->integer(), std::optional<std::int64_t>(4006883154366038243LL));
    CHECK_EQ(*name, "Look");
    for (const auto& seen : server.seen)
        CHECK(!hasHeader(seen, "Cookie: "));
    server.checkNoLeaks();
}

namespace {

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / std::format("lunarstone-web-{}-{}", stamp, next++);
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

void writeFile(const std::filesystem::path& path, std::string_view bytes, unsigned mode)
{
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ::chmod(path.c_str(), mode);
}

// An environment for Credentials::load that holds only what a test puts in.
Credentials::Environment fakeEnvironment(std::map<std::string, std::string, std::less<>> variables)
{
    return [variables = std::move(variables)](std::string_view name) {
        auto it = variables.find(name);
        return it == variables.end() ? std::string() : it->second;
    };
}

} // namespace

TEST(web_https_hosts)
{
    CHECK_EQ(httpsHost("https://avatar.roblox.com/v1/users/1").value_or(""), "avatar.roblox.com");
    CHECK_EQ(httpsHost("HTTPS://Avatar.Roblox.COM?x=1").value_or(""), "avatar.roblox.com");
    CHECK_EQ(httpsHost("https://users.roblox.com:443/v1").value_or(""), "users.roblox.com");
    CHECK(!httpsHost("http://avatar.roblox.com/").has_value());
    CHECK(!httpsHost("https://avatar.roblox.com:8443/").has_value());
    CHECK(!httpsHost("https://me@avatar.roblox.com/").has_value());
    CHECK(!httpsHost("https://avatar.roblox.com\\@evil.example/").has_value());
    CHECK(!httpsHost("https:///nohost").has_value());

    CHECK(isCredentialHost("https://assetdelivery.roblox.com/v2/asset/?id=1"));
    CHECK(isCredentialHost("https://avatar.roblox.com/v2/avatar/avatar-fetch/?placeId=0&userId=1"));
    CHECK(isCredentialHost("https://users.roblox.com/v1/users/authenticated"));
    CHECK(isCredentialHost("https://thumbnails.roblox.com/v1/users/avatar"));
    CHECK(isCredentialHost("https://apis.roblox.com/asset-delivery-api/v1/assetId/1"));
    CHECK(isCredentialHost("https://gamejoin.roblox.com/v1/join-game"));
    // Not API hosts, not Roblox, or not https.
    CHECK(!isCredentialHost("https://fts.rbxcdn.com/sc4/abc"));
    CHECK(!isCredentialHost("https://c0.rbxcdn.com/abc"));
    CHECK(!isCredentialHost("https://www.roblox.com/asset/?id=1"));
    CHECK(!isCredentialHost("https://roblox.com/"));
    CHECK(!isCredentialHost("http://assetdelivery.roblox.com/v1/asset/?id=1"));
    CHECK(!isCredentialHost("https://assetdelivery.roblox.com.evil.example/v1/asset/?id=1"));
    CHECK(!isCredentialHost("https://evil.example/?https://assetdelivery.roblox.com/"));
    CHECK(!isCredentialHost("https://assetdelivery.roblox.com./v1/asset/?id=1"));
    CHECK(!isCredentialHost("https://x@assetdelivery.roblox.com/"));

    // The client app's wider set: roblox.com and every host under it, https only.
    CHECK(isRobloxWebHost("https://friends.roblox.com/v1/my/friends/count"));
    CHECK(isRobloxWebHost("https://www.roblox.com/home"));
    CHECK(isRobloxWebHost("https://roblox.com/"));
    CHECK(isRobloxWebHost("https://APIS.Roblox.com:443/x"));
    CHECK(!isRobloxWebHost("http://friends.roblox.com/"));
    CHECK(!isRobloxWebHost("https://friends.roblox.com.evil.example/"));
    CHECK(!isRobloxWebHost("https://evilroblox.com/"));
    CHECK(!isRobloxWebHost("https://evil.example/?https://www.roblox.com/"));
    CHECK(!isRobloxWebHost("https://x@www.roblox.com/"));
    CHECK(!isRobloxWebHost("https://www.roblox.com./"));
    CHECK(!isRobloxWebHost("https://fts.rbxcdn.com/sc4/abc"));
}

TEST(web_cookie_file_permissions)
{
    TempDir dir;
    const std::filesystem::path file = dir.path() / "cookie";

    // Loose permissions are refused with the reason, and never the contents.
    for (unsigned mode : {0644u, 0640u, 0604u, 0660u, 0666u}) {
        writeFile(file, fakeCookie, mode);
        auto loaded = Credentials::fromCookieFile(file);
        CHECK(!loaded.has_value());
        CHECK(loaded.error().contains("refusing to read"));
        CHECK(loaded.error().contains(std::format("{:04o}", mode)));
        CHECK(loaded.error().contains("0600"));
        CHECK(!loaded.error().contains("FAKE-COOKIE"));
    }
    // 0600 and stricter are fine.
    for (unsigned mode : {0600u, 0400u}) {
        writeFile(file, fakeCookie, mode);
        auto loaded = Credentials::fromCookieFile(file);
        CHECK(loaded.has_value());
        CHECK(loaded->hasCookie());
        CHECK_EQ(loaded->cookieSource(), std::format("the cookie file {}", file.string()));
        ::chmod(file.c_str(), 0600);
    }
    // Not a file, not there, empty, or not a cookie.
    CHECK(!Credentials::fromCookieFile(dir.path()).has_value());
    CHECK(!Credentials::fromCookieFile(dir.path() / "missing").has_value());
    writeFile(file, "\n", 0600);
    CHECK(Credentials::fromCookieFile(file).error().contains("holds no cookie"));
    writeFile(file, "FAKE-COOKIE-FOR-TESTS\r\nX-Injected: 1", 0600);
    auto injected = Credentials::fromCookieFile(file);
    CHECK(!injected.has_value());
    CHECK(!injected.error().contains("FAKE-COOKIE"));
    writeFile(file, "FAKE-COOKIE; other=1", 0600);
    CHECK(!Credentials::fromCookieFile(file).has_value());
    // The cookie line as a browser shows it works too, with whitespace around.
    writeFile(file, std::format("  .ROBLOSECURITY={}\n", fakeCookie), 0600);
    CHECK(Credentials::fromCookieFile(file).has_value());
}

TEST(web_login_sources)
{
    TempDir dir;
    const std::filesystem::path home = dir.path() / "home";
    const std::filesystem::path config = dir.path() / "config";
    const std::filesystem::path explicitFile = dir.path() / "explicit";
    writeFile(explicitFile, fakeCookie, 0600);

    // Nothing anywhere: no login, which isn't an error.
    auto none = Credentials::load(fakeEnvironment({{"HOME", home.string()}}));
    CHECK(none.has_value());
    CHECK(!none->hasCookie());
    CHECK(!none->hasApiKey());
    CHECK_EQ(Credentials::defaultCookieFile(fakeEnvironment({{"HOME", home.string()}})),
             home / ".config/lunarstone/cookie");
    CHECK_EQ(Credentials::defaultCookieFile(
                 fakeEnvironment({{"HOME", home.string()}, {"XDG_CONFIG_HOME", config.string()}})),
             config / "lunarstone/cookie");

    // ~/.config/lunarstone/cookie, under the permission rule.
    writeFile(home / ".config/lunarstone/cookie", fakeCookie, 0644);
    auto loose = Credentials::load(fakeEnvironment({{"HOME", home.string()}}));
    CHECK(!loose.has_value());
    CHECK(loose.error().contains("0644"));
    ::chmod((home / ".config/lunarstone/cookie").c_str(), 0600);
    auto fromHome = Credentials::load(fakeEnvironment({{"HOME", home.string()}}));
    CHECK(fromHome.has_value());
    CHECK(fromHome->cookieSource().contains(".config/lunarstone/cookie"));

    // The environment variable comes before the file...
    auto fromEnv = Credentials::load(
        fakeEnvironment({{"HOME", home.string()}, {"LUNAR_ROBLOSECURITY", std::string(fakeCookie)}}));
    CHECK(fromEnv.has_value());
    CHECK_EQ(fromEnv->cookieSource(), "LUNAR_ROBLOSECURITY");
    // ...and --cookie-file before both.
    auto fromFlag = Credentials::load(
        fakeEnvironment({{"HOME", home.string()}, {"LUNAR_ROBLOSECURITY", std::string(fakeCookie)}}), explicitFile);
    CHECK(fromFlag.has_value());
    CHECK(fromFlag->cookieSource().contains(explicitFile.string()));
    // A --cookie-file that breaks the rules is an error, not a fallback.
    ::chmod(explicitFile.c_str(), 0640);
    CHECK(!Credentials::load(fakeEnvironment({{"LUNAR_ROBLOSECURITY", std::string(fakeCookie)}}), explicitFile)
               .has_value());

    // A malformed variable is refused without being repeated.
    auto badEnv = Credentials::load(fakeEnvironment({{"LUNAR_ROBLOSECURITY", "FAKE-COOKIE\nX-Injected: 1"}}));
    CHECK(!badEnv.has_value());
    CHECK(!badEnv.error().contains("FAKE-COOKIE"));

    auto keyed = Credentials::load(fakeEnvironment({{"LUNAR_ROBLOX_API_KEY", "FAKE-KEY-FOR-TESTS"}}));
    CHECK(keyed.has_value());
    CHECK(keyed->hasApiKey());
    CHECK(!keyed->hasCookie());
}

TEST(web_cookie_only_goes_to_api_hosts)
{
    FakeServer server;
    server.answers["https://fts.rbxcdn.com/abc"] = {.body = "cdn"};
    server.signedInAnswers["https://assetdelivery.roblox.com/v2/asset/?id=1"] = {.body = "{}"};
    const RobloxWeb web(server.transport(), fakeLogin());
    CHECK(web.hasCookie());

    for (std::string_view url : {"https://fts.rbxcdn.com/abc", "https://www.roblox.com/", "http://avatar.roblox.com/",
                                 "https://avatar.roblox.com.evil.example/", "https://evil.example/"}) {
        auto refused = web.withCookie({.url = std::string(url)});
        CHECK(!refused.has_value());
        CHECK(!refused.error().contains("FAKE-COOKIE"));
    }
    CHECK(server.seen.empty());

    // To an API host: exactly one Cookie header, and never along a redirect.
    auto sent = web.withCookie({.url = "https://assetdelivery.roblox.com/v2/asset/?id=1", .followRedirects = true});
    CHECK(sent.has_value());
    CHECK_EQ(server.seen.size(), 1u);
    CHECK(!server.seen[0].followRedirects);
    CHECK_EQ(server.seen[0].headers.size(), 1u);
    CHECK_EQ(server.seen[0].headers[0], std::format("Cookie: .ROBLOSECURITY={}", fakeCookie));

    // Requests can't smuggle a login of their own through either door.
    CHECK(!web.anonymous({.url = "https://fts.rbxcdn.com/abc", .headers = {"Cookie: .ROBLOSECURITY=x"}}).has_value());
    CHECK(!web.anonymous({.url = "https://fts.rbxcdn.com/abc", .headers = {"authorization: x"}}).has_value());
    CHECK(!web.withCookie({.url = "https://users.roblox.com/", .headers = {"X-API-KEY: x"}}).has_value());
    CHECK(!web.anonymous({.url = "http://fts.rbxcdn.com/abc"}).has_value());
    // Anonymous requests never carry the cookie.
    CHECK(web.anonymous({.url = "https://fts.rbxcdn.com/abc"}).has_value());
    CHECK(server.seen.back().headers.empty());

    // The API key only goes to apis.roblox.com.
    auto key = Credentials::load(fakeEnvironment({{"LUNAR_ROBLOX_API_KEY", "FAKE-KEY-FOR-TESTS"}}));
    const RobloxWeb keyed(server.transport(), std::make_shared<const Credentials>(std::move(*key)));
    CHECK(!keyed.withApiKey({.url = "https://assetdelivery.roblox.com/v1/asset/?id=1"}).has_value());
    CHECK(!keyed.withCookie({.url = "https://assetdelivery.roblox.com/v1/asset/?id=1"}).has_value());
    const std::size_t before = server.seen.size();
    CHECK(keyed.withApiKey({.url = "https://apis.roblox.com/asset-delivery-api/v1/assetId/1"}).has_value());
    CHECK_EQ(server.seen.size(), before + 1);
    CHECK_EQ(server.seen.back().headers.front(), "x-api-key: FAKE-KEY-FOR-TESTS");
    server.checkNoLeaks();
}

TEST(web_download_signed_in)
{
    // Anonymous asks get 401; the signed-in v2 reply names a CDN location,
    // which is fetched without the login.
    FakeServer server;
    const std::string denied = R"({"errors":[{"code":0,"message":"Authentication required to access Asset."}]})";
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=7902470429"] = {.status = 401, .body = denied};
    server.signedInAnswers["https://assetdelivery.roblox.com/v2/asset/?id=7902470429"] = {
        .body
        = R"({"locations":[{"assetFormat":"source","location":"https://fts.rbxcdn.com/sc4/place"}],"requestId":"x","assetTypeId":9})"};
    server.answers["https://fts.rbxcdn.com/sc4/place"] = {.body = "<roblox!fake place"};

    const RobloxWeb web(server.transport(), fakeLogin());
    auto bytes = web.downloadAsset(7902470429);
    if (!bytes)
        lt::fail(bytes.error(), std::source_location::current());
    CHECK_EQ(*bytes, "<roblox!fake place");
    CHECK_EQ(server.seen.size(), 3u);
    CHECK(server.seen[0].headers.empty());
    CHECK(hasHeader(server.seen[1], "Cookie: "));
    CHECK(server.seen[2].headers.empty());
    server.checkNoLeaks();

    // Without a login the 401 says how to sign in.
    const RobloxWeb anonymousWeb(server.transport(), std::make_shared<const Credentials>());
    auto refused = anonymousWeb.downloadAsset(7902470429);
    CHECK(!refused.has_value());
    CHECK(refused.error().contains("Authentication required"));
    CHECK(refused.error().contains("not signed in"));
}

TEST(web_download_signed_in_redirect)
{
    // v2 fails; v1 answers the signed-in request with a redirect, which
    // isn't followed with the cookie but fetched anonymously.
    FakeServer server;
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=5"] = {.status = 403, .body = "{}"};
    server.signedInAnswers["https://assetdelivery.roblox.com/v2/asset/?id=5"] = {.status = 500, .body = "{}"};
    server.signedInAnswers["https://assetdelivery.roblox.com/v1/asset/?id=5"]
        = {.status = 302, .location = "https://c1.rbxcdn.com/five"};
    server.answers["https://c1.rbxcdn.com/five"] = {.body = "five"};

    const RobloxWeb web(server.transport(), fakeLogin());
    auto bytes = web.downloadAsset(5);
    CHECK(bytes.has_value());
    CHECK_EQ(bytes.value_or(""), "five");
    CHECK(server.seen.back().headers.empty());
    server.checkNoLeaks();

    // A location that isn't https is not fetched.
    server.signedInAnswers["https://assetdelivery.roblox.com/v1/asset/?id=5"]
        = {.status = 302, .location = "http://c1.rbxcdn.com/five"};
    auto insecure = RobloxWeb(server.transport(), fakeLogin()).downloadAsset(5);
    CHECK(!insecure.has_value());
    CHECK(!insecure.error().contains("FAKE-COOKIE"));
    server.checkNoLeaks();
}

TEST(web_failures_never_show_the_cookie)
{
    const HttpTransport failing = [](const HttpRequest& request) -> std::expected<HttpResponse, std::string> {
        if (hasHeader(request, "Cookie: "))
            return std::unexpected("download failed: connection reset");
        return HttpResponse{.status = 401, .body = "{}"};
    };
    const RobloxWeb web(failing, fakeLogin());
    auto result = web.downloadAsset(99);
    CHECK(!result.has_value());
    CHECK(result.error().starts_with("signed in: "));
    CHECK(!result.error().contains("FAKE-COOKIE"));
    CHECK(!result.error().contains("ROBLOSECURITY="));
}

TEST(web_asset_cache_downloads_through_the_web)
{
    TempDir dir;
    FakeServer server;
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=77"] = {.body = "public bytes"};
    auto cache = std::make_shared<AssetCache>(AssetConfig{
        .contentDir = {},
        .cacheDir = dir.path(),
        .download = true,
        .web = std::make_shared<const RobloxWeb>(server.transport(), std::make_shared<const Credentials>())});
    auto bytes = cache->fetch("rbxassetid://77");
    CHECK(bytes.has_value());
    CHECK_EQ(**bytes, "public bytes");
    CHECK(std::filesystem::is_regular_file(dir.path() / "77"));
    // Public assets went anonymously.
    CHECK_EQ(server.seen.size(), 1u);
    CHECK(server.seen[0].headers.empty());
}

TEST(web_json_reader)
{
    const std::string_view text
        = R"({"id":156,"big":12995020128123,"f":1.5,"e":2e3,"s":"a\u00e9\ud83d\ude00","n":null,"list":[true,false],"o":{"k":-3}})";
    auto value = json::parse(text);
    CHECK(value.has_value());
    CHECK_EQ((*value)["id"]->integer().value_or(0), 156);
    CHECK_EQ((*value)["big"]->integer().value_or(0), 12995020128123);
    CHECK_EQ((*value)["f"]->number().value_or(0.0), 1.5);
    CHECK(!(*value)["f"]->integer().has_value());
    CHECK_EQ((*value)["e"]->integer().value_or(0), 2000);
    CHECK_EQ(*(*value)["s"]->string(), "a\xc3\xa9\xf0\x9f\x98\x80");
    CHECK((*value)["list"]->array()->size() == 2);
    CHECK_EQ((*(*value)["o"])["k"]->integer().value_or(0), -3);
    CHECK(!json::parse("{\"a\":1,}").has_value());
    CHECK(!json::parse("[1 2]").has_value());
}

TEST(web_format_download_signed_in_with_csrf)
{
    // The batch API: anonymous asks get a 401 item; the signed-in POST first
    // gets a 403 with a CSRF token, then names the location when sent again
    // with it. The CDN gets no login.
    std::vector<HttpRequest> seen;
    const HttpTransport transport = [&](const HttpRequest& request) -> std::expected<HttpResponse, std::string> {
        seen.push_back(request);
        if (request.url == "https://fts.rbxcdn.com/head")
            return HttpResponse{.status = 200, .body = "head bytes"};
        if (request.url != "https://assetdelivery.roblox.com/v1/assets/batch")
            return HttpResponse{.status = 404};
        if (!hasHeader(request, "Cookie: "))
            return HttpResponse{
                .status = 200,
                .body
                = R"([{"requestId":"0","errors":[{"code":401,"message":"Authentication required to access Asset."}]}])"};
        if (!hasHeader(request, "X-CSRF-TOKEN: fake-token"))
            return HttpResponse{.status = 403, .body = "{}", .headers = {{"x-csrf-token", "fake-token"}}};
        return HttpResponse{.status = 200, .body = R"([{"location":"https://fts.rbxcdn.com/head","requestId":"0"}])"};
    };
    const RobloxWeb web(transport, fakeLogin());
    auto bytes = web.downloadAssetFormat(10638267973, "avatar_meshpart_head");
    if (!bytes)
        lt::fail(bytes.error(), std::source_location::current());
    CHECK_EQ(*bytes, "head bytes");
    CHECK_EQ(seen.size(), 4u);
    CHECK(seen[0].body.contains(R"("assetFormat":"avatar_meshpart_head")"));
    CHECK(!hasHeader(seen[0], "Cookie: "));
    CHECK(hasHeader(seen[2], "X-CSRF-TOKEN: fake-token"));
    CHECK(seen[3].headers.empty());
    for (const HttpRequest& request : seen)
        if (hasHeader(request, "Cookie: ")) {
            CHECK(isCredentialHost(request.url));
            CHECK(!request.followRedirects);
        }

    // Without a login the 401 item says so; odd formats never go out.
    const RobloxWeb anonymousWeb(transport, std::make_shared<const Credentials>());
    auto refused = anonymousWeb.downloadAssetFormat(10638267973, "avatar_meshpart_head");
    CHECK(!refused.has_value());
    CHECK(refused.error().contains("HTTP 401"));
    CHECK(refused.error().contains("not signed in"));
    const std::size_t before = seen.size();
    CHECK(!web.downloadAssetFormat(1, R"(x","assetId":2)").has_value());
    CHECK_EQ(seen.size(), before);
}

TEST(web_responses_never_keep_set_cookie)
{
    // What curl's header callback hands over, including a redirect hop and
    // a rotated login.
    std::vector<std::pair<std::string, std::string>> headers;
    for (std::string_view line :
         {"HTTP/2 302\r\n", "Location: https://fts.rbxcdn.com/a\r\n", "HTTP/2 403\r\n", "X-CSRF-Token: fake-token\r\n",
          "Set-Cookie: .ROBLOSECURITY=FAKE-COOKIE-FOR-TESTS\r\n", "SET-COOKIE2: x=FAKE-COOKIE-FOR-TESTS\r\n",
          "Content-Type: application/json\r\n", "\r\n"})
        addHeaderLine(headers, line);
    CHECK_EQ(headers.size(), 2u);
    CHECK_EQ(headers[0].first, "x-csrf-token");
    CHECK_EQ(headers[0].second, "fake-token");
    CHECK_EQ(headers[1].first, "content-type");

    // Whatever a transport keeps, RobloxWeb's answers carry no Set-Cookie.
    const HttpTransport transport = [](const HttpRequest&) -> std::expected<HttpResponse, std::string> {
        return HttpResponse{.status = 200,
                            .body = "{}",
                            .headers = {{"Set-Cookie", ".ROBLOSECURITY=FAKE-COOKIE-FOR-TESTS-ROTATED"},
                                        {"set-cookie", "other=1"},
                                        {"x-csrf-token", "fake-token"}}};
    };
    const RobloxWeb web(transport, fakeLogin());
    for (const auto& response : {web.withCookie({.url = "https://users.roblox.com/v1/users/authenticated"}),
                                 web.anonymous({.url = "https://users.roblox.com/v1/users/1"})}) {
        CHECK(response.has_value());
        CHECK_EQ(response->headers.size(), 1u);
        CHECK(response->header("set-cookie") == nullptr);
        for (const auto& [name, value] : response->headers)
            CHECK(!value.contains("FAKE-COOKIE"));
    }
}

// A download whose server never answers gives up as soon as its thread's
// HttpStopScope is told to stop, not after the timeouts: a teleport tore a
// MeshLoader down and waited minutes on a worker stuck in one.
TEST(web_download_stops_when_asked)
{
    if (!AssetCache::canDownload())
        SKIP("built without LUNAR_WITH_CURL");
    // Accepts connections and never says anything, so TLS never finishes.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    CHECK_EQ(::listen(listener, 4), 0);
    socklen_t length = sizeof address;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);

    std::stop_source stop;
    std::atomic<bool> done = false;
    std::expected<HttpResponse, std::string> result;
    std::thread worker([&] {
        const HttpStopScope scope(stop.get_token());
        result = curlTransport()(HttpRequest{.url = std::format("https://127.0.0.1:{}/", port)});
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!done);
    const auto asked = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(3));
    CHECK(!result.has_value());
    ::close(listener);
}

namespace {

// A fresh directory for an HttpCache, removed again afterwards.
struct CacheDir {
    std::filesystem::path path;
    explicit CacheDir(std::string_view name) : path(std::filesystem::temp_directory_path() / name)
    {
        std::filesystem::remove_all(path);
    }
    ~CacheDir() { std::filesystem::remove_all(path); }
    CacheDir(const CacheDir&) = delete;
    CacheDir& operator=(const CacheDir&) = delete;
};

HttpRequest get(std::string url)
{
    HttpRequest request;
    request.url = std::move(url);
    request.headers.push_back("User-Agent: Roblox/WinInet");
    return request;
}

} // namespace

TEST(web_http_cache_keeps_what_its_headers_allow)
{
    const auto fresh = [](std::vector<std::pair<std::string, std::string>> headers, long status = 200) {
        return HttpCache::freshness(HttpResponse{.status = status, .headers = std::move(headers)});
    };
    CHECK(fresh({{"cache-control", "public, max-age=31536000"}}) == std::chrono::seconds(31536000));
    CHECK(fresh({{"cache-control", "max-age=300"}, {"age", "114"}}) == std::chrono::seconds(186));
    CHECK(fresh({{"cache-control", "Max-Age=\"60\""}}) == std::chrono::seconds(60));
    // A private cache may keep what says private.
    CHECK(fresh({{"cache-control", "private, max-age=60"}}) == std::chrono::seconds(60));
    // Not kept: no lifetime, an explicit no, a 404, all used up, varying on everything.
    CHECK(!fresh({}));
    CHECK(!fresh({{"expires", "Wed, 04 Nov 2026 05:10:27 GMT"}}));
    CHECK(!fresh({{"cache-control", "max-age=60, no-cache"}}));
    CHECK(!fresh({{"cache-control", "private, max-age=0, no-store, no-cache, must-revalidate"}}));
    CHECK(!fresh({{"cache-control", "max-age=60"}}, 404));
    CHECK(!fresh({{"cache-control", "max-age=60"}, {"age", "60"}}));
    CHECK(!fresh({{"cache-control", "max-age=60"}, {"vary", "*"}}));
}

TEST(web_http_cache_answers_again_from_disk)
{
    const CacheDir dir("lunarstone-http-cache-test");
    FakeServer server;
    server.answers["http://setup.test/API-Dump.json"]
        = {.body = R"({"Classes": []})", .headers = {{"cache-control", "public, max-age=3600"}, {"etag", "\"v1\""}}};
    server.answers["https://api.test/now"] = {.body = "1", .headers = {{"cache-control", "no-store"}}};
    const HttpTransport transport = server.transport();
    const auto seen = [&] {
        const std::scoped_lock lock(server.mutex);
        return server.seen.size();
    };

    const HttpCache cache(dir.path);
    for (int i = 0; i < 3; ++i) {
        const auto response = cache.get(transport, get("http://setup.test/API-Dump.json"));
        CHECK(response.has_value());
        CHECK_EQ(response->status, 200L);
        CHECK_EQ(response->body, std::string(R"({"Classes": []})"));
        CHECK(response->header("etag") && *response->header("etag") == "\"v1\"");
    }
    CHECK_EQ(seen(), std::size_t{1});
    // Another process's cache over the same directory finds it too.
    CHECK(HttpCache(dir.path).get(transport, get("http://setup.test/API-Dump.json")).has_value());
    CHECK_EQ(seen(), std::size_t{1});
    // reload asks again, and what comes back is kept in its place.
    server.answers["http://setup.test/API-Dump.json"].body = "{}";
    CHECK_EQ(cache.get(transport, get("http://setup.test/API-Dump.json"), true)->body, std::string("{}"));
    CHECK_EQ(cache.get(transport, get("http://setup.test/API-Dump.json"))->body, std::string("{}"));
    CHECK_EQ(seen(), std::size_t{2});
    // A different request is a different answer.
    HttpRequest other = get("http://setup.test/API-Dump.json");
    other.headers.push_back("Accept: text/plain");
    CHECK(cache.get(transport, other).has_value());
    CHECK_EQ(seen(), std::size_t{3});
    // What may not be kept is asked for each time, as is all but a GET.
    CHECK(cache.get(transport, get("https://api.test/now")).has_value());
    CHECK(cache.get(transport, get("https://api.test/now")).has_value());
    HttpRequest post = get("http://setup.test/API-Dump.json");
    post.method = "POST";
    CHECK(cache.get(transport, post).has_value());
    CHECK_EQ(seen(), std::size_t{6});
    // No directory, no cache.
    CHECK(HttpCache(std::filesystem::path()).get(transport, get("http://setup.test/API-Dump.json")).has_value());
    CHECK_EQ(seen(), std::size_t{7});
}

TEST(web_http_cache_prunes_the_stale_then_the_oldest)
{
    const CacheDir dir("lunarstone-http-cache-prune-test");
    FakeServer server;
    for (const char* name : {"a", "b", "c"})
        server.answers[std::format("https://cdn.test/{}", name)]
            = {.body = std::string(1000, *name), .headers = {{"cache-control", "max-age=3600"}}};
    const HttpTransport transport = server.transport();
    const HttpCache cache(dir.path);
    std::vector<std::uintmax_t> sizes; // of each file, as it came
    std::uintmax_t before = 0;
    for (const char* name : {"a", "b", "c"}) {
        CHECK(cache.get(transport, get(std::format("https://cdn.test/{}", name))).has_value());
        std::uintmax_t total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir.path))
            total += entry.file_size();
        sizes.push_back(total - std::exchange(before, total));
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // so the files are written in order
    }
    const auto files = [&] {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir.path))
            ++count;
        return count;
    };
    CHECK_EQ(files(), std::size_t{3});
    // A file that isn't a kept answer, or one gone stale, goes first; then
    // the oldest, until what's left fits (here: the last two).
    { std::ofstream(dir.path / "0000000000000000.http") << "lunarstone-http 1\n1\n200\n0\n0\n"; }
    { std::ofstream(dir.path / "1111111111111111.http") << "junk"; }
    { std::ofstream(dir.path / "notes.txt") << "not the cache's"; }
    HttpCache(dir.path, sizes.at(1) + sizes.at(2)).prune();
    CHECK_EQ(files(), std::size_t{3}); // b, c and notes.txt
    CHECK(std::filesystem::exists(dir.path / "notes.txt"));
    const auto asked = [&] {
        const std::scoped_lock lock(server.mutex);
        return server.seen.size();
    }();
    for (const char* name : {"b", "c"})
        CHECK(cache.get(transport, get(std::format("https://cdn.test/{}", name))).has_value());
    const std::scoped_lock lock(server.mutex);
    CHECK_EQ(server.seen.size(), asked);
}
