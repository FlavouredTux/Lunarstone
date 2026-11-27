#pragma once

// Play Solo the way Studio does it: a server context running Scripts and a
// client context running LocalScripts over one DataModel, with remotes looped
// back between them, a local player with a character, and a frame loop that
// runs RunService's steps in Roblox's order.

#include <lunar/Tools.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar {

// What a frame calls out to. Each hook is optional.
struct FrameHooks {
    // After RunService.PreAnimation: step animators.
    std::function<void(double dt)> animation;
    // Between PreSimulation/Stepped and PostSimulation/Heartbeat: step physics.
    // Touched and other physics events go through Instance::fireEvent.
    std::function<void(double dt)> physics;
};

// One frame over a set of contexts sharing a game, in the 0.742 client's job
// order: remote delivery and input, the render step (BindToRenderStep by
// priority, RenderStepped, PreRender) on clients, legacy wait/spawn
// resumption, PreAnimation, Stepped and PreSimulation, physics, PostSimulation
// and Heartbeat, then tweens and task.wait/task.delay resumption.
//
// From the binary: the per-step profiler labels sit in the order RenderStepped,
// PreRender / Stepped, PreSimulation, which is the order each legacy/new pair
// fires here. (Guess: where legacy waits and task timers fall relative to the
// RunService signals is from the documented scheduler, not the binary.)
void runFrame(std::span<ScriptContext* const> contexts, double dt, const FrameHooks& hooks = {});
// runFrame in two: up to and with the render step, then the rest. The
// client draws between them, so a frame shows the world as the render step
// left it: the camera and anything the scripts placed from the character
// then agree with the character, which the physics would otherwise have
// moved on before the picture.
void runFrameRender(std::span<ScriptContext* const> contexts, double dt);
void runFrameSimulate(std::span<ScriptContext* const> contexts, double dt, const FrameHooks& hooks = {});

// Keyboard and mouse from the platform layer for a client context, queued
// as they come and handed to UserInputService, ContextActionService, the
// legacy Mouse and the local character's tools at the start of the
// context's next frame.
class ClientInput {
public:
    using GuiDispatch = std::function<void(const InstancePtr&)>;
    explicit ClientInput(ScriptContext& client) : client_(&client) {}

    void keyDown(Enum::KeyCode key, bool processed = false, GuiDispatch gui = {});
    void keyUp(Enum::KeyCode key, bool processed = false, GuiDispatch gui = {});
    void mouseButton(Enum::UserInputType button, bool down, bool processed = false, GuiDispatch gui = {});
    void mouseMove(Vector2 position, Vector2 delta, bool processed = false, GuiDispatch gui = {});
    void mouseWheel(float delta, bool processed = false, GuiDispatch gui = {});

    // Everything queued since the last call, in order: a frame's input phase.
    void process();
    // Whether the last process() had anything: the idle clock starts over.
    bool hadInput() const { return hadInput_; }

private:
    struct Pending {
        Enum::UserInputType type = Enum::UserInputType::None;
        Enum::KeyCode key = Enum::KeyCode::None;
        Enum::UserInputState state = Enum::UserInputState::None;
        Vector2 position;
        Vector3 delta;
        bool processed = false;
        GuiDispatch gui;
    };

    void toolInput(const Pending& in);

    ScriptContext* client_;
    std::vector<Pending> pending_;
    std::optional<Vector2> pendingMouse_;
    bool hadInput_ = false;
};

class PlaySession {
public:
    explicit PlaySession(std::shared_ptr<DataModel> game);
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    // print/warn/errors of both sides; role says which side spoke.
    void setOutput(std::function<void(ScriptRole, MessageType, std::string_view)> sink);
    void setHooks(FrameHooks hooks) { hooks_ = std::move(hooks); }
    void setRaycastHook(RaycastHook hook);
    // Where scripts' animations play (see ScriptContext::setAnimationSystem).
    void setAnimationSystem(avatar::AnimationSystem* system);
    // Builds a player's character model; without one a plain R6-shaped stand-in spawns.
    void setCharacterFactory(std::function<InstancePtr(Player&)> factory);

    // Runs the server's scripts, adds the local player (PlayerAdded),
    // fills its PlayerScripts/PlayerGui/Backpack from the starter containers,
    // runs the client's scripts (ReplicatedFirst first) and spawns the
    // character if Players.CharacterAutoLoads. Scripts added or enabled later
    // start on their own. The player's CharacterAppearanceId is its UserId
    // (0 for the negative ids of test players); displayName defaults to the name.
    void start(const std::string& playerName = "Player1", std::int64_t userId = -1,
               const std::string& displayName = {});
    // Another player joining the server without a client of its own, like
    // Studio's local server test with several players: PlayerAdded, a
    // Backpack from StarterPack and a character. Call after start.
    Player* addPlayer(const std::string& name, std::int64_t userId);
    void step(double dt);
    double time() const;

    // Whether the local player's controls may move the character: the
    // PlayerModule's ControlModule:Disable() turns them off. The app's native
    // controls (keyboard move vector, jump) honour it.
    bool controlsEnabled() const;

    // Collects where frames spend their time (see SimulationProfile) until
    // turned off; takeProfile hands over what was collected since the last take.
    void setProfiling(bool on);
    SimulationProfile takeProfile();

    ScriptContext& server() { return *server_; }
    ScriptContext& client() { return *client_; }
    DataModel& game() const { return *game_; }
    Player* localPlayer() const;

    // Input from the platform layer, handed to UserInputService,
    // ContextActionService and the legacy Mouse at the start of the next frame.
    ClientInput& input() { return input_; }
    void keyDown(Enum::KeyCode key) { input_.keyDown(key); }
    void keyUp(Enum::KeyCode key) { input_.keyUp(key); }
    void mouseButton(Enum::UserInputType button, bool down) { input_.mouseButton(button, down); }
    void mouseMove(Vector2 position, Vector2 delta) { input_.mouseMove(position, delta); }
    void mouseWheel(float delta) { input_.mouseWheel(delta); }

private:
    void startScripts(ScriptContext& context);
    void watchScripts();

    std::shared_ptr<DataModel> game_;
    std::unique_ptr<ScriptContext> server_;
    std::unique_ptr<ScriptContext> client_;
    ClientInput input_;
    FrameHooks hooks_;
    std::vector<Connection> connections_;
    bool started_ = false;
};

// A client of a real game server (--net-join --scripts, --net-replay --run):
// one client context over the DataModel the network replicates into. The
// server owns the players, their Backpacks and characters; the client fills
// its own PlayerScripts (and PlayerGui when the server left it empty) from
// the starter containers and runs its LocalScripts, which start and stop as
// replication moves them about. Nothing here talks to the network: remotes
// and the character's physics go through the owner's hooks.
//
// Without a physics hook nothing simulates our character, so it stands
// where the server put it: its Humanoid is Running at speed 0, which is
// where a real client's state machine settles once the character lands
// (Running fires with 0 and the Animate script plays idle).
class ClientSession {
public:
    explicit ClientSession(std::shared_ptr<DataModel> game);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void setHooks(FrameHooks hooks) { hooks_ = std::move(hooks); }
    const FrameHooks& hooks() const { return hooks_; }
    // Where scripts' animations play (see ScriptContext::setAnimationSystem);
    // set it before start, and step it from the animation hook.
    void setAnimationSystem(avatar::AnimationSystem* system);

    // Makes the player with this UserId the LocalPlayer and starts the
    // client's scripts; false while the server hasn't sent that player yet.
    bool start(std::int64_t userId);
    // The same with the player chosen by the owner: a server can still list
    // a Player of ours from an earlier session, with the same UserId.
    bool start(Player& player);
    bool started() const { return started_; }
    void step(double dt);
    // With this on, step ends after the render step and starts with the rest
    // of the frame before (animation, physics, Heartbeat, timers), so what is
    // drawn after step is what the genuine client draws (runFrameRender).
    void setDrawBeforeSimulation(bool on) { drawBeforeSimulation_ = on; }

    // The screen the client pretends to draw on: Workspace.CurrentCamera's
    // ViewportSize. 1280x720 unless set, the size --render pictures it at.
    void setViewportSize(Vector2 size) { viewportSize_ = size; }

    // Collects where its frames spend their time (see SimulationProfile) until
    // turned off; takeProfile hands over what was collected since the last
    // take. Nothing is collected, and no instrumentation runs, until it is on.
    void setProfiling(bool on);
    bool profiling() const;
    SimulationProfile takeProfile();

    // The local character's tools, moved the way a real client's input moves
    // them. The client's ToolSystem runs in client mode: it welds a tool that
    // comes in from our Backpack (that weld, the tool's moves and its events
    // are what net::ToolReplicator sends the server) and leaves other
    // players' tools to their own clients.
    // - selectTool: the hotbar key or a click on its slot (0-based, slots as
    //   the backpack GUI fills them): equips the slot's tool, or puts it away
    //   when it's the one in hand. False for an empty slot or no character.
    // - unequipTools: Humanoid:UnequipTools.
    // - toolButton: the left button over the 3D view (ToolMouseCommand):
    //   press activates the held tools, release deactivates them;
    //   ManualActivationOnly tools ignore it.
    // - dropTools: the drop key (Backspace), for tools that CanBeDropped.
    bool selectTool(int index);
    void unequipTools();
    void toolButton(bool down);
    void dropTools();
    const Hotbar& hotbar() const { return hotbar_; }
    ToolSystem& tools();
    // The user's keyboard and mouse (--net-play's window), handed to the
    // scripts at the start of the next step.
    ClientInput& input() { return input_; }

    ScriptContext& client() { return *client_; }
    DataModel& game() const { return *game_; }
    Player* localPlayer() const;

private:
    void standCharacter();
    void updateCamera();
    void watchCharacters(Players& players);
    void copyLateStarterScripts();
    void watchIdle(double dt);
    void watchBackpack(Instance& backpack);

    std::shared_ptr<DataModel> game_;
    std::unique_ptr<ScriptContext> client_;
    ClientInput input_;
    FrameHooks hooks_;
    bool drawBeforeSimulation_ = false;
    std::optional<double> simulationDue_; // the frame whose simulation is still to run, by its dt
    std::vector<Connection> connections_;
    // What reached StarterPlayerScripts after the scripts started, to copy
    // into PlayerScripts at the start of the next step.
    std::vector<std::weak_ptr<Instance>> lateStarterScripts_;
    std::weak_ptr<Instance> playerScripts_;
    std::weak_ptr<Instance> stood_; // the Humanoid standCharacter last set up
    Vector2 viewportSize_{1280.0f, 720.0f};
    Hotbar hotbar_;
    // Roblox's idle kick: Idled on the LocalPlayer two minutes after the last
    // input and every 30 s after, and the client's own handler on it, which
    // kicks at twenty minutes (the C connection getconnections finds there).
    double idleFor_ = 0.0;
    double nextIdled_ = 0.0;
    std::shared_ptr<void> idleKick_;
    // The client's own backpack's connections on the LocalPlayer's Backpack
    // (Roblox's are its BackpackScript's, a CoreScript's).
    std::vector<std::shared_ptr<void>> backpackWatch_;
    bool started_ = false;
};

} // namespace lunar
