#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/Quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace lunar::avatar {

// --- AnimationTrack ------------------------------------------------------------------

AnimationTrack::AnimationTrack(Animator& animator, ClipSource clip, std::string name)
    : animator_(&animator), source_(std::move(clip)), name_(std::move(name))
{
}

ClipPtr AnimationTrack::clip() const
{
    if (!clip_ && source_)
        clip_ = source_();
    return clip_;
}

float AnimationTrack::length() const
{
    ClipPtr c = clip();
    return c ? c->length : 0.0f;
}

bool AnimationTrack::looped() const
{
    if (looped_)
        return *looped_;
    ClipPtr c = clip();
    return c ? c->loop : false;
}

Enum::AnimationPriority AnimationTrack::priority() const
{
    if (priority_)
        return *priority_;
    ClipPtr c = clip();
    return c ? c->priority : Enum::AnimationPriority::Action;
}

void AnimationTrack::setTimePosition(float time)
{
    const float end = length();
    time_ = end > 0.0f ? std::clamp(time, 0.0f, end) : std::max(time, 0.0f);
}

Signal<std::string>& AnimationTrack::markerReachedSignal(std::string_view name)
{
    auto found = markers_.find(name);
    if (found == markers_.end())
        found = markers_.emplace(std::string(name), std::make_unique<Signal<std::string>>()).first;
    return *found->second;
}

void AnimationTrack::play(float fadeTime, float weight, float speed)
{
    if (!animator_ || !animator_->activate(this))
        return;
    speed_ = speed;
    time_ = speed < 0.0f ? length() : 0.0f;
    // WeightCurrent moves at the next step, even with no fade (0x103411ea0
    // only sets the target and the fade time).
    weightTarget_ = weight;
    fadeRemaining_ = std::max(fadeTime, 0.0f);
    fadeTime_ = fadeTime;
    playing_ = true;
    ++plays_;
}

void AnimationTrack::stopInternal(float fadeTime)
{
    // 0x103410bbc: the target and fade are set whether or not the track is
    // playing, so a second Stop restarts a fade-out already under way (the
    // 0.3 s one of a track that ran out, say); Stopped fires only for the
    // first.
    const bool wasPlaying = playing_;
    weightTarget_ = 0.0f;
    fadeRemaining_ = std::max(fadeTime, 0.0f);
    fadeTime_ = fadeTime;
    playing_ = false;
    if (wasPlaying)
        stopped.fire();
}

void AnimationTrack::stop(float fadeTime)
{
    stopInternal(fadeTime);
}

void AnimationTrack::adjustWeight(float weight, float fadeTime)
{
    if (!playing_)
        return;
    weightTarget_ = weight;
    fadeRemaining_ = std::max(fadeTime, 0.0f);
    fadeTime_ = fadeTime;
}

void AnimationTrack::adjustSpeed(float speed)
{
    speed_ = speed;
}

void AnimationTrack::destroy()
{
    destroyed_ = true;
    if (!active_ && animator_)
        animator_->remove(this);
}

void AnimationTrack::fireEvents(float from, float to, bool wrapped, bool inclusiveEnd)
{
    ClipPtr c = clip();
    if (!c || c->events.empty())
        return;
    // The window the step swept, as in 0x10348bdf4: moving forwards,
    // from <= time < to; backwards, to < time <= from. A wrap goes round the
    // end. A non-looped track reaching its end takes the end itself too.
    const bool forwards = speed_ >= 0.0f;
    auto inWindow = [&](float t) {
        if (forwards) {
            if (wrapped)
                return t >= from || t < to;
            return t >= from && (t < to || (inclusiveEnd && t <= to));
        }
        if (wrapped)
            return t <= from || t > to;
        return t <= from && (t > to || (inclusiveEnd && t >= to));
    };
    std::vector<const ClipEvent*> hits;
    auto collect = [&](const ClipEvent& event) {
        if (inWindow(event.time))
            hits.push_back(&event);
    };
    // In sweep order: forwards from `from` (round the end when wrapping),
    // backwards likewise.
    if (forwards) {
        for (const ClipEvent& event : c->events)
            if (!wrapped || event.time >= from)
                collect(event);
        if (wrapped)
            for (const ClipEvent& event : c->events)
                if (event.time < from)
                    collect(event);
    } else {
        for (const ClipEvent& event : c->events | std::views::reverse)
            if (!wrapped || event.time <= from)
                collect(event);
        if (wrapped)
            for (const ClipEvent& event : c->events | std::views::reverse)
                if (event.time > from)
                    collect(event);
    }
    // Markers go out first, then KeyframeReached (0x10340fe52).
    for (const ClipEvent* event : hits)
        if (event->marker)
            if (auto found = markers_.find(event->name); found != markers_.end())
                found->second->fire(event->value);
    for (const ClipEvent* event : hits)
        if (!event->marker)
            keyframeReached.fire(event->name);
}

bool AnimationTrack::advance(float dt)
{
    // Weight: linear from where it was to the target over the fade time
    // (0x103410fbc, before the time moves).
    if (fadeRemaining_ > 0.0f) {
        const float remaining = std::max(fadeRemaining_ - dt, 0.0f);
        weight_ = remaining > 0.0f ? weightTarget_ + (weight_ - weightTarget_) * (remaining / fadeRemaining_)
                                   : weightTarget_;
        fadeRemaining_ = remaining;
    } else {
        weight_ = weightTarget_;
    }
    // Faded out to nothing: leave the Animator (0x10341435e).
    if (fadeRemaining_ <= 0.0f && std::abs(weightTarget_) <= 1e-5f) {
        if (playing_)
            stopInternal(0.0f);
        ended.fire();
        return false;
    }

    // GUESS: a track fading out after Stop holds its time (the binary holds
    // a finished non-looped track at its end; DidLoop fires only once).
    if (!playing_)
        return true;
    ClipPtr c = clip();
    const float end = c ? c->length : 0.0f;
    const float before = time_;
    const float raw = time_ + speed_ * dt;
    if (end <= 0.0f) {
        time_ = raw;
        return true;
    }
    const bool loops = looped();
    if (loops) {
        time_ = std::fmod(raw, end);
        if (time_ < 0.0f)
            time_ += end;
    } else {
        time_ = std::clamp(raw, 0.0f, end);
    }
    const bool ranOut = !loops && (speed_ >= 0.0f ? time_ >= end : time_ <= 0.0f);
    const bool wrapped = loops && (raw > end || raw < 0.0f);
    fireEvents(before, time_, wrapped, ranOut);
    // DidLoop fires on passing either end, looped or not.
    if (raw > end || raw < 0.0f)
        didLoop.fire();
    // A non-looped track that ran out holds its last pose while it fades
    // out over 0.3 s (constant at 0x10607518c).
    if (ranOut && playing_)
        stopInternal(0.3f);
    return true;
}

// --- Animator --------------------------------------------------------------------------

Animator::Animator(Instance& rig) : rig_(&rig)
{
    refreshJoints();
}

Animator::~Animator()
{
    for (const TrackPtr& track : tracks_)
        track->animator_ = nullptr;
}

namespace {

// The part an attachment is on (directly, or through parent attachments).
const BasePart* partOf(const Attachment* attachment)
{
    const Instance* node = attachment;
    while (node && !node->isA("BasePart"))
        node = node->parent();
    return static_cast<const BasePart*>(node);
}

} // namespace

void Animator::refreshJoints()
{
    joints_.clear();
    for (const InstancePtr& descendant : rig_->descendants()) {
        Joint joint;
        const BasePart* part0 = nullptr;
        const BasePart* part1 = nullptr;
        if (auto* motor = instanceCast<Motor6D>(descendant.get())) {
            joint.motor = motor;
            part0 = motor->part0();
            part1 = motor->part1();
        } else if (auto* constraint = instanceCast<AnimationConstraint>(descendant.get())) {
            joint.constraint = constraint;
            part0 = partOf(constraint->attachment0());
            part1 = partOf(constraint->attachment1());
        } else {
            continue;
        }
        if (!part1)
            continue;
        joint.alive = descendant;
        joint.part = part1->name();
        joint.parent = part0 ? part0->name() : std::string();
        // The joint the Animator remembers as the rig's root (0x10341ea9c):
        // HumanoidRootPart to LowerTorso, so R15 only.
        joint.root = joint.part == "LowerTorso" && joint.parent == "HumanoidRootPart";
        joints_.push_back(std::move(joint));
    }
}

void Animator::Joint::setTransform(const CFrame& transform) const
{
    if (motor)
        motor->setTransform(transform);
    else
        constraint->setTransform(transform);
}

TrackPtr Animator::loadAnimation(const ClipPtr& clip, std::string name)
{
    if (name.empty() && clip)
        name = clip->name;
    return loadAnimation([clip] { return clip; }, std::move(name));
}

TrackPtr Animator::loadAnimation(AnimationTrack::ClipSource clip, std::string name)
{
    auto track = std::make_shared<AnimationTrack>(*this, std::move(clip), std::move(name));
    tracks_.push_back(track);
    return track;
}

std::vector<TrackPtr> Animator::playingTracks() const
{
    std::vector<TrackPtr> playing;
    for (const TrackPtr& track : tracks_)
        if (track->isPlaying())
            playing.push_back(track);
    return playing;
}

bool Animator::activate(AnimationTrack* track)
{
    if (track->active_)
        return true;
    const auto active
        = static_cast<std::size_t>(std::ranges::count_if(tracks_, [](const TrackPtr& t) { return t->active_; }));
    // "AnimationTrack limit of %d tracks exceeded" (0x10341be92).
    if (active > maxActiveTracks)
        return false;
    track->active_ = true;
    return true;
}

void Animator::remove(const AnimationTrack* track)
{
    std::erase_if(tracks_, [&](const TrackPtr& t) {
        if (t.get() != track)
            return false;
        t->animator_ = nullptr; // released: Play does nothing from now on
        return true;
    });
}

const Animator::Joint* Animator::findJoint(const JointCurve& curve) const
{
    const Joint* byPart = nullptr;
    for (const Joint& joint : joints_) {
        if (joint.part != curve.part)
            continue;
        if (joint.parent == curve.parent)
            return &joint;
        if (!byPart)
            byPart = &joint;
    }
    return byPart;
}

void Animator::step(float dt)
{
    // A joint destroyed since (its last reference gone) is never touched again.
    std::erase_if(joints_, [](const Joint& joint) { return joint.alive.expired(); });
    // Handlers may play, stop or destroy tracks; walk a copy.
    const std::vector<TrackPtr> tracks = tracks_;
    for (const TrackPtr& track : tracks) {
        if (!track->active_)
            continue;
        if (!track->advance(dt)) {
            track->active_ = false;
            if (track->destroyed_)
                remove(track.get());
        }
    }
    apply();
}

namespace {

// The order layers are merged in: highest priority first, Core last (table
// at 0x1064ba8b0).
constexpr Enum::AnimationPriority blendOrder[] = {
    Enum::AnimationPriority::Action4, Enum::AnimationPriority::Action3,  Enum::AnimationPriority::Action2,
    Enum::AnimationPriority::Action,  Enum::AnimationPriority::Movement, Enum::AnimationPriority::Idle,
    Enum::AnimationPriority::Core,
};

// A weighted sum of joint samples (0x103495d62).
struct Accumulator {
    Quaternion q{0.0f, 0.0f, 0.0f, 0.0f};
    Vector3 p;
    float w = 0.0f;

    void add(const Quaternion& rotation, Vector3 position, float weight)
    {
        const Quaternion aligned = q.dot(rotation) < 0.0f ? -rotation : rotation;
        q = q + aligned * weight;
        p = p + position * weight;
        w += weight;
    }
};

} // namespace

Animator::TranslationScale Animator::translationScale() const
{
    // 0x103422589..0x10342273e: Model:GetScale() of the model holding the
    // Humanoid (1 without one); the root joint instead HipHeight / 2 when
    // the Humanoid has AutomaticScalingEnabled, which is what makes a
    // shorter or taller avatar's hips bob and lift in proportion (the
    // Animate script's getHeightScale uses the same reference hip height of 2).
    //
    // GUESS: the binary checks the Animator's own parent is a Humanoid; the
    // rig's Humanoid stands in for that here (rigs animated through an
    // AnimationController normally have none).
    TranslationScale scale;
    if (const auto* model = instanceCast<const Model>(rig_))
        scale.joint = model->scaleFactor();
    scale.root = scale.joint;
    if (const auto* humanoid = instanceCast<const Humanoid>(rig_->findFirstChildOfClass("Humanoid"));
        humanoid && humanoid->automaticScalingEnabled())
        scale.root = humanoid->hipHeight() * 0.5f;
    return scale;
}

void Animator::apply()
{
    constexpr std::size_t layers = std::size(blendOrder);
    const TranslationScale scale = translationScale();
    auto layerOf = [](Enum::AnimationPriority priority) {
        for (std::size_t i = 0; i < layers; ++i)
            if (blendOrder[i] == priority)
                return i;
        return std::size_t{3}; // unknown values blend as Action
    };

    std::unordered_map<const Joint*, std::array<Accumulator, layers>> perJoint;
    for (const TrackPtr& track : tracks_) {
        // "AnimationTrack::accumulate" skips nearly weightless tracks.
        if (!track->active_ || track->weightCurrent() < 0.001f)
            continue;
        ClipPtr clip = track->clip();
        if (!clip)
            continue;
        const std::size_t layer = layerOf(track->priority());
        for (std::size_t i = 0; i < clip->joints.size(); ++i) {
            const Joint* joint = findJoint(clip->joints[i]);
            if (!joint)
                continue;
            auto sample = clip->sample(i, track->timePosition());
            if (!sample)
                continue;
            perJoint[joint][layer].add(sample->rotation, sample->position, track->weightCurrent());
        }
    }

    for (const Joint& joint : joints_) {
        auto found = perJoint.find(&joint);
        if (found == perJoint.end()) {
            joint.setTransform(CFrame());
            continue;
        }
        // Merge the layers top down (0x1034369b8).
        Accumulator total;
        for (Accumulator layer : found->second) {
            if (layer.w <= 0.0f)
                continue;
            if (layer.w > 1.0f) {
                layer.p = layer.p / layer.w;
                layer.w = 1.0f;
            }
            const float take = std::min(layer.w, 1.0f - total.w);
            const float k = take / layer.w;
            const Quaternion aligned = total.q.dot(layer.q) < 0.0f ? -layer.q : layer.q;
            total.q = total.q + aligned * k;
            total.p = total.p + layer.p * k;
            total.w += take;
            if (total.w >= 1.0f)
                break;
        }
        // Rotation from identity by the total weight, position from zero
        // (0x1034231df).
        Quaternion rotation = total.q.dot(total.q) > 0.0f ? total.q.normalized() : Quaternion{};
        const float weight = std::min(total.w, 1.0f);
        if (weight < 1.0f)
            rotation = fromRotationVector(toRotationVector(rotation) * weight);
        joint.setTransform(rotation.toCFrame(total.p * (joint.root ? scale.root : scale.joint)));
    }
}

} // namespace lunar::avatar
