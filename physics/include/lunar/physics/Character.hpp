#pragma once

#include <lunar/Classes.hpp>

#include <memory>
#include <string_view>

namespace lunar::physics {

// A stand-in R15-sized character made of blocks: HumanoidRootPart (2x2x1,
// invisible), LowerTorso, UpperTorso, Head, arms and legs in three pieces
// each, joined by Motor6Ds named like the R15 rig's, plus a Humanoid with
// RigType R15 and HipHeight 2. Built standing with its feet at y = 0 and the
// root part's centre at y = 3, facing -Z. Only the root, torso and head
// collide, as the Humanoid leaves limbs to animation.
std::shared_ptr<Model> buildPlaceholderCharacter(std::string_view name);

} // namespace lunar::physics
