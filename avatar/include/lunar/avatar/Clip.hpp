#pragma once

// Animation clips: a KeyframeSequence compiled into per-joint key lists that
// can be sampled at any time.

#include <lunar/Classes.hpp>
#include <lunar/avatar/Quaternion.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lunar::avatar {

// One Pose of one joint at one Keyframe. The rotation is kept as a rotation
// vector (axis times angle), the form the client compiles poses into.
struct PoseKey {
    float time = 0.0f;
    Vector3 position;
    Vector3 rotation;
    Enum::PoseEasingStyle style = Enum::PoseEasingStyle::Linear;
    Enum::PoseEasingDirection direction = Enum::PoseEasingDirection::In;
};

// Every key of the joint whose Part1 is named `part` (and whose Part0 is
// named `parent`, the enclosing Pose), sorted by time.
struct JointCurve {
    std::string part;
    std::string parent;
    std::vector<PoseKey> keys;
};

// A named moment: a Keyframe whose Name isn't "Keyframe" (KeyframeReached
// fires with that name) or a KeyframeMarker (GetMarkerReachedSignal(name)
// fires with its Value).
struct ClipEvent {
    float time = 0.0f;
    std::string name;
    std::string value;
    bool marker = false;
};

// A joint's sampled offset: the Motor6D.Transform it asks for.
struct JointSample {
    Vector3 position;
    Quaternion rotation;

    CFrame cframe() const { return rotation.toCFrame(position); }
};

// The client's rotation interpolation between two poses (0x103490548): a
// straight blend of rotation vectors, kept on the short way round, then
// turned back into a quaternion. `f` is the eased fraction towards b.
Quaternion blendRotations(Vector3 a, Vector3 b, float f);

struct AnimationClipData {
    std::string name;
    float length = 0.0f;    // time of the last keyframe (never negative)
    float firstTime = 0.0f; // time of the first keyframe
    bool loop = true;
    Enum::AnimationPriority priority = Enum::AnimationPriority::Action;
    std::vector<JointCurve> joints;
    std::vector<ClipEvent> events; // sorted by time

    // The joint's pose at time t, as the client samples it (0x10348cad4):
    // between the nearest keys at or before t and after t, eased by the
    // earlier key's style; held after the last key. Before a joint's first
    // key there is nothing (nullopt) unless that key is on the clip's first
    // keyframe, which then holds back to time 0.
    std::optional<JointSample> sample(std::size_t joint, float t) const;
};

using ClipPtr = std::shared_ptr<const AnimationClipData>;

// Compiles a KeyframeSequence (its Keyframes in any order).
ClipPtr compileKeyframeSequence(const KeyframeSequence& sequence);

// The first KeyframeSequence in a loaded animation asset (a model file whose
// root is the sequence, as Roblox serves them), compiled. Null if none.
ClipPtr compileAnimationAsset(const std::vector<InstancePtr>& roots);

} // namespace lunar::avatar
