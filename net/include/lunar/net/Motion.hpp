#pragma once

#include <lunar/Instance.hpp>
#include <lunar/Signal.hpp>
#include <lunar/net/Physics.hpp>
#include <lunar/net/Values.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace lunar {
class BasePart;
}

namespace lunar::net {

// Physics replication on our side (ROBLOX-LAYER.md 9, notes-physics.md): the
// motion a server sends for the assemblies it simulates or relays from their
// owners (ISR ReplicationPV properties, 0x85 frames), played back a little
// behind real time so each assembly root glides from one sample to the next,
// and runs on by its velocity for a moment when the next sample is late. The
// parts welded or jointed to a root keep their place relative to it (a
// character's rig follows its HumanoidRootPart; Lunarstone's animation then
// poses the joints from the root as usual). A part that joins another
// root's assembly stops being a root of its own: of the two tracks, the one
// that still gets samples moves it (the game's ragdoll sets its Motor6Ds' Part0
// to nil, so each limb comes as its own root, then puts them back [capture]).
//
// The client keeps up to MaxFrameBufferSize (10) frames per mechanism,
// drops a frame older than its newest, replaces one with the same time,
// and averages the spacing of the frames' times (0.9 old + 0.1 new, each
// capped at InterpolationMaxDelayMSec, 500 ms); it shows the time
// TargetTimeDelayFacctorTenths/10 (1.0) of that average behind "now"
// (sub_10121ee1c, sub_10121eab4) [code: these numbers are the compiled-in
// FInt defaults, which a live client's FFlags can override]. What its
// "now" is relative to the server's clock (an offset kept next to the
// average) isn't traced, and neither is how it interpolates, so the rest
// is a GUESS shaped by the recordings: each root runs its own clock, as a
// sample's lateness (when it arrived less when it was taken) is the
// network's delay plus how long its owner's motion took to reach the
// server and go out again, which for a player's character varies by a
// couple of send intervals from one sample to the next [capture]. The
// playback runs the averaged spacing behind the latest of the last few
// seconds' latenesses, so the next sample is (nearly) always there to
// glide to; when the lateness changes the playback catches up by running
// a little faster or slower, or at once while the part stands still (a
// character that starts walking after a rest often comes with a much
// bigger age than it had [capture]). Between samples: cubic Hermite from their
// positions and velocities, slerp for the rotation; past the newest one it
// runs on by its velocities for at most `maxExtrapolation` seconds, then
// holds (and it holds across a pause in the samples: the server sends
// nothing while a part rests). When a sample that came late moves the path
// from under what was shown, the difference is eased out over
// `correctionTime` instead of jumped.
class ReplicatedMotion {
public:
    using Resolve = std::function<InstancePtr(InstanceId)>;

    ReplicatedMotion() = default;
    ~ReplicatedMotion();
    ReplicatedMotion(const ReplicatedMotion&) = delete;
    ReplicatedMotion& operator=(const ReplicatedMotion&) = delete;

    // A sample for `part` (an assembly root): its motion at `serverTime`
    // seconds (the server's clock, whichever packets it came in), which
    // arrived at local time `arrival` (seconds). `timed` false: the time is
    // only a bound (an ISR age clamped at 255 ticks), so the sample doesn't
    // count toward how late the playback runs.
    void receive(InstanceId part, double serverTime, double arrival, const PhysicsPV& pv, bool timed = true);

    // Where `part` is shown at local time `now`, and the server time that
    // is; nullopt without samples.
    std::optional<PhysicsPV> at(InstanceId part, double now) const;
    std::optional<double> shownAt(InstanceId part, double now) const;

    // Moves every root with samples (and its assembly) to where it is at
    // `now`. Call once a frame, before animating and drawing. Roots whose
    // instance is gone are forgotten after a while. Roots `ours` says this
    // client owns are left to its own simulation: their samples are dropped
    // and their playback starts afresh if the server takes them back.
    void step(double now, const Resolve& resolve, const std::function<bool(InstanceId)>& ours = {});
    // True once step has run: until then every sample is applied as it
    // arrives (what a decoder or a client that never steps shows).
    bool stepping() const { return stepping_; }
    // Applies `part`'s newest sample now.
    void applyNewest(InstanceId part, const Resolve& resolve);

    // How many averaged sample spacings behind the latest recent lateness
    // the playback runs (TargetTimeDelayFacctorTenths / 10 [code]).
    double delayFactor = 1.0;
    // How long a root may run on by its velocity past its newest sample
    // before it holds, and the time constant a correction fades with (GUESS
    // both).
    double maxExtrapolation = 0.25;
    double correctionTime = 0.1;

    std::size_t roots() const { return tracks_.size(); }
    std::vector<InstanceId> rootIds() const;
    std::size_t received() const { return received_; }
    // The samples received for `root` so far (0 for one never sent).
    std::size_t receivedFor(InstanceId root) const;

    // The parts held to `root` by enabled JointInstances, WeldConstraints,
    // AnimationConstraints and RigidConstraints inside the Model the root is
    // in (or under the root when it isn't in one), root excluded: what moves
    // with it.
    static std::vector<BasePart*> assemblyOf(BasePart& root);

private:
    struct Sample {
        double serverTime = 0;
        PhysicsPV pv;
    };
    struct Track {
        std::vector<Sample> samples; // by server time
        double spacing = 0;          // the averaged time between samples
        // (arrival, arrival - server time) of the last few seconds' samples;
        // the playback's offset eases toward their maximum.
        std::deque<std::pair<double, double>> lateness;
        double target = 0;
        std::optional<double> offset;
        double lastArrival = 0;
        std::size_t received = 0;
        double lastStep = 0;
        std::optional<double> lastShown; // the server time the last step showed
        bool held = false;               // and whether the part stood still there
        std::optional<PhysicsPV> applied;
        // The parts jointed to the root, worked out again only when the
        // root's model changes shape (like avatar::CharacterJointSolver).
        std::vector<std::weak_ptr<Instance>> assembly;
        std::shared_ptr<bool> dirty = std::make_shared<bool>(true);
        std::vector<Connection> watches;
    };

    void apply(Track& track, BasePart& root, const PhysicsPV& pv);
    // Drops the tracks whose part has been jointed into a livelier track's
    // assembly since (a ragdoll's limb once its joints are back): it is no
    // root any more, and the server only sends the assembly's root.
    void dropJoined(const Resolve& resolve);
    void rebuild(Track& track, BasePart& root);
    static void forget(Track& track);
    // The server time shown at `now`, and the path through the samples there.
    double shownTime(const Track& track, double now) const;
    // `held` (if given) says whether the part stands still there: before
    // its first sample, done running on past its newest, or in a pause.
    std::optional<PhysicsPV> pathAt(const Track& track, double shown, bool* held = nullptr) const;

    std::map<std::uint64_t, Track> tracks_;
    std::size_t received_ = 0;
    bool stepping_ = false;
};

// Interpolation between two motion samples `dt` seconds apart, `u` in
// [0, 1] of the way (cubic Hermite position, slerp rotation, lerp
// velocities), and the run-on by velocity for `dt` seconds.
PhysicsPV interpolatePv(const PhysicsPV& a, const PhysicsPV& b, double dt, double u);
PhysicsPV extrapolatePv(const PhysicsPV& pv, double dt);

} // namespace lunar::net
