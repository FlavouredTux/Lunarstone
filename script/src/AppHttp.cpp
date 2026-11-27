// The client's own HTTP, which its Lua app fetches every page's data with:
// HttpService:RequestInternal (an HttpRequest that calls back) and
// HttpRbxApiService's Get/PostAsync(FullUrl) (which yield). Requests to
// roblox.com hosts go signed in, as the client sends them
// (RobloxWeb::asClientApp); others go without a login.

#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/Classes.hpp>
#include <lunar/RobloxVersion.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace lunar::lua {

namespace {
struct CatalogSearchParams {
    std::map<std::string, Packed, std::less<>> fields;
};
} // namespace

template <>
struct TypeTraits<CatalogSearchParams> {
    static constexpr int tag = CatalogSearchParamsTag;
    static constexpr const char* name = "CatalogSearchParams";
};

namespace {

using Result = std::expected<assets::HttpResponse, std::string>;

// What the request's thread fills in, read once `done` is set.
struct Fetch {
    std::atomic<bool> done = false;
    Result result = std::unexpected(std::string());
    double seconds = 0.0; // how long it took, sent to answered
};

// Sends the request on a thread of its own; the answer turns up in the Fetch.
std::shared_ptr<Fetch> startFetch(lua_State* L, assets::HttpRequest request, bool anonymous = false)
{
    if (std::getenv("LUNAR_APP_HTTP_LOG"))
        std::println(stderr, "app http: start {} {}", request.method, request.url);
    robloxWeb(L); // made now (and raising if requests are off), on this thread
    std::shared_ptr<const assets::RobloxWeb> web = Runtime::of(L).shared().web;
    auto fetch = std::make_shared<Fetch>();
    try {
        std::thread([fetch, web = std::move(web), request = std::move(request), anonymous] {
            const auto started = std::chrono::steady_clock::now();
            fetch->result = !anonymous && assets::isRobloxWebHost(request.url) ? web->asClientApp(request)
                                                                               : web->anonymous(request);
            // research: LUNAR_APP_HTTP_LOG=1 says each request and its status
            // (never its headers: they carry the login).
            fetch->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            static const bool log = std::getenv("LUNAR_APP_HTTP_LOG") != nullptr;
            if (log) {
                std::string names;
                for (const std::string& header : request.headers)
                    names += std::format(" {}", std::string_view(header).substr(0, header.find(':')));
                std::println(stderr, "app http: {} {} -> {} in {:.1f} s (headers:{})", request.method, request.url,
                             fetch->result ? std::to_string(fetch->result->status) : fetch->result.error(),
                             fetch->seconds, names);
            }
            fetch->done.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error& e) {
        raise(L, std::format("HTTP request failed: {}", e.what()));
    }
    return fetch;
}

// Calls `ready` on the frame the fetch is done (checked each Heartbeat).
void whenDone(Runtime& rt, std::shared_ptr<Fetch> fetch, std::move_only_function<void(const Fetch&)> ready)
{
    // Always defer the first poll: even an immediately answered request must
    // let its caller yield before wake() can resume it.
    rt.after(0.0, [&rt, fetch = std::move(fetch), ready = std::move(ready)] mutable {
        if (!fetch->done.load(std::memory_order_acquire))
            whenDone(rt, std::move(fetch), std::move(ready));
        else
            ready(*fetch);
    });
}

std::string_view statusMessage(long status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
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
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "";
    }
}

// {StatusCode, StatusMessage, Body, Headers, Stats}, as the client answers.
void pushResponse(lua_State* L, const assets::HttpResponse& response, double seconds)
{
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, static_cast<double>(response.status));
    lua_setfield(L, -2, "StatusCode");
    pushString(L, statusMessage(response.status));
    lua_setfield(L, -2, "StatusMessage");
    pushString(L, response.body);
    lua_setfield(L, -2, "Body");
    lua_createtable(L, 0, static_cast<int>(response.headers.size()));
    for (const auto& [name, value] : response.headers) {
        pushString(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    lua_setfield(L, -2, "Headers");
    // The client's timings of the request's stages, which the app adds up
    // for its network profile. GUESS: zeros; the transport doesn't time them.
    lua_createtable(L, 0, 8);
    for (const char* stage : {"DurationInQueue", "DurationNameLookup", "DurationConnect", "DurationSSLHandshake",
                              "DurationMakeRequest", "DurationReceiveResponse"}) {
        lua_pushnumber(L, 0);
        lua_setfield(L, -2, stage);
    }
    lua_pushnumber(L, seconds);
    lua_setfield(L, -2, "RoundTripTime");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "WasRequestRetried");
    lua_setfield(L, -2, "Stats");
}

// The request a {Url, Method, Headers, Body} table describes.
assets::HttpRequest requestFrom(lua_State* L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    assets::HttpRequest request;
    lua_getfield(L, idx, "Url");
    if (lua_type(L, -1) != LUA_TSTRING)
        raise(L, "RequestInternal: Url is required");
    request.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "Method");
    if (lua_type(L, -1) == LUA_TSTRING)
        request.method = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "Body");
    if (lua_type(L, -1) == LUA_TSTRING)
        request.body = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "Headers");
    if (lua_istable(L, -1))
        for (lua_pushnil(L); lua_next(L, -2) != 0; lua_pop(L, 1)) {
            if (lua_type(L, -2) != LUA_TSTRING || (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER))
                continue;
            const std::string_view name = lua_tostring(L, -2);
            const std::string value = lua_tostring(L, -1);
            if (name.find_first_of("\r\n:") != std::string_view::npos
                || value.find_first_of("\r\n") != std::string::npos)
                raise(L, std::format("RequestInternal: header {} isn't one line", name));
            request.headers.push_back(std::format("{}: {}", name, value));
        }
    lua_pop(L, 1);
    return request;
}

int requestInternal(lua_State* L)
{
    self(L);
    HttpRequest::Spec spec;
    assets::HttpRequest request = requestFrom(L, 2);
    spec.url = std::move(request.url);
    spec.method = std::move(request.method);
    spec.headers = std::move(request.headers);
    spec.body = std::move(request.body);
    auto made = std::make_shared<HttpRequest>();
    made->assignSpec(std::move(spec));
    return returnInstance(L, made.get());
}

// HttpRequest:Start(callback): sends it; callback(true, response) when an
// answer came (whatever its status), callback(false, {StatusMessage}) when
// none did. In a thread of its own, as the engine calls back.
int httpRequestStart(lua_State* L)
{
    auto& request = static_cast<HttpRequest&>(self(L));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (request.started())
        raise(L, "HttpRequest:Start: the request was already started");
    request.markStarted();
    const HttpRequest::Spec& spec = request.spec();
    auto fetch = startFetch(L, {.method = spec.method, .url = spec.url, .headers = spec.headers, .body = spec.body});
    Runtime& rt = Runtime::of(L);
    lua_pushvalue(L, 2);
    const int callback = lua_ref(L, -1);
    lua_pop(L, 1);
    whenDone(rt, std::move(fetch),
             [&rt, callback, access = rt.accessOf(L), globals = Runtime::refGlobals(L),
              keep = std::static_pointer_cast<HttpRequest>(request.shared_from_this())](const Fetch& done) {
                 const Result& result = done.result;
                 lua_State* main = rt.main();
                 if (keep->cancelled()) {
                     lua_unref(main, callback);
                     lua_unref(main, globals);
                     return;
                 }
                 lua_State* thread = lua_newthread(main);
                 rt.setAccess(thread, access);
                 Runtime::useGlobals(thread, globals);
                 const int ref = lua_ref(main, -1);
                 lua_pop(main, 1);
                 lua_getref(thread, callback);
                 lua_pushboolean(thread, result.has_value());
                 if (result) {
                     pushResponse(thread, *result, done.seconds);
                 } else {
                     lua_createtable(thread, 0, 1);
                     pushString(thread, result.error());
                     lua_setfield(thread, -2, "StatusMessage");
                 }
                 if (auto error = rt.resume(thread, 2, "HttpRequest:Start"))
                     rt.report(*error);
                 lua_unref(main, ref);
                 lua_unref(main, callback);
                 lua_unref(main, globals);
             });
    return 0;
}

int httpRequestCancel(lua_State* L)
{
    static_cast<HttpRequest&>(self(L)).cancel();
    return 0;
}

// Yields until the answer, then returns its body; a failure raises where it
// was called. HTTP error statuses raise too, as HttpRbxApiService's do.
int yieldForBody(lua_State* L, assets::HttpRequest request, std::string what)
{
    checkYieldable(L);
    auto fetch = startFetch(L, std::move(request));
    Runtime& rt = Runtime::of(L);
    const std::uint64_t token = rt.suspend(L);
    whenDone(rt, std::move(fetch), [&rt, L, token, what = std::move(what)](const Fetch& done) {
        const Result& result = done.result;
        if (!rt.isSuspended(L, token))
            return;
        if (!result)
            rt.fail(L, token, std::format("{} failed: {}", what, result.error()), what);
        else if (result->status < 200 || result->status >= 300)
            rt.fail(L, token, std::format("HTTP {} ({})", result->status, statusMessage(result->status)), what);
        else
            rt.wake(
                L, token,
                [&](lua_State* t) {
                    pushString(t, result->body);
                    return 1;
                },
                what);
    });
    return lua_yield(L, 0);
}

int apiGetAsyncFullUrl(lua_State* L)
{
    self(L);
    return yieldForBody(L, {.url = std::string(checkString(L, 2))}, "GetAsyncFullUrl");
}

int apiPostAsyncFullUrl(lua_State* L)
{
    self(L);
    return yieldForBody(L,
                        {.method = "POST",
                         .url = std::string(checkString(L, 2)),
                         .headers = {"Content-Type: application/json"},
                         .body = std::string(checkString(L, 3))},
                        "PostAsyncFullUrl");
}

// GetAsync(path) and PostAsync(path, data): paths on the API site.
int apiGetAsync(lua_State* L)
{
    self(L);
    return yieldForBody(L, {.url = std::format("https://api.roblox.com/{}", checkString(L, 2))}, "GetAsync");
}

int apiPostAsync(lua_State* L)
{
    self(L);
    return yieldForBody(L,
                        {.method = "POST",
                         .url = std::format("https://api.roblox.com/{}", checkString(L, 2)),
                         .headers = {"Content-Type: application/json"},
                         .body = std::string(checkString(L, 3))},
                        "PostAsync");
}

// AvatarEditorService exposes the web rules with PascalCase dictionary keys,
// including nested palettes, scale limits and accessory refinement bounds.
Packed pascalJsonValue(const assets::json::Value& value)
{
    if (const auto* object = value.object()) {
        auto table = std::make_shared<PackedTable>();
        for (const auto& [name, member] : *object) {
            std::string key = name;
            if (!key.empty() && key.front() >= 'a' && key.front() <= 'z')
                key.front() = static_cast<char>(key.front() - 'a' + 'A');
            table->emplace_back(packed(Value(std::move(key))), pascalJsonValue(member));
        }
        return Packed{Value(), std::move(table)};
    }
    if (const auto* array = value.array()) {
        auto table = std::make_shared<PackedTable>();
        double index = 0;
        for (const auto& item : *array)
            table->emplace_back(packed(Value(++index)), pascalJsonValue(item));
        return Packed{Value(), std::move(table)};
    }
    if (const auto* text = value.string())
        return packed(Value(*text));
    if (const auto boolean = value.boolean())
        return packed(Value(*boolean));
    if (const auto number = value.number())
        return packed(Value(*number));
    return {};
}

using JsonReply = std::expected<Packed, std::string>;
using JsonDecoder = std::function<JsonReply(const assets::json::Value&)>;

int yieldForJson(lua_State* L, std::string url, std::string what, JsonDecoder decode, bool returnValue = true,
                 bool anonymous = true)
{
    checkYieldable(L);
    auto fetch = startFetch(L, {.url = std::move(url)}, anonymous);
    Runtime& rt = Runtime::of(L);
    const std::uint64_t token = rt.suspend(L);
    whenDone(rt, std::move(fetch),
             [&rt, L, token, what = std::move(what), decode = std::move(decode), returnValue](const Fetch& done) {
                 if (!rt.isSuspended(L, token))
                     return;
                 const auto& result = done.result;
                 if (!result) {
                     rt.fail(L, token, std::format("{} failed: {}", what, result.error()), what);
                 } else if (result->status != 200) {
                     rt.fail(L, token, std::format("{} failed: HTTP {}", what, result->status), what);
                 } else if (auto value = assets::json::parse(result->body); value && value->object()) {
                     if (auto decoded = decode(*value)) {
                         rt.wake(
                             L, token,
                             [&](lua_State* thread) {
                                 if (returnValue)
                                     unpack(thread, *decoded);
                                 return returnValue ? 1 : 0;
                             },
                             what);
                     } else {
                         rt.fail(L, token, std::format("{} failed: {}", what, decoded.error()), what);
                     }
                 } else {
                     rt.fail(L, token, std::format("{} returned invalid JSON", what), what);
                 }
             });
    return lua_yield(L, 0);
}

int getPlayerPolicy(lua_State* L)
{
    self(L);
    auto* player = instanceCast<Player>(toInstance(L, 2));
    if (!player)
        raise(L, "Instance was not a player");
    Runtime& rt = Runtime::of(L);
    const auto* players = instanceCast<Players>(rt.game().findService("Players"));
    if (!players)
        raise(L, "Players not found");
    if (players->localPlayer() != player)
        raise(L, "This method cannot be called on the client for a non-local player");
    if (player->userId() <= 0)
        raise(L, "Policy info is unavailable for a local test player");
    auto& policies = rt.shared().playerPolicies;
    if (const auto cached = policies.find(player->userId());
        cached != policies.end() && cached->second.expires > rt.now()) {
        unpack(L, cached->second.fields);
        return 1;
    }
    // Native 0.742 URL builder sub_10060daeb: authenticated client policies
    // come from this fixed endpoint and are returned to Lua in PascalCase.
    return yieldForJson(
        L, "https://apis.roblox.com/player-policy-service/v1/player-policy-client",
        "PolicyService:GetPolicyInfoForPlayerAsync",
        [&rt, userId = player->userId()](const assets::json::Value& value) -> JsonReply {
            const auto* trading = value["isPaidItemTradingAllowed"];
            const auto* random = value["arePaidRandomItemsRestricted"];
            if (!trading || !trading->boolean() || !random || !random->boolean())
                return std::unexpected("player policy response is incomplete");
            Packed fields = pascalJsonValue(value);
            rt.shared().playerPolicies.insert_or_assign(userId, Shared::PlayerPolicy{rt.now() + 60, fields});
            return fields;
        },
        true, false);
}

int getAvatarRules(lua_State* L)
{
    self(L);
    return yieldForJson(L, "https://avatar.roblox.com/v1/avatar-rules", "AvatarEditorService:GetAvatarRulesAsync",
                        [](const assets::json::Value& value) -> JsonReply {
                            if (!value["scales"] || !value["wearableAssetTypes"])
                                return std::unexpected("no avatar rules in the reply");
                            return pascalJsonValue(value);
                        });
}

// 0.745 Players::getProfileConfigurationFromUserIdAsync requests just the
// Background and ProfileFrame selections from v4/avatar/users/{id}/.
// Unset selections yield an empty dictionary, not a fabricated asset id.
int getProfileConfiguration(lua_State* L)
{
    self(L);
    const double userId = luaL_checknumber(L, 2);
    if (!std::isfinite(userId) || userId <= 0 || userId >= 9.2e18 || std::floor(userId) != userId)
        raise(L, "Players:GetProfileConfigurationFromUserIdAsync() got invalid userId");
    return yieldForJson(
        L,
        std::format(
            "https://avatar.roblox.com/v4/avatar/users/{}/?selectionTypes=Background&selectionTypes=ProfileFrame",
            static_cast<std::int64_t>(userId)),
        "Players:GetProfileConfigurationFromUserIdAsync", [](const assets::json::Value& value) -> JsonReply {
            const auto* config = value["avatarConfigurations"];
            if (!config || !config->object())
                return std::unexpected("no avatar configurations in the reply");
            auto table = std::make_shared<PackedTable>();
            auto asset = [&](const char* selection, const char* field, const char* key) {
                const auto* selected = (*config)[selection];
                const auto* item = selected ? (*selected)[field] : nullptr;
                const auto* id = item ? (*item)["id"] : nullptr;
                if (id && id->integer())
                    table->emplace_back(packed(Value(std::string(key))),
                                        packed(Value(static_cast<double>(*id->integer()))));
            };
            asset("background", "backgroundAsset", "BackgroundAssetId");
            asset("profileFrame", "frameAsset", "FrameAssetId");
            return Packed{Value(), std::move(table)};
        });
}

// Catalog search uses the same query fields as the native 0.745 client
// (0x1037c9430), including enum names rather than the public enum numbers.
int catalogParamsNew(lua_State* L)
{
    CatalogSearchParams params;
    auto& fields = params.fields;
    for (const char* name : {"SearchKeyword", "CreatorName"})
        fields.emplace(name, packed(Value(std::string())));
    for (const auto& [name, number] : std::array<std::pair<const char*, double>, 4>{
             {{"MinPrice", 0}, {"MaxPrice", 2147483647}, {"CreatorId", 0}, {"Limit", 30}}})
        fields.emplace(name, packed(Value(number)));
    fields.emplace("IncludeOffSale", packed(Value(false)));
    for (const auto& [name, type, item] :
         std::array<std::array<const char*, 3>, 5>{{{{"SortType", "CatalogSortType", "Relevance"}},
                                                    {{"SortAggregation", "CatalogSortAggregation", "AllTime"}},
                                                    {{"CategoryFilter", "CatalogCategoryFilter", "None"}},
                                                    {{"SalesTypeFilter", "SalesTypeFilter", "All"}},
                                                    {{"CreatorType", "CreatorTypeFilter", "All"}}}})
        fields.emplace(name, packed(Value(enumItem(type, item))));
    for (const char* name : {"AssetTypes", "BundleTypes"})
        fields.emplace(name, Packed{Value(), std::make_shared<PackedTable>()});
    push<CatalogSearchParams>(L, std::move(params));
    return 1;
}

int catalogParamsIndex(lua_State* L)
{
    const auto& fields = check<CatalogSearchParams>(L, 1).fields;
    const std::string_view key = checkString(L, 2);
    const auto it = fields.find(key);
    if (it == fields.end())
        raise(L, std::format("{} is not a valid member of CatalogSearchParams", key));
    unpack(L, it->second);
    return 1;
}

int catalogParamsNewIndex(lua_State* L)
{
    auto& fields = check<CatalogSearchParams>(L, 1).fields;
    const std::string_view key = checkString(L, 2);
    auto it = fields.find(key);
    if (it == fields.end())
        raise(L, std::format("{} is not a valid member of CatalogSearchParams", key));
    if (it->second.table) {
        luaL_checktype(L, 3, LUA_TTABLE);
        const char* type = key == "AssetTypes" ? "AvatarAssetType" : "BundleType";
        auto items = std::make_shared<PackedTable>();
        for (int i = 1; i <= lua_objlen(L, 3); ++i) {
            lua_rawgeti(L, 3, i);
            const auto item = toEnumItem(L, -1, type, true);
            if (!item)
                raise(L, std::format("{} must contain Enum.{} items", key, type));
            items->emplace_back(packed(Value(static_cast<double>(i))), packed(Value(*item)));
            lua_pop(L, 1);
        }
        it->second = Packed{Value(), std::move(items)};
        return 0;
    } else if (const auto* item = std::get_if<EnumItem>(&it->second.value)) {
        if (!toEnumItem(L, 3, item->enumType->name, true))
            raise(L, std::format("{} must be Enum.{}", key, item->enumType->name));
    } else if (std::holds_alternative<double>(it->second.value)) {
        const double number = luaL_checknumber(L, 3);
        if (!std::isfinite(number) || number < 0 || number >= 9.2e18 || std::floor(number) != number)
            raise(L, std::format("{} must be a non-negative integer", key));
        it->second = packed(Value(number));
        return 0;
    } else {
        luaL_checktype(L, 3, std::holds_alternative<bool>(it->second.value) ? LUA_TBOOLEAN : LUA_TSTRING);
    }
    it->second = pack(L, 3);
    return 0;
}

std::string queryText(std::string_view text)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-'
            || byte == '_' || byte == '.' || byte == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4u];
            out += hex[byte & 15u];
        }
    }
    return out;
}

class Pages : public InstanceImpl<Pages, Instance, ClassKind::NotCreatable> {};

class CatalogPages final : public InstanceImpl<CatalogPages, Pages, ClassKind::NotCreatable> {
public:
    LUNAR_PROPERTY() bool isFinished() const { return nextCursor.empty(); }
    Packed items;
    std::string url;
    std::string nextCursor;
};

JsonReply readCatalogPage(const assets::json::Value& value, const std::shared_ptr<CatalogPages>& pages)
{
    const auto* data = value["data"];
    if (!data || !data->array())
        return std::unexpected("no catalog items in the reply");
    pages->items = pascalJsonValue(*data);
    const auto* cursor = value["nextPageCursor"];
    pages->nextCursor = cursor && cursor->string() ? *cursor->string() : std::string();
    return packed(Value(std::static_pointer_cast<Instance>(pages)));
}

int searchCatalog(lua_State* L)
{
    self(L);
    const auto& params = check<CatalogSearchParams>(L, 2);
    std::string query = "https://catalog.roblox.com/v2/search/items/details?";
    auto append = [&](std::string_view key, std::string_view value) {
        if (query.back() != '?')
            query += '&';
        query += key;
        query += '=';
        query += queryText(value);
    };
    for (const auto& [field, key] :
         std::array<std::pair<const char*, const char*>, 14>{{{"SearchKeyword", "model.keyword"},
                                                              {"MinPrice", "model.minPrice"},
                                                              {"MaxPrice", "model.maxPrice"},
                                                              {"Limit", "limit"},
                                                              {"SortType", "model.sortType"},
                                                              {"SortAggregation", "model.sortAggregation"},
                                                              {"CategoryFilter", "model.categoryFilter"},
                                                              {"SalesTypeFilter", "model.salesTypeFilter"},
                                                              {"CreatorName", "model.creatorName"},
                                                              {"CreatorId", "model.creatorTargetId"},
                                                              {"CreatorType", "model.creatorType"},
                                                              {"IncludeOffSale", "model.includeNotForSale"},
                                                              {"AssetTypes", "model.assetTypeIds"},
                                                              {"BundleTypes", "model.bundleTypeIds"}}}) {
        const Packed& value = params.fields.at(field);
        if (value.table) {
            for (const auto& [index, item] : *value.table) {
                (void)index;
                append(key, std::to_string(std::get<EnumItem>(item.value).value));
            }
        } else if (const auto* item = std::get_if<EnumItem>(&value.value)) {
            if ((std::string_view(field) == "CategoryFilter" && item->name == "None")
                || (std::string_view(field) == "CreatorType" && item->name == "All"))
                continue;
            append(key, item->name);
        } else if (const auto* text = std::get_if<std::string>(&value.value)) {
            if (!text->empty())
                append(key, *text);
        } else if (const auto* number = std::get_if<double>(&value.value)) {
            if (*number > 0 && (std::string_view(field) != "MaxPrice" || *number != 2147483647))
                append(key, std::format("{:.0f}", *number));
        } else if (const auto* boolean = std::get_if<bool>(&value.value)) {
            append(key, *boolean ? "true" : "false");
        }
    }
    auto pages = std::make_shared<CatalogPages>();
    pages->url = query;
    return yieldForJson(L, std::move(query), "AvatarEditorService:SearchCatalogAsync",
                        [pages](const assets::json::Value& value) { return readCatalogPage(value, pages); });
}

int catalogGetCurrentPage(lua_State* L)
{
    const auto* pages = dynamic_cast<CatalogPages*>(&self(L));
    if (!pages)
        raise(L, "GetCurrentPage requires CatalogPages");
    unpack(L, pages->items);
    return 1;
}

int catalogAdvance(lua_State* L)
{
    auto* pages = dynamic_cast<CatalogPages*>(&self(L));
    if (!pages || pages->isFinished())
        raise(L, "No more catalog pages");
    auto keep = std::static_pointer_cast<CatalogPages>(pages->shared_from_this());
    return yieldForJson(
        L, pages->url + "&cursor=" + queryText(pages->nextCursor), "CatalogPages:AdvanceToNextPageAsync",
        [keep](const assets::json::Value& value) -> JsonReply {
            auto result = readCatalogPage(value, keep);
            return result ? JsonReply(Packed()) : std::unexpected(result.error());
        },
        false);
}

// The supported Mac client's agent string, also used by the join.
int getUserAgent(lua_State* L)
{
    self(L);
    pushString(L, lunar::robloxMacUserAgent);
    return 1;
}

constexpr auto methods = std::to_array<Method>({
    {"GetPolicyInfoForPlayerAsync", "PolicyService", getPlayerPolicy},
    {"SearchCatalogAsync", "AvatarEditorService", searchCatalog},
    {"SearchCatalog", "AvatarEditorService", searchCatalog},
    {"GetCurrentPage", "CatalogPages", catalogGetCurrentPage},
    {"AdvanceToNextPageAsync", "CatalogPages", catalogAdvance},
    {"GetAvatarRulesAsync", "AvatarEditorService", getAvatarRules},
    {"GetAvatarRules", "AvatarEditorService", getAvatarRules},
    {"GetProfileConfigurationFromUserIdAsync", "Players", getProfileConfiguration},
    {"RequestInternal", "HttpService", requestInternal},
    {"GetUserAgent", "HttpService", getUserAgent},
    {"Start", "HttpRequest", httpRequestStart},
    {"Cancel", "HttpRequest", httpRequestCancel},
    {"GetAsyncFullUrl", "HttpRbxApiService", apiGetAsyncFullUrl},
    {"PostAsyncFullUrl", "HttpRbxApiService", apiPostAsyncFullUrl},
    {"GetAsync", "HttpRbxApiService", apiGetAsync},
    {"PostAsync", "HttpRbxApiService", apiPostAsync},
});

} // namespace

std::span<const Method> appHttpMethods()
{
    return methods;
}

void openAppHttp(lua_State* L)
{
    registerUserdata<CatalogSearchParams>(L, catalogParamsIndex, {},
                                          std::to_array<luaL_Reg>({{"__newindex", catalogParamsNewIndex}}));
    setGlobalTable(L, "CatalogSearchParams", std::to_array<luaL_Reg>({{"new", catalogParamsNew}}));
}

} // namespace lunar::lua
