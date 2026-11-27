#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

namespace lunar::assets {

namespace {

// The hosts a login may go to. GUESS: the set the client sends its cookie
// to is wider (it signs in to most of *.roblox.com); these are the ones
// Lunarstone needs.
constexpr std::array credentialHosts{
    std::string_view("assetdelivery.roblox.com"), std::string_view("avatar.roblox.com"),
    std::string_view("users.roblox.com"),         std::string_view("thumbnails.roblox.com"),
    std::string_view("apis.roblox.com"),      std::string_view("gamejoin.roblox.com"),
};

// Nobody's cookie file is this big; a bigger one isn't a cookie file.
constexpr std::size_t maxCookieFile = 64u << 10;

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

std::string_view trim(std::string_view text)
{
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

// What can go into a Cookie header unquoted (RFC 6265 cookie-octet): no
// spaces, controls, quotes, commas, semicolons or backslashes, so nothing in
// a cookie file can add headers or other cookies.
bool cookieOctets(std::string_view value)
{
    return std::ranges::all_of(value, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u > 0x20 && u < 0x7f && c != '"' && c != ',' && c != ';' && c != '\\';
    });
}

// Header names that carry a login.
bool credentialHeader(std::string_view header)
{
    const std::string name = lower(trim(header.substr(0, header.find(':'))));
    return name == "cookie" || name == "x-api-key" || name == "authorization";
}

std::string environmentVariable(std::string_view name)
{
    const char* value = std::getenv(std::string(name).c_str()); // NOLINT(concurrency-mt-unsafe): nothing sets variables
    return value ? std::string(value) : std::string();
}

struct FileCloser {
    int fd;
    explicit FileCloser(int descriptor) : fd(descriptor) {}
    FileCloser(const FileCloser&) = delete;
    FileCloser& operator=(const FileCloser&) = delete;
    ~FileCloser()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

struct DefaultLogin {
    std::mutex mutex;
    bool loaded = false;
    std::shared_ptr<const Credentials> credentials;
    std::string problem;
};

DefaultLogin& defaultLogin()
{
    static DefaultLogin login;
    return login;
}

} // namespace

std::optional<std::string> httpsHost(std::string_view url)
{
    constexpr std::string_view scheme = "https://";
    if (url.size() < scheme.size() || lower(url.substr(0, scheme.size())) != scheme)
        return std::nullopt;
    url.remove_prefix(scheme.size());
    std::string_view authority = url.substr(0, url.find_first_of("/?#"));
    // User names and backslashes are how URLs get to mean another host.
    if (authority.contains('@') || authority.contains('\\') || authority.empty())
        return std::nullopt;
    if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
        if (authority.substr(colon + 1) != "443")
            return std::nullopt;
        authority = authority.substr(0, colon);
    }
    if (authority.empty())
        return std::nullopt;
    return lower(authority);
}

bool isCredentialHost(std::string_view url)
{
    const auto host = httpsHost(url);
    return host && std::ranges::contains(credentialHosts, std::string_view(*host));
}

bool isRobloxWebHost(std::string_view url)
{
    const auto host = httpsHost(url);
    return host && (*host == "roblox.com" || host->ends_with(".roblox.com"));
}

void addHeaderLine(std::vector<std::pair<std::string, std::string>>& headers, std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);
    if (line.starts_with("HTTP/")) {
        headers.clear();
        return;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || headers.size() > 256)
        return;
    std::string name = lower(trim(line.substr(0, colon)));
    if (name == "set-cookie" || name == "set-cookie2")
        return;
    headers.emplace_back(std::move(name), std::string(trim(line.substr(colon + 1))));
}

// --- Credentials -------------------------------------------------------------------

std::expected<Credentials, std::string> Credentials::fromCookie(std::string_view value, std::string_view source)
{
    value = trim(value);
    if (value.starts_with(".ROBLOSECURITY="))
        value = trim(value.substr(15));
    if (value.empty())
        return std::unexpected(std::format("{} holds no cookie", source));
    if (!cookieOctets(value))
        return std::unexpected(std::format("{} holds characters a cookie can't have", source));
    Credentials credentials;
    credentials.cookie_ = std::string(value);
    credentials.cookieSource_ = std::string(source);
    return credentials;
}

std::expected<Credentials, std::string> Credentials::fromCookieFile(const std::filesystem::path& path)
{
    const std::string name = path.string();
    // Checked on the open file, so it can't be swapped between the check and the read.
    const FileCloser file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
    if (file.fd < 0)
        return std::unexpected(std::format("can't open the cookie file {}", name));
    struct stat info{};
    if (::fstat(file.fd, &info) != 0)
        return std::unexpected(std::format("can't check the cookie file {}", name));
    if (!S_ISREG(info.st_mode))
        return std::unexpected(std::format("the cookie file {} isn't a regular file", name));
    if (info.st_uid != ::geteuid())
        return std::unexpected(std::format("refusing to read the cookie file {}: it belongs to another user", name));
    if ((info.st_mode & 077) != 0)
        return std::unexpected(std::format(
            "refusing to read the cookie file {}: its mode is {:04o}; it must be 0600 or stricter (chmod 600 {})", name,
            info.st_mode & 07777, name));
    if (std::cmp_greater(info.st_size, maxCookieFile))
        return std::unexpected(std::format("the cookie file {} is too big to be a cookie", name));

    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t got = ::read(file.fd, bytes.data() + done, bytes.size() - done);
        if (got <= 0)
            break;
        done += static_cast<std::size_t>(got);
    }
    bytes.resize(done);
    auto credentials = fromCookie(bytes, std::format("the cookie file {}", name));
    // Wipe the buffer: the only copy left is the one in the credentials.
    std::ranges::fill(bytes, '\0');
    return credentials;
}

Credentials::Environment Credentials::processEnvironment()
{
    return &environmentVariable;
}

std::filesystem::path Credentials::defaultCookieFile(const Environment& environment)
{
    if (const std::string config = environment("XDG_CONFIG_HOME"); !config.empty() && config.front() == '/')
        return std::filesystem::path(config) / "lunarstone/cookie";
    if (const std::string home = environment("HOME"); !home.empty())
        return std::filesystem::path(home) / ".config/lunarstone/cookie";
    return {};
}

std::expected<Credentials, std::string> Credentials::load(const Environment& environment,
                                                          const std::optional<std::filesystem::path>& cookieFile)
{
    std::expected<Credentials, std::string> loaded = Credentials();
    if (cookieFile) {
        loaded = fromCookieFile(*cookieFile);
    } else if (std::string value = environment("LUNAR_ROBLOSECURITY"); !value.empty()) {
        loaded = fromCookie(value, "LUNAR_ROBLOSECURITY");
    } else if (value = environment("ROBLOSECURITY"); !value.empty()) {
        loaded = fromCookie(value, "ROBLOSECURITY");
    } else if (const std::filesystem::path path = defaultCookieFile(environment); !path.empty()) {
        // No file is no login; a file that's there has to follow the rules.
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::symlink_status(path, ec)))
            loaded = fromCookieFile(path);
    }
    if (!loaded)
        return loaded;

    if (std::string key = environment("LUNAR_ROBLOX_API_KEY"); !key.empty()) {
        const std::string_view trimmed = trim(key);
        if (!std::ranges::all_of(trimmed, [](char c) { return c > 0x20 && c < 0x7f; }))
            return std::unexpected("LUNAR_ROBLOX_API_KEY holds characters an API key can't have");
        loaded->apiKey_ = std::string(trimmed);
    }
    return loaded;
}

std::shared_ptr<const Credentials> defaultCredentials()
{
    DefaultLogin& login = defaultLogin();
    const std::scoped_lock lock(login.mutex);
    if (!login.loaded) {
        login.loaded = true;
        auto loaded = Credentials::load(Credentials::processEnvironment());
        if (loaded) {
            login.credentials = std::make_shared<const Credentials>(std::move(*loaded));
        } else {
            login.credentials = std::make_shared<const Credentials>();
            login.problem = std::move(loaded.error());
        }
    }
    return login.credentials;
}

std::string loginProblem()
{
    defaultCredentials();
    DefaultLogin& login = defaultLogin();
    const std::scoped_lock lock(login.mutex);
    return login.problem;
}

std::expected<void, std::string> configureLogin(const std::optional<std::filesystem::path>& cookieFile)
{
    auto loaded = Credentials::load(Credentials::processEnvironment(), cookieFile);
    DefaultLogin& login = defaultLogin();
    const std::scoped_lock lock(login.mutex);
    login.loaded = true;
    if (!loaded) {
        login.credentials = std::make_shared<const Credentials>();
        login.problem = loaded.error();
        return std::unexpected(std::move(loaded.error()));
    }
    login.credentials = std::make_shared<const Credentials>(std::move(*loaded));
    login.problem.clear();
    return {};
}

// --- Requests ------------------------------------------------------------------------

std::string describeHttpFailure(const HttpResponse& response)
{
    std::string message;
    if (auto document = json::parse(response.body))
        if (const json::Value* errors = (*document)["errors"]; errors && errors->array() && !errors->array()->empty())
            if (const json::Value* text = errors->array()->front()["message"]; text && text->string())
                message = *text->string();
    if (message.empty())
        return std::format("HTTP {}", response.status);
    return std::format("HTTP {}: {}", response.status, message);
}

RobloxWeb::RobloxWeb(HttpTransport transport, std::shared_ptr<const Credentials> credentials)
    : transport_(std::move(transport)), credentials_(std::move(credentials))
{
}

// Held for a whole request, so a configureLogin meanwhile can't pull it away.
std::shared_ptr<const Credentials> RobloxWeb::credentials() const
{
    return credentials_ ? credentials_ : defaultCredentials();
}

bool RobloxWeb::hasCookie() const
{
    return credentials()->hasCookie();
}

bool RobloxWeb::hasApiKey() const
{
    return credentials()->hasApiKey();
}

std::string RobloxWeb::noLoginHint() const
{
    if (!credentials_)
        if (std::string problem = loginProblem(); !problem.empty())
            return std::format("not signed in: {}", problem);
    return "not signed in: use --cookie-file, ~/.config/lunarstone/cookie (mode 0600) or LUNAR_ROBLOSECURITY";
}

namespace {

// Whatever the transport kept, a cookie Roblox sets (a rotated login) never
// leaves RobloxWeb.
std::expected<HttpResponse, std::string> withoutSetCookie(std::expected<HttpResponse, std::string> response)
{
    if (response)
        std::erase_if(response->headers, [](const auto& header) {
            const std::string name = lower(trim(header.first));
            return name == "set-cookie" || name == "set-cookie2";
        });
    return response;
}

} // namespace

std::expected<HttpResponse, std::string> RobloxWeb::anonymous(HttpRequest request) const
{
    if (!httpsHost(request.url))
        return std::unexpected("only https:// URLs are fetched");
    if (std::ranges::any_of(request.headers, credentialHeader))
        return std::unexpected("anonymous requests carry no login");
    return withoutSetCookie(transport_(request));
}

std::expected<HttpResponse, std::string> RobloxWeb::withCookie(HttpRequest request) const
{
    if (!isCredentialHost(request.url))
        return std::unexpected("the login is only sent to Roblox's API hosts");
    if (std::ranges::any_of(request.headers, credentialHeader))
        return std::unexpected("the request already carries a login");
    const std::shared_ptr<const Credentials> held = credentials();
    if (!held->hasCookie())
        return std::unexpected(noLoginHint());
    request.followRedirects = false;
    std::string header = "Cookie: .ROBLOSECURITY=";
    header += held->cookie_;
    request.headers.push_back(std::move(header));
    auto response = withoutSetCookie(transport_(request));
    // Don't leave the header copy around longer than the request.
    std::ranges::fill(request.headers.back(), '\0');
    return response;
}

std::expected<HttpResponse, std::string> RobloxWeb::asClientApp(HttpRequest request) const
{
    if (!isRobloxWebHost(request.url))
        return std::unexpected("the client app's login only goes to roblox.com hosts");
    if (std::ranges::any_of(request.headers, credentialHeader))
        return std::unexpected("the request already carries a login");
    const std::shared_ptr<const Credentials> held = credentials();
    if (!held->hasCookie())
        return std::unexpected(noLoginHint());
    request.followRedirects = false;
    std::string header = "Cookie: .ROBLOSECURITY=";
    header += held->cookie_;
    request.headers.push_back(std::move(header));
    auto response = withoutSetCookie(transport_(request));
    if (response && response->status == 403)
        if (const std::string* token = response->header("x-csrf-token"); token && !token->empty()) {
            request.headers.insert(request.headers.end() - 1, std::format("X-CSRF-TOKEN: {}", *token));
            response = withoutSetCookie(transport_(request));
        }
    // Don't leave the header copy around longer than the request.
    std::ranges::fill(request.headers.back(), '\0');
    return response;
}

std::expected<HttpResponse, std::string> RobloxWeb::withApiKey(HttpRequest request) const
{
    if (httpsHost(request.url) != "apis.roblox.com")
        return std::unexpected("the API key is only sent to apis.roblox.com");
    if (std::ranges::any_of(request.headers, credentialHeader))
        return std::unexpected("the request already carries a login");
    const std::shared_ptr<const Credentials> held = credentials();
    if (!held->hasApiKey())
        return std::unexpected("no Open Cloud key (LUNAR_ROBLOX_API_KEY)");
    request.followRedirects = false;
    request.headers.push_back("x-api-key: " + held->apiKey_);
    auto response = withoutSetCookie(transport_(request));
    std::ranges::fill(request.headers.back(), '\0');
    return response;
}

namespace {

// A download location an API named, fetched without any login.
std::expected<std::string, std::string> fetchLocation(const RobloxWeb& web, const std::string& location)
{
    if (!httpsHost(location))
        return std::unexpected("asset delivery named no https download location");
    auto file = web.anonymous({.url = location, .followRedirects = true});
    if (!file)
        return std::unexpected(file.error());
    if (file->status != 200)
        return std::unexpected(std::format("CDN answered HTTP {}", file->status));
    return std::move(file->body);
}

// Asset delivery's signed-in answers: JSON naming a CDN location (v2's
// {"locations":[{"location":...}]}, the Open Cloud API's {"location":...}),
// or a redirect there (v1), which isn't followed with the login on.
std::expected<std::string, std::string> fetchNamedLocation(const RobloxWeb& web,
                                                           const std::expected<HttpResponse, std::string>& reply)
{
    if (!reply)
        return std::unexpected(reply.error());
    if (reply->status >= 300 && reply->status < 400 && !reply->location.empty())
        return fetchLocation(web, reply->location);
    if (reply->status != 200)
        return std::unexpected(describeHttpFailure(*reply));
    auto document = json::parse(reply->body);
    const json::Value* location = document ? (*document)["location"] : nullptr;
    if (document && !location)
        if (const json::Value* locations = (*document)["locations"];
            locations && locations->array() && !locations->array()->empty())
            location = locations->array()->front()["location"];
    if (!location || !location->string())
        return std::unexpected("asset delivery named no download location");
    return fetchLocation(web, *location->string());
}

} // namespace

namespace {

// A batch reply: [{"location":...} or {"errors":[{"code":401,...}]}].
struct BatchItem {
    std::string location;
    long code = 0; // the item's error code, when it has one
    std::string message;
};

std::expected<BatchItem, std::string> batchItem(const std::expected<HttpResponse, std::string>& reply)
{
    if (!reply)
        return std::unexpected(reply.error());
    if (reply->status != 200)
        return std::unexpected(describeHttpFailure(*reply));
    auto document = json::parse(reply->body);
    if (!document || !document->array() || document->array()->empty())
        return std::unexpected("asset delivery's batch reply isn't a list");
    const json::Value& item = document->array()->front();
    BatchItem out;
    if (const json::Value* location = item["location"]; location && location->string())
        out.location = *location->string();
    if (const json::Value* errors = item["errors"]; errors && errors->array() && !errors->array()->empty()) {
        const json::Value& error = errors->array()->front();
        const json::Value* code = error["code"];
        if (const auto number = code ? code->integer() : std::nullopt)
            out.code = static_cast<long>(*number);
        if (const json::Value* message = error["message"]; message && message->string())
            out.message = *message->string();
    }
    if (out.location.empty() && out.code == 0)
        out.message = "asset delivery named no download location";
    return out;
}

std::string describeItem(const BatchItem& item)
{
    return item.code ? std::format("HTTP {}: {}", item.code, item.message) : item.message;
}

} // namespace

namespace {

// A string field of the first object in a thumbnails API reply ("state",
// "imageUrl"); empty when it isn't there. The API's strings carry no
// escapes beyond "\/".
std::string thumbnailField(std::string_view json, std::string_view name)
{
    const std::string key = std::format("\"{}\":\"", name);
    const std::size_t at = json.find(key);
    if (at == std::string_view::npos)
        return {};
    const std::size_t start = at + key.size();
    const std::size_t end = json.find('"', start);
    if (end == std::string_view::npos)
        return {};
    std::string value(json.substr(start, end - start));
    for (std::size_t slash = value.find("\\/"); slash != std::string::npos; slash = value.find("\\/", slash))
        value.erase(slash, 1);
    return value;
}

} // namespace

std::expected<std::string, std::string> RobloxWeb::downloadThumbnail(std::string_view type, std::uint64_t id,
                                                                     std::uint32_t width, std::uint32_t height,
                                                                     bool circular) const
{
    // GameIcon takes a universe, GameThumbnail a place (its asset thumbnail).
    constexpr std::array<std::pair<std::string_view, std::string_view>, 10> endpoints{{
        {"AvatarHeadShot", "users/avatar-headshot?userIds"},
        {"AvatarBust", "users/avatar-bust?userIds"},
        {"Avatar", "users/avatar?userIds"},
        {"GameIcon", "games/icons?universeIds"},
        {"GameThumbnail", "assets?assetIds"},
        {"Asset", "assets?assetIds"},
        {"BadgeIcon", "badges/icons?badgeIds"},
        {"GroupIcon", "groups/icons?groupIds"},
        {"BundleThumbnail", "bundles/thumbnails?bundleIds"},
        {"GamePass", "game-passes?gamePassIds"},
    }};
    const auto endpoint = std::ranges::find(endpoints, type, &std::pair<std::string_view, std::string_view>::first);
    if (endpoint == endpoints.end() && type != "Look")
        return std::unexpected(std::format("rbxthumb: no thumbnail type {}", type));
    std::string size = std::format("{}x{}", width, height);
    for (int attempt = 0; attempt < 10; ++attempt) {
        // Marketplace looks only have a batch thumbnail endpoint. Keep the
        // full uint64 id in the JSON; converting it to a double loses bits.
        const HttpRequest request
            = type == "Look"
                  ? HttpRequest{.method = "POST",
                                .url = "https://thumbnails.roblox.com/v1/batch",
                                .headers = {"Accept: application/json", "Content-Type: application/json"},
                                .body = std::format(
                                    R"([{{"requestId":"lunarstone","targetId":{},"type":"Look","size":"{}","format":"Png","isCircular":{}}}])",
                                    id, size, circular ? "true" : "false")}
                  : HttpRequest{.url
                                = std::format("https://thumbnails.roblox.com/v1/{}={}&size={}&format=Png&isCircular={}",
                                              endpoint->second, id, size, circular ? "true" : "false"),
                                .headers = {"Accept: application/json"}};
        auto reply = anonymous(request);
        if (!reply)
            return std::unexpected(reply.error());
        if (reply->status == 400 && size != "150x150") {
            size = "150x150"; // not a size this type comes in
            continue;
        }
        if (reply->status == 429) {
            // Too many at once (a page asks for dozens): wait and ask again.
            std::this_thread::sleep_for(std::chrono::milliseconds(750 * (attempt + 1)));
            continue;
        }
        if (reply->status != 200)
            return std::unexpected(std::format("thumbnail {} {}: {}", type, id, describeHttpFailure(*reply)));
        const std::string state = thumbnailField(reply->body, "state");
        const std::string image = thumbnailField(reply->body, "imageUrl");
        if (state == "Completed" && !image.empty()) {
            auto png = anonymous({.url = image, .followRedirects = true});
            if (!png)
                return std::unexpected(png.error());
            if (png->status != 200)
                return std::unexpected(std::format("thumbnail {} {}: {}", type, id, describeHttpFailure(*png)));
            return std::move(png->body);
        }
        if (state != "Pending")
            return std::unexpected(std::format("thumbnail {} {}: {}", type, id, state.empty() ? "no image" : state));
        // Still being made: Roblox renders it on request.
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
    }
    return std::unexpected(std::format("thumbnail {} {}: still pending", type, id));
}

std::expected<std::string, std::string> RobloxWeb::downloadAssetFormat(std::uint64_t assetId,
                                                                       std::string_view format) const
{
    // Formats are short names ("avatar_meshpart_head"); nothing that needs escaping.
    if (format.empty() || !std::ranges::all_of(format, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }))
        return std::unexpected(std::format("{} isn't an asset format", format));
    HttpRequest request{.method = "POST",
                        .url = "https://assetdelivery.roblox.com/v1/assets/batch",
                        .headers = {"Content-Type: application/json", "Accept: application/json"},
                        .body
                        = std::format(R"([{{"assetId":{},"requestId":"0","assetFormat":"{}"}}])", assetId, format)};
    auto anonymousItem = batchItem(anonymous(request));
    if (!anonymousItem)
        return std::unexpected(anonymousItem.error());
    if (!anonymousItem->location.empty())
        return fetchLocation(*this, anonymousItem->location);
    std::string failure = describeItem(*anonymousItem);
    if (anonymousItem->code != 401 && anonymousItem->code != 403)
        return std::unexpected(failure);
    if (!hasCookie())
        return std::unexpected(std::format("{} ({})", failure, noLoginHint()));

    // A signed-in POST wants the CSRF token Roblox hands out in a 403.
    auto reply = withCookie(request);
    if (reply && reply->status == 403)
        if (const std::string* token = reply->header("x-csrf-token"); token && !token->empty()) {
            HttpRequest again = request;
            again.headers.push_back(std::format("X-CSRF-TOKEN: {}", *token));
            reply = withCookie(std::move(again));
        }
    auto signedIn = batchItem(reply);
    if (!signedIn)
        return std::unexpected(std::format("signed in: {}", signedIn.error()));
    if (signedIn->location.empty())
        return std::unexpected(std::format("signed in: {}", describeItem(*signedIn)));
    return fetchLocation(*this, signedIn->location);
}

std::expected<std::string, std::string> RobloxWeb::downloadAsset(std::uint64_t assetId, std::uint64_t placeId) const
{
    auto forPlace = [placeId](HttpRequest request) {
        if (placeId != 0)
            request.headers.push_back(std::format("Roblox-Place-Id: {}", placeId));
        return request;
    };
    // Public assets need nothing, and asking anonymously first keeps
    // the login off the wire whenever possible.
    auto anonymousReply = anonymous(forPlace(
        {.url = std::format("https://assetdelivery.roblox.com/v1/asset/?id={}", assetId), .followRedirects = true}));
    if (!anonymousReply)
        return std::unexpected(anonymousReply.error());
    if (anonymousReply->status == 200 && !anonymousReply->body.empty())
        return std::move(anonymousReply->body);
    std::string failure = describeHttpFailure(*anonymousReply);
    if (anonymousReply->status != 401 && anonymousReply->status != 403)
        return std::unexpected(failure);

    const bool cookie = hasCookie();
    const bool apiKey = hasApiKey();
    if (!cookie && !apiKey)
        return std::unexpected(std::format("{} ({})", failure, noLoginHint()));
    if (cookie) {
        auto signedIn = fetchNamedLocation(
            *this, withCookie(forPlace({.url = std::format("https://assetdelivery.roblox.com/v2/asset/?id={}", assetId)})));
        // GUESS: v1 is what Studio's place downloads use; it answers a
        // signed-in request with a redirect to the CDN.
        if (!signedIn)
            if (auto v1 = fetchNamedLocation(
                    *this,
                    withCookie(forPlace({.url = std::format("https://assetdelivery.roblox.com/v1/asset/?id={}", assetId)}))))
                signedIn = std::move(v1);
        if (signedIn)
            return signedIn;
        failure = std::format("signed in: {}", signedIn.error());
    }
    if (apiKey) {
        auto keyed = fetchNamedLocation(
            *this,
            withApiKey({.url = std::format("https://apis.roblox.com/asset-delivery-api/v1/assetId/{}", assetId)}));
        if (keyed)
            return keyed;
        failure = std::format("API key: {}", keyed.error());
    }
    return std::unexpected(failure);
}

} // namespace lunar::assets
