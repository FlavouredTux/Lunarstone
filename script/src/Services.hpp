#pragma once

// State and helpers behind the services: tweens, input and bound actions,
// character spawning, tag watching. Lives in Shared, so the server and client
// contexts of Play Solo see one game.

#include "Userdata.hpp"

#include <atomic>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar::assets {
class RobloxWeb;
} // namespace lunar::assets

namespace lunar::lua {

// Where scripts' web requests go (ScriptContext::setWeb), made over libcurl
// with the default login on first use; raises when LUNAR_OFFLINE is set.
const assets::RobloxWeb& robloxWeb(lua_State* L);

// --- Tweens ------------------------------------------------------------------

enum class TweenCommand : std::uint8_t { Play, Pause, Cancel };

struct TweenTrack {
    std::string property;
    Value start; // read when the tween starts playing
    Value goal;
};

// TweenService:GetValue: alpha through an easing style and direction.
double ease(double alpha, std::string_view style, std::string_view direction);
bool tweenable(const Value& value);
Value lerpValue(const Value& from, const Value& to, double t);

InstancePtr createTween(Shared& shared, Instance& target, const TweenInfo& info, std::vector<TweenTrack> tracks);
void commandTween(Shared& shared, Instance& instance, TweenCommand command);
// Advances every playing tween; runs in the frame's Heartbeat phase.
void stepTweens(Shared& shared, double dt);
// A Motor (or Motor6D) given a DesiredAngle turns its CurrentAngle towards
// it, MaxVelocity radians a 60th of a second, until it gets there.
void turnMotor(Shared& shared, Instance& motor);
void stepMotors(Shared& shared, double dt);

// --- Page layouts ------------------------------------------------------------------

// A UIPageLayout moving to `page` (one of its pages): PageLeave and
// PageEnter now, then the slide stepPageMoves eases over TweenTime, and
// Stopped once it's there (the next frame when it isn't Animated).
void jumpToPage(Shared& shared, UIPageLayout& layout, GuiObject& page);
void stepPageMoves(Shared& shared, double dt);

struct PageMove {
    std::weak_ptr<Instance> layout;
    double elapsed = 0.0;
};

// --- Input ---------------------------------------------------------------------

// What UserInputService knows about the devices, fed by the platform layer.
struct InputState {
    std::set<Enum::KeyCode> keys;
    std::set<Enum::UserInputType> buttons;
    std::set<Enum::KeyCode> processedKeys;
    std::set<Enum::UserInputType> processedButtons;
    Vector2 mousePosition;
    Vector2 mouseDelta;
    Enum::UserInputType lastInputType = Enum::UserInputType::Keyboard;
    // InputObjects of held keys, so InputEnded passes the same object.
    std::map<Enum::KeyCode, InstancePtr> keyObjects;
    std::map<Enum::UserInputType, InstancePtr> buttonObjects;
    InstancePtr mouseMovementObject;
    InstancePtr mouseWheelObject;

    bool keyDown(Enum::KeyCode key) const { return keys.contains(key); }
    bool mouseButtonDown(Enum::UserInputType type) const { return buttons.contains(type); }
    std::vector<InstancePtr> pressedKeyObjects() const;
};

Vector2 guiCoordinateOrigin(const DataModel& game);

// Index only input actions, rather than walking the replicated world for every
// mouse move. Button connections are refreshed when bindings change.
struct InputActionState {
    struct Button {
        std::weak_ptr<Instance> binding;
        std::weak_ptr<Instance> button;
        Connection events;
    };
    std::vector<std::weak_ptr<Instance>> actions;
    std::vector<Button> buttons;
    Connection added;
    bool initialized = false;
    ~InputActionState();
};
void refreshInputActions(Shared& shared, DataModel& game);
bool runInputActions(Shared& shared, Enum::KeyCode key, Enum::UserInputType type, Enum::UserInputState state,
                     Vector2 position, bool processed);

struct ActionInput {
    bool isKey = true;
    std::uint32_t value = 0; // KeyCode or UserInputType
};

// A ContextActionService binding, owned by the runtime whose function it calls.
struct BoundAction {
    Runtime* runtime = nullptr;
    std::string name;
    int ref = LUA_NOREF;
    int priority = 0;
    std::uint64_t seq = 0;
    std::vector<ActionInput> inputs;
    // It runs as the thread that bound it, with its globals (Runtime::useGlobals).
    ThreadAccess access;
    int globals = LUA_NOREF;
};

// --- Shared state ----------------------------------------------------------------

struct Tween {
    std::weak_ptr<Instance> tween;
    // The Tween itself while it plays (or waits out its delay): the engine
    // keeps it going after the script lets go of it, as in
    // TweenService:Create(...):Play().
    InstancePtr playing;
    std::weak_ptr<Instance> target;
    TweenInfo info;
    std::vector<TweenTrack> tracks;
    double elapsed = 0.0; // seconds into the current state, delay included
    int repetition = 0;   // completed passes (a reversed pass counts as part of its forward one)
    bool reversing = false;
    std::string_view state = "Begin"; // Enum.PlaybackState item name
    bool gui = false;                 // a GuiObject's TweenSize, TweenPosition or TweenSizeAndPosition
};

// One of Players' name lookups (GetNameFromUserIdAsync, GetUserIdFromNameAsync)
// in flight on a worker thread; every script asking the same meanwhile waits on it.
struct UserLookup {
    std::atomic<bool> done = false;
    std::expected<std::string, std::string> result = std::unexpected(std::string());
};

struct SharedState {
    std::vector<std::shared_ptr<Tween>> tweens;
    std::vector<PageMove> pageMoves;
    // Motors still turning towards their DesiredAngle (turnMotor).
    std::vector<std::weak_ptr<Instance>> turningMotors;
    InputState input;
    InputActionState inputActions;
    std::vector<BoundAction> actions;
    std::uint64_t nextActionSeq = 0;
    // Values scripts wrote to properties the engine doesn't implement.
    std::unordered_map<const Instance*, std::pair<std::weak_ptr<Instance>, std::map<std::string, Packed, std::less<>>>>
        extraProperties;
    // The console's isscriptable/setscriptable: which members it has made
    // scriptable, per instance and member name. Weak, for the same reason and
    // with the same check on the address as the values above.
    std::unordered_map<const Instance*, std::pair<std::weak_ptr<Instance>, std::map<std::string, bool, std::less<>>>>
        scriptableOverrides;
    // Every instance the VM has handed to a script, weakly: what the console's
    // getnilinstances() looks through for the ones left with no parent. An
    // instance nothing has referred to yet isn't in here, and never will be:
    // the engine keeps no list of its own.
    std::vector<std::weak_ptr<Instance>> knownInstances;
    // CollectionService's index: the tagged instances in the game, per tag.
    // Built on the first GetTagged and kept current by watchTags and the tag
    // methods, instead of walking the whole tree on every call.
    std::map<std::string, std::vector<Instance*>, std::less<>> tagged;
    bool taggedBuilt = false;
    // Players' name lookups by "name:<id>" or "id:<name>": the answers so far
    // (a leaderboard asks the same ids again for every stat), and those in flight.
    std::map<std::string, std::string, std::less<>> userLookups;
    std::map<std::string, std::shared_ptr<UserLookup>, std::less<>> pendingUserLookups;
};

InputState& inputState(Shared& shared);
void bindAction(Runtime& runtime, std::string name, int ref, int priority, std::vector<ActionInput> inputs,
                ThreadAccess access, int globals);
void unbindAction(Runtime& runtime, std::string_view name);
void unbindAllActions(Runtime& runtime);

// Runs the bound actions for an input (highest priority, newest first) and
// returns whether one sank it.
bool runActions(Shared& shared, const InstancePtr& input, Enum::KeyCode key, Enum::UserInputType type,
                Enum::UserInputState state);

// Player:LoadCharacter: makes the character (through the installed factory,
// or a minimal stand-in), copies StarterCharacterScripts in, parents it to the
// Workspace and fires CharacterAdded.
void spawnCharacter(Shared& shared, DataModel& game, Player& player);

// Starts announcing tagged instances entering and leaving the game.
void watchTags(Shared& shared, DataModel& game);

// Player:GetMouse(): the legacy Mouse, made on first use (also Tool.Equipped's argument).
Instance& legacyMouse(Instance& player);
// The legacy Mouse's X and Y (where the pointer is) and ViewSizeX and
// ViewSizeY (the view's size), kept as its members.
void updateMouseView(Shared& shared, Instance& mouse, Vector2 position, Vector2 viewSize);
// The legacy Mouse's Hit, Origin, UnitRay, Target and TargetSurface, pushed
// when `member` is one of them: a 1000-stud ray from the camera through the
// pointer, past the local character and TargetFilter (the Mouse reference).
// A miss puts Hit at the ray's end, Target at nil and TargetSurface at Right.
bool pushMouseRay(lua_State* L, Instance& mouse, std::string_view member);
// Starts the game's ToolSystem (once per Shared), which equips tools as they move.
void startTools(Shared& shared, DataModel& game);
// Starts the game's PromptSystem (once per Shared), which sends its events
// to a real server through the remote sink (Prompts.cpp)...
void startPrompts(Shared& shared, DataModel& game);
// ...and steps it from the camera and the local character: a client frame's.
void stepPrompts(Shared& shared, DataModel& game, double dt);

// What a real server sent for a remote (ScriptContext::receiveRemote).
bool receiveNetworkRemote(Shared& shared, Instance& remote, std::string_view event,
                          std::span<const RemoteValue> arguments);
using NativeRemoteReply = std::function<void(bool, PackedArgs)>;
void invokeServerForCore(Runtime& caller, Instance& remote, PackedArgs args, NativeRemoteReply done);

// Runs a callback (OnInvoke, OnServerInvoke...) in its own runtime with
// `args`, calling `done` with the packed results or the error once it
// finishes, which may be right away or after it yields.
void invokeCallback(const Shared::Callback& callback, const PackedArgs& args,
                    std::move_only_function<void(bool, PackedArgs)> done);

// --- TextChatService (TextChat.cpp) -----------------------------------------------

// The server's chat signals on TextChatService (receiveNetworkRemote):
// ServerToClientMessageReplicateSignal, ...V2 and UserMessageIntentSentRemote.
bool receiveTextChat(Shared& shared, Instance& service, std::string_view event, std::span<const RemoteValue> arguments);
void setChatSink(Shared& shared, std::function<void(const ChatLine&)> sink);
// ScriptContext::sendChat in the client runtime `client`.
std::expected<std::string, std::string> sendChat(Runtime& client, std::string_view text, std::string_view channel = {});
void requestWhisper(Runtime& client, std::int64_t target,
                    std::function<void(std::expected<std::string, std::string>)> done);
PresetChatAccess presetChatAccess(Shared& shared);
std::expected<std::string, std::string> sendPresetChat(Runtime& client, const ChatPreset& preset);

} // namespace lunar::lua
