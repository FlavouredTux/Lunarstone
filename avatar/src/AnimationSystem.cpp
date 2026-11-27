#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/Rig.hpp>

#include <algorithm>
#include <utility>

namespace lunar::avatar {

namespace {

std::shared_ptr<assets::AssetCache> orDefault(std::shared_ptr<assets::AssetCache> cache)
{
    return cache ? std::move(cache) : std::make_shared<assets::AssetCache>();
}

} // namespace

AnimationSystem::AnimationSystem(std::shared_ptr<assets::AssetCache> cache)
    : cache_(orDefault(std::move(cache))), clips_(cache_)
{
}

CharacterAnimation& AnimationSystem::add(const std::shared_ptr<Model>& character, bool animate)
{
    if (CharacterAnimation* existing = find(*character))
        return *existing;
    auto entry = std::make_unique<CharacterAnimation>();
    entry->character = character;
    entry->animator = std::make_unique<Animator>(*character);
    entry->joints = std::make_unique<CharacterJointSolver>(character);
    if (animate)
        entry->animate = std::make_unique<AnimateController>(*character, *entry->animator, clips_, seed_++);
    entries_.push_back(std::move(entry));
    return *entries_.back();
}

void AnimationSystem::remove(const Model& character)
{
    std::erase_if(entries_, [&](const auto& entry) { return entry->character.lock().get() == &character; });
}

CharacterAnimation* AnimationSystem::find(const Model& character)
{
    for (const auto& entry : entries_)
        if (entry->character.lock().get() == &character)
            return entry.get();
    return nullptr;
}

void AnimationSystem::step(float dt)
{
    std::erase_if(entries_, [](const auto& entry) {
        auto character = entry->character.lock();
        return !character || character->destroyed();
    });
    for (const auto& entry : entries_) {
        if (entry->animate)
            entry->animate->update(dt);
        entry->animator->step(dt);
        entry->joints->solve();
    }
}

} // namespace lunar::avatar
