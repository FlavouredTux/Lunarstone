#pragma once

// What Roblox's Animator and AnimationTrack do: play clips on a rig, fade
// and weigh them, and write the blended result into the joints' Transform:
// Motor6D.Transform, or AnimationConstraint.Transform on rigs whose joints
// are constraints (the avatar joint upgrade).

#include <lunar/Classes.hpp>
#include <lunar/Signal.hpp>
#include <lunar/avatar/Clip.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::avatar {

class Animator;

// A clip as it plays on one Animator (Roblox's AnimationTrack). Made by
// Animator::loadAnimation; does nothing until play().
//
// Source: RobloxPlayer 0.742. Argument defaults from the method
// registration (0x100141505); Play 0x103411ea0, Stop 0x103410bbc,
// AdjustWeight 0x103412788, AdjustSpeed 0x103412ae4, the per-step update
// 0x103410fbc (fade, time, wrap, events), the keyframe scan 0x10348bdf4 and
// the Animator's removal check 0x10341435e.
class AnimationTrack {
public:
    // Supplies the clip once it has loaded (an AnimationId still downloading
    // gives null for a while); the track has Length 0 until then.
    using ClipSource = std::function<ClipPtr()>;

    AnimationTrack(Animator& animator, ClipSource clip, std::string name);

    // AnimationTrack:Play(fadeTime, weight, speed). Always starts over: time
    // goes to 0 (to Length when speed is negative), even when already
    // playing. The weight fades from wherever it is; with no fade it jumps
    // at the next step.
    void play(float fadeTime = 0.1f, float weight = 1.0f, float speed = 1.0f);
    // AnimationTrack:Stop(fadeTime). IsPlaying turns false and Stopped fires
    // at once; the track keeps contributing while its weight fades to 0,
    // then leaves the Animator and fires Ended. Stopping a track that is
    // already fading out starts its fade again with the new time.
    void stop(float fadeTime = 0.1f);
    // AnimationTrack:AdjustWeight(weight, fadeTime); ignored unless playing.
    void adjustWeight(float weight = 1.0f, float fadeTime = 0.1f);
    // AnimationTrack:AdjustSpeed(speed).
    void adjustSpeed(float speed = 1.0f);
    // AnimationTrack:Destroy(): the track is done with, but one fading out
    // still finishes its fade (the Animate script stops and destroys tracks
    // in one go and still blends smoothly).
    void destroy();

    const std::string& name() const { return name_; }
    // The Animation's AnimationId it was loaded from, if it was given one:
    // what the owner's Animator names it by on the network.
    const std::string& animationId() const { return animationId_; }
    void setAnimationId(std::string id) { animationId_ = std::move(id); }
    bool isPlaying() const { return playing_; }
    // Still blended in: playing, or fading out after Stop.
    bool isActive() const { return active_; }
    float length() const;
    // Looped and Priority: the value set on the track, else the clip's,
    // else false and Action while the clip hasn't loaded (0x103411c14,
    // 0x103411c40).
    bool looped() const;
    void setLooped(bool looped) { looped_ = looped; }
    Enum::AnimationPriority priority() const;
    void setPriority(Enum::AnimationPriority priority) { priority_ = priority; }
    float speed() const { return speed_; }
    float timePosition() const { return time_; }
    // Jumping fires no keyframe events.
    void setTimePosition(float time);
    float weightCurrent() const { return weight_; }
    float weightTarget() const { return weightTarget_; }
    // The fade time of the last Play, Stop or AdjustWeight.
    float fadeTime() const { return fadeTime_; }
    // How many times Play has started it (restarts count): one Play on a
    // track already playing still changes what it does.
    std::uint32_t plays() const { return plays_; }

    Signal<std::string> keyframeReached; // KeyframeReached(keyframeName)
    Signal<> didLoop;
    Signal<> stopped; // Stop() was called (or a non-looped track ran out)
    Signal<> ended;   // faded out and left the Animator
    // GetMarkerReachedSignal(name): fires with the marker's Value.
    Signal<std::string>& markerReachedSignal(std::string_view name);

    // The clip, once loaded.
    ClipPtr clip() const;

private:
    friend class Animator;

    // Moves weight and time on by dt seconds and fires events. Returns
    // false once the track has faded out and should leave the Animator.
    bool advance(float dt);
    void fireEvents(float from, float to, bool wrapped, bool inclusiveEnd);
    void stopInternal(float fadeTime);

    Animator* animator_;
    ClipSource source_;
    mutable ClipPtr clip_;
    std::string name_;
    std::string animationId_;
    bool playing_ = false;
    bool active_ = false;
    bool destroyed_ = false;
    std::optional<bool> looped_;
    std::optional<Enum::AnimationPriority> priority_;
    float time_ = 0.0f;
    float speed_ = 1.0f;
    float weight_ = 0.0f;
    float weightTarget_ = 0.0f;
    float fadeRemaining_ = 0.0f;
    float fadeTime_ = 0.0f;
    std::uint32_t plays_ = 0;
    std::map<std::string, std::unique_ptr<Signal<std::string>>, std::less<>> markers_;
};

using TrackPtr = std::shared_ptr<AnimationTrack>;

// Plays tracks on the joints of one rig (a character model, or whatever
// holds an AnimationController): its Motor6Ds and AnimationConstraints.
//
// An AnimationConstraint joins Attachment0's part (its Part0) to
// Attachment1's part (Part1) and stands in for the Motor6D of the same name
// on upgraded avatars: R15 characters from a live server come with
// kinematic AnimationConstraints Root, Waist, Neck, LeftShoulder... between
// the RigAttachments and no Motor6Ds [capture]. The 0.742 client's animation
// code walks AnimationConstraints (RBX::Anim::ModelInstance's countJoints,
// drawSkeleton) [code]; matching a clip's poses to them by Part1's and
// Part0's names as for Motor6Ds is a GUESS.
class Animator {
public:
    explicit Animator(Instance& rig);
    // Tracks can outlive their Animator (a script still holds one after its
    // rig went away); they're cut loose and never play again.
    ~Animator();
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;

    // Animator:LoadAnimation for a clip that's already compiled, or still
    // loading (see AnimationTrack::ClipSource).
    TrackPtr loadAnimation(const ClipPtr& clip, std::string name = {});
    TrackPtr loadAnimation(AnimationTrack::ClipSource clip, std::string name);

    // Animator:GetPlayingAnimationTracks().
    std::vector<TrackPtr> playingTracks() const;
    // Every track loaded and not yet released, playing or not, in load order.
    const std::vector<TrackPtr>& tracks() const { return tracks_; }

    // Moves every track on by dt and writes the blended pose of every joint
    // into its Transform (joints no track animates get identity).
    //
    // Blending (0x103421e00, 0x103495d62, 0x1034369b8): priorities from
    // Action4 down to Idle, then Core last (it's the lowest). Within a
    // priority, joint samples are summed weighted by track weight (tracks
    // under 0.001 skipped; quaternions flipped onto the running sum's
    // hemisphere). Each priority then takes min(its weight, what's left of
    // 1), its position averaged when its weights add up past 1; lower
    // priorities only get the remainder. The result's rotation is scaled by
    // the total weight from identity, its position from zero, then scaled:
    // see translationScale().
    void step(float dt);

    // The rig's joints were rebuilt or swapped: look them up again.
    void refreshJoints();

    Instance& rig() const { return *rig_; }

private:
    friend class AnimationTrack;

    // Joints are held weakly: scripts destroy them (BreakJoints, a removed
    // limb, the end of a round) while the rig lives on.
    struct Joint {
        Motor6D* motor = nullptr;                  // the joint is this
        AnimationConstraint* constraint = nullptr; // or this
        std::weak_ptr<Instance> alive;
        std::string part;   // Part1's name
        std::string parent; // Part0's name
        bool root = false;  // HumanoidRootPart to LowerTorso

        void setTransform(const CFrame& transform) const;
    };

    // What the blended translation of a joint is multiplied by.
    struct TranslationScale {
        float joint = 1.0f;
        float root = 1.0f;
    };
    TranslationScale translationScale() const;

    void remove(const AnimationTrack* track);
    bool activate(AnimationTrack* track);
    void apply();
    const Joint* findJoint(const JointCurve& curve) const;

    Instance* rig_;
    std::vector<Joint> joints_;
    std::vector<TrackPtr> tracks_; // loaded and not yet released, in load order

    // DFInt MaxActiveAnimationTracks (default set at 0x10343f698).
    static constexpr std::size_t maxActiveTracks = 256;
};

} // namespace lunar::avatar
