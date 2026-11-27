#pragma once

// The default "Animate" LocalScript every Roblox character gets, in C++: it
// picks the animation for what the Humanoid is doing and keeps the walk/run
// blend in step with the speed.
//
// Source: the client's own copies of the script, avatar/scripts/
// humanoidAnimateR15_DEPRECATED_20260813.rbxm for R15 (the newest R15
// version in the 0.742 content folder; the binary also names
// avatar/scripts/humanoidAnimateR15.lua, which isn't shipped) and
// avatar/scripts/humanoidAnimatePlayEmote.rbxm for R6. Functions keep the
// script's names so the two can be read side by side.

#include <lunar/Classes.hpp>
#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/ClipLoader.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::avatar {

// Enum.HumanoidStateType, with Roblox's values (from the binary's enum
// table), until the engine has it. static_cast between the two.
// NOLINTNEXTLINE(performance-enum-size): Roblox enums are 32 bits
enum class HumanoidState : std::uint32_t {
    FallingDown = 0,
    Ragdoll = 1,
    GettingUp = 2,
    Jumping = 3,
    Swimming = 4,
    Freefall = 5,
    Flying = 6,
    Landed = 7,
    Running = 8,
    RunningNoPhysics = 10,
    StrafingNoPhysics = 11,
    Climbing = 12,
    Seated = 13,
    PlatformStanding = 14,
    Dead = 15,
    Physics = 16,
    None = 18,
};

// One choice of an animation set ("idle" has two, picked by weight).
struct AnimationChoice {
    std::string animationId;
    double weight = 1.0;
};

// The script's built-in animNames table for the rig type, used for sets the
// character's Animate object doesn't configure.
const std::map<std::string, std::vector<AnimationChoice>, std::less<>>& defaultAnimations(Enum::HumanoidRigType rig);

class AnimateController {
public:
    // The script's `pose` variable.
    enum class Pose : std::uint8_t {
        Standing,
        Running,
        Jumping,
        FreeFall,
        Climbing,
        Seated,
        Dead,
        GettingUp,
        FallingDown,
        PlatformStanding,
        Swimming,
    };

    // Reads animation sets from the character's "Animate" child (StringValues
    // named after a set, holding Animations with optional Weight
    // NumberValues), like configureAnimationSet; missing sets use
    // defaultAnimations. Starts idle, as the script does when its character
    // is parented.
    AnimateController(Model& character, Animator& animator, ClipLoader& clips, std::uint32_t seed = 0);
    // Stops and releases its tracks (they fade out), so a script taking
    // over the character's animation starts from a clean Animator.
    ~AnimateController();
    AnimateController(const AnimateController&) = delete;
    AnimateController& operator=(const AnimateController&) = delete;

    // Humanoid events, connected exactly as the script connects them.
    void onRunning(float speed);
    void onDied();
    void onJumping();
    void onClimbing(float speed);
    void onGettingUp();
    void onFreeFall();
    void onFallingDown();
    void onSeated();
    void onPlatformStanding();
    void onSwimming(float speed);

    // For a controller that reports state changes rather than the events
    // above: fires the event Roblox's Humanoid fires on entering the state
    // (Running/Swimming/Climbing get the speed).
    void onStateChanged(HumanoidState state, float speed = 0.0f);

    // The PlayEmote BindableFunction for a default emote name ("wave",
    // "dance"...). Only plays while standing; false if it didn't.
    bool playEmote(std::string_view emote);

    // Advances the script's clock. Every 0.1 s it runs stepAnimate, like the
    // script's `while wait(0.1)` loop (timed transitions and tools).
    void update(float dt);

    // Re-reads the Animate object's sets (it changed).
    void reconfigure();

    Pose pose() const { return pose_; }
    const std::string& currentAnimation() const { return currentAnim_; }
    const TrackPtr& currentTrack() const { return currentAnimTrack_; }
    const TrackPtr& runTrack() const { return runAnimTrack_; }
    const TrackPtr& toolTrack() const { return toolAnimTrack_; }

private:
    struct Choice {
        std::string animationId;
        double weight = 1.0;
        std::string name; // the Animation instance's name
        std::string key;  // stands for the Animation instance: set name and index
    };
    struct Set {
        std::vector<Choice> choices;
        double totalWeight = 0.0;
    };

    void configureAnimationSet(const std::string& name, const std::vector<AnimationChoice>& fallback);
    std::string stopAllAnimations();
    float getHeightScale() const;
    float rootMotionCompensation(float speed) const;
    void setRunSpeed(float speed);
    void setAnimationSpeed(float speed);
    void keyFrameReachedFunc(const std::string& frameName);
    std::size_t rollAnimation(const std::string& animName);
    void switchToAnim(const Choice& anim, const std::string& animName, float transitionTime);
    void playAnimation(const std::string& animName, float transitionTime);
    void playToolAnimation(const std::string& animName, float transitionTime,
                           std::optional<Enum::AnimationPriority> priority);
    void stopToolAnimations();
    void animateTool();
    void stepAnimate(double currentTime);
    TrackPtr load(const Choice& anim);
    Humanoid* humanoid() const;

    Model* character_;
    Animator* animator_;
    ClipLoader* clips_;
    Enum::HumanoidRigType rig_ = Enum::HumanoidRigType::R15;
    std::mt19937 random_;
    std::map<std::string, Set, std::less<>> animTable_;

    Pose pose_ = Pose::Standing;
    std::string currentAnim_;
    std::string currentAnimInstanceId_;
    TrackPtr currentAnimTrack_;
    Connection currentAnimKeyframeHandler_;
    float currentAnimSpeed_ = 1.0f;
    TrackPtr runAnimTrack_;
    Connection runAnimKeyframeHandler_;
    bool currentlyPlayingEmote_ = false;

    std::string toolAnim_ = "None";
    double toolAnimTime_ = 0.0;
    double jumpAnimTime_ = 0.0;
    std::string toolAnimName_;
    TrackPtr toolAnimTrack_;
    std::string toolAnimInstanceId_;
    Connection currentToolAnimKeyframeHandler_;

    double clock_ = 0.0;    // game time the script sees
    double nextStep_ = 0.1; // when the wait(0.1) loop runs next
    double lastTick_ = 0.0;
};

} // namespace lunar::avatar
