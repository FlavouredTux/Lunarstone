#pragma once

// Other players' animations on a client. The client that owns a character
// plays its tracks (the Animate script's, a tool's) and its Animator sends
// every change through the server as the OnCombinedUpdate event; the other
// clients play the same tracks on their copy of the rig. Nothing about a
// track crosses the network but its AnimationId and the values below.

#include <lunar/Classes.hpp>
#include <lunar/Value.hpp>
#include <lunar/avatar/Animator.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::avatar {

class AnimationSystem;

// Animator.OnCombinedUpdate(animation, playState, fadeTime, weight, speed,
// timePosition, priority, looping, valuesUpdated): one track of the sending
// Animator, named by its AnimationId. The parameter names are the event's
// registration (0x10016259c) [code]; which values each valuesUpdated bit
// covers is from captures of a live server [capture].
struct TrackUpdate {
    std::string animationId;
    bool playing = false;
    float fadeTime = 0.1f;
    float weight = 1.0f;
    float speed = 1.0f;
    float timePosition = 0.0f;
    Enum::AnimationPriority priority = Enum::AnimationPriority::Core;
    bool looped = false;
    std::uint32_t changed = 0; // valuesUpdated

    // valuesUpdated. A track the sender hasn't sent before comes with
    // Looping and Priority set, plus PlayState when it plays at once; after
    // that only what changed is flagged, and an unflagged Looping reads
    // false whatever the track does [capture]. Bits 3 and 5 were never
    // seen set: GUESS, TimePosition by elimination (the value travels in
    // every update, but only the sender's own clock moves it).
    static constexpr std::uint32_t PlayState = 1u << 0;
    static constexpr std::uint32_t Weight = 1u << 1;
    static constexpr std::uint32_t Speed = 1u << 2;
    static constexpr std::uint32_t TimePosition = 1u << 3;
    static constexpr std::uint32_t Looping = 1u << 4;
    static constexpr std::uint32_t Priority = 1u << 6;

    // From the event's arguments as the network decodes them (ContentId as
    // a string, floats and the int as numbers, the priority as an EnumItem
    // or its number); nullopt when they aren't shaped like one.
    static std::optional<TrackUpdate> fromArguments(std::span<const Value> arguments);
    // The event's nine arguments, as fromArguments reads them back.
    std::vector<Value> arguments() const;
};

// The sending side: what the owner's client sends for its own Animator
// (our character's, whose tracks the Animate script plays). The client
// sends combined updates, one per track that changed since the last frame
// [capture: the 0.742 client's own Animator in a genuine join]:
//   - a track it hasn't sent before goes out with Looping and Priority
//     flagged, and PlayState when it plays: valuesUpdated 80 for the tracks
//     Animate only loads (weight 0, fade 0), 81 for the one it plays;
//   - after that only what changed: PlayState when it starts, restarts or
//     stops (1, with weight 0 on a stop), Weight for AdjustWeight (2),
//     Speed for AdjustSpeed (4), combined when they come in one frame;
//   - every update carries the track's values: the target weight, the
//     fade time of its last Play/Stop/AdjustWeight, its speed, time
//     position and priority; Looping reads false unless flagged.
// GUESS: Weight and Speed flagged together with a Play when it plays at
// another weight or speed than 1 (83, 85, 87 in captures); the client's
// own bookkeeping wasn't traced. Lunarstone's own: a track's weight and
// speed changes go out at most every kAdjustInterval (the latest values),
// so a character pushing against a wall, whose Animate adjusts the walk
// every frame, doesn't send sixty updates a second.
class TrackSender {
public:
    // What changed on `animator` since the last call. A track seen for the
    // first time waits up to `clipWait` seconds for its clip, so its Looping
    // and Priority go out as the animation has them; `dt` is the time since
    // the last call.
    std::vector<TrackUpdate> collect(const Animator& animator, float dt, float clipWait = 1.0f);
    static constexpr float kAdjustInterval = 0.1f;
    // Forget what was sent (a new character, a new Animator).
    void reset() { sent_.clear(); }

private:
    struct Sent {
        std::weak_ptr<AnimationTrack> track;
        bool announced = false; // the first update went out
        float waited = 0.0f;    // for the clip, before that
        float since = 0.0f;     // seconds since the last update went out
        bool playing = false;
        std::uint32_t plays = 0;
        float weight = 0.0f;
        float speed = 1.0f;
        bool looped = false;
        Enum::AnimationPriority priority = Enum::AnimationPriority::Core;
    };
    std::unordered_map<const AnimationTrack*, Sent> sent_;
};

// Plays what other clients replicate on an AnimationSystem: per Animator
// instance, one native track for each AnimationId it has sent, on the rig
// that holds the Animator (Model > Humanoid or AnimationController >
// Animator). The system's step poses the rig like any other.
class ReplicatedAnimation {
public:
    explicit ReplicatedAnimation(AnimationSystem& system) : system_(system) {}

    // One update for `animator`. A track the update says is playing starts
    // (again) when the update flags PlayState or the track isn't playing
    // here (we joined after it started), at the update's time position;
    // otherwise flagged weight and speed changes go through AdjustWeight
    // (with the update's fadeTime) and AdjustSpeed. Not playing stops it
    // with the fadeTime, but only with PlayState flagged: unflagged, it is
    // another track of the same animation the sender just loaded. Flagged
    // Looping and Priority override the clip's.
    // Ignored when the Animator isn't in a rig.
    //
    // GUESS: how the 0.742 client applies an update wasn't traced; this is
    // what makes the receiver's tracks match the values each update carries.
    void apply(Instance& animator, const TrackUpdate& update);

    // The native track playing `animationId` for `animator`, if it sent one.
    TrackPtr track(const Instance& animator, std::string_view animationId) const;
    // Whether a track of the rig's own (the scripts' or Animate's, not one an
    // update made) is playing `animationId`: an update for it on the
    // character this client owns is the server relaying what we sent.
    bool playsLocally(const Instance& animator, std::string_view animationId) const;

private:
    struct Rig {
        std::weak_ptr<Instance> animator;
        std::unordered_map<std::string, TrackPtr> tracks; // by ContentId key
    };

    AnimationSystem& system_;
    std::unordered_map<const Instance*, Rig> rigs_;
};

} // namespace lunar::avatar
