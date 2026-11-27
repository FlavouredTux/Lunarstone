// The services and instance classes scripts lean on: RunService, Players and
// Player, Debris, HttpService, CollectionService, TweenService and Tween,
// UserInputService, ContextActionService, TextService, PhysicsService,
// Workspace queries, BindableEvent/BindableFunction, RemoteEvent/RemoteFunction
// (through the Play Solo loopback) and a few Humanoid basics. Animation and
// the Humanoid's state machine are in Animation.cpp and HumanoidApi.cpp.
//
// Error messages are the 0.742 client's strings where the client has one.

#include "Services.hpp"
#include "InstanceApi.hpp"
#include "RobloxApi.hpp"
#include "Userdata.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/avatar/UserAvatar.hpp>

#include <algorithm>
#include <thread>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <numbers>
#include <print>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

#include <unistd.h>

namespace lunar::lua {

// Runs a callback (OnInvoke, OnServerInvoke...) in its own runtime with
// `args`, calling `done` with the packed results or the error once it
// finishes, which may be right away or after it yields.
void invokeCallback(const Shared::Callback& callback, const PackedArgs& args,
                    std::move_only_function<void(bool, PackedArgs)> done)
{
    Runtime& rt = *callback.runtime;
    lua_State* thread = lua_newthread(rt.main());
    rt.setAccess(thread, callback.access); // it runs as whoever set it
    Runtime::useGlobals(thread, callback.globals);
    const int ref = lua_ref(rt.main(), -1);
    lua_pop(rt.main(), 1);
    lua_getref(thread, callback.ref);
    const int nargs = unpackArgs(thread, args);
    rt.onFinish(thread, [&rt, ref, done = std::move(done)](lua_State* t, int status, int results) mutable {
        if (status == LUA_OK) {
            done(true, packArgs(t, lua_gettop(t) - results + 1, lua_gettop(t)));
        } else {
            PackedArgs message{packed(Value(std::string(lua_isstring(t, -1) ? lua_tostring(t, -1) : "error")))};
            done(false, std::move(message));
        }
        rt.defer([&rt, ref] { lua_unref(rt.main(), ref); });
    });
    if (auto error = rt.resume(thread, nargs, "invoke"))
        rt.report(*error);
}

namespace {

// --- Invoking callbacks -------------------------------------------------------

// Resumes an invoking thread with the callee's results, or with its error
// raised at the call.
void finishInvoke(Runtime& caller, lua_State* thread, std::uint64_t token, bool ok, const PackedArgs& results)
{
    if (ok) {
        caller.wake(thread, token, [&](lua_State* t) { return unpackArgs(t, results); });
        return;
    }
    const auto* message = results.empty() ? nullptr : std::get_if<std::string>(&results[0].value);
    caller.fail(thread, token, message ? *message : "error");
}

// The calling side of an invoke: returns right away when the callee finished
// synchronously, otherwise yields until it does.
int invokeAndWait(lua_State* L, const Shared::Callback& callback, const PackedArgs& args)
{
    struct Outcome {
        bool finished = false;
        bool ok = false;
        PackedArgs results;
        lua_State* waiter = nullptr;
        std::uint64_t token = 0;
    };
    auto outcome = std::make_shared<Outcome>();
    Runtime& caller = Runtime::of(L);
    invokeCallback(callback, args, [outcome, &caller](bool ok, PackedArgs results) {
        outcome->finished = true;
        outcome->ok = ok;
        outcome->results = std::move(results);
        if (!outcome->waiter)
            return;
        caller.defer([outcome, &caller] {
            finishInvoke(caller, outcome->waiter, outcome->token, outcome->ok, outcome->results);
        });
    });
    if (outcome->finished) {
        if (!outcome->ok)
            raise(L, outcome->results.empty() ? "error" : std::get<std::string>(outcome->results[0].value));
        lua_settop(L, 0);
        return unpackArgs(L, outcome->results);
    }
    checkYieldable(L);
    outcome->waiter = L;
    outcome->token = caller.suspend(L);
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

Player* localPlayer(const DataModel& game)
{
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

Value instanceValue(const Instance* instance)
{
    return instance ? Value(std::const_pointer_cast<Instance>(instance->shared_from_this())) : Value();
}

// --- RunService ----------------------------------------------------------------

// --- EncodingService ----------------------------------------------------------------

// Its argument #`idx`, a buffer, as bytes.
std::string_view bufferArg(lua_State* L, int idx)
{
    if (lua_type(L, idx) != LUA_TBUFFER)
        raise(L, std::format("invalid argument #{} (buffer expected, got {})", idx - 1, luaL_typename(L, idx)));
    std::size_t length = 0;
    const void* bytes = lua_tobuffer(L, idx, &length);
    return {static_cast<const char*>(bytes), length};
}

void pushBuffer(lua_State* L, std::string_view bytes)
{
    void* out = lua_newbuffer(L, bytes.size());
    if (!bytes.empty())
        std::copy(bytes.begin(), bytes.end(), static_cast<char*>(out));
}

// Zstd is the only Enum.CompressionAlgorithm.
void checkAlgorithm(lua_State* L, int idx)
{
    if (!toEnumItem(L, idx, "CompressionAlgorithm"))
        raise(L, std::format("invalid argument #{} (Enum.CompressionAlgorithm expected)", idx - 1));
}

constexpr std::string_view base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int encodingBase64Encode(lua_State* L)
{
    self(L);
    const std::string_view in = bufferArg(L, 2);
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const auto byte = [&](std::size_t k) { return k < in.size() ? static_cast<unsigned char>(in[k]) : 0u; };
        const unsigned n = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        out += base64Alphabet[(n >> 18) & 63];
        out += base64Alphabet[(n >> 12) & 63];
        out += i + 1 < in.size() ? base64Alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? base64Alphabet[n & 63] : '=';
    }
    pushBuffer(L, out);
    return 1;
}

int encodingBase64Decode(lua_State* L)
{
    self(L);
    const std::string_view in = bufferArg(L, 2);
    std::string out;
    unsigned bits = 0;
    int count = 0;
    for (char c : in) {
        const std::size_t at = base64Alphabet.find(c);
        if (at == std::string_view::npos)
            continue; // padding and anything else
        bits = (bits << 6) | static_cast<unsigned>(at);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    pushBuffer(L, out);
    return 1;
}

int encodingCompressBuffer(lua_State* L)
{
    self(L);
    const std::string_view in = bufferArg(L, 2);
    checkAlgorithm(L, 3);
    const int level = lua_isnoneornil(L, 4) ? ZSTD_CLEVEL_DEFAULT : static_cast<int>(luaL_checkinteger(L, 4));
    std::string out(ZSTD_compressBound(in.size()), '\0');
    const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n))
        raise(L, std::format("CompressBuffer failed: {}", ZSTD_getErrorName(n)));
    out.resize(n);
    pushBuffer(L, out);
    return 1;
}

// The size a Zstd frame says it holds.
std::optional<std::size_t> decompressedSize(std::string_view in)
{
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN)
        return std::nullopt;
    return static_cast<std::size_t>(size);
}

int encodingDecompressBuffer(lua_State* L)
{
    self(L);
    const std::string_view in = bufferArg(L, 2);
    checkAlgorithm(L, 3);
    const auto size = decompressedSize(in);
    if (!size)
        raise(L, "DecompressBuffer failed: not a Zstd frame with its size"); // GUESS: the client's wording
    std::string out(*size, '\0');
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n))
        raise(L, std::format("DecompressBuffer failed: {}", ZSTD_getErrorName(n)));
    out.resize(n);
    pushBuffer(L, out);
    return 1;
}

int encodingGetDecompressedBufferSize(lua_State* L)
{
    self(L);
    const std::string_view in = bufferArg(L, 2);
    checkAlgorithm(L, 3);
    const auto size = decompressedSize(in);
    if (!size)
        raise(L, "GetDecompressedBufferSize failed: not a Zstd frame with its size");
    lua_pushnumber(L, static_cast<double>(*size));
    return 1;
}

int runIsClient(lua_State* L)
{
    self(L);
    lua_pushboolean(L, Runtime::of(L).isClient());
    return 1;
}

int runIsServer(lua_State* L)
{
    self(L);
    lua_pushboolean(L, Runtime::of(L).isServer());
    return 1;
}

// A live game, not Studio: IsStudio and IsEdit are false, IsRunning true.
template <bool Value>
int constantBool(lua_State* L)
{
    self(L);
    lua_pushboolean(L, Value);
    return 1;
}

int bindToRenderStep(lua_State* L)
{
    self(L);
    std::string name(checkString(L, 2));
    const int priority = static_cast<int>(luaL_checknumber(L, 3));
    luaL_checktype(L, 4, LUA_TFUNCTION);
    Runtime& rt = Runtime::of(L);
    // The binding runs as whoever made it: a console run's keeps full access.
    lua_pushthread(L);
    const int thread = lua_ref(L, -1);
    lua_pop(L, 1);
    rt.bindToRenderStep(std::move(name), priority, lua_ref(L, 4), thread, rt.accessOf(L), Runtime::refGlobals(L));
    return 0;
}

int unbindFromRenderStep(lua_State* L)
{
    self(L);
    Runtime::of(L).unbindFromRenderStep(checkString(L, 2));
    return 0;
}

int noopMethod(lua_State* L)
{
    self(L);
    return 0;
}

// ReplicatedFirst:RemoveDefaultLoadingScreen(): a game's own loader takes
// over, and the app takes its loading screen down at once.
int replicatedFirstRemoveDefaultLoadingScreen(lua_State* L)
{
    self(L);
    Runtime::of(L).shared().defaultLoadingScreenRemoved = true;
    return 0;
}

// Queries about client menus Lunarstone doesn't have: they're never open.
int falseMethod(lua_State* L)
{
    lua_pushboolean(L, 0);
    return 1;
}

// --- Network ownership ----------------------------------------------------------------
// Play Solo simulates everything in one place, so there is no ownership to
// hand out: setting it is accepted, the server owns every part (nil), and
// ownership can always be set.

int nilMethod(lua_State* L)
{
    self(L);
    lua_pushnil(L);
    return 1;
}

int trueMethod(lua_State* L)
{
    self(L);
    lua_pushboolean(L, 1);
    return 1;
}

int canSetNetworkOwnership(lua_State* L)
{
    self(L);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// UserGameSettings:GetCameraYInvertValue: 1, or -1 with Invert Camera on (off by default).
int cameraYInvertValue(lua_State* L)
{
    lua_pushnumber(L, 1.0);
    return 1;
}

// --- Terrain -----------------------------------------------------------------------

Enum::Material checkMaterial(lua_State* L, int idx)
{
    const EnumItem& item = check<EnumItem>(L, idx);
    if (item.enumType != enums::item(Enum::Material::Air).enumType)
        raise(L, "Unable to cast to Material"); // GUESS: the client's wording
    return static_cast<Enum::Material>(item.value);
}

// Terrain:GetMaterialColor / SetMaterialColor (Air and Water have none).
int terrainGetMaterialColor(lua_State* L)
{
    const auto& terrain = static_cast<const Terrain&>(self(L));
    pushColor3(L, terrain.getMaterialColor(checkMaterial(L, 2)));
    return 1;
}

int terrainSetMaterialColor(lua_State* L)
{
    auto& terrain = static_cast<Terrain&>(self(L));
    terrain.setMaterialColor(checkMaterial(L, 2), checkColor3(L, 3));
    return 0;
}

// --- Players, Player -------------------------------------------------------------

std::vector<Player*> playersOf(const Instance& service)
{
    std::vector<Player*> result;
    for (const InstancePtr& child : service.children())
        if (auto* player = dynamic_cast<Player*>(child.get()))
            result.push_back(player);
    return result;
}

int getPlayers(lua_State* L)
{
    auto players = playersOf(self(L));
    lua_createtable(L, static_cast<int>(players.size()), 0);
    int i = 0;
    for (Player* player : players) {
        pushInstance(L, player);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int getPlayerFromCharacter(lua_State* L)
{
    Instance& service = self(L);
    Instance* character = toInstance(L, 2);
    for (Player* player : playersOf(service))
        if (character && player->character() == character)
            return returnInstance(L, player);
    lua_pushnil(L);
    return 1;
}

int getPlayerByUserId(lua_State* L)
{
    Instance& service = self(L);
    const auto userId = static_cast<std::int64_t>(luaL_checknumber(L, 2));
    for (Player* player : playersOf(service))
        if (player->userId() == userId)
            return returnInstance(L, player);
    lua_pushnil(L);
    return 1;
}

int playerLoadCharacter(lua_State* L)
{
    auto* player = dynamic_cast<Player*>(&self(L));
    Runtime& rt = Runtime::of(L);
    if (!rt.isServer())
        raise(L, "LoadCharacter can only be called by the backend server");
    if (player)
        spawnCharacter(rt.shared(), rt.game(), *player);
    return 0;
}

// Player:Kick. A client can only kick its own player: the client's Kick
// (0x104e3a1f2) throws for anyone else [code].
int playerKick(lua_State* L)
{
    Instance& player = self(L);
    Runtime& rt = Runtime::of(L);
    if (rt.role() == ScriptRole::Client && localPlayer(rt.game()) != &player)
        raise(L, "Cannot kick a non-local Player from a LocalScript");
    rt.notice(MessageType::Warning,
              std::format("{} was kicked: {}", player.name(), lua_isstring(L, 2) ? lua_tostring(L, 2) : ""));
    return 0;
}

// Player:RequestStreamAroundAsync(position [, timeOut]): in a game that
// streams its content, asks the server for the area around position and
// waits until it has come (ScriptContext::setStreamAroundHook); elsewhere
// everything is there already.
int playerRequestStreamAroundAsync(lua_State* L)
{
    self(L);
    const Vector3 position = checkVector3(L, 2);
    const double timeout = luaL_optnumber(L, 3, 0.0);
    Runtime& caller = Runtime::of(L);
    Shared& shared = caller.shared();
    if (!shared.streamAroundHook)
        return 0;
    checkYieldable(L);
    const std::uint64_t request = ++shared.lastStreamAround;
    shared.streamArounds[request] = Shared::StreamAroundWait{&caller, L, caller.suspend(L)};
    shared.streamAroundHook(request, position, timeout);
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

int playerDistanceFromCharacter(lua_State* L)
{
    auto* player = dynamic_cast<Player*>(&self(L));
    const Vector3 point = checkVector3(L, 2);
    Model* character = player ? player->character() : nullptr;
    auto* root = character ? dynamic_cast<BasePart*>(character->findFirstChild("HumanoidRootPart")) : nullptr;
    pushNumber(L, root ? static_cast<double>((root->position() - point).magnitude()) : 0.0);
    return 1;
}

int zero(lua_State* L)
{
    self(L);
    lua_pushnumber(L, 0);
    return 1;
}

int emptyTable(lua_State* L)
{
    self(L);
    lua_newtable(L);
    return 1;
}

int emptyString(lua_State* L)
{
    self(L);
    lua_pushliteral(L, "");
    return 1;
}

// Player:GetMouse(): the legacy Mouse object, one per player, already
// knowing where the pointer is and how big the view is.
int playerGetMouse(lua_State* L)
{
    Instance& mouse = legacyMouse(self(L));
    Runtime& rt = Runtime::of(L);
    if (Workspace* workspace = rt.game().workspace())
        updateMouseView(rt.shared(), mouse, inputState(rt.shared()).mousePosition - guiCoordinateOrigin(rt.game()),
                        workspace->ensureCurrentCamera()->viewportSize());
    return returnInstance(L, &mouse);
}

// --- Debris ------------------------------------------------------------------------

int debrisAddItem(lua_State* L)
{
    self(L);
    Instance& item = checkInstance(L, 2);
    const double lifetime = luaL_optnumber(L, 3, 10.0);
    Runtime& rt = Runtime::of(L);
    rt.after(lifetime, [weak = std::weak_ptr<Instance>(item.shared_from_this())] {
        if (auto instance = weak.lock(); instance && !instance->destroyed())
            instance->destroy();
    });
    return 0;
}

// --- HttpService ----------------------------------------------------------------

void encodeString(std::string& out, std::string_view text)
{
    out += '"';
    for (char c : text) {
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
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            else
                out += c;
        }
    }
    out += '"';
}

void encodeJson(lua_State* L, int idx, std::string& out, int depth)
{
    if (depth > 64)
        raise(L, "Can't convert to JSON");
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        out += "null";
        return;
    case LUA_TBOOLEAN:
        out += lua_toboolean(L, idx) ? "true" : "false";
        return;
    case LUA_TNUMBER: {
        const double d = lua_tonumber(L, idx);
        if (std::isnan(d) || std::isinf(d))
            out += "null";
        else if (d == std::floor(d) && std::fabs(d) < 1e15)
            out += std::format("{}", static_cast<long long>(d));
        else
            out += std::format("{}", d);
        return;
    }
    case LUA_TSTRING:
        encodeString(out, lua_tostring(L, idx));
        return;
    case LUA_TTABLE:
        break;
    default:
        out += "null";
        return;
    }
    idx = lua_absindex(L, idx);
    const int length = lua_objlen(L, idx);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++count;
        lua_pop(L, 1);
    }
    if (count == 0) {
        out += "[]";
        return;
    }
    if (count == length) {
        out += '[';
        for (int i = 1; i <= length; ++i) {
            if (i > 1)
                out += ',';
            lua_rawgeti(L, idx, i);
            encodeJson(L, -1, out, depth + 1);
            lua_pop(L, 1);
        }
        out += ']';
        return;
    }
    out += '{';
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (!first)
            out += ',';
        first = false;
        if (lua_type(L, -2) == LUA_TSTRING) {
            encodeString(out, lua_tostring(L, -2));
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
            lua_pushvalue(L, -2);
            encodeString(out, lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            raise(L, "Can't convert to JSON");
        }
        out += ':';
        encodeJson(L, -1, out, depth + 1);
        lua_pop(L, 1);
    }
    out += '}';
}

struct JsonParser {
    lua_State* L;
    std::string_view text;
    std::size_t at = 0;

    [[noreturn]] void fail() const { raise(L, "Can't parse JSON"); }

    void skip()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
            ++at;
    }

    bool literal(std::string_view word)
    {
        if (!text.substr(at).starts_with(word))
            return false;
        at += word.size();
        return true;
    }

    void appendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t hex4()
    {
        if (at + 4 > text.size())
            fail();
        std::uint32_t value = 0;
        auto [end, ec] = std::from_chars(text.data() + at, text.data() + at + 4, value, 16);
        if (ec != std::errc() || end != text.data() + at + 4)
            fail();
        at += 4;
        return value;
    }

    std::string string()
    {
        std::string out;
        ++at; // opening quote
        while (at < text.size() && text[at] != '"') {
            char c = text[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size())
                fail();
            char e = text[at++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && literal("\\u")) {
                    const std::uint32_t low = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                fail();
            }
        }
        if (at >= text.size())
            fail();
        ++at;
        return out;
    }

    void value(int depth)
    {
        if (depth > 128)
            fail();
        lua_checkstack(L, 4);
        skip();
        if (at >= text.size())
            fail();
        const char c = text[at];
        if (c == '{') {
            ++at;
            lua_newtable(L);
            skip();
            if (at < text.size() && text[at] == '}') {
                ++at;
                return;
            }
            while (true) {
                skip();
                if (at >= text.size() || text[at] != '"')
                    fail();
                std::string key = string();
                skip();
                if (at >= text.size() || text[at] != ':')
                    fail();
                ++at;
                pushString(L, key);
                value(depth + 1);
                lua_rawset(L, -3);
                skip();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == '}') {
                    ++at;
                    return;
                }
                fail();
            }
        }
        if (c == '[') {
            ++at;
            lua_newtable(L);
            skip();
            if (at < text.size() && text[at] == ']') {
                ++at;
                return;
            }
            for (int i = 1;; ++i) {
                value(depth + 1);
                lua_rawseti(L, -2, i);
                skip();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == ']') {
                    ++at;
                    return;
                }
                fail();
            }
        }
        if (c == '"') {
            pushString(L, string());
            return;
        }
        if (literal("true")) {
            lua_pushboolean(L, true);
            return;
        }
        if (literal("false")) {
            lua_pushboolean(L, false);
            return;
        }
        if (literal("null")) {
            lua_pushnil(L);
            return;
        }
        double number = 0;
        auto [end, ec] = std::from_chars(text.data() + at, text.data() + text.size(), number);
        if (ec != std::errc())
            fail();
        at = static_cast<std::size_t>(end - text.data());
        lua_pushnumber(L, number);
    }
};

int jsonEncode(lua_State* L)
{
    self(L);
    std::string out;
    encodeJson(L, 2, out, 0);
    pushString(L, out);
    return 1;
}

int jsonDecode(lua_State* L)
{
    self(L);
    JsonParser parser{L, checkString(L, 2)};
    parser.value(0);
    parser.skip();
    if (parser.at != parser.text.size())
        parser.fail();
    return 1;
}

} // namespace

const assets::RobloxWeb& robloxWeb(lua_State* L)
{
    Shared& shared = Runtime::of(L).shared();
    if (!shared.web) {
        if (!assets::AssetConfig::fromEnvironment().download)
            raise(L, "HTTP requests are off (LUNAR_OFFLINE)");
        shared.web = std::make_shared<const assets::RobloxWeb>();
    }
    return *shared.web;
}

namespace {

// --- Players' web lookups -------------------------------------------------------
// The client asks Roblox's web APIs and yields. The avatar ones block the
// frame for the request instead (a script calls them once per player); the
// name lookups, which leaderboards make by the hundred, yield (lookUpUser).

// Players:GetHumanoidDescriptionFromUserId: avatar-fetch (0x104e2ccc2)
// made into a HumanoidDescription.
int playersGetHumanoidDescriptionFromUserId(lua_State* L)
{
    self(L);
    const double id = luaL_checknumber(L, 2);
    if (!(id > 0) || id != std::floor(id))
        raise(L, "Players:GetHumanoidDescriptionFromUserId() got invalid userId");
    auto avatar = avatar::fetchUserAvatar(robloxWeb(L), static_cast<std::int64_t>(id));
    if (!avatar)
        raise(L, std::format("Players:GetHumanoidDescriptionFromUserId() failed because {}", avatar.error()));
    return returnInstance(L, avatar->description.get());
}

// Players:GetCharacterAppearanceInfoAsync: avatar.roblox.com's
// v2/avatar/users/{id}/avatar reply as a table (0x104e29e26).
int playersGetCharacterAppearanceInfoAsync(lua_State* L)
{
    self(L);
    const double id = luaL_checknumber(L, 2);
    if (!(id > 0) || id != std::floor(id))
        raise(L, "Players:GetCharacterAppearanceInfoAsync() must be called with a positive userId");
    auto json = avatar::fetchCharacterAppearanceInfo(robloxWeb(L), static_cast<std::int64_t>(id));
    if (!json)
        raise(L, std::format("Players:GetCharacterAppearanceInfoAsync() failed: {}", json.error()));
    JsonParser parser{L, *json};
    parser.value(0);
    return 1;
}

// Polls a UserLookup each frame until it's done, then resumes the thread
// that asked (or fails it with the error), keeping a successful answer.
struct AwaitUserLookup {
    Runtime* rt;
    lua_State* thread;
    std::uint64_t token;
    std::string key, method;
    std::shared_ptr<UserLookup> lookup;
    std::function<int(lua_State*, const std::string&)> push;

    void operator()()
    {
        if (!lookup->done.load(std::memory_order_acquire)) {
            Runtime& runtime = *rt;
            runtime.after(0, std::move(*this));
            return;
        }
        SharedState& state = *rt->shared().state;
        if (const auto pending = state.pendingUserLookups.find(key);
            pending != state.pendingUserLookups.end() && pending->second == lookup)
            state.pendingUserLookups.erase(pending);
        if (lookup->result)
            state.userLookups.insert_or_assign(key, *lookup->result);
        if (!rt->isSuspended(thread, token))
            return;
        if (lookup->result)
            rt->wake(thread, token, [&](lua_State* L) { return push(L, *lookup->result); }, method);
        else
            rt->fail(thread, token, std::format("Players:{}() failed: {}", method, lookup->result.error()), method);
    }
};

// Answers a name lookup the way the client does, yielding meanwhile: a
// worker thread asks users.roblox.com, and the thread resumes with the
// answer a later frame. Answers are kept for the session, and a lookup
// already in flight is shared. Blocking the frame instead froze Ball Tower
// Defense for 15 seconds while its leaderboards looked up a hundred names.
int lookUpUser(lua_State* L, std::string key, std::string_view method,
               std::function<std::expected<std::string, std::string>(const assets::RobloxWeb&)> work,
               std::function<int(lua_State*, const std::string&)> push)
{
    Runtime& rt = Runtime::of(L);
    SharedState& state = *rt.shared().state;
    if (const auto known = state.userLookups.find(key); known != state.userLookups.end())
        return push(L, known->second);
    checkYieldable(L);
    std::shared_ptr<UserLookup> lookup;
    if (const auto pending = state.pendingUserLookups.find(key); pending != state.pendingUserLookups.end())
        lookup = pending->second;
    else {
        robloxWeb(L);
        lookup = std::make_shared<UserLookup>();
        state.pendingUserLookups.emplace(key, lookup);
        try {
            std::thread([lookup, web = rt.shared().web, work = std::move(work)] {
                try {
                    lookup->result = work(*web);
                } catch (const std::exception& error) {
                    lookup->result = std::unexpected(error.what());
                }
                lookup->done.store(true, std::memory_order_release);
            }).detach();
        } catch (const std::system_error& error) {
            lookup->result = std::unexpected(std::string("the request could not start: ") + error.what());
            lookup->done.store(true, std::memory_order_release);
        }
    }
    rt.after(0, AwaitUserLookup{&rt, L, rt.suspend(L), std::move(key), std::string(method), std::move(lookup),
                                std::move(push)});
    return lua_yield(L, 0);
}

int playersGetUserIdFromNameAsync(lua_State* L)
{
    self(L);
    const std::string name(checkString(L, 2));
    // A name made of digits would read as an id.
    if (name.empty() || std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; }))
        raise(L, "Players:GetUserIdFromNameAsync() failed: Unknown user");
    return lookUpUser(
        L, "id:" + name, "GetUserIdFromNameAsync",
        [name](const assets::RobloxWeb& web) -> std::expected<std::string, std::string> {
            auto user = avatar::findUser(web, name);
            if (!user)
                return std::unexpected(user.error());
            return std::to_string(user->id);
        },
        [](lua_State* thread, const std::string& id) {
            lua_pushnumber(thread, std::stod(id));
            return 1;
        });
}

int playersGetNameFromUserIdAsync(lua_State* L)
{
    self(L);
    const double id = luaL_checknumber(L, 2);
    if (!(id > 0) || id != std::floor(id))
        raise(L, "Players:GetNameFromUserIdAsync() failed: Unknown user");
    const std::string text = std::to_string(static_cast<std::int64_t>(id));
    return lookUpUser(
        L, "name:" + text, "GetNameFromUserIdAsync",
        [text](const assets::RobloxWeb& web) -> std::expected<std::string, std::string> {
            auto user = avatar::findUser(web, text);
            if (!user)
                return std::unexpected(user.error());
            return user->name;
        },
        [](lua_State* thread, const std::string& name) {
            pushString(thread, name);
            return 1;
        });
}

int generateGuid(lua_State* L)
{
    self(L);
    const std::uint64_t a = scriptEntropy(), b = scriptEntropy();
    std::string guid
        = std::format("{:08X}-{:04X}-4{:03X}-{:04X}-{:012X}", static_cast<std::uint32_t>(a >> 32),
                      static_cast<std::uint32_t>((a >> 16) & 0xFFFF), static_cast<std::uint32_t>(a & 0xFFF),
                      static_cast<std::uint32_t>(0x8000 | ((b >> 48) & 0x3FFF)), b & 0xFFFFFFFFFFFFull);
    const bool curly = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    pushString(L, curly ? std::format("{{{}}}", guid) : guid);
    return 1;
}

int urlEncode(lua_State* L)
{
    self(L);
    std::string out;
    for (char ch : checkString(L, 2)) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", c);
    }
    pushString(L, out);
    return 1;
}

// No network for scripts: the offline client behaves like HttpEnabled = false.
int httpRequest(lua_State* L)
{
    self(L);
    raise(L, "Http requests are not enabled. Enable via game settings");
}

// --- CollectionService ---------------------------------------------------------

constexpr std::string_view addedPrefix = "InstanceAdded:";
constexpr std::string_view removedPrefix = "InstanceRemoved:";

Instance* collectionService(DataModel& game)
{
    return game.findService("CollectionService");
}

bool inGame(const Instance& instance, const DataModel& game)
{
    return instance.isDescendantOf(&game);
}

int collectionAddTag(lua_State* L)
{
    self(L);
    addTagAndNotify(L, checkInstance(L, 2), checkString(L, 3));
    return 0;
}

int collectionRemoveTag(lua_State* L)
{
    self(L);
    removeTagAndNotify(L, checkInstance(L, 2), checkString(L, 3));
    return 0;
}

int collectionHasTag(lua_State* L)
{
    self(L);
    lua_pushboolean(L, checkInstance(L, 2).hasTag(checkString(L, 3)));
    return 1;
}

int collectionGetTags(lua_State* L)
{
    self(L);
    const auto& tags = checkInstance(L, 2).tags();
    lua_createtable(L, static_cast<int>(tags.size()), 0);
    int i = 0;
    for (const std::string& tag : tags) {
        pushString(L, tag);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

void indexTag(SharedState& state, Instance& instance, std::string_view tag)
{
    if (!state.taggedBuilt)
        return;
    auto it = state.tagged.find(tag);
    if (it == state.tagged.end())
        it = state.tagged.emplace(std::string(tag), std::vector<Instance*>{}).first;
    if (!std::ranges::contains(it->second, &instance))
        it->second.push_back(&instance);
}

void unindexTag(SharedState& state, const Instance& instance, std::string_view tag)
{
    if (auto it = state.tagged.find(tag); it != state.tagged.end())
        std::erase(it->second, &instance);
}

const std::vector<Instance*>& taggedInstances(Shared& shared, DataModel& game, std::string_view tag)
{
    SharedState& state = *shared.state;
    if (!state.taggedBuilt) {
        watchTags(shared, game);
        for (const InstancePtr& instance : game.descendants())
            for (const std::string& name : instance->tags())
                state.tagged[name].push_back(instance.get());
        state.taggedBuilt = true;
    }
    static const std::vector<Instance*> none;
    const auto it = state.tagged.find(tag);
    return it == state.tagged.end() ? none : it->second;
}

int collectionGetTagged(lua_State* L)
{
    self(L);
    std::string_view tag = checkString(L, 2);
    Runtime& rt = Runtime::of(L);
    const std::vector<Instance*>& tagged = taggedInstances(rt.shared(), rt.game(), tag);
    lua_createtable(L, static_cast<int>(tagged.size()), 0);
    int i = 0;
    for (Instance* instance : tagged) {
        pushInstance(L, instance);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int collectionGetAllTags(lua_State* L)
{
    self(L);
    std::vector<std::string> all;
    for (const InstancePtr& instance : Runtime::of(L).game().descendants())
        for (const std::string& tag : instance->tags())
            if (std::ranges::find(all, tag) == all.end())
                all.push_back(tag);
    lua_createtable(L, static_cast<int>(all.size()), 0);
    int i = 0;
    for (const std::string& tag : all) {
        pushString(L, tag);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

template <bool Added>
int collectionInstanceSignal(lua_State* L)
{
    Instance& service = self(L);
    std::string_view tag = checkString(L, 2);
    watchTags(Runtime::of(L).shared(), Runtime::of(L).game());
    pushHubSignal(L, service, std::format("{}{}", Added ? addedPrefix : removedPrefix, tag));
    return 1;
}

// --- TweenService, Tween -----------------------------------------------------------

int tweenServiceGetValue(lua_State* L)
{
    self(L);
    const double alpha = luaL_checknumber(L, 2);
    auto style = toEnumItem(L, 3, "EasingStyle");
    auto direction = toEnumItem(L, 4, "EasingDirection");
    if (!style)
        luaL_typeerror(L, 3, "Enum.EasingStyle");
    if (!direction)
        luaL_typeerror(L, 4, "Enum.EasingDirection");
    pushNumber(L, ease(alpha, style->name, direction->name));
    return 1;
}

int tweenServiceCreate(lua_State* L)
{
    self(L);
    Instance& target = checkInstance(L, 2);
    const TweenInfo& info = check<TweenInfo>(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);

    std::vector<TweenTrack> tracks;
    lua_pushnil(L);
    while (lua_next(L, 4)) {
        std::string_view name = checkString(L, -2);
        const Property* property = target.classInfo().findProperty(name);
        // Or one only the API knows, kept beside the instance as scripts set it.
        const api::MemberData* member = property ? nullptr : api::findMember(target.className(), name);
        if (!property && (!member || member->kind != api::MemberKind::Property))
            raise(L, std::format("{} is not a valid member of {} \"{}\"", name, target.className(), target.fullName()));
        std::optional<Value> goal = toValue(L, -1);
        if (!goal || !tweenable(*goal)) {
            const char* given = luaL_typename(L, -1);
            const std::string_view type = property ? std::string_view(property->type) : member->type;
            raise(L, std::vformat(concat("TweenService:Create property named '{}' cannot be tweened due to type ",
                                         "mismatch (property is a '{}', but given type is '{}')"),
                                  std::make_format_args(name, type, given)));
        }
        tracks.push_back(TweenTrack{std::string(name), Value(), std::move(*goal)});
        lua_pop(L, 1);
    }
    Runtime& rt = Runtime::of(L);
    InstancePtr tween = createTween(rt.shared(), target, info, std::move(tracks));
    return returnInstance(L, tween.get());
}

// A GuiObject tween's callback: Completed's PlaybackState as the
// Enum.TweenStatus the callback takes (upvalue 1).
int guiTweenDone(lua_State* L)
{
    auto state = toEnumItem(L, 1, "PlaybackState");
    lua_pushvalue(L, lua_upvalueindex(1));
    pushEnumItem(L, enumItem("TweenStatus", state && state->name == "Completed" ? "Completed" : "Canceled"));
    lua_call(L, 1, 0);
    return 0;
}

// GuiObject:TweenSize(size, direction, style, time, override, callback),
// TweenPosition and TweenSizeAndPosition: a tween of the object as
// TweenService's, Out and Quad over a second unless said. One runs on an
// object at a time: a new one replaces it with override, else isn't
// started (false).
int guiTween(lua_State* L, bool size, bool position)
{
    Instance& target = self(L);
    int arg = 2;
    std::vector<TweenTrack> tracks;
    if (size)
        tracks.push_back(TweenTrack{"Size", Value(), Value(check<UDim2>(L, arg++))});
    if (position)
        tracks.push_back(TweenTrack{"Position", Value(), Value(check<UDim2>(L, arg++))});
    TweenInfo info;
    auto direction = lua_isnoneornil(L, arg) ? std::nullopt : toEnumItem(L, arg, "EasingDirection");
    info.easingDirection = direction ? *direction : enumItem("EasingDirection", "Out");
    auto style = lua_isnoneornil(L, arg + 1) ? std::nullopt : toEnumItem(L, arg + 1, "EasingStyle");
    info.easingStyle = style ? *style : enumItem("EasingStyle", "Quad");
    info.time = luaL_optnumber(L, arg + 2, 1.0);
    const bool override = lua_toboolean(L, arg + 3) != 0;
    const int callback = arg + 4;

    Runtime& rt = Runtime::of(L);
    Shared& shared = rt.shared();
    for (const auto& other : shared.state->tweens) {
        if (!other->gui || other->target.lock().get() != &target
            || (other->state != "Playing" && other->state != "Delayed"))
            continue;
        if (!override) {
            lua_pushboolean(L, false);
            return 1;
        }
        if (auto running = other->tween.lock())
            commandTween(shared, *running, TweenCommand::Cancel);
    }
    InstancePtr tween = createTween(shared, target, info, std::move(tracks));
    for (const auto& made : shared.state->tweens)
        if (made->tween.lock() == tween)
            made->gui = true;
    if (lua_isfunction(L, callback)) {
        // tween.Completed:Connect(done), done calling the callback.
        pushInstance(L, tween.get());
        lua_getfield(L, -1, "Completed");
        lua_getfield(L, -1, "Connect");
        lua_pushvalue(L, -2);
        lua_pushvalue(L, callback);
        lua_pushcclosure(L, guiTweenDone, "guiTweenDone", 1);
        lua_call(L, 2, 0);
        lua_pop(L, 2);
    }
    commandTween(shared, *tween, TweenCommand::Play);
    lua_pushboolean(L, true);
    return 1;
}

int guiTweenSize(lua_State* L)
{
    return guiTween(L, true, false);
}

int guiTweenPosition(lua_State* L)
{
    return guiTween(L, false, true);
}

int guiTweenSizeAndPosition(lua_State* L)
{
    return guiTween(L, true, true);
}

template <TweenCommand Command>
int tweenCommand(lua_State* L)
{
    Instance& tween = self(L);
    commandTween(Runtime::of(L).shared(), tween, Command);
    return 0;
}

// --- UserInputService, ContextActionService ------------------------------------------

int uisIsKeyDown(lua_State* L)
{
    self(L);
    auto key = toEnumItem(L, 2, "KeyCode");
    lua_pushboolean(L, key && inputState(Runtime::of(L).shared()).keyDown(static_cast<Enum::KeyCode>(key->value)));
    return 1;
}

int uisIsMouseButtonPressed(lua_State* L)
{
    self(L);
    auto type = toEnumItem(L, 2, "UserInputType");
    lua_pushboolean(
        L, type && inputState(Runtime::of(L).shared()).mouseButtonDown(static_cast<Enum::UserInputType>(type->value)));
    return 1;
}

int uisGetMouseLocation(lua_State* L)
{
    self(L);
    push<Vector2>(L, inputState(Runtime::of(L).shared()).mousePosition);
    return 1;
}

int uisGetMouseDelta(lua_State* L)
{
    self(L);
    push<Vector2>(L, inputState(Runtime::of(L).shared()).mouseDelta);
    return 1;
}

int textBoxFocus(lua_State* L)
{
    auto& box = static_cast<TextBox&>(self(L));
    const bool capture = std::string_view(methodName(L)) == "CaptureFocus";
    const bool entered = !capture && lua_toboolean(L, 2);
    Runtime& rt = Runtime::of(L);
    if (rt.shared().guiFocus) rt.shared().guiFocus(&box, capture, entered);
    else if (capture) {
        auto* input = rt.game().getService<UserInputService>();
        if (input->focusedTextBox() == &box) return 0;
        if (auto* previous = input->focusedTextBox()) { previous->setCursorPosition(-1); previous->fireFocusLost(false); }
        box.setCursorPosition(1);
        box.fireFocused();
    } else if (rt.game().getService<UserInputService>()->focusedTextBox() == &box) {
        box.setCursorPosition(-1);
        box.fireFocusLost(entered);
    }
    return 0;
}

int uisGetFocusedTextBox(lua_State* L)
{
    return returnInstance(L, static_cast<UserInputService&>(self(L)).focusedTextBox());
}

int uisGetLastInputType(lua_State* L)
{
    self(L);
    pushEnumItem(L, enums::item(inputState(Runtime::of(L).shared()).lastInputType));
    return 1;
}

int uisGetKeysPressed(lua_State* L)
{
    self(L);
    const InputState& input = inputState(Runtime::of(L).shared());
    lua_createtable(L, static_cast<int>(input.keys.size()), 0);
    int i = 0;
    for (const auto& object : input.pressedKeyObjects()) {
        pushInstance(L, object.get());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int uisGetStringForKeyCode(lua_State* L)
{
    self(L);
    auto key = toEnumItem(L, 2, "KeyCode");
    if (!key)
        luaL_typeerror(L, 2, "Enum.KeyCode");
    // Printable keys give their character, upper case for letters.
    std::string text;
    if (key->value >= 97 && key->value <= 122)
        text = static_cast<char>('A' + (key->value - 97));
    else if (key->value >= 32 && key->value < 127)
        text = static_cast<char>(key->value);
    else
        text = key->name;
    pushString(L, text);
    return 1;
}

int uisFalse(lua_State* L)
{
    self(L);
    lua_pushboolean(L, false);
    return 1;
}

int uisGetPlatform(lua_State* L)
{
    self(L);
    // The Mac build's unless the embedding application says otherwise.
    pushEnumItem(L, enumItem("Platform", Runtime::of(L).shared().platform));
    return 1;
}

int casBindAction(lua_State* L, bool withPriority)
{
    self(L);
    std::string name(checkString(L, 2));
    luaL_checktype(L, 3, LUA_TFUNCTION);
    int priority = 2000; // Enum.ContextActionPriority.Default
    int first = 5;
    if (withPriority) {
        priority = static_cast<int>(luaL_checknumber(L, 5));
        first = 6;
    }
    std::vector<ActionInput> inputs;
    for (int i = first; i <= lua_gettop(L); ++i) {
        if (auto key = toEnumItem(L, i, "KeyCode", true))
            inputs.push_back(ActionInput{true, key->value});
        else if (auto type = toEnumItem(L, i, "UserInputType", true))
            inputs.push_back(ActionInput{false, type->value});
        else if (auto action = toEnumItem(L, i, "PlayerActions", true))
            continue; // movement actions: the control scripts' business
        else if (lua_type(L, i) == LUA_TSTRING) {
            // The client takes strings too ("invalid hotkey (should be either
            // Enum.KeyCode, Enum.UserInputType or string)"). GUESS: a single
            // character is that key, as KeyCode values are the lower-case
            // characters; longer strings bind nothing (scripts pass "On").
            const std::string_view text = checkString(L, i);
            if (text.size() == 1 && text[0] > ' ' && text[0] < 127)
                inputs.push_back(
                    ActionInput{true, static_cast<std::uint32_t>(std::tolower(static_cast<unsigned char>(text[0])))});
        } else
            luaL_typeerror(L, i, "Enum.KeyCode");
    }
    Runtime& rt = Runtime::of(L);
    lua_pushvalue(L, 3);
    bindAction(rt, std::move(name), lua_ref(L, -1), priority, std::move(inputs), rt.accessOf(L),
               Runtime::refGlobals(L));
    lua_pop(L, 1);
    return 0;
}

int casBind(lua_State* L)
{
    return casBindAction(L, false);
}

int casBindAtPriority(lua_State* L)
{
    return casBindAction(L, true);
}

int casUnbind(lua_State* L)
{
    self(L);
    unbindAction(Runtime::of(L), checkString(L, 2));
    return 0;
}

int casUnbindAll(lua_State* L)
{
    self(L);
    unbindAllActions(Runtime::of(L));
    return 0;
}

int casGetButton(lua_State* L)
{
    self(L);
    lua_pushnil(L);
    return 1;
}

// --- TextService -------------------------------------------------------------------

// TextService:GetTextSize, approximated: average glyph width of half the text
// size, lines of the text size, wrapping at the frame width. (Guess: real
// metrics need the font files; this keeps layouts sane.)
int textGetTextSize(lua_State* L)
{
    self(L);
    std::string_view text = checkString(L, 2);
    const double size = luaL_checknumber(L, 3);
    const auto frame = opt<Vector2>(L, 5, Vector2{1e9f, 1e9f});
    const double glyph = size * 0.5;
    double width = 0, lineWidth = 0;
    int lines = 1;
    for (char c : text) {
        if (c == '\n' || lineWidth + glyph > static_cast<double>(frame.x)) {
            width = std::max(width, lineWidth);
            lineWidth = 0;
            ++lines;
            if (c == '\n')
                continue;
        }
        lineWidth += glyph;
    }
    width = std::max(width, lineWidth);
    push<Vector2>(L, Vector2{static_cast<float>(std::ceil(width)),
                             static_cast<float>(std::min(lines * size, static_cast<double>(frame.y)))});
    return 1;
}

// --- PhysicsService ------------------------------------------------------------------

Workspace* workspaceOf(lua_State* L)
{
    return Runtime::of(L).game().workspace();
}

int physicsRegisterGroup(lua_State* L)
{
    self(L);
    std::string name(checkString(L, 2));
    Workspace* workspace = workspaceOf(L);
    auto groups = workspace->collisionGroups();
    if (std::ranges::find(groups, name, &CollisionGroup::name) == groups.end()) {
        int id = 0;
        for (const CollisionGroup& group : groups)
            id = std::max(id, group.id + 1);
        if (id >= 32)
            raise(L, "Could not create collision group, the limit has been reached.");
        groups.push_back(CollisionGroup{name, id, 0xFFFFFFFF});
        workspace->setCollisionGroups(std::move(groups));
    }
    pushNumber(L, 0);
    return 1;
}

int physicsSetCollidable(lua_State* L)
{
    self(L);
    std::string_view a = checkString(L, 2);
    std::string_view b = checkString(L, 3);
    const bool collide = lua_toboolean(L, 4) != 0;
    Workspace* workspace = workspaceOf(L);
    auto groups = workspace->collisionGroups();
    auto ga = std::ranges::find(groups, a, &CollisionGroup::name);
    auto gb = std::ranges::find(groups, b, &CollisionGroup::name);
    if (ga == groups.end() || gb == groups.end())
        raise(L, "Both collision groups must be valid.");
    auto set = [collide](CollisionGroup& group, int id) {
        const std::uint32_t bit = 1u << id;
        group.mask = collide ? (group.mask | bit) : (group.mask & ~bit);
    };
    set(*ga, gb->id);
    set(*gb, ga->id);
    workspace->setCollisionGroups(std::move(groups));
    return 0;
}

int physicsGetGroupId(lua_State* L)
{
    self(L);
    std::string_view name = checkString(L, 2);
    for (const CollisionGroup& group : workspaceOf(L)->collisionGroups())
        if (group.name == name) {
            pushNumber(L, group.id);
            return 1;
        }
    raise(L, std::format("Invalid collision group name: {}", name));
}

int physicsCollidable(lua_State* L)
{
    self(L);
    lua_pushboolean(L, workspaceOf(L)->collisionGroupsCollide(checkString(L, 2), checkString(L, 3)));
    return 1;
}

int physicsSetPartGroup(lua_State* L)
{
    self(L);
    auto* part = dynamic_cast<BasePart*>(&checkInstance(L, 2));
    if (part)
        part->setCollisionGroup(std::string(checkString(L, 3)));
    return 0;
}

int physicsIsRegistered(lua_State* L)
{
    self(L);
    std::string_view name = checkString(L, 2);
    const auto& groups = workspaceOf(L)->collisionGroups();
    lua_pushboolean(L, std::ranges::find(groups, name, &CollisionGroup::name) != groups.end());
    return 1;
}

// --- Workspace queries ----------------------------------------------------------------

std::vector<InstancePtr> lockFilter(const std::vector<std::weak_ptr<Instance>>& filter)
{
    std::vector<InstancePtr> result;
    for (const auto& weak : filter)
        if (auto instance = weak.lock())
            result.push_back(std::move(instance));
    return result;
}

std::optional<RaycastHit> castRay(lua_State* L, const RaycastQuery& query)
{
    Runtime& rt = Runtime::of(L);
    if (!rt.shared().raycast)
        return std::nullopt;
    return rt.shared().raycast(rt.game(), query);
}

int workspaceRaycast(lua_State* L)
{
    self(L);
    RaycastQuery query;
    query.origin = checkVector3(L, 2);
    query.direction = checkVector3(L, 3);
    if (const RaycastParams* params = to<RaycastParams>(L, 4)) {
        query.filter = lockFilter(params->filterDescendantsInstances);
        query.filterIsInclude = params->filterType.name == "Include";
        query.ignoreWater = params->ignoreWater;
        query.respectCanCollide = params->respectCanCollide;
        query.collisionGroup = params->collisionGroup;
    }
    auto hit = castRay(L, query);
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    push<RaycastResult>(
        L, RaycastResult{hit->instance, hit->position, hit->normal, enums::item(hit->material), hit->distance});
    return 1;
}

// The legacy FindPartOnRay family: (ray, ignore[, terrainCellsAreCubes, ignoreWater]).
template <bool List, bool Whitelist>
int workspaceFindPartOnRay(lua_State* L)
{
    self(L);
    const Ray& ray = check<Ray>(L, 2);
    RaycastQuery query{ray.origin, ray.direction};
    if constexpr (List) {
        if (lua_istable(L, 3)) {
            const int n = lua_objlen(L, 3);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, 3, i);
                if (Instance* instance = toInstance(L, -1))
                    query.filter.push_back(instance->shared_from_this());
                lua_pop(L, 1);
            }
        }
        query.ignoreWater = lua_toboolean(L, 5) != 0;
    } else if (Instance* ignore = toInstance(L, 3)) {
        query.filter.push_back(ignore->shared_from_this());
        query.ignoreWater = lua_toboolean(L, 5) != 0;
    }
    query.filterIsInclude = Whitelist;
    auto hit = castRay(L, query);
    if (!hit) {
        lua_pushnil(L);
        pushVector3(L, ray.origin + ray.direction);
        pushVector3(L, Vector3{});
        pushEnumItem(L, enums::item(Enum::Material::Air));
        return 4;
    }
    pushInstance(L, hit->instance.get());
    pushVector3(L, hit->position);
    pushVector3(L, hit->normal);
    pushEnumItem(L, enums::item(hit->material));
    return 4;
}

int workspaceServerTimeNow(lua_State* L)
{
    self(L);
    lua_pushnumber(L, scriptWallClock(L));
    return 1;
}

int sixty(lua_State* L)
{
    self(L);
    lua_pushnumber(L, 60);
    return 1;
}

// HapticService: no controller here rumbles. A motor reads as stopped.
int hapticGetMotor(lua_State* L)
{
    self(L);
    lua_pushnumber(L, 0);
    return 1;
}

// GuiService:GetGuiInset(): the room the top bar takes, top-left then
// bottom-right, and what SetGlobalGuiInset adds. The renderer supplies the
// active bar height (58 for the modern desktop UI, zero for the Lua app).
// GUI queries and camera screen coordinates use the same origin.
} // namespace

Vector2 guiCoordinateOrigin(const DataModel& game)
{
    const auto* service = instanceCast<const GuiService>(game.findService("GuiService"));
    const auto global = service ? service->globalGuiInset() : GuiService::Inset{};
    return {global.left, (service ? service->topbarInset().height() : 36.0f) + global.top};
}

namespace {

int guiGetGuiInset(lua_State* L)
{
    auto* service = dynamic_cast<GuiService*>(&self(L));
    const GuiService::Inset global = service ? service->globalGuiInset() : GuiService::Inset{};
    push<Vector2>(L, guiCoordinateOrigin(Runtime::of(L).game()));
    push<Vector2>(L, Vector2{global.right, global.bottom});
    return 2;
}

// LogService:GetLogHistory(): what this peer has printed, oldest first, as
// {message, messageType, timestamp} tables (the keys of 0x10578cce4).
int logGetHistory(lua_State* L)
{
    self(L);
    static constexpr std::string_view types[] = {"MessageOutput", "MessageInfo", "MessageWarning", "MessageError"};
    const auto& history = Runtime::of(L).logHistory();
    lua_createtable(L, static_cast<int>(history.size()), 0);
    int i = 0;
    for (const Runtime::LogEntry& entry : history) {
        lua_createtable(L, 0, 3);
        pushString(L, entry.message);
        lua_setfield(L, -2, "message");
        pushEnumItem(L, enumItemNamed("MessageType", types[static_cast<int>(entry.type)]));
        lua_setfield(L, -2, "messageType");
        lua_pushnumber(L, entry.timestamp);
        lua_setfield(L, -2, "timestamp");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// --- Bindables and remotes ----------------------------------------------------------

int bindableFire(lua_State* L)
{
    Instance& event = self(L);
    Runtime& rt = Runtime::of(L);
    rt.shared().events.fire(event, "Event", packArgs(L, 2, lua_gettop(L)));
    return 0;
}

int remoteInvoke(lua_State* L, std::string_view callbackName, PackedArgs args);

int bindableInvoke(lua_State* L)
{
    Instance& function = self(L);
    Runtime& rt = Runtime::of(L);
    auto callback = rt.shared().callback(function, "OnInvoke");
    // With no callback yet it waits for one, as a remote's invoke does: a
    // script can invoke before the one that sets OnInvoke has run [live: the
    // sUNC test game's bleepbloop and sunchookfuncthing].
    if (!callback)
        return remoteInvoke(L, "OnInvoke", packArgs(L, 2, lua_gettop(L)));
    return invokeAndWait(L, *callback, packArgs(L, 2, lua_gettop(L)));
}

// --- ContentProvider ---------------------------------------------------------

// The content ids an instance holds: its asset properties (the engine keeps
// them as strings; the API data's newer Content type, and the classic
// ContentId properties by name).
void collectContent(const Instance& instance, const std::function<void(std::string_view)>& add)
{
    static constexpr std::array<std::string_view, 32> names{
        "Image",        "HoverImage",       "PressedImage",  "Texture",       "TextureId",     "TextureID",
        "MeshId",       "SoundId",          "AnimationId",   "ShirtTemplate", "PantsTemplate", "Graphic",
        "SkyboxBk",     "SkyboxDn",         "SkyboxFt",      "SkyboxLf",      "SkyboxRt",      "SkyboxUp",
        "SunTextureId", "MoonTextureId",    "ColorMap",      "NormalMap",     "MetalnessMap",  "RoughnessMap",
        "CursorIcon",   "OverlayTextureId", "BaseTextureId", "Face",          "CageMeshId",    "ReferenceMeshId",
        "VideoContent", "LinkedSource",
    };
    for (const Property* p : instance.properties()) {
        const api::MemberData* member = api::findMember(instance.className(), p->name);
        if (!std::ranges::contains(names, p->name) && !(member && member->type == "Content"))
            continue;
        if (const Value v = p->get(instance); const auto* text = std::get_if<std::string>(&v))
            add(*text);
    }
}

// ContentProvider:PreloadAsync(list [, callback]): the content of the list's
// instances (with their descendants) and strings, each id once, and the
// callback told of each with its AssetFetchStatus. GUESS: all Success
// (Lunarstone fetches when something draws them); Roblox yields until the
// downloads are done.
int contentProviderPreloadAsync(lua_State* L)
{
    self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> ids;
    std::set<std::string, std::less<>> seen;
    auto add = [&](std::string_view id) {
        if (!id.empty() && !seen.contains(id)) {
            seen.emplace(id);
            ids.emplace_back(id);
        }
    };
    const int count = static_cast<int>(lua_objlen(L, 2));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, i);
        if (const Instance* instance = toInstance(L, -1)) {
            collectContent(*instance, add);
            for (const InstancePtr& d : instance->descendants())
                collectContent(*d, add);
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            add(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    // research: LUNAR_TRACE_PRELOAD=1 says who preloads what.
    if (static const char* const trace = std::getenv("LUNAR_TRACE_PRELOAD"); trace && *trace) {
        luaL_where(L, 1);
        std::string what;
        for (int i = 1; i <= count && i <= 4; ++i) {
            lua_rawgeti(L, 2, i);
            const Instance* instance = toInstance(L, -1);
            what += std::format("{}{}", what.empty() ? "" : ", ",
                                instance ? instance->fullName() : std::string(luaL_typename(L, -1)));
            lua_pop(L, 1);
        }
        std::fprintf(stderr, "preload: %s{%s%s} -> %zu ids\n", lua_tostring(L, -1), what.c_str(),
                     count > 4 ? ", ..." : "", ids.size());
        lua_pop(L, 1);
    }
    if (lua_isfunction(L, 3)) {
        const EnumItem success = enumItem("AssetFetchStatus", "Success");
        for (const std::string& id : ids) {
            lua_pushvalue(L, 3);
            pushString(L, id);
            pushEnumItem(L, success);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                Runtime::of(L).output(MessageType::Error, lua_isstring(L, -1) ? lua_tostring(L, -1) : "error");
                lua_pop(L, 1);
            }
        }
    }
    return 0;
}

int contentProviderGetAssetFetchStatus(lua_State* L)
{
    self(L);
    checkString(L, 2);
    pushEnumItem(L, enumItem("AssetFetchStatus", "Success"));
    return 1;
}

// --- Actors ------------------------------------------------------------------

// Actor:BindToMessage(topic, callback) and BindToMessageParallel: a callback
// for the actor's messages on that topic, as a connection; SendMessage fires
// them. Lunarstone runs every script serially, which parallel Luau allows
// (its parallel phase may run on one thread), so both bind the same way.
std::string actorTopic(std::string_view topic)
{
    return std::format("Message {}", topic); // a hub event no real event is named
}

int actorBindToMessage(lua_State* L)
{
    Instance& actor = self(L);
    const std::string_view topic = checkString(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    pushHubSignal(L, actor, actorTopic(topic));
    lua_getfield(L, -1, "Connect");
    lua_pushvalue(L, -2);
    lua_pushvalue(L, 3);
    lua_call(L, 2, 1);
    return 1;
}

int actorSendMessage(lua_State* L)
{
    Instance& actor = self(L);
    const std::string_view topic = checkString(L, 2);
    Runtime::of(L).shared().events.fire(actor, actorTopic(topic), packArgs(L, 3, lua_gettop(L)));
    return 0;
}

void sendRemote(lua_State* L, Instance& remote, std::string event, PackedArgs args)
{
    Runtime& rt = Runtime::of(L);
    rt.shared().inbox.push_back(Shared::Message{remote.shared_from_this(), std::move(event), std::move(args)});
}

} // namespace

// Remotes on a real server (ScriptContext::setRemoteSink): FireServer and
// InvokeServer go to the sink as the network event invocations
// (OnRemoteServerEvent, RemoteOnInvokeServer), and the server's answers come
// back through receiveNetworkRemote. The client's code (0x1045eaf64,
// 0x1045eb5b6) fires the same events, the invocation id from a per-remote
// counter that starts at 1 and wraps after 2^31 - 1 (0x1045eb918).
struct NetworkInvokes {
    struct Waiting {
        std::weak_ptr<Instance> remote;
        Runtime* caller = nullptr;
        lua_State* thread = nullptr;
        std::uint64_t token = 0;
        NativeRemoteReply nativeReply;
    };
    std::map<std::pair<const Instance*, std::uint32_t>, Waiting> waiting;
    std::unordered_map<const Instance*, std::pair<std::weak_ptr<Instance>, std::uint32_t>> counters;
    std::unordered_map<const Instance*, std::size_t> dropped; // OnClientEvents past the queue limit

    std::uint32_t nextId(Instance& remote)
    {
        auto& [owner, last] = counters[&remote];
        if (owner.lock().get() != &remote) { // a dead remote's address came back
            owner = remote.weak_from_this();
            last = 0;
        }
        last = last == 0x7fffffff ? 1 : last + 1;
        return last;
    }
};

namespace {

NetworkInvokes& networkInvokes(Shared& shared)
{
    if (!shared.networkInvokes)
        shared.networkInvokes = std::make_shared<NetworkInvokes>();
    return *shared.networkInvokes;
}

std::vector<RemoteValue> remoteArgs(std::span<const Packed> args)
{
    std::vector<RemoteValue> out;
    out.reserve(args.size());
    for (const Packed& arg : args)
        out.push_back(toRemote(arg));
    return out;
}

PackedArgs packedArgs(std::span<const RemoteValue> args)
{
    PackedArgs out;
    out.reserve(args.size());
    for (const RemoteValue& arg : args)
        out.push_back(fromRemote(arg));
    return out;
}

// Unsent (over the send limits), the invocation is never answered, and the
// thread waits as for any answer.
int networkInvokeServer(lua_State* L, Instance& remote, PackedArgs args, bool send)
{
    checkYieldable(L);
    Runtime& caller = Runtime::of(L);
    NetworkInvokes& invokes = networkInvokes(caller.shared());
    const std::uint32_t id = invokes.nextId(remote);
    invokes.waiting[{&remote, id}] = NetworkInvokes::Waiting{remote.weak_from_this(), &caller, L, caller.suspend(L), {}};
    std::vector<RemoteValue> out{remoteValue(static_cast<double>(id)),
                                 remoteValue(instanceValue(localPlayer(caller.game())))};
    for (const Packed& arg : args)
        out.push_back(toRemote(arg));
    if (send)
        caller.shared().remoteSink(remote, "RemoteOnInvokeServer", std::move(out));
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

// Whether the arguments are over the client's send limits (overSendLimits).
// The writer's throw is caught out of the script's sight: 0.742's preserialize
// wrapper (0x105694dd0) only logs it to DFLog::NetworkSharedQueue, so nothing
// reaches Output or LogService. GUESS: that the client then stays connected.
// Its data-out step (0x1055fde7a) disconnects on a throw, yet sUNC, whose last
// remote call is over the limit, passes on the real client.
bool refuseSend(lua_State* L, int first)
{
    return overSendLimits(L, first, lua_gettop(L)).has_value();
}

// OnClientInvoke for an invocation from the server: waits for the callback
// like Roblox's queue, then sends its results or its error back.
void attemptClientInvoke(Shared& shared, const std::shared_ptr<Instance>& remote, std::uint32_t id, PackedArgs args)
{
    if (remote->destroyed())
        return;
    auto callback = shared.callback(*remote, "OnClientInvoke");
    if (!callback) {
        shared.invocations.emplace_back([&shared, remote, id, args = std::move(args)]() mutable {
            attemptClientInvoke(shared, remote, id, std::move(args));
        });
        return;
    }
    invokeCallback(*callback, args, [&shared, remote, id](bool ok, PackedArgs results) {
        if (!shared.remoteSink)
            return;
        std::vector<RemoteValue> out{remoteValue(static_cast<double>(id))};
        if (ok) {
            for (const Packed& r : results)
                out.push_back(toRemote(r));
            shared.remoteSink(*remote, "RemoteOnInvokeSuccess", std::move(out));
        } else {
            out.push_back(results.empty() ? remoteValue(Value(std::string("error"))) : toRemote(results[0]));
            shared.remoteSink(*remote, "RemoteOnInvokeError", std::move(out));
        }
    });
}

int remoteFireServer(lua_State* L)
{
    Instance& remote = self(L);
    Runtime& rt = Runtime::of(L);
    if (!rt.isClient())
        raise(L, "FireServer can only be called from the client");
    if (rt.shared().remoteSink) {
        if (!refuseSend(L, 2))
            rt.shared().remoteSink(remote, "OnRemoteServerEvent", remoteArgs(packArgs(L, 2, lua_gettop(L))));
        return 0;
    }
    PackedArgs args{packed(instanceValue(localPlayer(rt.game())))};
    for (Packed& arg : packArgs(L, 2, lua_gettop(L)))
        args.push_back(std::move(arg));
    sendRemote(L, remote, "OnServerEvent", std::move(args));
    return 0;
}

int remoteFireClient(lua_State* L)
{
    Instance& remote = self(L);
    Runtime& rt = Runtime::of(L);
    if (!rt.isServer())
        raise(L, "FireClient can only be called from the server");
    auto* player = dynamic_cast<Player*>(toInstance(L, 2));
    if (!player)
        raise(L, "FireClient: player argument must be a Player object");
    // Only the local player has a client in Play Solo.
    if (player == localPlayer(rt.game()))
        sendRemote(L, remote, "OnClientEvent", packArgs(L, 3, lua_gettop(L)));
    return 0;
}

int remoteFireAllClients(lua_State* L)
{
    Instance& remote = self(L);
    if (!Runtime::of(L).isServer())
        raise(L, "FireAllClients can only be called from the server");
    sendRemote(L, remote, "OnClientEvent", packArgs(L, 2, lua_gettop(L)));
    return 0;
}

// A remote function call: the callee runs on the other side on the next
// frame (the loopback's "network"), the caller waits for its results. Calls
// made before the callback is set wait for it, like Roblox's queue.
struct PendingInvoke {
    Runtime* caller = nullptr;
    lua_State* thread = nullptr;
    std::uint64_t token = 0;
    std::weak_ptr<Instance> remote;
    std::string callbackName;
    PackedArgs args;
};

void attemptInvoke(const std::shared_ptr<PendingInvoke>& pending)
{
    Runtime& caller = *pending->caller;
    auto remote = pending->remote.lock();
    if (!remote || !caller.isSuspended(pending->thread, pending->token))
        return;
    auto callback = caller.shared().callback(*remote, pending->callbackName);
    if (!callback) {
        caller.shared().invocations.emplace_back([pending] { attemptInvoke(pending); });
        return;
    }
    invokeCallback(*callback, pending->args, [pending](bool ok, PackedArgs results) {
        Runtime& rt = *pending->caller;
        rt.defer([pending, ok, results = std::move(results)] {
            finishInvoke(*pending->caller, pending->thread, pending->token, ok, results);
        });
    });
}

int remoteInvoke(lua_State* L, std::string_view callbackName, PackedArgs args)
{
    Instance& remote = self(L);
    checkYieldable(L);
    Runtime& caller = Runtime::of(L);
    auto pending = std::make_shared<PendingInvoke>(PendingInvoke{
        &caller, L, caller.suspend(L), remote.shared_from_this(), std::string(callbackName), std::move(args)});
    caller.shared().invocations.emplace_back([pending] { attemptInvoke(pending); });
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

int remoteInvokeServer(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    if (!rt.isClient())
        raise(L, "InvokeServer can only be called from the client");
    if (rt.shared().remoteSink) {
        if (refuseSend(L, 2))
            return networkInvokeServer(L, self(L), {}, false);
        return networkInvokeServer(L, self(L), packArgs(L, 2, lua_gettop(L)), true);
    }
    PackedArgs args{packed(instanceValue(localPlayer(rt.game())))};
    for (Packed& arg : packArgs(L, 2, lua_gettop(L)))
        args.push_back(std::move(arg));
    return remoteInvoke(L, "OnServerInvoke", std::move(args));
}

int remoteInvokeClient(lua_State* L)
{
    if (!Runtime::of(L).isServer())
        raise(L, "InvokeClient can only be called from the server");
    if (!dynamic_cast<Player*>(toInstance(L, 2)))
        raise(L, "InvokeClient: player argument must be a Player object");
    return remoteInvoke(L, "OnClientInvoke", packArgs(L, 3, lua_gettop(L)));
}

// --- Humanoid basics ----------------------------------------------------------------
// (The state machine's methods are in HumanoidApi.cpp.)

int humanoidTakeDamage(lua_State* L)
{
    auto* humanoid = dynamic_cast<Humanoid*>(&self(L));
    const auto amount = static_cast<float>(luaL_checknumber(L, 2));
    Model* character = humanoid ? dynamic_cast<Model*>(humanoid->parent()) : nullptr;
    if (humanoid && !(character && character->findFirstChildOfClass("ForceField")))
        humanoid->setHealth(std::max(0.0f, humanoid->health() - amount));
    return 0;
}

// --- Settings, StarterGui, PlayerGui ------------------------------------------------

// UserSettings():GetService / settings():GetService: settings objects live
// outside the DataModel; each is a placeholder created on first use.
int settingsGetService(lua_State* L)
{
    Instance& provider = self(L);
    std::string_view name = checkString(L, 2);
    for (const InstancePtr& child : provider.children())
        if (child->className() == name)
            return returnInstance(L, child.get());
    if (!api::findClass(name))
        raise(L, std::format("'{}' is not a valid Service name", name));
    auto service = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(name));
    service->setParent(&provider);
    return returnInstance(L, service.get());
}

// CoreScripts may register handlers; the built-in controls otherwise talk to
// the native core UI through StarterGui's client-local settings.
int starterGuiRegisterCore(lua_State* L)
{
    Instance& starter = self(L);
    const std::string_view name = luaL_checkstring(L, 2);
    const std::string prefix = std::string_view(methodName(L)) == "RegisterSetCore" ? "SetCore:" : "GetCore:";
    setCallback(L, starter, prefix + std::string(name), 3);
    return 0;
}

int starterGuiSetCore(lua_State* L)
{
    auto& starter = static_cast<StarterGui&>(self(L));
    const std::string name = luaL_checkstring(L, 2);
    Runtime& rt = Runtime::of(L);
    if (const auto callback = rt.shared().callback(starter, "SetCore:" + name))
        return invokeAndWait(L, *callback, PackedArgs{pack(L, 3)});
    auto& core = starter.coreSettings();
    auto boolean = [&] {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        return lua_toboolean(L, 3) != 0;
    };
    if (name == "TopbarEnabled")
        core.topbarEnabled = boolean();
    else if (name == "ChatActive")
        core.chatActive = boolean();
    else if (name == "ChatBarDisabled")
        core.chatBarDisabled = boolean();
    else if (name == "DevConsoleVisible")
        core.devConsoleVisible = boolean();
    else if (name == "PointsNotificationsActive")
        core.pointsNotificationsActive = boolean();
    else if (name == "BadgesNotificationsActive")
        core.badgesNotificationsActive = boolean();
    else if (name == "AvatarContextMenuEnabled")
        core.avatarContextMenuEnabled = boolean();
    else if (name == "ChatWindowPosition")
        core.chatWindowPosition = check<UDim2>(L, 3);
    else if (name == "ChatWindowSize")
        core.chatWindowSize = check<UDim2>(L, 3);
    else if (name == "ResetButtonCallback") {
        if (lua_isboolean(L, 3)) {
            core.resetButtonCallback = boolean();
        } else {
            Instance& event = checkInstance(L, 3);
            if (!event.isA("BindableEvent"))
                raise(L, "ResetButtonCallback must be a boolean or BindableEvent");
            core.resetButtonCallback = event.shared_from_this();
        }
    } else if (name == "SendNotification") {
        luaL_checktype(L, 3, LUA_TTABLE);
        StarterGui::Notification notification;
        auto textField = [&](const char* key, bool required = false) {
            lua_getfield(L, 3, key);
            std::string value;
            if (required || !lua_isnil(L, -1))
                value = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            return value;
        };
        notification.title = textField("Title", true);
        notification.text = textField("Text", true);
        notification.icon = textField("Icon");
        lua_getfield(L, 3, "Duration");
        if (!lua_isnil(L, -1))
            notification.duration = std::clamp(luaL_checknumber(L, -1), 0.1, 3600.0);
        lua_pop(L, 1);
        for (const char* key : {"Button1", "Button2"}) {
            std::string label = textField(key);
            if (!label.empty())
                notification.buttons.push_back(std::move(label));
        }
        lua_getfield(L, 3, "Callback");
        if (!lua_isnil(L, -1)) {
            Instance& callback = checkInstance(L, -1);
            if (!callback.isA("BindableFunction"))
                raise(L, "Notification Callback must be a BindableFunction");
            notification.choose = [weak = std::weak_ptr<Shared>(rt.sharedPtr()),
                                   callback = callback.shared_from_this()](std::string label) {
                if (const auto shared = weak.lock())
                    if (const auto handler = shared->callback(*callback, "OnInvoke"))
                        invokeCallback(*handler, {packed(Value(std::move(label)))}, [](bool, PackedArgs) {});
            };
        }
        lua_pop(L, 1);
        starter.postNotification(std::move(notification));
    } else if (name == "ChatMakeSystemMessage") {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "Text");
        std::string text = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        Color3 color{1, 1, 1};
        lua_getfield(L, 3, "Color");
        if (!lua_isnil(L, -1))
            color = checkColor3(L, -1);
        lua_pop(L, 1);
        starter.postSystemChat({std::move(text), color});
    } else {
        raise(L, std::format("SetCore: {} has not been registered by the CoreScripts", name));
    }
    return 0;
}

int starterGuiGetCore(lua_State* L)
{
    auto& starter = static_cast<StarterGui&>(self(L));
    const std::string name = luaL_checkstring(L, 2);
    Runtime& rt = Runtime::of(L);
    if (const auto callback = rt.shared().callback(starter, "GetCore:" + name))
        return invokeAndWait(L, *callback, {});
    const auto& core = starter.coreSettings();
    if (name == "TopbarEnabled")
        lua_pushboolean(L, core.topbarEnabled);
    else if (name == "ChatActive")
        lua_pushboolean(L, core.chatActive);
    else if (name == "ChatBarDisabled")
        lua_pushboolean(L, core.chatBarDisabled);
    else if (name == "DevConsoleVisible")
        lua_pushboolean(L, core.devConsoleVisible);
    else if (name == "PointsNotificationsActive")
        lua_pushboolean(L, core.pointsNotificationsActive);
    else if (name == "BadgesNotificationsActive")
        lua_pushboolean(L, core.badgesNotificationsActive);
    else if (name == "AvatarContextMenuEnabled")
        lua_pushboolean(L, core.avatarContextMenuEnabled);
    else if (name == "ChatWindowPosition")
        push<UDim2>(L, core.chatWindowPosition.value_or(UDim2{}));
    else if (name == "ChatWindowSize")
        push<UDim2>(L, core.chatWindowSize.value_or(UDim2{{0.3f, 0}, {0.25f, 24}}));
    else if (name == "ResetButtonCallback")
        pushValue(L, core.resetButtonCallback);
    else
        raise(L, std::format("GetCore: {} has not been registered by the CoreScripts", name));
    return 1;
}

int chatInputFocus(lua_State* L)
{
    if (auto* config = instanceCast<ChatInputBarConfiguration>(&self(L)); config && config->enabled()) {
        auto* starter = Runtime::of(L).game().getService<StarterGui>();
        if (starter->coreGuiEnabled(Enum::CoreGuiType::Chat)) {
            starter->coreSettings().chatActive = true;
            config->requestFocus();
        }
    }
    return 0;
}

int guiObjectsAtPosition(lua_State* L)
{
    Instance& root = self(L);
    const auto& query = Runtime::of(L).shared().guiHitTest;
    const float x = static_cast<float>(luaL_checknumber(L, 2));
    const float y = static_cast<float>(luaL_checknumber(L, 3));
    const Vector2 inset = guiCoordinateOrigin(Runtime::of(L).game());
    const std::vector<InstancePtr> objects = query ? query(x + inset.x, y + inset.y) : std::vector<InstancePtr>{};
    lua_newtable(L);
    int index = 0;
    for (const InstancePtr& object : objects)
        if (object && root.isAncestorOf(object.get())) {
            pushInstance(L, object.get());
            lua_rawseti(L, -2, ++index);
        }
    return 1;
}

// --- Sound --------------------------------------------------------------------------
// Play/Stop/Pause/Resume set Playing (and Play and Stop TimePosition) and
// fire their events; a sound system (lunar::audio::SoundSystem), where
// there is one, plays what that says and fires Ended, DidLoop and Loaded.

template <bool Playing, const char* Event>
int soundCommand(lua_State* L)
{
    auto* sound = dynamic_cast<Sound*>(&self(L));
    if (!sound)
        return 0;
    // research: LUNAR_SOUND_TRACE (lunar::audio::SoundSystem) says who played or stopped it.
    if (static const bool tracing = std::getenv("LUNAR_SOUND_TRACE") != nullptr; tracing) {
        luaL_where(L, 1);
        std::println(stderr, "soundtrace: cause script-{} {} {}", Event, static_cast<const void*>(sound),
                     lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    if (std::string_view(Event) == "Played" || std::string_view(Event) == "Stopped")
        sound->setTimePosition(0.0);
    sound->setPlaying(Playing);
    Runtime& rt = Runtime::of(L);
    rt.shared().events.fire(*sound, Event, PackedArgs{packed(Value(sound->soundId()))});
    return 0;
}

constexpr char played[] = "Played";
constexpr char stopped[] = "Stopped";
constexpr char paused[] = "Paused";
constexpr char resumed[] = "Resumed";

// SoundService:PlayLocalSound(sound): plays it from the start on this client
// alone. Lunarstone's audio is the client's own anyway, so it plays the way
// Sound:Play does. GUESS: that Played doesn't fire (the documentation only
// says the sound is heard locally); a sound that isn't a Sound does nothing.
int playLocalSound(lua_State* L)
{
    self(L);
    if (auto* sound = dynamic_cast<Sound*>(toInstance(L, 2))) {
        sound->setTimePosition(0.0);
        sound->setPlaying(true);
    }
    return 0;
}

// --- Camera projection ---------------------------------------------------------------
// Pinhole projection through CFrame and the vertical FieldOfView onto
// ViewportSize. ScreenPoint variants use GuiService's current inset, just
// like GuiObject.AbsolutePosition and GetGuiObjectsAtPosition.

template <bool Screen>
int cameraWorldToPoint(lua_State* L)
{
    auto* camera = dynamic_cast<Camera*>(&self(L));
    const Vector3 world = checkVector3(L, 2);
    if (!camera) {
        pushVector3(L, {});
        lua_pushboolean(L, false);
        return 2;
    }
    const Vector3 p = camera->cframe().pointToObjectSpace(world);
    const Vector2 size = camera->viewportSize();
    const float depth = -p.z;
    const float tanHalf = std::tan(camera->fieldOfView() * std::numbers::pi_v<float> / 360.0f);
    const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
    const float safeDepth = std::fabs(depth) < 1e-6f ? 1e-6f : depth;
    float x = (p.x / (safeDepth * tanHalf * aspect) + 1.0f) * 0.5f * size.x;
    float y = (1.0f - p.y / (safeDepth * tanHalf)) * 0.5f * size.y;
    const bool visible = depth > 0.0f && x >= 0.0f && x <= size.x && y >= 0.0f && y <= size.y;
    if constexpr (Screen) {
        const Vector2 inset = guiCoordinateOrigin(Runtime::of(L).game());
        x -= inset.x;
        y -= inset.y;
    }
    pushVector3(L, {x, y, depth});
    lua_pushboolean(L, visible);
    return 2;
}

// Camera:GetLargestCutoffDistance(ignoreList): how far the camera has to move
// toward its Focus so that no part (outside the ignore list) sits between them;
// 0 when the view is clear. A ray from the Focus to the camera finds the
// closest obstruction.
int cameraLargestCutoff(lua_State* L)
{
    auto* camera = dynamic_cast<Camera*>(&self(L));
    if (!camera) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    RaycastQuery query;
    if (lua_istable(L, 2))
        for (int i = 1;; ++i) {
            lua_rawgeti(L, 2, i);
            Instance* ignored = toInstance(L, -1);
            const bool done = lua_isnil(L, -1);
            lua_pop(L, 1);
            if (done)
                break;
            if (ignored)
                query.filter.push_back(ignored->shared_from_this());
        }
    query.origin = camera->focus().position;
    const Vector3 toCamera = camera->cframe().position - query.origin;
    const float length = toCamera.magnitude();
    if (length < 1e-4f) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    query.direction = toCamera;
    query.respectCanCollide = false;
    const auto hit = castRay(L, query);
    lua_pushnumber(L, hit ? static_cast<double>(std::max(length - hit->distance, 0.0f)) : 0.0);
    return 1;
}

// The unit direction from the camera through a point of its viewport, in pixels.
Vector3 viewportRayDirection(const Camera& camera, float x, float y)
{
    const Vector2 size = camera.viewportSize();
    const float tanHalf = std::tan(camera.fieldOfView() * std::numbers::pi_v<float> / 360.0f);
    const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
    const Vector3 local{(x / size.x * 2.0f - 1.0f) * tanHalf * aspect, (1.0f - y / size.y * 2.0f) * tanHalf, -1.0f};
    return camera.cframe().vectorToWorldSpace(local.unit());
}

template <bool Screen>
int cameraPointToRay(lua_State* L)
{
    auto* camera = dynamic_cast<Camera*>(&self(L));
    auto x = static_cast<float>(luaL_checknumber(L, 2));
    auto y = static_cast<float>(luaL_checknumber(L, 3));
    const auto depth = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    if (!camera) {
        push<Ray>(L, Ray{});
        return 1;
    }
    if constexpr (Screen) {
        const Vector2 inset = guiCoordinateOrigin(Runtime::of(L).game());
        x += inset.x;
        y += inset.y;
    }
    const Vector3 direction = viewportRayDirection(*camera, x, y);
    push<Ray>(L, Ray{camera->cframe().position + direction * depth, direction});
    return 1;
}

// Stats:GetTotalMemoryUsageMb(): this process's resident memory.
int statsTotalMemory(lua_State* L)
{
    double megabytes = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long size = 0, resident = 0;
        if (std::fscanf(f, "%lu %lu", &size, &resident) == 2)
            megabytes = static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        std::fclose(f);
    }
    lua_pushnumber(L, megabytes);
    return 1;
}

// StatsItem:GetValue() / GetValueString(): what the item measures. Here only
// "Data Ping" measures anything: the round trip to the server in
// milliseconds, as the connection keeps it; the rest answer 0. GUESS: the
// string's form, the number then its unit.
double statsItemValue(lua_State* L, const Instance& item)
{
    const Shared& shared = Runtime::of(L).shared();
    if (item.name() == "Data Ping" && shared.networkPing)
        return shared.networkPing() * 1000.0;
    return 0.0;
}

int statsItemGetValue(lua_State* L)
{
    lua_pushnumber(L, statsItemValue(L, self(L)));
    return 1;
}

int statsItemGetValueString(lua_State* L)
{
    const Instance& item = self(L);
    const double value = statsItemValue(L, item);
    const std::string text
        = item.name() == "Data Ping" ? std::format("{:.2f} ms", value) : std::format("{:.2f}", value);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// --- UIPageLayout ------------------------------------------------------------------
// JumpTo a page (one of the layout's; anything else is ignored), JumpToIndex
// from 0, Next and Previous (round the ends when Circular): moved by
// jumpToPage, which fires the events and starts the slide.

int pageJumpTo(lua_State* L)
{
    auto* layout = dynamic_cast<UIPageLayout*>(&self(L));
    auto* page = dynamic_cast<GuiObject*>(&checkInstance(L, 2));
    if (layout && page && std::ranges::contains(layout->pages(), page))
        jumpToPage(Runtime::of(L).shared(), *layout, *page);
    return 0;
}

int pageJumpToIndex(lua_State* L)
{
    auto* layout = dynamic_cast<UIPageLayout*>(&self(L));
    const auto index = static_cast<std::ptrdiff_t>(luaL_checkinteger(L, 2));
    if (!layout)
        return 0;
    const std::vector<GuiObject*> pages = layout->pages();
    if (index >= 0 && index < std::ssize(pages))
        jumpToPage(Runtime::of(L).shared(), *layout, *pages[static_cast<std::size_t>(index)]);
    return 0;
}

template <int Step>
int pageStep(lua_State* L)
{
    auto* layout = dynamic_cast<UIPageLayout*>(&self(L));
    if (!layout)
        return 0;
    const std::vector<GuiObject*> pages = layout->pages();
    const auto count = std::ssize(pages);
    if (count == 0)
        return 0;
    auto next = (std::ranges::find(pages, layout->currentPage()) - pages.begin()) + Step;
    if (next < 0 || next >= count) {
        if (!layout->circular())
            return 0;
        next = (next + count) % count;
    }
    jumpToPage(Runtime::of(L).shared(), *layout, *pages[static_cast<std::size_t>(next)]);
    return 0;
}

// --- Thumbnails and products ---------------------------------------------------------

// Players:GetUserThumbnailAsync(userId, type, size): the rbxthumb:// content
// for the picture, ready at once, as the client hands it to an ImageLabel.
// (Lunarstone doesn't load rbxthumb images yet: they draw as nothing.)
int playersGetUserThumbnailAsync(lua_State* L)
{
    self(L);
    const auto userId = static_cast<std::int64_t>(luaL_checknumber(L, 2));
    const auto type = toEnumItem(L, 3, "ThumbnailType");
    const auto size = toEnumItem(L, 4, "ThumbnailSize");
    std::string_view kind = "AvatarHeadShot";
    if (type && type->name == "AvatarBust")
        kind = "AvatarBust";
    else if (type && type->name == "AvatarThumbnail")
        kind = "Avatar";
    // Size420x420 and the like.
    int width = 420;
    int height = 420;
    auto number = [](std::string_view digits) {
        int value = 0;
        for (const char c : digits)
            value = c >= '0' && c <= '9' ? value * 10 + (c - '0') : value;
        return value;
    };
    if (size && size->name.starts_with("Size")) {
        const std::string_view dims = size->name.substr(4);
        if (const auto x = dims.find('x'); x != std::string_view::npos) {
            width = number(dims.substr(0, x));
            height = number(dims.substr(x + 1));
        }
    }
    pushString(L, std::format("rbxthumb://type={}&id={}&w={}&h={}", kind, userId, width, height));
    lua_pushboolean(L, 1);
    return 2;
}

// MarketplaceService:GetProductInfo(id[, infoType]): what Lunarstone can
// tell of a product it doesn't look up, every field the client's table has,
// none for sale. GUESS: the shape of an empty answer (the fields are the API's).
// A client's own teleport: to the place, or to one server of it
// (TeleportToPlaceInstance), the host's to make (ScriptContext::setLaunchHandler).
template <bool Instance>
int teleportServiceTeleport(lua_State* L)
{
    self(L);
    const double place = luaL_checknumber(L, 2);
    ScriptContext::LaunchRequest request;
    request.placeId = place > 0.0 && place < 9.2e18 ? static_cast<std::uint64_t>(place) : 0;
    if constexpr (Instance)
        request.gameInstanceId = luaL_checkstring(L, 3);
    Runtime& rt = Runtime::of(L);
    if (!rt.shared().launchHandler)
        luaL_error(L, "Teleport is not available here");
    rt.shared().launchHandler(request);
    return 0;
}

int marketplaceGetProductInfo(lua_State* L)
{
    self(L);
    const double id = luaL_checknumber(L, 2);
    lua_createtable(L, 0, 24);
    auto text = [&](const char* key, std::string_view value) {
        pushString(L, value);
        lua_setfield(L, -2, key);
    };
    auto number = [&](const char* key, double value) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    };
    auto flag = [&](const char* key, bool value) {
        lua_pushboolean(L, value ? 1 : 0);
        lua_setfield(L, -2, key);
    };
    text("Name", "");
    text("Description", "");
    text("Created", "");
    text("Updated", "");
    number("AssetId", id);
    number("ProductId", id);
    number("TargetId", id);
    number("AssetTypeId", 0);
    number("IconImageAssetId", 0);
    number("PriceInRobux", 0);
    number("Remaining", 0);
    number("Sales", 0);
    number("MinimumMembershipLevel", 0);
    number("ContentRatingTypeId", 0);
    flag("IsForSale", false);
    flag("IsLimited", false);
    flag("IsLimitedUnique", false);
    flag("IsNew", false);
    flag("IsPublicDomain", false);
    lua_createtable(L, 0, 5);
    text("Name", "");
    text("CreatorType", "User");
    number("Id", 0);
    number("CreatorTargetId", 0);
    flag("HasVerifiedBadge", false);
    lua_setfield(L, -2, "Creator");
    return 1;
}

// --- The method table --------------------------------------------------------------

constexpr auto methods = std::to_array<Method>({
    {"GetService", "UserSettings", settingsGetService},
    {"GetService", "GlobalSettings", settingsGetService},
    {"SetCore", "StarterGui", starterGuiSetCore},
    {"GetEmotesMenuOpen", "GuiService", falseMethod},
    {"SetEmotesMenuOpen", "GuiService", noopMethod},
    {"GetCameraYInvertValue", "UserGameSettings", cameraYInvertValue},
    // The settings menu's switches the PlayerModule shows or hides.
    {"SetCameraYInvertVisible", "UserGameSettings", noopMethod},
    {"SetGamepadCameraSensitivityVisible", "UserGameSettings", noopMethod},
    {"GetTutorialState", "UserGameSettings", falseMethod},
    {"SetTutorialState", "UserGameSettings", noopMethod},
    {"InFullScreen", "UserGameSettings", falseMethod},
    {"InStudioMode", "UserGameSettings", falseMethod},
    {"IsUserFeatureEnabled", "UserGameSettings", falseMethod},
    {"SetNetworkOwner", "BasePart", noopMethod},
    {"SetNetworkOwnershipAuto", "BasePart", noopMethod},
    {"GetNetworkOwner", "BasePart", nilMethod},
    {"GetNetworkOwnershipAuto", "BasePart", trueMethod},
    {"CanSetNetworkOwnership", "BasePart", canSetNetworkOwnership},
    {"GetMaterialColor", "Terrain", terrainGetMaterialColor},
    {"SetMaterialColor", "Terrain", terrainSetMaterialColor},
    {"WorldToViewportPoint", "Camera", cameraWorldToPoint<false>},
    {"WorldToScreenPoint", "Camera", cameraWorldToPoint<true>},
    {"ViewportPointToRay", "Camera", cameraPointToRay<false>},
    {"ScreenPointToRay", "Camera", cameraPointToRay<true>},
    {"GetLargestCutoffDistance", "Camera", cameraLargestCutoff},
    {"JumpTo", "UIPageLayout", pageJumpTo},
    {"JumpToIndex", "UIPageLayout", pageJumpToIndex},
    {"Next", "UIPageLayout", pageStep<1>},
    {"Previous", "UIPageLayout", pageStep<-1>},
    // The legacy bubble chat's look; the bubbles are TextChatService's.
    {"SetBubbleChatSettings", "Chat", noopMethod},
    {"GetUserThumbnailAsync", "Players", playersGetUserThumbnailAsync},
    {"GetProductInfo", "MarketplaceService", marketplaceGetProductInfo},
    // The loading screen a teleport carried over: none (Lunarstone's teleports carry no GUI).
    {"GetArrivingTeleportGui", "TeleportService", nilMethod},
    {"Teleport", "TeleportService", teleportServiceTeleport<false>},
    {"TeleportToPlaceInstance", "TeleportService", teleportServiceTeleport<true>},
    // Returns at once [live: World Tower Defense's ReplicatedFirst loader
    // calls it first, and died there with its own "Loading..." screen still up].
    {"RemoveDefaultLoadingScreen", "ReplicatedFirst", replicatedFirstRemoveDefaultLoadingScreen},
    {"IsVibrationSupported", "HapticService", falseMethod},
    {"IsMotorSupported", "HapticService", falseMethod},
    {"SetMotor", "HapticService", noopMethod},
    {"GetMotor", "HapticService", hapticGetMotor},
    {"Play", "Sound", soundCommand<true, played>},
    {"PlayLocalSound", "SoundService", playLocalSound},
    {"Stop", "Sound", soundCommand<false, stopped>},
    {"Pause", "Sound", soundCommand<false, paused>},
    {"Resume", "Sound", soundCommand<true, resumed>},
    {"IsUserFeatureEnabled", "UserSettings", constantBool<false>},
    {"IsUserFeatureEnabled", "GlobalSettings", constantBool<false>},
    {"BindToClose", "DataModel", noopMethod},
    {"PreloadAsync", "ContentProvider", contentProviderPreloadAsync},
    {"GetAssetFetchStatus", "ContentProvider", contentProviderGetAssetFetchStatus},
    {"Preload", "ContentProvider", noopMethod},
    {"GetCore", "StarterGui", starterGuiGetCore},
    {"FocusTextBox", "ChatInputBarConfiguration", chatInputFocus},
    {"GetGuiObjectsAtPosition", "BasePlayerGui", guiObjectsAtPosition},
    {"RegisterGetCore", "StarterGui", starterGuiRegisterCore},
    {"RegisterSetCore", "StarterGui", starterGuiRegisterCore},
    {"SetTopbarTransparency", "PlayerGui", noopMethod},
    {"GetTopbarTransparency", "PlayerGui", zero},
    {"SetCoreGuiEnabled", "PlayerGui", noopMethod},
    // GUESS: offline, nobody owns anything that's for sale; the real calls
    // ask the website (and yield).
    {"PlayerOwnsAsset", "MarketplaceService", falseMethod},
    {"PlayerOwnsAssetAsync", "MarketplaceService", falseMethod},
    {"PlayerOwnsBundle", "MarketplaceService", falseMethod},
    {"PlayerOwnsBundleAsync", "MarketplaceService", falseMethod},
    {"UserOwnsGamePassAsync", "MarketplaceService", falseMethod},
    // GUESS: the purchase prompts are the client's own GUI, which Lunarstone
    // doesn't draw; nothing is bought, so nothing is finished either.
    {"PromptPurchase", "MarketplaceService", noopMethod},
    {"PromptGamePassPurchase", "MarketplaceService", noopMethod},
    {"PromptProductPurchase", "MarketplaceService", noopMethod},
    {"IsClient", "RunService", runIsClient},
    {"IsServer", "RunService", runIsServer},
    {"IsStudio", "RunService", constantBool<false>},
    // Not a console (the 10-foot interface), and not arriving from a teleport.
    {"IsTenFootInterface", "GuiService", constantBool<false>},
    // Zstd and Base64 on buffers (JJS ships its effect data compressed).
    {"Base64Encode", "EncodingService", encodingBase64Encode},
    {"Base64Decode", "EncodingService", encodingBase64Decode},
    {"CompressBuffer", "EncodingService", encodingCompressBuffer},
    {"DecompressBuffer", "EncodingService", encodingDecompressBuffer},
    {"GetDecompressedBufferSize", "EncodingService", encodingGetDecompressedBufferSize},
    {"GetGuiInset", "GuiService", guiGetGuiInset},
    // PlayerScripts' movement and camera modes are what the settings menu
    // offers; the PlayerModule registers its own, which Lunarstone's menu
    // doesn't list yet.
    {"RegisterComputerCameraMovementMode", "PlayerScripts", noopMethod},
    {"RegisterComputerMovementMode", "PlayerScripts", noopMethod},
    {"RegisterTouchCameraMovementMode", "PlayerScripts", noopMethod},
    {"RegisterTouchMovementMode", "PlayerScripts", noopMethod},
    {"ClearComputerCameraMovementModes", "PlayerScripts", noopMethod},
    {"ClearComputerMovementModes", "PlayerScripts", noopMethod},
    {"ClearTouchCameraMovementModes", "PlayerScripts", noopMethod},
    {"ClearTouchMovementModes", "PlayerScripts", noopMethod},
    {"GetRegisteredComputerCameraMovementModes", "PlayerScripts", emptyTable},
    {"GetRegisteredComputerMovementModes", "PlayerScripts", emptyTable},
    {"GetRegisteredTouchCameraMovementModes", "PlayerScripts", emptyTable},
    {"GetRegisteredTouchMovementModes", "PlayerScripts", emptyTable},
    {"IsWindows", "GuiService", constantBool<false>}, // Lunarstone runs on Linux
    {"GetLocalPlayerTeleportData", "TeleportService", nilMethod},
    {"SetTeleportGui", "TeleportService", noopMethod}, // shown while teleporting, which we don't yet
    {"GetTotalMemoryUsageMb", "Stats", statsTotalMemory},
    {"GetValue", "StatsItem", statsItemGetValue},
    {"GetValueString", "StatsItem", statsItemGetValueString},
    {"IsEdit", "RunService", constantBool<false>},
    {"IsRunMode", "RunService", constantBool<false>},
    {"IsRunning", "RunService", constantBool<true>},
    {"BindToRenderStep", "RunService", bindToRenderStep},
    {"UnbindFromRenderStep", "RunService", unbindFromRenderStep},
    {"Set3dRenderingEnabled", "RunService", noopMethod},
    {"GetPlayers", "Players", getPlayers},
    {"getPlayers", "Players", getPlayers},
    {"players", "Players", getPlayers},
    {"GetPlayerFromCharacter", "Players", getPlayerFromCharacter},
    {"getPlayerFromCharacter", "Players", getPlayerFromCharacter},
    {"playerFromCharacter", "Players", getPlayerFromCharacter},
    {"GetPlayerByUserId", "Players", getPlayerByUserId},
    {"GetHumanoidDescriptionFromUserId", "Players", playersGetHumanoidDescriptionFromUserId},
    {"GetHumanoidDescriptionFromUserIdAsync", "Players", playersGetHumanoidDescriptionFromUserId},
    {"GetCharacterAppearanceInfoAsync", "Players", playersGetCharacterAppearanceInfoAsync},
    {"GetUserIdFromNameAsync", "Players", playersGetUserIdFromNameAsync},
    {"GetNameFromUserIdAsync", "Players", playersGetNameFromUserIdAsync},
    {"LoadCharacter", "Player", playerLoadCharacter},
    {"LoadCharacterAsync", "Player", playerLoadCharacter},
    {"Kick", "Player", playerKick},
    {"HasAppearanceLoaded", "Player", constantBool<true>},
    {"DistanceFromCharacter", "Player", playerDistanceFromCharacter},
    {"RequestStreamAroundAsync", "Player", playerRequestStreamAroundAsync},
    {"GetMouse", "Player", playerGetMouse},
    {"GetRankInGroup", "Player", zero},
    {"GetRankInGroupAsync", "Player", zero},
    {"GetRoleInGroup", "Player", emptyString},
    {"GetRoleInGroupAsync", "Player", emptyString},
    {"IsInGroup", "Player", constantBool<false>},
    {"IsInGroupAsync", "Player", constantBool<false>},
    {"IsFriendsWith", "Player", constantBool<false>},
    {"IsFriendsWithAsync", "Player", constantBool<false>},
    {"GetNetworkPing", "Player", zero},
    {"GetJoinData", "Player", emptyTable},
    {"SetSuperSafeChat", "Player", noopMethod},
    {"AddItem", "Debris", debrisAddItem},
    {"addItem", "Debris", debrisAddItem},
    {"JSONEncode", "HttpService", jsonEncode},
    {"JSONDecode", "HttpService", jsonDecode},
    {"GenerateGUID", "HttpService", generateGuid},
    {"UrlEncode", "HttpService", urlEncode},
    {"GetAsync", "HttpService", httpRequest},
    {"PostAsync", "HttpService", httpRequest},
    {"RequestAsync", "HttpService", httpRequest},
    {"AddTag", "CollectionService", collectionAddTag},
    {"RemoveTag", "CollectionService", collectionRemoveTag},
    {"HasTag", "CollectionService", collectionHasTag},
    {"GetTags", "CollectionService", collectionGetTags},
    {"GetTagged", "CollectionService", collectionGetTagged},
    {"GetCollection", "CollectionService", collectionGetTagged},
    {"GetAllTags", "CollectionService", collectionGetAllTags},
    {"GetInstanceAddedSignal", "CollectionService", collectionInstanceSignal<true>},
    {"GetInstanceRemovedSignal", "CollectionService", collectionInstanceSignal<false>},
    {"Create", "TweenService", tweenServiceCreate},
    {"TweenSize", "GuiObject", guiTweenSize},
    {"TweenPosition", "GuiObject", guiTweenPosition},
    {"TweenSizeAndPosition", "GuiObject", guiTweenSizeAndPosition},
    {"GetValue", "TweenService", tweenServiceGetValue},
    {"Play", "Tween", tweenCommand<TweenCommand::Play>},
    {"Pause", "Tween", tweenCommand<TweenCommand::Pause>},
    {"Cancel", "Tween", tweenCommand<TweenCommand::Cancel>},
    {"IsKeyDown", "UserInputService", uisIsKeyDown},
    {"IsMouseButtonPressed", "UserInputService", uisIsMouseButtonPressed},
    {"GetMouseLocation", "UserInputService", uisGetMouseLocation},
    {"GetMouseDelta", "UserInputService", uisGetMouseDelta},
    {"GetLastInputType", "UserInputService", uisGetLastInputType},
    {"GetKeysPressed", "UserInputService", uisGetKeysPressed},
    {"GetStringForKeyCode", "UserInputService", uisGetStringForKeyCode},
    {"GetGamepadConnected", "UserInputService", uisFalse},
    {"GetConnectedGamepads", "UserInputService", emptyTable},
    {"GetNavigationGamepads", "UserInputService", emptyTable},
    {"GetSupportedGamepadKeyCodes", "UserInputService", emptyTable},
    {"GetMouseButtonsPressed", "UserInputService", emptyTable},
    {"GetFocusedTextBox", "UserInputService", uisGetFocusedTextBox},
    {"CaptureFocus", "TextBox", textBoxFocus},
    {"ReleaseFocus", "TextBox", textBoxFocus},
    {"GetPlatform", "UserInputService", uisGetPlatform},
    {"BindAction", "ContextActionService", casBind},
    {"BindActionAtPriority", "ContextActionService", casBindAtPriority},
    {"UnbindAction", "ContextActionService", casUnbind},
    // The client's own bindings (the Lua app's, the core GUI's): the same
    // actions, at the core's level of the stack.
    {"BindCoreAction", "ContextActionService", casBind},
    {"BindCoreActionAtPriority", "ContextActionService", casBindAtPriority},
    {"UnbindCoreAction", "ContextActionService", casUnbind},
    {"UnbindAllActions", "ContextActionService", casUnbindAll},
    {"GetButton", "ContextActionService", casGetButton},
    {"SetTitle", "ContextActionService", noopMethod},
    {"SetImage", "ContextActionService", noopMethod},
    {"SetDescription", "ContextActionService", noopMethod},
    {"SetPosition", "ContextActionService", noopMethod},
    {"GetTextSize", "TextService", textGetTextSize},
    {"RegisterCollisionGroup", "PhysicsService", physicsRegisterGroup},
    {"CreateCollisionGroup", "PhysicsService", physicsRegisterGroup},
    {"CollisionGroupSetCollidable", "PhysicsService", physicsSetCollidable},
    {"CollisionGroupsAreCollidable", "PhysicsService", physicsCollidable},
    {"GetCollisionGroupId", "PhysicsService", physicsGetGroupId},
    {"SetPartCollisionGroup", "PhysicsService", physicsSetPartGroup},
    {"IsCollisionGroupRegistered", "PhysicsService", physicsIsRegistered},
    {"Raycast", "WorldRoot", workspaceRaycast},
    {"FindPartOnRay", "WorldRoot", workspaceFindPartOnRay<false, false>},
    {"findPartOnRay", "WorldRoot", workspaceFindPartOnRay<false, false>},
    {"FindPartOnRayWithIgnoreList", "WorldRoot", workspaceFindPartOnRay<true, false>},
    {"FindPartOnRayWithWhitelist", "WorldRoot", workspaceFindPartOnRay<true, true>},
    {"GetPartBoundsInBox", "WorldRoot", emptyTable},
    {"GetPartBoundsInRadius", "WorldRoot", emptyTable},
    {"GetPartsInPart", "WorldRoot", emptyTable},
    {"FindPartsInRegion3", "WorldRoot", emptyTable},
    {"FindPartsInRegion3WithIgnoreList", "WorldRoot", emptyTable},
    {"FindPartsInRegion3WithWhiteList", "WorldRoot", emptyTable},
    {"GetServerTimeNow", "Workspace", workspaceServerTimeNow},
    {"GetRealPhysicsFPS", "Workspace", sixty},
    {"GetLogHistory", "LogService", logGetHistory},
    {"Fire", "BindableEvent", bindableFire},
    {"BindToMessage", "Actor", actorBindToMessage},
    {"BindToMessageParallel", "Actor", actorBindToMessage},
    {"SendMessage", "Actor", actorSendMessage},
    {"GetSharedTable", "SharedTableRegistry", sharedTableRegistryGet},
    {"SetSharedTable", "SharedTableRegistry", sharedTableRegistrySet},
    {"Invoke", "BindableFunction", bindableInvoke},
    {"FireServer", "BaseRemoteEvent", remoteFireServer},
    {"FireClient", "BaseRemoteEvent", remoteFireClient},
    {"FireAllClients", "BaseRemoteEvent", remoteFireAllClients},
    {"InvokeServer", "RemoteFunction", remoteInvokeServer},
    {"InvokeClient", "RemoteFunction", remoteInvokeClient},
    {"TakeDamage", "Humanoid", humanoidTakeDamage},
});

} // namespace

std::span<const Method> serviceMethods()
{
    return methods;
}

bool receiveNetworkRemote(Shared& shared, Instance& remote, std::string_view event,
                          std::span<const RemoteValue> arguments)
{
    if (remote.isA("TextChatService"))
        return receiveTextChat(shared, remote, event, arguments);
    if (event == "OnClientEvent" && remote.isA("BaseRemoteEvent")) {
        // A remote nobody listens to keeps its events up to a limit, then
        // drops them with the client's warning (0x106ac4141). GUESS: the
        // limit, an FInt whose default wasn't found.
        constexpr std::ptrdiff_t queueLimit = 256;
        const auto queued = std::ranges::count_if(shared.inbox, [&remote](const Shared::Message& m) {
            return m.event == "OnClientEvent" && m.remote.lock().get() == &remote;
        });
        if (queued >= queueLimit) {
            const std::size_t n = ++networkInvokes(shared).dropped[&remote];
            if (Runtime* client = shared.runtimeFor(ScriptRole::Client); client && (n == 1 || n % 256 == 0))
                client->output(
                    MessageType::Warning,
                    std::format(
                        "Remote event invocation queue exhausted for {}; did " "you " "forget " "to " "implement" " " "OnCli" "entEv" "ent? " "({} " "events " "dropped)",
                        remote.fullName(), n));
            return true;
        }
        shared.inbox.push_back(Shared::Message{remote.weak_from_this(), "OnClientEvent", packedArgs(arguments)});
        return true;
    }
    if (!remote.isA("RemoteFunction") || arguments.empty())
        return false;
    const double* number = std::get_if<double>(&arguments[0].value);
    const std::uint32_t id = number ? static_cast<std::uint32_t>(*number) : 0;
    if (event == "RemoteOnInvokeClient") {
        shared.invocations.emplace_back(
            [&shared, remote = remote.shared_from_this(), id, args = packedArgs(arguments.subspan(1))]() mutable {
                attemptClientInvoke(shared, remote, id, std::move(args));
            });
        return true;
    }
    if (event != "RemoteOnInvokeSuccess" && event != "RemoteOnInvokeError")
        return false;
    NetworkInvokes& invokes = networkInvokes(shared);
    auto it = invokes.waiting.find({&remote, id});
    if (it == invokes.waiting.end() || it->second.remote.lock().get() != &remote)
        return true; // an answer nobody waits for (the caller died, or a stale id)
    NetworkInvokes::Waiting w = it->second;
    invokes.waiting.erase(it);
    const bool ok = event == "RemoteOnInvokeSuccess";
    PackedArgs results = packedArgs(arguments.subspan(1));
    if (!ok && results.empty())
        results.push_back(packed(Value(std::string("error"))));
    if (w.nativeReply) {
        shared.invocations.emplace_back([done = std::move(w.nativeReply), ok, results = std::move(results)]() mutable { done(ok, std::move(results)); });
    } else {
        w.caller->defer([w, ok, results = std::move(results)] { finishInvoke(*w.caller, w.thread, w.token, ok, results); });
    }
    return true;
}

void invokeServerForCore(Runtime& caller, Instance& remote, PackedArgs args, NativeRemoteReply done)
{
    if (!done)
        return;
    Shared& shared = caller.shared();
    if (!shared.remoteSink) {
        auto callback = shared.callback(remote, "OnServerInvoke");
        if (!callback) {
            done(false, {packed(Value(std::string("The server has no whisper handler")))});
            return;
        }
        args.insert(args.begin(), packed(instanceValue(localPlayer(caller.game()))));
        invokeCallback(*callback, args,
                       [done = std::move(done)](bool ok, PackedArgs results) mutable { done(ok, std::move(results)); });
        return;
    }
    auto& invokes = networkInvokes(shared);
    const std::uint32_t id = invokes.nextId(remote);
    NetworkInvokes::Waiting waiting;
    waiting.remote = remote.weak_from_this();
    waiting.nativeReply = std::move(done);
    invokes.waiting[{&remote, id}] = std::move(waiting);
    caller.after(15.0, [weak = std::weak_ptr<NetworkInvokes>(shared.networkInvokes), owner = remote.weak_from_this(),
                        key = std::pair{&remote, id}] {
        const auto requests = weak.lock();
        if (!requests)
            return;
        auto it = requests->waiting.find(key);
        if (it == requests->waiting.end() || it->second.remote.lock() != owner.lock())
            return;
        auto reply = std::move(it->second.nativeReply);
        requests->waiting.erase(it);
        if (reply)
            reply(false, {packed(Value(std::string("Whisper channel request timed out")))});
    });
    std::vector<RemoteValue> out{remoteValue(static_cast<double>(id)),
                                 remoteValue(instanceValue(localPlayer(caller.game())))};
    for (const auto& arg : args)
        out.push_back(toRemote(arg));
    shared.remoteSink(remote, "RemoteOnInvokeServer", std::move(out));
}

void addTagAndNotify(lua_State* L, Instance& instance, std::string_view tag)
{
    if (instance.hasTag(tag))
        return;
    instance.addTag(tag);
    Runtime& rt = Runtime::of(L);
    if (inGame(instance, rt.game()))
        indexTag(*rt.shared().state, instance, tag);
    if (Instance* service = collectionService(rt.game()); service && inGame(instance, rt.game()))
        rt.shared().events.fire(*service, std::format("{}{}", addedPrefix, tag),
                                PackedArgs{packed(instanceValue(&instance))});
}

void removeTagAndNotify(lua_State* L, Instance& instance, std::string_view tag)
{
    if (!instance.hasTag(tag))
        return;
    instance.removeTag(tag);
    Runtime& rt = Runtime::of(L);
    unindexTag(*rt.shared().state, instance, tag);
    if (Instance* service = collectionService(rt.game()); service && inGame(instance, rt.game()))
        rt.shared().events.fire(*service, std::format("{}{}", removedPrefix, tag),
                                PackedArgs{packed(instanceValue(&instance))});
}

Instance& legacyMouse(Instance& player)
{
    for (const InstancePtr& child : player.children())
        if (child->className() == "PlayerMouse" && !child->archivable())
            return *child;
    auto mouse = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("PlayerMouse"));
    mouse->setName("Mouse");
    mouse->setArchivable(false);
    // Hidden from scripts in Roblox; here it lives under the player so it stays alive.
    mouse->setParent(&player);
    return *mouse;
}

bool pushMouseRay(lua_State* L, Instance& mouse, std::string_view member)
{
    static constexpr std::array<std::string_view, 5> members{"Hit", "Origin", "UnitRay", "Target", "TargetSurface"};
    if (std::ranges::find(members, member) == members.end())
        return false;
    Runtime& rt = Runtime::of(L);
    Workspace* workspace = rt.game().workspace();
    Camera* camera = workspace ? workspace->currentCamera() : nullptr;
    if (!camera)
        return false;
    // The pointer, or the middle of the view while the mouse is locked there.
    Vector2 point = rt.shared().state->input.mousePosition;
    if (const auto* input = dynamic_cast<const UserInputService*>(rt.game().findService("UserInputService"));
        input && input->mouseBehavior() == Enum::MouseBehavior::LockCenter)
        point = camera->viewportSize() * 0.5f;
    const Vector3 origin = camera->cframe().position;
    const Vector3 direction = viewportRayDirection(*camera, point.x, point.y);
    if (member == "UnitRay") {
        push<Ray>(L, Ray{origin, direction});
        return true;
    }
    // No roll: the camera's is left out.
    const CFrame aim = lookAtCFrame(origin, origin + direction, {0.0f, 1.0f, 0.0f});
    if (member == "Origin") {
        pushCFrame(L, aim);
        return true;
    }
    constexpr float reach = 1000.0f;
    RaycastQuery query{origin, direction * reach};
    if (const auto* player = dynamic_cast<const Player*>(mouse.parent()); player && player->character())
        query.filter.push_back(player->character()->shared_from_this());
    auto& extra = rt.shared().state->extraProperties;
    if (auto it = extra.find(&mouse); it != extra.end() && it->second.first.lock().get() == &mouse)
        if (auto filter = it->second.second.find("TargetFilter"); filter != it->second.second.end()) {
            unpack(L, filter->second);
            if (Instance* ignored = toInstance(L, -1))
                query.filter.push_back(ignored->shared_from_this());
            lua_pop(L, 1);
        }
    const auto hit = castRay(L, query);
    if (member == "Hit") {
        CFrame at = aim;
        at.position = hit ? hit->position : origin + direction * reach;
        pushCFrame(L, at);
    } else if (member == "Target") {
        pushInstance(L, hit ? hit->instance.get() : nullptr);
    } else {
        Enum::NormalId surface = Enum::NormalId::Right;
        if (hit) {
            const auto* part = dynamic_cast<const BasePart*>(hit->instance.get());
            const Vector3 n = part ? part->cframe().vectorToObjectSpace(hit->normal) : hit->normal;
            const Vector3 a{std::abs(n.x), std::abs(n.y), std::abs(n.z)};
            if (a.x >= a.y && a.x >= a.z)
                surface = n.x > 0 ? Enum::NormalId::Right : Enum::NormalId::Left;
            else if (a.y >= a.z)
                surface = n.y > 0 ? Enum::NormalId::Top : Enum::NormalId::Bottom;
            else
                surface = n.z > 0 ? Enum::NormalId::Back : Enum::NormalId::Front;
        }
        pushEnumItem(L, enums::item(surface));
    }
    return true;
}

// Scripts divide X by ViewSizeX for where on the screen the pointer is
// [live: Midnight Racing's map menu turns its camera by it; with no view
// size it divided 0 by 0 and drew nothing but black].
void updateMouseView(Shared& shared, Instance& mouse, Vector2 position, Vector2 viewSize)
{
    auto& values = shared.state->extraProperties[&mouse];
    values.first = mouse.shared_from_this();
    values.second.insert_or_assign("X", packed(Value(static_cast<double>(position.x))));
    values.second.insert_or_assign("Y", packed(Value(static_cast<double>(position.y))));
    values.second.insert_or_assign("ViewSizeX", packed(Value(static_cast<double>(viewSize.x))));
    values.second.insert_or_assign("ViewSizeY", packed(Value(static_cast<double>(viewSize.y))));
}

void watchTags(Shared& shared, DataModel& game)
{
    if (shared.tagWatch.connected())
        return;
    Shared* s = &shared;
    DataModel* g = &game;
    shared.tagWatch = game.descendantAdded.connect([s, g](Instance* instance) {
        if (instance->tags().empty())
            return;
        for (const std::string& tag : instance->tags())
            indexTag(*s->state, *instance, tag);
        if (Instance* service = collectionService(*g))
            for (const std::string& tag : std::vector(instance->tags()))
                s->events.fire(*service, std::format("{}{}", addedPrefix, tag),
                               PackedArgs{packed(instanceValue(instance))});
    });
    shared.tagUnwatch = game.descendantRemoving.connect([s, g](Instance* instance) {
        if (instance->tags().empty())
            return;
        for (const std::string& tag : instance->tags())
            unindexTag(*s->state, *instance, tag);
        if (Instance* service = collectionService(*g))
            for (const std::string& tag : std::vector(instance->tags()))
                s->events.fire(*service, std::format("{}{}", removedPrefix, tag),
                               PackedArgs{packed(instanceValue(instance))});
    });
}

void setCallback(lua_State* L, Instance& instance, std::string_view name, int idx)
{
    Runtime& rt = Runtime::of(L);
    // The wrong side refuses a remote's callback only while the remote is in
    // its game: the setters (0x1045ece88, 0x1045ecfa0) look for a DataModel
    // over it (0x104f57d88 walks the parents) and ask that whether it is the
    // client's (+0x91c) or the server's (+0x895). They drop what was set
    // before throwing. A remote out of any game takes either.
    const bool inGame = instance.findFirstAncestorOfClass("DataModel") != nullptr;
    const bool wrongSide =
        inGame && ((name == "OnServerInvoke" && !rt.isServer()) || (name == "OnClientInvoke" && !rt.isClient()));
    if (wrongSide) {
        rt.shared().setCallback(instance, name, &rt, LUA_NOREF);
        raise(L, name == "OnServerInvoke" ? "OnServerInvoke can only be implemented on the server"
                                          : "OnClientInvoke can only be implemented on the client");
    }
    if (lua_isnil(L, idx)) {
        rt.shared().setCallback(instance, name, &rt, LUA_NOREF);
        return;
    }
    luaL_checktype(L, idx, LUA_TFUNCTION);
    rt.shared().setCallback(instance, name, &rt, lua_ref(L, idx), rt.accessOf(L), Runtime::refGlobals(L));
}

namespace {

// UserSettings(): one object per game. Its name is not its class name: the
// client's constructor calls it "GlobalBasicSettings" (0x1041dcf78), and the
// 0x9b challenge adds up the bytes of tostring(UserSettings()).
int userSettingsGlobal(lua_State* L)
{
    InstancePtr& object = Runtime::of(L).shared().userSettings;
    if (!object) {
        object = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("UserSettings"));
        object->setName("GlobalBasicSettings");
        // UserSettings().GameSettings is the UserGameSettings service.
        auto gameSettings = std::make_shared<UserGameSettings>();
        gameSettings->setName("GameSettings");
        gameSettings->setParent(object.get());
    }
    return returnInstance(L, object.get());
}

// settings()'s object, one per game: a GlobalSettings the engine only knows by
// name, since nothing here models its members (the place file can't set them
// either). Its sections are children by name, as in the client
// (settings().Rendering is its RenderSettings), and GetService finds them too.
const Instance& globalSettings(lua_State* L)
{
    InstancePtr& object = Runtime::of(L).shared().globalSettings;
    if (!object) {
        object = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("GlobalSettings"));
        object->setName("GlobalSettings");
        for (const auto& [name, className] :
             {std::pair{"Rendering", "RenderSettings"}, std::pair{"Network", "NetworkSettings"},
              std::pair{"Physics", "PhysicsSettings"}, std::pair{"Diagnostics", "DebugSettings"},
              std::pair{"Task Scheduler", "TaskScheduler"}}) {
            auto section = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
            section->setName(name);
            section->setParent(object);
        }
    }
    return *object;
}

// settings() needs the Plugin capability, which game scripts lack: the
// client's global (0x1014c8dbe) checks it first (0x10459e2a4 with mask 1,
// "call", "settings"; bit 0 is named Plugin at 0x1016fa07b) and throws
// "The current thread cannot {} '{}' ({} {})" [code]. Only the console's Luau
// tab has that capability here.
int settingsGlobal(lua_State* L)
{
    if (!Runtime::of(L).accessOf(L).has(1)) // Plugin, the bit the client checks
        raise(L, "The current thread cannot call 'settings' (lacking capability Plugin)");
    return returnInstance(L, &globalSettings(L));
}

// PluginManager(), the deprecated way plugins got theirs. GUESS: it checks the
// Plugin capability as settings() does; past that it is an object the engine
// only knows by name, one per game.
int pluginManagerGlobal(lua_State* L)
{
    if (!Runtime::of(L).accessOf(L).has(1))
        raise(L, "The current thread cannot call 'PluginManager' (lacking capability Plugin)");
    InstancePtr& object = Runtime::of(L).shared().pluginManager;
    if (!object) {
        object = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("PluginManager"));
        object->setName("PluginManager");
    }
    return returnInstance(L, object.get());
}

// stats() and Stats(): the Stats service, as game:GetService("Stats") is.
int statsGlobal(lua_State* L)
{
    return returnInstance(L, findOrCreateService(L, Runtime::of(L).game(), "Stats", true));
}

} // namespace

void openServices(lua_State* L)
{
    lua_pushcfunction(L, userSettingsGlobal, "UserSettings");
    lua_setglobal(L, "UserSettings");
    lua_pushcfunction(L, settingsGlobal, "settings");
    lua_setglobal(L, "settings");
    lua_pushcfunction(L, pluginManagerGlobal, "PluginManager");
    lua_setglobal(L, "PluginManager");
    for (const char* name : {"stats", "Stats"}) {
        lua_pushcfunction(L, statsGlobal, name);
        lua_setglobal(L, name);
    }
}

} // namespace lunar::lua
