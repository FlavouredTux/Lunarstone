#include <lunar/script/ScriptContext.hpp>

#include <lunar/ClassRegistry.hpp>
#include <lunar/RobloxVersion.hpp>

#include "Characters.hpp"
#include "RobloxApi.hpp"
#include "Runtime.hpp"
#include "Services.hpp"

#include <Luau/Bytecode.h>
#include <Luau/BytecodeUtils.h>
#include <Luau/Common.h>
#include <Luau/Compiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>
#include <string>
#include <unordered_map>

namespace lunar {

namespace lua {

namespace {

// Roblox's limit on handlers that fire events that run handlers...
constexpr int maxEventDepth = 80;
// Deferred resumptions per flush before the rest waits for the next step.
constexpr int maxDeferredPerFlush = 10000;
// Slack for summed frame times, so wait(0.5) at 60 Hz resumes on frame 30.
constexpr double timeEpsilon = 1e-9;
// The legacy wait()'s shortest wait, its classic 30 Hz throttle. (Guess: the
// legacy scheduler's exact minimum isn't pinned against the client.)
constexpr double legacyMinimumWait = 0.03;

double clockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool later(const auto& a, const auto& b)
{
    return std::tie(a.at, a.seq) > std::tie(b.at, b.seq);
}

std::string joinArgs(lua_State* L)
{
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1)
            line += ' ';
        line.append(text, length);
        lua_pop(L, 1);
    }
    return line;
}

// The function (or thread) at `first` plus the arguments after it become a
// thread ready to resume; it replaces them on L's stack. Returns the thread
// and how many arguments it got. The task library's message for anything
// else, missing or not, is the client's own (its helper 0x1014cfb04 calls
// luaL_argerror with "function or thread expected" at 0x1014cfc9a): Luraph
// fingerprints builtins by what they raise (JJS hung on ours).
std::pair<lua_State*, int> prepareThread(lua_State* L, int first, bool task = false)
{
    if (task && !lua_isfunction(L, first) && !lua_isthread(L, first))
        luaL_argerror(L, first, "function or thread expected");
    const int nargs = lua_gettop(L) - first;
    if (lua_isthread(L, first)) {
        lua_State* thread = lua_tothread(L, first);
        if (thread == L)
            raise(L, "cannot resume the running thread");
        lua_xmove(L, thread, nargs);
        return {thread, nargs};
    }
    luaL_checktype(L, first, LUA_TFUNCTION);
    lua_State* thread = lua_newthread(L);
    lua_insert(L, first);
    lua_xmove(L, thread, nargs + 1);
    return {thread, nargs};
}

// Resumes the thread with the values already on its stack once the action runs.
auto resumeLater(Runtime& rt, lua_State* thread, int nargs, std::string_view what)
{
    std::uint64_t token = rt.suspend(thread);
    return [&rt, thread, token, nargs, what] { rt.wake(thread, token, [nargs](lua_State*) { return nargs; }, what); };
}

// --- Globals -----------------------------------------------------------------

int print(lua_State* L)
{
    Runtime::of(L).output(MessageType::Output, joinArgs(L));
    return 0;
}

int warn(lua_State* L)
{
    Runtime::of(L).output(MessageType::Warning, joinArgs(L));
    return 0;
}

int tick(lua_State* L)
{
    lua_pushnumber(L, scriptWallClock(L));
    return 1;
}

// os.clock under simulated clocks: the context's simulated seconds.
int simulatedOsClock(lua_State* L)
{
    lua_pushnumber(L, Runtime::of(L).now());
    return 1;
}

// os.time under simulated clocks: now is the simulated wall clock; a date
// table goes to the real os.time, the upvalue.
int simulatedOsTime(lua_State* L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushnumber(L, std::floor(scriptWallClock(L)));
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

int time(lua_State* L)
{
    lua_pushnumber(L, Runtime::of(L).now());
    return 1;
}

int elapsedTime(lua_State* L)
{
    static const double start = clockSeconds();
    lua_pushnumber(L, clockSeconds() - start);
    return 1;
}

int version(lua_State* L)
{
    lua_pushstring(L, lunar::robloxVersion);
    return 1;
}

// collectgarbage: Luau leaves it out, the client adds one (0x1014c7f30)
// that only counts: the heap in KB as a number, what gcinfo() says too.
// "collect" works there only behind a flag (0x10752bee0). GUESS: the flag
// is off in a live client, so it errors like any other option.
int collectGarbage(lua_State* L)
{
    if (std::string_view(luaL_checkstring(L, 1)) != "count")
        luaL_error(L, "collectgarbage must be called with 'count'; use gcinfo() instead");
    lua_pushnumber(L, static_cast<double>(lua_gc(L, LUA_GCCOUNT, 0)));
    return 1;
}

int noop(lua_State*)
{
    return 0;
}

// loadstring for game scripts: the client never compiles their code, so it
// answers nil and why. GUESS: the message, which is the server's with
// LoadStringEnabled off. The console has its own (Console.cpp).
int gameLoadstring(lua_State* L)
{
    lua_pushnil(L);
    lua_pushliteral(L, "loadstring() is not available");
    return 2;
}

// research: LUNAR_TRACE_WAITS=1 notes each wait a console thread asks for
// (which thread, what it asked for, how long it took on the wall clock), as
// a notice, which LogService doesn't see.
bool traceWaits()
{
    static const bool on = std::getenv("LUNAR_TRACE_WAITS") != nullptr;
    return on;
}

// task.wait yields until the Heartbeat-phase timer is due; the legacy wait
// has a minimum and resumes in its own phase, returning the time as well.
template <bool Legacy>
int wait(lua_State* L)
{
    // The client's own words, bare as its C++ exception arrives (task.wait
    // 0x1014cf855, wait 0x101472985), checked before the argument.
    if (!lua_isyieldable(L))
        raise(L, "thread is not yieldable");
    Runtime& rt = Runtime::of(L);
    double delay = luaL_optnumber(L, 1, 0.0);
    const bool traced = traceWaits() && rt.accessOf(L).console;
    const double asked = delay;
    const auto wallStart = std::chrono::steady_clock::now();
    if constexpr (Legacy)
        delay = std::max(delay, legacyMinimumWait);
    const double start = rt.now();
    std::uint64_t token = rt.suspend(L);
    auto resume = [&rt, L, token, start, traced, asked, wallStart] {
        if (traced)
            rt.notice(MessageType::Info,
                      std::format("trace: {} {} asked {} took {:.4f} s", Legacy ? "wait" : "task.wait",
                                  static_cast<const void*>(L), asked,
                                  std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count()));
        rt.wake(
            L, token,
            [&](lua_State* thread) {
                lua_pushnumber(thread, rt.now() - start);
                if constexpr (Legacy)
                    lua_pushnumber(thread, rt.now());
                return Legacy ? 2 : 1;
            },
            Legacy ? "wait" : "task.wait");
    };
    if constexpr (Legacy)
        rt.afterLegacy(delay, resume);
    else
        rt.after(delay, resume);
    return lua_yield(L, 0);
}

int taskSpawn(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    auto [thread, nargs] = prepareThread(L, 1, true);
    rt.forget(thread);
    if (auto error = rt.resume(thread, nargs, "task.spawn"))
        rt.report(*error);
    return 1;
}

int taskDefer(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    auto [thread, nargs] = prepareThread(L, 1, true);
    rt.defer(resumeLater(rt, thread, nargs, "task.defer"));
    return 1;
}

int taskDelay(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    const double delay = luaL_optnumber(L, 1, 0.0);
    auto [thread, nargs] = prepareThread(L, 2, true);
    rt.after(delay, resumeLater(rt, thread, nargs, "task.delay"));
    return 1;
}

int taskCancel(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State* thread = lua_tothread(L, 1);
    const int status = lua_costatus(L, thread);
    if (status == LUA_CORUN || status == LUA_CONOR)
        luaL_error(L, "cannot cancel thread"); // with the caller's position, as the client's (0x1014cf94e)
    Runtime::of(L).cancel(thread);
    return 0;
}

// Parallel Luau isn't a thing here; everything runs serially. An actor's
// thread still goes into the parallel phase and back out of it, which
// isparallel reports. Roblox raises for one outside an actor; here it stays
// as it was.
template <bool Parallel>
int taskSynchronize(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    ThreadAccess access = rt.accessOf(L);
    if (!access.actor.expired()) {
        access.parallel = Parallel;
        rt.setAccess(L, access);
    }
    return 0;
}

// Legacy spawn(f) and delay(t, f) run f from the legacy scheduler.
int legacySpawn(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    auto [thread, nargs] = prepareThread(L, 1);
    rt.afterLegacy(legacyMinimumWait, resumeLater(rt, thread, nargs, "spawn"));
    return 0;
}

int legacyDelay(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    const double delay = std::max(luaL_optnumber(L, 1, 0.0), legacyMinimumWait);
    auto [thread, nargs] = prepareThread(L, 2);
    rt.afterLegacy(delay, resumeLater(rt, thread, nargs, "delay"));
    return 0;
}

// require by string, as Roblox resolves Luau's paths: "./x" a sibling of the
// requiring script, "../x" one level further up, "@self/x" its own child,
// "@game/..." from the DataModel; each further component a child by name,
// ".." its parent.
Instance* resolveRequirePath(lua_State* L, std::string_view path)
{
    Instance* script = callingScript(L);
    Instance* at = nullptr;
    std::string_view rest;
    if (path.starts_with("@self")) {
        at = script;
        rest = path.substr(5);
    } else if (path.starts_with("@game")) {
        at = &Runtime::of(L).game();
        rest = path.substr(5);
    } else if (path.starts_with("./")) {
        at = script ? script->parent() : nullptr;
        rest = path.substr(2);
    } else if (path.starts_with("../")) {
        at = script && script->parent() ? script->parent()->parent() : nullptr;
        rest = path.substr(3);
    } else {
        raise(L, "require path must start with a valid prefix: ./, ../, or @"); // Luau's wording (GUESS: Roblox's too)
    }
    while (at && !rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (part.empty() || part == ".")
            continue;
        at = part == ".." ? at->parent() : at->findFirstChild(part);
    }
    if (!at)
        raise(L, std::format("could not resolve require path '{}' from {}", path,
                             script ? script->fullName() : std::string("a script-less thread")));
    return at;
}

int requireGlobal(lua_State* L)
{
    Instance* module = lua_type(L, 1) == LUA_TSTRING ? resolveRequirePath(L, checkString(L, 1)) : toInstance(L, 1);
    if (!module) {
        // Asset ids name modules on the website, which an offline client can't fetch.
        if (lua_type(L, 1) == LUA_TNUMBER)
            raise(L, std::format("Unable to load module {}: loading modules by asset id needs the network",
                                 static_cast<std::int64_t>(lua_tonumber(L, 1))));
        raise(L, "Attempted to call require with invalid argument(s).");
    }
    if (!module->isA("ModuleScript"))
        raise(L, "Attempted to call require with invalid argument(s).");
    return Runtime::of(L).require(L, *module);
}

void setFunction(lua_State* L, const char* name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

void setGlobal(lua_State* L, const char* name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn, name);
    lua_setglobal(L, name);
}

// The outermost function a thread runs; its environment says which script owns it.
bool pushThreadOwner(lua_State* main, lua_State* thread)
{
    const int depth = lua_stackdepth(thread);
    if (depth <= 0)
        return false;
    lua_Debug ar{};
    if (!lua_getinfo(thread, depth - 1, "f", &ar))
        return false;
    lua_getfenv(thread, -1);
    lua_remove(thread, -2);
    lua_xmove(thread, main, 1);
    return true;
}

} // namespace

// --- Helpers -----------------------------------------------------------------

ThreadAccess accessOf(RunAccess access)
{
    const api::Identity level = access == RunAccess::Full ? api::Identity::Full : api::Identity::Basic;
    return ThreadAccess{.identity = static_cast<int>(level),
                        .capabilities = api::capabilitiesOf(level),
                        .console = access == RunAccess::Full};
}

Instance* callingScript(lua_State* L)
{
    lua_Debug ar{};
    if (!lua_getinfo(L, 1, "f", &ar))
        return currentScript(L);
    lua_getfenv(L, -1);
    lua_getfield(L, -1, "script");
    Instance* script = toInstance(L, -1);
    lua_pop(L, 3);
    return script ? script : currentScript(L);
}

void raise(lua_State* L, std::string_view message)
{
    lua_pushlstring(L, message.data(), message.size());
    lua_error(L);
}

std::string_view checkString(lua_State* L, int idx)
{
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, idx, &length);
    return {text, length};
}

void checkYieldable(lua_State* L)
{
    if (!lua_isyieldable(L))
        raise(L, "attempt to yield across metamethod/C-call boundary");
}

namespace {

// pushNamecall's: upvalue 1 is the methods table, 2 the __index. A method
// runs in this frame, so its errors name it (Luau names a __namecall frame
// after the call's method); anything else is what indexing gives, called.
int namecallMethods(lua_State* L)
{
    const char* name = lua_namecallatom(L, nullptr);
    if (name) {
        lua_rawgetfield(L, lua_upvalueindex(1), name);
        const lua_CFunction method = lua_tocfunction(L, -1);
        const bool alone = method && !lua_getupvalue(L, -1, 1);
        lua_settop(L, lua_gettop(L) - (method && !alone ? 2 : 1));
        if (alone)
            return method(L);
    }
    const int args = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, 1);
    if (name)
        lua_pushstring(L, name);
    else
        lua_pushnil(L);
    lua_call(L, 2, 1);
    lua_insert(L, 1);
    lua_call(L, args, LUA_MULTRET);
    return lua_gettop(L);
}

} // namespace

void pushNamecall(lua_State* L)
{
    lua_pushcclosurek(L, namecallMethods, "__namecall", 2, nullptr);
}

void registerType(lua_State* L, int tag, const char* typeName, lua_Destructor dtor)
{
    lua_pushstring(L, typeName);
    lua_setfield(L, -2, "__type");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, tag);
    if (dtor)
        lua_setuserdatadtor(L, tag, dtor);
}

Instance* currentScript(lua_State* L)
{
    lua_getglobal(L, "script");
    Instance* script = toInstance(L, -1);
    lua_pop(L, 1);
    return script;
}

Listener::~Listener()
{
    // Can run inside the garbage collector, so the refs are released later.
    if (auto refs = graveyard.lock()) {
        if (callback != LUA_NOREF)
            refs->push_back(callback);
        if (thread != LUA_NOREF)
            refs->push_back(thread);
    }
}

void Listener::disconnect(lua_State* L)
{
    connection.disconnect();
    if (callback != LUA_NOREF)
        lua_unref(L, callback);
    if (thread != LUA_NOREF) {
        lua_unref(L, thread);
        thread = LUA_NOREF;
    }
    callback = LUA_NOREF;
}

// --- Runtime -----------------------------------------------------------------

Runtime::Runtime(std::shared_ptr<DataModel> game, ScriptRole role, std::shared_ptr<Shared> shared)
    : game_(std::move(game)), role_(role), shared_(std::move(shared)), main_(luaL_newstate())
{
    // Luau 0.744 handles Roblox's CALLFB instructions unconditionally.
    lua_callbacks(main_)->userdata = this;
    lua_callbacks(main_)->interrupt = interrupt;
    // Every thread the VM makes, whoever makes it: a console run's task.spawn
    // and its coroutines inherit its access.
    lua_callbacks(main_)->userthread = threadMade;
    setAccess(main_, ThreadAccess{});
    output_ = [](MessageType type, std::string_view message) {
        std::println(type == MessageType::Warning || type == MessageType::Error ? stderr : stdout, "{}", message);
    };
    shared_->runtimes.push_back(this);
    startTools(*shared_, *game_);
    startPrompts(*shared_, *game_);
    openGlobals();
}

Runtime::~Runtime()
{
    // Instances can outlive us; make sure none of their signals call back in.
    for (const auto& weak : listeners_)
        if (auto listener = weak.lock())
            listener->connection.disconnect();
    for (auto& [thread, suspension] : suspended_)
        suspension.connection.disconnect();
    timers_.clear();
    legacyTimers_.clear();
    deferred_.clear();
    suspended_.clear();
    finishers_.clear();
    shared_->dropCallbacks(this);
    std::erase(shared_->runtimes, this);
    if (consoleGlobals_ != LUA_NOREF)
        lua_unref(main_, consoleGlobals_);
    lua_close(main_);
}

Runtime& Runtime::of(lua_State* L)
{
    return *static_cast<Runtime*>(lua_callbacks(L)->userdata);
}

void Runtime::threadMade(lua_State* from, lua_State* thread)
{
    Runtime& rt = of(from ? from : thread);
    if (from) {
        rt.threads_.insert(thread);
        rt.setAccess(thread, rt.accessOf(from));
        return;
    }
    rt.threads_.erase(thread);
    // Gone: the record it had joins the free ones for the next thread.
    if (const auto* access = static_cast<ThreadAccess*>(lua_getthreaddata(thread))) {
        rt.freedAccess_.push_back(access->slot);
        lua_setthreaddata(thread, nullptr);
    }
}

ThreadAccess Runtime::accessOf(lua_State* thread) const
{
    // A thread with no record of its own runs as a game script does.
    const auto* access = static_cast<const ThreadAccess*>(lua_getthreaddata(thread));
    return access ? *access : ThreadAccess{};
}

void Runtime::setAccess(lua_State* thread, ThreadAccess access)
{
    if (auto* mine = static_cast<ThreadAccess*>(lua_getthreaddata(thread))) {
        access.slot = mine->slot;
        *mine = access;
        return;
    }
    if (!freedAccess_.empty()) {
        const std::size_t slot = freedAccess_.back();
        freedAccess_.pop_back();
        access.slot = slot;
        access_[slot] = access;
        lua_setthreaddata(thread, &access_[slot]);
        return;
    }
    access.slot = access_.size();
    access_.push_back(access);
    lua_setthreaddata(thread, &access_.back());
}

int Runtime::refGlobals(lua_State* L)
{
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}

void Runtime::useGlobals(lua_State* thread, int ref)
{
    if (ref == LUA_NOREF)
        return;
    lua_getref(thread, ref);
    if (lua_isthread(thread, -1)) {
        lua_getfenv(thread, -1); // its globals, without touching its stack
        lua_remove(thread, -2);
    }
    if (lua_istable(thread, -1))
        lua_replace(thread, LUA_GLOBALSINDEX);
    else
        lua_pop(thread, 1);
}

namespace {

struct ScriptRandom {
    std::mutex mutex;
    std::optional<std::mt19937_64> seeded;
};

ScriptRandom& scriptRandom()
{
    static ScriptRandom random;
    return random;
}

// simulateClocks' epoch, while it's on.
std::optional<double>& simulatedEpoch()
{
    static std::optional<double> epoch;
    return epoch;
}

} // namespace

double scriptWallClock(lua_State* L)
{
    if (const std::optional<double>& epoch = simulatedEpoch())
        return *epoch + Runtime::of(L).now();
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool clocksSimulated()
{
    return simulatedEpoch().has_value();
}

std::uint64_t scriptEntropy()
{
    ScriptRandom& random = scriptRandom();
    const std::scoped_lock lock(random.mutex);
    if (random.seeded)
        return (*random.seeded)();
    static std::random_device entropy;
    return (static_cast<std::uint64_t>(entropy()) << 32U) | entropy();
}

void Runtime::openGlobals()
{
    lua_State* L = main_;
    luaL_openlibs(L);
    // Luau seeds math.random from the clock and the state's address; a seeded
    // run gives each context the next number of its sequence instead.
    bool seeded = false;
    {
        const std::scoped_lock lock(scriptRandom().mutex);
        seeded = scriptRandom().seeded.has_value();
    }
    if (seeded) {
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "randomseed");
        lua_pushnumber(L, static_cast<double>(scriptEntropy() & 0x7FFFFFFFU));
        lua_call(L, 1, 0);
        lua_pop(L, 1);
    }
    if (clocksSimulated()) {
        lua_getglobal(L, "os");
        lua_pushcfunction(L, simulatedOsClock, "clock");
        lua_setfield(L, -2, "clock");
        lua_getfield(L, -1, "time");
        lua_pushcclosure(L, simulatedOsTime, "time", 1);
        lua_setfield(L, -2, "time");
        lua_pop(L, 1);
    }
    openDatatypes(L);
    openInstances(L);
    openEvents(L);
    openServices(L);
    openSharedTable(L);
    openAppHttp(L);
    openUtf8Extensions(L);

    setGlobal(L, "print", print);
    setGlobal(L, "warn", warn);
    setGlobal(L, "tick", tick);
    setGlobal(L, "time", time);
    setGlobal(L, "elapsedTime", elapsedTime);
    setGlobal(L, "ElapsedTime", elapsedTime);
    setGlobal(L, "version", version);
    setGlobal(L, "Version", version);
    setGlobal(L, "wait", wait<true>);
    setGlobal(L, "Wait", wait<true>);
    setGlobal(L, "spawn", legacySpawn);
    setGlobal(L, "Spawn", legacySpawn);
    setGlobal(L, "delay", legacyDelay);
    setGlobal(L, "Delay", legacyDelay);
    lua_pushcclosurek(L, requireGlobal, "require", 0, requireContinuation());
    lua_setglobal(L, "require");
    setGlobal(L, "printidentity", noop);
    setGlobal(L, "loadstring", gameLoadstring);
    setGlobal(L, "collectgarbage", collectGarbage);
    lua_getglobal(L, "pcall");
    lua_setglobal(L, "ypcall");

    lua_newtable(L);
    setFunction(L, "spawn", taskSpawn);
    setFunction(L, "defer", taskDefer);
    setFunction(L, "delay", taskDelay);
    setFunction(L, "wait", wait<false>);
    setFunction(L, "cancel", taskCancel);
    setFunction(L, "synchronize", taskSynchronize<false>);
    setFunction(L, "desynchronize", taskSynchronize<true>);
    lua_setglobal(L, "task");

    // Roblox's additions to debug: profiling and memory categories, no-ops here.
    lua_getglobal(L, "debug");
    for (const char* name : {"profilebegin", "profileend", "setmemorycategory", "resetmemorycategory"})
        setFunction(L, name, noop);
    lua_pop(L, 1);

    pushInstance(L, game_.get());
    lua_pushvalue(L, -1);
    lua_setglobal(L, "game");
    lua_setglobal(L, "Game");
    pushInstance(L, game_->workspace());
    lua_pushvalue(L, -1);
    lua_setglobal(L, "workspace");
    lua_setglobal(L, "Workspace");

    installBuiltinTrace(L); // research, off unless LUNAR_TRACE_BUILTINS is set

    // Libraries and globals become read-only; each script writes its own globals.
    luaL_sandbox(L);

    // Except _G and shared, which Roblox shares between all scripts.
    lua_setreadonly(L, LUA_GLOBALSINDEX, false);
    lua_newtable(L);
    lua_setglobal(L, "_G");
    lua_newtable(L);
    lua_setglobal(L, "shared");
    lua_setreadonly(L, LUA_GLOBALSINDEX, true);
}

namespace {

// research: LUNAR_TRACE_SLOW=<ms> logs each resume that ran longer than that,
// with the script and what resumed it: what froze a frame. Nested resumes
// (task.spawn from a handler) are indented under theirs, and an outermost
// one that was slow lists the lines its time went to, sampled every 1024th
// VM interrupt (the innermost three frames of each sample).
double slowTraceSeconds()
{
    static const double seconds = [] {
        const char* value = std::getenv("LUNAR_TRACE_SLOW");
        return value && *value ? std::atof(value) / 1000.0 : 0.0;
    }();
    return seconds;
}

std::unordered_map<std::string, int>& slowSamples()
{
    static std::unordered_map<std::string, int> samples;
    return samples;
}

std::unordered_map<std::string, double>& slowNatives()
{
    static std::unordered_map<std::string, double> natives;
    return natives;
}

void sampleSlow(lua_State* L)
{
    std::string key;
    lua_Debug ar{};
    for (int level = 0, frames = 0; frames < 3 && lua_getinfo(L, level, "sl", &ar); ++level) {
        if (ar.currentline < 0)
            continue;
        if (!key.empty())
            key += " < ";
        key += std::format("{}:{}", ar.short_src, ar.currentline);
        ++frames;
    }
    ++slowSamples()[key];
}

void printSlowSamples()
{
    std::vector<std::pair<std::string, int>> top(slowSamples().begin(), slowSamples().end());
    std::ranges::sort(top, std::greater{}, &std::pair<std::string, int>::second);
    int total = 0;
    for (const auto& [key, count] : top)
        total += count;
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 15); ++i)
        std::fprintf(stderr, "slow:   %5.1f%% %s\n", 100.0 * top[i].second / std::max(total, 1), top[i].first.c_str());
    std::vector<std::pair<std::string, double>> natives(slowNatives().begin(), slowNatives().end());
    std::ranges::sort(natives, std::greater{}, &std::pair<std::string, double>::second);
    for (std::size_t i = 0; i < std::min<std::size_t>(natives.size(), 15); ++i)
        std::fprintf(stderr, "slow:   %7.0f ms %s\n", natives[i].second * 1000.0, natives[i].first.c_str());
}

} // namespace

bool slowTraceOn()
{
    return slowTraceSeconds() > 0.0;
}

double slowClock()
{
    return clockSeconds();
}

void noteSlowNative(std::string key, double seconds)
{
    slowNatives()[std::move(key)] += seconds;
}

void Runtime::interrupt(lua_State* L, int gc)
{
    if (gc < 0 && slowTraceSeconds() > 0.0 && (of(L).interrupts_ & 1023u) == 1023u)
        sampleSlow(L);
    if (gc >= 0)
        return;
    Runtime& rt = of(L);
    // The VM interrupts at every call and loop iteration; reading the clock
    // each time took a quarter of a tight loop's time. Every 1024th is
    // still far finer than the ten second limit.
    if (rt.deadline_ <= 0.0 || (++rt.interrupts_ & 1023u) != 0)
        return;
    if (clockSeconds() > rt.deadline_) {
        // Whoever catches this gets a fresh allowance.
        rt.deadline_ = clockSeconds() + rt.timeout_;
        dumpBuiltinTrace();
        luaL_where(L, 0); // the interrupted Lua function itself
        lua_pushliteral(L, "Script timeout: exhausted allowed execution time");
        lua_concat(L, 2);
        lua_error(L);
    }
}

// Everything printed also goes to LogService.MessageOut (message, Enum.MessageType)
// and its history.
void Runtime::output(MessageType type, std::string_view message) const
{
    if (output_)
        output_(type, message);
    // GUESS: the history's length; old entries go past this many.
    constexpr std::size_t historyLimit = 10000;
    if (logHistory_.size() >= historyLimit)
        logHistory_.pop_front();
    // GUESS: whole Unix seconds, like os.time() (the documented unit).
    logHistory_.push_back(LogEntry{std::string(message), type, static_cast<double>(std::time(nullptr))});
    static constexpr std::string_view names[] = {"MessageOutput", "MessageInfo", "MessageWarning", "MessageError"};
    static thread_local int depth = 0;
    Instance* logService = game_->findService("LogService");
    if (!logService) {
        for (const InstancePtr& child : game_->children())
            if (child->className() == "LogService")
                logService = child.get();
    }
    if (!logService || depth > 0)
        return; // handlers printing don't feed back into MessageOut
    ++depth;
    shared_->events.fire(*logService, "MessageOut",
                         PackedArgs{packed(Value(std::string(message))),
                                    packed(Value(enumItemNamed("MessageType", names[static_cast<int>(type)])))});
    --depth;
}

void Runtime::notice(MessageType type, std::string_view message) const
{
    if (output_)
        output_(type, message);
}

// Errors also fire ScriptContext.Error (message, stack trace, script).
void Runtime::report(const ScriptError& error) const
{
    output(MessageType::Error, error.message);
    if (!error.traceback.empty())
        output(MessageType::Info, std::format("Stack Begin\n{}Stack End", error.traceback));
    for (const InstancePtr& child : game_->children()) {
        if (child->className() != "ScriptContext")
            continue;
        shared_->events.fire(*child, "Error",
                             PackedArgs{packed(Value(error.message)), packed(Value(error.traceback)), packed(Value())});
        break;
    }
}

// Workspace.SignalBehavior: Default reads as Immediate (see SignalBehavior),
// AncestryDeferred defers only ancestry events in Roblox; it's Immediate here.
bool Runtime::deferredSignals() const
{
    if (dataUpdateInProgress())
        return true;
    if (signalBehavior_)
        return *signalBehavior_ == SignalBehavior::Deferred;
    const Workspace* workspace = game_->findFirstChildOfClass("Workspace") ? game_->workspace() : nullptr;
    return workspace && workspace->signalBehavior() == Enum::SignalBehavior::Deferred;
}

bool Runtime::dataUpdateInProgress() const
{
    return shared_->dataUpdateInProgress && shared_->dataUpdateInProgress();
}

std::expected<lua_State*, ScriptError> Runtime::loadThread(std::string_view bytecode, std::string_view chunkName,
                                                           Instance* script, RunAccess runAccess)
{
    lua_State* thread = lua_newthread(main_);
    luaL_sandboxthread(thread);
    if (builtinsHooked_)
        lua_setsafeenv(thread, LUA_GLOBALSINDEX, 0);
    ThreadAccess access = accessOf(thread);
    if (runAccess == RunAccess::Full) {
        // Full access, and the globals an executor offers, before the chunk
        // loads: its globals are the ones it loads with.
        access = lua::accessOf(runAccess);
        openConsole(thread);
    } else {
        pushInstance(thread, script);
        lua_setglobal(thread, "script");
    }
    access.script = script ? script->weak_from_this() : std::weak_ptr<Instance>();
    if (Instance* actor = script ? script->findFirstAncestorOfClass("Actor") : nullptr)
        access.actor = actor->weak_from_this();
    setAccess(thread, access);

    const std::string name = std::format("={}", chunkName);
    if (luau_load(thread, name.c_str(), bytecode.data(), bytecode.size(), 0) != 0) {
        ScriptError error{lua_tostring(thread, -1), {}};
        lua_pop(main_, 1);
        return std::unexpected(std::move(error));
    }
    return thread;
}

std::expected<void, ScriptError> Runtime::runChunk(std::string_view bytecode, std::string_view chunkName,
                                                   Instance* script, RunAccess access)
{
    auto thread = loadThread(bytecode, chunkName, script, access);
    if (!thread)
        return std::unexpected(std::move(thread.error()));
    const int ref = lua_ref(main_, -1);
    lua_pop(main_, 1);
    auto error = resume(*thread, 0, "start");
    lua_unref(main_, ref);
    flushDeferred();
    collectRefs();
    if (error)
        return std::unexpected(std::move(*error));
    return {};
}

std::expected<std::vector<double>, ScriptError>
Runtime::callChunk(std::string_view bytecode, std::string_view chunkName, std::span<const double> args)
{
    auto thread = loadThread(bytecode, chunkName, nullptr);
    if (!thread)
        return std::unexpected(std::move(thread.error()));
    lua_State* t = *thread;
    const int ref = lua_ref(main_, -1);
    lua_pop(main_, 1);
    for (const double a : args)
        lua_pushnumber(t, a);
    lua_State* from = running_;
    if (depth_ == 0)
        deadline_ = timeout_ > 0.0 ? clockSeconds() + timeout_ : 0.0;
    ++depth_;
    running_ = t;
    const int status = lua_resume(t, from, static_cast<int>(args.size()));
    running_ = from;
    if (--depth_ == 0)
        deadline_ = 0.0;
    std::expected<std::vector<double>, ScriptError> result;
    if (status == LUA_OK) {
        std::vector<double> values;
        for (int i = 1; i <= lua_gettop(t); ++i)
            values.push_back(lua_isnumber(t, i) ? lua_tonumber(t, i) : std::numeric_limits<double>::quiet_NaN());
        result = std::move(values);
    } else {
        ScriptError error;
        error.message = status == LUA_YIELD   ? "the chunk yielded"
                        : lua_isstring(t, -1) ? lua_tostring(t, -1)
                                              : "Error occurred, no output from Lua.";
        result = std::unexpected(std::move(error));
    }
    lua_settop(t, 0);
    lua_unref(main_, ref);
    collectRefs();
    return result;
}

std::optional<ScriptError> Runtime::resume(lua_State* thread, int nargs, std::string_view what, bool raising)
{
    std::optional<Profiler::Scope> profiled;
    if (shared_->profiler)
        profiled.emplace(shared_->profiler.get(), resumedScript(thread, nargs), role_, what.empty() ? "resume" : what);
    // research: LUNAR_TRACE_SLOW (slowTraceSeconds).
    const double slowSeconds = slowTraceSeconds();
    const bool timed = slowSeconds > 0.0;
    if (timed && depth_ == 0) {
        slowSamples().clear();
        slowNatives().clear();
    }
    const int slowDepth = depth_;
    const Instance* slowScript = timed ? resumedScript(thread, nargs) : nullptr;
    const double started = timed ? clockSeconds() : 0.0;
    lua_State* from = running_;
    if (depth_ == 0)
        deadline_ = timeout_ > 0.0 ? clockSeconds() + timeout_ : 0.0;
    ++depth_;
    running_ = thread;
    const int status = raising ? lua_resumeerror(thread, from) : lua_resume(thread, from, nargs);
    running_ = from;
    if (--depth_ == 0)
        deadline_ = 0.0;
    if (profiled)
        profiled->finish();
    if (timed && clockSeconds() - started > slowSeconds)
        std::fprintf(stderr, "slow: %*s%.0f ms in %s (%.*s)\n", slowDepth * 2, "", (clockSeconds() - started) * 1000.0,
                     slowScript ? slowScript->fullName().c_str() : "?", static_cast<int>(what.size()), what.data());
    if (timed && slowDepth == 0 && clockSeconds() - started > slowSeconds)
        printSlowSamples();

    if (status == LUA_YIELD) {
        lua_pop(thread, lua_gettop(thread)); // values passed to coroutine.yield
        return std::nullopt;
    }
    auto finisher = finishers_.extract(thread);
    if (status == LUA_OK) {
        if (finisher)
            finisher.mapped()(thread, status, lua_gettop(thread));
        lua_pop(thread, lua_gettop(thread));
        return std::nullopt;
    }

    ScriptError error;
    error.message = lua_isstring(thread, -1) ? lua_tostring(thread, -1) : "Error occurred, no output from Lua.";
    error.traceback = lua_debugtrace(thread);
    if (finisher)
        finisher.mapped()(thread, status, 0);
    forget(thread);
    lua_resetthread(thread);
    return error;
}

void Runtime::onFinish(lua_State* thread, Finisher finisher)
{
    finishers_.insert_or_assign(thread, std::move(finisher));
}

std::uint64_t Runtime::suspend(lua_State* thread, Connection connection)
{
    forget(thread);
    lua_pushthread(thread);
    const int ref = lua_ref(thread, -1);
    lua_pop(thread, 1);
    const std::uint64_t token = ++nextToken_;
    suspended_.insert_or_assign(thread, Suspension{token, ref, std::move(connection)});
    return token;
}

bool Runtime::isSuspended(lua_State* thread, std::uint64_t token) const
{
    auto it = suspended_.find(thread);
    return it != suspended_.end() && it->second.token == token;
}

void Runtime::attach(lua_State* thread, std::uint64_t token, Connection connection)
{
    auto it = suspended_.find(thread);
    if (it != suspended_.end() && it->second.token == token)
        it->second.connection = std::move(connection);
    else
        connection.disconnect();
}

bool Runtime::wake(lua_State* thread, std::uint64_t token, ArgPusher push, std::string_view what)
{
    auto it = suspended_.find(thread);
    if (it == suspended_.end() || it->second.token != token)
        return false;
    Suspension suspension = std::move(it->second);
    suspended_.erase(it);
    suspension.connection.disconnect();

    if (lua_costatus(main_, thread) == LUA_COSUS) {
        const int nargs = push(thread);
        if (auto error = resume(thread, nargs, what))
            report(*error);
    }
    lua_unref(main_, suspension.ref);
    return true;
}

bool Runtime::fail(lua_State* thread, std::uint64_t token, std::string_view message, std::string_view what)
{
    auto it = suspended_.find(thread);
    if (it == suspended_.end() || it->second.token != token)
        return false;
    Suspension suspension = std::move(it->second);
    suspended_.erase(it);
    suspension.connection.disconnect();

    if (lua_costatus(main_, thread) == LUA_COSUS) {
        lua_pushlstring(thread, message.data(), message.size());
        if (auto error = resume(thread, 1, what, true))
            report(*error);
    }
    lua_unref(main_, suspension.ref);
    return true;
}

void Runtime::wakeWith(lua_State* thread, std::uint64_t token, PackedArgs args, std::string what)
{
    auto it = suspended_.find(thread);
    if (it == suspended_.end() || it->second.token != token)
        return;
    // One fire only: the connection that woke us goes now, even if we resume later.
    it->second.connection.disconnect();
    auto run = [this, thread, token, args = std::move(args), what = std::move(what)] {
        wake(thread, token, [&](lua_State* t) { return unpackArgs(t, args); }, what);
    };
    if (deferredSignals())
        defer(std::move(run));
    else
        run();
}

void Runtime::forget(lua_State* thread)
{
    auto it = suspended_.find(thread);
    if (it == suspended_.end())
        return;
    it->second.connection.disconnect();
    lua_unref(main_, it->second.ref);
    suspended_.erase(it);
}

void Runtime::cancel(lua_State* thread)
{
    if (lua_costatus(main_, thread) == LUA_COSUS)
        lua_resetthread(thread);
    forget(thread);
    finishers_.erase(thread);
}

void Runtime::after(double delay, std::move_only_function<void()> action)
{
    timers_.push_back(Timer{now() + std::max(delay, 0.0), nextSeq_++, std::move(action)});
    std::ranges::push_heap(timers_, [](const Timer& a, const Timer& b) { return later(a, b); });
}

void Runtime::afterLegacy(double delay, std::move_only_function<void()> action)
{
    legacyTimers_.push_back(Timer{now() + std::max(delay, 0.0), nextSeq_++, std::move(action)});
    std::ranges::push_heap(legacyTimers_, [](const Timer& a, const Timer& b) { return later(a, b); });
}

void Runtime::defer(std::move_only_function<void()> action)
{
    deferred_.push_back(std::move(action));
}

void Runtime::advance(double dt)
{
    collectRefs();
    clock_ += dt;
    stepCollector();
}

// The client's collector works a little every frame, not only when the
// scripts allocate: garbage goes, and weak tables empty, while nothing does
// (sUNC's cloneref test waits on one). A frame's share is a millisecond, or
// the end of the cycle it is in. After a cycle it rests 99 times as long as
// the cycle took, a quarter of a second at most, so a heap nothing touches
// isn't marked over and over: the game's 8 MB took 2.7 ms a cycle, ten cycles
// a second, a third of a headless client's CPU. Weak tables still empty
// within a quarter of a second of a cycle, and a small heap's cycle, a
// millisecond, is back within a tenth of one.
void Runtime::stepCollector()
{
    constexpr double budget = 0.001;
    constexpr double rest = 99.0, longestRest = 0.25;
    const double start = clockSeconds();
    if (start < collectorResumeAt_)
        return;
    double now = start;
    bool finished = false;
    while (now - start < budget && !finished) {
        finished = lua_gc(main_, LUA_GCSTEP, 64) != 0;
        now = clockSeconds();
    }
    collectorWork_ += now - start;
    if (finished) {
        collectorResumeAt_ = now + std::min(rest * collectorWork_, longestRest);
        collectorWork_ = 0.0;
    }
}

void Runtime::runTimers(std::vector<Timer>& timers)
{
    // Timers added while we run (task.wait() in a loop) wait for the next step.
    const std::uint64_t limit = nextSeq_;
    while (!timers.empty() && timers.front().at <= now() + timeEpsilon && timers.front().seq < limit) {
        std::ranges::pop_heap(timers, [](const Timer& a, const Timer& b) { return later(a, b); });
        Timer timer = std::move(timers.back());
        timers.pop_back();
        timer.action();
        flushDeferred();
    }
    flushDeferred();
    collectRefs();
}

void Runtime::resumeTimers()
{
    runTimers(timers_);
}

void Runtime::resumeLegacyTimers()
{
    runTimers(legacyTimers_);
}

void Runtime::flushDeferred()
{
    if (depth_ > 0 || dataUpdateInProgress())
        return; // runs once the outermost resumption is done
    for (int budget = maxDeferredPerFlush; !deferred_.empty(); --budget) {
        if (budget == 0) {
            output(MessageType::Warning, "Too many deferred resumptions, continuing next step");
            return;
        }
        auto action = std::move(deferred_.front());
        deferred_.pop_front();
        action();
    }
}

bool Runtime::idle() const
{
    return timers_.empty() && legacyTimers_.empty() && deferred_.empty() && shared_->inbox.empty()
           && shared_->invocations.empty();
}

std::shared_ptr<Listener> Runtime::newListener()
{
    if (listeners_.size() >= 64 && listeners_.size() == listeners_.capacity())
        std::erase_if(listeners_, [](const auto& weak) { return weak.expired(); });
    auto listener = std::make_shared<Listener>();
    listener->runtime = this;
    listener->graveyard = graveyard_;
    listeners_.push_back(listener);
    return listener;
}

std::vector<std::shared_ptr<Listener>> Runtime::listeners() const
{
    std::vector<std::shared_ptr<Listener>> found;
    for (const auto& weak : listeners_)
        if (auto listener = weak.lock(); listener && listener->callback != LUA_NOREF)
            found.push_back(std::move(listener));
    return found;
}

Runtime::ConsoleConnection* Runtime::keepConnection(std::shared_ptr<Listener> listener, SignalData signal)
{
    // Kept for as long as the VM lives: the table getconnections made closes
    // over this, and may be held (or fired from) long after the connection
    // itself has gone.
    auto kept = std::make_unique<ConsoleConnection>();
    kept->listener = std::move(listener);
    kept->signal = std::move(signal);
    consoleConnections_.push_back(std::move(kept));
    return consoleConnections_.back().get();
}

void Runtime::dispatch(const std::shared_ptr<Listener>& listener, const PackedArgs& args, bool now)
{
    if (listener->callback == LUA_NOREF || !listener->enabled)
        return;
    // Once disconnects at the fire, even when the handler runs later.
    if (listener->once)
        listener->connection.disconnect();

    auto run = [this](Listener& l, const PackedArgs& arguments) {
        if (l.callback == LUA_NOREF)
            return; // disconnected before a deferred handler got to run
        if (eventDepth_ >= maxEventDepth) {
            output(MessageType::Error, std::format("Maximum event re-entrancy depth exceeded for {}", l.source));
            return;
        }
        // Each fire has a thread of its own, as the engine always gives one.
        lua_State* thread = lua_newthread(main_);
        // The handler runs as whoever connected it: a console run's
        // connections keep the full access and the globals their code had.
        setAccess(thread, l.access);
        useGlobals(thread, l.thread);
        const int ref = lua_ref(main_, -1);
        lua_pop(main_, 1);
        lua_getref(thread, l.callback);
        if (l.once)
            l.disconnect(main_);
        const int nargs = unpackArgs(thread, arguments);

        ++eventDepth_;
        auto error = resume(thread, nargs, l.source);
        --eventDepth_;
        if (error)
            report(*error);
        lua_unref(main_, ref);
    };
    if (deferredSignals() && !now) {
        defer([run, listener, args] { run(*listener, args); });
        return;
    }
    run(*listener, args);
    flushDeferred();
}

void Runtime::collectRefs()
{
    std::vector<int> refs;
    std::swap(refs, *graveyard_);
    for (int ref : refs)
        lua_unref(main_, ref);
}

// --- Scripts -------------------------------------------------------------------

bool Runtime::isRunning(const Instance& script) const
{
    auto it = scripts_.find(&script);
    return it != scripts_.end() && it->second.script.lock().get() == &script;
}

bool Runtime::startedInActor(const Instance& script) const
{
    auto it = scripts_.find(&script);
    return it != scripts_.end() && it->second.script.lock().get() == &script && it->second.actor;
}

std::vector<InstancePtr> Runtime::runningActors() const
{
    std::vector<InstancePtr> actors;
    for (const ScriptRecord& record : scripts_ | std::views::values) {
        const InstancePtr script = record.script.lock();
        Instance* actor = script && record.actor ? script->findFirstAncestorOfClass("Actor") : nullptr;
        if (actor && std::ranges::find(actors, actor, &InstancePtr::get) == actors.end())
            actors.push_back(actor->shared_from_this());
    }
    return actors;
}

int Runtime::environmentOf(const Instance& script) const
{
    auto it = scripts_.find(&script);
    return it != scripts_.end() && it->second.script.lock().get() == &script ? it->second.env : int{LUA_NOREF};
}

std::vector<InstancePtr> Runtime::loadedModules() const
{
    std::vector<InstancePtr> modules;
    for (const auto& [key, module] : modules_) {
        if (module.state != Module::State::Loaded || module.module.lock().get() != key)
            continue;
        if (InstancePtr held = module.module.lock())
            modules.push_back(std::move(held));
    }
    return modules;
}

namespace {

// The console's setscriptable store, by instance and member name. The entry is
// dropped when the instance it was made for is gone, and an address that has
// been reused is not taken for the instance it was.
std::map<std::string, bool, std::less<>>* scriptableValues(const Instance& instance, SharedState& state, bool create)
{
    auto& all = state.scriptableOverrides;
    auto it = all.find(&instance);
    if (it != all.end() && it->second.first.lock().get() != &instance) {
        all.erase(it);
        it = all.end();
    }
    if (it == all.end()) {
        if (!create)
            return nullptr;
        if (all.size() > 1024)
            std::erase_if(all, [](const auto& entry) { return entry.second.first.expired(); });
        it = all.emplace(&instance, std::pair{std::const_pointer_cast<Instance>(instance.shared_from_this()),
                                              std::map<std::string, bool, std::less<>>{}})
                 .first;
    }
    return &it->second.second;
}

} // namespace

bool Runtime::scriptableOverride(const Instance& instance, std::string_view name)
{
    const std::map<std::string, bool, std::less<>>* values = scriptableValues(instance, *shared_->state, false);
    if (!values)
        return false;
    const auto it = values->find(name);
    return it != values->end() && it->second;
}

void Runtime::setScriptableOverride(const Instance& instance, std::string_view name, bool value)
{
    scriptableValues(instance, *shared_->state, true)->insert_or_assign(std::string(name), value);
}

void Runtime::noteInstance(const Instance& instance)
{
    // Every instance a script has been handed, so the console's
    // getnilinstances() has the ones nothing parents to look through.
    std::vector<std::weak_ptr<Instance>>& known = shared_->state->knownInstances;
    known.push_back(std::const_pointer_cast<Instance>(instance.shared_from_this()));
    // Dead ones go as the list grows, like the extra properties' owners. A
    // sweep every thousand or so keeps that off the per-instance path.
    constexpr std::size_t sweep = 1024;
    if (known.size() > sweep && known.size() % sweep == 0)
        std::erase_if(known, [](const auto& weak) { return weak.expired(); });
}

std::expected<void, ScriptError> Runtime::startScript(Instance& script)
{
    if (isRunning(script))
        return {};
    const Property* sourceProperty = script.classInfo().findProperty("Source");
    if (!sourceProperty)
        return std::unexpected(ScriptError{
            std::format("{}: can't read Source, {} isn't implemented yet", script.fullName(), script.className()), {}});
    Value value = sourceProperty->get(script);
    const auto* source = std::get_if<std::string>(&value);
    const std::string chunkName = script.fullName();
    std::optional<Profiler::Scope> compiling;
    if (shared_->profiler)
        compiling.emplace(shared_->profiler.get(), &script, role_, "compile");
    auto bytecode = codeOf(source ? *source : std::string(), chunkName);
    if (compiling)
        compiling->finish();
    if (!bytecode)
        return std::unexpected(ScriptError{std::move(bytecode.error()), {}});

    auto thread = loadThread(*bytecode, chunkName, &script);
    if (!thread)
        return std::unexpected(std::move(thread.error()));
    lua_pushvalue(*thread, LUA_GLOBALSINDEX);
    lua_xmove(*thread, main_, 1);
    const int env = lua_ref(main_, -1);
    lua_pop(main_, 1);
    scripts_.insert_or_assign(&script,
                              ScriptRecord{script.shared_from_this(), env, script.findFirstAncestorOfClass("Actor") != nullptr});

    const int ref = lua_ref(main_, -1);
    lua_pop(main_, 1);
    auto error = resume(*thread, 0, "start");
    lua_unref(main_, ref);
    flushDeferred();
    collectRefs();
    if (error)
        return std::unexpected(std::move(*error));
    return {};
}

void Runtime::stopScript(const Instance& script)
{
    auto it = scripts_.find(&script);
    if (it == scripts_.end())
        return;
    const int env = it->second.env;
    scripts_.erase(it);
    if (env == LUA_NOREF)
        return;
    lua_State* L = main_;
    lua_getref(L, env);
    const int envIndex = lua_gettop(L);

    // Threads it started die, its connections drop.
    std::vector<lua_State*> doomed;
    for (const auto& [thread, suspension] : suspended_) {
        if (!pushThreadOwner(L, thread))
            continue;
        if (lua_rawequal(L, -1, envIndex))
            doomed.push_back(thread);
        lua_pop(L, 1);
    }
    for (lua_State* thread : doomed)
        cancel(thread);
    for (const auto& weak : listeners_) {
        auto listener = weak.lock();
        if (!listener || listener->callback == LUA_NOREF)
            continue;
        lua_getref(L, listener->callback);
        lua_getfenv(L, -1);
        const bool owned = lua_rawequal(L, -1, envIndex);
        lua_pop(L, 2);
        if (owned)
            listener->disconnect(L);
    }
    lua_pop(L, 1);
    lua_unref(L, env);
}

// --- RunService:BindToRenderStep --------------------------------------------------

void Runtime::bindToRenderStep(std::string name, int priority, int ref, int thread, ThreadAccess access,
                               int globals)
{
    renderBindings_.push_back(RenderBinding{std::move(name), priority, nextSeq_++, ref, thread, access, globals});
    std::ranges::stable_sort(renderBindings_, {}, &RenderBinding::priority);
}

void Runtime::unbindFromRenderStep(std::string_view name)
{
    // Roblox removes every binding with the name (and warns when there were several).
    std::erase_if(renderBindings_, [&](const RenderBinding& binding) {
        if (binding.name != name)
            return false;
        lua_unref(main_, binding.ref);
        lua_unref(main_, binding.thread);
        if (binding.globals != LUA_NOREF)
            lua_unref(main_, binding.globals);
        return true;
    });
}

int Runtime::pushRenderBindings(lua_State* L) const
{
    lua_createtable(L, static_cast<int>(renderBindings_.size()), 0);
    int i = 0;
    for (const RenderBinding& binding : renderBindings_) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, binding.name.data(), binding.name.size());
        lua_setfield(L, -2, "Name");
        lua_pushinteger(L, binding.priority);
        lua_setfield(L, -2, "Priority");
        lua_getref(L, binding.ref);
        lua_setfield(L, -2, "Function");
        lua_getref(L, binding.thread);
        lua_setfield(L, -2, "Thread");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

void Runtime::renderStep(double dt)
{
    // Lowest priority first; bindings made while running wait for the next frame.
    std::vector<int> refs;
    refs.reserve(renderBindings_.size());
    for (const RenderBinding& binding : renderBindings_)
        refs.push_back(binding.ref);
    for (int ref : refs) {
        auto binding = std::ranges::find(renderBindings_, ref, &RenderBinding::ref);
        if (binding == renderBindings_.end())
            continue; // unbound by an earlier callback this frame
        lua_State* thread = lua_newthread(main_);
        setAccess(thread, binding->access);
        useGlobals(thread, binding->globals);
        const int threadRef = lua_ref(main_, -1);
        lua_pop(main_, 1);
        lua_getref(thread, ref);
        lua_pushnumber(thread, dt);
        if (auto error = resume(thread, 1, "BindToRenderStep"))
            report(*error);
        lua_unref(main_, threadRef);
    }
    flushDeferred();
}

} // namespace lua

void simulateClocks(std::optional<double> epoch)
{
    lua::simulatedEpoch() = epoch;
}

void seedScripts(std::optional<std::uint64_t> seed)
{
    lua::ScriptRandom& random = lua::scriptRandom();
    const std::scoped_lock lock(random.mutex);
    if (seed)
        random.seeded.emplace(*seed);
    else
        random.seeded.reset();
}

// --- ScriptContext -----------------------------------------------------------

namespace {

// Services a running game has before any script asks for them: scripts
// index game["Run Service"] or game.Debris without GetService (the genuine
// client's own core scripts have made these by then; the game uses game.Debris).
// ServerStorage and ServerScriptService are there on a client too, empty:
// The game's loader parents a folder to game.ServerStorage either way.
void ensureStartupServices(DataModel& game)
{
    for (const char* name : {"RunService", "Debris", "TweenService", "UserInputService", "ContextActionService",
                             "HttpService", "GuiService", "ServerStorage", "ServerScriptService"})
        if (const ClassInfo* info = ClassRegistry::find(name); info && info->kind == ClassKind::Service)
            game.getService(name);
    installEnumCatalog();
}

} // namespace

void installEnumCatalog()
{
    enums::setCatalog(&lua::catalogEnum);
}

ScriptContext::ScriptContext(std::shared_ptr<DataModel> game, ScriptRole role)
    : runtime_(std::make_unique<lua::Runtime>(game, role, std::make_shared<lua::Shared>()))
{
    ensureStartupServices(*game);
}

ScriptContext::ScriptContext(std::shared_ptr<DataModel> game, ScriptRole role, const ScriptContext& peer)
    : runtime_(std::make_unique<lua::Runtime>(game, role, peer.runtime().sharedPtr()))
{
    ensureStartupServices(*game);
}

ScriptContext::~ScriptContext() = default;

void ScriptContext::setOutput(OutputSink sink)
{
    runtime_->setOutput(std::move(sink));
}

void ScriptContext::setTimeout(double seconds)
{
    runtime_->setTimeout(seconds);
}

void ScriptContext::setSignalBehavior(SignalBehavior behavior)
{
    runtime_->setSignalBehavior(behavior);
}

void ScriptContext::setRaycastHook(RaycastHook hook)
{
    runtime_->shared().raycast = std::move(hook);
}

std::string ScriptContext::mouseIcon() const
{
    if (const auto* uis = dynamic_cast<const UserInputService*>(game().findService("UserInputService"));
        uis && !uis->mouseIcon().empty())
        return uis->mouseIcon();
    // The Mouse is a placeholder: what scripts set its Icon to is kept
    // with the other members the engine doesn't implement.
    const auto* players = dynamic_cast<const Players*>(game().findFirstChildOfClass("Players"));
    const Player* player = players ? players->localPlayer() : nullptr;
    if (!player)
        return {};
    const auto& extra = runtime_->shared().state->extraProperties;
    for (const InstancePtr& child : player->children())
        if (child->className() == "PlayerMouse")
            if (auto it = extra.find(child.get()); it != extra.end() && it->second.first.lock() == child)
                if (auto icon = it->second.second.find("Icon"); icon != it->second.second.end())
                    if (const auto* text = std::get_if<std::string>(&icon->second.value))
                        return *text;
    return {};
}

PromptSystem* ScriptContext::prompts() const
{
    return runtime_->shared().prompts.get();
}

bool ScriptContext::mouseIconShown() const
{
    const auto* uis = dynamic_cast<const UserInputService*>(game().findService("UserInputService"));
    return !uis || uis->mouseIconEnabled();
}

bool ScriptContext::cameraRelativeRotation() const
{
    const InstancePtr& settings = runtime_->shared().userSettings;
    const auto* game = settings
                           ? dynamic_cast<const UserGameSettings*>(settings->findFirstChildOfClass("UserGameSettings"))
                           : nullptr;
    return game && game->rotationType() == Enum::RotationType::CameraRelative;
}

bool ScriptContext::defaultLoadingScreenRemoved() const
{
    return runtime_->shared().defaultLoadingScreenRemoved;
}

void ScriptContext::setAnimationSystem(avatar::AnimationSystem* system)
{
    lua::setAnimationSystem(runtime_->shared(), system);
}

void ScriptContext::setWeb(std::shared_ptr<const assets::RobloxWeb> web)
{
    runtime_->shared().web = std::move(web);
}

std::expected<std::string, std::string> ScriptContext::compile(std::string_view source, std::string_view chunkName,
                                                               int debugLevel)
{
    // Lookups through these can change at run time, so they mustn't be
    // resolved once at load like library imports (workspace.Part, game.X).
    static constexpr const char* mutableGlobals[]
        = {"Game", "Workspace", "game", "plugin", "script", "shared", "workspace", "_G", nullptr};
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = debugLevel;
    options.mutableGlobals = mutableGlobals;
    std::string bytecode = Luau::compile(std::string(source), options);
    // Failed compiles give a zero byte followed by ":line: message".
    if (!bytecode.empty() && bytecode[0] == '\0')
        return std::unexpected(std::format("{}{}", chunkName, std::string_view(bytecode).substr(1)));
    return bytecode;
}

std::expected<void, ScriptError> ScriptContext::run(std::string_view source, std::string_view chunkName,
                                                    Instance* script, RunAccess access)
{
    auto bytecode = compile(source, chunkName, access == RunAccess::Full ? 2 : 1);
    if (!bytecode)
        return std::unexpected(ScriptError{std::move(bytecode.error()), {}});
    return runtime_->runChunk(*bytecode, chunkName, script, access);
}

namespace {

// The Escape menu's Luau tab's "--!server" line: the code after it runs in
// the server's context. A comment in Luau already, but it is trimmed away so
// the chunk's first line is the code's own.
constexpr std::string_view serverDirective = "--!server";

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

} // namespace

std::expected<void, ScriptError> ScriptContext::runConsole(std::string_view code, ScriptContext* server)
{
    std::string_view source = code;
    const std::size_t line = code.find('\n');
    std::string_view first = trim(code.substr(0, line));
    if (first != serverDirective)
        return run(code, "Console", nullptr, RunAccess::Full);
    if (!server)
        return std::unexpected(ScriptError{"There is no server here to run on.", {}});
    source = trim(code.substr(line == std::string_view::npos ? code.size() : line + 1));
    return server->run(source, "Console", nullptr, RunAccess::Full);
}

namespace {

// Undoes the client's opcode encoding (op * 227 mod 256) in every
// instruction of every function, or with `encode` does it; AUX words stay as
// they are. From version 12 each function carries its size, so the walk skips
// what follows its code; before that (the client's own models are version 9)
// it reads the rest of each function as luau_load does.
std::optional<std::string> recodeOpcodes(std::string bytecode, bool encode)
{
    auto* d = reinterpret_cast<std::uint8_t*>(bytecode.data());
    const std::size_t size = bytecode.size();
    std::size_t o = 0;
    bool bad = false;
    auto u8 = [&]() -> std::uint8_t {
        if (o >= size) {
            bad = true;
            return 0;
        }
        return d[o++];
    };
    auto var = [&]() {
        std::uint64_t r = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const std::uint8_t b = u8();
            r |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                break;
        }
        return r;
    };
    auto skip = [&](std::uint64_t bytes) {
        if (bytes > size - std::min(o, size))
            bad = true;
        else
            o += static_cast<std::size_t>(bytes);
    };
    const std::uint8_t version = u8();
    if (version < LBC_VERSION_MIN || version > LBC_VERSION_MAX)
        return std::nullopt;
    const std::uint8_t types = version >= 4 ? u8() : 0;
    for (std::uint64_t n = var(); n > 0 && !bad; --n)
        skip(var());
    if (types == 3)
        for (std::uint8_t index = u8(); index != 0 && !bad; index = u8())
            var();
    for (std::uint64_t protos = var(); protos > 0 && !bad; --protos) {
        const std::uint64_t protoSize = version >= 12 ? var() : 0;
        const std::size_t start = o;
        skip(4); // maxstacksize, numparams, nups, is_vararg
        if (version >= 4) {
            skip(1);     // flags
            skip(var()); // type info
        }
        const std::uint64_t sizecode = var();
        if (bad || sizecode > (size - o) / 4)
            return std::nullopt;
        for (std::uint64_t j = 0; j < sizecode;) {
            std::uint8_t& op = d[o + j * 4];
            const auto plain = static_cast<std::uint8_t>(encode ? op : op * 203); // 203 = 227^-1 mod 256
            if (plain >= LOP__COUNT)
                return std::nullopt;
            op = encode ? static_cast<std::uint8_t>(op * 227) : plain;
            j += static_cast<std::uint64_t>(Luau::getOpLength(LuauOpcode(plain)));
        }
        o += static_cast<std::size_t>(sizecode * 4);
        if (version >= 12) {
            o = start;
            skip(protoSize);
            continue;
        }
        for (std::uint64_t k = var(); k > 0 && !bad; --k) {
            switch (u8()) {
            case LBC_CONSTANT_NIL:
                break;
            case LBC_CONSTANT_BOOLEAN:
                skip(1);
                break;
            case LBC_CONSTANT_NUMBER:
                skip(8);
                break;
            case LBC_CONSTANT_STRING:
            case LBC_CONSTANT_CLOSURE:
                var();
                break;
            case LBC_CONSTANT_IMPORT:
                skip(4);
                break;
            case LBC_CONSTANT_TABLE:
                for (std::uint64_t n = var(); n > 0 && !bad; --n)
                    var();
                break;
            case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
                for (std::uint64_t n = var(); n > 0 && !bad; --n) {
                    var();   // key
                    skip(4); // constant index
                }
                break;
            case LBC_CONSTANT_VECTOR:
                skip(16);
                break;
            case LBC_CONSTANT_VECTORD:
                skip(32);
                break;
            case LBC_CONSTANT_INTEGER:
                skip(1); // sign
                var();
                break;
            case LBC_CONSTANT_CLASS_SHAPE: {
                var(); // class name
                const std::uint64_t properties = var(), methods = var();
                for (std::uint64_t n = properties + methods; n > 0 && !bad; --n)
                    var();
                break;
            }
            default:
                return std::nullopt;
            }
        }
        for (std::uint64_t n = var(); n > 0 && !bad; --n) // child functions
            var();
        var();           // linedefined
        var();           // debugname
        if (u8() != 0) { // line info: a byte per instruction, an int per interval
            const std::uint8_t gapLog2 = u8();
            if (gapLog2 > 31 || sizecode == 0)
                return std::nullopt;
            skip(sizecode);
            skip((((sizecode - 1) >> gapLog2) + 1) * 4);
        }
        if (u8() != 0) { // debug info: locals, then upvalue names
            for (std::uint64_t n = var(); n > 0 && !bad; --n) {
                var();   // name
                var();   // startpc
                var();   // endpc
                skip(1); // register
            }
            for (std::uint64_t n = var(); n > 0 && !bad; --n)
                var();
        }
        if (version >= 11)
            for (std::uint64_t n = var(); n > 0 && !bad; --n) { // feedback slots
                skip(1);
                var();
            }
    }
    if (bad || o > size)
        return std::nullopt;
    return bytecode;
}

std::optional<std::string> unscrambleOpcodes(std::string bytecode)
{
    return recodeOpcodes(std::move(bytecode), false);
}

// Encoded bytecode from version 11 on, written as the oldest version that has
// everything it uses (from 4 to 10, which all lay a function out alike):
// without the feedback slots (11), the function sizes and costs (12) and with
// CALLFB as CALL and a NOP, FASTPCALL as the NOP the VM treats it as when it
// has no fast path. What getscriptbytecode hands decompilers [sUNC: its
// version byte is from 3 to 10, "Didn't return the correct byte"]. Nothing
// for code that needs version 11 or later (CMPPROTO, the class opcodes,
// double vector constants); older bytecode stays as it is.
std::optional<std::string> olderBytecode(std::string bytecode)
{
    auto* d = reinterpret_cast<std::uint8_t*>(bytecode.data());
    const std::size_t size = bytecode.size();
    std::size_t o = 0;
    bool bad = false;
    auto u8 = [&]() -> std::uint8_t {
        if (o >= size) {
            bad = true;
            return 0;
        }
        return d[o++];
    };
    auto var = [&]() {
        std::uint64_t r = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const std::uint8_t b = u8();
            r |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                break;
        }
        return r;
    };
    auto skip = [&](std::uint64_t bytes) {
        if (bytes > size - std::min(o, size))
            bad = true;
        else
            o += static_cast<std::size_t>(bytes);
    };
    // What is kept is copied a stretch at a time, up to what is left out.
    std::string out;
    out.reserve(size);
    std::size_t kept = 0;
    auto keepTo = [&](std::size_t end) {
        out.append(bytecode, kept, end - kept);
        kept = end;
    };
    auto encode = [](LuauOpcode op) { return static_cast<std::uint8_t>(op * 227); };

    const std::uint8_t version = u8();
    if (version < 11)
        return bytecode;
    if (version > LBC_VERSION_MAX)
        return std::nullopt;
    const std::uint8_t types = u8();
    int needs = types == 3 ? 5 : 4; // userdata type names came with 5
    for (std::uint64_t n = var(); n > 0 && !bad; --n)
        skip(var());
    if (types == 3)
        for (std::uint8_t index = u8(); index != 0 && !bad; index = u8())
            var();
    for (std::uint64_t protos = var(); protos > 0 && !bad; --protos) {
        keepTo(o);
        const std::uint64_t protoSize = version >= 12 ? var() : 0;
        kept = o;
        const std::size_t start = o;
        skip(4); // maxstacksize, numparams, nups, is_vararg
        skip(1);     // flags
        skip(var()); // type info
        const std::uint64_t sizecode = var();
        if (bad || sizecode > (size - o) / 4)
            return std::nullopt;
        for (std::uint64_t j = 0; j < sizecode;) {
            std::uint8_t* insn = d + o + j * 4;
            const auto op = static_cast<LuauOpcode>(static_cast<std::uint8_t>(insn[0] * 203));
            if (op == LOP_CALLFB) {
                if (j + 1 >= sizecode)
                    return std::nullopt;
                insn[0] = encode(LOP_CALL);
                std::fill_n(insn + 4, 4, std::uint8_t{0}); // the feedback slot, now a NOP
            } else if (op == LOP_FASTPCALL) {
                std::fill_n(insn, 4, std::uint8_t{0});
            } else if (op >= LOP_CMPPROTO) {
                return std::nullopt;
            } else if (op == LOP_SUBRK || op == LOP_DIVRK) {
                needs = std::max(needs, 5);
            } else if (op == LOP_FASTCALL3) {
                needs = std::max(needs, 6);
            } else if (op == LOP_GETUDATAKS || op == LOP_SETUDATAKS || op == LOP_NAMECALLUDATA) {
                needs = std::max(needs, 9);
            } else if (op == LOP_NEWCLASSMEMBER) {
                needs = std::max(needs, 10);
            }
            j += static_cast<std::uint64_t>(Luau::getOpLength(op));
        }
        o += static_cast<std::size_t>(sizecode * 4);
        for (std::uint64_t k = var(); k > 0 && !bad; --k) {
            switch (u8()) {
            case LBC_CONSTANT_NIL:
                break;
            case LBC_CONSTANT_BOOLEAN:
                skip(1);
                break;
            case LBC_CONSTANT_NUMBER:
                skip(8);
                break;
            case LBC_CONSTANT_STRING:
            case LBC_CONSTANT_CLOSURE:
                var();
                break;
            case LBC_CONSTANT_IMPORT:
                skip(4);
                break;
            case LBC_CONSTANT_TABLE:
                for (std::uint64_t n = var(); n > 0 && !bad; --n)
                    var();
                break;
            case LBC_CONSTANT_VECTOR:
                needs = std::max(needs, 5);
                skip(16);
                break;
            case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
                needs = std::max(needs, 7);
                for (std::uint64_t n = var(); n > 0 && !bad; --n) {
                    var();   // key
                    skip(4); // constant index
                }
                break;
            case LBC_CONSTANT_INTEGER:
                needs = std::max(needs, 8);
                skip(1); // sign
                var();
                break;
            case LBC_CONSTANT_CLASS_SHAPE: {
                needs = std::max(needs, 10);
                var(); // class name
                const std::uint64_t properties = var(), methods = var();
                for (std::uint64_t n = properties + methods; n > 0 && !bad; --n)
                    var();
                break;
            }
            default: // a double vector among them
                return std::nullopt;
            }
        }
        for (std::uint64_t n = var(); n > 0 && !bad; --n) // child functions
            var();
        var();           // linedefined
        var();           // debugname
        if (u8() != 0) { // line info
            const std::uint8_t gapLog2 = u8();
            if (gapLog2 > 31 || sizecode == 0)
                return std::nullopt;
            skip(sizecode);
            skip((((sizecode - 1) >> gapLog2) + 1) * 4);
        }
        if (u8() != 0) { // debug info
            for (std::uint64_t n = var(); n > 0 && !bad; --n) {
                var();
                var();
                var();
                skip(1);
            }
            for (std::uint64_t n = var(); n > 0 && !bad; --n)
                var();
        }
        if (bad)
            return std::nullopt;
        keepTo(o); // the rest is what the older versions don't have: feedback slots and the cost
        if (version >= 12) {
            if (protoSize > size - start)
                return std::nullopt;
            o = start + static_cast<std::size_t>(protoSize);
        } else {
            for (std::uint64_t n = var(); n > 0 && !bad; --n) { // feedback slots
                skip(1);
                var();
            }
        }
        kept = o;
    }
    var(); // the main function
    if (bad || o > size)
        return std::nullopt;
    keepTo(size);
    out[0] = static_cast<char>(needs);
    return out;
}

} // namespace

namespace lua {

std::expected<std::string, std::string> Runtime::codeOf(std::string_view source, std::string_view chunkName) const
{
    if (shared_->networkBytecode)
        if (auto scrambled = shared_->networkBytecode(source)) {
            auto plain = unscrambleOpcodes(std::move(*scrambled));
            if (!plain)
                return std::unexpected(std::format("{}: the script's bytecode did not walk", chunkName));
            return std::move(*plain);
        }
    return ScriptContext::compile(source, chunkName);
}

std::expected<std::string, std::string> Runtime::sentCodeOf(std::string_view source, std::string_view chunkName) const
{
    std::optional<std::string> encoded;
    if (shared_->networkBytecode)
        encoded = shared_->networkBytecode(source);
    if (!encoded) {
        auto compiled = ScriptContext::compile(source, chunkName);
        if (!compiled)
            return compiled;
        encoded = recodeOpcodes(std::move(*compiled), true);
        if (!encoded)
            return std::unexpected(std::format("{}: the script's bytecode did not walk", chunkName));
    }
    // As the older format when it can be, which is what decompilers read.
    if (auto older = olderBytecode(*encoded))
        return std::move(*older);
    return std::move(*encoded);
}

} // namespace lua

bool ScriptContext::setPlatform(std::string_view enumName)
{
    const lua::api::EnumData* platforms = lua::api::findEnum("Platform");
    if (!platforms)
        return false;
    for (const lua::api::EnumItemData& item : lua::api::itemsOf(*platforms))
        if (item.name == enumName) {
            runtime_->shared().platform = std::string(enumName);
            return true;
        }
    return false;
}

void ScriptContext::setNetworkBytecode(std::function<std::optional<std::string>(std::string_view source)> decode)
{
    runtime_->shared().networkBytecode = std::move(decode);
}

std::optional<std::size_t> ScriptContext::loadClientSettings(std::string_view json)
{
    return lua::loadClientSettings(json);
}

void ScriptContext::setAppStorageItem(std::string_view key, std::string_view value)
{
    lua::setAppStorageItem(key, value);
}

void ScriptContext::setMemStorageItem(std::string_view key, std::string_view value)
{
    lua::setMemStorageItem(*runtime_, key, value);
}

void ScriptContext::setLaunchHandler(std::function<void(const LaunchRequest&)> handler)
{
    runtime_->shared().launchHandler = std::move(handler);
}

void ScriptContext::setTextMeasure(TextMeasure measure)
{
    runtime_->shared().textMeasure = std::move(measure);
}

void ScriptContext::setGuiHitTest(GuiHitTest hitTest)
{
    runtime_->shared().guiHitTest = std::move(hitTest);
}

void ScriptContext::setGuiFocus(GuiFocus focus)
{
    runtime_->shared().guiFocus = std::move(focus);
}

void ScriptContext::setGuiLayoutQuery(std::function<void(Instance*)> query)
{
    runtime_->shared().guiLayoutQuery = std::move(query);
}

void ScriptContext::setDataUpdateInProgress(std::function<bool()> updating)
{
    runtime_->shared().dataUpdateInProgress = std::move(updating);
}

void ScriptContext::setGameInvitePromptHandler(GameInvitePromptHandler handler)
{
    runtime_->shared().gameInvitePrompt = std::move(handler);
}

std::function<void(UIPageLayout&, int)> ScriptContext::guiPageStepper() const
{
    return [weak = std::weak_ptr<lua::Shared>(runtime_->sharedPtr())](UIPageLayout& layout, int step) {
        auto shared = weak.lock();
        if (!shared)
            return;
        const auto pages = layout.pages();
        if (pages.empty())
            return;
        auto index = static_cast<int>(std::ranges::find(pages, layout.currentPage()) - pages.begin()) + step;
        const auto count = static_cast<int>(pages.size());
        index = layout.circular() ? ((index % count) + count) % count : std::clamp(index, 0, count - 1);
        if (pages[static_cast<std::size_t>(index)] != layout.currentPage())
            lua::jumpToPage(*shared, layout, *pages[static_cast<std::size_t>(index)]);
    };
}

void ScriptContext::stepGuiPage(UIPageLayout& layout, int step)
{
    guiPageStepper()(layout, step);
}

void ScriptContext::setImageLoadedChecker(std::function<bool(std::string_view)> checker)
{
    runtime_->shared().imageLoaded = std::move(checker);
    if (!runtime_->shared().imageLoaded)
        runtime_->shared().imageStates.clear();
}

void ScriptContext::setCoreScriptAccess()
{
    runtime_->setAccess(runtime_->main(),
                        lua::ThreadAccess{.identity = static_cast<int>(lua::api::Identity::Full),
                                          .capabilities = lua::api::capabilitiesOf(lua::api::Identity::Full)});
}

void ScriptContext::setRemoteSink(RemoteSink sink)
{
    runtime_->shared().remoteSink = std::move(sink);
}

void ScriptContext::setEventShape(EventShape shape)
{
    runtime_->shared().eventShape = std::move(shape);
}

void ScriptContext::setStreamAroundHook(StreamAroundHook hook)
{
    runtime_->shared().streamAroundHook = std::move(hook);
}

void ScriptContext::setActive(std::function<bool()> hook)
{
    runtime_->shared().active = std::move(hook);
}

void ScriptContext::setClipboard(std::function<void(std::string_view)> hook)
{
    runtime_->shared().clipboard = std::move(hook);
}

void ScriptContext::setNetworkPing(std::function<double()> hook)
{
    runtime_->shared().networkPing = std::move(hook);
}

void ScriptContext::setFrameCap(std::function<void(int)> hook)
{
    runtime_->shared().frameCap = std::move(hook);
}

void ScriptContext::setInput(std::function<void(const HostInput&)> hook)
{
    runtime_->shared().input = std::move(hook);
}

void ScriptContext::setPlaytest(PlaytestHost host)
{
    runtime_->shared().playtest = std::move(host);
}

void ScriptContext::runTeleportCode()
{
    lua::Shared& shared = runtime_->shared();
    if (shared.teleportCode.empty())
        return;
    // Taken first: a run that errors must not leave it for the next teleport.
    const std::string code = std::exchange(shared.teleportCode, {});
    // What it printed goes where everything else does; an error in it is the
    // console's own, which runConsole has already reported.
    (void)runConsole(code);
}

void ScriptContext::streamAroundDone(std::uint64_t request)
{
    auto& waiting = runtime_->shared().streamArounds;
    auto it = waiting.find(request);
    if (it == waiting.end())
        return;
    const auto w = it->second;
    waiting.erase(it);
    w.caller->defer([w] { w.caller->wake(w.thread, w.token, [](lua_State*) { return 0; }); });
}

bool ScriptContext::receiveRemote(Instance& remote, std::string_view event, std::span<const RemoteValue> arguments)
{
    return lua::receiveNetworkRemote(runtime_->shared(), remote, event, arguments);
}

void ScriptContext::setChatSink(std::function<void(const ChatLine&)> sink)
{
    lua::setChatSink(runtime_->shared(), std::move(sink));
}

std::expected<std::string, std::string> ScriptContext::sendChat(std::string_view text, std::string_view channel)
{
    return lua::sendChat(*runtime_, text, channel);
}

PresetChatAccess ScriptContext::presetChatAccess() const
{
    return lua::presetChatAccess(runtime_->shared());
}

void ScriptContext::requestWhisperChannel(std::int64_t target, std::function<void(std::expected<std::string, std::string>)> done)
{
    lua::requestWhisper(*runtime_, target, std::move(done));
}

std::expected<std::string, std::string> ScriptContext::sendPresetChat(const ChatPreset& preset)
{
    return lua::sendPresetChat(*runtime_, preset);
}

std::expected<std::vector<double>, ScriptError> ScriptContext::callBytecode(std::string bytecode,
                                                                            std::string_view chunkName,
                                                                            std::span<const double> args,
                                                                            bool scrambledOpcodes)
{
    if (scrambledOpcodes) {
        auto plain = unscrambleOpcodes(std::move(bytecode));
        if (!plain)
            return std::unexpected(ScriptError{"bytecode with scrambled opcodes did not walk", {}});
        bytecode = std::move(*plain);
    }
    return runtime_->callChunk(bytecode, chunkName, args);
}

std::expected<void, ScriptError> ScriptContext::runScript(Instance& script)
{
    return runtime_->startScript(script);
}

void ScriptContext::stopScript(const Instance& script)
{
    runtime_->stopScript(script);
}

double ScriptContext::time() const
{
    return runtime_->now();
}

bool ScriptContext::idle() const
{
    return runtime_->idle();
}

ScriptRole ScriptContext::role() const
{
    return runtime_->role();
}

DataModel& ScriptContext::game() const
{
    return runtime_->game();
}

lua_State* ScriptContext::state() const
{
    return runtime_->main();
}

} // namespace lunar
