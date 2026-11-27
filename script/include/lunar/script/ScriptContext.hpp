#pragma once

#include <lunar/Classes.hpp>
#include <lunar/GameInvite.hpp>
#include <lunar/RemoteValue.hpp>
#include <lunar/script/TextChat.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace lunar {

class PromptSystem;

namespace lua {
class Runtime;
class Shared;
} // namespace lua

namespace avatar {
class AnimationSystem;
} // namespace avatar

namespace assets {
class RobloxWeb;
} // namespace assets

enum class MessageType : std::uint8_t {
    Output,  // print
    Info,    // tracebacks and other engine notes
    Warning, // warn
    Error,   // errors from threads nobody is waiting on (events, task.spawn, resumed waits)
};

using OutputSink = std::function<void(MessageType, std::string_view)>;

struct ScriptError {
    std::string message;   // "Workspace.Script:3: attempt to index nil with 'Foo'"
    std::string traceback; // one "chunk:line function name" line per frame, may be empty
};

// Which side of the game a context plays. Standalone is one context doing
// everything (tests, --run-file): RunService says it's both client and server.
enum class ScriptRole : std::uint8_t { Standalone, Server, Client };

// Workspace.SignalBehavior as the scheduler applies it. Roblox's Default
// means Immediate in the 0.742 client: the DebugForceDeferredSignalBehavior
// flag it consults for Default ships as false.
enum class SignalBehavior : std::uint8_t { Immediate, Deferred };

// How much code run from outside the game may reach. Game is what a LocalScript
// or Script runs as: identity 2 and none of the capabilities. Full is the menu's
// Luau tab (the F9 console's stand-in): identity 8, every capability, and the
// globals an executor offers, which no game script can see.
enum class RunAccess : std::uint8_t { Game, Full };

// Workspace:Raycast, supplied by the physics side. Origin and direction as
// the script gave them; the filter is RaycastParams.FilterDescendantsInstances.
struct RaycastQuery {
    Vector3 origin;
    Vector3 direction;
    std::vector<InstancePtr> filter;
    bool filterIsInclude = false;
    bool ignoreWater = false;
    bool respectCanCollide = false;
    std::string collisionGroup = "Default";
};

struct RaycastHit {
    InstancePtr instance;
    Vector3 position;
    Vector3 normal;
    Enum::Material material = Enum::Material::Plastic;
    float distance = 0.0f;
};

using RaycastHook = std::function<std::optional<RaycastHit>(const DataModel&, const RaycastQuery&)>;

// A remote's traffic with a real server, as the network carries it: the
// event's name in the network schema and its parameters in order, a Tuple's
// members last. What the client's scripts send:
//   FireServer(args...)          OnRemoteServerEvent(args...)
//   InvokeServer(args...)        RemoteOnInvokeServer(id, LocalPlayer, args...)
//   OnClientInvoke returning     RemoteOnInvokeSuccess(id, results...)
//   OnClientInvoke erroring      RemoteOnInvokeError(id, message)
// The id is the RemoteFunction's own invocation counter (1, 2, ...).
using RemoteSink = std::function<void(Instance& remote, std::string_view event, std::vector<RemoteValue> arguments)>;
// The parameters a real server's schema gives an instance's event, for the
// console's replicatesignal: each one's Luau type (typeof's answer), empty
// where any value goes, and "..." for a Tuple, which takes the rest. Nothing
// when the class has no such event.
using EventShape = std::function<std::optional<std::vector<std::string>>(const Instance& owner, std::string_view event)>;
// Player:RequestStreamAroundAsync on a real server: `request` names the call for ScriptContext::streamAroundDone.
using StreamAroundHook = std::function<void(std::uint64_t request, Vector3 position, double timeoutSeconds)>;

// Model:MoveTo's rule, which spawning a character follows too: anything
// collidable inside the model's box (parts, terrain) pushes the model straight
// up until it rests on top. `cast` is Workspace:Raycast; the model's own
// parts are filtered out. Returns how far the model rose.
float liftOutOfObstructions(Model& model, const std::function<std::optional<RaycastHit>(const RaycastQuery&)>& cast);

// Where simulated frames spent their time, in wall-clock seconds, for
// LUNAR_PROFILE: the frame loop's phases and the Luau that ran, by script and
// by what resumed it, like Studio's Script Performance window. Collected
// while profiling is on (PlaySession::setProfiling), between two takes.
struct ScriptActivity {
    std::string script; // full name of the script (or module) whose code ran, "?" when unknown
    std::string what;   // what resumed it: "RunService.Heartbeat", "task.wait", "start", "require"...
    ScriptRole role = ScriptRole::Standalone;
    double seconds = 0.0; // exclusive: handlers its code set off count for themselves
    std::uint64_t resumes = 0;
};

struct SimulationProfile {
    // runFrame's phases in order; each includes the Luau it ran and the
    // deferred resumptions flushed after it.
    enum Phase : std::uint8_t {
        Input,        // platform input, remotes and remote calls from last frame
        RenderStep,   // BindToRenderStep, RenderStepped, PreRender
        LegacyWait,   // wait(), spawn(), delay()
        PreAnimation, // RunService.PreAnimation
        Animation,    // the animation hook (Animators, the native Animate)
        Stepped,      // Stepped, PreSimulation
        Physics,      // the physics hook, Touched handlers, tool pickups
        Heartbeat,    // PostSimulation, Heartbeat
        Tweens,       // TweenService
        TaskWait,     // task.wait, task.delay
        PhaseCount,
    };
    static constexpr std::array<std::string_view, PhaseCount> phaseNames{
        "input",   "render step", "wait()",    "PreAnimation", "animation",
        "Stepped", "physics",     "Heartbeat", "tweens",       "task.wait",
    };

    int frames = 0;
    std::array<double, PhaseCount> phases{};
    std::array<double, PhaseCount> phaseLuau{}; // the part of each phase spent in Luau
    std::array<double, 3> luau{};               // Luau by ScriptRole (Standalone, Server, Client)
    // Most time first. Includes work outside frames (scripts starting in PlaySession::start).
    std::vector<ScriptActivity> activity;
};

// One Luau VM with Roblox's script environment: game, workspace, Instance,
// the datatypes, task and friends. Every script runs in its own sandboxed
// thread, so scripts share the built-in libraries (read-only) but not their
// globals. Waiting threads (task.wait, :Wait(), WaitForChild) resume from
// step(), which the caller drives with simulated time.
// Hands the engine every enum of the client (enums::findAny), so values of
// enums the engine doesn't model (a remote's Enum.EasingStyle) decode as
// EnumItems. Making a ScriptContext does it; call it before decoding
// anything without one.
void installEnumCatalog();

// Every random number the scripts are given -- math.random, Random.new()
// without a seed, HttpService:GenerateGUID, BrickColor.random -- from `seed`
// instead of the machine's entropy, so a run can be played again the same
// way (--playtest --seed). For the whole process; contexts made after the
// call start from it. nullopt goes back to the machine's entropy.
void seedScripts(std::optional<std::uint64_t> seed);
// The scripts' clocks -- tick, os.clock, os.time, DateTime.now and
// Workspace:GetServerTimeNow -- go by each context's simulated time from
// here on, the wall clock starting at `epoch` (Unix seconds) and os.clock
// at 0, so a run faster than real time (--playtest) looks to a game's
// cooldowns and hold timers like one at real speed. For the whole process;
// contexts made after the call use them. nullopt goes back to real clocks.
void simulateClocks(std::optional<double> epoch);

class ScriptContext {
public:
    explicit ScriptContext(std::shared_ptr<DataModel> game, ScriptRole role = ScriptRole::Standalone);
    // A context that shares events, callbacks and the remote loopback with
    // `peer`: the other half of a Play Solo pair.
    ScriptContext(std::shared_ptr<DataModel> game, ScriptRole role, const ScriptContext& peer);
    ~ScriptContext();
    ScriptContext(const ScriptContext&) = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;

    // Where print, warn and asynchronous errors go. Defaults to stdout/stderr.
    void setOutput(OutputSink sink);
    // Longest a script may run without yielding before it's stopped with
    // Roblox's "Script timeout" error. Zero turns the limit off.
    void setTimeout(double seconds);
    // Overrides Workspace.SignalBehavior.
    void setSignalBehavior(SignalBehavior behavior);
    // Workspace:Raycast's implementation; without one, Raycast finds nothing.
    void setRaycastHook(RaycastHook hook);
    // The mouse icon the game asks for: UserInputService.MouseIcon, else the
    // local player's Mouse.Icon; empty for the platform's own, or none at
    // all with MouseIconEnabled off (see mouseIconShown).
    std::string mouseIcon() const;
    bool mouseIconShown() const;
    // UserGameSettings.RotationType is CameraRelative: the engine turns our
    // character to face the camera (a game's own Shift Lock sets it).
    bool cameraRelativeRotation() const;
    // A script called ReplicatedFirst:RemoveDefaultLoadingScreen(): the game
    // shows its own loading screen, so the app's comes down.
    bool defaultLoadingScreenRemoved() const;
    // The client's ProximityPrompts (stepped with its frames).
    PromptSystem* prompts() const;
    // Where LoadAnimation's tracks play. The caller steps it (FrameHooks::
    // animation) and keeps it alive until it sets another or null. Without
    // one the scripts make their own on first use and step it in the frame's
    // animation phase. Shared with the peer context.
    void setAnimationSystem(avatar::AnimationSystem* system);
    // Where Players' web lookups (GetHumanoidDescriptionFromUserId,
    // GetCharacterAppearanceInfoAsync, GetUserIdFromNameAsync...) go; by
    // default libcurl with the default login. Shared with the peer context.
    void setWeb(std::shared_ptr<const assets::RobloxWeb> web);
    // Live games send LocalScripts' and ModuleScripts' Source as Roblox's
    // protected bytecode: `decode` turns such a Source into Luau bytecode
    // with the client's scrambled opcodes (lunar::net::decodeProtectedBytecode)
    // and gives nothing for plain source text. Shared with the peer context.
    void setNetworkBytecode(std::function<std::optional<std::string>(std::string_view source)> decode);
    // How big text comes out in the GUI that draws it: the bounds of `text`
    // in `font` at `size` pixels, wrapped at `width` (0: one line), rich text
    // markup read when `richText`. TextService:GetTextBoundsAsync answers
    // with it; without one it estimates.
    using TextMeasure
        = std::function<Vector2(std::string_view text, const Font& font, float size, float width, bool richText)>;
    void setTextMeasure(TextMeasure measure);
    // The GUI's nonblocking image lookup, for ImageLabel/ImageButton.IsLoaded
    // and its property-changed signal. Null clears the observed images.
    void setImageLoadedChecker(std::function<bool(std::string_view)> checker);
    using GuiHitTest = std::function<std::vector<InstancePtr>(float x, float y)>;
    void setGuiHitTest(GuiHitTest hitTest);
    using GuiFocus = std::function<void(TextBox*, bool capture, bool enterPressed)>;
    void setGuiFocus(GuiFocus focus);
    void setGuiLayoutQuery(std::function<void(Instance*)> query);
    // A host transaction is still applying related DataModel changes (for
    // example, a replication packet). Queue signals, WaitForChild resumes
    // and new script starts until the transaction is complete.
    void setDataUpdateInProgress(std::function<bool()> updating);
    using GameInvitePromptHandler = std::function<bool(std::shared_ptr<GameInviteSession>)>;
    void setGameInvitePromptHandler(GameInvitePromptHandler handler);
    void stepGuiPage(UIPageLayout& layout, int step);
    std::function<void(UIPageLayout&, int)> guiPageStepper() const;
    // A game the Lua app asks the native shell to join: ExperienceService's
    // LaunchExperience and the "Game.launch" bus message. The place, and when
    // given, the server (gameInstanceId), the friend to follow (userId) or
    // the private server's codes.
    struct LaunchRequest {
        std::uint64_t placeId = 0;
        std::uint64_t userId = 0;
        std::string gameInstanceId;
        std::string linkCode;
        std::string accessCode;
        std::string joinAttemptId;
    };
    void setLaunchHandler(std::function<void(const LaunchRequest&)> handler);
    // Every script of this context runs as the client's own CoreScripts do,
    // with all of the API (RobloxScript and the rest) but none of the
    // console's extra globals: the Lua app's DataModel holds nothing else.
    // Applies to the threads made from here on.
    void setCoreScriptAccess();
    // Roblox's client settings as it serves them to a client
    // ({"applicationSettings": {"FFlagX": "True", ...}}): DefineFastFlag and
    // the rest answer with these over the Lua code's defaults. Process-wide,
    // as the client's flags are. How many it read, nothing when the text
    // isn't that.
    static std::optional<std::size_t> loadClientSettings(std::string_view json);
    // An AppStorageService item, as the native shell around the Lua app
    // writes them (the signed-in user's UserId, Username...). Process-wide.
    static void setAppStorageItem(std::string_view key, std::string_view value);
    // A native app configuration item (GUAC:<userId>:app-policy, etc.).
    // Updates MemStorageService and notifies its bound Lua subscribers.
    void setMemStorageItem(std::string_view key, std::string_view value);
    // The Enum.Platform UserInputService:GetPlatform() reports: "OSX" (the
    // build Lunarstone stands in for) unless told otherwise. It stays on
    // this client: nothing sends it anywhere. Game scripts can't call
    // GetPlatform (RobloxScriptSecurity), so only code with that capability
    // would see it. Shared with the peer context.
    // False (and no change) for a name Enum.Platform doesn't have.
    bool setPlatform(std::string_view enumName);
    // Sends the scripts' remotes (FireServer, InvokeServer, the answers of
    // OnClientInvoke) to `sink` instead of the Play Solo loopback: a client
    // of a real server. Shared with the peer context.
    void setRemoteSink(RemoteSink sink);
    // What the server's schema says an event takes (see EventShape), which
    // the console's replicatesignal holds its arguments to.
    void setEventShape(EventShape shape);
    // What a real server sent for a remote (see RemoteSink): OnClientEvent(
    // args...), RemoteOnInvokeClient(id, args...) and the answers to
    // InvokeServer, RemoteOnInvokeSuccess(id, results...) and
    // RemoteOnInvokeError(id, message). Scripts see it from the next frame
    // (OnClientEvent waits for a listener, an invocation for OnClientInvoke).
    // False when it isn't a remote's event.
    bool receiveRemote(Instance& remote, std::string_view event, std::span<const RemoteValue> arguments);
    // Player:RequestStreamAroundAsync in a game that streams its content: the
    // hook asks the server for the area (lunar::net::StreamingClient::
    // requestStreamAround) and the calling script waits until
    // streamAroundDone(request). Without a hook the call returns at once.
    // Shared with the peer context.
    void setStreamAroundHook(StreamAroundHook hook);
    void streamAroundDone(std::uint64_t request);

    // Whether the game window has the focus, which the console's isrbxactive
    // reports. Nothing is focused without a hook: there may be no window at all
    // (--run, --console-run, the tests). Shared with the peer context.
    void setActive(std::function<bool()> hook);
    // Where the console's setclipboard puts its text: the window's clipboard,
    // which needs a compositor connection the platform layer owns. Without a
    // hook the console's text goes nowhere. Shared with the peer context.
    void setClipboard(std::function<void(std::string_view)> hook);
    // The frames a second the console's setfpscap asks the frame loop for; 0 is
    // no cap. Without a hook the value is only kept to be read back.
    void setFrameCap(std::function<void(int)> hook);
    // The round trip to the server in seconds, as the connection measures it:
    // what Stats.Network.ServerStatsItem["Data Ping"] answers. Without a hook
    // it answers 0 (there is no server).
    void setNetworkPing(std::function<double()> hook);
    // What the console's keypress, mouse1press, mousemoverel and their
    // neighbours ask the window to have happened, as if the keyboard and mouse
    // had: the window takes it as input of its own, so the game sees it the
    // way it sees the player's. Without a hook it goes nowhere.
    struct HostInput {
        enum class Kind : std::uint8_t { Key, Button, MoveBy, MoveTo, Scroll } kind = Kind::Key;
        Enum::KeyCode key{}; // Key
        int button = 1;      // Button: 1 left, 2 right, 3 middle
        bool pressed = false;
        // MoveBy: pixels of relative motion; MoveTo: a point in the window;
        // Scroll: y in wheel notches, up (away from the user) positive.
        float x = 0, y = 0;
    };
    void setInput(std::function<void(const HostInput&)> hook);
    // What `lunarstone --playtest` gives the test it runs in the console: a
    // `playtest` table of the console's own (no such global without it), whose
    // screenshot(name) asks for a picture of the frame the test is on and
    // finish(passed, message) ends the run. Set before the test runs.
    struct PlaytestHost {
        std::function<void(std::string_view name)> screenshot;
        std::function<void(bool passed, std::string_view message)> finish;
    };
    void setPlaytest(PlaytestHost host);
    // Runs what the console's queue_on_teleport stored, in the console, and
    // forgets it -- what an application calls once a teleport has happened. The
    // code is gone whatever the run does with it.
    void runTeleportCode();

    // TextChatService on a client (TextChat.cpp): every message its chat
    // window would show, after the game's OnIncomingMessage callbacks, goes
    // to `sink`, our own first as Sending and again once the server answers;
    // hidden ones (UserMessageIntentSentRemote), system lines and bubbles
    // too. Shared with the peer context.
    void setChatSink(std::function<void(const ChatLine&)> sink);
    // A message typed into the chat bar, sent the way the core chat sends it:
    // TextChannel:SendAsync on ChatInputBarConfiguration.TargetTextChannel,
    // else TextChatService.TextChannels.RBXGeneral, from the local player.
    // The message id, or why it couldn't go (no local player, no channel, no
    // TextSource of ours in it, nothing left after trimming).
    std::expected<std::string, std::string> sendChat(std::string_view text, std::string_view channel = {});
    // The core chat's server-mediated private-channel request. The returned
    // channel must contain both participants; refusal never selects General.
    void requestWhisperChannel(std::int64_t target, std::function<void(std::expected<std::string, std::string>)> done);
    void playChatEmote(std::string name, std::function<void(std::expected<void, std::string>)> done);
    // Preset chat as the server allows it, and a phrase of its catalog sent
    // like TextChannel:SendPresetAsync: the phrase's text with its presetId
    // and the catalog version in ClientToServerMessageReplicateSignalV4.
    PresetChatAccess presetChatAccess() const;
    std::expected<std::string, std::string> sendPresetChat(const ChatPreset& preset);

    // Luau bytecode, or a "chunkName:line: message" syntax error. Debug level 2
    // is the console's: every local keeps its register and its name, where 1
    // (the game's scripts, as the server compiles them) drops a constant local
    // nothing reads, which debug.getstack and the executors' tests count on.
    static std::expected<std::string, std::string> compile(std::string_view source, std::string_view chunkName,
                                                           int debugLevel = 1);

    // Compiles source and runs it in a fresh thread until it finishes or first
    // yields. Errors raised before that come back here; later ones go to the
    // output sink. `script` becomes the script global. `access` is how much the
    // code may reach: RunAccess::Game for the game's own scripts (what every
    // caller passed), RunAccess::Full for the menu's Luau tab.
    std::expected<void, ScriptError> run(std::string_view source, std::string_view chunkName = "Script",
                                         Instance* script = nullptr, RunAccess access = RunAccess::Game);
    // The Escape menu's Luau tab: like run() at RunAccess::Full. A first line of
    // `--!server` asks for the server's context, which local play has (`server`)
    // and a client of a live game has none of: that answers "There is no server
    // here to run on." The line itself is a Luau comment, and is trimmed away.
    std::expected<void, ScriptError> runConsole(std::string_view code, ScriptContext* server = nullptr);
    // Runs Luau bytecode as a function of `args` (numbers) until it returns,
    // and gives back its results that are numbers (others as NaN). With
    // `scrambledOpcodes`, the opcode bytes are first divided by 227 the way
    // the Roblox client loads server-sent bytecode. Used to answer the
    // server's bytecode challenge (0x9b).
    std::expected<std::vector<double>, ScriptError> callBytecode(std::string bytecode, std::string_view chunkName,
                                                                 std::span<const double> args,
                                                                 bool scrambledOpcodes = false);
    // Runs a Script instance's Source, named after the script's full name.
    // Each script instance only ever starts once per context.
    std::expected<void, ScriptError> runScript(Instance& script);
    // Stops a running script: its waiting threads die and its connections drop.
    void stopScript(const Instance& script);

    // One frame of simulated time for this context alone: the RunService
    // events and every thread that's due resume in Roblox's order (see
    // FrameDriver in PlaySession.hpp).
    void step(double dt);
    // Simulated seconds since the context started, what time() returns.
    double time() const;
    // True when no thread is scheduled to resume, so stepping changes nothing
    // until something outside the scripts fires an event.
    bool idle() const;

    ScriptRole role() const;
    DataModel& game() const;
    lua_State* state() const;
    lua::Runtime& runtime() const { return *runtime_; }

private:
    std::unique_ptr<lua::Runtime> runtime_;
};

// Server Scripts a running game would start: every Script under Workspace or
// ServerScriptService that isn't disabled, in tree order. LocalScripts and
// ModuleScripts are left out.
std::vector<InstancePtr> serverScripts(const DataModel& game);

// Whether Roblox would run this script in a context of the given role where
// it sits now: Scripts by RunContext and container, LocalScripts only under
// a player's Backpack, PlayerGui, PlayerScripts or character, or ReplicatedFirst.
bool runsIn(const Instance& script, ScriptRole role);

} // namespace lunar
