// Full access for the Escape menu's Luau tab, the stand-in for the F9 console:
// the globals an executor offers, in the globals of the one thread that runs
// the code typed there. No game script sees them, and their capabilities are
// the ones an identity of 8 has, while a LocalScript or Script runs as 2 with
// none of them (see InstanceApi.cpp's checkCapability).
//
// What the executors' own naming says about each of them is not in the
// client's source (there is none: these are third-party tools that patch it),
// so the names and shapes here follow the Lua side those tools document.
// GUESS: the parts marked as such below, and everything about the identities
// between None and Full.

#include "Console.hpp"
#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Services.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/RemoteValue.hpp>

#include <lobject.h>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

// What identifyexecutor() reports. Lunarstone has no version of its own in the
// binary, so this is the project's version (CMakeLists.txt).
constexpr std::string_view executor = "Lunarstone";
constexpr std::string_view executorVersion = LUNAR_VERSION;

Player* localPlayer(lua_State* L)
{
    DataModel& game = Runtime::of(L).game();
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

// A table of instances, as the ones below all answer with.
int pushInstanceList(lua_State* L, const std::vector<InstancePtr>& instances)
{
    lua_createtable(L, static_cast<int>(instances.size()), 0);
    int i = 0;
    for (const InstancePtr& instance : instances) {
        pushInstance(L, instance.get());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// --- Environments ------------------------------------------------------------

// The executor's environments, as the clients lay them out (docs.sunc.su,
// Environment): getgenv()'s table holds the console's functions and is
// shared by every console run; its metatable falls back on getrenv()'s, the
// game's globals. Each run's globals are a table of its own over getgenv()'s,
// which is where the run's own globals go: `getfenv().x = 1` is that run's,
// `getgenv().x = 1` every run's, and `getrenv().warn = 1` the game's, which
// the console then reads too.

void fillConsole(lua_State* thread);

// getgenv()'s table, made on first use and held by the Runtime, so it
// outlives the thread it was made in. The console's functions go into it then,
// through the thread's globals, which it is while they do.
void pushGenv(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    if (rt.consoleGlobals() != LUA_NOREF) {
        lua_getref(L, rt.consoleGlobals());
        return;
    }
    lua_checkstack(L, 4);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(rt.main(), LUA_GLOBALSINDEX);
    lua_xmove(rt.main(), L, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    rt.setConsoleGlobals(lua_ref(L, -1));
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_pushvalue(L, -2);
    lua_replace(L, LUA_GLOBALSINDEX);
    fillConsole(L);
    lua_replace(L, LUA_GLOBALSINDEX);
}

int getgenv(lua_State* L)
{
    pushGenv(L);
    return 1;
}

// getrenv(): the environment the game's own scripts run in -- this VM's shared
// globals, which each script's own globals fall back on. Writable, as the
// executors hand it out: what a script has already loaded keeps the globals
// it found then (the VM resolves a safe environment's imports at load), and
// what reads them afterwards, the console included, sees the change. What a
// running script has of its own is getsenv(script)'s.
int getrenv(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    lua_pushvalue(rt.main(), LUA_GLOBALSINDEX);
    lua_xmove(rt.main(), L, 1);
    lua_setreadonly(L, -1, false);
    return 1;
}

// getreg(): the registry the VM keeps its own references in.
int getreg(lua_State* L)
{
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
}

// --- Tables ------------------------------------------------------------------

// getrawmetatable(value): the metatable itself, whatever __metatable says.
int getrawmetatable(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1)) // pushes nothing when there is none
        lua_pushnil(L);
    return 1;
}

// setrawmetatable(value, metatable): the same past __metatable, handing the
// value back.
int setrawmetatable(lua_State* L)
{
    luaL_checkany(L, 1);
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    lua_settop(L, 1);
    return 1;
}

// setreadonly(table, true) locks a table against changes; isreadonly says
// whether it is.
int setreadonly(lua_State* L)
{
    luaL_checkany(L, 1);
    lua_setreadonly(L, 1, lua_toboolean(L, 2) != 0);
    return 0;
}

// makereadonly(table) and makewriteable(table): the same, the one way each.
template <bool ReadOnly>
int makeReadOnly(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_setreadonly(L, 1, ReadOnly);
    return 0;
}

int isreadonly(lua_State* L)
{
    luaL_checkany(L, 1);
    lua_pushboolean(L, lua_getreadonly(L, 1) != 0);
    return 1;
}

// --- Identity ----------------------------------------------------------------

// getidentity(): what this thread may reach, as a level: 2 for a LocalScript or
// Script, 8 for the console.
int getidentity(lua_State* L)
{
    lua_pushinteger(L, Runtime::of(L).accessOf(L).identity);
    return 1;
}

// setidentity(level): what this thread may reach from here on. The capabilities
// go with the level, so a lower one takes them away again. Any number is
// taken, as the executors take it [sUNC sets inf]: one that isn't a level of
// the client's (0 to 8) has none of them, and reads back as the integer it
// became (the lowest int when it was none).
int setidentity(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    ThreadAccess access = rt.accessOf(L);
    const double number = luaL_checknumber(L, 1);
    constexpr double lowest = std::numeric_limits<int>::min(), highest = std::numeric_limits<int>::max();
    const int level = number >= lowest && number <= highest ? static_cast<int>(number) : std::numeric_limits<int>::min();
    access.identity = level;
    access.capabilities = level >= 0 && level <= static_cast<int>(api::Identity::Full)
                              ? api::capabilitiesOf(static_cast<api::Identity>(level))
                              : 0;
    rt.setAccess(L, access);
    return 0;
}

// --- The game's objects ------------------------------------------------------

// getinstances(): every instance there is: the game, its descendants and the
// services under it, then the ones out of it that a script has had (with
// what's under them), as getnilinstances finds them.
int getinstances(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    DataModel& game = rt.game();
    std::vector<InstancePtr> instances;
    instances.push_back(game.shared_from_this());
    const std::vector<InstancePtr> descendants = game.descendants();
    instances.insert(instances.end(), descendants.begin(), descendants.end());
    for (const std::weak_ptr<Instance>& weak : rt.shared().state->knownInstances) {
        InstancePtr instance = weak.lock();
        if (!instance || instance->parent() || instance->destroyed() || instance.get() == &game)
            continue;
        instances.push_back(instance);
        const std::vector<InstancePtr> under = instance->descendants();
        instances.insert(instances.end(), under.begin(), under.end());
    }
    return pushInstanceList(L, instances);
}

// getnilinstances(): the instances with no parent that are still alive, which
// scripts leak by every Instance.new they don't parent. Only what the VM has
// handed a script is knowable: the engine keeps no list of its own.
int getnilinstances(lua_State* L)
{
    std::vector<InstancePtr> loose;
    for (const std::weak_ptr<Instance>& weak : Runtime::of(L).shared().state->knownInstances) {
        InstancePtr instance = weak.lock();
        if (instance && !instance->parent() && !instance->destroyed())
            loose.push_back(std::move(instance));
    }
    return pushInstanceList(L, loose);
}

// getscripts(): every script the client could run, whether it is running or
// not: LocalScripts, ModuleScripts and the Scripts whose RunContext is Client,
// in the game or out of it (one a script made and never parented) [sUNC]. A
// server's Script is left out, as the executors leave it: there is nothing of
// it a client runs (and over the network, no code either); so are the core
// scripts, the ones in CorePackages and CoreGui [sUNC: "Shouldn't return core
// scripts by default"].
int getscripts(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    DataModel& game = rt.game();
    std::vector<InstancePtr> candidates = game.descendants();
    for (const std::weak_ptr<Instance>& weak : rt.shared().state->knownInstances) {
        InstancePtr instance = weak.lock();
        if (!instance || instance->parent() || instance->destroyed() || instance.get() == &game)
            continue;
        candidates.push_back(instance);
        const std::vector<InstancePtr> under = instance->descendants();
        candidates.insert(candidates.end(), under.begin(), under.end());
    }
    const Instance* corePackages = game.findFirstChildOfClass("CorePackages");
    const Instance* coreGui = game.findFirstChildOfClass("CoreGui");
    std::vector<InstancePtr> scripts;
    for (InstancePtr& instance : candidates) {
        if (!isA(*instance, "LuaSourceContainer"))
            continue;
        if (const auto* script = dynamic_cast<const Script*>(instance.get());
            script && !isA(*instance, "LocalScript") && script->runContext() != Enum::RunContext::Client)
            continue;
        if ((corePackages && instance->isDescendantOf(corePackages)) || (coreGui && instance->isDescendantOf(coreGui)))
            continue;
        scripts.push_back(std::move(instance));
    }
    return pushInstanceList(L, scripts);
}

// getloadedmodules(): the ModuleScripts whose require() has finished, with the
// values they returned already cached.
int getloadedmodules(lua_State* L)
{
    return pushInstanceList(L, Runtime::of(L).loadedModules());
}

// getsenv(script): the globals a running script writes to, so console code can
// read and change them. Nil for a script that isn't running here.
int getsenv(lua_State* L)
{
    const int env = Runtime::of(L).environmentOf(checkInstance(L, 1));
    if (env == LUA_NOREF)
        return 0;
    lua_getref(L, env);
    return 1;
}

// getcallingscript(): the script the calling thread runs for: a game
// script's in a hook the console put on what it calls, nil on the console's
// own threads (ThreadAccess::script).
int getcallingscript(lua_State* L)
{
    InstancePtr script = Runtime::of(L).accessOf(L).script.lock();
    return returnInstance(L, script.get());
}

// getconnections(signal): the connections the VM holds on a signal, each a
// table with the callback, whether it runs, and Fire/Disconnect/Disable/Enable
// over it. Disabling one leaves it connected and stops its handler, as the
// executors' does.
int getconnections(lua_State* L)
{
    return pushConnections(L, 1);
}

// getrendersteppedlist(): what RunService:BindToRenderStep has bound, in the
// order it runs, each {Name, Priority, Function, Thread} where Thread is the
// thread that bound it.
int getrendersteppedlist(lua_State* L)
{
    return Runtime::of(L).pushRenderBindings(L);
}

// getsignalarguments(signal): the types of what the signal passes its
// handlers, as the engine names them ({"BasePart"} for Touched).
int getsignalarguments(lua_State* L)
{
    return pushSignalArguments(L, 1, false);
}

// getsignalargumentsinfo(signal): the same with the parameters' names, each
// {Name = "otherPart", Type = "BasePart"}.
int getsignalargumentsinfo(lua_State* L)
{
    return pushSignalArguments(L, 1, true);
}

// Signal.new(): a signal of the script's own, an RBXScriptSignal like the
// engine's (Connect, Once, Wait, and connections that Disconnect) with a
// Fire(...) that runs its connections now and wakes its waiting threads. It
// is an event of a Folder no one else can reach, which the signal keeps.
int signalNew(lua_State* L)
{
    const InstancePtr holder = ClassRegistry::create("Folder");
    pushHubSignal(L, *holder, "Event", true);
    return 1;
}

// --- Actors -------------------------------------------------------------------
//
// Every Actor runs on the one VM here (SharedTable.cpp says why), so "its
// VM" is the threads that run for it: its scripts', and run_on_actor's.
// What crosses into one is copied as Actor:SendMessage copies it, tables
// deep and instances as themselves.

// getactors(): the Actors whose scripts are running.
int getactors(lua_State* L)
{
    const std::vector<InstancePtr> actors = Runtime::of(L).runningActors();
    lua_createtable(L, static_cast<int>(actors.size()), 0);
    int i = 0;
    for (const InstancePtr& actor : actors) {
        pushInstance(L, actor.get());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// isparallel(): whether the thread is in the parallel phase, between an
// actor's task.desynchronize and its task.synchronize.
int isparallel(lua_State* L)
{
    lua_pushboolean(L, Runtime::of(L).accessOf(L).parallel);
    return 1;
}

// run_on_actor(actor, code, ...): the code runs for the actor, in a thread of
// the console's that is one of the actor's (task.desynchronize works in it),
// with copies of the arguments. Started as task.spawn starts a function; what
// it raises goes to the output.
int run_on_actor(lua_State* L)
{
    Instance& actor = checkInstance(L, 1);
    if (!actor.isA("Actor"))
        luaL_argerror(L, 1, "is not an Actor");
    const std::string_view source = checkString(L, 2);
    const PackedArgs arguments = packArgs(L, 3, lua_gettop(L));
    auto bytecode = ScriptContext::compile(source, "run_on_actor", 2);
    if (!bytecode)
        raise(L, bytecode.error());
    Runtime& rt = Runtime::of(L);
    lua_State* thread = lua_newthread(L); // the console's globals and access
    ThreadAccess access = rt.accessOf(thread);
    access.actor = actor.weak_from_this();
    access.parallel = false;
    rt.setAccess(thread, access);
    if (luau_load(thread, "=run_on_actor", bytecode->data(), bytecode->size(), 0) != 0)
        raise(L, lua_tostring(thread, -1));
    const int nargs = unpackArgs(thread, arguments);
    if (auto error = rt.resume(thread, nargs, "run_on_actor"))
        rt.report(*error);
    lua_pop(L, 1);
    return 0;
}

// create_comm_channel(): a channel between the console and the actors: its
// id, a whole number, and a BindableEvent that is the channel, whose Fire
// copies what it carries as one across actors does. get_comm_channel(id):
// the channel, or nil when there is none by that id.
constexpr const char* commChannelsKey = "lunarstone.commchannels";

void pushCommChannels(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, commChannelsKey);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, commChannelsKey);
}

int create_comm_channel(lua_State* L)
{
    pushCommChannels(L);
    const int id = lua_objlen(L, -1) + 1;
    const InstancePtr channel = ClassRegistry::create("BindableEvent");
    pushInstance(L, channel.get());
    lua_rawseti(L, -2, id);
    lua_pushinteger(L, id);
    pushInstance(L, channel.get());
    return 2;
}

int get_comm_channel(lua_State* L)
{
    const double id = luaL_checknumber(L, 1);
    pushCommChannels(L);
    if (id >= 1 && id <= lua_objlen(L, -1) && id == static_cast<double>(static_cast<int>(id)))
        lua_rawgeti(L, -1, static_cast<int>(id));
    else
        lua_pushnil(L);
    return 1;
}

// --- Firing events -----------------------------------------------------------

// firesignal(signal, ...): every Luau connection on the signal runs now with
// the arguments, whatever Workspace.SignalBehavior says.
int firesignal(lua_State* L)
{
    return fireConnections(L, 1);
}

// replicatesignal(signal, ...): the signal's event goes to the server with the
// arguments, as the engine replicates the events it does (a ClickDetector's
// MouseActionReplicated, a GuiObject's MouseWheelForward). They must be what
// the server's schema has the event take, in number and type, or it raises
// [sUNC docs: "If incorrect arguments are passed, an error must be thrown"].
int replicatesignal(lua_State* L)
{
    return replicateSignal(L, 1);
}

// fireproximityprompt(prompt): the prompt a player would trigger, with the local
// player as the one who did. The service's own PromptTriggered goes with it,
// as the engine sends both, and a real server hears what a held key tells it:
// the prompt's ...ActionReplicated events, the hold's around the trigger when
// the prompt has one [sUNC: "Failed to fire a proximityprompt on the server"].
int fireproximityprompt(lua_State* L)
{
    Instance& prompt = checkInstance(L, 1);
    Player* who = localPlayer(L);
    const std::array args{Value(who ? who->shared_from_this() : nullptr)};
    prompt.fireEvent("Triggered", args);
    Runtime& rt = Runtime::of(L);
    DataModel& game = rt.game();
    if (auto* service = game.findFirstChildOfClass("ProximityPromptService")) {
        const std::array serviceArgs{Value(prompt.shared_from_this()), args[0]};
        service->fireEvent("PromptTriggered", serviceArgs);
    }
    if (const RemoteSink& sink = rt.shared().remoteSink; sink && who) {
        const auto* proximity = dynamic_cast<const ProximityPrompt*>(&prompt);
        const bool held = proximity && proximity->holdDuration() > 0.0f;
        std::vector<std::string_view> events;
        if (held)
            events.push_back("ButtonHoldBeganActionReplicated");
        events.push_back("TriggeredActionReplicated");
        if (held)
            events.push_back("ButtonHoldEndedActionReplicated");
        events.push_back("TriggerEndedActionReplicated");
        for (std::string_view event : events)
            sink(prompt, event, {remoteValue(args[0])});
    }
    return 0;
}

// fireclickdetector(detector, distance, event): what a mouse would do to it:
// the event (MouseClick unless named: RightMouseClick, MouseHoverEnter or
// MouseHoverLeave) on the detector, with the local player as the one who did
// it. The class is one the engine doesn't model, so this is the event a game
// listens to; the distance is what the mouse is in reach of, all of it here.
int fireclickdetector(lua_State* L)
{
    Instance& detector = checkInstance(L, 1);
    luaL_optnumber(L, 2, 0);
    const std::string_view event = luaL_optstring(L, 3, "MouseClick");
    static constexpr std::string_view events[]
        = {"MouseClick", "RightMouseClick", "MouseHoverEnter", "MouseHoverLeave"};
    if (std::ranges::find(events, event) == std::end(events))
        raise(L, std::format("{} is not a ClickDetector event", event));
    Player* player = localPlayer(L);
    const std::array args{Value(player ? player->shared_from_this() : nullptr)};
    detector.fireEvent(event, args);
    return 0;
}

// firetouchinterest(part1, part2, 0): the touch the physics side would report:
// Touched on both parts, each with the other as its argument. Toggle 1 is the
// TouchEnded that ends one. A boolean is the number it stands for, false 0
// and true 1, as most executors read one [Myriad: false touches, true ends;
// sUNC's docs say the opposite, but its test passes numbers].
int firetouchinterest(lua_State* L)
{
    Instance& first = checkInstance(L, 1);
    Instance& second = checkInstance(L, 2);
    const lua_Integer toggle = lua_isboolean(L, 3) ? lua_toboolean(L, 3) : luaL_checkinteger(L, 3);
    const std::string_view event = toggle == 0 ? "Touched" : "TouchEnded";
    const std::array onFirst{Value(second.shared_from_this())};
    const std::array onSecond{Value(first.shared_from_this())};
    first.fireEvent(event, onFirst);
    second.fireEvent(event, onSecond);
    return 0;
}

// --- What is running this ----------------------------------------------------

// checkcaller(): whether the code calling is the console's and not the game's.
// Only console code can call this global, but a hook it installed
// (hookfunction, hookmetamethod) runs on whichever thread called the hooked
// function, so what is asked is the thread: one the console started, or one of
// the game's.
int checkcaller(lua_State* L)
{
    lua_pushboolean(L, Runtime::of(L).accessOf(L).console);
    return 1;
}

// identifyexecutor(): who is running this: the name and the version, two
// strings, as the executors answer it.
int identifyexecutor(lua_State* L)
{
    lua_pushlstring(L, executor.data(), executor.size());
    lua_pushlstring(L, executorVersion.data(), executorVersion.size());
    return 2;
}

// getexecutorname(): the name alone.
int getExecutorName(lua_State* L)
{
    lua_pushlstring(L, executor.data(), executor.size());
    return 1;
}

// loadstring(source, chunkname): the function the source makes, to call later,
// or nil and the compiler's message when it doesn't compile, as Lua's own
// loadstring answers. Only source: bytecode handed to it is text that doesn't
// compile, so a script's bytecode from getscriptbytecode can't be loaded back.
// The function runs in this thread's globals, like the code typed here. The
// chunk name is Lua's: "=name" reads as name in messages and debug.info,
// "@file" as file, anything else as [string "..."]; without one, Console.
int loadstring(lua_State* L)
{
    const std::string_view source = checkString(L, 1);
    const std::string name = lua_type(L, 2) == LUA_TSTRING ? std::string(checkString(L, 2)) : "=Console";
    std::array<char, LUA_IDSIZE> buffer{};
    const char* shown = luaO_chunkid(buffer.data(), buffer.size(), name.c_str(), name.size());
    auto bytecode = ScriptContext::compile(source, shown, 2);
    if (!bytecode) {
        lua_pushnil(L);
        lua_pushlstring(L, bytecode.error().data(), bytecode.error().size());
        return 2;
    }
    // Loaded here, so it keeps the console's globals: luau_load gives the
    // function the globals of the thread it loads into.
    if (luau_load(L, name.c_str(), bytecode->data(), bytecode->size(), 0) != 0) {
        lua_pushnil(L);
        lua_insert(L, -2); // nil, then the message
        return 2;
    }
    return 1;
}

} // namespace

namespace {

// Pushes what a name like "debug.getupvalue" holds in the thread's globals:
// its first part is a global, the rest fields of what that holds.
void pushNamed(lua_State* thread, std::string_view name)
{
    const std::size_t dot = name.find('.');
    lua_getglobal(thread, std::string(name.substr(0, dot)).c_str());
    for (std::size_t at = dot; at != std::string_view::npos && lua_istable(thread, -1);) {
        const std::size_t next = name.find('.', at + 1);
        lua_getfield(thread, -1, std::string(name.substr(at + 1, next - at - 1)).c_str());
        lua_remove(thread, -2);
        at = next;
    }
}

// The other names the executors give what the console has, each the very
// function (or table) it stands for, so that the two compare equal. Myriad
// asks for these; a dotted one is a field of a table the console has.
void addAliases(lua_State* thread)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 23> aliases{{
        {"hookfunc", "hookfunction"},
        {"restorefunc", "restorefunction"},
        {"get_hidden_gui", "gethui"},
        {"getcallbackmember", "getcallbackvalue"},
        {"clonereference", "cloneref"},
        {"iswindowactive", "isrbxactive"},
        {"crypt.lz4compress", "lz4compress"},
        {"crypt.lz4decompress", "lz4decompress"},
        {"websocket", "WebSocket"},
        {"getconstant", "debug.getconstant"},
        {"getconstants", "debug.getconstants"},
        {"setconstant", "debug.setconstant"},
        {"getproto", "debug.getproto"},
        {"getprotos", "debug.getprotos"},
        {"getupvalue", "debug.getupvalue"},
        {"getupvalues", "debug.getupvalues"},
        {"setupvalue", "debug.setupvalue"},
        {"getstack", "debug.getstack"},
        {"setstack", "debug.setstack"},
        {"keytap", "keyclick"},
        {"rconsolesettitle", "rconsolename"},
        {"clearteleportqueue", "clearqueueonteleport"},
        {"clear_teleport_queue", "clearqueueonteleport"},
    }};
    for (const auto& [alias, original] : aliases) {
        const std::size_t dot = alias.rfind('.');
        if (dot == std::string_view::npos) {
            pushNamed(thread, original);
            lua_setglobal(thread, std::string(alias).c_str());
            continue;
        }
        pushNamed(thread, alias.substr(0, dot));
        pushNamed(thread, original);
        lua_setfield(thread, -2, std::string(alias.substr(dot + 1)).c_str());
        lua_pop(thread, 1);
    }
}

// The console's functions, into the thread's globals, which are getgenv()'s
// table while this runs (pushGenv).
void fillConsole(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    set("getgenv", getgenv);
    set("getrenv", getrenv);
    set("getreg", getreg);
    set("getrawmetatable", getrawmetatable);
    set("setrawmetatable", setrawmetatable);
    set("setreadonly", setreadonly);
    set("isreadonly", isreadonly);
    set("makereadonly", makeReadOnly<true>);
    set("makewriteable", makeReadOnly<false>);
    set("getidentity", getidentity);
    set("getthreadidentity", getidentity);
    set("getthreadcontext", getidentity);
    set("setidentity", setidentity);
    set("setthreadidentity", setidentity);
    set("setthreadcontext", setidentity);
    set("getinstances", getinstances);
    set("getnilinstances", getnilinstances);
    set("getscripts", getscripts);
    set("getloadedmodules", getloadedmodules);
    set("getsenv", getsenv);
    set("getcallingscript", getcallingscript);
    set("getconnections", getconnections);
    set("getrendersteppedlist", getrendersteppedlist);
    set("getsignalarguments", getsignalarguments);
    set("getsignalargumentsinfo", getsignalargumentsinfo);
    set("fireproximityprompt", fireproximityprompt);
    set("fireclickdetector", fireclickdetector);
    set("firesignal", firesignal);
    set("replicatesignal", replicatesignal);
    set("firetouchinterest", firetouchinterest);
    set("checkcaller", checkcaller);
    set("identifyexecutor", identifyexecutor);
    set("getexecutorname", getExecutorName);
    set("loadstring", loadstring);
    set("getactors", getactors);
    set("isparallel", isparallel);
    set("run_on_actor", run_on_actor);
    set("create_comm_channel", create_comm_channel);
    set("get_comm_channel", get_comm_channel);
    lua_createtable(thread, 0, 1);
    lua_pushcfunction(thread, signalNew, "new");
    lua_setfield(thread, -2, "new");
    lua_setglobal(thread, "Signal");
    // _G is the console's own, kept in getgenv()'s table so that every run
    // shares it, and not the game's: that one is getrenv()._G.
    lua_newtable(thread);
    lua_setglobal(thread, "_G");
    openConsoleCaches(thread);
    openConsoleCrypt(thread);
    openConsoleDebug(thread);
    openConsoleHooks(thread);
    openConsoleDrawing(thread);
    openConsoleFiles(thread);
    openConsoleDialogs(thread);
    openConsoleProperties(thread);
    openConsoleHost(thread);
    openConsoleHttp(thread);
    openConsoleWebSocket(thread);
    addAliases(thread);
    installConsoleTrace(thread); // research, off unless LUNAR_TRACE_CONSOLE is set
}

} // namespace

void openConsole(lua_State* thread)
{
    // A run's own globals, over getgenv()'s table. Not a safe environment, so
    // the code loaded into it reads its globals as they are when it runs,
    // getrenv()'s writes included, and not as they were when it loaded.
    pushGenv(thread);
    lua_newtable(thread);
    lua_newtable(thread);
    lua_pushvalue(thread, -3);
    lua_setfield(thread, -2, "__index");
    lua_setmetatable(thread, -2);
    lua_replace(thread, LUA_GLOBALSINDEX);
    lua_pop(thread, 1);
}

} // namespace lunar::lua