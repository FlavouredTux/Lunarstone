// What the client's own Lua app (the universal app: its home screen and the
// rest, app/LuaApp) asks of the engine beyond what games use.

#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/RobloxVersion.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Character.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lunar::lua {

namespace {

// --- Fast flags --------------------------------------------------------------------
//
// The client's FFlags, FInts and FStrings: defined by the Lua code with a
// default (game:DefineFastFlag) and read back by name. Their values are
// Roblox's client settings when loaded (loadClientSettings), else the
// engine's own defaults, else the Lua code's. Process-wide, as the client's are.

using FlagValue = std::variant<bool, double, std::string>;

struct Flags {
    std::mutex mutex;
    std::unordered_map<std::string, FlagValue> values;
    // What Roblox's client settings say, by the flag's bare name (FFlagX,
    // DFIntX... all as X), as text (loadClientSettings).
    std::unordered_map<std::string, std::string> served;
};

Flags& flags()
{
    static Flags f;
    return f;
}

// Engine variables the app reads with settings():GetFVariable that the
// client settings leave at the 0.742 binary's own defaults (each one's
// static initializer stores the value next to the name).
constexpr std::array<std::pair<std::string_view, std::string_view>, 12> engineDefaults{{
    {"CaptchaTimeout", "20"},
    {"LuaAnalyticsReleasePeriod", "60"},
    {"LuaAppErrorReporterRateLimitIntervalInSec", "5"},
    {"LuaAppErrorReporterRateLimitThreshold", "10"},
    {"LuaAppGameDetailsRefreshIntervalInSeconds", "1800"},
    {"LuaAppNonFinalThumbnailMaxRetries", "3"},
    {"LuaAppThumbnailsApiRetryTimeMultiplier", "2"},
    {"LuaErrorsInfluxThrottling", "1"},
    // GUESS: these two take a register set far up their initializer; 0.
    {"LuaAppLoginMethod", "0"},
    {"PercentReportingGamesListRTT", "0"},
    // The rest the app reads are counter and series names for analytics,
    // which go nowhere here: empty.
    {"LuaAppDefaultPageOverride", ""},
    {"LuaErrorsInfluxSeries", ""},
}};

// The served or built-in text for a flag, if there's any.
std::optional<std::string> knownText(const Flags& f, const std::string& name)
{
    if (auto it = f.served.find(name); it != f.served.end())
        return it->second;
    for (const auto& [key, value] : engineDefaults)
        if (key == name)
            return std::string(value);
    return std::nullopt;
}

// Text as a value of the fallback's type, or the fallback when it isn't one.
FlagValue fromText(const std::string& text, const FlagValue& fallback)
{
    if (std::holds_alternative<bool>(fallback)) {
        if (text == "True" || text == "true")
            return true;
        if (text == "False" || text == "false")
            return false;
        return fallback;
    }
    if (std::holds_alternative<double>(fallback)) {
        double number = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        return ec == std::errc() && end == text.data() + text.size() ? FlagValue(number) : fallback;
    }
    return text;
}

// The flag's value: fixed the first time it's asked for, from the client
// settings, else the engine's default, else `fallback` (the Lua code's own).
FlagValue define(std::string_view name, FlagValue fallback)
{
    Flags& f = flags();
    const std::scoped_lock lock(f.mutex);
    const std::string key(name);
    if (auto it = f.values.find(key); it != f.values.end())
        return it->second;
    if (auto text = knownText(f, key))
        fallback = fromText(*text, fallback);
    return f.values.emplace(key, std::move(fallback)).first->second;
}

// --- Client settings JSON ----------------------------------------------------------
//
// {"applicationSettings": {"FFlagX": "True", ...}}: an object whose values the
// reader keeps when they're strings and skips otherwise.

struct JsonReader {
    std::string_view text;
    std::size_t at = 0;
    bool bad = false;

    void space()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
            ++at;
    }
    bool eat(char c)
    {
        space();
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    std::optional<std::string> string()
    {
        if (!eat('"'))
            return std::nullopt;
        std::string out;
        while (at < text.size() && text[at] != '"') {
            char c = text[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size())
                break;
            c = text[at++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned code = 0;
                if (at + 4 > text.size()
                    || std::from_chars(text.data() + at, text.data() + at + 4, code, 16).ec != std::errc()) {
                    bad = true;
                    return std::nullopt;
                }
                at += 4;
                // UTF-8; surrogate pairs stay as two (the flags are ASCII).
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xc0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (code & 0x3f));
                }
                break;
            }
            default: out += c; break;
            }
        }
        if (!eat('"')) {
            bad = true;
            return std::nullopt;
        }
        return out;
    }
    // Any value, read past.
    void skip(int depth = 0)
    {
        space();
        if (at >= text.size() || depth > 64) {
            bad = true;
            return;
        }
        if (text[at] == '"') {
            string();
        } else if (text[at] == '{' || text[at] == '[') {
            const char close = text[at] == '{' ? '}' : ']';
            ++at;
            if (eat(close))
                return;
            do {
                if (close == '}') {
                    string();
                    if (!eat(':')) {
                        bad = true;
                        return;
                    }
                }
                skip(depth + 1);
            } while (!bad && eat(','));
            if (!eat(close))
                bad = true;
        } else {
            while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']')
                ++at;
        }
    }
};

// The flag's name without its kind: FFlag, DFInt, SFString and the rest.
std::string_view bareFlagName(std::string_view name)
{
    for (std::string_view kind : {"DFFlag", "DFInt", "DFString", "DFLog", "SFFlag", "SFInt", "SFString", "FFlag",
                                  "FInt", "FString", "FLog"})
        if (name.starts_with(kind) && name.size() > kind.size())
            return name.substr(kind.size());
    return name;
}

void pushFlag(lua_State* L, const FlagValue& value)
{
    if (const bool* b = std::get_if<bool>(&value))
        lua_pushboolean(L, *b);
    else if (const double* d = std::get_if<double>(&value))
        lua_pushnumber(L, *d);
    else
        lua_pushlstring(L, std::get<std::string>(value).data(), std::get<std::string>(value).size());
}

// game:DefineFastFlag(name, default), DefineFastInt, DefineFastString: the
// flag's value (its default here).
int defineFastFlag(lua_State* L)
{
    self(L);
    pushFlag(L, define(checkString(L, 2), lua_toboolean(L, 3) != 0));
    return 1;
}

int defineFastInt(lua_State* L)
{
    self(L);
    pushFlag(L, define(checkString(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

int defineFastString(lua_State* L)
{
    self(L);
    pushFlag(L, define(checkString(L, 2), std::string(checkString(L, 3))));
    return 1;
}

// game:GetFastFlag(name) and the others: a flag the Lua code defined, else
// GUESS the type's zero (the client's own C++ flags aren't known here).
template <class T>
int getFast(lua_State* L)
{
    self(L);
    pushFlag(L, define(checkString(L, 2), T{}));
    return 1;
}

// settings():GetFFlag(name) and GetFVariable(name): the same flags, as a
// boolean and as text.
int globalGetFFlag(lua_State* L)
{
    self(L);
    const FlagValue value = define(checkString(L, 2), false);
    const bool* b = std::get_if<bool>(&value);
    lua_pushboolean(L, b ? *b : false);
    return 1;
}

int globalGetFVariable(lua_State* L)
{
    self(L);
    const FlagValue value = define(checkString(L, 2), std::string());
    if (const bool* b = std::get_if<bool>(&value))
        lua_pushstring(L, *b ? "True" : "False");
    else if (const double* d = std::get_if<double>(&value))
        lua_pushstring(L, std::format("{}", *d).c_str());
    else
        pushString(L, std::get<std::string>(value));
    return 1;
}

// --- Helpers -----------------------------------------------------------------------

// Calls the function at fnIdx with `args` in a thread of its own once the
// current call has returned, the way the engine runs a callback it fires.
void deferCall(lua_State* L, int fnIdx, PackedArgs args, std::string_view what)
{
    luaL_checktype(L, fnIdx, LUA_TFUNCTION);
    Runtime& rt = Runtime::of(L);
    lua_State* thread = lua_newthread(L);
    rt.setAccess(thread, rt.accessOf(L));
    lua_pushvalue(L, fnIdx);
    lua_xmove(L, thread, 1);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    rt.defer([&rt, thread, ref, args = std::move(args), what = std::string(what)] {
        const int count = unpackArgs(thread, args);
        if (auto error = rt.resume(thread, count, what))
            rt.report(*error);
        lua_unref(rt.main(), ref);
    });
}

// owner's hub event `event`:Connect(fn) (or Once), leaving the connection on
// the stack.
void connectHub(lua_State* L, Instance& owner, std::string_view event, int fnIdx, bool once)
{
    luaL_checktype(L, fnIdx, LUA_TFUNCTION);
    pushHubSignal(L, owner, std::string(event));
    lua_getfield(L, -1, once ? "Once" : "Connect");
    lua_pushvalue(L, -2);
    lua_pushvalue(L, fnIdx);
    lua_call(L, 2, 1);
    lua_remove(L, -2);
}

// Enum.<type>.<item>, for the enums only the API data has.
void pushEnum(lua_State* L, const char* type, const char* item)
{
    lua_getglobal(L, "Enum");
    lua_getfield(L, -1, type);
    lua_getfield(L, -1, item);
    lua_remove(L, -2);
    lua_remove(L, -2);
}

// A string argument that may be missing: `fallback` then.
std::string optString(lua_State* L, int idx, std::string_view fallback = {})
{
    return lua_isnoneornil(L, idx) ? std::string(fallback) : std::string(checkString(L, idx));
}

// --- MessageBusService -------------------------------------------------------------
//
// The app's line to the native shell around it: messages by id that the Lua
// side and the host publish and subscribe to. Here it's a bus inside the
// app's DataModel: each id is a hub event of the service ("bus:<id>"), so
// subscriptions are ordinary RBXScriptConnections, and the last message per
// id is kept for GetLast and sticky subscriptions. Native functions (Call)
// and protocol requests (MakeRequest) have nobody behind them yet.

constexpr std::string_view busPrefix = "bus:";

std::string busEvent(std::string_view mid)
{
    return std::format("{}{}", busPrefix, mid);
}

struct BusState {
    std::mutex mutex;
    std::unordered_map<std::string, Packed> last; // by message id
};

BusState& busState()
{
    static BusState s;
    return s;
}

// GUESS: the ids are the names joined with dots; nothing outside sees them.
int busGetMessageId(lua_State* L)
{
    self(L);
    const std::string id = std::format("{}.{}", checkString(L, 2), checkString(L, 3));
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

template <bool request>
int busGetProtocolMessageId(lua_State* L)
{
    self(L);
    const std::string id
        = std::format("{}.{}.{}", checkString(L, 2), checkString(L, 3), request ? "request" : "response");
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

void publish(lua_State* L, Instance& bus, std::string_view mid, int paramsIdx)
{
    Packed params = pack(L, paramsIdx);
    {
        BusState& s = busState();
        const std::scoped_lock lock(s.mutex);
        s.last.insert_or_assign(std::string(mid), params);
    }
    Runtime::of(L).shared().events.fire(bus, busEvent(mid), PackedArgs{std::move(params)});
}

// Subscribe(mid, callback, once, sticky): the callback hears each message
// published to mid from now on (with sticky, the last one already sent too).
void subscribe(lua_State* L, Instance& bus, std::string_view mid, int callbackIdx, bool once, bool sticky)
{
    std::optional<Packed> last;
    if (sticky) {
        BusState& s = busState();
        const std::scoped_lock lock(s.mutex);
        if (auto it = s.last.find(std::string(mid)); it != s.last.end())
            last = it->second;
    }
    connectHub(L, bus, busEvent(mid), callbackIdx, once);
    if (last) // as if published again for this subscriber alone
        deferCall(L, callbackIdx, PackedArgs{std::move(*last)}, "MessageBusService sticky message");
}

void requestLaunch(lua_State* L, int paramsIdx, std::string_view via);

int busPublish(lua_State* L)
{
    Instance& bus = self(L);
    const std::string_view mid = checkString(L, 2);
    // The app's other way to launch a game: a message to the shell.
    if (mid == "Game.launch" && lua_istable(L, 3))
        requestLaunch(L, 3, "MessageBusService Game.launch");
    publish(L, bus, mid, 3);
    return 0;
}

int busSubscribe(lua_State* L)
{
    Instance& bus = self(L);
    subscribe(L, bus, checkString(L, 2), 3, lua_toboolean(L, 4) != 0, lua_toboolean(L, 5) != 0);
    return 1;
}

template <bool request>
int busSubscribeProtocol(lua_State* L)
{
    Instance& bus = self(L);
    const std::string mid
        = std::format("{}.{}.{}", checkString(L, 2), checkString(L, 3), request ? "request" : "response");
    subscribe(L, bus, mid, 4, lua_toboolean(L, 5) != 0, lua_toboolean(L, 6) != 0);
    return 1;
}

template <bool request>
int busPublishProtocol(lua_State* L)
{
    Instance& bus = self(L);
    const std::string mid
        = std::format("{}.{}.{}", checkString(L, 2), checkString(L, 3), request ? "request" : "response");
    publish(L, bus, mid, 4);
    return 0;
}

int busGetLast(lua_State* L)
{
    self(L);
    const std::string mid(checkString(L, 2));
    BusState& s = busState();
    std::optional<Packed> last;
    {
        const std::scoped_lock lock(s.mutex);
        if (auto it = s.last.find(mid); it != s.last.end())
            last = it->second;
    }
    if (last)
        unpack(L, *last);
    else
        lua_pushnil(L);
    return 1;
}

int nothing(lua_State* L)
{
    self(L);
    return 0;
}

int nilResult(lua_State* L)
{
    self(L);
    lua_pushnil(L);
    return 1;
}

// --- MemStorageService -------------------------------------------------------------
//
// Strings by key that the app's DataModels share while the client runs, with
// callbacks bound to a key (a hub event of the service, "mem:<key>").

struct MemState {
    std::mutex mutex;
    std::unordered_map<std::string, std::string> items;
};

MemState& memState()
{
    static MemState s;
    return s;
}

std::optional<std::string> memItem(const std::string& key)
{
    MemState& s = memState();
    const std::scoped_lock lock(s.mutex);
    auto it = s.items.find(key);
    return it == s.items.end() ? std::nullopt : std::optional(it->second);
}

std::string memEvent(std::string_view key)
{
    return std::format("mem:{}", key);
}

void memFire(lua_State* L, Instance& service, std::string_view key, std::string value)
{
    Runtime::of(L).shared().events.fire(service, memEvent(key), PackedArgs{packed(Value(std::move(value)))});
}

int memHasItem(lua_State* L)
{
    self(L);
    lua_pushboolean(L, memItem(std::string(checkString(L, 2))).has_value());
    return 1;
}

int memGetItem(lua_State* L)
{
    self(L);
    const auto item = memItem(std::string(checkString(L, 2)));
    pushString(L, item ? *item : optString(L, 3));
    return 1;
}

int memSetItem(lua_State* L)
{
    Instance& service = self(L);
    const std::string key(checkString(L, 2));
    std::string value = optString(L, 3);
    {
        MemState& s = memState();
        const std::scoped_lock lock(s.mutex);
        s.items.insert_or_assign(key, value);
    }
    memFire(L, service, key, std::move(value));
    return 0;
}

int memRemoveItem(lua_State* L)
{
    self(L);
    MemState& s = memState();
    const std::scoped_lock lock(s.mutex);
    lua_pushboolean(L, s.items.erase(std::string(checkString(L, 2))) > 0);
    return 1;
}

// Fire(key, value): the bound callbacks hear value; nothing is stored.
int memFireMethod(lua_State* L)
{
    Instance& service = self(L);
    memFire(L, service, checkString(L, 2), optString(L, 3));
    return 0;
}

// Bind(key, callback) and BindAndFire, which also calls back at once with
// the key's value when it has one (GUESS: nothing when it hasn't).
template <bool andFire>
int memBind(lua_State* L)
{
    Instance& service = self(L);
    const std::string key(checkString(L, 2));
    connectHub(L, service, memEvent(key), 3, false);
    if (andFire)
        if (auto item = memItem(key))
            deferCall(L, 3, PackedArgs{packed(Value(std::move(*item)))}, "MemStorageService:BindAndFire");
    return 1;
}

// --- LocalStorageService / AppStorageService ----------------------------------------
//
// The app's settings that last between runs on the real client. GUESS: kept
// for this run only; nothing reads them back next time yet.

struct LocalState {
    std::mutex mutex;
    std::unordered_map<std::string, std::string> items;
};

LocalState& localState()
{
    static LocalState s;
    return s;
}

int localGetItem(lua_State* L)
{
    self(L);
    LocalState& s = localState();
    const std::scoped_lock lock(s.mutex);
    auto it = s.items.find(std::string(checkString(L, 2)));
    pushString(L, it == s.items.end() ? std::string_view() : std::string_view(it->second));
    return 1;
}

int localSetItem(lua_State* L)
{
    Instance& service = self(L);
    std::string key(checkString(L, 2));
    std::string value = optString(L, 3);
    {
        LocalState& s = localState();
        const std::scoped_lock lock(s.mutex);
        s.items.insert_or_assign(key, value);
    }
    Runtime::of(L).shared().events.fire(service, "ItemWasSet",
                                        PackedArgs{packed(Value(std::move(key))), packed(Value(std::move(value)))});
    return 0;
}

// WhenLoaded(callback): the store is always loaded here, so right away.
int localWhenLoaded(lua_State* L)
{
    self(L);
    deferCall(L, 2, {}, "LocalStorageService:WhenLoaded");
    return 0;
}

// --- IXPService ----------------------------------------------------------------------
//
// Roblox's experiments: which variant of a feature this user gets. Every
// layer is loaded and empty, so the app takes each feature's default.

int ixpStatus(lua_State* L)
{
    self(L);
    pushEnum(L, "IXPLoadingStatus", "Initialized");
    return 1;
}

int emptyTable(lua_State* L)
{
    self(L);
    lua_newtable(L);
    return 1;
}

// --- DataModel:GetEngineFeature ------------------------------------------------------
//
// Whether the engine has a feature the Lua code may lean on (204 names in the
// 0.742 app). None: the app takes the paths an engine without them needs.

int getEngineFeature(lua_State* L)
{
    self(L);
    checkString(L, 2);
    lua_pushboolean(L, false);
    return 1;
}

// --- GuiService ----------------------------------------------------------------------

// The notification types the app broadcasts to the native shell, in the
// order the 0.742 client's strings list them. GUESS: numbered from 0 in
// that order (the shell is ours, so only consistency matters).
constexpr std::array notificationTypes{
    "VIEW_PROFILE",
    "REPORT_ABUSE",
    "VIEW_GAME_DETAILS",
    "SHOW_TAB_BAR",
    "HIDE_TAB_BAR",
    "UNREAD_COUNT",
    "PRIVACY_SETTINGS",
    "BACK_BUTTON_NOT_CONSUMED",
    "PURCHASE_ROBUX",
    "VIEW_NOTIFICATIONS",
    "APP_READY",
    "CLOSE_MODAL",
    "VIEW_GAME_DETAILS_ANIMATED",
    "LAUNCH_GAME",
    "VIEW_MY_FEED",
    "SEARCH_GAMES",
    "GAMES_SEE_ALL",
    "ACTION_LOG_OUT",
    "LAUNCH_CONVERSATION",
    "UNIVERSAL_FRIENDS",
    "OPEN_CUSTOM_WEBVIEW",
    "OPEN_SOCIAL_MEDIA",
    "SET_STATUS_BAR_STYLE",
    "OPEN_YOUTUBE_VIDEO",
    "DID_LOG_OUT",
    "OPEN_BUILDERS_CLUB",
    "ACTION_LOG_IN",
    "OPEN_SETTINGS_WEBVIEW",
    "DID_LOG_IN",
    "OPEN_CAPTCHA_VIEW",
    "DID_SIGN_UP",
    "LUA_UNAUTHORIZED_LOG_OUT",
    "NATIVE_SHARE",
    "SDK_SHARE",
    "SDK_OPEN_URL",
    "NATIVE_EXIT",
    "RESTART_LUA_APP",
    "OPEN_EXTERNAL_BROWSER",
    "VERIFY_IDENTITY",
    "SCAN_QR_CODE",
    "SHOW_FRIEND_QR_CODE",
    "RESTART_APP",
    "DID_SWITCH_ACCOUNT",
    "GAME_STREAMING_STATUS_CHANGED",
    "POST_PURCHASE",
};

int guiGetNotificationTypeList(lua_State* L)
{
    self(L);
    lua_createtable(L, 0, static_cast<int>(notificationTypes.size()));
    for (std::size_t i = 0; i < notificationTypes.size(); ++i) {
        lua_pushnumber(L, static_cast<double>(i));
        lua_setfield(L, -2, notificationTypes[i]);
    }
    return 1;
}

// BroadcastNotification(data, type): a message for the native shell. Said
// in the output until the host acts on them (LAUNCH_GAME is the one that matters).
int guiBroadcastNotification(lua_State* L)
{
    self(L);
    const std::string data = optString(L, 2);
    const auto type = static_cast<std::size_t>(luaL_optnumber(L, 3, -1));
    const std::string_view name = type < notificationTypes.size() ? notificationTypes[type] : "?";
    Runtime::of(L).notice(MessageType::Info, std::format("GuiService:BroadcastNotification {} {}", name, data));
    return 0;
}

int guiOpenBrowserWindow(lua_State* L)
{
    self(L);
    Runtime::of(L).notice(MessageType::Info, std::format("GuiService:OpenBrowserWindow {}", optString(L, 2)));
    return 0;
}

int textBoxIsFocused(lua_State* L)
{
    // GuiEngine sets the cursor to -1 on blur and to a 1-based position on
    // focus, before it fires the corresponding TextBox event.
    const auto& box = static_cast<TextBox&>(self(L));
    lua_pushboolean(L, box.cursorPosition() >= 1);
    return 1;
}

int createHumanoidModel(lua_State* L)
{
    self(L);
    const auto* description = dynamic_cast<HumanoidDescription*>(toInstance(L, 2));
    if (!description)
        luaL_typeerror(L, 2, "HumanoidDescription");
    const auto rig = toEnumItem(L, 3, "HumanoidRigType", true);
    if (!rig)
        luaL_typeerror(L, 3, "Enum.HumanoidRigType");
    checkYieldable(L);
    Runtime& rt = Runtime::of(L);
    const auto token = rt.suspend(L);
    rt.after(0.0, [&rt, L, token, rigType = static_cast<Enum::HumanoidRigType>(rig->value),
                   keep = std::static_pointer_cast<const HumanoidDescription>(description->shared_from_this())] {
        if (!rt.isSuspended(L, token))
            return;
        auto model = avatar::createCharacter({.rigType = rigType, .name = "Model", .description = keep.get()});
        if (!model) {
            rt.fail(L, token, model.error(), "Players:CreateHumanoidModelFromDescriptionAsync");
            return;
        }
        if (Instance* humanoid = (*model)->findFirstChildOfClass("Humanoid"))
            if (InstancePtr applied = keep->clone())
                applied->setParent(humanoid);
        rt.wake(
            L, token, [&](lua_State* thread) { return returnInstance(thread, model->get()); },
            "Players:CreateHumanoidModelFromDescriptionAsync");
    });
    return lua_yield(L, 0);
}

int getAppliedDescription(lua_State* L)
{
    Instance& humanoid = self(L);
    const Instance* applied = humanoid.findFirstChildOfClass("HumanoidDescription");
    InstancePtr copy = applied ? applied->cloneForClient() : std::make_shared<HumanoidDescription>();
    return returnInstance(L, copy.get());
}

int applyHumanoidDescription(lua_State* L)
{
    Instance& humanoid = self(L);
    const auto* description = dynamic_cast<HumanoidDescription*>(toInstance(L, 2));
    auto* character = dynamic_cast<Model*>(humanoid.parent());
    if (!description || !character)
        raise(L, "ApplyDescription requires a HumanoidDescription and a Humanoid inside a Model");
    auto copy = std::dynamic_pointer_cast<HumanoidDescription>(description->cloneForClient());
    if (!copy)
        raise(L, "ApplyDescription could not clone the HumanoidDescription");
    checkYieldable(L);
    Runtime& rt = Runtime::of(L);
    const auto token = rt.suspend(L);
    rt.after(0.0, [&rt, L, token, copy = std::move(copy), keep = humanoid.shared_from_this(),
                   model = std::static_pointer_cast<Model>(character->shared_from_this())] {
        if (!rt.isSuspended(L, token))
            return;
        auto config = assets::AssetConfig::fromEnvironment();
        config.web = rt.shared().web;
        assets::AssetCache cache(std::move(config));
        for (const InstancePtr& child : std::vector(model->children().begin(), model->children().end()))
            if (child->isA("Accoutrement"))
                child->destroy();
        for (const auto& problem : avatar::applyDescription(*model, *copy, cache))
            rt.notice(MessageType::Warning, problem);
        if (Instance* previous = keep->findFirstChildOfClass("HumanoidDescription"))
            previous->destroy();
        copy->setParent(keep.get());
        rt.shared().events.fire(*keep, "ApplyDescriptionFinished", PackedArgs{packed(Value(copy))});
        rt.wake(L, token, [](lua_State*) { return 0; }, "Humanoid:ApplyDescriptionAsync");
    });
    return lua_yield(L, 0);
}

int humanoidGetAccessories(lua_State* L)
{
    const Instance& humanoid = self(L);
    lua_newtable(L);
    int index = 0;
    if (const auto* model = humanoid.parent())
        for (const auto& child : model->children())
            if (child->isA("Accoutrement")) {
                pushInstance(L, child.get());
                lua_rawseti(L, -2, ++index);
            }
    return 1;
}

int loadLocalAsset(lua_State* L)
{
    self(L);
    const std::string path(checkString(L, 2));
    if (!path.starts_with("rbxasset://"))
        raise(L, "LoadLocalAsset expects an rbxasset:// content path");
    auto config = assets::AssetConfig::fromEnvironment();
    config.download = false;
    assets::AssetCache cache(std::move(config));
    auto bytes = cache.fetch(path);
    if (!bytes)
        raise(L, bytes.error());
    auto loaded = loadModel(**bytes);
    if (!loaded || loaded->roots.empty())
        raise(L, loaded ? "LoadLocalAsset returned no instances" : loaded.error());
    if (loaded->roots.size() == 1)
        return returnInstance(L, loaded->roots.front().get());
    auto model = std::make_shared<Model>();
    for (const InstancePtr& root : loaded->roots)
        root->setParent(model.get());
    return returnInstance(L, model.get());
}

struct AccessorySlot {
    Enum::AccessoryType type;
    const char* property;
};

constexpr std::array accessorySlots{AccessorySlot{Enum::AccessoryType::Hat, "HatAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Hair, "HairAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Face, "FaceAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Neck, "NeckAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Shoulder, "ShouldersAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Front, "FrontAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Back, "BackAccessory"},
                                    AccessorySlot{Enum::AccessoryType::Waist, "WaistAccessory"}};

int descriptionSetAccessories(lua_State* L)
{
    auto& description = static_cast<HumanoidDescription&>(self(L));
    luaL_checktype(L, 2, LUA_TTABLE);
    const bool includeRigid = lua_toboolean(L, 3) != 0;
    std::vector<std::shared_ptr<AccessoryDescription>> accessories;
    for (int i = 1; i <= lua_objlen(L, 2); ++i) {
        lua_rawgeti(L, 2, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        auto item = std::make_shared<AccessoryDescription>();
        lua_getfield(L, -1, "AssetId");
        const double id = luaL_checknumber(L, -1);
        if (!std::isfinite(id) || id <= 0 || id >= 9.2e18 || std::floor(id) != id)
            raise(L, "SetAccessories requires positive asset ids");
        item->setAssetId(static_cast<std::int64_t>(id));
        lua_pop(L, 1);
        lua_getfield(L, -1, "AccessoryType");
        const auto type = toEnumItem(L, -1, "AccessoryType", true);
        if (!type)
            raise(L, "SetAccessories requires Enum.AccessoryType");
        item->setAccessoryType(static_cast<Enum::AccessoryType>(type->value));
        lua_pop(L, 1);
        lua_getfield(L, -1, "Order");
        const bool hasOrder = !lua_isnil(L, -1);
        const double order = luaL_optnumber(L, -1, 0);
        if (!std::isfinite(order) || order < 0 || order > 2147483647)
            raise(L, "SetAccessories requires a non-negative accessory order");
        item->setOrder(static_cast<int>(order));
        lua_pop(L, 1);
        lua_getfield(L, -1, "IsLayered");
        item->setIsLayered(lua_isnil(L, -1) ? hasOrder : lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "Puffiness");
        item->setPuffiness(static_cast<float>(luaL_optnumber(L, -1, 1)));
        lua_pop(L, 1);
        for (const char* field : {"Position", "Rotation", "Scale"}) {
            lua_getfield(L, -1, field);
            if (!lua_isnil(L, -1))
                item->classInfo().findProperty(field)->set(*item, Value(checkVector3(L, -1)));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        accessories.push_back(std::move(item));
    }
    for (const InstancePtr& child : std::vector(description.children().begin(), description.children().end()))
        if (const auto* item = dynamic_cast<const AccessoryDescription*>(child.get());
            item && (includeRigid || item->isLayered()))
            child->destroy();
    if (includeRigid)
        for (const auto& slot : accessorySlots)
            description.classInfo().findProperty(slot.property)->set(description, Value(std::string()));
    for (const auto& item : accessories) {
        if (!includeRigid && !item->isLayered())
            continue;
        if (!item->isLayered())
            for (const auto& slot : accessorySlots)
                if (slot.type == item->accessoryType()) {
                    const Property* property = description.classInfo().findProperty(slot.property);
                    std::string ids = std::get<std::string>(property->get(description));
                    if (!ids.empty())
                        ids += ',';
                    ids += std::to_string(item->assetId());
                    property->set(description, Value(std::move(ids)));
                }
        item->setParent(&description);
    }
    return 0;
}

int descriptionGetAccessories(lua_State* L)
{
    const auto& description = static_cast<HumanoidDescription&>(self(L));
    const bool includeRigid = lua_toboolean(L, 2) != 0;
    lua_newtable(L);
    std::vector<std::int64_t> seen;
    int index = 0;
    auto append = [&](const AccessoryDescription& item) {
        if (!includeRigid && !item.isLayered())
            return;
        seen.push_back(item.assetId());
        lua_newtable(L);
        for (const char* field :
             {"AssetId", "AccessoryType", "IsLayered", "Order", "Puffiness", "Position", "Rotation", "Scale"}) {
            pushValue(L, item.classInfo().findProperty(field)->get(item));
            lua_setfield(L, -2, field);
        }
        lua_rawseti(L, -2, ++index);
    };
    for (const auto& child : description.children())
        if (const auto* item = dynamic_cast<const AccessoryDescription*>(child.get()))
            append(*item);
    if (includeRigid)
        for (const auto& slot : accessorySlots) {
            std::string ids
                = std::get<std::string>(description.classInfo().findProperty(slot.property)->get(description));
            std::string_view remaining = ids;
            while (!remaining.empty()) {
                const auto comma = remaining.find(',');
                const auto number = remaining.substr(0, comma);
                std::int64_t id = 0;
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), id);
                if (parsed.ec == std::errc() && id > 0 && std::ranges::find(seen, id) == seen.end()) {
                    AccessoryDescription item;
                    item.setAssetId(id);
                    item.setAccessoryType(slot.type);
                    append(item);
                }
                if (comma == std::string_view::npos)
                    break;
                remaining.remove_prefix(comma + 1);
            }
        }
    return 1;
}

// The screen in pixels: the camera's viewport. GUESS 1920x1080 before
// there's a camera.
Vector2 screenSize(Instance& service)
{
    if (auto* game = dynamic_cast<DataModel*>(service.parent()))
        if (const Workspace* workspace = game->workspace())
            if (const Camera* camera = workspace->currentCamera())
                return camera->viewportSize();
    return {1920.0f, 1080.0f};
}

int guiGetScreenResolution(lua_State* L)
{
    push<Vector2>(L, screenSize(self(L)));
    return 1;
}

// GetInsetArea(screenInsets): the screen less what those insets keep out.
// A desktop window has no notch, and the app's DataModel no top bar: all of it.
int guiGetInsetArea(lua_State* L)
{
    Instance& instance = self(L);
    auto* service = instanceCast<GuiService>(&instance);
    const Vector2 size = screenSize(instance);
    const auto mode = toEnumItem(L, 2, "ScreenInsets");
    if (!mode) luaL_error(L, "GetInsetArea expects Enum.ScreenInsets");
    Rect area{{0, 0}, size};
    if (service && mode->value == static_cast<std::uint32_t>(Enum::ScreenInsets::TopbarSafeInsets)) {
        area = service->topbarInset();
    } else if (service && mode->value == static_cast<std::uint32_t>(Enum::ScreenInsets::CoreUISafeInsets)) {
        const auto inset = service->globalGuiInset();
        area = {{inset.left, service->topbarInset().height() + inset.top},
                {std::max(size.x - inset.right, inset.left), std::max(size.y - inset.bottom, 0.0f)}};
    }
    push<Rect>(L, area);
    return 1;
}

// SetGlobalGuiInset(left, top, right, bottom): the room the app's own bars
// take (its navigation rail on the left), which the GUI engine keeps out of
// the ScreenGuis that keep the GUI inset. Never negative.
int guiSetGlobalGuiInset(lua_State* L)
{
    auto* service = dynamic_cast<GuiService*>(&self(L));
    if (!service)
        luaL_error(L, "SetGlobalGuiInset is a GuiService method");
    float sides[4];
    for (int i = 0; i < 4; ++i) {
        sides[i] = static_cast<float>(luaL_checknumber(L, i + 2));
        if (!(sides[i] >= 0.0f))
            luaL_error(L, "SetGlobalGuiInset: the inset can't be negative");
    }
    service->assignGlobalGuiInset({sides[0], sides[1], sides[2], sides[3]});
    return 0;
}

// No notch or rounded corners on a desktop.
int guiGetSafeZoneOffsets(lua_State* L)
{
    self(L);
    lua_createtable(L, 0, 4);
    for (const char* side : {"top", "bottom", "left", "right"}) {
        lua_pushnumber(L, 0);
        lua_setfield(L, -2, side);
    }
    return 1;
}

// --- DataModel ------------------------------------------------------------------------

// Shutdown(): the app asks to quit. Said in the output; the host decides.
int dataModelShutdown(lua_State* L)
{
    self(L);
    Runtime::of(L).notice(MessageType::Info, "DataModel:Shutdown");
    return 0;
}

// Not Roblox's own app under another brand ("Pioneer" builds).
int pioneerSource(lua_State* L)
{
    self(L);
    pushEnum(L, "PioneerSource", "Roblox");
    return 1;
}

int zero(lua_State* L)
{
    self(L);
    lua_pushnumber(L, 0);
    return 1;
}

int one(lua_State* L)
{
    self(L);
    lua_pushnumber(L, 1);
    return 1;
}

int falseResult(lua_State* L)
{
    self(L);
    lua_pushboolean(L, false);
    return 1;
}

int emptyString(lua_State* L)
{
    self(L);
    lua_pushstring(L, "");
    return 1;
}

// --- TextService ---------------------------------------------------------------------

// GetTextBoundsAsync(params): the text's bounds as the GUI lays it out
// (ScriptContext::setTextMeasure), else estimated as GetTextSize does.
int getTextBoundsAsync(lua_State* L)
{
    self(L);
    const auto* params = dynamic_cast<const GetTextBoundsParams*>(toInstance(L, 2));
    if (!params)
        raise(L, "GetTextBoundsAsync: argument 1 is not a GetTextBoundsParams");
    Vector2 bounds;
    if (const auto& measure = Runtime::of(L).shared().textMeasure) {
        bounds = measure(params->text(), params->font(), params->size(), params->width(), params->richText());
    } else {
        const float glyph = params->size() * 0.5f;
        const float wrap = params->width() > 0.0f ? params->width() : 1e9f;
        float width = 0.0f, line = 0.0f;
        int lines = 1;
        for (char c : params->text()) {
            if (c == '\n' || line + glyph > wrap) {
                width = std::max(width, line);
                line = 0.0f;
                ++lines;
                if (c == '\n')
                    continue;
            }
            line += glyph;
        }
        bounds = {std::ceil(std::max(width, line)), static_cast<float>(lines) * params->size()};
    }
    push<Vector2>(L, bounds);
    return 1;
}

// --- The client's own facts ----------------------------------------------------------

int pushText(lua_State* L, std::string_view text)
{
    self(L);
    pushString(L, text);
    return 1;
}

// Whether the client's own GUI has the focus (the app sets it as menus
// open); per process, as the client's is.
std::atomic<bool> robloxGuiFocus = false;

int setRobloxGuiFocused(lua_State* L)
{
    self(L);
    robloxGuiFocus = lua_toboolean(L, 2) != 0;
    return 0;
}

int robloxGuiFocused(lua_State* L)
{
    self(L);
    lua_pushboolean(L, robloxGuiFocus.load());
    return 1;
}

// The same supported release the join advertises.
int robloxVersion(lua_State* L)
{
    return pushText(L, lunar::robloxVersion);
}

// GUESS: the live channel's name.
int robloxClientChannel(lua_State* L)
{
    return pushText(L, "production");
}

int identityCFrame(lua_State* L)
{
    self(L);
    pushCFrame(L, CFrame{});
    return 1;
}

int vrTouchpadMode(lua_State* L)
{
    self(L);
    pushEnum(L, "VRTouchpadMode", "Touch");
    return 1;
}

int deviceType(lua_State* L)
{
    self(L);
    pushEnum(L, "DeviceType", "Desktop");
    return 1;
}

int deviceLevel(lua_State* L)
{
    self(L);
    pushEnum(L, "DeviceLevel", "High");
    return 1;
}

int notRestricted(lua_State* L)
{
    self(L);
    pushEnum(L, "ChatRestrictionStatus", "NotRestricted");
    return 1;
}

int trueResult(lua_State* L)
{
    self(L);
    lua_pushboolean(L, true);
    return 1;
}

int setRobloxLocaleId(lua_State* L)
{
    auto& service = static_cast<LocalizationService&>(self(L));
    guard(L, [&] { service.setRobloxLocaleId(std::string(checkString(L, 2))); });
    return 0;
}

// Players:SetLocalPlayerInfo(userId, userName, displayName, membershipType,
// ...): who's signed in, on the local player (made if there's none yet).
int setLocalPlayerInfo(lua_State* L)
{
    auto& players = static_cast<Players&>(self(L));
    const auto userId = static_cast<std::int64_t>(luaL_checknumber(L, 2));
    const std::string name(checkString(L, 3));
    const std::string displayName = optString(L, 4, name);
    guard(L, [&] {
        Player* player = players.localPlayer();
        if (!player) {
            auto made = std::make_shared<Player>();
            made->setParent(&players);
            players.assignLocalPlayer(made.get());
            player = made.get();
        }
        player->setUserId(userId);
        player->setName(name);
        player->setDisplayName(displayName);
    });
    return 0;
}

// The OS's light or dark mode, which the app follows when it can ask:
// it can't here, and the app's own default is dark.
int systemTheme(lua_State* L)
{
    self(L);
    pushEnum(L, "SystemThemeValue", "dark");
    return 1;
}

// GUESS: a player's country is the US (the client asks Roblox).
int countryRegion(lua_State* L)
{
    return pushText(L, "US");
}

// --- ExperienceService ---------------------------------------------------------------
//
// How the app starts a game: LaunchExperience(params) asks the native shell
// to join one. Said in the output until the host joins games from here; the
// join and leave callbacks are hub events of the service the host fires.

// A table's scalar fields as "key=value ...", for the output.
std::string describeFields(lua_State* L, int idx)
{
    std::string out;
    if (!lua_istable(L, idx))
        return out;
    for (lua_pushnil(L); lua_next(L, idx) != 0; lua_pop(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            continue;
        const int type = lua_type(L, -1);
        if (type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TBOOLEAN)
            continue;
        lua_pushvalue(L, -1);
        std::size_t length = 0;
        const char* value = luaL_tolstring(L, -1, &length);
        out += std::format("{}{}={}", out.empty() ? "" : " ", lua_tostring(L, -4), std::string_view(value, length));
        lua_pop(L, 2);
    }
    return out;
}

// The launch a GameParams table describes ({placeId, userId, gameInstanceId,
// linkCode, accessCode, joinAttemptId}), told to the host.
void requestLaunch(lua_State* L, int paramsIdx, std::string_view via)
{
    ScriptContext::LaunchRequest request;
    auto number = [&](const char* name) -> std::uint64_t {
        lua_getfield(L, paramsIdx, name);
        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
        return value > 0.0 && value < 9.2e18 ? static_cast<std::uint64_t>(value) : 0;
    };
    auto text = [&](const char* name) {
        lua_getfield(L, paramsIdx, name);
        std::string value = lua_type(L, -1) == LUA_TSTRING ? std::string(lua_tostring(L, -1)) : std::string();
        lua_pop(L, 1);
        return value;
    };
    request.placeId = number("placeId");
    request.userId = number("userId");
    request.gameInstanceId = text("gameInstanceId");
    request.linkCode = text("linkCode");
    request.accessCode = text("accessCode");
    request.joinAttemptId = text("joinAttemptId");
    Runtime& rt = Runtime::of(L);
    rt.notice(MessageType::Info, std::format("{} {}", via, describeFields(L, paramsIdx)));
    if (rt.shared().launchHandler)
        rt.shared().launchHandler(request);
}

int launchExperience(lua_State* L)
{
    self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    requestLaunch(L, 2, "ExperienceService:LaunchExperience");
    // GUESS: the join attempt's id is what the params carry, else a fixed one.
    lua_getfield(L, 2, "joinAttemptId");
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        pushString(L, "00000000-0000-4000-8000-000000000002");
    }
    return 1;
}

template <bool join>
int registerForExperience(lua_State* L)
{
    Instance& service = self(L);
    connectHub(L, service, join ? "ExperienceJoin" : "ExperienceLeave", 2, false);
    return 1;
}

// ContentProvider:GetAssetFetchStatusChangedSignal(contentId): a signal per
// asset. GUESS: it never fires; GetAssetFetchStatus answers what's there.
int assetFetchStatusChangedSignal(lua_State* L)
{
    Instance& provider = self(L);
    pushHubSignal(L, provider, std::format("AssetFetchStatusChanged {}", checkString(L, 2)));
    return 1;
}

// AppUpdateService:CheckForUpdate(handler): there's never one; the handler
// hears so, as the client calls back.
int checkForUpdate(lua_State* L)
{
    self(L);
    pushEnum(L, "AppUpdateStatus", "NotAvailable");
    PackedArgs args{pack(L, -1)};
    lua_pop(L, 1);
    deferCall(L, 2, std::move(args), "AppUpdateService:CheckForUpdate");
    return 0;
}

// --- SessionService ------------------------------------------------------------------
//
// The client's tree of analytics sessions (the app's pages, a game's join),
// kept as the Lua side sets it; nothing reports it anywhere.

struct Session {
    std::string tag;
    std::string parent;
    std::unordered_map<std::string, Packed> metadata;
};

struct SessionState {
    std::mutex mutex;
    std::unordered_map<std::string, Session> sessions;
};

SessionState& sessionState()
{
    static SessionState s;
    return s;
}

constexpr std::string_view rootSid = "00000000-0000-4000-8000-000000000001";

int sessionGetRootSid(lua_State* L)
{
    return pushText(L, rootSid);
}

// GetSessionID(structuralId): the structural id names the session here.
int sessionGetSessionId(lua_State* L)
{
    self(L);
    lua_pushvalue(L, 2);
    return 1;
}

int sessionExists(lua_State* L)
{
    self(L);
    const std::string sid(checkString(L, 2));
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    lua_pushboolean(L, sid == rootSid || s.sessions.contains(sid));
    return 1;
}

int sessionSet(lua_State* L)
{
    self(L);
    const std::string parent(checkString(L, 2)), child(checkString(L, 3));
    std::string tag = optString(L, 4);
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    Session& session = s.sessions[child];
    session.parent = parent;
    session.tag = std::move(tag);
    return 0;
}

int sessionReplace(lua_State* L)
{
    self(L);
    const std::string sid(checkString(L, 2));
    std::string tag = optString(L, 3);
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    s.sessions[sid].tag = std::move(tag);
    return 0;
}

int sessionRemove(lua_State* L)
{
    self(L);
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    s.sessions.erase(std::string(checkString(L, 2)));
    return 0;
}

int sessionGetTag(lua_State* L)
{
    self(L);
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    auto it = s.sessions.find(std::string(checkString(L, 2)));
    pushString(L, it == s.sessions.end() ? std::string_view() : std::string_view(it->second.tag));
    return 1;
}

int sessionSetMetadata(lua_State* L)
{
    self(L);
    const std::string sid(checkString(L, 2)), key(checkString(L, 3));
    Packed value = pack(L, 4);
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    s.sessions[sid].metadata.insert_or_assign(key, std::move(value));
    return 0;
}

int sessionGetMetadata(lua_State* L)
{
    self(L);
    const std::string sid(checkString(L, 2)), key(checkString(L, 3));
    std::optional<Packed> value;
    {
        SessionState& s = sessionState();
        const std::scoped_lock lock(s.mutex);
        if (auto it = s.sessions.find(sid); it != s.sessions.end())
            if (auto m = it->second.metadata.find(key); m != it->second.metadata.end())
                value = m->second;
    }
    if (value)
        unpack(L, *value);
    else
        lua_pushnil(L);
    return 1;
}

int sessionRemoveMetadata(lua_State* L)
{
    self(L);
    const std::string sid(checkString(L, 2)), key(checkString(L, 3));
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    if (auto it = s.sessions.find(sid); it != s.sessions.end())
        it->second.metadata.erase(key);
    return 0;
}

int sessionRemoveWithMetadataKey(lua_State* L)
{
    self(L);
    const std::string key(checkString(L, 2));
    SessionState& s = sessionState();
    const std::scoped_lock lock(s.mutex);
    std::erase_if(s.sessions, [&](const auto& entry) { return entry.second.metadata.contains(key); });
    return 0;
}

// --- RbxAnalyticsService -------------------------------------------------------------

// The ids it reports; GUESS: any GUID-shaped string does, nobody checks them here.
int analyticsId(lua_State* L)
{
    self(L);
    pushString(L, "00000000-0000-4000-8000-000000000000");
    return 1;
}

constexpr auto methods = std::to_array<Method>({
    {"IsFocused", "TextBox", textBoxIsFocused},
    {"CreateHumanoidModelFromDescriptionAsync", "Players", createHumanoidModel},
    {"CreateHumanoidModelFromDescription", "Players", createHumanoidModel},
    {"LoadLocalAsset", "InsertService", loadLocalAsset},
    {"SetAccessories", "HumanoidDescription", descriptionSetAccessories},
    {"GetAccessories", "HumanoidDescription", descriptionGetAccessories},
    {"GetAppliedDescription", "Humanoid", getAppliedDescription},
    {"ApplyDescription", "Humanoid", applyHumanoidDescription},
    {"ApplyDescriptionAsync", "Humanoid", applyHumanoidDescription},
    {"ApplyDescriptionReset", "Humanoid", applyHumanoidDescription},
    {"ApplyDescriptionResetAsync", "Humanoid", applyHumanoidDescription},
    {"GetAccessories", "Humanoid", humanoidGetAccessories},
    {"GetMessageId", "MessageBusService", busGetMessageId},
    {"GetProtocolMethodRequestMessageId", "MessageBusService", busGetProtocolMessageId<true>},
    {"GetProtocolMethodResponseMessageId", "MessageBusService", busGetProtocolMessageId<false>},
    {"Publish", "MessageBusService", busPublish},
    {"PublishProtocolMethodRequest", "MessageBusService", busPublishProtocol<true>},
    {"PublishProtocolMethodResponse", "MessageBusService", busPublishProtocol<false>},
    {"Subscribe", "MessageBusService", busSubscribe},
    {"SubscribeToProtocolMethodRequest", "MessageBusService", busSubscribeProtocol<true>},
    {"SubscribeToProtocolMethodResponse", "MessageBusService", busSubscribeProtocol<false>},
    {"GetLast", "MessageBusService", busGetLast},
    // GUESS: no native side to answer; the callers take nil as "nothing there".
    {"Call", "MessageBusService", nilResult},
    {"MakeRequest", "MessageBusService", nothing},
    {"SetRequestHandler", "MessageBusService", nothing},
    {"HasItem", "MemStorageService", memHasItem},
    {"GetItem", "MemStorageService", memGetItem},
    {"SetItem", "MemStorageService", memSetItem},
    {"RemoveItem", "MemStorageService", memRemoveItem},
    {"Fire", "MemStorageService", memFireMethod},
    {"Bind", "MemStorageService", memBind<false>},
    {"BindAndFire", "MemStorageService", memBind<true>},
    {"Call", "MemStorageService", nilResult},
    {"GetItem", "LocalStorageService", localGetItem},
    {"SetItem", "LocalStorageService", localSetItem},
    {"WhenLoaded", "LocalStorageService", localWhenLoaded},
    {"Flush", "LocalStorageService", nothing},
    {"GetUserLayerLoadingStatus", "IXPService", ixpStatus},
    {"GetCreatorLayerLoadingStatus", "IXPService", ixpStatus},
    {"GetBrowserTrackerLayerLoadingStatus", "IXPService", ixpStatus},
    {"GetUserStatusForLayer", "IXPService", ixpStatus},
    {"GetCreatorStatusForLayer", "IXPService", ixpStatus},
    {"GetBrowserTrackerStatusForLayer", "IXPService", ixpStatus},
    {"GetUserLayerVariables", "IXPService", emptyTable},
    {"GetCreatorLayerVariables", "IXPService", emptyTable},
    {"GetBrowserTrackerLayerVariables", "IXPService", emptyTable},
    {"GetRegisteredUserLayersToStatus", "IXPService", emptyTable},
    {"GetRegisteredCreatorLayersToStatus", "IXPService", emptyTable},
    {"RegisterUserLayers", "IXPService", nothing},
    {"RegisterCreatorLayers", "IXPService", nothing},
    {"InitializeUserLayers", "IXPService", nothing},
    {"InitializeCreatorLayers", "IXPService", nothing},
    {"ClearUserLayers", "IXPService", nothing},
    {"ClearCreatorLayers", "IXPService", nothing},
    {"LogUserLayerExposure", "IXPService", nothing},
    {"LogCreatorLayerExposure", "IXPService", nothing},
    {"LogBrowserTrackerLayerExposure", "IXPService", nothing},
    {"LogFlagLinkedUserLayerExposure", "IXPService", nothing},
    {"GetTextBoundsAsync", "TextService", getTextBoundsAsync},
    // GUESS: no family needs its size nudged.
    {"GetTextSizeOffsetAsync", "TextService", zero},
    {"GetRobloxGuiFocused", "RunService", robloxGuiFocused},
    {"GetEngineFeature", "DataModel", getEngineFeature},
    {"SetFlagVersion", "DataModel", nothing},
    {"Shutdown", "DataModel", dataModelShutdown},
    {"IsPioneerApp", "DataModel", falseResult},
    {"GetPioneerSource", "DataModel", pioneerSource},
    {"GetPioneerRootPlaceId", "DataModel", zero},
    {"GetNotificationTypeList", "GuiService", guiGetNotificationTypeList},
    {"BroadcastNotification", "GuiService", guiBroadcastNotification},
    {"OpenBrowserWindow", "GuiService", guiOpenBrowserWindow},
    {"OpenNativeOverlay", "GuiService", guiOpenBrowserWindow},
    {"GetScreenResolution", "GuiService", guiGetScreenResolution},
    {"GetInsetArea", "GuiService", guiGetInsetArea},
    {"GetSafeZoneOffsets", "GuiService", guiGetSafeZoneOffsets},
    {"GetResolutionScale", "GuiService", one},
    {"SendNotification", "GuiService", emptyString},
    {"DismissNotification", "GuiService", falseResult},
    {"ShowStatsBasedOnInputString", "GuiService", falseResult},
    {"CloseStatsBasedOnInputString", "GuiService", falseResult},
    {"GetUiMessage", "GuiService", emptyString},
    {"SetMenuIsOpen", "GuiService", nothing},
    {"Select", "GuiService", nothing},
    {"AddSelectionParent", "GuiService", nothing},
    {"AddSelectionTuple", "GuiService", nothing},
    {"RemoveSelectionGroup", "GuiService", nothing},
    {"SetTopbarInset", "GuiService", nothing},
    {"SetGlobalGuiInset", "GuiService", guiSetGlobalGuiInset},
    {"SetPurchasePromptIsShown", "GuiService", nothing},
    {"OnNotificationDisplayed", "GuiService", nothing},
    {"OnNotificationInteraction", "GuiService", nothing},
    {"SetSafeZoneOffsets", "GuiService", nothing},
    {"SetHardwareSafeAreaInsets", "GuiService", nothing},
    {"GetRobloxVersion", "RunService", robloxVersion},
    {"GetCoreScriptVersion", "RunService", emptyString},
    {"GetRobloxClientChannel", "RunService", robloxClientChannel},
    {"GetRobloxClientIxpEnrolledExperiments", "RunService", emptyString},
    {"SetRobloxGuiFocused", "RunService", setRobloxGuiFocused},
    {"IsEdit", "RunService", falseResult},
    {"IsTeamCreateViewer", "RunService", falseResult},
    {"IsVRAppBuild", "VRService", falseResult},
    {"GetUserCFrameEnabled", "VRService", falseResult},
    {"GetUserCFrame", "VRService", identityCFrame},
    {"GetTouchpadMode", "VRService", vrTouchpadMode},
    {"RecenterUserHeadCFrame", "VRService", nothing},
    {"RequestNavigation", "VRService", nothing},
    {"GetDeviceType", "UserInputService", deviceType},
    {"GetDeviceLevel", "UserInputService", deviceLevel},
    {"GamepadSupports", "UserInputService", falseResult},
    {"GetImageForKeyCode", "UserInputService", emptyString},
    {"SetRobloxLocaleId", "LocalizationService", setRobloxLocaleId},
    {"SetLocalPlayerInfo", "Players", setLocalPlayerInfo},
    {"GetRootSID", "SessionService", sessionGetRootSid},
    {"GetSessionID", "SessionService", sessionGetSessionId},
    {"SessionExists", "SessionService", sessionExists},
    {"SetSession", "SessionService", sessionSet},
    {"ReplaceSession", "SessionService", sessionReplace},
    {"RemoveSession", "SessionService", sessionRemove},
    {"GetSessionTag", "SessionService", sessionGetTag},
    {"SetMetadata", "SessionService", sessionSetMetadata},
    {"GetMetadata", "SessionService", sessionGetMetadata},
    {"RemoveMetadata", "SessionService", sessionRemoveMetadata},
    {"RemoveSessionsWithMetadataKey", "SessionService", sessionRemoveWithMetadataKey},
    {"GenerateSessionInfoString", "SessionService", emptyString},
    {"GetBreadcrumbs", "SessionService", emptyTable},
    {"GetHistory", "SessionService", emptyTable},
    {"GetCreatedTimestampUtcMs", "SessionService", zero},
    {"AcquireContextFocus", "SessionService", nothing},
    {"ReleaseContextFocus", "SessionService", nothing},
    {"IsContextFocused", "SessionService", trueResult},
    {"GetUserPlatformName", "ThirdPartyUserService", emptyString},
    {"GetVoiceChatRestrictionStatus", "ThirdPartyUserService", notRestricted},
    {"HaveActiveUser", "ThirdPartyUserService", trueResult},
    {"IsAccountSwitchingSupported", "ThirdPartyUserService", falseResult},
    {"IsChatRestrictionSupported", "ThirdPartyUserService", falseResult},
    {"IsSingleSignOnSupported", "ThirdPartyUserService", falseResult},
    {"ShowAccountPicker", "ThirdPartyUserService", nothing},
    // Platform hooks with nothing behind them on a desktop: quiet defaults.
    {"LaunchExperience", "ExperienceService", launchExperience},
    {"LaunchExperienceFromSource", "ExperienceService", launchExperience},
    {"LaunchExperienceFromSourceWithCallback", "ExperienceService", launchExperience},
    {"RegisterForExperienceJoin", "ExperienceService", registerForExperience<true>},
    {"RegisterForExperienceLeave", "ExperienceService", registerForExperience<false>},
    {"GetFollowUserId", "ExperienceService", zero},
    {"GetPendingJoinAttempt", "ExperienceService", nilResult},
    {"GetPlaceJoinState", "ExperienceService", emptyString},
    {"GetQueuePosition", "ExperienceService", zero},
    {"ConsumePendingExperienceLeaveWithReason", "ExperienceService", emptyTable},
    {"LeaveExperienceWithReason", "ExperienceService", nothing},
    {"StartCrossExperience", "ExperienceService", nothing},
    {"StopCrossExperience", "ExperienceService", nothing},
    {"ExecuteCrossExperienceCall", "ExperienceService", nothing},
    {"GetUnder13", "Player", falseResult},
    {"SwitchedToAppShellFeature", "NotificationService", nothing},
    {"setThrottleFramerateEnabled", "RunService", nothing},
    {"getThrottleFramerateEnabled", "RunService", falseResult},
    {"GetAssetFetchStatusChangedSignal", "ContentProvider", assetFetchStatusChangedSignal},
    {"ActionEnabled", "NotificationService", nothing},
    {"ActionTaken", "NotificationService", nothing},
    {"CancelAllNotification", "NotificationService", nothing},
    {"CancelNotification", "NotificationService", nothing},
    {"ScheduleNotification", "NotificationService", nothing},
    {"SubscribeToRccEventNamespace", "NotificationService", nothing},
    {"SubscribeToTopic", "NotificationService", nothing},
    {"UnsubscribeFromTopic", "NotificationService", nothing},
    {"GetScheduledNotifications", "NotificationService", emptyTable},
    {"IsAvailable", "AchievementService", falseResult},
    {"GrantAchievement", "AchievementService", falseResult},
    {"HasAchieved", "AchievementService", falseResult},
    {"CanPerformBinaryUpdate", "AppUpdateService", falseResult},
    {"CheckForUpdate", "AppUpdateService", checkForUpdate},
    {"PerformManagedUpdate", "AppUpdateService", falseResult},
    {"GetProtocolLaunchUpdateName", "AppUpdateService", emptyString},
    {"GetProtocolLaunchUpdateType", "AppUpdateService", emptyString},
    // No message from Roblox to show at startup.
    {"GetStartupMessage", "StartupMessageService", nilResult},
    {"ExecuteActionButton", "StartupMessageService", nothing},
    {"isSystemThemeAvailable", "SystemThemeService", falseResult},
    {"getSystemTheme", "SystemThemeService", systemTheme},
    {"getSystemThemeAsync", "SystemThemeService", systemTheme},
    {"setTheme", "SystemThemeService", nothing},
    {"setClassicThemeActive", "SystemThemeService", nothing},
    {"GetCountryRegionForPlayerAsync", "LocalizationService", countryRegion},
    {"RegisterLuaUrl", "LinkingService", nothing},
    {"StartLuaUrlDelivery", "LinkingService", nothing},
    {"StopLuaUrlDelivery", "LinkingService", nothing},
    {"IsUrlRegistered", "LinkingService", falseResult},
    {"GetLastLuaUrl", "LinkingService", emptyString},
    {"GetAndClearLastPendingUrl", "LinkingService", emptyString},
    {"DetectUrl", "LinkingService", falseResult},
    {"OpenUrl", "LinkingService", guiOpenBrowserWindow},
    {"SupportsSwitchToSettingsApp", "LinkingService", falseResult},
    {"SwitchToSettingsApp", "LinkingService", nothing},
    {"IsAvailable", "WindowProtocolService", falseResult},
    {"ShouldRenderTitleBarControlsNatively", "WindowProtocolService", trueResult},
    {"SetTitleBarMode", "WindowProtocolService", nothing},
    {"SetCustomTitleBarHeight", "WindowProtocolService", nothing},
    {"BeginDrag", "WindowProtocolService", nothing},
    {"OnDragAreaDoubleClicked", "WindowProtocolService", nothing},
    {"OnDragAreaRightClicked", "WindowProtocolService", nothing},
    {"Minimize", "WindowProtocolService", nothing},
    {"Maximize", "WindowProtocolService", nothing},
    {"Restore", "WindowProtocolService", nothing},
    {"Close", "WindowProtocolService", nothing},
    {"SendCommand", "BrowserService", nothing},
    {"CloseBrowserWindow", "BrowserService", nothing},
    {"ExecuteJavaScript", "BrowserService", nothing},
    {"OpenWindow", "WebViewService", nothing},
    {"CloseWindow", "WebViewService", nothing},
    {"SetGamepadCursorPosition", "GamepadService", nothing},
    {"AutoSelectGui", "GamepadService", nothing},
    {"IsEnabled", "AvatarChatService", falseResult},
    {"EnableVoice", "AvatarChatService", nothing},
    {"IsCapturingVideo", "CaptureService", falseResult},
    {"SignalChallengeLoaded", "GenericChallengeService", nothing},
    {"SignalChallengeCompleted", "GenericChallengeService", nothing},
    {"SignalChallengeInvalidated", "GenericChallengeService", nothing},
    {"SignalChallengeAbandoned", "GenericChallengeService", nothing},
    {"GameStreamingEnabled", "VideoService", falseResult},
    {"ReportLuaRequireCount", "ScriptContext", nothing},
    {"CompressLuaApp", "ScriptContext", nothing},
    {"SendTextChatCommandClientSent", "TextChatService", nothing},
    {"SubmitAdNotification", "AdService", nothing},
    {"CanEnterCaptureMode", "ExperienceStateCaptureService", falseResult},
    {"ToggleCaptureMode", "ExperienceStateCaptureService", nothing},
    {"UpdatePlayerPartyData", "SocialService", nothing},
    {"SetSoundEnabled", "SoundService", nothing},
    {"ScopeCheckUIComplete", "ExperienceAuthService", nothing},
    {"TakeScreenshot", "SafetyService", nothing},
    {"Stop", "FaceAnimatorService", nothing},
    {"GetDeviceId", "DeviceIdService", analyticsId},
    {"GetControlAndVariantRolloutFlags", "RunService", nothing},
    {"BindCoreActivate", "ContextActionService", nothing},
    {"SignalPromptSubscriptionPurchaseFinished", "MarketplaceService", nothing},
    {"SignalRobuxTransferCompleted", "MarketplaceService", nothing},
    {"SignalMockPurchaseRobloxSubscription", "MarketplaceService", nothing},
    {"SignalClientPurchaseSuccess", "MarketplaceService", nothing},
    {"ReportRobuxUpsellStarted", "MarketplaceService", nothing},
    {"ReportAbuse", "Players", nothing},
    {"ReportChatAbuse", "Players", nothing},
    {"ReportAvatarAbuse", "Players", nothing},
    // Telemetry goes nowhere.
    {"LogCounter", "TelemetryService", nilResult},
    {"LogDurationEvent", "TelemetryService", nilResult},
    {"LogDurationEventWithTimestamp", "TelemetryService", nilResult},
    {"LogEvent", "TelemetryService", nilResult},
    {"LogStat", "TelemetryService", nilResult},
    {"SendEventDeferred", "EventIngestService", nothing},
    {"SendEventImmediately", "EventIngestService", nothing},
    {"SetRBXEvent", "EventIngestService", nothing},
    {"SetRBXEventStream", "EventIngestService", nothing},
    {"GetClientId", "RbxAnalyticsService", analyticsId},
    {"GetSessionId", "RbxAnalyticsService", analyticsId},
    {"GetPlaySessionId", "RbxAnalyticsService", analyticsId},
    {"AddGlobalPointsField", "RbxAnalyticsService", nothing},
    {"AddGlobalPointsTag", "RbxAnalyticsService", nothing},
    {"RemoveGlobalPointsField", "RbxAnalyticsService", nothing},
    {"RemoveGlobalPointsTag", "RbxAnalyticsService", nothing},
    {"ReleaseRBXEventStream", "RbxAnalyticsService", nothing},
    {"ReportCounter", "RbxAnalyticsService", nothing},
    {"ReportInfluxSeries", "RbxAnalyticsService", nothing},
    {"ReportStats", "RbxAnalyticsService", nothing},
    {"ReportToDiagByCountryCode", "RbxAnalyticsService", nothing},
    {"SendEventDeferred", "RbxAnalyticsService", nothing},
    {"SendEventImmediately", "RbxAnalyticsService", nothing},
    {"SetRBXEvent", "RbxAnalyticsService", nothing},
    {"SetRBXEventStream", "RbxAnalyticsService", nothing},
    {"TrackEvent", "RbxAnalyticsService", nothing},
    {"TrackEventWithArgs", "RbxAnalyticsService", nothing},
    {"DEPRECATED_TrackEvent", "RbxAnalyticsService", nothing},
    {"DEPRECATED_TrackEventWithArgs", "RbxAnalyticsService", nothing},
    {"UpdateHeartbeatObject", "RbxAnalyticsService", nothing},
    {"GetFFlag", "GlobalSettings", globalGetFFlag},
    {"GetFVariable", "GlobalSettings", globalGetFVariable},
    {"DefineFastFlag", "DataModel", defineFastFlag},
    {"DefineFastInt", "DataModel", defineFastInt},
    {"DefineFastString", "DataModel", defineFastString},
    {"GetFastFlag", "DataModel", getFast<bool>},
    {"GetFastInt", "DataModel", getFast<double>},
    {"GetFastString", "DataModel", getFast<std::string>},
});

} // namespace

std::span<const Method> appMethods()
{
    return methods;
}

void setAppStorageItem(std::string_view key, std::string_view value)
{
    LocalState& s = localState();
    const std::scoped_lock lock(s.mutex);
    s.items.insert_or_assign(std::string(key), std::string(value));
}

void setMemStorageItem(Runtime& runtime, std::string_view key, std::string_view value)
{
    {
        MemState& s = memState();
        const std::scoped_lock lock(s.mutex);
        s.items.insert_or_assign(std::string(key), std::string(value));
    }
    if (Instance* service = findOrCreateService(runtime.main(), runtime.game(), "MemStorageService", false))
        memFire(runtime.main(), *service, key, std::string(value));
}

std::optional<std::size_t> loadClientSettings(std::string_view json)
{
    JsonReader in{json};
    std::unordered_map<std::string, std::string> served;
    if (!in.eat('{'))
        return std::nullopt;
    do {
        const auto key = in.string();
        if (!key || !in.eat(':'))
            return std::nullopt;
        if (*key != "applicationSettings") {
            in.skip();
            continue;
        }
        if (!in.eat('{'))
            return std::nullopt;
        if (in.eat('}'))
            continue;
        do {
            const auto name = in.string();
            if (!name || !in.eat(':'))
                return std::nullopt;
            in.space();
            if (in.at < in.text.size() && in.text[in.at] == '"') {
                if (auto value = in.string())
                    served.insert_or_assign(std::string(bareFlagName(*name)), std::move(*value));
            } else {
                in.skip();
            }
        } while (!in.bad && in.eat(','));
        if (in.bad || !in.eat('}'))
            return std::nullopt;
    } while (!in.bad && in.eat(','));
    if (in.bad || !in.eat('}'))
        return std::nullopt;
    const std::size_t count = served.size();
    Flags& f = flags();
    const std::scoped_lock lock(f.mutex);
    f.served = std::move(served);
    return count;
}

} // namespace lunar::lua
