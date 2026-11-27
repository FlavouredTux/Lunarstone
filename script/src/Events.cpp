// RBXScriptSignal and RBXScriptConnection: the engine's own Instance signals,
// property and attribute change signals, and every other event by name
// through the EventHub.
//
// Member names and messages are the 0.742 client's: Connect/connect,
// ConnectParallel/connectParallel, Once, Wait/wait, Disconnect/disconnect,
// Connected, "Attempt to connect failed: Passed value is not a function",
// "%s is not a valid member of RBXScriptSignal".

#include "RobloxApi.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lunar/ClassRegistry.hpp>

#include <array>
#include <format>
#include <functional>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::lua {

namespace {

using Handler = std::function<void(const PackedArgs&)>;

Packed packArg(const Instance* instance)
{
    return packed(instance ? Value(std::const_pointer_cast<Instance>(instance->shared_from_this())) : Value());
}

Packed packArg(std::string_view text)
{
    return packed(Value(std::string(text)));
}

template <auto Member>
Connection connectMember(Instance& owner, Handler handler)
{
    return (owner.*Member).connect([handler = std::move(handler)](const auto&... args) {
        PackedArgs packedArgs{packArg(args)...};
        handler(packedArgs);
    });
}

// Changed on a ValueBase (IntValue, ObjectValue...) passes the new Value
// rather than a property name, and only fires for Value.
Connection connectChanged(Instance& owner, Handler handler)
{
    if (!owner.isA("ValueBase"))
        return connectMember<&Instance::changed>(owner, std::move(handler));
    return owner.changed.connect([&owner, handler = std::move(handler)](std::string_view property) {
        if (property == "Value")
            handler(PackedArgs{packed(owner.get("Value"))});
    });
}

// What the console's Fire(...) gives the engine's own signals: the instance at
// `index` (null for none or anything else), and the text.
Instance* argInstance(std::span<const Value> args, std::size_t index)
{
    const auto* instance = index < args.size() ? std::get_if<InstancePtr>(&args[index]) : nullptr;
    return instance ? instance->get() : nullptr;
}

std::string_view argText(std::span<const Value> args, std::size_t index)
{
    const auto* text = index < args.size() ? std::get_if<std::string>(&args[index]) : nullptr;
    return text ? std::string_view(*text) : std::string_view{};
}

void fireAncestryChanged(Instance& owner, std::span<const Value> args)
{
    owner.ancestryChanged.fire(argInstance(args, 0), argInstance(args, 1));
}

void fireChanged(Instance& owner, std::span<const Value> args)
{
    // A ValueBase reads its new value out of itself, like connectChanged does.
    owner.changed.fire(argText(args, 0));
}

void fireChildAdded(Instance& owner, std::span<const Value> args)
{
    if (Instance* child = argInstance(args, 0))
        owner.childAdded.fire(child);
}

void fireChildRemoved(Instance& owner, std::span<const Value> args)
{
    if (Instance* child = argInstance(args, 0))
        owner.childRemoved.fire(child);
}

void fireDescendantAdded(Instance& owner, std::span<const Value> args)
{
    if (Instance* child = argInstance(args, 0))
        owner.descendantAdded.fire(child);
}

void fireDescendantRemoving(Instance& owner, std::span<const Value> args)
{
    if (Instance* child = argInstance(args, 0))
        owner.descendantRemoving.fire(child);
}

void fireDestroying(Instance& owner, std::span<const Value>)
{
    owner.destroying.fire();
}

void fireAttributeChanged(Instance& owner, std::span<const Value> args)
{
    owner.attributeChanged.fire(argText(args, 0));
}

struct BuiltinEvent {
    std::string_view name;
    Connection (*connect)(Instance&, Handler);
    void (*fire)(Instance&, std::span<const Value>);
};

constexpr auto builtins = std::to_array<BuiltinEvent>({
    {"AncestryChanged", connectMember<&Instance::ancestryChanged>, fireAncestryChanged},
    {"Changed", connectChanged, fireChanged},
    {"ChildAdded", connectMember<&Instance::childAdded>, fireChildAdded},
    {"ChildRemoved", connectMember<&Instance::childRemoved>, fireChildRemoved},
    {"DescendantAdded", connectMember<&Instance::descendantAdded>, fireDescendantAdded},
    {"DescendantRemoving", connectMember<&Instance::descendantRemoving>, fireDescendantRemoving},
    {"Destroying", connectMember<&Instance::destroying>, fireDestroying},
    {"AttributeChanged", connectMember<&Instance::attributeChanged>, fireAttributeChanged},
});

struct ConnectionData {
    std::shared_ptr<Listener> listener;
};

Connection connect(lua_State* L, const SignalData& signal, Handler handler)
{
    Instance& owner = *signal.owner;
    switch (signal.kind) {
    case SignalData::Kind::Builtin:
        return builtins[static_cast<std::size_t>(signal.builtin)].connect(owner, std::move(handler));
    case SignalData::Kind::Property:
        return owner.changed.connect([handler = std::move(handler), property = signal.name](std::string_view p) {
            if (p == property)
                handler({});
        });
    case SignalData::Kind::Attribute:
        return owner.attributeChanged.connect(
            [handler = std::move(handler), attribute = signal.name](std::string_view a) {
                if (a == attribute)
                    handler({});
            });
    case SignalData::Kind::Hub:
        break;
    }
    Runtime& rt = Runtime::of(L);
    return rt.shared().events.event(owner, signal.name).connect(std::move(handler));
}

SignalData& checkSignal(lua_State* L, const char* method)
{
    if (auto* signal = static_cast<SignalData*>(lua_touserdatatagged(L, 1, SignalTag)))
        return *signal;
    raise(L, std::format("Expected ':' not '.' calling member function {}", method));
}

ConnectionData& checkConnection(lua_State* L, int idx)
{
    if (auto* connection = static_cast<ConnectionData*>(lua_touserdatatagged(L, idx, ConnectionTag)))
        return *connection;
    luaL_typeerror(L, idx, "RBXScriptConnection");
}

std::string_view signalName(const SignalData& signal)
{
    switch (signal.kind) {
    case SignalData::Kind::Builtin:
        return builtins[static_cast<std::size_t>(signal.builtin)].name;
    case SignalData::Kind::Hub:
        return signal.name;
    default:
        return "Changed";
    }
}

// Remotes only deliver on their receiving side.
void checkRemoteSide(lua_State* L, const SignalData& signal)
{
    if (signal.kind != SignalData::Kind::Hub)
        return;
    const Runtime& rt = Runtime::of(L);
    if (signal.name == "OnServerEvent" && !rt.isServer())
        raise(L, "OnServerEvent can only be used on the server");
    if (signal.name == "OnClientEvent" && !rt.isClient())
        raise(L, "OnClientEvent can only be used on the client");
    if (signal.name == "RenderStepped" && !rt.isClient() && signal.owner->isA("RunService"))
        raise(L, "RenderStepped event can only be used from local scripts");
}

// A part a script listens to for touches has a TouchTransmitter in it, as
// Roblox puts one in ("TouchInterest") the first time a script connects its
// Touched or TouchEnded: it's what the client's physics looks for to report
// the part's touches, and scripts check for it.
void noteTouchInterest(lua_State* L, const SignalData& signal)
{
    if (signal.kind != SignalData::Kind::Hub || (signal.name != "Touched" && signal.name != "TouchEnded")
        || !signal.owner->isA("BasePart") || signal.owner->findFirstChildOfClass("TouchTransmitter"))
        return;
    // Not a class the engine has (nor one Instance.new makes): it's the
    // placeholder the replicator gives one the server sends.
    auto interest = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("TouchTransmitter"));
    interest->setName("TouchInterest");
    guard(L, [&] { interest->setParent(signal.owner.get()); });
}

int connectCallback(lua_State* L, const char* method, bool once)
{
    SignalData& signal = checkSignal(L, method);
    if (!lua_isfunction(L, 2))
        raise(L, "Attempt to connect failed: Passed value is not a function");
    checkRemoteSide(L, signal);

    Runtime& rt = Runtime::of(L);
    auto listener = rt.newListener();
    listener->callback = lua_ref(L, 2);
    listener->once = once;
    // The handler runs as whoever is connecting: a console run's connections
    // keep the full access its code had.
    listener->access = rt.accessOf(L);
    lua_pushthread(L);
    listener->thread = lua_ref(L, -1);
    lua_pop(L, 1);
    listener->owner = signal.owner;
    listener->kind = signal.kind;
    listener->event = signalName(signal);
    listener->source = std::format("{}.{}", signal.owner->className(), listener->event);
    listener->connection
        = connect(L, signal, [listener](const PackedArgs& args) { listener->runtime->dispatch(listener, args); });

    noteTouchInterest(L, signal);

    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(ConnectionData), ConnectionTag);
    new (memory) ConnectionData{std::move(listener)};
    // Remotes that queued up before anyone listened get delivered now.
    if (signal.kind == SignalData::Kind::Hub && (signal.name == "OnServerEvent" || signal.name == "OnClientEvent"))
        rt.defer([&rt] { rt.shared().deliverMessages(); });
    return 1;
}

int signalConnect(lua_State* L)
{
    return connectCallback(L, "Connect", false);
}

int signalOnce(lua_State* L)
{
    return connectCallback(L, "Once", true);
}

// Yields until the next fire and returns its arguments.
int signalWait(lua_State* L)
{
    SignalData& signal = checkSignal(L, "Wait");
    checkYieldable(L);
    checkRemoteSide(L, signal);
    Runtime& rt = Runtime::of(L);
    const std::uint64_t token = rt.suspend(L);
    std::string what;
    if (rt.shared().profiler)
        what = std::format("{}.{}:Wait", signal.owner->className(), signalName(signal));
    // The engine's own connection, which the console's getconnections lists
    // while the thread waits [sUNC's getconnections waits on a folder's
    // ChildAdded from a task.spawn and reads [1]]. The slot holds it, so it
    // goes when the wait is over.
    auto native = std::make_shared<EventHub::NativeConnection>();
    native->waiting = L;
    native->handler = [&rt, L, token, what = std::move(what)](const PackedArgs& args) {
        // Drop the connection first: a deferred wake mustn't see a second fire.
        if (rt.isSuspended(L, token))
            rt.wakeWith(L, token, args, what);
    };
    native->connection = connect(L, signal, [native](const PackedArgs& args) {
        if (native->enabled)
            native->handler(args);
    });
    if (signal.kind == SignalData::Kind::Hub || signal.kind == SignalData::Kind::Builtin)
        rt.shared().events.noteNative(*signal.owner, signalName(signal), native);
    rt.attach(L, token, native->connection);
    if (signal.kind == SignalData::Kind::Hub && (signal.name == "OnServerEvent" || signal.name == "OnClientEvent"))
        rt.defer([&rt] { rt.shared().deliverMessages(); });
    return lua_yield(L, 0);
}

// A Signal.new signal's Fire(...): its connections run now, and the threads
// waiting on it wake, as firesignal has them.
int signalFire(lua_State* L)
{
    checkSignal(L, "Fire");
    return fireConnections(L, 1);
}

int signalIndex(lua_State* L)
{
    std::string_view key = checkString(L, 2);
    if (key == "Connect" || key == "connect" || key == "ConnectParallel" || key == "connectParallel")
        lua_pushcfunction(L, signalConnect, "Connect");
    else if (key == "Once")
        lua_pushcfunction(L, signalOnce, "Once");
    else if (key == "Wait" || key == "wait")
        lua_pushcfunction(L, signalWait, "Wait");
    else if (key == "Fire" && checkSignal(L, "Fire").fireable)
        lua_pushcfunction(L, signalFire, "Fire");
    else
        raise(L, std::format("{} is not a valid member of RBXScriptSignal", key));
    return 1;
}

int signalToString(lua_State* L)
{
    std::string text = std::format("Signal {}", signalName(checkSignal(L, "__tostring")));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int connectionDisconnect(lua_State* L)
{
    checkConnection(L, 1).listener->disconnect(L);
    return 0;
}

int connectionIndex(lua_State* L)
{
    ConnectionData& connection = checkConnection(L, 1);
    std::string_view key = checkString(L, 2);
    if (key == "Connected" || key == "connected")
        lua_pushboolean(L, connection.listener->connection.connected());
    else if (key == "Disconnect" || key == "disconnect")
        lua_pushcfunction(L, connectionDisconnect, "Disconnect");
    else
        raise(L, std::format("{} is not a valid member of RBXScriptConnection", key));
    return 1;
}

int connectionToString(lua_State* L)
{
    lua_pushliteral(L, "Connection");
    return 1;
}

void pushSignalData(lua_State* L, SignalData data)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(SignalData), SignalTag);
    new (memory) SignalData(std::move(data));
}

InstancePtr ownerPtr(const Instance& owner)
{
    return std::const_pointer_cast<Instance>(owner.shared_from_this());
}

// The console's getconnections: one table per listener the VM holds on this
// signal, the way an executor hands them back -- the callback, whether it runs,
// and Fire/Disconnect/Disable/Enable over it. A listener disabled here is one
// the console turned off; it stays connected, as Disable() leaves it.
Runtime::ConsoleConnection* checkConsoleConnection(lua_State* L, int idx)
{
    if (auto* connection = static_cast<Runtime::ConsoleConnection*>(lua_touserdata(L, idx)))
        return connection;
    luaL_typeerror(L, idx, "connection table from getconnections");
}

// Where a connection method's arguments start: past the connection table when
// it's called with a colon (connection:Fire(a)), as the docs call it, and at
// 1 for a plain call (connection.Fire(a)). The table is the one whose Fire
// closes over the same connection.
int firstArgument(lua_State* L, const Runtime::ConsoleConnection* connection)
{
    if (lua_type(L, 1) != LUA_TTABLE)
        return 1;
    bool self = false;
    if (lua_rawgetfield(L, 1, "Fire") == LUA_TFUNCTION && lua_getupvalue(L, -1, 1)) {
        self = lua_touserdata(L, -1) == connection;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return self ? 2 : 1;
}

// Fire(...): the connection's own function runs now with the arguments, and
// no other connection on the signal does.
int consoleFire(lua_State* L)
{
    Runtime::ConsoleConnection* connection = checkConsoleConnection(L, lua_upvalueindex(1));
    Runtime::of(L).dispatch(connection->listener, packArgs(L, firstArgument(L, connection), lua_gettop(L)), true);
    return 0;
}

int consoleDisconnect(lua_State* L)
{
    checkConsoleConnection(L, lua_upvalueindex(1))->listener->disconnect(L);
    return 0;
}

int consoleDisable(lua_State* L)
{
    checkConsoleConnection(L, lua_upvalueindex(1))->listener->enabled = false;
    return 0;
}

int consoleEnable(lua_State* L)
{
    checkConsoleConnection(L, lua_upvalueindex(1))->listener->enabled = true;
    return 0;
}

// Defer(...): the connection's function runs with these arguments from the
// next resumption point instead of from here, as task.defer would run it.
int consoleDefer(lua_State* L)
{
    Runtime::ConsoleConnection* connection = checkConsoleConnection(L, lua_upvalueindex(1));
    PackedArgs arguments = packArgs(L, firstArgument(L, connection), lua_gettop(L));
    Runtime& rt = Runtime::of(L);
    auto listener = connection->listener;
    rt.defer([&rt, listener, arguments = std::move(arguments)]() mutable { rt.dispatch(listener, arguments, true); });
    return 0;
}

// The thread that made the connection, as the executors' Thread is [sUNC's
// getconnections connects from a game script's thread, caught in a
// __namecall hook, and looks for that thread]; nil for one the client made.
void pushConnectingThread(lua_State* L, const Listener& listener)
{
    if (listener.thread == LUA_NOREF)
        lua_pushnil(L);
    else
        lua_getref(L, listener.thread);
}

// Enabled is read and written on the connection itself, so a table the console
// kept still says whether its handler runs.
int consoleGetEnabled(lua_State* L)
{
    Runtime::ConsoleConnection* connection = checkConsoleConnection(L, lua_upvalueindex(1));
    if (const char* key = lua_tostring(L, 2); !key || std::string_view(key) != "Enabled") {
        lua_pushnil(L); // a field it doesn't have (Thread, for one the client made)
        return 1;
    }
    // Disconnected, it's off for good.
    lua_pushboolean(L, connection->listener->enabled && connection->listener->callback != LUA_NOREF);
    return 1;
}

int consoleSetEnabled(lua_State* L)
{
    if (const char* key = lua_tostring(L, 2); !key || std::string_view(key) != "Enabled") {
        lua_rawset(L, 1);
        return 0;
    }
    checkConsoleConnection(L, lua_upvalueindex(1))->listener->enabled = lua_toboolean(L, 3) != 0;
    return 0;
}

// The methods close over the connection, which the Runtime keeps alive for as
// long as the console may be holding its table.
int pushConsoleConnection(lua_State* L, const SignalData& signal, const std::shared_ptr<Listener>& listener)
{
    Runtime::ConsoleConnection* connection = Runtime::of(L).keepConnection(listener, signal);
    lua_createtable(L, 0, 8);
    const int table = lua_gettop(L);
    for (const auto& [name, function] :
         {std::pair{"Fire", consoleFire}, std::pair{"Defer", consoleDefer}, std::pair{"Disconnect", consoleDisconnect},
          std::pair{"Disable", consoleDisable}, std::pair{"Enable", consoleEnable}}) {
        lua_pushlightuserdata(L, connection);
        lua_pushcclosure(L, function, name, 1);
        lua_setfield(L, table, name);
    }
    if (listener->callback == LUA_NOREF)
        lua_pushnil(L);
    else
        lua_getref(L, listener->callback);
    // LuaConnection: the callback is Luau code rather than one of the client's
    // own C functions, which has to be asked of it while it is on the stack.
    const bool luaConnection = listener->callback != LUA_NOREF && consoleIsLuauClosure(L, -1);
    lua_setfield(L, table, "Function");
    // ForeignState: the handler runs as somebody else, so what it does is not
    // this thread's own -- a game script's connection asked from the console, or
    // the other way about.
    lua_pushboolean(L, listener->access.identity != Runtime::of(L).accessOf(L).identity);
    lua_setfield(L, table, "ForeignState");
    lua_pushboolean(L, luaConnection);
    lua_setfield(L, table, "LuaConnection");
    pushConnectingThread(L, *listener);
    lua_setfield(L, table, "Thread");
    lua_newtable(L);
    const int metatable = lua_gettop(L);
    for (const auto& [name, function] :
         {std::pair{"__index", consoleGetEnabled}, std::pair{"__newindex", consoleSetEnabled}}) {
        lua_pushlightuserdata(L, connection);
        lua_pushcclosure(L, function, name, 1);
        lua_setfield(L, metatable, name);
    }
    lua_setmetatable(L, table);
    return 1;
}

// A C connection's table: what the client's own code connected (EventHub's
// native connections), with no Function and no Thread, as the executors hand
// back the engine's; or a thread's :Wait(), the engine's too. Its methods
// close over a weak hold on it.
using NativeHold = std::weak_ptr<EventHub::NativeConnection>;

std::shared_ptr<EventHub::NativeConnection> checkNative(lua_State* L)
{
    auto* hold = static_cast<NativeHold*>(lua_touserdata(L, lua_upvalueindex(1)));
    return hold ? hold->lock() : nullptr;
}

// Fire(...) and Defer(...) run its handler, now or from the next resumption
// point; called with a colon, the table isn't one of the arguments.
int nativeFire(lua_State* L)
{
    const int first = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
    if (const auto native = checkNative(L))
        native->handler(packArgs(L, first, lua_gettop(L)));
    return 0;
}

int nativeDefer(lua_State* L)
{
    const int first = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
    if (const auto native = checkNative(L)) {
        PackedArgs arguments = packArgs(L, first, lua_gettop(L));
        Runtime::of(L).defer([weak = std::weak_ptr(native), arguments = std::move(arguments)] {
            if (const auto held = weak.lock())
                held->handler(arguments);
        });
    }
    return 0;
}

int nativeDisconnect(lua_State* L)
{
    if (const auto native = checkNative(L))
        native->connection.disconnect();
    return 0;
}

int nativeDisable(lua_State* L)
{
    if (const auto native = checkNative(L))
        native->enabled = false;
    return 0;
}

int nativeEnable(lua_State* L)
{
    if (const auto native = checkNative(L))
        native->enabled = true;
    return 0;
}

int nativeGetEnabled(lua_State* L)
{
    if (const char* key = lua_tostring(L, 2); !key || std::string_view(key) != "Enabled") {
        lua_pushnil(L); // Function and Thread: a C connection has neither
        return 1;
    }
    const auto native = checkNative(L);
    lua_pushboolean(L, native && native->enabled && native->connection.connected());
    return 1;
}

int nativeSetEnabled(lua_State* L)
{
    if (const char* key = lua_tostring(L, 2); !key || std::string_view(key) != "Enabled") {
        lua_rawset(L, 1);
        return 0;
    }
    if (const auto native = checkNative(L))
        native->enabled = lua_toboolean(L, 3) != 0;
    return 0;
}

void pushNativeConnection(lua_State* L, const std::shared_ptr<EventHub::NativeConnection>& native)
{
    lua_createtable(L, 0, 7);
    const int table = lua_gettop(L);
    void* memory = lua_newuserdatadtor(L, sizeof(NativeHold),
                                       [](lua_State*, void* hold) { static_cast<NativeHold*>(hold)->~NativeHold(); });
    new (memory) NativeHold(native);
    const int hold = lua_gettop(L);
    for (const auto& [name, function] :
         {std::pair{"Fire", nativeFire}, std::pair{"Defer", nativeDefer}, std::pair{"Disconnect", nativeDisconnect},
          std::pair{"Disable", nativeDisable}, std::pair{"Enable", nativeEnable}}) {
        lua_pushvalue(L, hold);
        lua_pushcclosure(L, function, name, 1);
        lua_setfield(L, table, name);
    }
    // A thread's :Wait() is a Luau connection with that thread and no function
    // [sUNC's getconnections checks LuaConnection on one], foreign as a
    // listener is: when the thread runs as someone else.
    const bool waiting = native->waiting && lua_mainthread(native->waiting) == lua_mainthread(L);
    if (waiting) {
        lua_pushthread(native->waiting);
        lua_xmove(native->waiting, L, 1);
        lua_setfield(L, table, "Thread");
    }
    const Runtime& rt = Runtime::of(L);
    lua_pushboolean(L, !waiting || rt.accessOf(native->waiting).identity != rt.accessOf(L).identity);
    lua_setfield(L, table, "ForeignState");
    lua_pushboolean(L, waiting);
    lua_setfield(L, table, "LuaConnection");
    lua_newtable(L);
    const int metatable = lua_gettop(L);
    for (const auto& [name, function] :
         {std::pair{"__index", nativeGetEnabled}, std::pair{"__newindex", nativeSetEnabled}}) {
        lua_pushvalue(L, hold);
        lua_pushcclosure(L, function, name, 1);
        lua_setfield(L, metatable, name);
    }
    lua_setmetatable(L, table);
    lua_pop(L, 1); // the hold
}

} // namespace

namespace {

const SignalData& checkSignal(lua_State* L, int idx)
{
    const SignalData* signal = static_cast<SignalData*>(lua_touserdatatagged(L, idx, SignalTag));
    if (!signal)
        luaL_typeerror(L, idx, "RBXScriptSignal");
    return *signal;
}

// The Luau connections the VM holds on the signal.
std::vector<std::shared_ptr<Listener>> listenersOn(lua_State* L, const SignalData& signal)
{
    const std::string_view event = signalName(signal);
    std::vector<std::shared_ptr<Listener>> found;
    for (const std::shared_ptr<Listener>& listener : Runtime::of(L).listeners()) {
        if (listener->callback == LUA_NOREF)
            continue;
        if (listener->owner.lock() == signal.owner && listener->event == event && listener->kind == signal.kind)
            found.push_back(listener);
    }
    return found;
}

} // namespace

int pushConnections(lua_State* L, int idx)
{
    const SignalData& signalData = checkSignal(L, idx);
    const SignalData* signal = &signalData;
    const std::vector<std::shared_ptr<Listener>> found = listenersOn(L, signalData);
    lua_createtable(L, static_cast<int>(found.size()), 0);
    int i = 0;
    // Newest first, as Roblox's signal keeps them and the executors walk it
    // [sUNC's getconnections connects from a game thread and reads [1]].
    for (const std::shared_ptr<Listener>& listener : std::views::reverse(found)) {
        pushConsoleConnection(L, *signal, listener);
        lua_rawseti(L, -2, ++i);
    }
    // Then the C ones: what the client connected itself, and the threads
    // waiting on the signal.
    if (signal->kind == SignalData::Kind::Hub || signal->kind == SignalData::Kind::Builtin)
        for (const auto& native : std::views::reverse(
                 Runtime::of(L).shared().events.nativeConnections(*signal->owner, signalName(*signal)))) {
            pushNativeConnection(L, native);
            lua_rawseti(L, -2, ++i);
        }
    return 1;
}

int findBuiltinEvent(std::string_view name)
{
    for (std::size_t i = 0; i < builtins.size(); ++i)
        if (builtins[i].name == name)
            return static_cast<int>(i);
    return -1;
}

void pushSignal(lua_State* L, const Instance& owner, int builtin)
{
    pushSignalData(L, SignalData{ownerPtr(owner), SignalData::Kind::Builtin, builtin, {}});
}

void pushPropertySignal(lua_State* L, const Instance& owner, std::string property)
{
    pushSignalData(L, SignalData{ownerPtr(owner), SignalData::Kind::Property, -1, std::move(property)});
}

void pushAttributeSignal(lua_State* L, const Instance& owner, std::string attribute)
{
    pushSignalData(L, SignalData{ownerPtr(owner), SignalData::Kind::Attribute, -1, std::move(attribute)});
}

void pushHubSignal(lua_State* L, const Instance& owner, std::string name, bool fireable)
{
    pushSignalData(L, SignalData{ownerPtr(owner), SignalData::Kind::Hub, -1, std::move(name), fireable});
}

int fireConnections(lua_State* L, int idx)
{
    const SignalData& signal = checkSignal(L, idx);
    const PackedArgs arguments = packArgs(L, idx + 1, lua_gettop(L));
    Runtime& rt = Runtime::of(L);
    for (const std::shared_ptr<Listener>& listener : listenersOn(L, signal))
        rt.dispatch(listener, arguments, true);
    // And the threads of this VM that :Wait() on it; the client's own C
    // connections are not the scripts' to fire.
    if (signal.kind == SignalData::Kind::Hub || signal.kind == SignalData::Kind::Builtin)
        for (const auto& native : rt.shared().events.nativeConnections(*signal.owner, signalName(signal)))
            if (native->enabled && native->waiting && lua_mainthread(native->waiting) == lua_mainthread(L))
                native->handler(arguments);
    return 0;
}

namespace {

// What typeof says of a value: a userdata's or vector's __type, else its type.
std::string typeOf(lua_State* L, int idx)
{
    const int type = lua_type(L, idx);
    if ((type == LUA_TUSERDATA || type == LUA_TVECTOR) && luaL_getmetafield(L, idx, "__type")) {
        std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : luaL_typename(L, idx);
        lua_pop(L, 1);
        return name;
    }
    return luaL_typename(L, idx);
}

} // namespace

int replicateSignal(lua_State* L, int idx)
{
    const SignalData& signal = checkSignal(L, idx);
    const Shared& shared = Runtime::of(L).shared();
    const std::string_view event = signalName(signal);
    const bool replicable = signal.kind == SignalData::Kind::Builtin || signal.kind == SignalData::Kind::Hub;
    if (!shared.remoteSink || !shared.eventShape)
        raise(L, "there is no server to replicate the signal to");
    const auto shape = replicable ? shared.eventShape(*signal.owner, event) : std::nullopt;
    if (!shape)
        raise(L, std::format("{}.{} does not replicate", signal.owner->className(), event));
    const int given = lua_gettop(L) - idx;
    int taken = 0;
    bool rest = false;
    for (const std::string& type : *shape) {
        if (type == "...") {
            rest = true;
            break;
        }
        ++taken;
        if (taken > given)
            raise(L, std::format("missing argument #{} to {} ({} expected)", taken, event,
                                 type.empty() ? "a value" : type));
        const int at = idx + taken;
        if (type.empty() || (type == "Instance" && lua_isnil(L, at)))
            continue;
        if (std::string got = typeOf(L, at); got != type)
            raise(L, std::format("invalid argument #{} to {} ({} expected, got {})", taken, event, type, got));
    }
    if (!rest && given > taken)
        raise(L, std::format("{} takes {} argument{}, not {}", event, taken, taken == 1 ? "" : "s", given));
    std::vector<RemoteValue> arguments;
    for (const Packed& argument : packArgs(L, idx + 1, lua_gettop(L)))
        arguments.push_back(toRemote(argument));
    shared.remoteSink(*signal.owner, event, std::move(arguments));
    return 0;
}

int pushSignalArguments(lua_State* L, int idx, bool info)
{
    const SignalData& signal = checkSignal(L, idx);
    // A property's or an attribute's signal passes nothing; one of ours that
    // isn't an engine event (a WebSocket's) has no parameters to give.
    std::span<const api::EventParameter> parameters;
    if (signal.kind == SignalData::Kind::Builtin || signal.kind == SignalData::Kind::Hub)
        parameters = api::eventParameters(signal.owner->className(), signalName(signal)).value_or(parameters);
    lua_createtable(L, static_cast<int>(parameters.size()), 0);
    int i = 0;
    for (const api::EventParameter& parameter : parameters) {
        if (info) {
            lua_createtable(L, 0, 2);
            pushString(L, parameter.name);
            lua_setfield(L, -2, "Name");
            pushString(L, parameter.type);
            lua_setfield(L, -2, "Type");
        } else {
            pushString(L, parameter.type);
        }
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

void openEvents(lua_State* L)
{
    lua_newtable(L);
    pushMetamethod(L, signalIndex);
    lua_setfield(L, -2, "__index");
    pushMetamethod(L, signalToString);
    lua_setfield(L, -2, "__tostring");
    registerType(L, SignalTag, "RBXScriptSignal",
                 [](lua_State*, void* memory) { static_cast<SignalData*>(memory)->~SignalData(); });

    lua_newtable(L);
    pushMetamethod(L, connectionIndex);
    lua_setfield(L, -2, "__index");
    pushMetamethod(L, connectionToString);
    lua_setfield(L, -2, "__tostring");
    registerType(L, ConnectionTag, "RBXScriptConnection",
                 [](lua_State*, void* memory) { static_cast<ConnectionData*>(memory)->~ConnectionData(); });
}

} // namespace lunar::lua
