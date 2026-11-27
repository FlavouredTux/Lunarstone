#include "PlaySession.hpp"

#include "ConsoleApi.hpp"
#include "GameWindow.hpp"
#include "MapScene.hpp"
#include "OffscreenUi.hpp"
#include "PlaceScene.hpp"
#include "Playtest.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/Seats.hpp>
#include <lunar/Tools.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/audio/SoundSystem.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/Character.hpp>
#include <lunar/avatar/Clothing.hpp>
#include <lunar/avatar/UserAvatar.hpp>
#include <lunar/physics/Character.hpp>
#include <lunar/physics/HumanoidController.hpp>
#include <lunar/physics/PlayerCamera.hpp>
#include <lunar/physics/World.hpp>
#include <lunar/platform/Window.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/Presenter.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>
#include <lunar/ui/GameUi.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace lunar::platform;
namespace physics = lunar::physics;
namespace render = lunar::render;
using lunar::Vector3;

// ControlModule's keyboard move vector: W/Up forward (-Z), S/Down back, A left, D right.
Vector3 keyboardMoveVector(const InputState& input)
{
    const float forward = (input.isDown(Key::W) || input.isDown(Key::Up)) ? -1.0f : 0.0f;
    const float backward = (input.isDown(Key::S) || input.isDown(Key::Down)) ? 1.0f : 0.0f;
    const float left = input.isDown(Key::A) ? -1.0f : 0.0f;
    const float right = input.isDown(Key::D) ? 1.0f : 0.0f;
    return {left + right, 0.0f, forward + backward};
}

// ControlModule.calculateRawMoveVector: the move vector turned by the
// camera's yaw only, keeping its length.
Vector3 cameraRelative(Vector3 move, const lunar::CFrame& camera)
{
    const auto& r = camera.r;
    float c = 0.0f;
    float s = 0.0f;
    if (r[1][2] > -1.0f && r[1][2] < 1.0f) {
        c = r[2][2];
        s = r[0][2];
    } else {
        c = r[0][0];
        s = -r[0][1] * (r[1][2] < 0.0f ? -1.0f : 1.0f);
    }
    const float n = std::sqrt(c * c + s * s);
    if (n < 1e-6f)
        return move;
    return {(c * move.x + s * move.z) / n, 0.0f, (c * move.z - s * move.x) / n};
}

// What /gogogo and --simulate --gogogo run on the server: start a round in
// places built on Roblox's battle royale template, as its admin-only GoGoGo
// console command does.
constexpr std::string_view goGoGoSource = R"(
    local handler = require(game:GetService("ServerScriptService").Core.GameStageHandler).getInstance()
    local stage = handler.currentStageClassName
    if stage == "Waiting" then
        handler.currentStage:startGame(true)
    elseif stage == "Queue" then
        handler.currentStage:teleportQueue()
    else
        error("nothing to start in stage " .. tostring(stage))
    end
)";

// LUNAR_PROFILE: unset is off, 2 (or more) also lists the busiest scripts
// each second, anything else splits the simulation time only.
int profileLevel()
{
    const char* value = std::getenv("LUNAR_PROFILE");
    if (!value)
        return 0;
    return std::strtol(value, nullptr, 10) >= 2 ? 2 : 1;
}

// Where the scripts' frames went, per frame on average, and with level 2 the
// scripts that took the most, like Studio's Script Performance window.
void printSimulationProfile(const lunar::SimulationProfile& profile, int level)
{
    // Outside frames (the scripts starting) everything is a total.
    const double perFrame = 1000.0 / std::max(profile.frames, 1);
    std::string line
        = std::format("  simulate split, {}: luau server {:.2f}, client {:.2f};",
                      profile.frames > 0 ? "ms/frame" : "ms", profile.luau[1] * perFrame, profile.luau[2] * perFrame);
    for (std::size_t i = 0; i < profile.phases.size(); ++i) {
        const double ms = profile.phases[i] * perFrame;
        if (ms < 0.005)
            continue;
        line += std::format(" {} {:.2f}", lunar::SimulationProfile::phaseNames[i], ms);
        if (const double luau = profile.phaseLuau[i] * perFrame; luau >= 0.005)
            line += std::format(" (luau {:.2f})", luau);
        line += ',';
    }
    line.pop_back();
    std::println(stderr, "{}", line);
    if (level < 2)
        return;
    constexpr std::size_t shown = 12;
    for (const lunar::ScriptActivity& a : profile.activity | std::views::take(shown)) {
        if (a.seconds < 0.0001)
            break;
        std::println(stderr, "    {:8.2f} ms {:6} x  {:6}  {}  [{}]", a.seconds * 1000.0, a.resumes,
                     a.role == lunar::ScriptRole::Server ? "server" : "client", a.script, a.what);
    }
}

class Session {
public:
    // With `scripts`, the script session owns the local player and its
    // characters (spawning, Died, respawns) and runs the frame in RunService's
    // order; this session drives whatever character the player has.
    Session(std::shared_ptr<lunar::DataModel> game, const PlayOptions& options, lunar::PlaySession* scripts = nullptr)
        : game_(std::move(game)), options_(options), scripts_(scripts), workspace_(*game_->workspace()),
          world_(workspace_), camera_(*workspace_.ensureCurrentCamera()), playerCamera_(world_, camera_),
          assets_(std::make_shared<lunar::assets::AssetCache>()), animation_(assets_), clothing_(assets_)
    {
        if (options.user.avatar && options.user.userId > 0)
            avatars_[options.user.userId] = options.user.avatar;
        world_.onStep = [this](float dt) {
            if (!controller_)
                return;
            // Where the root was before this step: the start of the span drawn between.
            if (const lunar::BasePart* root = controller_->humanoid().rootPart())
                stepStart_ = root->position();
            controller_->step(dt);
        };
        if (scripts_) {
            scripts_->setAnimationSystem(&animation_);
            scripts_->setCharacterFactory([this](lunar::Player& player) { return makeCharacter(player); });
            scripts_->setHooks({
                .animation = [this](double dt) { animate(static_cast<float>(dt)); },
                .physics = [this](double dt) { stepPhysics(static_cast<float>(dt)); },
            });
            scripts_->setRaycastHook(
                [this](const lunar::DataModel&, const lunar::RaycastQuery& query) { return raycast(query); });
            scripts_->start(options.user.name, options.user.userId, options.user.displayName);
            player_ = scripts_->localPlayer();
            for (int i = 2; i <= options.players; ++i)
                scripts_->addPlayer(std::format("Player{}", i), -i);
        } else {
            auto* players = game_->getService<lunar::Players>();
            auto player = std::make_shared<lunar::Player>();
            player->setName(options.user.name);
            player->setDisplayName(options.user.displayName.empty() ? options.user.name : options.user.displayName);
            player->setUserId(options.user.userId);
            player->setCharacterAppearanceId(std::max<std::int64_t>(options.user.userId, 0));
            player->setParent(players);
            players->assignLocalPlayer(player.get());
            player_ = player.get();
        }
        playerCamera_.setZoomLimits(*player_);
        playerCamera_.setMouseLockAllowed(options.shiftLock);
        if (scripts_)
            adoptCharacter();
        else
            spawn();
    }

    ~Session()
    {
        if (scripts_)
            scripts_->setAnimationSystem(nullptr);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    lunar::Camera& camera() const { return camera_; }
    // The menu's Shift Lock Switch and camera sensitivity.
    void setShiftLock(bool allowed) { playerCamera_.setMouseLockAllowed(allowed); }
    void setCameraSensitivity(float sensitivity) { sensitivity_ = sensitivity; }

    // A character for `player`, standing on a spawn, not parented yet.
    std::shared_ptr<lunar::Model> makeCharacter(lunar::Player& player)
    {
        auto character = options_.buildCharacter ? options_.buildCharacter(player) : defaultCharacter(player);
        // Feet on the spawn: lift the pivot by how far it sits above the model's bottom.
        const auto [box, size] = character->getBoundingBox();
        const float pivotAboveFeet = character->getPivot().position.y - (box.position.y - size.y * 0.5f);
        character->pivotTo(spawnPoint(workspace_) * lunar::CFrame(0.0f, pivotAboveFeet, 0.0f));
        lunar::liftOutOfObstructions(*character, [this](const lunar::RaycastQuery& query) { return raycast(query); });
        return character;
    }

    // Without scripts this session spawns (and respawns) the character itself.
    void spawn()
    {
        if (character_)
            character_->destroy();
        controller_.reset();
        auto character = makeCharacter(*player_);
        character->setParent(&workspace_);
        player_->setCharacter(character.get());
        adopt(std::move(character));
    }

    // Picks up a character the script session spawned (or respawned).
    void adoptCharacter()
    {
        lunar::Model* current = player_ ? player_->character() : nullptr;
        if (current == character_.get())
            return;
        controller_.reset();
        character_.reset();
        if (current)
            adopt(std::static_pointer_cast<lunar::Model>(current->shared_from_this()));
    }

    void adopt(std::shared_ptr<lunar::Model> character)
    {
        character_ = std::move(character);
        auto* humanoid = static_cast<lunar::Humanoid*>(character_->findFirstChildOfClass("Humanoid"));
        if (!humanoid) {
            std::println(stderr, "The character has no Humanoid");
            return;
        }
        if (const auto* starter = game_->findService("StarterPlayer"))
            static_cast<const lunar::StarterPlayer*>(starter)->applyCharacterSettings(*humanoid);
        world_.refresh();
        controller_ = std::make_unique<physics::HumanoidController>(world_, *humanoid);
        playerCamera_.setSubject(humanoid);
        animation_.add(character_, !scriptAnimated(*character_));
        lastState_.reset();
        hasSpan_ = false;
        deadFor_.reset();
    }

    // Composited Shirt/Pants textures resolve through this.
    lunar::avatar::ClothingCompositor& clothing() { return clothing_; }

    // One frame: controls and camera (RenderStepped), scripts, then physics.
    // `uiHasKeys`: the chat, a TextBox, the menu or the map has the
    // keyboard, so the character and camera get no input this frame.
    // `uiHasWheel`: the wheel is the GUI's (over the minimap), not the camera's.
    void frame(const InputState& input, float dt, float aspect, bool pointerLocked, bool uiHasKeys,
               bool uiHasWheel = false)
    {
        if (scripts_)
            adoptCharacter();
        if (!controller_) {
            if (scripts_)
                scripts_->step(dt); // keep scripts running while there's no character
            return;
        }
        lunar::Humanoid& humanoid = controller_->humanoid();
        const bool active = input.focused() && !uiHasKeys;
        // A script's ControlModule:Disable() holds the character still.
        const bool controls = active && (!scripts_ || scripts_->controlsEnabled());

        // ControlModule at RenderPriority.Input, with last frame's camera.
        const Vector3 move = controls ? keyboardMoveVector(input) : Vector3{};
        driveSeat(humanoid, move);
        controller_->move(cameraRelative(move, camera_.cframe()));
        humanoid.setJump(controls && input.isDown(Key::Space));

        // Simulate and pose first, then point the camera, so the camera and the
        // character drawn this frame are from the same moment. With scripts,
        // RunService's frame runs animation and physics through the hooks.
        if (const lunar::BasePart* root = humanoid.rootPart())
            frameStart_ = root->position();
        if (scripts_)
            scripts_->step(dt);
        else
            advance(dt);
        if (!controller_)
            return;

        // CameraModule at RenderPriority.Camera.
        const physics::PlayerCamera::Input in
            = cameraInput(input, active, pointerLocked, !uiHasWheel, aspect, sensitivity_);
        cameraInput_ = in;
        // A Scriptable camera is the place's own: the CameraModule leaves it
        // alone (the Weapons Kit's shoulder camera drives it from RenderStepped).
        if (scriptedCamera()) {
            controller_->setFacing(std::nullopt);
        } else {
            updateCamera(dt, in);
            controller_->setFacing(playerCamera_.cameraRelativeRotation()
                                       ? std::optional<Vector3>(camera_.cframe().lookVector())
                                       : std::nullopt);
        }

        // Players.RespawnTime after dying, a new character (the script session
        // does this itself when it runs).
        if (!scripts_ && humanoid.getState() == lunar::Enum::HumanoidStateType::Dead) {
            deadFor_ = deadFor_.value_or(0.0f) + dt;
            if (*deadFor_ >= game_->getService<lunar::Players>()->respawnTime())
                spawn();
        }
    }

    bool scriptedCamera() const { return camera_.cameraType() == lunar::Enum::CameraType::Scriptable; }

    // The mouse is captured when the camera wants it or a script set
    // UserInputService.MouseBehavior to lock it.
    bool wantsPointerLock() const
    {
        if (scripts_) {
            const auto* input = dynamic_cast<const lunar::UserInputService*>(game_->findService("UserInputService"));
            if (input && input->mouseBehavior() != lunar::Enum::MouseBehavior::Default)
                return true;
            if (scriptedCamera())
                return false;
        }
        return playerCamera_.pointerLocked(cameraInput_);
    }

    // The same frame without a window: walk along `move` (camera space), jumping when asked.
    void frame(Vector3 move, bool jump, float dt)
    {
        if (!controller_) {
            if (scripts_) {
                scripts_->step(dt); // keep scripts running while there's no character
                adoptCharacter();
            }
            return;
        }
        if (scripts_ && !scripts_->controlsEnabled()) {
            move = {};
            jump = false;
        }
        driveSeat(controller_->humanoid(), move);
        controller_->move(cameraRelative(move, camera_.cframe()));
        controller_->humanoid().setJump(jump);
        if (const lunar::BasePart* root = controller_->humanoid().rootPart())
            frameStart_ = root->position();
        if (scripts_) {
            scripts_->step(dt);
            adoptCharacter();
        } else {
            advance(dt);
        }
        if (!scriptedCamera())
            updateCamera(dt, physics::PlayerCamera::Input{});
    }

    // The CameraModule's frame, with the player's current zoom limits (the
    // battle royale template raises the minimum while following its bus).
    void updateCamera(float dt, const physics::PlayerCamera::Input& in)
    {
        playerCamera_.setZoomLimits(*player_);
        playerCamera_.update(dt, in);
    }

    // Physics runs in fixed 240 Hz steps, so a 160 Hz frame gets one or two of
    // them. Drawing the character (and the camera following it) between the
    // last two steps keeps it smooth; the simulation itself is untouched.
    Vector3 renderOffset() const
    {
        if (!controller_ || !hasSpan_)
            return {};
        const float alpha = std::clamp(world_.interpolationAlpha(), 0.0f, 1.0f);
        return (spanEnd_ - spanStart_) * (alpha - 1.0f);
    }
    const lunar::Model* character() const { return character_.get(); }

    // Where to draw the camera relative to its CFrame. The default camera
    // follows the character after physics, so it moves with renderOffset. A
    // scripted one (the Weapons Kit's shoulder camera) was placed in
    // RenderStepped from where the root part was before this frame's
    // physics: it moves by however far the drawn character has come since,
    // or it trails a frame behind.
    Vector3 cameraOffset() const
    {
        if (!controller_)
            return {};
        // Following something else than the character (a part, a model).
        if (!scriptedCamera() && camera_.cameraSubject() != &controller_->humanoid())
            return {};
        if (!scriptedCamera())
            return renderOffset();
        const lunar::BasePart* root = controller_->humanoid().rootPart();
        return root ? root->position() + renderOffset() - frameStart_ : Vector3{};
    }

    lunar::Humanoid* humanoid() const { return controller_ ? &controller_->humanoid() : nullptr; }

    // What the character is playing, for headless runs: who drives it
    // (the native Animate controller or a script) and each playing track's
    // name and weight.
    std::string animationSummary()
    {
        auto* entry = character_ ? animation_.find(*character_) : nullptr;
        if (!entry)
            return "no animator";
        std::string text = entry->animate ? "native:" : "script:";
        for (const lunar::avatar::TrackPtr& track : entry->animator->playingTracks())
            if (track->weightCurrent() >= 0.01f)
                text += std::format(" {} {:.2f}", track->name(), track->weightCurrent());
        return text;
    }
    const physics::World& world() const { return world_; }
    physics::World& world() { return world_; }

    // Puts the character's feet at `feet`, standing still.
    void teleport(const lunar::CFrame& feet)
    {
        if (!character_)
            return;
        const auto [box, size] = character_->getBoundingBox();
        const float pivotAboveFeet = character_->getPivot().position.y - (box.position.y - size.y * 0.5f);
        character_->pivotTo(feet * lunar::CFrame(0.0f, pivotAboveFeet, 0.0f));
        for (const lunar::InstancePtr& d : character_->descendants())
            if (auto* part = dynamic_cast<lunar::BasePart*>(d.get())) {
                part->setAssemblyLinearVelocity({});
                part->setAssemblyAngularVelocity({});
            }
        hasSpan_ = false;
    }

    // A frame walking along a world-space direction instead of a camera-relative one.
    void frameWorld(Vector3 move, bool jump, float dt)
    {
        if (!controller_)
            return;
        controller_->move(move);
        controller_->humanoid().setJump(jump);
        advance(dt);
    }

private:
    // A VehicleSeat takes the occupant's controls (the client's VehicleController).
    static void driveSeat(lunar::Humanoid& humanoid, Vector3 move)
    {
        if (auto* seat = dynamic_cast<lunar::VehicleSeat*>(humanoid.seatPart()))
            lunar::driveVehicleSeat(*seat, move);
    }

    // The avatar of the user a player's CharacterAppearanceId names, fetched
    // once (like the client's character loader, which asks avatar-fetch for
    // it on each spawn); null for none (0, test players) or when it can't
    // be had.
    std::shared_ptr<const lunar::avatar::UserAvatar> appearance(std::int64_t userId)
    {
        if (userId <= 0)
            return nullptr;
        if (auto known = avatars_.find(userId); known != avatars_.end())
            return known->second;
        std::shared_ptr<const lunar::avatar::UserAvatar> avatar;
        if (assets_->config().download && lunar::assets::AssetCache::canDownload()) {
            auto fetched = lunar::avatar::fetchUserAvatar(*assets_->config().web, userId);
            if (fetched)
                avatar = std::make_shared<const lunar::avatar::UserAvatar>(std::move(*fetched));
            else
                std::println(stderr, "avatar: {}; using the default avatar", fetched.error());
        }
        avatars_[userId] = avatar;
        return avatar;
    }

    // The player's avatar (CharacterAppearanceId), else the default R15
    // avatar in default clothing; the blocky placeholder if it can't be built.
    std::shared_ptr<lunar::Model> defaultCharacter(lunar::Player& player)
    {
        lunar::avatar::CharacterOptions character;
        character.name = player.name();
        character.assets = assets_;
        std::vector<std::string> problems;
        const auto avatar = appearance(player.characterAppearanceId());
        if (avatar) {
            character.rigType = avatar->rigType;
            character.description = avatar->description.get();
            character.problems = &problems;
        }
        // Game Settings > Avatar decides the rig unless it's Player Choice.
        if (const auto* starter = dynamic_cast<const lunar::StarterPlayer*>(game_->findService("StarterPlayer"))) {
            if (starter->gameSettingsAvatar() == lunar::Enum::GameAvatarType::R6)
                character.rigType = lunar::Enum::HumanoidRigType::R6;
            else if (starter->gameSettingsAvatar() == lunar::Enum::GameAvatarType::R15)
                character.rigType = lunar::Enum::HumanoidRigType::R15;
        }
        auto made = lunar::avatar::createCharacter(character);
        for (const std::string& problem : problems)
            std::println(stderr, "avatar: {}", problem);
        if (!made) {
            std::println(stderr, "avatar: {}; using the placeholder character", made.error());
            return physics::buildPlaceholderCharacter(player.name());
        }
        clothing_.apply(**made);
        return *made;
    }

    void advance(float dt)
    {
        stepPhysics(dt);
        animate(dt);
    }

    void stepPhysics(float dt)
    {
        const int steps = world_.advance(dt);
        if (steps > 0 && controller_)
            if (const lunar::BasePart* root = controller_->humanoid().rootPart()) {
                spanStart_ = stepStart_;
                spanEnd_ = root->position();
                hasSpan_ = true;
            }
    }

    // Workspace:Raycast for scripts, answered by the physics world.
    std::optional<lunar::RaycastHit> raycast(const lunar::RaycastQuery& query)
    {
        physics::RaycastParams params;
        params.filterType
            = query.filterIsInclude ? lunar::Enum::RaycastFilterType::Include : lunar::Enum::RaycastFilterType::Exclude;
        for (const lunar::InstancePtr& instance : query.filter)
            params.filterDescendantsInstances.push_back(instance.get());
        params.ignoreWater = query.ignoreWater;
        params.respectCanCollide = query.respectCanCollide;
        params.collisionGroup = query.collisionGroup;
        const auto hit = world_.raycast(query.origin, query.direction, params);
        if (!hit || !hit->instance)
            return std::nullopt;
        return lunar::RaycastHit{.instance = hit->instance->shared_from_this(),
                                 .position = hit->position,
                                 .normal = hit->normal,
                                 .material = hit->material,
                                 .distance = hit->distance};
    }

    // Whether the character's own Animate script animates it (through
    // LoadAnimation), so the native AnimateController stands aside. That
    // controller is the fallback for characters without one running: no
    // script session, or a character whose Animate is the avatar's empty
    // stand-in.
    bool scriptAnimated(const lunar::Model& character) const
    {
        if (!scripts_)
            return false;
        return std::ranges::any_of(character.children(), [](const lunar::InstancePtr& child) {
            const auto* script = dynamic_cast<const lunar::LuaSourceContainer*>(child.get());
            return script && script->name() == "Animate" && !script->source().empty();
        });
    }

    // After physics moved the root part: tell the Animate logic what the
    // Humanoid did (the events it connects to), then pose the joints. With a
    // running Animate script, the script hears the Humanoid's events itself
    // and its tracks are already on this character's Animator.
    void animate(float dt)
    {
        if (!controller_ || !character_)
            return;
        lunar::Humanoid& humanoid = controller_->humanoid();
        auto* entry = animation_.find(*character_);
        if (entry && entry->animate && scriptAnimated(*character_))
            entry->animate.reset(); // an Animate script arrived after the character: it takes over
        if (entry && entry->animate) {
            float speed = 0.0f;
            if (const lunar::BasePart* root = humanoid.rootPart()) {
                const Vector3 v = root->assemblyLinearVelocity();
                speed = Vector3{v.x, 0.0f, v.z}.magnitude();
            }
            const auto state = humanoid.getState();
            const auto animState = static_cast<lunar::avatar::HumanoidState>(state);
            // Humanoid.Running fires when the speed changes, not every step:
            // each call rerolls the idle choice, which restarted the blend.
            if (state != lastState_) {
                entry->animate->onStateChanged(animState, speed);
                lastState_ = state;
                lastSpeed_ = speed;
            } else if (state == lunar::Enum::HumanoidStateType::Running && speed != lastSpeed_) {
                entry->animate->onRunning(speed);
                lastSpeed_ = speed;
            }
        }
        animation_.step(dt);
    }

    std::shared_ptr<lunar::DataModel> game_;
    PlayOptions options_;
    lunar::PlaySession* scripts_ = nullptr;
    lunar::Workspace& workspace_;
    physics::World world_;
    lunar::Camera& camera_;
    physics::PlayerCamera playerCamera_;
    lunar::Player* player_ = nullptr;
    std::shared_ptr<lunar::Model> character_;
    std::unique_ptr<physics::HumanoidController> controller_;
    physics::PlayerCamera::Input cameraInput_;
    float sensitivity_ = 1.0f;
    std::optional<float> deadFor_;
    std::shared_ptr<lunar::assets::AssetCache> assets_;
    lunar::avatar::AnimationSystem animation_;
    lunar::avatar::ClothingCompositor clothing_;
    std::unordered_map<std::int64_t, std::shared_ptr<const lunar::avatar::UserAvatar>> avatars_;
    std::optional<lunar::Enum::HumanoidStateType> lastState_;
    float lastSpeed_ = 0.0f;
    Vector3 frameStart_; // the root part's position before this frame's simulation
    Vector3 stepStart_;
    Vector3 spanStart_;
    Vector3 spanEnd_;
    bool hasSpan_ = false;
};

} // namespace

namespace {

// --simulate --give: a copy of the first Tool by that name anywhere in the
// game, into the local player's Backpack.
std::shared_ptr<lunar::Tool> giveTool(lunar::DataModel& game, lunar::PlaySession& scripts, std::string_view name)
{
    lunar::Player* player = scripts.localPlayer();
    lunar::Instance* backpack = player ? player->findFirstChildOfClass("Backpack") : nullptr;
    for (const lunar::InstancePtr& d : game.descendants())
        if (d->isA("Tool") && d->name() == name && backpack && !d->isDescendantOf(backpack)) {
            auto tool = std::dynamic_pointer_cast<lunar::Tool>(d->clone());
            if (!tool)
                break;
            tool->setParent(backpack);
            std::println("gave {} (from {})", name, d->fullName());
            return tool;
        }
    std::println(stderr, "no Tool named {}", name);
    return nullptr;
}

// Equips it after half a second, clicks at one second, and says what the
// grip looks like.
void driveGivenTool(lunar::PlaySession& scripts, lunar::Tool& tool, int frame)
{
    lunar::Player* player = scripts.localPlayer();
    lunar::Model* character = player ? player->character() : nullptr;
    auto* humanoid = character ? dynamic_cast<lunar::Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (!humanoid)
        return;
    if (frame == 30) {
        lunar::equipTool(scripts.game(), *humanoid, tool);
        std::println("equipped: parent {}", tool.parent() ? tool.parent()->name() : "nil");
    } else if (frame == 60) {
        scripts.mouseButton(lunar::Enum::UserInputType::MouseButton1, true);
    } else if (frame == 62) {
        scripts.mouseButton(lunar::Enum::UserInputType::MouseButton1, false);
    }
    if (frame == 31 || frame == 90) {
        const lunar::Weld* grip = lunar::rightGrip(*humanoid);
        const lunar::BasePart* handle = tool.handle();
        const lunar::BasePart* limb = lunar::gripLimb(*humanoid);
        std::println("t={:.2f} RightGrip {} in {}; handle {} from the hand", static_cast<float>(frame) / 60.0f,
                     grip ? "present" : "missing", limb ? limb->name() : "no hand",
                     handle && limb ? (handle->position() - limb->position()).magnitude() : -1.0f);
    }
}

// --simulate --gogogo: starts a round a second in, then follows the
// delivery vehicle (the battle bus: a model a PrismaticConstraint motor
// pulls along its path) and jumps out with Space once it's Ready.
class RoundDriver {
public:
    void frame(lunar::PlaySession& scripts, lunar::Workspace& workspace, int frame, float time)
    {
        if (frame == 60) {
            auto started = scripts.server().run(goGoGoSource, "GoGoGo");
            std::println("t={:5.2f} gogogo: {}", time, started ? "round started" : started.error().message);
        }
        if (frame == spaceUp_)
            scripts.keyUp(lunar::Enum::KeyCode::Space);
        lunar::Model* bus = nullptr;
        for (const lunar::InstancePtr& child : workspace.children())
            if (child->name().starts_with("DeliveryVehicle") && child->isA("Model"))
                bus = static_cast<lunar::Model*>(child.get());
        if (!bus)
            return;
        const auto* slider
            = dynamic_cast<lunar::SlidingBallConstraint*>(bus->findFirstChildOfClass("PrismaticConstraint"));
        const auto* ready = dynamic_cast<lunar::BoolValue*>(bus->findFirstChild("Ready"));
        if (frame % 30 == 0)
            if (const lunar::BasePart* root = bus->primaryPart()) {
                const Vector3 p = root->position();
                const Vector3 v = root->assemblyLinearVelocity();
                std::println(
                    "t={:5.2f} {} at ({:8.1f}, {:7.1f}, {:8.1f}) speed {:6.1f}, CurrentPosition {:7.1f}, Ready {}",
                    time, bus->name(), p.x, p.y, p.z, v.magnitude(), slider ? slider->currentPosition() : 0.0f,
                    ready && ready->value());
                if (const lunar::Camera* camera = workspace.currentCamera()) {
                    const Vector3 c = camera->cframe().position;
                    const lunar::Instance* subject = camera->cameraSubject();
                    std::println("        camera at ({:8.1f}, {:7.1f}, {:8.1f}), {:.1f} from the bus, subject {}", c.x,
                                 c.y, c.z, (c - p).magnitude(), subject ? subject->name() : "nil");
                }
            }
        if (ready && ready->value() && !jumped_) {
            std::println("t={:5.2f} Ready: pressing Space to jump out", time);
            scripts.keyDown(lunar::Enum::KeyCode::Space);
            spaceUp_ = frame + 2;
            jumped_ = true;
        }
    }

private:
    bool jumped_ = false;
    int spaceUp_ = -1;
};

// --simulate --drive: six seconds in, the character stands beside the first
// vehicle in Workspace.Vehicles and asks to get in the way the Dune Buggy's
// SeatLocator does (its GetInSeat remote, whose server side calls
// VehicleSeat:Sit); W is held from eight to thirteen, and a jump at fifteen
// gets it out.
class DriveDriver {
public:
    // Whether to jump this frame.
    bool frame(lunar::PlaySession& scripts, Session& session, lunar::Workspace& workspace, int frame, float time)
    {
        if (frame == 360) {
            if (lunar::Instance* vehicles = workspace.findFirstChild("Vehicles"))
                for (const lunar::InstancePtr& vehicle : vehicles->children())
                    for (const lunar::InstancePtr& d : vehicle->descendants())
                        if (auto* found = dynamic_cast<lunar::VehicleSeat*>(d.get()); found && !seat_)
                            seat_ = std::static_pointer_cast<lunar::VehicleSeat>(found->shared_from_this());
            if (!seat_) {
                std::println("t={:5.2f} drive: no vehicle with a VehicleSeat in Workspace.Vehicles", time);
                return false;
            }
            const lunar::CFrame beside = seat_->cframe() * lunar::CFrame(-7.0f, 4.0f, 0.0f);
            session.teleport(lunar::CFrame(beside.position) * seat_->cframe().rotation());
            std::println("t={:5.2f} drive: beside {}", time, seat_->fullName());
        }
        if (frame == 420 && seat_) {
            auto asked = scripts.client().run(R"(
                for _, vehicle in workspace.Vehicles:GetChildren() do
                    local seat = vehicle:FindFirstChildWhichIsA("VehicleSeat", true)
                    if seat then
                        vehicle.Remotes.GetInSeat:FireServer(seat)
                        return
                    end
                end
            )",
                                              "Drive");
            std::println("t={:5.2f} drive: GetInSeat {}", time, asked ? "sent" : asked.error().message);
        }
        if (frame == 480)
            scripts.keyDown(lunar::Enum::KeyCode::W);
        if (frame == 780)
            scripts.keyUp(lunar::Enum::KeyCode::W);
        if (seat_ && frame >= 360 && frame % 30 == 0) {
            const Vector3 p = seat_->position();
            const lunar::Humanoid* sitter = seat_->occupant();
            const lunar::Instance* model = seat_->parent() ? seat_->parent()->parent() : nullptr;
            const auto* motor
                = model ? dynamic_cast<const lunar::CylindricalConstraint*>(model->findFirstChild("MotorRL", true))
                        : nullptr;
            const lunar::Humanoid* humanoid = session.humanoid();
            std::print("t={:5.2f} seat at ({:8.1f}, {:6.1f}, {:8.1f}) speed {:5.1f}, ", time, p.x, p.y, p.z,
                       seat_->assemblyLinearVelocity().magnitude());
            std::println("Occupant {}, Sit {}, MotorRL velocity {:.1f} torque {:.0f}",
                         sitter && sitter->parent() ? sitter->parent()->name() : "nil", humanoid && humanoid->sit(),
                         motor ? motor->angularVelocity() : 0.0f, motor ? motor->motorMaxTorque() : 0.0f);
        }
        return frame >= 900 && frame < 905;
    }

private:
    std::shared_ptr<lunar::VehicleSeat> seat_;
};

} // namespace

int simulatePlace(const std::filesystem::path& path, const SimulateOptions& options)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::optional<lunar::PlaySession> scripts;
    const int profiling = profileLevel();
    if (options.scripts) {
        scripts.emplace(place->game);
        scripts->setOutput([](lunar::ScriptRole role, lunar::MessageType type, std::string_view text) {
            if (type != lunar::MessageType::Info)
                std::println(stderr, "[{}] {}", role == lunar::ScriptRole::Server ? "server" : "client", text);
        });
        scripts->setProfiling(profiling > 0);
    }
    Session session(place->game, {.players = options.players, .user = options.user}, scripts ? &*scripts : nullptr);
    const float setup = std::chrono::duration<float>(Clock::now() - start).count();
    std::println("{} colliders, set up in {:.2f} s", session.world().colliderCount(), setup);
    if (scripts && profiling > 1) {
        std::println(stderr, "starting the scripts:");
        printSimulationProfile(scripts->takeProfile(), profiling);
    }
    {
        // The physics part alone: a second world over the same parts.
        const auto t0 = Clock::now();
        physics::World world(*place->game->workspace());
        world.refresh();
        std::println("physics world alone: {:.3f} s ({} with convex pieces sharing {} geometries)",
                     std::chrono::duration<float>(Clock::now() - t0).count(), world.meshColliderCount(),
                     world.meshGeometryCount());
    }
    constexpr float dt = 1.0f / 60.0f;
    const int frames = static_cast<int>(options.seconds / dt);
    float busiest = 0.0f;
    float total = 0.0f;
    std::shared_ptr<lunar::Tool> given;
    lunar::PlaySession* playSession = scripts ? &*scripts : nullptr;
    if (playSession && !options.give.empty())
        given = giveTool(*place->game, *playSession, options.give);
    RoundDriver round;
    DriveDriver drive;
    float secondTotal = 0.0f;
    float secondWorst = 0.0f;
    // --map: the scene and the map's upkeep, timed apart from the simulation.
    std::unique_ptr<PlaceScene> mapPlace;
    std::unique_ptr<MapScene> mapScene;
    lunar::Connection mapAdded;
    lunar::Connection mapRemoving;
    double mapSeconds = 0.0;
    if (options.map) {
        mapPlace = std::make_unique<PlaceScene>(*place->game);
        auto touch = [scene = mapPlace.get()](lunar::Instance* instance) { scene->touch(*instance); };
        mapAdded = place->game->workspace()->descendantAdded.connect(touch);
        mapRemoving = place->game->workspace()->descendantRemoving.connect(touch);
        mapScene = std::make_unique<MapScene>(*mapPlace);
    }
    for (int i = 0; i < frames; ++i) {
        if (given && playSession)
            driveGivenTool(*playSession, *given, i);
        if (playSession && options.goGoGo)
            round.frame(*playSession, *place->game->workspace(), i, static_cast<float>(i) * dt);
        const auto t0 = Clock::now();
        // Walk forward; jump once a second.
        const bool jump
            = playSession && options.drive
              && drive.frame(*playSession, session, *place->game->workspace(), i, static_cast<float>(i) * dt);
        if (options.idle)
            session.frame({}, jump, dt);
        else
            session.frame({0.0f, 0.0f, -1.0f}, i % 60 < 5, dt);
        const float spent = std::chrono::duration<float>(Clock::now() - t0).count();
        if (mapScene) {
            mapPlace->refresh();
            mapPlace->followParts();
            const auto m0 = Clock::now();
            mapScene->update(static_cast<double>(i) * static_cast<double>(dt));
            mapSeconds += std::chrono::duration<double>(Clock::now() - m0).count();
            mapScene->takeMesh();
            mapScene->takeMoving();
            if ((i + 1) % 60 == 0) {
                const MapScene::Counters& c = mapScene->counters();
                const auto mesh = mapScene->mesh();
                std::println("map: {:.3f} ms/frame upkeep this second; {} rebuilds", mapSeconds * 1000.0 / 60.0,
                             c.rebuilds);
                std::println("  {} moving parts as boxes, {} anchored seen moving", c.movingParts, c.moved);
                std::println("  latest mesh {} instances, {:.1f} MiB", mesh ? mesh->instances.size() : 0,
                             mesh ? static_cast<double>(mesh->bytes()) / (1 << 20) : 0.0);
                mapSeconds = 0.0;
            }
        }
        busiest = std::max(busiest, spent);
        total += spent;
        secondTotal += spent;
        secondWorst = std::max(secondWorst, spent);
        // LUNAR_PROFILE: each simulated second's frame times and where they went.
        if (profiling && (i + 1) % 60 == 0) {
            std::println(stderr, "second {:3}: simulate {:.2f} ms average, {:.2f} ms worst", (i + 1) / 60,
                         secondTotal / 60.0f * 1000.0f, secondWorst * 1000.0f);
            if (playSession)
                printSimulationProfile(playSession->takeProfile(), profiling);
            secondTotal = 0.0f;
            secondWorst = 0.0f;
        }
        if (i % 30 == 0)
            if (lunar::Humanoid* h = session.humanoid(); h && h->rootPart()) {
                const Vector3 p = h->rootPart()->position();
                const Vector3 v = h->rootPart()->assemblyLinearVelocity();
                std::println("t={:5.2f} pos=({:7.2f}, {:7.2f}, {:7.2f}) vel=({:6.2f}, {:6.2f}, {:6.2f}) {} [{}]",
                             static_cast<float>(i) * dt, p.x, p.y, p.z, v.x, v.y, v.z,
                             lunar::enums::item(h->getState()).name, session.animationSummary());
            }
    }
    std::println("frame time: {:.2f} ms average, {:.2f} ms worst; {} terrain blocks meshed for collision",
                 total / static_cast<float>(frames) * 1000.0f, busiest * 1000.0f, session.world().terrainBlockCount());
    const physics::World::Stats& stats = session.world().stats();
    const double updateMs = stats.updates ? stats.updateSeconds / static_cast<double>(stats.updates) * 1000.0 : 0.0;
    std::println("physics updates: {} ({:.2f} ms average, {:.2f} ms worst), {} static rebuilds", stats.updates,
                 updateMs, stats.worstUpdateSeconds * 1000.0, stats.staticRebuilds);
    std::println("last step: {} assemblies awake, {} asleep, {} contacts; {} of {} constraints acting ({} rows)",
                 stats.awakeAssemblies, stats.sleepingAssemblies, stats.contacts, stats.activeConstraints,
                 stats.constraints, stats.rows);
    std::println("physics steps: {:.2f} ms a frame on average; {} of {} spread over up to {} threads",
                 stats.stepSeconds / static_cast<double>(frames) * 1000.0, stats.parallelSteps, stats.steps,
                 session.world().threads());
    return 0;
}

namespace {

// Distance from the vertical line through `axis`, ignoring height.
float horizontalDistance(Vector3 a, Vector3 axis)
{
    const float dx = a.x - axis.x;
    const float dz = a.z - axis.z;
    return std::sqrt(dx * dx + dz * dz);
}

struct WalkResult {
    float distance = 0.0f; // from the tree's axis where the character stopped
    std::string blockedBy; // the part right in front of the character, if any
    float frameMs = 0.0f;  // average time per 60 Hz frame
};

// Puts the character `start` studs out along +X from the part's axis, on
// whatever is below, and walks it at the part for `seconds`.
WalkResult walkInto(Session& session, lunar::BasePart& target, float start, float seconds)
{
    const lunar::CFrame cf = target.cframe();
    const Vector3 axis = cf.position;
    const Vector3 out{1.0f, 0.0f, 0.0f};
    Vector3 from = axis + out * start;
    lunar::Humanoid* humanoid = session.humanoid();
    physics::RaycastParams params;
    params.filterDescendantsInstances = {session.character()};
    // Ground under the start point, from above the part's top.
    const float top = axis.y + target.size().magnitude();
    auto ground = session.world().raycast({from.x, top, from.z}, {0.0f, -top * 4.0f - 2000.0f, 0.0f}, params);
    from.y = ground ? ground->position.y : axis.y - target.size().y * 0.5f;
    std::println("  start ({:.2f}, {:.2f}, {:.2f}) on {}", from.x, from.y, from.z,
                 ground && ground->instance ? ground->instance->name() : "nothing");
    session.teleport(lunar::CFrame(from) * lunar::CFrame::angles(0.0f, std::numbers::pi_v<float> * 0.5f, 0.0f));
    constexpr float dt = 1.0f / 60.0f;
    const int frames = static_cast<int>(seconds / dt);
    float spent = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        session.frameWorld(-out, false, dt);
        spent += std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        if (i % 30 == 0 && humanoid && humanoid->rootPart()) {
            const Vector3 p = humanoid->rootPart()->position();
            std::println("  t={:.1f} root ({:.2f}, {:.2f}, {:.2f}) {:.2f} from the axis, {}",
                         static_cast<float>(i) * dt, p.x, p.y, p.z, horizontalDistance(p, axis),
                         lunar::enums::item(humanoid->getState()).name);
        }
    }
    WalkResult result;
    result.frameMs = spent / static_cast<float>(std::max(frames, 1)) * 1000.0f;
    if (humanoid && humanoid->rootPart()) {
        const Vector3 p = humanoid->rootPart()->position();
        result.distance = horizontalDistance(p, axis);
        // What's straight ahead of the character, at waist height?
        if (auto hit = session.world().raycast(p, -out * 3.0f, params); hit && hit->instance)
            result.blockedBy = std::format("{} at {:.2f} studs", hit->instance->name(), hit->distance);
    }
    return result;
}

} // namespace

int treeTest(const std::filesystem::path& path, std::string_view prefix)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    Session session(place->game, {});
    physics::World& world = session.world();
    std::println("{} colliders, {} with convex pieces sharing {} geometries", world.colliderCount(),
                 world.meshColliderCount(), world.meshGeometryCount());

    // The first anchored, colliding MeshPart with that name prefix that has
    // pieces and stands on level ground, so a character can walk up to its trunk.
    lunar::MeshPart* tree = nullptr;
    physics::Shape shape;
    for (const lunar::InstancePtr& d : place->game->workspace()->descendants()) {
        auto* part = dynamic_cast<lunar::MeshPart*>(d.get());
        if (!part || !part->anchored() || !part->canCollide() || !part->name().starts_with(prefix))
            continue;
        shape = physics::shapeOf(*part);
        if (shape.kind != physics::ShapeKind::Mesh)
            continue;
        const Vector3 c = part->position();
        const float bottom = c.y - part->size().y * 0.5f;
        physics::RaycastParams notThis;
        notThis.filterDescendantsInstances = {part, session.character()};
        bool level = true;
        for (float out : {0.0f, std::max(part->size().x, part->size().z) * 0.5f + 4.0f}) {
            auto ground = world.raycast({c.x + out, bottom + 3.0f, c.z}, {0.0f, -8.0f, 0.0f}, notThis);
            level = level && ground && std::abs(ground->position.y - bottom) < 1.0f;
        }
        if (level) {
            tree = part;
            break;
        }
    }
    if (!tree) {
        std::println(stderr, "no anchored MeshPart named {}* with collision pieces", prefix);
        return 1;
    }
    const lunar::CFrame cf = tree->cframe();
    const Vector3 half = tree->size() * 0.5f;
    std::println("{}: Size ({:.2f}, {:.2f}, {:.2f}) at ({:.1f}, {:.1f}, {:.1f})", tree->fullName(), tree->size().x,
                 tree->size().y, tree->size().z, cf.position.x, cf.position.y, cf.position.z);
    std::println("{} convex pieces, scale ({:.3f}, {:.3f}, {:.3f})", shape.mesh->pieces.size(), shape.scale.x,
                 shape.scale.y, shape.scale.z);

    // Rays from all round at a few heights, only against the tree.
    physics::RaycastParams only;
    only.filterDescendantsInstances = {tree};
    only.filterType = lunar::Enum::RaycastFilterType::Include;
    const float reach = std::max(half.x, half.z) + 5.0f;
    std::println("horizontal rays toward the axis (box half widths {:.2f} x {:.2f}):", half.x, half.z);
    for (float h : {0.05f, 0.15f, 0.3f, 0.5f, 0.7f, 0.9f}) {
        const float y = cf.position.y - half.y + tree->size().y * h;
        float nearest = std::numeric_limits<float>::max();
        float furthest = 0.0f;
        int hits = 0;
        for (int k = 0; k < 16; ++k) {
            const float a = static_cast<float>(k) * std::numbers::pi_v<float> / 8.0f;
            const Vector3 dir{std::cos(a), 0.0f, std::sin(a)};
            const Vector3 origin = Vector3{cf.position.x, y, cf.position.z} + dir * reach;
            if (auto hit = world.raycast(origin, -dir * reach, only)) {
                const float d = horizontalDistance(hit->position, cf.position);
                nearest = std::min(nearest, d);
                furthest = std::max(furthest, d);
                ++hits;
            }
        }
        if (hits == 0)
            std::println("  {:3.0f}% up: no hits (empty space)", h * 100.0f);
        else
            std::println("  {:3.0f}% up: {:2} of 16 hit, surface {:.2f} to {:.2f} studs from the axis", h * 100.0f,
                         hits, nearest, furthest);
    }

    // A 1-stud cube inside the box, low down near a corner: empty space around the trunk.
    const physics::Shape cube = physics::makeShape(physics::ShapeKind::Box, {1.0f, 1.0f, 1.0f});
    const lunar::CFrame corner = cf * lunar::CFrame(half.x * 0.8f, -half.y * 0.8f, half.z * 0.8f);
    physics::Manifold contact;
    std::println("cube inside the box near its bottom corner: {}",
                 physics::collide(cube, corner, shape, cf, contact, 0.0f) ? "COLLIDES (box-like)"
                                                                          : "no contact (outside the pieces)");
    const lunar::CFrame trunk = cf * lunar::CFrame(0.0f, -half.y * 0.8f, 0.0f);
    std::println("cube on the axis near the bottom (trunk): {}",
                 physics::collide(cube, trunk, shape, cf, contact, 0.0f) ? "collides" : "NO CONTACT");

    // Walk into it, then the same with the data removed: the old box.
    const float start = std::max(half.x, half.z) + 4.0f;
    const WalkResult withPieces = walkInto(session, *tree, start, 4.0f);
    std::println("walking at it from {:.1f} studs out: stopped {:.2f} studs from the axis{}, {:.3f} ms a frame", start,
                 withPieces.distance, withPieces.blockedBy.empty() ? "" : " (blocked by " + withPieces.blockedBy + ")",
                 withPieces.frameMs);
    const lunar::SharedBytes blob = tree->physicalConfigData();
    tree->setPhysicalConfigData(nullptr);
    tree->setPhysicsData(nullptr);
    world.refresh();
    const WalkResult asBox = walkInto(session, *tree, start, 4.0f);
    std::println("same walk as its box: stopped {:.2f} studs from the axis (box face {:.2f}), {:.3f} ms a frame",
                 asBox.distance, half.x, asBox.frameMs);
    tree->setPhysicalConfigData(blob);
    return 0;
}

lunar::CFrame spawnPoint(lunar::Workspace& workspace)
{
    for (const lunar::InstancePtr& d : workspace.descendants()) {
        auto* spawn = dynamic_cast<lunar::SpawnLocation*>(d.get());
        if (!spawn || !spawn->enabled())
            continue;
        // Keep only the spawn's yaw, so the character stands upright.
        const Vector3 look = spawn->cframe().lookVector();
        const float yaw = std::atan2(-look.x, -look.z);
        const Vector3 top = spawn->cframe().pointToWorldSpace({0.0f, spawn->size().y * 0.5f, 0.0f});
        return lunar::CFrame(top) * lunar::CFrame::angles(0.0f, yaw, 0.0f);
    }
    return {0.0f, 100.0f, 0.0f};
}

namespace {

// Keyboard and mouse for UserInputService, ContextActionService and the legacy Mouse.
// Chat in Play Solo, with two commands for testing places: /server <code>
// runs Luau in the server's context, and /gogogo starts a round in places
// built on Roblox's battle royale template (what its admin-only GoGoGo
// console command does). Anything else is chat as usual.
class DevChatSource final : public lunar::ui::ChatSource {
public:
    DevChatSource(lunar::ui::ClassicChat& chat, lunar::PlaySession* scripts, const std::string& name)
        : chat_(chat), scripts_(scripts), echo_(name, name)
    {
        echo_.deliver = [this](const lunar::ui::ChatMessage& message) {
            if (deliver)
                deliver(message);
        };
    }

    void send(std::string_view text) override
    {
        if (scripts_ && text == "/gogogo") {
            runOnServer(goGoGoSource);
            return;
        }
        if (scripts_ && text.starts_with("/server ")) {
            runOnServer(text.substr(8));
            return;
        }
        echo_.send(text);
    }

private:
    void runOnServer(std::string_view code)
    {
        if (auto result = scripts_->server().run(code, "ChatCommand"); !result)
            chat_.postSystem(result.error().message, lunar::Color3::fromRGB(255, 80, 80));
        else
            chat_.postSystem("Done.");
    }

    lunar::ui::ClassicChat& chat_;
    lunar::PlaySession* scripts_;
    lunar::ui::LocalEchoChatSource echo_;
};

} // namespace

int playPlace(const std::filesystem::path& path, const PlayOptions& options)
{
    // LUNAR_PROFILE also times each step of loading in.
    const bool timeLoading = profileLevel() > 0;
    auto loadMark = std::chrono::steady_clock::now();
    auto loaded = [&](std::string_view step) {
        const auto now = std::chrono::steady_clock::now();
        if (timeLoading)
            std::println(stderr, "loading in: {} {:.0f} ms", step,
                         std::chrono::duration<double, std::milli>(now - loadMark).count());
        loadMark = now;
    };
    // --api listens before anything loads: a port that's taken stops it here.
    std::unique_ptr<ConsoleApi> api;
    if (options.api) {
        auto started = startConsoleApi(*options.api);
        if (!started) {
            std::println(stderr, "{}", started.error());
            return 1;
        }
        api = std::move(*started);
    }
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 1;
    }
    std::shared_ptr<lunar::DataModel> game = place->game;
    loaded("read+parse");

    // Studio's Play Solo: server Scripts and client LocalScripts in two contexts.
    // LUNAR_PROFILE=1 prints where each second's frames went, 2 also the busiest scripts.
    const int profiling = profileLevel();
    std::optional<lunar::PlaySession> scripts;
    if (options.runScripts) {
        scripts.emplace(game);
        scripts->setOutput([](lunar::ScriptRole role, lunar::MessageType type, std::string_view text) {
            if (type == lunar::MessageType::Info)
                return;
            std::println(stderr, "[{}] {}", role == lunar::ScriptRole::Server ? "server" : "client", text);
        });
        scripts->setProfiling(profiling > 0);
    }

    loaded("scripts");
    Session session(game, options, scripts ? &*scripts : nullptr);
    loaded("session");
    PlaceScene scene(*game);
    scene.setTerrainDistance(options.graphics.terrainDistance);
    loaded("scene");
    scene.wrapTextures(
        [&](lunar::render::TextureProvider next) { return session.clothing().provider(std::move(next)); });
    // Parts coming and going (characters, debris, fallen parts) are extracted
    // again, or dropped, one by one.
    auto touch = [&scene](lunar::Instance* instance) { scene.touch(*instance); };
    lunar::Connection added = game->workspace()->descendantAdded.connect(touch);
    lunar::Connection removing = game->workspace()->descendantRemoving.connect(touch);
    render::Lighting lighting = scene.lighting();
    double gameTime = 0.0;
    render::ParticleSystem particles(*game->workspace(), scene.textures());
    particles.setSettings({.density = options.graphics.particleDensity});
    lunar::audio::SoundSystem sounds(*game, std::make_shared<lunar::assets::AssetCache>(), !options.mute);

    loaded("lighting");
    GameWindow::Options windowOptions{.title = std::format("Lunarstone - {}", path.filename().string()),
                                      .graphics = options.graphics,
                                      .vsync = options.vsync,
                                      .fpsCap = options.fpsCap,
                                      .map = options.map,
                                      .shiftLock = options.shiftLock,
                                      .screenshots = options.screenshots,
                                      .seconds = options.seconds};
    // Play Solo is a local session: the map may show the other players.
    windowOptions.map.showOtherPlayers = true;
    auto gameWindow = GameWindow::create(*game, windowOptions);
    if (!gameWindow) {
        std::println(stderr, "{}", gameWindow.error());
        return 1;
    }
    Window* window = &(*gameWindow)->window();
    if (scripts) {
        (*gameWindow)->bindGui(scripts->client());
        (*gameWindow)->bindGui(scripts->server());
    }
    render::Presenter* presenter = &(*gameWindow)->presenter();
    lunar::ui::GameUi* ui = &(*gameWindow)->ui();
    // The menu's Luau tab: both sides' output (the server's marked), and Luau
    // run with full access on the client, or on the server where the code asks
    // for it with a first line of --!server.
    if (scripts) {
        lunar::ui::DevConsole* console = &ui->core().console();
        scripts->setOutput([console](lunar::ScriptRole role, lunar::MessageType type, std::string_view text) {
            const bool server = role == lunar::ScriptRole::Server;
            console->add(consoleKind(type), server ? std::format("[server] {}", text) : std::string(text));
            if (type != lunar::MessageType::Info)
                std::println(stderr, "[{}] {}", server ? "server" : "client", text);
        });
        console->run = [&scripts](const std::string& code) -> std::string {
            auto ran = scripts->client().runConsole(code, &scripts->server());
            return ran ? std::string() : ran.error().message;
        };
        // What the console can ask of the window: whether it has the focus, its
        // clipboard, and how many frames a second it draws at.
        scripts->client().setActive([window] { return window->focused(); });
        scripts->client().setClipboard([window](std::string_view text) { window->setClipboardText(std::string(text)); });
        scripts->client().setFrameCap([&gameWindow](int fps) { (*gameWindow)->setFrameCap(fps); });
    }
    // --api: code from the page runs in the same console (without scripts
    // there's nothing to run it in, and it says so).
    if (api)
        api->attach(&ui->core().console());
    const ConsoleApi::Detach detachApi(api.get());
    presenter->renderer().setMaterialTextures(scene.materialTextures());
    presenter->renderer().setTerrainMaterials(scene.terrainMaterials());
    ui->viewports()->setAssets(scene.viewportAssets());
    presenter->renderer().addEffects(&scene.ribbons());
    presenter->renderer().addEffects(&scene.adornments());
    presenter->renderer().setLights(&scene.lights());
    presenter->renderer().addEffects(&particles);
    // A graphics preset picked in the menu: what the renderer doesn't hold itself.
    (*gameWindow)->onGraphics = [&scene, &particles, terrainDistance = options.graphics.terrainDistance](
                                    const render::GraphicsSettings& graphics) mutable {
        if (graphics.terrainDistance != terrainDistance) // the terrain's levels of detail start over
            scene.setTerrainDistance(terrainDistance = graphics.terrainDistance);
        particles.setSettings({.density = graphics.particleDensity});
    };
    ui->core().setTitle(path.stem().string());
    DevChatSource chatSource(ui->chat(), scripts ? &*scripts : nullptr, options.user.name);
    // Lunarstone's map of the place, built in the background.
    MapScene mapScene(scene);
    ui->map().setTitle(path.stem().string());
    ui->setChatSource(&chatSource);
    std::println("WASD walks, Space jumps, right mouse looks, wheel zooms, / chats, Esc opens the menu, {} the map.",
                 options.map.fullMapKey ? lunar::platform::keyName(*options.map.fullMapKey) : "no key");
    if (scripts)
        std::println("Chat commands: /server <Luau> runs on the server, /gogogo starts a battle royale round.");

    loaded("window, Vulkan and UI");
    using Clock = std::chrono::steady_clock;
    lunar::Vector2 mouse;
    if (scripts && profiling > 1) {
        std::println(stderr, "starting the scripts:");
        printSimulationProfile(scripts->takeProfile(), profiling);
    }
    struct {
        float simulate = 0.0f, rebuild = 0.0f, follow = 0.0f, map = 0.0f, ui = 0.0f, draw = 0.0f;
        int rebuilds = 0;
    } profile;
    while (!window->shouldClose()) {
        const float dt = (*gameWindow)->beginFrame({}, [&](const Event& event, bool processed) {
            if (scripts)
                forwardInput(scripts->input(), event, mouse, window->pointerLockRequested() && window->pointerLocked(),
                             ui->menuOpen() || ui->mapOpen(), processed, ui->gui().takeInputDispatch());
        });
        // Code that came in over --api runs where typed code would.
        if (api)
            api->pump();

        // Esc opens the menu (its Leave button quits); typing keeps keys from the character.
        const InputState& input = window->input();
        const bool uiHasKeys = (*gameWindow)->uiHasKeys();
        // F9 prints where the character and camera are, as a --look for --render.
        if (input.wasPressed(Key::F9)) {
            const Vector3 eye = session.camera().cframe().position;
            const Vector3 at = eye + session.camera().cframe().lookVector() * 10.0f;
            const Vector3 feet = session.humanoid() && session.humanoid()->rootPart()
                                     ? session.humanoid()->rootPart()->position()
                                     : Vector3{};
            std::println("character at {:.1f},{:.1f},{:.1f}; --look {:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}", feet.x,
                         feet.y, feet.z, eye.x, eye.y, eye.z, at.x, at.y, at.z);
        }

        const float aspect = (*gameWindow)->aspect();
        session.camera().updateViewportSize((*gameWindow)->logicalSize());

        auto mark = Clock::now();
        auto lap = [&](float& into) {
            const auto t = Clock::now();
            into += std::chrono::duration<float, std::milli>(t - mark).count();
            mark = t;
        };
        session.setShiftLock((*gameWindow)->shiftLock());
        session.setCameraSensitivity((*gameWindow)->cameraSensitivity());
        session.frame(input, dt, aspect, window->pointerLocked(), uiHasKeys, ui->wheelCaptured());
        window->setPointerLocked(input.focused() && !ui->menuOpen() && !ui->mapOpen() && session.wantsPointerLock());
        lap(profile.simulate);

        const bool rebuilt = scene.refresh();
        if (rebuilt)
            lighting = scene.lighting(); // more of the sky may have loaded
        profile.rebuilds += rebuilt ? 1 : 0;
        lap(profile.rebuild);
        const Vector3 offset = session.renderOffset();
        scene.followParts(session.character(), offset);
        render::Camera view;
        view.cframe = session.camera().cframe() + session.cameraOffset();
        view.fieldOfView = session.camera().fieldOfView();
        scene.updateTerrain(view.cframe.position);
        sounds.setMasterVolume((*gameWindow)->volume());
        sounds.update(view.cframe);
        particles.step(std::min(dt, 0.1f), view.cframe.position);
        lap(profile.follow);
        mapScene.update(gameTime, ui->map());
        lap(profile.map);
        (*gameWindow)->updateUi(dt);
        lap(profile.ui);
        gameTime += static_cast<double>(dt);
        lighting.time = gameTime; // animates the water
        presenter->drawFrame(view, scene.items(), lighting);
        lap(profile.draw);

        if (const auto frames = (*gameWindow)->endFrame()) {
            window->setTitle(std::format("Lunarstone - {} - {} fps", path.filename().string(), *frames));
            if (profiling) {
                const float n = static_cast<float>(std::max(*frames, 1));
                std::println(stderr, "{} fps, ms: simulate {:.2f}, rebuild {:.2f} ({}x), follow {:.2f}", *frames,
                             profile.simulate / n, profile.rebuild / n, profile.rebuilds, profile.follow / n);
                std::println(stderr, "  map {:.2f}, ui {:.2f}, draw {:.2f}", profile.map / n, profile.ui / n,
                             profile.draw / n);
                if (scripts)
                    printSimulationProfile(scripts->takeProfile(), profiling);
            }
            profile = {};
        }
    }
    added.disconnect();
    removing.disconnect();
    if (window->error()) {
        std::println(stderr, "{}", *window->error());
        return 1;
    }
    return 0;
}

int playtestPlace(const std::filesystem::path& path, const PlaytestOptions& options)
{
    auto bytes = lunar::readFile(path);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 2;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", path.string(), place.error());
        return 2;
    }
    auto test = lunar::readFile(options.test);
    if (!test) {
        std::println(stderr, "{}", test.error());
        return 2;
    }
    const std::filesystem::path out = options.out.empty() ? "playtest" / options.test.stem() : options.out;
    std::error_code madeOut;
    std::filesystem::create_directories(out, madeOut);
    std::ofstream log(out / "log.txt");
    if (madeOut || !log) {
        std::println(stderr, "{}: can't write there", out.string());
        return 2;
    }
    std::shared_ptr<lunar::DataModel> game = place->game;
    // A seed of its own when none was given, so any run can be played again.
    const std::uint64_t seed = options.seed.value_or(std::random_device{}() & 0xFFFFFFFF);
    lunar::seedScripts(seed);
    // The game's clocks go by the simulation, which runs faster than real
    // time, from a date of its own so the seed repeats os.time() too.
    constexpr double epoch = 1'767'225'600.0; // 2026-01-01 00:00 UTC
    lunar::simulateClocks(epoch);

    // Everything said, on stdout and in the log, by simulated time and side.
    lunar::PlaySession scripts(game);
    auto say = [&](std::string_view side, std::string_view text) {
        const std::string line = std::format("{:7.2f} [{}] {}", scripts.time(), side, text);
        std::println("{}", line);
        log << line << '\n';
        log.flush();
    };
    int gameErrors = 0;
    scripts.setOutput([&](lunar::ScriptRole role, lunar::MessageType type, std::string_view text) {
        const bool server = role == lunar::ScriptRole::Server;
        const char* kind = type == lunar::MessageType::Error     ? " error"
                           : type == lunar::MessageType::Warning ? " warning"
                                                                 : "";
        if (type == lunar::MessageType::Error)
            ++gameErrors;
        say(std::format("{}{}", server ? "server" : "client", kind), text);
    });

    PlayOptions play;
    play.players = options.players;
    play.user = options.user;
    play.graphics = options.graphics;
    Session session(game, play, &scripts);

    // The picture: the scene, its effects and the GUI over it, as --play
    // draws them, at the size the test's pixels are in.
    auto context = render::Context::create({.rayTracing = options.graphics.rayTracing});
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 2;
    }
    auto offscreen = render::OffscreenRenderer::create(**context, options.width, options.height);
    if (!offscreen) {
        std::println(stderr, "Vulkan: {}", offscreen.error());
        return 2;
    }
    render::MeshRenderer& renderer = (*offscreen)->renderer();
    renderer.setGraphics(options.graphics);
    PlaceScene scene(*game);
    scene.setTerrainDistance(options.graphics.terrainDistance);
    scene.wrapTextures([&](render::TextureProvider next) { return session.clothing().provider(std::move(next)); });
    auto touch = [&scene](lunar::Instance* instance) { scene.touch(*instance); };
    lunar::Connection added = game->workspace()->descendantAdded.connect(touch);
    lunar::Connection removing = game->workspace()->descendantRemoving.connect(touch);
    render::ParticleSystem particles(*game->workspace(), scene.textures());
    particles.setSettings({.density = options.graphics.particleDensity});
    renderer.setMaterialTextures(scene.materialTextures());
    renderer.setTerrainMaterials(scene.terrainMaterials());
    renderer.addEffects(&scene.ribbons());
    renderer.addEffects(&scene.adornments());
    renderer.setLights(&scene.lights());
    renderer.addEffects(&particles);
    auto offscreenUi = OffscreenUi::create(**context, **offscreen, *game);
    if (!offscreenUi) {
        std::println(stderr, "Vulkan: {}", offscreenUi.error());
        return 2;
    }
    (*offscreenUi)->setWaitForImages(false); // only a picture waits for them
    (*offscreenUi)->bind(scripts.client());
    lunar::ui::GameUi& ui = (*offscreenUi)->ui();
    ui.viewports()->setAssets(scene.viewportAssets());

    // The test's keypress, mouse1press and mousemoveabs, as the window's own
    // input: the GUI first, then the scripts and the character.
    std::vector<Event> pending;
    InputState input;
    input.apply(FocusEvent{.focused = true});
    input.apply(MouseEnterEvent{.entered = true});
    scripts.client().setInput([&](const lunar::ScriptContext::HostInput& host) {
        using Kind = lunar::ScriptContext::HostInput::Kind;
        const Vec2d at = input.mousePosition();
        const auto dx = static_cast<double>(host.x); // MoveBy: motion; MoveTo: the point
        const auto dy = static_cast<double>(host.y);
        switch (host.kind) {
        case Kind::Key:
            if (const auto key = platformKey(host.key))
                pending.emplace_back(KeyEvent{.key = *key, .pressed = host.pressed});
            break;
        case Kind::Button:
            pending.emplace_back(MouseButtonEvent{.button = host.button == 2   ? MouseButton::Right
                                                            : host.button == 3 ? MouseButton::Middle
                                                                               : MouseButton::Left,
                                                  .pressed = host.pressed,
                                                  .x = at.x,
                                                  .y = at.y});
            break;
        case Kind::MoveBy:
            pending.emplace_back(RelativeMotionEvent{.dx = dx, .dy = dy, .rawDx = dx, .rawDy = dy});
            break;
        case Kind::MoveTo:
            pending.emplace_back(MouseMoveEvent{.x = dx, .y = dy});
            break;
        case Kind::Scroll:
            pending.emplace_back(ScrollEvent{.dy = dy});
            break;
        }
    });
    std::vector<std::string> shots;
    std::optional<std::pair<bool, std::string>> result;
    scripts.client().setPlaytest({
        .screenshot = [&](std::string_view name) { shots.emplace_back(name); },
        .finish = [&](bool passed, std::string_view message) { result.emplace(passed, message); },
    });

    scripts.start(play.user.name, play.user.userId, play.user.displayName);
    for (int i = 1; i < options.players; ++i)
        scripts.addPlayer(std::format("Player{}", i + 1), -1 - i);
    const lunar::Vector2 size{static_cast<float>(options.width), static_cast<float>(options.height)};
    const float aspect = size.x / size.y;
    session.camera().updateViewportSize(size);
    render::Lighting lighting = scene.lighting();
    lunar::Vector2 mouse;
    bool pointerLocked = false;
    constexpr double dt = 1.0 / 60.0;
    auto frame = [&] {
        input.beginFrame();
        for (const Event& event : std::exchange(pending, {})) {
            input.apply(event);
            const bool privateTyping = ui.chat().focused() && ui.chat().selectedTab() == lunar::ui::ClassicChat::Tab::Rtc;
            const bool used = ui.handle(event);
            const auto* key = std::get_if<KeyEvent>(&event);
            if (!(privateTyping && key && key->pressed))
                forwardInput(scripts.input(), event, mouse, pointerLocked, ui.menuOpen() || ui.mapOpen(), used || ui.keyboardCaptured(), ui.gui().takeInputDispatch());
        }
        const bool uiHasKeys = ui.keyboardCaptured() || ui.menuOpen() || ui.mapOpen();
        session.frame(input, static_cast<float>(dt), aspect, pointerLocked, uiHasKeys, ui.wheelCaptured());
        pointerLocked = session.wantsPointerLock();
        particles.step(static_cast<float>(dt), session.camera().cframe().position);
        (*offscreenUi)->update(dt);
    };
    auto picture = [&](const std::string& name) {
        if (scene.refresh())
            lighting = scene.lighting();
        scene.followParts(session.character(), session.renderOffset());
        render::Camera view;
        view.cframe = session.camera().cframe() + session.cameraOffset();
        view.fieldOfView = session.camera().fieldOfView();
        scene.settleTerrain(view.cframe.position);
        scene.waitForMeshes();
        lighting.time = scripts.time();
        (*offscreenUi)->setWaitForImages(true);
        (*offscreenUi)->update();
        (*offscreenUi)->setWaitForImages(false);
        render::PixelImage image = (*offscreen)->render(view, scene.items(), lighting);
        for (int i = 0; i < 1000 && renderer.stats().texturesPending > 0; ++i) {
            (*offscreenUi)->update();
            image = (*offscreen)->render(view, scene.items(), lighting);
        }
        const std::filesystem::path file = out / std::format("{}.png", name);
        const std::string encoded = render::encodePng(image);
        std::ofstream png(file, std::ios::binary);
        png.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        say("test", png ? std::format("screenshot {}", file.string()) : std::format("can't write {}", file.string()));
    };

    // A second for the game's scripts to settle, as --console-run gives them.
    while (scripts.time() < 1.0)
        frame();
    say("test",
        std::format("running {} on {}, seed {}", options.test.filename().string(), path.filename().string(), seed));
    const std::string chunk = options.test.filename().string();
    if (auto ran = scripts.client().runConsole(playtestSource(*test, chunk), &scripts.server()); !ran)
        result.emplace(false, ran.error().message);
    const double started = scripts.time();
    while (!result && scripts.time() - started < options.timeout) {
        frame();
        for (const std::string& name : std::exchange(shots, {}))
            picture(name);
    }
    for (const std::string& name : std::exchange(shots, {}))
        picture(name);
    added.disconnect();
    removing.disconnect();

    if (!result)
        result.emplace(false, std::format("timed out after {} s", options.timeout));
    const bool errorsFail = gameErrors > 0 && !options.allowErrors;
    const bool passed = result->first && !errorsFail;
    std::string verdict = std::format("{} {}", passed ? "PASS" : "FAIL", chunk);
    if (!result->second.empty())
        verdict += std::format(": {}", result->second);
    if (gameErrors > 0)
        verdict += std::format(" ({} error(s) from the game's scripts{})", gameErrors,
                               options.allowErrors ? ", allowed" : "");
    say("test", verdict);
    if (!passed)
        say("test", std::format("again the same way: --seed {}", seed));
    return passed ? 0 : 1;
}

int scriptProfileLevel()
{
    return profileLevel();
}

void printScriptProfile(const lunar::SimulationProfile& profile, int level)
{
    printSimulationProfile(profile, level);
}
