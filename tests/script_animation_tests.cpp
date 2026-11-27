#include "script_harness.hpp"

// Scripts' animations and Humanoid state: LoadAnimation's tracks playing on
// the native Animator (fades, priorities, events, loading), and the
// Humanoid's events and state methods against the physics controller.

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/Clip.hpp>
#include <lunar/avatar/Quaternion.hpp>
#include <lunar/physics/Character.hpp>
#include <lunar/physics/HumanoidConstants.hpp>
#include <lunar/physics/HumanoidController.hpp>
#include <lunar/physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;

namespace {

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

bool has(const std::vector<std::string>& lines, std::string_view line)
{
    return std::ranges::find(lines, line) != lines.end();
}

// A clip holding the waist at `angle` about X, with the given events.
avatar::ClipPtr holdClip(float angle, float length = 1.0f, bool loop = true,
                         Enum::AnimationPriority priority = Enum::AnimationPriority::Core,
                         std::vector<avatar::ClipEvent> events = {})
{
    auto clip = std::make_shared<avatar::AnimationClipData>();
    clip->length = length;
    clip->loop = loop;
    clip->priority = priority;
    avatar::JointCurve curve{"UpperTorso", "LowerTorso", {}};
    curve.keys.push_back({0.0f, {}, {angle, 0.0f, 0.0f}});
    clip->joints.push_back(std::move(curve));
    clip->events = std::move(events);
    return clip;
}

// A script context over a game holding a rig ("Rig": LowerTorso and
// UpperTorso joined by a Waist Motor6D, a Humanoid with an Animator) and an
// animation system whose clips are inserted by hand.
struct AnimationFixture : Harness {
    avatar::AnimationSystem system{[] {
        assets::AssetConfig config;
        config.download = false;
        return std::make_shared<assets::AssetCache>(config);
    }()};
    std::shared_ptr<Model> rig = std::make_shared<Model>();
    std::shared_ptr<Motor6D> waist = std::make_shared<Motor6D>();

    AnimationFixture()
    {
        context.setAnimationSystem(&system);
        rig->setName("Rig");
        auto lower = std::make_shared<Part>();
        lower->setName("LowerTorso");
        lower->setAnchored(true);
        lower->setParent(rig.get());
        auto upper = std::make_shared<Part>();
        upper->setName("UpperTorso");
        upper->setParent(rig.get());
        waist->setName("Waist");
        waist->setPart0(lower.get());
        waist->setPart1(upper.get());
        waist->setParent(upper.get());
        rig->setPrimaryPart(lower.get());
        rig->setParent(game->workspace());
    }

    ~AnimationFixture() { context.setAnimationSystem(nullptr); }
    AnimationFixture(const AnimationFixture&) = delete;
    AnimationFixture& operator=(const AnimationFixture&) = delete;

    // A Humanoid (with an Animator) or an AnimationController on the rig.
    void humanoid()
    {
        auto h = std::make_shared<Humanoid>();
        h->setParent(rig.get());
        std::make_shared<Animator>()->setParent(h.get());
    }

    // Animation steps before the scripts' frame, as in the client.
    void frames(int count, float dt = 1.0f / 60.0f)
    {
        for (int i = 0; i < count; ++i) {
            system.step(dt);
            context.step(dt);
        }
    }

    float waistAngle() const { return avatar::toRotationVector(waist->transform()).x; }
};

} // namespace

TEST(script_load_animation_plays_on_the_rig)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1001", holdClip(0.5f));
    f.run(R"(
        local animator = workspace.Rig.Humanoid.Animator
        local animation = Instance.new("Animation")
        animation.Name = "Lean"
        animation.AnimationId = "rbxassetid://1001"
        _G.played = {}
        animator.AnimationPlayed:Connect(function(t) table.insert(_G.played, t) end)
        local track = animator:LoadAnimation(animation)
        _G.track = track
        print(track.ClassName, track.Name, track.Animation == animation, track.Length, track.IsPlaying)
        track:Play(0)
        print(track.IsPlaying, #_G.played, rawequal(_G.played[1], track), track.WeightCurrent, track.WeightTarget)
    )");
    CHECK_EQ(f.lines.at(0), "AnimationTrack Lean true 1 false");
    // WeightCurrent follows the target at the next step, even with no fade.
    CHECK_EQ(f.lines.at(1), "true 1 true 0 1");
    f.frames(2);
    CHECK(near(f.waistAngle(), 0.5f));
    f.lines.clear();
    f.run(R"(
        local playing = workspace.Rig.Humanoid.Animator:GetPlayingAnimationTracks()
        local viaHumanoid = workspace.Rig.Humanoid:GetPlayingAnimationTracks()
        print(#playing, rawequal(playing[1], _G.track), rawequal(viaHumanoid[1], _G.track))
    )");
    CHECK_EQ(f.lines.at(0), "1 true true");
}

// A track no script loaded (another player's, replicated, or the native
// Animate's) still has an Animation with its id: the game stops every playing
// track whose track.Animation.AnimationId isn't on its list.
TEST(script_playing_tracks_nobody_loaded_have_an_animation)
{
    AnimationFixture f;
    f.humanoid();
    avatar::CharacterAnimation& entry = f.system.add(f.rig, false);
    avatar::TrackPtr native = entry.animator->loadAnimation(holdClip(0.5f), "Run");
    native->setAnimationId("rbxassetid://1009");
    native->play(0);
    f.frames(1);
    auto printed = f.output(R"(
        local track = workspace.Rig.Humanoid.Animator:GetPlayingAnimationTracks()[1]
        print(track.Name, track.Animation.ClassName, track.Animation.Name, track.Animation.AnimationId)
        local again = workspace.Rig.Humanoid.Animator:GetPlayingAnimationTracks()[1]
        print(rawequal(track.Animation, again.Animation))
    )");
    CHECK_EQ(printed.at(0), "Run Animation Run rbxassetid://1009");
    CHECK_EQ(printed.at(1), "true");
}

// A track keeps the Animation it was loaded from, even one the script
// made with Instance.new and let go: the game's CharacterHandler reads
// track.Animation.AnimationId on every playing track, and a nil one broke
// its loop that stops the others.
TEST(script_track_keeps_its_animation)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1010", holdClip(0.5f));
    f.run(R"(
        local animation = Instance.new("Animation")
        animation.AnimationId = "rbxassetid://1010"
        _G.track = workspace.Rig.Humanoid.Animator:LoadAnimation(animation)
        _G.track:Play(0)
    )");
    auto printed = f.output(R"(
        -- Enough garbage for the collector to finish a few cycles.
        for _ = 1, 2000000 do local _ = {1, 2, 3} end
        local playing = workspace.Rig.Humanoid.Animator:GetPlayingAnimationTracks()[1]
        print(rawequal(playing, _G.track), playing.Animation ~= nil and playing.Animation.AnimationId)
    )");
    CHECK_EQ(printed.at(0), "true rbxassetid://1010");
}

TEST(script_animation_track_properties)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1002", holdClip(0.5f, 2.0f, false, Enum::AnimationPriority::Movement));
    auto printed = f.output(R"(
        local animation = Instance.new("Animation")
        animation.AnimationId = "rbxassetid://1002"
        local track = workspace.Rig.Humanoid:LoadAnimation(animation)
        print(track.Looped, track.Priority, track.Speed)
        track.Looped = true
        track.Priority = Enum.AnimationPriority.Action2
        print(track.Looped, track.Priority)
        track:Play(0.5, 0.8, 2)
        print(track.Speed, track.WeightCurrent, track.WeightTarget)
        track:AdjustSpeed(0.5)
        track:AdjustWeight(0.25, 0)
        track.TimePosition = 1.5
        print(track.Speed, track.WeightCurrent, track.TimePosition)
        track.TimePosition = 5
        print(track.TimePosition)
    )");
    CHECK_EQ(printed.at(0), "false Enum.AnimationPriority.Movement 1");
    CHECK_EQ(printed.at(1), "true Enum.AnimationPriority.Action2");
    CHECK_EQ(printed.at(2), "2 0 0.800000011920929");
    CHECK_EQ(printed.at(3), "0.5 0 1.5"); // the weight moves at the next step
    CHECK_EQ(printed.at(4), "2");
}

TEST(script_animation_length_is_zero_until_the_clip_loads)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1003", nullptr); // still downloading
    f.run(R"(
        local animation = Instance.new("Animation")
        animation.AnimationId = "rbxassetid://1003"
        _G.track = workspace.Rig.Humanoid.Animator:LoadAnimation(animation)
        _G.track.Priority = Enum.AnimationPriority.Core
        print(_G.track.Length, _G.track.Looped)
        _G.track:Play(0)
    )");
    CHECK_EQ(f.lines.at(0), "0 false");
    f.frames(1);
    CHECK(near(f.waistAngle(), 0.0f));
    f.system.clips().insert("rbxassetid://1003", holdClip(0.25f, 3.0f, true, Enum::AnimationPriority::Action));
    f.frames(1);
    CHECK(near(f.waistAngle(), 0.25f));
    // The clip's Loop arrives with it; the Priority the script set stays.
    CHECK_EQ(f.print("print(_G.track.Length, _G.track.Looped, _G.track.Priority)"),
             "3 true Enum.AnimationPriority.Core");
}

TEST(script_animation_events_in_order)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1004", holdClip(0.5f, 1.0f, true, Enum::AnimationPriority::Core,
                                                          {{0.25f, "Step", "left", true}, {0.25f, "Mid", {}, false}}));
    f.run(R"(
        local animation = Instance.new("Animation")
        animation.AnimationId = "rbxassetid://1004"
        local track = workspace.Rig.Humanoid.Animator:LoadAnimation(animation)
        _G.track = track
        track.KeyframeReached:Connect(function(name) print("keyframe", name) end)
        track:GetMarkerReachedSignal("Step"):Connect(function(value) print("marker", value) end)
        track.DidLoop:Connect(function() print("looped") end)
        track.Stopped:Connect(function() print("stopped", track.IsPlaying) end)
        track.Ended:Connect(function() print("ended") end)
        print("time of Mid", track:GetTimeOfKeyframe("Mid"))
        track:Play(0)
    )");
    f.frames(30); // 0.5 s: past the events
    f.frames(33); // past the end: loops
    f.run("_G.track:Stop(0.1) print('after stop')");
    f.frames(12);
    const std::vector<std::string> expected{
        "time of Mid 0.25", "marker left", "keyframe Mid", "looped", "stopped false", "after stop", "ended",
    };
    CHECK(f.lines == expected);
}

TEST(script_animation_priorities_blend_like_the_client)
{
    AnimationFixture f;
    f.humanoid();
    f.system.clips().insert("rbxassetid://1005", holdClip(0.4f, 1.0f, true, Enum::AnimationPriority::Core));
    f.system.clips().insert("rbxassetid://1006", holdClip(-0.6f, 1.0f, true, Enum::AnimationPriority::Action));
    f.run(R"(
        local function load(id)
            local animation = Instance.new("Animation")
            animation.AnimationId = id
            return workspace.Rig.Humanoid:LoadAnimation(animation)
        end
        _G.core = load("rbxassetid://1005")
        _G.action = load("rbxassetid://1006")
        _G.core:Play(0)
        _G.action:Play(0)
    )");
    f.frames(1);
    // The Action layer takes all of the weight.
    CHECK(near(f.waistAngle(), -0.6f));
    // At half weight it leaves the other half to Core.
    f.run("_G.action:AdjustWeight(0.5, 0)");
    f.frames(1);
    CHECK(near(f.waistAngle(), (-0.6f + 0.4f) * 0.5f, 2e-3f));
    // Fading out over 0.2 s: halfway through, a quarter of the weight.
    f.run("_G.action:Stop(0.2)");
    f.frames(6);
    CHECK(near(f.waistAngle(), -0.6f * 0.25f + 0.4f * 0.75f, 2e-2f));
    f.frames(12);
    CHECK(near(f.waistAngle(), 0.4f));
    CHECK_EQ(f.print("print(#workspace.Rig.Humanoid:GetPlayingAnimationTracks(), _G.action.IsPlaying)"), "1 false");
}

TEST(script_animation_controller_rigs_animate)
{
    AnimationFixture f;
    std::make_shared<AnimationController>()->setParent(f.rig.get());
    f.system.clips().insert("rbxassetid://1007", holdClip(0.3f));
    f.run(R"(
        local controller = workspace.Rig.AnimationController
        local animation = Instance.new("Animation")
        animation.AnimationId = "rbxassetid://1007"
        local track = controller:LoadAnimation(animation)
        track:Play(0)
        print(controller:FindFirstChildOfClass("Animator") ~= nil, #controller:GetPlayingAnimationTracks())
    )");
    CHECK_EQ(f.lines.at(0), "true 1");
    f.frames(1);
    CHECK(near(f.waistAngle(), 0.3f));
}

TEST(script_load_animation_errors)
{
    AnimationFixture f;
    f.humanoid();
    CHECK(f.error("workspace.Rig.Humanoid.Animator:LoadAnimation(nil)")
              .ends_with("LoadAnimation requires an Animation object"));
    CHECK(f.error("workspace.Rig.Humanoid:LoadAnimation(Instance.new('Animation'))")
              .ends_with("LoadAnimation requires the asset id to not be empty"));
}

// --- Humanoid ------------------------------------------------------------------------------

namespace {

// A placeholder character on a baseplate, driven by the physics controller,
// with a script context over the same game.
struct HumanoidFixture : Harness {
    std::shared_ptr<Model> character = physics::buildPlaceholderCharacter("Dummy");
    Humanoid& humanoid = *static_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid"));
    std::unique_ptr<physics::World> world;
    std::unique_ptr<physics::HumanoidController> controller;

    HumanoidFixture()
    {
        Workspace& ws = *game->workspace();
        auto plate = std::make_shared<Part>();
        plate->setName("Baseplate");
        plate->setSize({512.0f, 20.0f, 512.0f});
        plate->setPosition({0.0f, -10.0f, 0.0f});
        plate->setAnchored(true);
        plate->setParent(&ws);
        character->setParent(&ws);
        world = std::make_unique<physics::World>(ws);
        controller = std::make_unique<physics::HumanoidController>(*world, humanoid);
        world->onStep = [this](float dt) { controller->step(dt); };
    }

    void physicsFor(float seconds) const
    {
        const int steps = static_cast<int>(std::lround(seconds * physics::stepRate));
        for (int i = 0; i < steps; ++i)
            world->step();
    }
};

} // namespace

TEST(script_humanoid_events_follow_the_controller)
{
    HumanoidFixture f;
    f.run(R"(
        local humanoid = workspace.Dummy.Humanoid
        humanoid.StateChanged:Connect(function(old, new) print("state", old.Name, new.Name) end)
        humanoid.Running:Connect(function(speed) print("running", speed > 1 and "moving" or "still") end)
        humanoid.Jumping:Connect(function(active) print("jumping", active) end)
        humanoid.FreeFalling:Connect(function(active) print("freefalling", active) end)
        humanoid.HealthChanged:Connect(function(health) print("health", health) end)
        humanoid.Died:Connect(function() print("died") end)
    )");
    f.physicsFor(1.0f);
    CHECK_EQ(f.lines.at(0), "running still");
    CHECK_EQ(f.lines.at(1), "state None Running");
    f.lines.clear();
    f.run("print(workspace.Dummy.Humanoid:GetState().Name)");
    CHECK_EQ(f.lines.at(0), "Running");
    // A jump: the state machine's own events, in its order.
    f.lines.clear();
    f.humanoid.setJump(true);
    f.physicsFor(0.1f);
    f.humanoid.setJump(false);
    f.physicsFor(1.5f);
    CHECK(f.lines.size() >= 4);
    CHECK_EQ(f.lines.at(0), "running still");
    CHECK_EQ(f.lines.at(1), "jumping true");
    CHECK_EQ(f.lines.at(2), "state Running Jumping");
    CHECK(has(f.lines, "freefalling true"));
    f.lines.clear();
    f.run("workspace.Dummy.Humanoid.Health = 0");
    f.physicsFor(0.1f);
    CHECK_EQ(f.lines.at(0), "health 0");
    CHECK(std::ranges::count(f.lines, std::string_view("died")) == 1);
    CHECK(has(f.lines, "state Running Dead") || has(f.lines, "state Landed Dead"));
}

TEST(script_humanoid_state_methods_reach_the_controller)
{
    HumanoidFixture f;
    f.physicsFor(0.5f);
    f.run(R"(
        local humanoid = workspace.Dummy.Humanoid
        humanoid.StateEnabledChanged:Connect(function(state, enabled) print("enabled", state.Name, enabled) end)
        humanoid:SetStateEnabled(Enum.HumanoidStateType.Jumping, false)
        print(humanoid:GetStateEnabled(Enum.HumanoidStateType.Jumping), humanoid:GetStateEnabled("Running"))
        humanoid:ChangeState(Enum.HumanoidStateType.Jumping)
        print(humanoid:GetState().Name)
        humanoid:SetStateEnabled(Enum.HumanoidStateType.Jumping, true)
        humanoid:ChangeState(Enum.HumanoidStateType.Jumping)
        print(humanoid:GetState().Name)
    )");
    const std::vector<std::string> expected{
        "enabled Jumping false", "false true", "Running", "enabled Jumping true", "Jumping",
    };
    CHECK(f.lines == expected);
    CHECK(f.humanoid.stateEnabled(Enum::HumanoidStateType::Jumping));
}

TEST(script_humanoid_move_to_finishes)
{
    HumanoidFixture f;
    f.physicsFor(0.5f);
    f.run(R"(
        local humanoid = workspace.Dummy.Humanoid
        humanoid.MoveToFinished:Connect(function(reached) print("finished", reached) end)
        local root = workspace.Dummy.HumanoidRootPart
        humanoid:MoveTo(root.Position + Vector3.new(6, 0, 0))
    )");
    f.physicsFor(2.0f);
    CHECK_EQ(f.lines.at(0), "finished true");
}

TEST(script_humanoid_without_a_driver)
{
    Harness h;
    h.run(R"(
        local model = Instance.new("Model", workspace)
        local humanoid = Instance.new("Humanoid", model)
        humanoid.HealthChanged:Connect(function(health) print("health", health) end)
        humanoid.Died:Connect(function() print("died") end)
        print(humanoid:GetState().Name, humanoid:GetStateEnabled(Enum.HumanoidStateType.Climbing))
        humanoid:SetStateEnabled(Enum.HumanoidStateType.Climbing, false)
        humanoid:ChangeState(Enum.HumanoidStateType.Jumping)
        print(humanoid:GetState().Name, humanoid:GetStateEnabled(Enum.HumanoidStateType.Climbing))
        humanoid:TakeDamage(100)
        print(humanoid:GetState().Name)
    )");
    const std::vector<std::string> expected{"Running true", "Running false", "health 0", "died", "Dead"};
    CHECK(h.lines == expected);
}

TEST(script_humanoid_emote_plays_description_asset_and_stops_on_movement)
{
    AnimationFixture f;
    f.humanoid();
    auto* humanoid = instanceCast<Humanoid>(f.rig->findFirstChildOfClass("Humanoid"));
    auto description = std::make_shared<HumanoidDescription>();
    description->addEmote("Wave", 55);
    description->setParent(humanoid);
    f.system.clips().insert("rbxassetid://55", holdClip(0.8f));
    f.run(R"(
        local humanoid = workspace.Rig.Humanoid
        humanoid.AnimationPlayed:Connect(function(track) _G.emote = track end)
        assert(humanoid:PlayEmoteAsync("Wave"))
        assert(_G.emote.Animation.AnimationId == "rbxassetid://55")
        assert(_G.emote.Priority == Enum.AnimationPriority.Action)
        print("emote playing")
    )");
    f.frames(10);
    CHECK(has(f.lines, "emote playing"));
    CHECK(f.waistAngle() > 0.7f);
    humanoid->updateMovement({1, 0, 0}, Enum::Material::Plastic);
    f.frames(10);
    f.run(R"(print("after movement", _G.emote.IsPlaying))");
    CHECK(has(f.lines, "after movement false"));
    CHECK(std::abs(f.waistAngle()) < 0.01f);
    CHECK(f.error(R"(workspace.Rig.Humanoid:PlayEmote("Missing"))").find("No emote named") != std::string::npos);
}

TEST(script_chat_emote_selects_owned_names_and_equipped_slots)
{
    AnimationFixture f;
    f.humanoid();
    auto* humanoid = instanceCast<Humanoid>(f.rig->findFirstChildOfClass("Humanoid"));
    humanoid->setRigType(Enum::HumanoidRigType::R15);
    auto player = std::make_shared<Player>();
    player->setCharacter(f.rig.get());
    player->setParent(f.game->getService<Players>());
    f.game->getService<Players>()->assignLocalPlayer(player.get());
    auto description = std::make_shared<HumanoidDescription>();
    description->addEmote("Hello", 55);
    description->setEquippedEmotes({{3, "Hello"}});
    description->setParent(humanoid);
    f.system.clips().insert("rbxassetid://55", holdClip(0.8f));
    bool called = false, success = false;
    const auto done = [&](auto result) {
        called = true;
        success = result.has_value();
    };
    f.context.playChatEmote("hello", done);
    CHECK(called && success);
    f.frames(10);
    CHECK(f.waistAngle() > 0.7f);
    f.context.playChatEmote("3", done);
    CHECK(success);
    f.context.playChatEmote("8", done);
    CHECK(!success);
    f.game->getService<StarterPlayer>()->setUserEmotesEnabled(false);
    f.context.playChatEmote("Hello", done);
    CHECK(!success);
}

TEST(script_chat_legacy_emote_invokes_the_characters_own_handler)
{
    AnimationFixture f;
    f.humanoid();
    auto player = std::make_shared<Player>();
    player->setCharacter(f.rig.get());
    player->setParent(f.game->getService<Players>());
    f.game->getService<Players>()->assignLocalPlayer(player.get());
    f.run(R"(
        local animate = Instance.new("LocalScript", workspace.Rig)
        animate.Name = "Animate"
        local play = Instance.new("BindableFunction", animate)
        play.Name = "PlayEmote"
        play.OnInvoke = function(name)
            task.wait()
            print("legacy", name)
            return true
        end
    )");
    bool finished = false;
    f.context.playChatEmote("wave", [&](auto result) { finished = result.has_value(); });
    CHECK(!finished);
    f.frames(3);
    CHECK(finished);
    CHECK(has(f.lines, "legacy wave"));
}
