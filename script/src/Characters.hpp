#pragma once

// What scripts reach of a character beyond its properties: AnimationTracks
// played by the native animation system (Animation.cpp) and the Humanoid's
// state machine and events (HumanoidApi.cpp).

#include "InstanceApi.hpp"

#include <span>

namespace lunar::avatar {
class AnimationSystem;
} // namespace lunar::avatar

namespace lunar::lua {

// Methods merged into the Instance method table: LoadAnimation and the
// AnimationTrack's (Animation.cpp), Humanoid:GetState and friends (HumanoidApi.cpp).
std::span<const Method> animationMethods();
std::span<const Method> humanoidMethods();

// Where LoadAnimation plays: the embedder's system (it steps it), or null to
// go back to one of the scripts' own, made on first use.
void setAnimationSystem(Shared& shared, avatar::AnimationSystem* system);
// Steps the scripts' own animation system, if they made one; the frame's
// animation phase calls it when the embedder doesn't step animation itself.
void stepOwnAnimation(Shared& shared, double dt);

} // namespace lunar::lua
