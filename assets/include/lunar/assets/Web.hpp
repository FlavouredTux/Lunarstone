#pragma once

// Talking to Roblox's web APIs: requests go through a replaceable transport
// (libcurl, or a fake in tests), and a login (the .ROBLOSECURITY cookie or an
// Open Cloud key) only ever goes to Roblox's API hosts.
//
// Where the cookie comes from, first match wins:
//   1. --cookie-file <path> (configureLogin with a cookie file);
//   2. LUNAR_ROBLOSECURITY (or ROBLOSECURITY) in the environment;
//   3. $XDG_CONFIG_HOME/lunarstone/cookie, else ~/.config/lunarstone/cookie.
// A cookie file is only read when it's a regular file of the current user
// with no permissions for group or others (mode 0600 or stricter). The
// value may be bare or written as ".ROBLOSECURITY=<value>". The Open Cloud
// key only comes from LUNAR_ROBLOX_API_KEY.
//
// The cookie is never printed, logged, written anywhere or put in an error
// message. It goes only to https://{assetdelivery,avatar,users,thumbnails,
// apis,gamejoin}.roblox.com, never to the CDN (rbxcdn.com) and never along a redirect:
// signed-in requests don't follow redirects, and download locations they
// name are fetched anonymously.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::assets {

// While one lives, the downloads its thread makes give up as soon as `stop`
// is requested. A loader's workers hand in their own, so tearing a loader
// down (a teleport does) never waits for a download that stalled.
class HttpStopScope {
public:
    explicit HttpStopScope(std::stop_token stop);
    ~HttpStopScope();
    HttpStopScope(const HttpStopScope&) = delete;
    HttpStopScope& operator=(const HttpStopScope&) = delete;

private:
    std::stop_token previous_;
};

struct HttpRequest {
    std::string method = "GET"; // or POST, PUT, PATCH, DELETE, HEAD, OPTIONS
    std::string url;
    std::vector<std::string> headers; // "Name: value"
    std::string body;                 // sent when not empty (not with GET or HEAD)
    bool followRedirects = false;     // only ever to other https:// URLs
    // http:// as well as https://, there and on redirects. Only for a request
    // that carries no login: the console's, which fetch what scripts ask for.
    bool plainHttp = false;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string location; // the Location of a redirect that wasn't followed
    // The final response's headers, names lower-cased.
    std::vector<std::pair<std::string, std::string>> headers;

    const std::string* header(std::string_view name) const
    {
        for (const auto& [key, value] : headers)
            if (key == name)
                return &value;
        return nullptr;
    }
};

// Takes one raw header line ("Name: value\r\n") into a response's headers,
// the name lower-cased; a status line ("HTTP/2 302") starts the list over.
// Set-Cookie is dropped: Roblox can rotate the login through it, and no
// response object may hold one.
void addHeaderLine(std::vector<std::pair<std::string, std::string>>& headers, std::string_view line);

// Sends one request. Fails only when no response came back; HTTP errors are
// a status. Error messages never include the headers.
using HttpTransport = std::function<std::expected<HttpResponse, std::string>(const HttpRequest&)>;

// Requests over libcurl, https only; one that always fails in a build
// without LUNAR_WITH_CURL.
HttpTransport curlTransport();
bool curlAvailable();

// The lower-cased host of an https:// URL; nothing for other schemes, user
// names in the authority or ports other than 443.
std::optional<std::string> httpsHost(std::string_view url);

// Whether a login may be sent to this URL: https on one of Roblox's API
// hosts (assetdelivery, avatar, users, thumbnails, apis and gamejoin .roblox.com).
bool isCredentialHost(std::string_view url);
// Whether the client's own Lua app would send its login to url: https, and
// roblox.com or any host under it (the app's pages call friends, games,
// presence and the rest). Wider than isCredentialHost, and only for that app.
bool isRobloxWebHost(std::string_view url);

// A login. Holds the secret but never shows it: there's no accessor, and
// only RobloxWeb puts it into request headers.
class Credentials {
public:
    Credentials() = default;

    // Reads a cookie file under the permission rule above. Errors name the
    // file and the problem, never what's in it.
    static std::expected<Credentials, std::string> fromCookieFile(const std::filesystem::path& path);
    // A cookie value from elsewhere (an environment variable); `source` is
    // how messages refer to it.
    static std::expected<Credentials, std::string> fromCookie(std::string_view value, std::string_view source);

    // How the environment lookup goes: getenv in the program, a table in
    // tests (which must never see a real login).
    using Environment = std::function<std::string(std::string_view name)>;
    static Environment processEnvironment();

    // The cookie by the order above (`cookieFile` is --cookie-file) plus the
    // Open Cloud key. No login at all is not an error; a cookie file that
    // breaks the rules, or a malformed value, is.
    static std::expected<Credentials, std::string>
    load(const Environment& environment, const std::optional<std::filesystem::path>& cookieFile = std::nullopt);

    // Where the default cookie file is ($XDG_CONFIG_HOME or ~/.config).
    static std::filesystem::path defaultCookieFile(const Environment& environment);

    bool hasCookie() const { return !cookie_.empty(); }
    bool hasApiKey() const { return !apiKey_.empty(); }
    // Where the cookie came from (a path or a variable name), for messages.
    const std::string& cookieSource() const { return cookieSource_; }

private:
    friend class RobloxWeb;
    std::string cookie_;
    std::string cookieSource_;
    std::string apiKey_;
};

// The login used when nobody passes one: loaded from the process
// environment on first use (Credentials::load). When that fails (a cookie
// file with loose permissions, say), it's anonymous and the reason is kept
// for loginProblem().
std::shared_ptr<const Credentials> defaultCredentials();
// Why the default login couldn't be loaded; empty when it could.
std::string loginProblem();
// Loads the default login now, with --cookie-file when given, replacing the
// one in use. Errors say why (bad permissions...), never the cookie.
std::expected<void, std::string> configureLogin(const std::optional<std::filesystem::path>& cookieFile);

// Requests to Roblox's web APIs, with or without the login.
class RobloxWeb {
public:
    // Null credentials: defaultCredentials(), looked up when first needed.
    explicit RobloxWeb(HttpTransport transport = curlTransport(),
                       std::shared_ptr<const Credentials> credentials = nullptr);

    // Without any login, to any https URL. Refuses requests that carry
    // credential headers of their own (Cookie, x-api-key, Authorization).
    std::expected<HttpResponse, std::string> anonymous(HttpRequest request) const;
    // With the .ROBLOSECURITY cookie: only to isCredentialHost URLs and
    // never following redirects. Fails when there's no cookie.
    std::expected<HttpResponse, std::string> withCookie(HttpRequest request) const;
    // With the Open Cloud key as x-api-key: only to https://apis.roblox.com.
    std::expected<HttpResponse, std::string> withApiKey(HttpRequest request) const;
    // As the client's own Lua app sends a request: with the cookie, to an
    // isRobloxWebHost URL, never following redirects; a 403 that hands out a
    // CSRF token is sent again once with it, as the client's HTTP layer does.
    std::expected<HttpResponse, std::string> asClientApp(HttpRequest request) const;
    // What these send through, which adds no login of its own: for requests
    // whose headers are someone else's to choose (the console's request()).
    const HttpTransport& transport() const { return transport_; }

    bool hasCookie() const;
    bool hasApiKey() const;
    // What a request for a login gets told when there is none.
    std::string noLoginHint() const;

    // An asset's bytes (a mesh, a model, a place...): anonymously from
    // assetdelivery first, then for 401/403 with the cookie (v2 JSON naming
    // a CDN location, else v1's redirect, either fetched anonymously), then
    // with the Open Cloud key.
    // placeId: the experience it's for (Roblox-Place-Id), as the client
    // says; audio shared with a place and not with everyone needs it.
    std::expected<std::string, std::string> downloadAsset(std::uint64_t assetId, std::uint64_t placeId = 0) const;
    // One representation of an asset (assetFormat "avatar_meshpart_head"
    // for a dynamic head as a MeshPart, say) through the batch API the
    // client's asset loader uses (POST assetdelivery.roblox.com/v1/assets/
    // batch, 0x100a3d225): anonymously first, then with the cookie.
    std::expected<std::string, std::string> downloadAssetFormat(std::uint64_t assetId, std::string_view format) const;
    // The image an rbxthumb:// id names (type AvatarHeadShot, GameIcon,
    // Asset...): thumbnails.roblox.com's URL for it, asked again while it's
    // still being made, then the PNG. A size the API doesn't make for the
    // type falls back to 150x150. Anonymous.
    std::expected<std::string, std::string> downloadThumbnail(std::string_view type, std::uint64_t id,
                                                              std::uint32_t width, std::uint32_t height,
                                                              bool circular) const;

private:
    std::shared_ptr<const Credentials> credentials() const;

    HttpTransport transport_;
    std::shared_ptr<const Credentials> credentials_;
};

// Roblox's error replies look like {"errors":[{"code":0,"message":"..."}]}:
// "HTTP 401: Authentication required to access Asset.".
std::string describeHttpFailure(const HttpResponse& response);

} // namespace lunar::assets
