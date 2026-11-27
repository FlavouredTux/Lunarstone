#include <lunar/avatar/Clip.hpp>
#include <lunar/avatar/Easing.hpp>
#include <lunar/avatar/Quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <utility>

namespace lunar::avatar {

namespace {

struct CurveKey {
    std::string part;
    std::string parent;
    auto operator<=>(const CurveKey&) const = default;
};

void collectPoses(const Instance& parent, std::string_view parentName, float time,
                  std::map<CurveKey, std::vector<PoseKey>>& curves)
{
    for (const InstancePtr& child : parent.children()) {
        const auto* pose = dynamic_cast<const Pose*>(child.get());
        if (!pose)
            continue;
        // Poses with no weight are dropped when compiling (0x103491d5a), so
        // the joint is missing from that keyframe. Any other Weight is
        // stored but no sampler reads it.
        if (pose->weight() > 1e-6f)
            curves[{pose->name(), std::string(parentName)}].push_back({time, pose->cframe().position,
                                                                       toRotationVector(pose->cframe()),
                                                                       pose->easingStyle(), pose->easingDirection()});
        collectPoses(*pose, pose->name(), time, curves);
    }
}

} // namespace

ClipPtr compileKeyframeSequence(const KeyframeSequence& sequence)
{
    auto clip = std::make_shared<AnimationClipData>();
    clip->name = sequence.name();
    clip->loop = sequence.loop();
    clip->priority = sequence.priority();

    std::map<CurveKey, std::vector<PoseKey>> curves;
    bool first = true;
    for (const InstancePtr& child : sequence.children()) {
        const auto* keyframe = dynamic_cast<const Keyframe*>(child.get());
        if (!keyframe)
            continue;
        const float time = keyframe->time();
        clip->length = std::max(clip->length, time);
        clip->firstTime = first ? time : std::min(clip->firstTime, time);
        first = false;
        // Keyframes named anything but the default fire KeyframeReached.
        if (keyframe->name() != "Keyframe")
            clip->events.push_back({time, keyframe->name(), {}, false});
        for (const InstancePtr& item : keyframe->children())
            if (const auto* marker = dynamic_cast<const KeyframeMarker*>(item.get()))
                clip->events.push_back({time, marker->name(), marker->value(), true});
        collectPoses(*keyframe, {}, time, curves);
    }
    // Files keep keyframes in whatever order they were made in.
    for (auto& [key, keys] : curves) {
        std::ranges::stable_sort(keys, {}, &PoseKey::time);
        clip->joints.push_back({key.part, key.parent, std::move(keys)});
    }
    std::ranges::stable_sort(clip->events, {}, &ClipEvent::time);
    return clip;
}

ClipPtr compileAnimationAsset(const std::vector<InstancePtr>& roots)
{
    for (const InstancePtr& root : roots) {
        if (const auto* sequence = dynamic_cast<const KeyframeSequence*>(root.get()))
            return compileKeyframeSequence(*sequence);
        if (const auto* nested
            = dynamic_cast<const KeyframeSequence*>(root->findFirstChildWhichIsA("KeyframeSequence", true)))
            return compileKeyframeSequence(*nested);
    }
    return nullptr;
}

Quaternion blendRotations(Vector3 a, Vector3 b, float f)
{
    constexpr float pi = std::numbers::pi_v<float>;
    // Pointing apart: the longer one may be better expressed the other way
    // round its axis (angle - 2 pi), so the blend takes the short way.
    if (const float d = a.dot(b); d < 0.0f) {
        Vector3& longer = a.dot(a) >= b.dot(b) ? a : b;
        const float length = longer.magnitude();
        if (length > 0.0f && length - d / length > pi)
            longer = longer * ((length - 2.0f * pi) / length);
    }
    Vector3 r = a * (1.0f - f) + b * f;
    if (const float squared = r.dot(r); squared > pi * pi) {
        const float length = std::sqrt(squared);
        r = r * ((length - 2.0f * pi) / length);
    }
    return fromRotationVector(r);
}

std::optional<JointSample> AnimationClipData::sample(std::size_t joint, float t) const
{
    if (joint >= joints.size() || joints[joint].keys.empty())
        return std::nullopt;
    const std::vector<PoseKey>& keys = joints[joint].keys;
    auto next = std::ranges::upper_bound(keys, t, {}, &PoseKey::time);
    if (next == keys.begin()) {
        // t is before the joint's first key: the client starts from the
        // keyframe at or before t (the first keyframe, at the least), and a
        // joint missing there isn't animated.
        if (keys.front().time > firstTime)
            return std::nullopt;
        return JointSample{keys.front().position, fromRotationVector(keys.front().rotation)};
    }
    const PoseKey& a = *(next - 1);
    if (next == keys.end())
        return JointSample{a.position, fromRotationVector(a.rotation)};
    const PoseKey& b = *next;
    const float span = b.time - a.time;
    const float f = poseEase(a.style, a.direction, span > 0.0f ? (t - a.time) / span : 1.0f);
    return JointSample{a.position + (b.position - a.position) * f, blendRotations(a.rotation, b.rotation, f)};
}

} // namespace lunar::avatar
