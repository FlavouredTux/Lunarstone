#include <lunar/avatar/ReplicatedAnimation.hpp>

#include <lunar/assets/ContentId.hpp>
#include <lunar/avatar/AnimationSystem.hpp>

namespace lunar::avatar {

namespace {

std::optional<double> number(const Value& value)
{
    if (const auto* d = std::get_if<double>(&value))
        return *d;
    if (const auto* e = std::get_if<EnumItem>(&value))
        return static_cast<double>(e->value);
    return std::nullopt;
}

std::string keyOf(std::string_view animationId)
{
    auto parsed = assets::parseContentId(animationId);
    return parsed ? parsed->key() : std::string(animationId);
}

// The rig an Animator moves: the Model holding its Humanoid or AnimationController.
std::shared_ptr<Model> rigOf(const Instance& animator)
{
    const Instance* holder = animator.parent();
    if (!holder || !(holder->isA("Humanoid") || holder->isA("AnimationController")))
        return nullptr;
    auto* model = dynamic_cast<Model*>(holder->parent());
    return model ? std::static_pointer_cast<Model>(model->shared_from_this()) : nullptr;
}

} // namespace

std::optional<TrackUpdate> TrackUpdate::fromArguments(std::span<const Value> arguments)
{
    if (arguments.size() < 9)
        return std::nullopt;
    const auto* id = std::get_if<std::string>(&arguments[0]);
    const auto* playing = std::get_if<bool>(&arguments[1]);
    const auto* looped = std::get_if<bool>(&arguments[7]);
    const auto fade = number(arguments[2]), weight = number(arguments[3]), speed = number(arguments[4]),
               time = number(arguments[5]), priority = number(arguments[6]), changed = number(arguments[8]);
    if (!id || id->empty() || !playing || !looped || !fade || !weight || !speed || !time || !priority || !changed)
        return std::nullopt;
    TrackUpdate update;
    update.animationId = *id;
    update.playing = *playing;
    update.fadeTime = static_cast<float>(*fade);
    update.weight = static_cast<float>(*weight);
    update.speed = static_cast<float>(*speed);
    update.timePosition = static_cast<float>(*time);
    update.priority = static_cast<Enum::AnimationPriority>(static_cast<std::uint32_t>(*priority));
    update.looped = *looped;
    update.changed = static_cast<std::uint32_t>(*changed);
    return update;
}

std::vector<Value> TrackUpdate::arguments() const
{
    return {Value(animationId),
            Value(playing),
            Value(static_cast<double>(fadeTime)),
            Value(static_cast<double>(weight)),
            Value(static_cast<double>(speed)),
            Value(static_cast<double>(timePosition)),
            Value(static_cast<double>(static_cast<std::uint32_t>(priority))),
            Value(looped),
            Value(static_cast<double>(changed))};
}

std::vector<TrackUpdate> TrackSender::collect(const Animator& animator, float dt, float clipWait)
{
    std::vector<TrackUpdate> updates;
    // Tracks that went away (released and faded out) are forgotten.
    std::erase_if(sent_, [](const auto& item) { return item.second.track.expired(); });
    for (const TrackPtr& track : animator.tracks()) {
        if (track->animationId().empty())
            continue; // not loaded from an Animation: nothing names it on the network
        auto [it, fresh] = sent_.try_emplace(track.get());
        Sent& sent = it->second;
        if (fresh || sent.track.expired())
            sent = Sent{track};
        TrackUpdate update;
        update.animationId = track->animationId();
        update.playing = track->isPlaying();
        update.fadeTime = track->fadeTime();
        update.weight = track->weightTarget();
        update.speed = track->speed();
        update.timePosition = track->timePosition();
        update.priority = track->priority();
        const bool looped = track->looped();
        if (!sent.announced) {
            sent.waited += dt;
            if (!track->clip() && sent.waited < clipWait)
                continue;
            update.changed = TrackUpdate::Looping | TrackUpdate::Priority;
            if (update.playing) {
                update.changed |= TrackUpdate::PlayState;
                if (update.weight != 1.0f)
                    update.changed |= TrackUpdate::Weight;
                if (update.speed != 1.0f)
                    update.changed |= TrackUpdate::Speed;
            }
        } else {
            const bool replayed = track->plays() != sent.plays;
            if (update.playing != sent.playing || (update.playing && replayed)) {
                update.changed |= TrackUpdate::PlayState;
                if (update.playing && update.weight != 1.0f)
                    update.changed |= TrackUpdate::Weight;
                if (update.playing && update.speed != 1.0f)
                    update.changed |= TrackUpdate::Speed;
            } else if (update.playing) {
                if (update.weight != sent.weight)
                    update.changed |= TrackUpdate::Weight;
                if (update.speed != sent.speed)
                    update.changed |= TrackUpdate::Speed;
            }
            if (looped != sent.looped)
                update.changed |= TrackUpdate::Looping;
            if (update.priority != sent.priority)
                update.changed |= TrackUpdate::Priority;
            sent.since += dt;
            if (update.changed == 0)
                continue;
            constexpr std::uint32_t adjustments = TrackUpdate::Weight | TrackUpdate::Speed;
            if ((update.changed & ~adjustments) == 0 && sent.since < kAdjustInterval)
                continue; // goes out with the latest values once the interval is up
        }
        sent.since = 0.0f;
        update.looped = (update.changed & TrackUpdate::Looping) ? looped : false;
        sent.announced = true;
        sent.playing = update.playing;
        sent.plays = track->plays();
        sent.weight = update.weight;
        sent.speed = update.speed;
        sent.looped = looped;
        sent.priority = update.priority;
        updates.push_back(std::move(update));
    }
    return updates;
}

void ReplicatedAnimation::apply(Instance& animator, const TrackUpdate& update)
{
    std::shared_ptr<Model> model = rigOf(animator);
    if (!model)
        return;
    // Animators that went away (a respawned character) take their tracks with them.
    std::erase_if(rigs_, [](const auto& item) { return item.second.animator.expired(); });
    Rig& rig = rigs_[&animator];
    if (rig.animator.expired())
        rig = Rig{animator.weak_from_this(), {}};

    CharacterAnimation& entry = system_.add(model, false);
    const std::string key = keyOf(update.animationId);
    TrackPtr& track = rig.tracks[key];
    if (!track) {
        // Joints may have replicated after the system first saw this rig.
        entry.animator->refreshJoints();
        track = entry.animator->loadAnimation(system_.clips().source(update.animationId), key);
        track->setAnimationId(update.animationId);
    }
    if (update.changed & TrackUpdate::Looping)
        track->setLooped(update.looped);
    if (update.changed & TrackUpdate::Priority)
        track->setPriority(update.priority);

    // Only a flagged PlayState stops a track. A sender can hold several
    // tracks of one animation, and the updates name none of them but by
    // the AnimationId: when it loads another (a temporary scene prop loads the
    // move's animation again while the first one plays), that track's first
    // update says "not playing" with Looping and Priority flagged, and
    // stopping on it stopped the one still playing [capture].
    if (!update.playing) {
        if (update.changed & TrackUpdate::PlayState)
            track->stop(update.fadeTime);
        return;
    }
    if ((update.changed & TrackUpdate::PlayState) || !track->isPlaying()) {
        track->play(update.fadeTime, update.weight, update.speed);
        track->setTimePosition(update.timePosition);
        return;
    }
    if (update.changed & TrackUpdate::Weight)
        track->adjustWeight(update.weight, update.fadeTime);
    if (update.changed & TrackUpdate::Speed)
        track->adjustSpeed(update.speed);
    if (update.changed & TrackUpdate::TimePosition)
        track->setTimePosition(update.timePosition);
}

bool ReplicatedAnimation::playsLocally(const Instance& animator, std::string_view animationId) const
{
    const std::shared_ptr<Model> rig = rigOf(animator);
    const CharacterAnimation* entry = rig ? system_.find(*rig) : nullptr;
    if (!entry)
        return false;
    const TrackPtr replicated = track(animator, animationId);
    const std::string key = keyOf(animationId);
    return std::ranges::any_of(entry->animator->tracks(), [&](const TrackPtr& t) {
        return t != replicated && t->isPlaying() && keyOf(t->animationId()) == key;
    });
}

TrackPtr ReplicatedAnimation::track(const Instance& animator, std::string_view animationId) const
{
    auto rig = rigs_.find(&animator);
    if (rig == rigs_.end() || rig->second.animator.expired())
        return nullptr;
    auto found = rig->second.tracks.find(keyOf(animationId));
    return found == rig->second.tracks.end() ? nullptr : found->second;
}

} // namespace lunar::avatar
