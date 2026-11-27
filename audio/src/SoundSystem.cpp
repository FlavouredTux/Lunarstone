#include <lunar/audio/SoundSystem.hpp>

#include <lunar/assets/AssetCache.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <print>

namespace lunar::audio {

namespace {

// Where a sound is heard from: its part's or attachment's position; none
// for a sound anywhere else, which plays as it is.
std::optional<Vector3> emitterOf(const Sound& sound)
{
    const Instance* parent = sound.parent();
    if (const auto* part = dynamic_cast<const BasePart*>(parent))
        return part->position();
    if (const auto* attachment = dynamic_cast<const Attachment*>(parent))
        return attachment->worldPosition();
    return std::nullopt;
}

// Its SoundGroup's volume, times those of the groups that group is in.
float groupVolume(const Sound& sound)
{
    float volume = 1.0f;
    for (const Instance* at = sound.soundGroup(); at; at = at->parent()) {
        const auto* group = dynamic_cast<const SoundGroup*>(at);
        if (!group)
            break;
        volume *= std::clamp(group->volume(), 0.0f, 10.0f);
    }
    return volume;
}

// One SoundEffect as the mixer runs it; none for one it doesn't know.
std::optional<Effect> effectOf(const Instance& instance)
{
    using K = EffectKind;
    if (const auto* e = dynamic_cast<const EqualizerSoundEffect*>(&instance))
        return Effect{K::Equalizer, {e->lowGain(), e->midGain(), e->highGain()}};
    if (const auto* e = dynamic_cast<const DistortionSoundEffect*>(&instance))
        return Effect{K::Distortion, {e->level()}};
    if (const auto* e = dynamic_cast<const TremoloSoundEffect*>(&instance))
        return Effect{K::Tremolo, {e->depth(), e->duty(), e->frequency()}};
    if (const auto* e = dynamic_cast<const FlangeSoundEffect*>(&instance))
        return Effect{K::Flange, {e->depth(), e->mix(), e->rate()}};
    if (const auto* e = dynamic_cast<const ChorusSoundEffect*>(&instance))
        return Effect{K::Chorus, {e->depth(), e->mix(), e->rate()}};
    if (const auto* e = dynamic_cast<const EchoSoundEffect*>(&instance))
        return Effect{K::Echo, {e->delay(), e->dryLevel(), e->feedback(), e->wetLevel()}};
    if (const auto* e = dynamic_cast<const ReverbSoundEffect*>(&instance))
        return Effect{K::Reverb, {e->decayTime(), e->density(), e->diffusion(), e->dryLevel(), e->wetLevel()}};
    if (const auto* e = dynamic_cast<const CompressorSoundEffect*>(&instance))
        return Effect{K::Compressor, {e->attack(), e->gainMakeup(), e->ratio(), e->release(), e->threshold()}};
    if (const auto* e = dynamic_cast<const PitchShiftSoundEffect*>(&instance))
        return Effect{K::PitchShift, {e->octave()}};
    return std::nullopt;
}

// A sound's enabled effects in the order they run: its own, then its
// SoundGroup's, then that group's group's...; within each, higher Priority first.
std::vector<Effect> effectsOf(const Sound& sound)
{
    std::vector<Effect> effects;
    auto collect = [&](const Instance& holder) {
        std::vector<std::pair<int, Effect>> found;
        for (const InstancePtr& child : holder.children())
            if (const auto* e = dynamic_cast<const SoundEffect*>(child.get()); e && e->enabled())
                if (auto effect = effectOf(*e))
                    found.emplace_back(e->priority(), *effect);
        std::ranges::stable_sort(found, std::ranges::greater{}, &std::pair<int, Effect>::first);
        for (const auto& [priority, effect] : found)
            effects.push_back(effect);
    };
    collect(sound);
    for (const Instance* at = sound.soundGroup(); at && dynamic_cast<const SoundGroup*>(at); at = at->parent())
        collect(*at);
    return effects;
}

// research: LUNAR_SOUND_TRACE=1 logs every sound's start and stop, and why,
// on stderr as "soundtrace: <seconds> <what> <sound> ...".
const bool tracing = [] {
    const char* v = std::getenv("LUNAR_SOUND_TRACE");
    return v && *v && std::string_view(v) != "0";
}();
const double traceStart
    = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

template <class... Args>
void trace(const Sound& sound, std::string_view what, std::format_string<Args...> format, Args&&... args)
{
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::println(stderr, "soundtrace: {:.3f} {} {} {} {} | {}", now - traceStart, what,
                 static_cast<const void*>(&sound), sound.soundId(), sound.fullName(),
                 std::format(format, std::forward<Args>(args)...));
}

} // namespace

float SoundSystem::rollOff(Enum::RollOffMode mode, float distance, float minDistance, float maxDistance)
{
    minDistance = std::max(minDistance, 0.0f);
    maxDistance = std::max(maxDistance, minDistance);
    if (distance <= minDistance)
        return 1.0f;
    if (distance >= maxDistance)
        return 0.0f;
    const float inverse = minDistance > 0.0f ? minDistance / distance : 0.0f;
    const float linear = maxDistance > minDistance ? (maxDistance - distance) / (maxDistance - minDistance) : 0.0f;
    switch (mode) {
    case Enum::RollOffMode::Inverse:
        return inverse;
    case Enum::RollOffMode::Linear:
        return linear;
    case Enum::RollOffMode::LinearSquare:
        return linear * linear;
    case Enum::RollOffMode::InverseTapered:
        // Inverse near, LinearSquare towards the far end: whichever is quieter.
        return std::min(inverse, linear * linear);
    }
    return inverse;
}

SoundSystem::SoundSystem(DataModel& game, std::shared_ptr<assets::AssetCache> cache, bool output)
    : game_(game), cache_(std::move(cache)), mixer_(output)
{
    if (output && !mixer_.hasOutput())
        std::println(stderr, "sound: no output: {}", mixer_.outputError());
    for (const InstancePtr& d : game.descendants())
        if (auto* sound = dynamic_cast<Sound*>(d.get()))
            track(*sound);
    connections_.push_back(game.descendantAdded.connect([this](Instance* instance) {
        auto* sound = dynamic_cast<Sound*>(instance);
        if (!sound)
            return;
        if (tracing)
            trace(*sound, "added", "asked={}", sound->playing());
        // Back in as soon as it left: moved within the game, it plays on.
        const std::weak_ptr<Sound> self = std::static_pointer_cast<Sound>(sound->shared_from_this());
        std::erase_if(leaving_, [&](const Leaving& l) { return l.address == sound && sameOwner(l.sound, self); });
        track(*sound);
    }));
    connections_.push_back(game.descendantRemoving.connect([this](Instance* instance) {
        auto* sound = dynamic_cast<Sound*>(instance);
        if (!sound)
            return;
        if (tracing)
            if (const auto it = tracked_.find(sound); it != tracked_.end())
                trace(*sound, "removed", "asked={} heard={} at={:.3f} len={:.3f} looped={} playOnRemove={} done={}",
                      sound->playing(), it->second.playing, mixer_.state(it->second.voice).seconds,
                      sound->timeLength(), sound->looped(), sound->playOnRemove(),
                      mixer_.state(it->second.voice).ended > it->second.ended);
        // Whether it's leaving the game or only moving within it shows once
        // the move is done: DescendantAdded comes next for one that stays.
        Leaving l{sound, std::static_pointer_cast<Sound>(sound->shared_from_this())};
        // PlayOnRemove: played out once from the start, where it was, if it does leave.
        if (sound->playOnRemove() && audible(*sound)) {
            l.onRemove = find(sound->soundId(), true).clip;
            l.volume = std::clamp(sound->volume(), 0.0f, 10.0f) * groupVolume(*sound);
            l.speed = sound->playbackSpeed();
        }
        leaving_.push_back(std::move(l));
    }));
    for (int i = 0; i < loaderCount; ++i)
        loaders_.emplace_back([this, urgentOnly = i >= loaderCount - urgentOnlyLoaders](std::stop_token stop) {
            load(stop, urgentOnly);
        });
}

SoundSystem::~SoundSystem()
{
    for (Connection& c : connections_)
        c.disconnect();
    for (std::jthread& loader : loaders_)
        loader.request_stop();
    wake_.notify_all();
}

bool SoundSystem::sameOwner(const std::weak_ptr<Sound>& a, const std::weak_ptr<Sound>& b)
{
    return !a.owner_before(b) && !b.owner_before(a);
}

void SoundSystem::track(Sound& sound)
{
    std::weak_ptr<Sound> self = std::static_pointer_cast<Sound>(sound.shared_from_this());
    if (const auto it = tracked_.find(&sound); it != tracked_.end()) {
        if (sameOwner(it->second.sound, self))
            return;
        // Another sound where a removed one was: that one has gone.
        settle();
    }
    Tracked t;
    t.sound = std::move(self);
    t.voice = mixer_.add();
    tracked_.emplace(&sound, std::move(t));
}

void SoundSystem::settle()
{
    for (Leaving& l : leaving_) {
        if (const auto sound = l.sound.lock(); sound && sound->isDescendantOf(&game_))
            continue; // came back
        if (const auto it = tracked_.find(l.address); it != tracked_.end() && sameOwner(it->second.sound, l.sound)) {
            mixer_.remove(it->second.voice);
            tracked_.erase(it);
        }
        if (l.onRemove) {
            const Mixer::Voice voice = mixer_.add();
            const float volume = l.volume * master_;
            mixer_.set(voice, {l.onRemove, true, false, l.speed, volume, volume});
            oneShots_.emplace_back(voice, 0);
        }
    }
    leaving_.clear();
}

void SoundSystem::enqueue(const std::string& soundId, Loaded& entry, bool urgent)
{
    entry.queued = true;
    entry.urgent = entry.urgent || urgent;
    (urgent ? urgent_ : queue_).push_back(soundId);
    wake_.notify_all(); // not every loader takes every load
}

SoundSystem::Found SoundSystem::find(const std::string& soundId, bool samples)
{
    if (soundId.empty())
        return {};
    const std::scoped_lock lock(loadMutex_);
    const auto [it, fresh] = clips_.try_emplace(soundId);
    Loaded& entry = it->second;
    if (fresh) {
        enqueue(soundId, entry, samples);
    } else if (samples && !entry.done && !entry.urgent) {
        // Wanted now: ahead of the sounds that are only being got ready
        // (a rate-limited one, as soon as it may be asked again).
        entry.urgent = true;
        if (entry.queued)
            enqueue(soundId, entry, true);
    }
    if (samples)
        entry.lastUsed = now_;
    if (!entry.done || entry.failed)
        return {};
    if (samples) {
        if (!entry.clip && entry.file && !entry.decoding) {
            entry.decoding = true;
            enqueue(soundId, entry, true);
        }
    }
    return {entry.seconds, samples ? entry.clip : nullptr};
}

void SoundSystem::load(std::stop_token stop, bool urgentOnly)
{
    using Clock = std::chrono::steady_clock;
    for (;;) {
        std::string id;
        std::shared_ptr<const std::string> file;
        {
            std::unique_lock lock(loadMutex_);
            // The next to load: what's wanted now first. An id can be in
            // both queues; whichever loader comes to it first takes it.
            auto take = [&] {
                for (auto due = later_.begin(); due != later_.end() && due->first <= Clock::now();) {
                    enqueue(due->second, clips_[due->second], clips_[due->second].urgent);
                    due = later_.erase(due);
                }
                for (std::deque<std::string>* queue : {&urgent_, &queue_})
                    while (!queue->empty() && !(urgentOnly && queue == &queue_)) {
                        std::string next = std::move(queue->front());
                        queue->pop_front();
                        Loaded& entry = clips_[next];
                        if (!entry.queued)
                            continue;
                        entry.queued = false;
                        id = std::move(next);
                        file = entry.file; // set for a long one being decoded again
                        return true;
                    }
                return false;
            };
            const auto queued = [&] { return !urgent_.empty() || (!urgentOnly && !queue_.empty()); };
            while (!take()) {
                if (stop.stop_requested())
                    return;
                if (later_.empty())
                    wake_.wait(lock, stop, [&] { return queued() || !later_.empty(); });
                else
                    wake_.wait_until(lock, stop, later_.begin()->first, queued);
            }
        }
        const bool again = file != nullptr;
        const auto began = Clock::now();
        std::string error;
        if (!file) {
            if (auto bytes = cache_->fetch(id))
                file = *bytes;
            else
                error = bytes.error();
        }
        if (!file && assets::transientFailure(error)) {
            // Asked too often, Roblox answers a burst of a game's sounds
            // with 429s: asked again after a second, then two, four...
            // while the other loads go on.
            const std::scoped_lock lock(loadMutex_);
            Loaded& entry = clips_[id];
            if (entry.retries < maxRetries) {
                const int wait = std::min(1 << entry.retries, maxRetryWait);
                ++entry.retries;
                later_.emplace(Clock::now() + std::chrono::seconds(wait), id);
                wake_.notify_all();
                continue;
            }
        }
        std::shared_ptr<const Clip> loaded;
        if (file) {
            if (auto decoded = decodeAudio(*file))
                loaded = std::make_shared<const Clip>(std::move(*decoded));
            else
                error = decoded.error();
        }
        if (tracing)
            std::println(stderr, "soundtrace: load {} {:.3f}s again={} len={:.3f} {}", id,
                         std::chrono::duration<double>(Clock::now() - began).count(), again,
                         loaded ? loaded->seconds() : -1.0, error);
        // A few reasons, not hundreds.
        if (!loaded && ++failures_ <= 8)
            std::println(stderr, "sound: {}: {}", id, error);
        const std::scoped_lock lock(loadMutex_);
        Loaded& entry = clips_[id];
        entry.done = true;
        entry.failed = !loaded;
        entry.decoding = false;
        if (!loaded)
            continue;
        entry.seconds = loaded->seconds();
        const bool keep = loaded->samples.size() * sizeof(std::int16_t) <= keepBytes;
        entry.file = keep ? nullptr : file;
        // A long one stays decoded only if something wants it right now.
        if (keep || now_ - entry.lastUsed < 1.0)
            entry.clip = std::move(loaded);
    }
}

bool SoundSystem::audible(const Sound& sound) const
{
    // Sounds play wherever they are in the game, ReplicatedStorage too
    // [live: music can play from there], but in the
    // Starter containers, whose contents are only copied into players.
    const Instance* top = &sound;
    while (top->parent() && top->parent() != &game_)
        top = top->parent();
    if (!top->parent())
        return false;
    const std::string_view container = top->className();
    return container != "StarterGui" && container != "StarterPack" && container != "StarterPlayer";
}

void SoundSystem::update(const CFrame& listener)
{
    {
        // Long sounds nothing has played for a while give their samples back.
        const std::scoped_lock lock(loadMutex_);
        now_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for (auto& [id, entry] : clips_)
            if (entry.clip && entry.file && now_ - entry.lastUsed > keepSeconds)
                entry.clip.reset();
    }
    settle();
    // Events go out after the walk: their handlers may add or remove sounds.
    std::vector<std::function<void()>> events;
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        Tracked& t = it->second;
        const auto sound = t.sound.lock();
        if (!sound) {
            if (tracing && t.playing)
                std::println(stderr, "soundtrace: expired {}", static_cast<const void*>(it->first));
            mixer_.remove(t.voice);
            it = tracked_.erase(it);
            continue;
        }
        ++it;
        if (sound->soundId() != t.soundId) {
            if (tracing && t.playing)
                trace(*sound, "idchange", "from {}", t.soundId);
            t.soundId = sound->soundId();
            sound->assignLoaded(false, 0.0);
        }
        // Most of a game's sounds sit stopped: one that was and is, and has
        // loaded, has a silent voice already.
        if (t.idle && !sound->playing() && sound->isLoaded() && !tracing)
            continue;
        // Only what can be heard loads: the Starter containers' wait until they're copied out.
        const Found found = audible(*sound) ? find(t.soundId, sound->playing()) : Found{};
        const std::shared_ptr<const Clip>& c = found.clip;
        if (found.seconds && !sound->isLoaded()) {
            sound->assignLoaded(true, *found.seconds);
            events.emplace_back([sound, id = t.soundId] {
                const std::array args{Value(id)};
                sound->fireEvent("Loaded", args);
            });
        }

        const bool wanted = sound->playing() && c;
        const double position = sound->timePosition();
        if (tracing) {
            if (sound->playing() && !t.asked) {
                t.askedAt = now_;
                trace(*sound, "asked", "at={:.3f} loaded={} looped={} speed={}", position, found.seconds.has_value(),
                      sound->looped(), sound->playbackSpeed());
            } else if (!sound->playing() && t.asked)
                trace(*sound, "unasked", "heard={} at={:.3f} len={:.3f}", t.playing, mixer_.state(t.voice).seconds,
                      found.seconds.value_or(-1.0));
            if (wanted && !t.playing)
                trace(*sound, "start", "at={:.3f} len={:.3f} delay={:.3f}", position, c->seconds(), now_ - t.askedAt);
            else if (!wanted && t.playing && sound->playing())
                trace(*sound, "lostclip", "at={:.3f}", mixer_.state(t.voice).seconds);
            else if (wanted && std::abs(position - t.written) > 0.05)
                trace(*sound, "seek", "from={:.3f} to={:.3f}", t.written, position);
            t.asked = sound->playing();
        }
        if (wanted && (!t.playing || std::abs(position - t.written) > 0.05)) {
            t.seek = mixer_.seek(t.voice, position); // started, or moved by a script or the server
            t.written = position;
        }

        float volume = std::clamp(sound->volume(), 0.0f, 10.0f) * groupVolume(*sound) * master_;
        float left = volume;
        float right = volume;
        if (const std::optional<Vector3> at = emitterOf(*sound)) {
            const Vector3 local = listener.pointToObjectSpace(*at);
            const float distance = local.magnitude();
            volume *= rollOff(sound->rollOffMode(), distance, sound->rollOffMinDistance(), sound->rollOffMaxDistance());
            // Equal-ish power: full in the near ear, fading out of the far one.
            const float pan = distance > 0.01f ? std::clamp(local.x / distance, -1.0f, 1.0f) : 0.0f;
            left = volume * std::min(1.0f, std::sqrt(1.0f - pan));
            right = volume * std::min(1.0f, std::sqrt(1.0f + pan));
        }
        mixer_.set(t.voice, {c, wanted, sound->looped(), std::max(sound->playbackSpeed(), 0.0f), left, right,
                             wanted ? effectsOf(*sound) : std::vector<Effect>{}});
        t.idle = !wanted;

        const Mixer::State state = mixer_.state(t.voice);
        if (state.loops > t.loops) {
            t.loops = state.loops;
            events.emplace_back([sound, id = t.soundId, loops = state.loops] {
                const std::array args{Value(id), Value(static_cast<double>(loops))};
                sound->fireEvent("DidLoop", args);
            });
        }
        // An end from before the last seek is the play that seek replaced
        // (Play() again just as it ended): the new one goes on.
        const bool ended = state.ended > t.ended && state.endedAfter == t.seek;
        t.ended = state.ended;
        if (ended) {
            if (tracing)
                trace(*sound, "ended", "asked={} len={:.3f}", sound->playing(), c ? c->seconds() : -1.0);
            if (sound->playing()) {
                sound->setPlaying(false);
                sound->setTimePosition(0.0);
                t.written = 0.0;
                events.emplace_back([sound, id = t.soundId] {
                    const std::array args{Value(id)};
                    sound->fireEvent("Ended", args);
                });
            }
            t.playing = false;
            t.asked = false;
            continue;
        }
        if (wanted && state.playing) {
            sound->setTimePosition(state.seconds);
            t.written = state.seconds;
        }
        t.playing = wanted;
    }

    std::erase_if(oneShots_, [&](const auto& shot) {
        if (mixer_.state(shot.first).ended == 0)
            return false;
        mixer_.remove(shot.first);
        return true;
    });
    for (const auto& fire : events)
        fire();
}

SoundSystem::Stats SoundSystem::stats() const
{
    Stats s;
    s.sounds = tracked_.size();
    for (const auto& [address, t] : tracked_)
        s.playing += t.playing ? 1u : 0u;
    const std::scoped_lock lock(loadMutex_);
    for (const auto& [id, loaded] : clips_) {
        s.clips += loaded.clip ? 1u : 0u;
        s.bytes += loaded.clip ? loaded.clip->samples.size() * sizeof(std::int16_t) : 0;
        s.failed += loaded.failed ? 1u : 0u;
    }
    return s;
}

} // namespace lunar::audio
