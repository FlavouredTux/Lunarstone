#pragma once

// Internals shared by the script library's sources: the per-VM runtime with
// its scheduler, what runtimes of one game share (events, callbacks, the
// client/server loopback), and the helpers that move engine objects in and
// out of Luau.

#include <lunar/Classes.hpp>
#include <lunar/Prompts.hpp>
#include <lunar/Tools.hpp>
#include <lunar/script/ScriptContext.hpp>

#include "RobloxApi.hpp"

#include <lua.h>
#include <lualib.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lunar::assets {
class RobloxWeb;
} // namespace lunar::assets

namespace lunar::lua {

// Userdata tags, one metatable each. Vector3 is Luau's native vector.
enum Tag : std::uint8_t {
    InstanceTag = 1,
    Color3Tag,
    CFrameTag,
    SignalTag,
    ConnectionTag,
    Vector2Tag,
    UDimTag,
    UDim2Tag,
    RectTag,
    NumberRangeTag,
    NumberSequenceTag,
    NumberSequenceKeypointTag,
    ColorSequenceTag,
    ColorSequenceKeypointTag,
    BrickColorTag,
    EnumItemTag,
    EnumTag,
    EnumsTag,
    FacesTag,
    AxesTag,
    PhysicalPropertiesTag,
    Vector3int16Tag,
    FontTag,
    TweenInfoTag,
    RayTag,
    Region3Tag,
    Region3int16Tag,
    DateTimeTag,
    RandomTag,
    RaycastParamsTag,
    OverlapParamsTag,
    RaycastResultTag,
    Path2DControlPointTag,
    Vector2int16Tag,
    PathWaypointTag,
    RotationCurveKeyTag,
    ContentTag,
    PluginDragTag,
    InputObjectTag,
    CatalogSearchParamsTag,
    SharedTableTag,
};

// --- Errors ------------------------------------------------------------------

// Raises a Luau error with just the message, like the client's own
// functions: they throw a C++ exception (0x102f6f63d formats it) that the
// bridge turns into the error, so pcall sees "Kick is not a valid member of
// Workspace \"Workspace\"" with no "chunk:line: " in front [code]. Only
// Luau's library functions (luaL_error) add the caller's position.
[[noreturn]] void raise(lua_State* L, std::string_view message);

// Runs f, turning a lunar::Error from the engine into a Luau error.
template <class F>
decltype(auto) guard(lua_State* L, F&& f)
{
    std::string message;
    try {
        return std::forward<F>(f)();
    } catch (const Error& e) {
        message = e.what();
    }
    raise(L, message);
}

// Joins message pieces; keeps long messages out of single string literals.
template <class... Parts>
std::string concat(const Parts&... parts)
{
    std::string text;
    (text.append(parts), ...);
    return text;
}

// Argument helpers.
std::string_view checkString(lua_State* L, int idx);
void checkYieldable(lua_State* L);

// Turns the table on top of the stack into the metatable of a userdata tag
// (popping it), with __type, a locked __metatable and an optional destructor.
void registerType(lua_State* L, int tag, const char* typeName, lua_Destructor dtor = nullptr);

// Pushes a metamethod. The client's metatable builder (0x10149290f) pushes
// each one with lua_pushcclosurek(L, fn, NULL, 0, 0), so debug.info sees a
// C function with no name: "n" is "" [code].
inline void pushMetamethod(lua_State* L, lua_CFunction fn, int upvalues = 0)
{
    lua_pushcclosurek(L, fn, nullptr, upvalues, nullptr);
}

// Pops a type's methods table and its __index (in that order on the stack)
// and pushes the __namecall that goes with them. Roblox's types all have one,
// so a method call on any of them leaves its name where getnamecallmethod
// reads it.
void pushNamecall(lua_State* L);

// --- Values (Datatypes.cpp, InstanceApi.cpp, ValueConversion.cpp) -------------

void pushInstance(lua_State* L, const Instance* instance); // nil for null
Instance* toInstance(lua_State* L, int idx);               // null unless an Instance
Instance& checkInstance(lua_State* L, int idx);            // argument error unless an Instance

void pushVector3(lua_State* L, Vector3 v);
std::optional<Vector3> toVector3(lua_State* L, int idx);
Vector3 checkVector3(lua_State* L, int idx);

void pushColor3(lua_State* L, Color3 c);
const Color3* toColor3(lua_State* L, int idx);
Color3 checkColor3(lua_State* L, int idx);

void pushCFrame(lua_State* L, const CFrame& cf);
const CFrame* toCFrame(lua_State* L, int idx);
const CFrame& checkCFrame(lua_State* L, int idx);
// CFrame.lookAt.
CFrame lookAtCFrame(Vector3 at, Vector3 target, Vector3 up);

// Engine Value <-> Luau. toValue is empty for Luau values with no Value form.
void pushValue(lua_State* L, const Value& value);
std::optional<Value> toValue(lua_State* L, int idx);

// Library setup, run once on the main thread before sandboxing.
void openDatatypes(lua_State* L);
void openEnums(lua_State* L);
void openValueTypes(lua_State* L);
void openScriptTypes(lua_State* L);
void openInstances(lua_State* L);
void openEvents(lua_State* L);
void openServices(lua_State* L);
// SharedTable and SharedTableRegistry's methods (SharedTable.cpp).
void openSharedTable(lua_State* L);
bool isSharedTable(lua_State* L, int idx);
int sharedTableRegistryGet(lua_State* L);
int sharedTableRegistrySet(lua_State* L);
// The console's globals (getgenv, getidentity and the rest), into the globals
// of one thread: the menu's Luau tab runs with them, and no game script ever
// sees them.
void openConsole(lua_State* thread);

// utf8.graphemes, nfcnormalize, nfdnormalize (Utf8Roblox.cpp).
void openUtf8Extensions(lua_State* L);

// research: LUNAR_TRACE_BUILTINS (BuiltinTrace.cpp).
void installBuiltinTrace(lua_State* L);
void installConsoleTrace(lua_State* thread);
void dumpBuiltinTrace();

// --- Packed values -----------------------------------------------------------

// A Luau value copied out of one VM so it can be handed to another (or back
// to the same one later): what BindableEvents, remotes and deferred events
// carry. Tables are deep copies without metatables, like Roblox's argument
// serialization; functions, threads and other unrepresentable values become nil.
struct Packed;
using PackedTable = std::vector<std::pair<Packed, Packed>>;
struct Packed {
    Value value;
    std::shared_ptr<const PackedTable> table; // set for tables, then value is nil
    std::optional<std::uint64_t> random;      // set for a Random: its generator's state (RemoteValue::random)
};
using PackedArgs = std::vector<Packed>;

Packed pack(lua_State* L, int idx);
PackedArgs packArgs(lua_State* L, int first, int last);

// The client's limits on the arguments of an event it sends to the server.
// Its writer counts the values it writes (tables too) and their strings'
// bytes, and past either limit throws "Replicator send variant: ..."
// (0x1055f28c9, the messages 0x106b7b5ce/0x106b7b611), so the event is never
// sent. The limits are the live DFIntClientSendEventStringDataLimitBytes and
// DFIntClientSendEventVariantCountLimit (clientsettingscdn, PCDesktopClient,
// 2026-11-03; 0 in the image, which is none). GUESS: that a dictionary's keys
// count as string data.
inline constexpr std::uint64_t kClientSendStringBytes = 100'000'000;
inline constexpr std::uint64_t kClientSendVariants = 5'000'000;
// The client's message when values first..last are over the limits, counted
// before anything is packed (sUNC's last remote call has 2 GB of strings).
std::optional<std::string> overSendLimits(lua_State* L, int first, int last);
void unpack(lua_State* L, const Packed& value);
int unpackArgs(lua_State* L, std::span<const Packed> args);
inline Packed packed(Value value)
{
    return Packed{std::move(value), nullptr};
}

// Packed values as remotes carry them over the network: a table with the
// keys 1..n (and n > 0) goes as an array of those members, any other as a
// dictionary whose keys become strings (GUESS: Roblox's rule for mixed and
// non-string keys, from its documentation, not the binary).
RemoteValue toRemote(const Packed& value);
Packed fromRemote(const RemoteValue& value);

// --- Access -------------------------------------------------------------------

// What a Luau thread may reach: the identity level getidentity() reports and
// the capabilities that go with it. Every thread of a VM has one, kept in the
// thread's own storage (lua_getthreaddata): a new thread starts with its
// parent's, so the console's full access reaches everything it spawns (task.spawn,
// coroutines, the threads its connections run in), and setidentity changes the
// thread's own. A thread with no record runs as a game script does.
struct ThreadAccess {
    int identity = static_cast<int>(api::Identity::Basic);
    api::Capabilities capabilities = 0;
    std::size_t slot = 0; // which record of the Runtime's this is
    // Whether the console started it (checkcaller): kept through setidentity,
    // and false on a game thread that runs a hook the console installed.
    bool console = false;
    // The script it runs for: the one whose code made it, as the client keeps
    // it with each thread, whatever its globals now say `script` is
    // (getscriptfromthread, getcallingscript). None for the console's.
    std::weak_ptr<Instance> script;
    // The Actor it runs in: its script's, or the one run_on_actor ran it
    // in. Its threads keep it.
    std::weak_ptr<Instance> actor;
    // Whether it is in the parallel phase: between an actor's thread's
    // task.desynchronize and its task.synchronize (isparallel). Everything
    // still runs serially here; the phase is only what it says it is.
    bool parallel = false;

    // Whether a capability api::MemberData numbers is one of the thread's.
    // Zero, what a member needs none of, is always yes.
    bool has(std::uint8_t capability) const
    {
        return capability == 0 || ((capabilities >> (capability - 1)) & 1u) != 0;
    }
};

// What RunAccess means for a thread, as the executor globals report it.
ThreadAccess accessOf(RunAccess access);

// --- Events ------------------------------------------------------------------

class Runtime;

// Signals the engine doesn't declare as members, by instance and name: class
// events (Touched, Heartbeat, Event, OnServerEvent...) and the ones methods
// make up (GetInstanceAddedSignal). The engine's Instance::eventFired feeds
// into it, so C++ firing an event by name reaches scripts.
class EventHub {
public:
    using Event = Signal<const PackedArgs&>;

    EventHub() = default;
    EventHub(const EventHub&) = delete;
    EventHub& operator=(const EventHub&) = delete;
    ~EventHub();

    Event& event(const Instance& owner, std::string_view name);
    Event* find(const Instance& owner, std::string_view name);
    // Fires the event if anyone listens.
    void fire(const Instance& owner, std::string_view name, const PackedArgs& args);
    bool hasListeners(const Instance& owner, std::string_view name);

    // A connection the client's own code makes, as Roblox's engine connects to
    // some of its signals (a Player's Idled, for the idle kick): the console's
    // getconnections shows it as a C connection, with no Function and no
    // Thread. Whoever made it holds it; once nobody does, it disconnects.
    struct NativeConnection {
        Connection connection;
        std::function<void(const PackedArgs&)> handler;
        bool enabled = true; // the console's Disable() and Enable()
        // A thread's :Wait(): the thread, which the console's getconnections
        // hands back as the connection's.
        lua_State* waiting = nullptr;
        NativeConnection() = default;
        NativeConnection(const NativeConnection&) = delete;
        NativeConnection& operator=(const NativeConnection&) = delete;
        ~NativeConnection() { connection.disconnect(); }
    };
    std::shared_ptr<NativeConnection> connectNative(const Instance& owner, std::string_view name,
                                                    std::function<void(const PackedArgs&)> handler);
    // One the client connected to an engine signal itself (an instance's
    // ChildAdded), or a thread's :Wait() on one, listed under the signal's name.
    void noteNative(const Instance& owner, std::string_view name, const std::shared_ptr<NativeConnection>& native);
    // The native connections still connected to the event, oldest first.
    std::vector<std::shared_ptr<NativeConnection>> nativeConnections(const Instance& owner, std::string_view name);

private:
    struct Entry {
        std::weak_ptr<Instance> owner;
        Connection engineEvents;
        Connection destroying;
        std::vector<Connection> bridged; // typed engine signals (bridgeEngineSignals)
        std::map<std::string, std::unique_ptr<Event>, std::less<>> events;
        std::vector<std::pair<std::string, std::weak_ptr<NativeConnection>>> natives;
    };
    Entry* entry(const Instance& owner, bool create);
    std::unordered_map<const Instance*, Entry> entries_;
};

// Relays an instance's typed engine signals (a Humanoid's events) to the
// hub's events of the same Roblox names. The hub calls it once per instance
// it tracks and drops the connections with it (HumanoidApi.cpp).
std::vector<Connection> bridgeEngineSignals(EventHub& hub, Instance& owner);
std::vector<Connection> bridgeDescriptionSignals(EventHub& hub, HumanoidDescription& owner);

// An RBXScriptSignal: the instance it belongs to and which signal of its own
// it is. Builtin is one of the engine's own Signals (ChildAdded, Changed...),
// Property and Attribute the signals Instance:GetPropertyChangedSignal and
// GetAttributeChangedSignal make, and Hub one of the events the engine fires by
// name (Touched, MouseClick, OnServerEvent, the services' events).
struct SignalData {
    enum class Kind : std::uint8_t { Builtin, Property, Attribute, Hub };

    InstancePtr owner;
    Kind kind = Kind::Hub;
    int builtin = -1; // index into Events.cpp's builtins, for Kind::Builtin
    std::string name; // property, attribute or event name
    bool fireable = false; // the console's Signal.new: it has a Fire of its own
};

// The script side of a :Connect()ed callback; each fire runs it in a new thread.
struct Listener {
    Runtime* runtime = nullptr;
    std::weak_ptr<std::vector<int>> graveyard; // where the callback ref goes when we die
    Connection connection;
    int callback = LUA_NOREF;
    bool once = false;
    // What the console's Disable()/Enable() on a connection of ours turns off:
    // the connection stays, its handler doesn't run.
    bool enabled = true;
    // What its callback runs as: the access of the thread that connected it.
    ThreadAccess access;
    // The thread that connected it: the console's getconnections hands it
    // back as the connection's Thread.
    int thread = LUA_NOREF;
    std::string source; // "Instance.Event", for re-entrancy messages
    // The signal this listener is on, for getconnections(signal) to find it.
    std::weak_ptr<Instance> owner;
    SignalData::Kind kind = SignalData::Kind::Hub;
    std::string event;

    Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener();

    void disconnect(lua_State* L);
};

// Pushes resume arguments onto the thread and returns how many.
using ArgPusher = std::function_ref<int(lua_State*)>;

// Script-facing signals.
void pushSignal(lua_State* L, const Instance& owner, int builtin); // ChildAdded and friends
void pushPropertySignal(lua_State* L, const Instance& owner, std::string property);
void pushAttributeSignal(lua_State* L, const Instance& owner, std::string attribute);
void pushHubSignal(lua_State* L, const Instance& owner, std::string name, bool fireable = false);
int findBuiltinEvent(std::string_view name); // -1 when there's no such event
// getconnections(signal) (Console.cpp): the connections the VM holds on the
// signal at `idx`, as the tables an executor hands back.
int pushConnections(lua_State* L, int idx);
// Whether a value is a Luau closure rather than a C one (ConsoleDebug.cpp, which
// is where the VM's own structures are read), for the connections' LuaConnection.
bool consoleIsLuauClosure(lua_State* L, int idx);
// firesignal(signal, ...) (Console.cpp): every Luau connection on the signal
// at `idx` runs now with the arguments after it.
int fireConnections(lua_State* L, int idx);
// replicatesignal(signal, ...) (Console.cpp): the signal's event sent to the
// server with the arguments after `idx`, which must be what it takes.
int replicateSignal(lua_State* L, int idx);
// getsignalarguments(signal) and getsignalargumentsinfo(signal) (Console.cpp):
// what the signal at `idx` passes its handlers, as type names or as
// {Name, Type} tables.
int pushSignalArguments(lua_State* L, int idx, bool info);

// --- Profiling ---------------------------------------------------------------

// LUNAR_PROFILE's bookkeeping (Profiler.cpp), shared by the runtimes of one
// game; null in Shared unless profiling is on, so nothing here costs anything
// otherwise.
class Profiler {
public:
    // Times what runs from its making to finish() (a resumption, a compile)
    // for `script`, exclusive of scopes nested in it, which count for
    // themselves. Scopes finish in the reverse order of their making.
    class Scope {
    public:
        Scope(Profiler* profiler, const Instance* script, ScriptRole role, std::string_view what);
        void finish();

    private:
        Profiler* profiler_;
        const Instance* script_ = nullptr;
        ScriptRole role_ = ScriptRole::Standalone;
        std::string_view what_;
        double start_ = 0.0;
    };

    // A frame phase: the time since the last mark (or since begin) goes to `phase`.
    void beginFrame();
    void endPhase(SimulationProfile::Phase phase);

    SimulationProfile take();

private:
    struct Entry {
        std::weak_ptr<Instance> script;
        std::string name;
        std::string what;
        ScriptRole role = ScriptRole::Standalone;
        double seconds = 0.0;
        std::uint64_t resumes = 0;
    };
    void add(const Instance* script, ScriptRole role, std::string_view what, double seconds);

    std::unordered_map<std::string, Entry> entries_;
    std::vector<double> nested_; // per open scope: what its nested scopes took
    SimulationProfile profile_;
    double phaseMark_ = 0.0;
    double luauMark_ = 0.0;
    double luauTotal_ = 0.0;
};

// The script whose code a resumption runs: the `script` global of the
// thread's outermost function (or of the function about to start).
const Instance* resumedScript(lua_State* thread, int nargs);
// research, LUNAR_TRACE_SLOW (ScriptContext.cpp): whether it's on, its clock,
// and time spent in an engine member, listed with a slow resume's samples.
bool slowTraceOn();
double slowClock();
void noteSlowNative(std::string key, double seconds);

// --- Shared ------------------------------------------------------------------

// What every runtime of one game shares: the event hub, callbacks
// (OnServerInvoke and friends) and the loopback that carries remotes between
// the server and client contexts of Play Solo.
struct SharedState;
struct AnimationBridge;
struct NetworkInvokes;
struct TextChatState;
struct SocialState;

class Shared {
public:
    Shared();
    ~Shared();
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    EventHub events;
    std::vector<Runtime*> runtimes;
    std::unique_ptr<SharedState> state; // tweens, input, bound actions (Services.hpp)
    Connection tagWatch;
    Connection tagUnwatch;
    InstancePtr userSettings;          // UserSettings()
    InstancePtr globalSettings;        // settings(), which only the console can call
    InstancePtr pluginManager;         // PluginManager(), the same
    std::unique_ptr<ToolSystem> tools; // equips the game's Tools (startTools)
    // Shows the client's ProximityPrompts (startPrompts, stepped on clients).
    std::unique_ptr<PromptSystem> prompts;
    // ReplicatedFirst:RemoveDefaultLoadingScreen() was called: the app takes
    // its loading screen down (ScriptContext::defaultLoadingScreenRemoved).
    bool defaultLoadingScreenRemoved = false;
    // Players' avatar and user lookups; null makes one over libcurl with the
    // default login on first use (ScriptContext::setWeb).
    std::shared_ptr<const assets::RobloxWeb> web;

    struct Callback {
        Runtime* runtime = nullptr;
        int ref = LUA_NOREF;
        // What it runs as: the access and globals of the thread that set
        // it, as a connection's handler keeps its connector's.
        ThreadAccess access;
        int globals = LUA_NOREF;
    };
    void setCallback(const Instance& owner, std::string_view name, Runtime* runtime, int ref,
                     ThreadAccess access = {}, int globals = LUA_NOREF);
    std::optional<Callback> callback(const Instance& owner, std::string_view name) const;
    void dropCallbacks(Runtime* runtime);

    // Remotes on a real server (ScriptContext::setRemoteSink): where they
    // go, and the RemoteFunction calls waiting for the server's answer.
    RemoteSink remoteSink;
    EventShape eventShape; // what the server's events take (ScriptContext::setEventShape)
    std::shared_ptr<NetworkInvokes> networkInvokes;
    // TextChatService's client side (TextChat.cpp): the chat sink and the
    // messages of ours waiting for the server.
    std::shared_ptr<TextChatState> textChat;
    std::shared_ptr<SocialState> social;
    // Player:RequestStreamAroundAsync on a real server
    // (ScriptContext::setStreamAroundHook): the calls waiting for the area.
    StreamAroundHook streamAroundHook;
    struct StreamAroundWait {
        Runtime* caller = nullptr;
        lua_State* thread = nullptr;
        std::uint64_t token = 0;
    };
    std::map<std::uint64_t, StreamAroundWait> streamArounds;
    std::uint64_t lastStreamAround = 0;

    // A remote message waiting for its receiving side. Remotes fired before
    // anyone listens queue up like Roblox's, until a listener connects.
    struct Message {
        std::weak_ptr<Instance> remote;
        std::string event; // OnServerEvent or OnClientEvent
        PackedArgs args;
    };
    std::deque<Message> inbox;
    // Remote function calls in flight: run on the receiving side next frame.
    std::deque<std::move_only_function<void()>> invocations;

    Runtime* runtimeFor(ScriptRole role) const;
    void deliverMessages();

    // Hooks the embedding application installs (see ScriptContext.hpp).
    RaycastHook raycast;
    std::function<std::optional<std::string>(std::string_view source)> networkBytecode;
    // ScriptContext::setLaunchHandler's: who joins the games the app launches.
    std::function<void(const ScriptContext::LaunchRequest&)> launchHandler;
    // ScriptContext::setTextMeasure's: the GUI's own text layout, if any.
    std::function<Vector2(std::string_view text, const Font& font, float size, float width, bool richText)> textMeasure;
    std::function<bool(std::string_view)> imageLoaded;
    ScriptContext::GuiHitTest guiHitTest;
    ScriptContext::GuiFocus guiFocus;
    std::function<void(Instance*)> guiLayoutQuery;
    std::function<bool()> dataUpdateInProgress;
    ScriptContext::GameInvitePromptHandler gameInvitePrompt;
    struct PlayerPolicy {
        double expires = 0;
        Packed fields;
    };
    std::map<std::int64_t, PlayerPolicy> playerPolicies;
    struct ImageState {
        std::weak_ptr<Instance> instance;
        std::string content;
        bool loaded = false;
    };
    std::unordered_map<const Instance*, ImageState> imageStates;
    std::string platform = "OSX"; // UserInputService:GetPlatform(), an Enum.Platform name
    std::function<InstancePtr(Player&)> characterFactory;
    // LoadAnimation's side (Animation.cpp), made on first use.
    std::shared_ptr<AnimationBridge> animation;
    // Set while profiling (PlaySession::setProfiling).
    std::unique_ptr<Profiler> profiler;

    // Modules whose require() result Lunarstone makes itself instead of
    // running their Source: the default PlayerModule's stand-ins (PlayerModule.cpp).
    using NativeModule = void (*)(lua_State* L, Instance& module);
    void setNativeModule(Instance& module, NativeModule push);
    NativeModule nativeModule(const Instance& module) const;
    // ControlModule:Enable/Disable: whether the local player's controls move the character.
    bool controlsEnabled = true;
    // What the console's isrbxactive, setclipboard and setfpscap reach the
    // application through, and what it stores of what they were told.
    std::function<bool()> active;
    std::function<void(std::string_view)> clipboard;
    std::function<void(int)> frameCap;
    int fpsCap = 0;
    // The round trip to the server, in seconds (ScriptContext::setNetworkPing):
    // what Stats' "Data Ping" reads. None off a real server.
    std::function<double()> networkPing;
    // And keypress, mouse1press and mousemoverel's way to the window.
    std::function<void(const ScriptContext::HostInput&)> input;
    // A playtest's screenshot and finish, for the console's playtest table.
    ScriptContext::PlaytestHost playtest;
    // What queue_on_teleport stored, until the next teleport takes it.
    std::string teleportCode;

private:
    struct CallbackKey {
        std::weak_ptr<Instance> owner;
        std::string name;
        Callback callback;
    };
    std::vector<CallbackKey> callbacks_;
    std::unordered_map<const Instance*, std::pair<std::weak_ptr<Instance>, NativeModule>> nativeModules_;
};

// 64 random bits for the scripts: the machine's entropy, or the sequence
// seedScripts started.
std::uint64_t scriptEntropy();
// Seconds since the Unix epoch as the scripts see them: the system clock, or
// under simulateClocks its epoch plus the calling context's simulated time.
double scriptWallClock(lua_State* L);
// Whether simulateClocks is on (it replaces os.clock and os.time).
bool clocksSimulated();

// --- Runtime -----------------------------------------------------------------

class Runtime {
public:
    Runtime(std::shared_ptr<DataModel> game, ScriptRole role, std::shared_ptr<Shared> shared);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static Runtime& of(lua_State* L);

    lua_State* main() const { return main_; }
    DataModel& game() const { return *game_; }
    const std::shared_ptr<DataModel>& gamePtr() const { return game_; }
    Shared& shared() const { return *shared_; }
    const std::shared_ptr<Shared>& sharedPtr() const { return shared_; }
    ScriptRole role() const { return role_; }
    bool isClient() const { return role_ != ScriptRole::Server; }
    bool isServer() const { return role_ != ScriptRole::Client; }
    double now() const { return static_cast<double>(clock_); }

    void setOutput(OutputSink sink) { output_ = std::move(sink); }
    void setTimeout(double seconds) { timeout_ = seconds; }
    // Output the client would print: to the sink, LogService.MessageOut and
    // the log history.
    void output(MessageType type, std::string_view message) const;
    // Lunarstone's own notices (members it doesn't implement, kicks): to the
    // sink only. A real client prints no such thing, so scripts don't see them.
    void notice(MessageType type, std::string_view message) const;
    void report(const ScriptError& error) const;

    // An entry of LogService:GetLogHistory().
    struct LogEntry {
        std::string message;
        MessageType type = MessageType::Output;
        double timestamp = 0.0; // Unix seconds
    };
    const std::deque<LogEntry>& logHistory() const { return logHistory_; }
    void setSignalBehavior(SignalBehavior behavior) { signalBehavior_ = behavior; }
    bool deferredSignals() const;
    bool dataUpdateInProgress() const;

    // Loads bytecode into a fresh sandboxed thread and runs it to its first yield.
    std::expected<void, ScriptError> runChunk(std::string_view bytecode, std::string_view chunkName, Instance* script,
                                              RunAccess access = RunAccess::Game);
    std::expected<std::vector<double>, ScriptError> callChunk(std::string_view bytecode, std::string_view chunkName,
                                                              std::span<const double> args);
    // A script's Source as Luau bytecode: compiled when it's text, unwrapped
    // (Shared::networkBytecode) and unscrambled when the server sent it as
    // Roblox's protected bytecode. Errors are compile errors.
    std::expected<std::string, std::string> codeOf(std::string_view source, std::string_view chunkName) const;
    // The same as the client holds it, opcodes still encoded the way the
    // server's compiler writes them (op * 227), in the oldest format that
    // has what it uses (version 10 at most, which decompilers know): what
    // the executors' getscriptbytecode hands out. Text is compiled and
    // encoded alike.
    std::expected<std::string, std::string> sentCodeOf(std::string_view source, std::string_view chunkName) const;
    // A fresh sandboxed thread with `script` set, its function loaded from
    // bytecode, left on the main stack. With Full access it runs as the
    // console, in a console run's globals (openConsole). Errors are the load
    // error.
    std::expected<lua_State*, ScriptError> loadThread(std::string_view bytecode, std::string_view chunkName,
                                                      Instance* script, RunAccess access = RunAccess::Game);

    // Runs a thread whose function and arguments are already on its stack (or
    // a suspended one with nargs resume values on top). The caller keeps the
    // thread reachable. `what` says what resumed it, for the profiler.
    // `raising` resumes a suspended thread by throwing the value on top of its
    // stack where it yielded (lua_resumeerror), for calls that failed.
    std::optional<ScriptError> resume(lua_State* thread, int nargs, std::string_view what = {}, bool raising = false);

    // Called once the thread finishes: with its results on its stack (status
    // LUA_OK) or after an error (status is the error code, the message on top).
    using Finisher = std::move_only_function<void(lua_State* thread, int status, int results)>;
    void onFinish(lua_State* thread, Finisher finisher);

    // Anchors a thread that's about to yield (or hasn't started) until wake()
    // or cancel(). `connection` is dropped when it wakes. Returns the token a
    // waker has to present, so a stale wakeup can't resume it twice.
    std::uint64_t suspend(lua_State* thread, Connection connection = {});
    bool isSuspended(lua_State* thread, std::uint64_t token) const;
    // Gives a suspension the engine connection that will wake it.
    void attach(lua_State* thread, std::uint64_t token, Connection connection);
    // Resumes the thread with whatever push leaves on its stack. False if the
    // token is stale (already woken or cancelled).
    bool wake(lua_State* thread, std::uint64_t token, ArgPusher push, std::string_view what = {});
    // Resumes the thread with an error raised where it yielded: the yielding
    // call fails with that message, which a pcall around it catches.
    bool fail(lua_State* thread, std::uint64_t token, std::string_view message, std::string_view what = {});
    // Like wake, but honours deferred signal behaviour: the resumption is
    // queued instead of run when signals are deferred.
    void wakeWith(lua_State* thread, std::uint64_t token, PackedArgs args, std::string what = {});
    // Drops the suspension; cancel also kills the thread.
    void forget(lua_State* thread);
    void cancel(lua_State* thread);

    // task.wait/task.delay timers resume in the Heartbeat phase; the legacy
    // wait/spawn/delay ones in their own phase earlier in the frame.
    void after(double delay, std::move_only_function<void()> action);
    void afterLegacy(double delay, std::move_only_function<void()> action);
    void defer(std::move_only_function<void()> action);

    // Frame phases (FrameDriver runs them in Roblox's order).
    void advance(double dt);
    void resumeTimers();
    void resumeLegacyTimers();
    void flushDeferred();
    void renderStep(double dt);
    bool idle() const;

    // A listener tracked so shutdown can disconnect it from the engine.
    std::shared_ptr<Listener> newListener();
    // The listeners still connected, for the console's getconnections.
    std::vector<std::shared_ptr<Listener>> listeners() const;
    // Runs a listener for one fire, now or deferred depending on
    // SignalBehavior; `now` runs it now whatever that says (the console's
    // firesignal and a connection's Fire).
    void dispatch(const std::shared_ptr<Listener>& listener, const PackedArgs& args, bool now = false);

    // Drops registry refs released while we couldn't touch the VM.
    void collectRefs();
    void stepCollector();

    // Scripts: starts one (once), stops one, and the threads it owns.
    std::expected<void, ScriptError> startScript(Instance& script);
    void stopScript(const Instance& script);
    bool isRunning(const Instance& script) const;
    // Whether a running script started under an Actor, where Roblox runs it
    // in that Actor's VM rather than the main one (moving into one later
    // doesn't move it).
    bool startedInActor(const Instance& script) const;
    // The Actors the running scripts started under, each once (getactors).
    std::vector<InstancePtr> runningActors() const;
    // The globals a running script writes to, for getsenv(script); LUA_NOREF
    // for one that isn't running here.
    int environmentOf(const Instance& script) const;
    // The modules whose require() has finished, for getloadedmodules().
    std::vector<InstancePtr> loadedModules() const;

    // What the thread may reach (see ThreadAccess). A thread that has no
    // record of its own answers as a game script does.
    ThreadAccess accessOf(lua_State* thread) const;
    void setAccess(lua_State* thread, ThreadAccess access);
    // The globals a callback's thread runs with: those of the code that set
    // the callback up, as the engine makes that thread from the connecting
    // one. A console run's handlers see its executor globals through
    // getfenv(0), a script's its own. refGlobals refs L's; useGlobals makes
    // what `ref` holds (those, or a thread whose they are) the thread's.
    static int refGlobals(lua_State* L);
    static void useGlobals(lua_State* thread, int ref);
    // Every thread of this VM but its main one, from when it's made until the
    // collector frees it (so a dead one may still be here): what the console
    // goes through to find a thread, instead of walking the whole heap.
    const std::unordered_set<lua_State*>& threads() const { return threads_; }

    // getgenv()'s table: made by the first console run and kept, so what one
    // run writes to it the next sees. LUA_NOREF until then.
    int consoleGlobals() const { return consoleGlobals_; }
    void setConsoleGlobals(int ref) { consoleGlobals_ = ref; }
    // Whether the console has hooked a function the compiler turns calls of
    // into FASTCALL (math.floor, typeof): no script's globals are a safe
    // environment from then on, this VM's new ones included, so those calls
    // reach the hook (see ConsoleHooks.cpp).
    bool builtinsHooked() const { return builtinsHooked_; }
    void setBuiltinsHooked() { builtinsHooked_ = true; }
    // A connection the console asked about, and the signal it is on, held
    // together: the table getconnections handed out outlives both, and its
    // Fire and Disconnect still need both alive.
    struct ConsoleConnection {
        std::shared_ptr<Listener> listener;
        SignalData signal;
    };
    ConsoleConnection* keepConnection(std::shared_ptr<Listener> listener, SignalData signal);
    // Instances the VM has handed to scripts, weakly: what getnilinstances()
    // looks through for the ones left with no parent.
    void noteInstance(const Instance& instance);

    // The console's isscriptable/setscriptable: what this console has made a
    // member of this instance scriptable, by the instance's full name and the
    // member's name. Without one it is what Roblox's reflection says, which is
    // what the API data's capabilities and its hidden members tell us.
    bool scriptableOverride(const Instance& instance, std::string_view name);
    void setScriptableOverride(const Instance& instance, std::string_view name, bool value);

    // require(): the module's cached result, or loads it.
    int require(lua_State* L, Instance& module);

    // RunService:BindToRenderStep. `thread` is a ref to the thread that bound it.
    void bindToRenderStep(std::string name, int priority, int ref, int thread, ThreadAccess access, int globals);
    void unbindFromRenderStep(std::string_view name);
    // The bindings in the order they run, each {Name, Priority, Function, Thread}.
    int pushRenderBindings(lua_State* L) const;

    // Luau registry refs to the shared tables the libraries use.
    int instanceCache = LUA_NOREF;
    int instanceMethods = LUA_NOREF;
    int unimplementedMethods = LUA_NOREF;

private:
    struct Timer {
        double at = 0.0;
        std::uint64_t seq = 0;
        std::move_only_function<void()> action;
    };
    struct Suspension {
        std::uint64_t token = 0;
        int ref = LUA_NOREF;
        Connection connection;
    };
    struct ScriptRecord {
        std::weak_ptr<Instance> script;
        int env = LUA_NOREF; // the script's globals table
        bool actor = false;  // it started under an Actor
    };
    struct Module {
        std::weak_ptr<Instance> module;
        enum class State : std::uint8_t { Loading, Loaded, Failed } state = State::Loading;
        int result = LUA_NOREF;
        lua_State* thread = nullptr; // while loading
        int threadRef = LUA_NOREF;
        const Instance* requiredBy = nullptr; // the module whose loading required this one
        std::string error;
        std::vector<std::pair<lua_State*, std::uint64_t>> waiting;
    };
    struct RenderBinding {
        std::string name;
        int priority = 0;
        std::uint64_t seq = 0;
        int ref = LUA_NOREF;
        int thread = LUA_NOREF; // the thread that bound it
        ThreadAccess access; // its access, which the callbacks keep
        int globals = LUA_NOREF; // and its globals (useGlobals)
    };

    static void interrupt(lua_State* L, int gc);
    // A thread is made (its parent is `from`) or destroyed (no parent): the
    // first gets its parent's access, the second gives its slot back.
    static void threadMade(lua_State* from, lua_State* thread);
    void openGlobals();
    void runTimers(std::vector<Timer>& timers);
    void finishModule(const Instance* key, lua_State* thread, int status, int results);

    std::shared_ptr<DataModel> game_;
    ScriptRole role_;
    std::shared_ptr<Shared> shared_;
    lua_State* main_ = nullptr;
    OutputSink output_;
    mutable std::deque<LogEntry> logHistory_;
    double timeout_ = 10.0;
    double deadline_ = 0.0; // steady-clock seconds, 0 when nothing is running
    std::uint32_t interrupts_ = 0;
    long double clock_ = 0.0L; // summed step()s; the extra precision keeps 30 * (1/60) == 0.5
    double collectorWork_ = 0.0;    // seconds stepCollector has spent on the cycle it is in
    double collectorResumeAt_ = 0.0; // steady-clock seconds: stepCollector rests until then
    lua_State* running_ = nullptr;
    int depth_ = 0;
    int eventDepth_ = 0;
    std::optional<SignalBehavior> signalBehavior_;

    std::uint64_t nextToken_ = 0;
    std::uint64_t nextSeq_ = 0;
    std::unordered_map<lua_State*, Suspension> suspended_;
    std::unordered_map<lua_State*, Finisher> finishers_;
    std::vector<Timer> timers_;       // min-heap on (at, seq)
    std::vector<Timer> legacyTimers_; // same
    std::deque<std::move_only_function<void()>> deferred_;
    std::vector<std::weak_ptr<Listener>> listeners_;
    std::shared_ptr<std::vector<int>> graveyard_ = std::make_shared<std::vector<int>>();
    std::unordered_map<const Instance*, ScriptRecord> scripts_;
    std::unordered_map<const Instance*, Module> modules_;
    std::vector<RenderBinding> renderBindings_;
    // One record per live thread, its index in the thread's own storage;
    // freedAccess_ holds the records of the threads that have gone.
    std::deque<ThreadAccess> access_;
    std::vector<std::size_t> freedAccess_;
    std::unordered_set<lua_State*> threads_;
    int consoleGlobals_ = LUA_NOREF;
    bool builtinsHooked_ = false;
    std::vector<std::unique_ptr<ConsoleConnection>> consoleConnections_;
};

// An item of a client enum by name (the Enum global's catalog), empty if unknown.
EnumItem enumItemNamed(std::string_view enumName, std::string_view itemName);

// require's continuation, for the global's closure.
lua_Continuation requireContinuation();

// What the client inserts into StarterPlayerScripts when the place doesn't
// bring its own (PlayerModule.cpp): stand-ins for the default PlayerModule,
// PlayerScriptsLoader and RbxCharacterSounds. Returns whether it added a
// PlayerModule (whose copies registerPlayerModule then picks up).
bool installDefaultPlayerScripts(Shared& shared, DataModel& game);
// Makes the PlayerModule stand-in under `playerScripts` (a copy of the
// installed one) require like the original.
void registerPlayerModule(Shared& shared, Instance& playerScripts);

// The script a thread belongs to (its `script` global), null for none.
Instance* currentScript(lua_State* L);
// The script whose code calls: the `script` of the calling function's
// environment (a module's functions keep their module's, whichever thread runs
// them), which require()'s paths and getcallingscript() resolve against.
Instance* callingScript(lua_State* L);

} // namespace lunar::lua
