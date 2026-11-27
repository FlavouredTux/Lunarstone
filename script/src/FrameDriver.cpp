// The frame loop (runFrame) and Play Solo (PlaySession).

#include <lunar/script/PlaySession.hpp>

#include "Characters.hpp"
#include "InstanceApi.hpp"
#include "Services.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Tools.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <vector>

namespace lunar {

namespace {

using lua::packed;
using lua::PackedArgs;
using lua::Runtime;

// Roblox's Player.Idled: two minutes without input, then every 30 s; its
// client disconnects at twenty minutes.
constexpr double idleFirst = 120.0;
constexpr double idleEvery = 30.0;
constexpr double idleKick = 20.0 * 60.0;

// Fires a RunService event once for the game; listeners of every context hear it.
void fireRunService(lua::Shared& shared, DataModel& game, std::string_view event, const PackedArgs& args)
{
    if (Instance* runService = game.findService("RunService"))
        shared.events.fire(*runService, event, args);
}

void flushAll(std::span<ScriptContext* const> contexts)
{
    for (ScriptContext* context : contexts)
        context->runtime().flushDeferred();
}

// Ends a frame phase for the profiler, when there is one.
void phaseDone(lua::Profiler* profiler, SimulationProfile::Phase phase)
{
    if (profiler)
        profiler->endPhase(phase);
}

} // namespace

void runFrame(std::span<ScriptContext* const> contexts, double dt, const FrameHooks& hooks)
{
    runFrameRender(contexts, dt);
    runFrameSimulate(contexts, dt, hooks);
}

void runFrameRender(std::span<ScriptContext* const> contexts, double dt)
{
    if (contexts.empty())
        return;
    Runtime& first = contexts.front()->runtime();
    lua::Shared& shared = first.shared();
    DataModel& game = first.game();
    const PackedArgs delta{packed(Value(dt))};
    lua::Profiler* profiler = shared.profiler.get();
    using Phase = SimulationProfile::Phase;

    for (ScriptContext* context : contexts)
        context->runtime().advance(dt);
    if (auto* workspace = game.findFirstChildOfClass("Workspace"))
        static_cast<Workspace*>(workspace)->setDistributedGameTime(first.now());

    // Network receive: remotes and remote calls sent last frame.
    lua::updateImageLoading(shared);
    shared.deliverMessages();
    flushAll(contexts);
    phaseDone(profiler, Phase::Input);

    // Render step, on clients only.
    const bool anyClient = std::ranges::any_of(contexts, [](ScriptContext* c) { return c->runtime().isClient(); });
    if (anyClient) {
        for (ScriptContext* context : contexts)
            if (context->runtime().isClient())
                context->runtime().renderStep(dt);
        lua::stepPrompts(shared, game, dt);
        fireRunService(shared, game, "RenderStepped", delta);
        fireRunService(shared, game, "PreRender", delta);
        flushAll(contexts);
    }
    phaseDone(profiler, Phase::RenderStep);
}

void runFrameSimulate(std::span<ScriptContext* const> contexts, double dt, const FrameHooks& hooks)
{
    if (contexts.empty())
        return;
    Runtime& first = contexts.front()->runtime();
    lua::Shared& shared = first.shared();
    DataModel& game = first.game();
    const PackedArgs delta{packed(Value(dt))};
    lua::Profiler* profiler = shared.profiler.get();
    using Phase = SimulationProfile::Phase;

    // Legacy wait()/spawn()/delay().
    for (ScriptContext* context : contexts)
        context->runtime().resumeLegacyTimers();
    phaseDone(profiler, Phase::LegacyWait);

    // Animation.
    fireRunService(shared, game, "PreAnimation", delta);
    flushAll(contexts);
    phaseDone(profiler, Phase::PreAnimation);
    if (hooks.animation)
        hooks.animation(dt);
    else
        lua::stepOwnAnimation(shared, dt);
    phaseDone(profiler, Phase::Animation);

    // Physics.
    fireRunService(shared, game, "Stepped", PackedArgs{packed(Value(first.now())), packed(Value(dt))});
    fireRunService(shared, game, "PreSimulation", delta);
    flushAll(contexts);
    phaseDone(profiler, Phase::Stepped);
    if (hooks.physics)
        hooks.physics(dt);
    // Touches the physics step made: loose tools get picked up.
    if (shared.tools)
        shared.tools->pickUpTouching();
    phaseDone(profiler, Phase::Physics);
    fireRunService(shared, game, "PostSimulation", delta);
    fireRunService(shared, game, "Heartbeat", delta);
    flushAll(contexts);
    phaseDone(profiler, Phase::Heartbeat);

    // Heartbeat-phase work: tweens and turning motors, then task.wait/task.delay.
    lua::stepTweens(shared, dt);
    lua::stepPageMoves(shared, dt);
    lua::stepMotors(shared, dt);
    phaseDone(profiler, Phase::Tweens);
    for (ScriptContext* context : contexts)
        context->runtime().resumeTimers();
    flushAll(contexts);
    phaseDone(profiler, Phase::TaskWait);
}

void ScriptContext::step(double dt)
{
    ScriptContext* self = this;
    runFrame(std::span(&self, 1), dt);
}

// --- PlaySession -------------------------------------------------------------

PlaySession::PlaySession(std::shared_ptr<DataModel> game)
    : game_(std::move(game)), server_(std::make_unique<ScriptContext>(game_, ScriptRole::Server)),
      client_(std::make_unique<ScriptContext>(game_, ScriptRole::Client, *server_)), input_(*client_)
{
}

PlaySession::~PlaySession()
{
    for (Connection& connection : connections_)
        connection.disconnect();
}

void PlaySession::setOutput(std::function<void(ScriptRole, MessageType, std::string_view)> sink)
{
    auto shared = std::make_shared<decltype(sink)>(std::move(sink));
    server_->setOutput(
        [shared](MessageType type, std::string_view text) { (*shared)(ScriptRole::Server, type, text); });
    client_->setOutput(
        [shared](MessageType type, std::string_view text) { (*shared)(ScriptRole::Client, type, text); });
}

void PlaySession::setRaycastHook(RaycastHook hook)
{
    server_->setRaycastHook(std::move(hook));
}

void PlaySession::setAnimationSystem(avatar::AnimationSystem* system)
{
    server_->setAnimationSystem(system);
}

void PlaySession::setCharacterFactory(std::function<InstancePtr(Player&)> factory)
{
    server_->runtime().shared().characterFactory = std::move(factory);
}

Player* PlaySession::localPlayer() const
{
    auto* players = dynamic_cast<Players*>(game_->findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

double PlaySession::time() const
{
    return server_->time();
}

namespace {

void report(ScriptContext& context, const std::expected<void, ScriptError>& result)
{
    if (!result)
        context.runtime().report(result.error());
}

void copyChildren(const Instance* from, Instance& to)
{
    if (!from)
        return;
    for (const InstancePtr& child : from->children())
        if (InstancePtr copy = child->clone())
            copy->setParent(&to);
}

// The client copies StarterPlayerScripts into PlayerScripts Archivable or
// not, which Clone isn't: the default RbxCharacterSounds and
// PlayerScriptsLoader a server puts there aren't [live: The Mimic's, whose
// footsteps wait on RbxCharacterSounds' Running sound; sUNC reads the
// loader's constants]. The shipped PlayerModule beside them stays out:
// registerPlayerModule's facade stands for it, and the native camera and
// controls do its work (the loader only requires it).
InstancePtr copyStarterScript(const Instance& child)
{
    if (!child.archivable() && child.name() == "PlayerModule")
        return nullptr;
    return child.cloneForClient();
}

void copyStarterScripts(const Instance* starterScripts, Instance& playerScripts)
{
    if (!starterScripts)
        return;
    for (const InstancePtr& child : starterScripts->children())
        if (InstancePtr copy = copyStarterScript(*child))
            copy->setParent(&playerScripts);
}

template <class T>
std::shared_ptr<T> makeChild(Instance& parent, std::string_view name)
{
    auto child = std::make_shared<T>();
    child->setName(std::string(name));
    child->setParent(&parent);
    return child;
}

// A StatsItem: a placeholder, as the engine has no class of them; the
// methods are Services.cpp's.
std::shared_ptr<Instance> makeStatsItem(Instance& parent, std::string_view name)
{
    auto item = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("StatsItem"));
    item->setName(std::string(name));
    item->setParent(&parent);
    return item;
}

// The client's Stats as scripts read their ping from it,
// Stats.Network.ServerStatsItem["Data Ping"] [live: sUNC raised "Network is
// not a valid member of Stats" once its tests were done, and its error path
// then sent the server about 2 GB]. Stats is a placeholder service, as
// GetService makes it. GUESS: the items past Data Ping, and that they are all
// plain StatsItems.
void makeStats(DataModel& game)
{
    Instance* stats = nullptr;
    for (const InstancePtr& child : game.children())
        if (child->className() == "Stats" && dynamic_cast<UnknownInstance*>(child.get()))
            stats = child.get();
    if (!stats) {
        auto made = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("Stats"));
        made->setParent(&game);
        stats = made.get();
    }
    if (stats->findFirstChild("Network"))
        return;
    auto network = makeStatsItem(*stats, "Network");
    auto server = makeStatsItem(*network, "ServerStatsItem");
    for (std::string_view name : {"Data Ping", "Data Send Kbps", "Data Receive Kbps"})
        makeStatsItem(*server, name);
}

// The client's own services, there before any script runs as in the real
// client: the server never sends them, and scripts index them by name.
// CoreGui was otherwise made only by the chat bubbles once one showed
// [live: sUNC raised "CoreGui is not a valid member of DataModel" in a game
// with the chat off].
void makeClientServices(DataModel& game)
{
    game.getService<CoreGui>();
    makeStats(game);
    // The client's own Lua packages are in CorePackages, which no server
    // sends; Lunarstone has none of them to put there, but the service is
    // there for what looks for it [sUNC's getscripts reads game.CorePackages].
    if (!game.findFirstChild("CorePackages")) {
        auto packages = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("CorePackages"));
        packages->setName("CorePackages");
        packages->setParent(&game);
    }
}

} // namespace

namespace {

// Starts every script that runs in the context's role, ReplicatedFirst's
// first (they run before the rest of the game on a client).
void startScriptsIn(DataModel& game, ScriptContext& context)
{
    std::vector<InstancePtr> first, rest;
    for (const InstancePtr& instance : game.descendants()) {
        if (!runsIn(*instance, context.role()))
            continue;
        if (instance->findFirstAncestorOfClass("ReplicatedFirst"))
            first.push_back(instance);
        else
            rest.push_back(instance);
    }
    for (const auto& script : first)
        report(context, context.runScript(*script));
    for (const auto& script : rest)
        if (runsIn(*script, context.role())) // an earlier script may have moved it
            report(context, context.runScript(*script));
}

bool isDisabled(const Instance& script)
{
    const auto* base = dynamic_cast<const BaseScript*>(&script);
    return base && !base->enabled();
}

// Scripts that land somewhere they run, or get enabled, start at the next
// resumption point; leaving or disabling stops them.
void watchScriptsIn(DataModel& game, std::vector<ScriptContext*> contexts, std::vector<Connection>& connections)
{
    auto consider = [contexts](Instance* instance) {
        if (!instance->isA("LuaSourceContainer") || instance->className() == "ModuleScript")
            return;
        for (ScriptContext* context : contexts) {
            Runtime& rt = context->runtime();
            const bool runs = runsIn(*instance, context->role());
            if (runs && !rt.isRunning(*instance)) {
                rt.defer([context, weak = std::weak_ptr<Instance>(instance->shared_from_this())] {
                    auto script = weak.lock();
                    if (script && runsIn(*script, context->role()) && !context->runtime().isRunning(*script))
                        report(*context, context->runScript(*script));
                });
            } else if (!runs && rt.isRunning(*instance) && isDisabled(*instance)) {
                context->stopScript(*instance);
            }
        }
    };
    connections.push_back(game.descendantAdded.connect([consider, &connections](Instance* instance) {
        consider(instance);
        if (instance->isA("LuaSourceContainer"))
            connections.push_back(instance->changed.connect([consider, instance](std::string_view property) {
                if (property == "Enabled" || property == "Disabled" || property == "RunContext")
                    consider(instance);
            }));
    }));
    for (const InstancePtr& instance : game.descendants())
        if (instance->isA("LuaSourceContainer"))
            connections.push_back(instance->changed.connect([consider, raw = instance.get()](std::string_view p) {
                if (p == "Enabled" || p == "Disabled" || p == "RunContext")
                    consider(raw);
            }));
    // Moving a running script doesn't stop it (scripts that reparent
    // themselves keep going, as in Roblox); destroying one does.
    connections.push_back(game.descendantRemoving.connect([contexts](Instance* instance) {
        bool destroyed = false;
        for (const Instance* node = instance; node && !destroyed; node = node->parent())
            destroyed = node->destroyed();
        if (!destroyed)
            return;
        for (ScriptContext* context : contexts)
            if (context->runtime().isRunning(*instance))
                context->stopScript(*instance);
    }));
}

} // namespace

void PlaySession::startScripts(ScriptContext& context)
{
    startScriptsIn(*game_, context);
}

void PlaySession::watchScripts()
{
    watchScriptsIn(*game_, {server_.get(), client_.get()}, connections_);
}

void PlaySession::start(const std::string& playerName, std::int64_t userId, const std::string& displayName)
{
    if (started_)
        return;
    started_ = true;
    lua::Shared& shared = server_->runtime().shared();

    // The client's services and default player scripts, before anything runs.
    makeClientServices(*game_);
    lua::installDefaultPlayerScripts(shared, *game_);
    watchScripts();
    startScripts(*server_);

    // The local player joins.
    auto* players = static_cast<Players*>(game_->getService("Players"));
    auto player = std::make_shared<Player>();
    player->setName(playerName);
    player->setDisplayName(displayName.empty() ? playerName : displayName);
    player->setUserId(userId);
    player->setCharacterAppearanceId(std::max<std::int64_t>(userId, 0));
    auto backpack = makeChild<Backpack>(*player, "Backpack");
    makeChild<StarterGear>(*player, "StarterGear");
    auto playerGui = makeChild<PlayerGui>(*player, "PlayerGui");
    auto playerScripts = makeChild<PlayerScripts>(*player, "PlayerScripts");
    players->assignLocalPlayer(player.get());
    player->setParent(players);
    shared.events.fire(*players, "PlayerAdded", PackedArgs{packed(Value(InstancePtr(player)))});
    server_->runtime().flushDeferred();

    Instance* starterPlayer = game_->findService("StarterPlayer");
    copyStarterScripts(starterPlayer ? starterPlayer->findFirstChildOfClass("StarterPlayerScripts") : nullptr,
                       *playerScripts);
    lua::registerPlayerModule(shared, *playerScripts);
    copyChildren(game_->findService("StarterGui"), *playerGui);
    copyChildren(game_->findService("StarterPack"), *backpack);
    server_->runtime().flushDeferred();
    client_->runtime().flushDeferred();

    startScripts(*client_);

    if (players->characterAutoLoads())
        lua::spawnCharacter(shared, *game_, *player);
    server_->runtime().flushDeferred();
    client_->runtime().flushDeferred();
}

Player* PlaySession::addPlayer(const std::string& name, std::int64_t userId)
{
    lua::Shared& shared = server_->runtime().shared();
    auto* players = static_cast<Players*>(game_->getService("Players"));
    auto player = std::make_shared<Player>();
    player->setName(name);
    player->setDisplayName(name);
    player->setUserId(userId);
    player->setCharacterAppearanceId(std::max<std::int64_t>(userId, 0));
    auto backpack = makeChild<Backpack>(*player, "Backpack");
    makeChild<StarterGear>(*player, "StarterGear");
    makeChild<PlayerGui>(*player, "PlayerGui");
    player->setParent(players);
    shared.events.fire(*players, "PlayerAdded", PackedArgs{packed(Value(InstancePtr(player)))});
    server_->runtime().flushDeferred();
    copyChildren(game_->findService("StarterPack"), *backpack);
    if (players->characterAutoLoads())
        lua::spawnCharacter(shared, *game_, *player);
    server_->runtime().flushDeferred();
    client_->runtime().flushDeferred();
    return player.get();
}

void PlaySession::setProfiling(bool on)
{
    lua::Shared& shared = server_->runtime().shared();
    if (on && !shared.profiler)
        shared.profiler = std::make_unique<lua::Profiler>();
    else if (!on)
        shared.profiler.reset();
}

bool PlaySession::controlsEnabled() const
{
    return server_->runtime().shared().controlsEnabled;
}

SimulationProfile PlaySession::takeProfile()
{
    lua::Profiler* profiler = server_->runtime().shared().profiler.get();
    return profiler ? profiler->take() : SimulationProfile{};
}

void PlaySession::step(double dt)
{
    if (lua::Profiler* profiler = server_->runtime().shared().profiler.get())
        profiler->beginFrame();
    input_.process();
    ScriptContext* contexts[] = {server_.get(), client_.get()};
    runFrame(contexts, dt, hooks_);
}

// --- ClientSession -----------------------------------------------------------

ClientSession::ClientSession(std::shared_ptr<DataModel> game)
    : game_(std::move(game)), client_(std::make_unique<ScriptContext>(game_, ScriptRole::Client)), input_(*client_)
{
    tools().setMode(ToolSystem::Mode::Client);
}

ToolSystem& ClientSession::tools()
{
    return *client_->runtime().shared().tools;
}

ClientSession::~ClientSession()
{
    for (Connection& connection : connections_)
        connection.disconnect();
}

Player* ClientSession::localPlayer() const
{
    auto* players = dynamic_cast<Players*>(game_->findFirstChildOfClass("Players"));
    return players ? players->localPlayer() : nullptr;
}

bool ClientSession::start(std::int64_t userId)
{
    if (started_)
        return true;
    auto* players = dynamic_cast<Players*>(game_->findFirstChildOfClass("Players"));
    if (!players)
        return false;
    Player* player = nullptr;
    for (const InstancePtr& child : players->children())
        if (auto* p = dynamic_cast<Player*>(child.get()); p && p->userId() == userId)
            player = p;
    return player && start(*player);
}

bool ClientSession::start(Player& chosen)
{
    if (started_)
        return true;
    auto* players = dynamic_cast<Players*>(game_->findFirstChildOfClass("Players"));
    if (!players || chosen.parent() != players)
        return false;
    Player* player = &chosen;
    started_ = true;
    players->assignLocalPlayer(player);
    tools().refreshAll(); // the tools in our character are ours now
    updateCamera();
    lua::Shared& shared = client_->runtime().shared();
    makeClientServices(*game_);

    // The client fills its own PlayerScripts from StarterPlayerScripts (with
    // Lunarstone's PlayerModule when the game ships none). PlayerGui, Backpack
    // and the character come from the server: a PlayerGui still empty here is
    // one the server hasn't sent yet, and a copy of StarterGui made now would
    // run every GUI script twice beside the server's (the game's MainUI did, and
    // its camera stopped working).
    lua::installDefaultPlayerScripts(shared, *game_);
    Instance* playerScripts = player->findFirstChildOfClass("PlayerScripts");
    if (!playerScripts)
        playerScripts = makeChild<PlayerScripts>(*player, "PlayerScripts").get();
    Instance* starterPlayer = game_->findService("StarterPlayer");
    const Instance* starterScripts
        = starterPlayer ? starterPlayer->findFirstChildOfClass("StarterPlayerScripts") : nullptr;
    copyStarterScripts(starterScripts, *playerScripts);
    lua::registerPlayerModule(shared, *playerScripts);
    // A slow server sends the default scripts first and the game's own a
    // while later [live: a game server sent its Client
    // LocalScript 15 seconds after], and those count as much: the copy
    // goes on for what comes later (copyLateStarterScripts).
    playerScripts_ = playerScripts->weak_from_this();
    if (starterScripts)
        connections_.push_back(const_cast<Instance*>(starterScripts)->childAdded.connect([this](Instance* child) {
            lateStarterScripts_.push_back(child->weak_from_this());
        }));
    client_->runtime().flushDeferred();

    watchCharacters(*players);
    watchScriptsIn(*game_, {client_.get()}, connections_);
    // The idle kick's handler, made before any script's: GUESS that the time
    // Idled passes is how long the player has been idle, as the docs have it.
    nextIdled_ = idleFirst;
    idleKick_ = shared.events.connectNative(*player, "Idled", [this](const PackedArgs& args) {
        const double* idle = args.empty() ? nullptr : std::get_if<double>(&args.front().value);
        if (idle && *idle >= idleKick)
            client_->runtime().notice(MessageType::Warning,
                                      "You were disconnected for being idle 20 minutes (a kick only notes it here)");
    });
    // The client's own backpack takes a tool in as it arrives, as Roblox's
    // BackpackScript does: a script sees those connections on the Backpack
    // as foreign ones [sUNC's getconnections looks for one]. The server sends
    // a new Backpack with each character.
    if (Instance* backpack = player->findFirstChildOfClass("Backpack"))
        watchBackpack(*backpack);
    connections_.push_back(player->childAdded.connect([this](Instance* child) {
        if (child->isA("Backpack"))
            watchBackpack(*child);
    }));
    startScriptsIn(*game_, *client_);
    client_->runtime().flushDeferred();
    return true;
}

void ClientSession::watchBackpack(Instance& backpack)
{
    lua::EventHub& hub = client_->runtime().shared().events;
    backpackWatch_.clear();
    for (const bool added : {true, false}) {
        auto native = std::make_shared<lua::EventHub::NativeConnection>();
        native->handler = [this](const PackedArgs&) { hotbar_.sync(*game_); };
        Signal<Instance*>& signal = added ? backpack.childAdded : backpack.childRemoved;
        native->connection = signal.connect([weak = std::weak_ptr(native)](Instance*) {
            if (const auto held = weak.lock(); held && held->enabled)
                held->handler({});
        });
        hub.noteNative(backpack, added ? "ChildAdded" : "ChildRemoved", native);
        backpackWatch_.push_back(std::move(native));
    }
}

// After the frame's input: an idle player hears of it, two minutes on and
// every half minute after.
void ClientSession::watchIdle(double dt)
{
    if (!started_)
        return;
    if (input_.hadInput()) {
        idleFor_ = 0.0;
        nextIdled_ = idleFirst;
        return;
    }
    idleFor_ += dt;
    if (idleFor_ < nextIdled_)
        return;
    nextIdled_ += idleEvery;
    if (Player* player = localPlayer())
        client_->runtime().shared().events.fire(*player, "Idled", {packed(Value(idleFor_))});
}

void ClientSession::setAnimationSystem(avatar::AnimationSystem* system)
{
    client_->setAnimationSystem(system);
}

// Our character's Humanoid once nothing simulates it: Running, then Running
// fires with speed 0, once per Humanoid, in the frame's physics step where
// the client's state machine would run. A live client's character spawns a
// little above the ground and goes Freefall, Landed, Running [capture: what
// a freshly spawned player's NetworkHumanoidState goes through]; standing
// still, Running at 0 is where it stays.
// GUESS: StateChanged fires from None, the state a Humanoid nothing has
// stepped reports here; the fall before landing is left out.
void ClientSession::standCharacter()
{
    Player* player = localPlayer();
    Model* character = player ? player->character() : nullptr;
    auto* humanoid = character ? dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (!humanoid || humanoid->driver() || stood_.lock().get() == humanoid)
        return;
    stood_ = humanoid->shared_from_this();
    if (humanoid->health() <= 0.0f)
        return;
    const Enum::HumanoidStateType before = humanoid->getState();
    humanoid->assignState(Enum::HumanoidStateType::Running);
    if (before != Enum::HumanoidStateType::Running)
        humanoid->events.stateChanged.fire(before, Enum::HumanoidStateType::Running);
    humanoid->events.running.fire(0.0f);
}

// The real client's Player fires CharacterRemoving and CharacterAdded when
// its Character changes; here replication changes it (the server spawns
// ours after the join), so the session fires them, for every player.
// GUESS: CharacterRemoving fires once Character already names the new one.
void ClientSession::watchCharacters(Players& players)
{
    auto watch = [this](Player& player) {
        auto shown = std::make_shared<std::weak_ptr<Instance>>();
        if (Model* character = player.character())
            *shown = character->weak_from_this();
        connections_.push_back(player.changed.connect([this, &player, shown](std::string_view property) {
            Model* character = player.character();
            const InstancePtr before = shown->lock();
            if (property != "Character" || before.get() == character)
                return;
            *shown = character ? character->weak_from_this() : std::weak_ptr<Instance>{};
            // Our camera looks at our new character's Humanoid before its
            // scripts run, as the client's does when it comes [live: The
            // Mimic's head turning reads CameraSubject on its first frame];
            // the native camera follows it from there.
            const auto* service = dynamic_cast<const Players*>(player.parent());
            if (character && service && service->localPlayer() == &player)
                if (auto* humanoid = dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid")))
                    if (Workspace* workspace = game_->workspace())
                        if (Camera* camera = workspace->ensureCurrentCamera();
                            camera->cameraType() != Enum::CameraType::Scriptable)
                            camera->setCameraSubject(humanoid);
            if (before)
                player.fireEvent("CharacterRemoving", std::array{Value(before)});
            if (character)
                player.fireEvent("CharacterAdded", std::array{Value(character->shared_from_this())});
        }));
    };
    for (const InstancePtr& child : players.children())
        if (auto* player = dynamic_cast<Player*>(child.get()))
            watch(*player);
    connections_.push_back(players.childAdded.connect([watch](Instance* child) {
        if (auto* player = dynamic_cast<Player*>(child))
            watch(*player);
    }));
}

void ClientSession::setProfiling(bool on)
{
    lua::Shared& shared = client_->runtime().shared();
    if (on && !shared.profiler)
        shared.profiler = std::make_unique<lua::Profiler>();
    else if (!on)
        shared.profiler.reset();
}

bool ClientSession::profiling() const
{
    return client_->runtime().shared().profiler != nullptr;
}

SimulationProfile ClientSession::takeProfile()
{
    lua::Profiler* profiler = client_->runtime().shared().profiler.get();
    return profiler ? profiler->take() : SimulationProfile{};
}

// StarterPlayerScripts' late arrivals, copied into PlayerScripts a step
// after they came: by then the packet that brought one (and its children)
// is all in. The script watcher starts the copies' scripts.
void ClientSession::copyLateStarterScripts()
{
    if (lateStarterScripts_.empty())
        return;
    const InstancePtr playerScripts = playerScripts_.lock();
    for (const std::weak_ptr<Instance>& weak : std::exchange(lateStarterScripts_, {})) {
        const InstancePtr child = weak.lock();
        if (!child || !playerScripts || !child->parent() || !child->parent()->isA("StarterPlayerScripts"))
            continue;
        if (InstancePtr copy = copyStarterScript(*child))
            copy->setParent(playerScripts.get());
    }
}

void ClientSession::step(double dt)
{
    copyLateStarterScripts();
    if (lua::Profiler* profiler = client_->runtime().shared().profiler.get())
        profiler->beginFrame();
    ScriptContext* contexts[] = {client_.get()};
    FrameHooks hooks = hooks_;
    if (!hooks.physics)
        hooks.physics = [this](double) { standCharacter(); };
    if (!drawBeforeSimulation_) {
        updateCamera();
        input_.process();
        watchIdle(dt);
        runFrame(contexts, dt, hooks);
        return;
    }
    // The last frame's simulation, which ran after its picture, then this
    // frame up to its picture.
    if (simulationDue_)
        runFrameSimulate(contexts, *simulationDue_, hooks);
    updateCamera();
    input_.process();
    watchIdle(dt);
    runFrameRender(contexts, dt);
    simulationDue_ = dt;
}

// The client's own camera, which scripts expect to find: servers don't
// replicate theirs. Each frame, like the window's size, its ViewportSize
// follows the screen's, also on a camera the scripts put in its place.
void ClientSession::updateCamera()
{
    if (Workspace* workspace = game_->workspace())
        workspace->ensureCurrentCamera()->updateViewportSize(viewportSize_);
    // So does the legacy Mouse's view, with where the pointer is.
    if (Player* player = localPlayer())
        for (const InstancePtr& child : player->children())
            if (child->className() == "PlayerMouse") {
                lua::Shared& shared = client_->runtime().shared();
                lua::updateMouseView(shared, *child,
                                     shared.state->input.mousePosition - lua::guiCoordinateOrigin(*game_),
                                     viewportSize_);
            }
}

bool ClientSession::selectTool(int index)
{
    hotbar_.sync(*game_);
    if (!hotbar_.slot(index))
        return false;
    hotbar_.select(*game_, index);
    client_->runtime().flushDeferred();
    return true;
}

void ClientSession::unequipTools()
{
    Player* player = localPlayer();
    Model* character = player ? player->character() : nullptr;
    if (auto* humanoid = character ? dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr)
        lunar::unequipTools(*game_, *humanoid);
    client_->runtime().flushDeferred();
}

void ClientSession::toolButton(bool down)
{
    Player* player = localPlayer();
    Model* character = player ? player->character() : nullptr;
    if (!character)
        return;
    for (const InstancePtr& child : std::vector(character->children().begin(), character->children().end()))
        if (auto* tool = dynamic_cast<Tool*>(child.get())) {
            if (down)
                tools().activate(*tool, false);
            else
                tools().deactivate(*tool, false);
        }
    client_->runtime().flushDeferred();
}

void ClientSession::dropTools()
{
    if (Player* player = localPlayer())
        lunar::dropTools(*game_, *player);
    client_->runtime().flushDeferred();
}

// --- Input -------------------------------------------------------------------

void ClientInput::keyDown(Enum::KeyCode key, bool processed, GuiDispatch gui)
{
    pending_.push_back(
        {Enum::UserInputType::Keyboard, key, Enum::UserInputState::Begin, {}, {}, processed, std::move(gui)});
}

void ClientInput::keyUp(Enum::KeyCode key, bool processed, GuiDispatch gui)
{
    pending_.push_back(
        {Enum::UserInputType::Keyboard, key, Enum::UserInputState::End, {}, {}, processed, std::move(gui)});
}

void ClientInput::mouseButton(Enum::UserInputType button, bool down, bool processed, GuiDispatch gui)
{
    pending_.push_back({button,
                        Enum::KeyCode::None,
                        down ? Enum::UserInputState::Begin : Enum::UserInputState::End,
                        pendingMouse_.value_or(client_->runtime().shared().state->input.mousePosition),
                        {},
                        processed,
                        std::move(gui)});
}

void ClientInput::mouseMove(Vector2 position, Vector2 delta, bool processed, GuiDispatch gui)
{
    pending_.push_back({Enum::UserInputType::MouseMovement, Enum::KeyCode::None, Enum::UserInputState::Change, position,
                        Vector3{delta.x, delta.y, 0}, processed, std::move(gui)});
    pendingMouse_ = position;
}

void ClientInput::mouseWheel(float delta, bool processed, GuiDispatch gui)
{
    pending_.push_back({Enum::UserInputType::MouseWheel, Enum::KeyCode::None, Enum::UserInputState::Change,
                        pendingMouse_.value_or(client_->runtime().shared().state->input.mousePosition),
                        Vector3{0, 0, delta}, processed, std::move(gui)});
}

// The local character's tools: the left button activates the ones it holds
// (the client's ToolMouseCommand, before the Mouse hears the button) and
// Backspace drops them. The app only sends clicks the GUI didn't take.
// (Guess: that a bound action sinking the input stops these too.)
void ClientInput::toolInput(const Pending& in)
{
    DataModel& game = client_->game();
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    Player* player = players ? players->localPlayer() : nullptr;
    Model* character = player ? player->character() : nullptr;
    ToolSystem* tools = client_->runtime().shared().tools.get();
    if (!character || !tools)
        return;
    if (in.type == Enum::UserInputType::Keyboard && in.key == Enum::KeyCode::Backspace
        && in.state == Enum::UserInputState::Begin) {
        dropTools(game, *player);
        return;
    }
    if (in.type != Enum::UserInputType::MouseButton1)
        return;
    for (const InstancePtr& child : std::vector(character->children().begin(), character->children().end()))
        if (auto* tool = dynamic_cast<Tool*>(child.get())) {
            if (in.state == Enum::UserInputState::Begin)
                tools->activate(*tool, false);
            else if (in.state == Enum::UserInputState::End)
                tools->deactivate(*tool, false);
        }
}

// Each input becomes an InputObject that UserInputService's InputBegan,
// InputChanged or InputEnded carries, after the bound actions had their go.
// (Guess: the client's order between ContextActionService and
// UserInputService, and gameProcessedEvent beyond GUI focus, aren't pinned.)
void ClientInput::process()
{
    // GetMouseDelta: this frame's motion, all of it (a fast mouse sends
    // several moves a frame), and none in a frame without any.
    client_->runtime().shared().state->input.mouseDelta = {};
    lua::refreshInputActions(client_->runtime().shared(), client_->game());
    hadInput_ = !pending_.empty();
    if (pending_.empty())
        return;
    lua::Shared& shared = client_->runtime().shared();
    lua::InputState& state = shared.state->input;
    DataModel& game = client_->game();
    const Vector2 origin = lua::guiCoordinateOrigin(game);
    auto* uis = static_cast<UserInputService*>(game.getService("UserInputService"));
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    Player* player = players ? players->localPlayer() : nullptr;
    Instance* mouse = nullptr;
    if (player)
        for (const InstancePtr& child : player->children())
            if (child->className() == "PlayerMouse")
                mouse = child.get();

    auto pending = std::move(pending_);
    pending_.clear();
    for (const Pending& in : pending) {
        InstancePtr object;
        if (in.type == Enum::UserInputType::Keyboard) {
            if (in.state == Enum::UserInputState::Begin) {
                if (state.keys.contains(in.key))
                    continue; // key repeat isn't a new press
                state.keys.insert(in.key);
                if (in.processed)
                    state.processedKeys.insert(in.key);
                object = std::make_shared<InputObject>();
                state.keyObjects[in.key] = object;
            } else {
                if (!state.keys.erase(in.key))
                    continue;
                state.processedKeys.erase(in.key);
                auto it = state.keyObjects.find(in.key);
                object = it != state.keyObjects.end() ? it->second : std::make_shared<InputObject>();
            }
        } else {
            object = std::make_shared<InputObject>();
            if (in.type == Enum::UserInputType::MouseMovement) {
                if (!state.mouseMovementObject)
                    state.mouseMovementObject = object;
                object = state.mouseMovementObject;
                state.mouseDelta = state.mouseDelta + Vector2{in.delta.x, in.delta.y};
                state.mousePosition = in.position;
            } else if (in.type == Enum::UserInputType::MouseWheel) {
                if (!state.mouseWheelObject)
                    state.mouseWheelObject = object;
                object = state.mouseWheelObject;
            } else if (in.state == Enum::UserInputState::Begin) {
                state.buttons.insert(in.type);
                if (in.processed)
                    state.processedButtons.insert(in.type);
                state.buttonObjects[in.type] = object;
            } else if (in.state == Enum::UserInputState::End) {
                state.buttons.erase(in.type);
                state.processedButtons.erase(in.type);
                if (auto it = state.buttonObjects.find(in.type); it != state.buttonObjects.end()) {
                    object = std::move(it->second);
                    state.buttonObjects.erase(it);
                }
            }
        }
        auto* input = static_cast<InputObject*>(object.get());
        input->setKeyCode(in.key);
        input->setUserInputType(in.type);
        input->setUserInputState(in.state);
        // The wheel's notches are Position.Z too, where scripts read them.
        const float wheel = in.type == Enum::UserInputType::MouseWheel ? in.delta.z : 0.0f;
        input->setPosition(in.type == Enum::UserInputType::Keyboard
                               ? Vector3{}
                               : Vector3{in.position.x - origin.x, in.position.y - origin.y, wheel});
        input->setDelta(in.delta);
        if (state.lastInputType != in.type) {
            state.lastInputType = in.type;
            shared.events.fire(*uis, "LastInputTypeChanged", {packed(Value(enums::item(in.type)))});
        }

        if (in.gui)
            in.gui(object);

        // A shown ProximityPrompt's key holds it, and the game hears of the
        // key as processed (the client's prompts bind it above the game's
        // actions). GUESS: that it still fires InputBegan and InputEnded.
        bool prompted = false;
        if (in.type == Enum::UserInputType::Keyboard && shared.prompts) {
            if (in.state == Enum::UserInputState::Begin && !in.processed)
                prompted = shared.prompts->press(in.key);
            else if (in.state == Enum::UserInputState::End)
                shared.prompts->release(in.key);
        }
        const bool actionSunk
            = lua::runInputActions(shared, in.key, in.type, in.state, in.position - origin, in.processed || prompted);
        const bool sunk
            = in.processed || prompted || actionSunk || lua::runActions(shared, object, in.key, in.type, in.state);
        const PackedArgs args{packed(Value(object)), packed(Value(in.processed || prompted))};
        const char* event = in.state == Enum::UserInputState::Begin ? "InputBegan"
                            : in.state == Enum::UserInputState::End ? "InputEnded"
                                                                    : "InputChanged";
        shared.events.fire(*uis, event, args);
        if (!sunk)
            toolInput(in);

        // The legacy Mouse.
        if (mouse) {
            if (in.type == Enum::UserInputType::MouseMovement) {
                auto& values = shared.state->extraProperties[mouse];
                values.first = mouse->shared_from_this();
                values.second.insert_or_assign("X", packed(Value(static_cast<double>(in.position.x - origin.x))));
                values.second.insert_or_assign("Y", packed(Value(static_cast<double>(in.position.y - origin.y))));
                shared.events.fire(*mouse, "Move", {});
            } else if (in.type == Enum::UserInputType::MouseButton1 || in.type == Enum::UserInputType::MouseButton2) {
                const bool left = in.type == Enum::UserInputType::MouseButton1;
                const bool down = in.state == Enum::UserInputState::Begin;
                shared.events.fire(*mouse, std::format("Button{}{}", left ? 1 : 2, down ? "Down" : "Up"), {});
            }
        }
        client_->runtime().flushDeferred();
    }
}

} // namespace lunar
