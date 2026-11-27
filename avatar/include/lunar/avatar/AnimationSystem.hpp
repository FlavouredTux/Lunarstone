#pragma once

// Animating every character of a game each frame: the per-frame entry point
// the game loop calls.

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Animate.hpp>
#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/ClipLoader.hpp>
#include <lunar/avatar/Rig.hpp>

#include <memory>
#include <vector>

namespace lunar::avatar {

// What a character is animated by.
struct CharacterAnimation {
    std::weak_ptr<Model> character;
    std::unique_ptr<Animator> animator;
    std::unique_ptr<AnimateController> animate; // null for rigs without the Animate behaviour
    std::unique_ptr<CharacterJointSolver> joints;
};

class AnimationSystem {
public:
    explicit AnimationSystem(std::shared_ptr<assets::AssetCache> cache = nullptr);

    // Starts animating a character (or other rig): an Animator for its
    // Motor6Ds and, with `animate`, the default Animate behaviour driven by
    // the AnimateController's on*() handlers. Adding one twice returns the
    // existing entry.
    CharacterAnimation& add(const std::shared_ptr<Model>& character, bool animate = true);
    void remove(const Model& character);
    CharacterAnimation* find(const Model& character);

    // One frame, in Roblox's order: the Animate logic reacts to the state it
    // was told about, every Animator advances its tracks and writes the
    // joints' Transform (Roblox does this before the physics step), then the
    // joints place each part from its root part. Call it after physics has
    // moved the root parts for this frame and before rendering. Characters
    // that were destroyed are dropped.
    void step(float dt);

    ClipLoader& clips() { return clips_; }
    const std::shared_ptr<assets::AssetCache>& assets() const { return cache_; }

private:
    std::shared_ptr<assets::AssetCache> cache_;
    ClipLoader clips_;
    std::vector<std::unique_ptr<CharacterAnimation>> entries_;
    std::uint32_t seed_ = 1;
};

} // namespace lunar::avatar
