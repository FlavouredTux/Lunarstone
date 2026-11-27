// Scripts' animations: LoadAnimation on a Humanoid, Animator or
// AnimationController hands back an AnimationTrack instance played by a
// native avatar::AnimationTrack on the rig's avatar::Animator, the same
// machinery the native Animate controller uses. Fades, priority layers,
// looping and keyframe/marker events are the avatar library's (from the
// 0.742 client); this file passes them through to Luau.
//
// The rig is the Model holding the Humanoid or AnimationController, so
// characters and NPCs alike animate. Clips load by AnimationId through the
// system's ClipLoader, in the background: until one arrives its track has
// Length 0 and plays nothing, as in the client.

#include "Characters.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/avatar/AnimationSystem.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::lua {

// What Shared::animation holds.
struct AnimationBridge {
    avatar::AnimationSystem* external = nullptr; // the embedder's, which it steps
    std::unique_ptr<avatar::AnimationSystem> own;

    // The script instance of every native track scripts have seen, so
    // GetPlayingAnimationTracks hands back the object LoadAnimation did.
    struct Known {
        std::weak_ptr<avatar::AnimationTrack> native;
        std::weak_ptr<AnimationTrack> instance;
        std::weak_ptr<Animation> animation;
        std::weak_ptr<Instance> animator;
        // The Animation made for a track no script loaded (replicated, or our
        // Animate's): scripts read track.Animation.AnimationId (the game does).
        std::shared_ptr<Animation> made;
    };
    std::unordered_map<const avatar::AnimationTrack*, Known> known;
    struct Emote {
        std::weak_ptr<Humanoid> owner;
        std::shared_ptr<AnimationTrack> track;
    };
    std::unordered_map<const Humanoid*, Emote> emotes;

    avatar::AnimationSystem& system()
    {
        if (external)
            return *external;
        if (!own)
            own = std::make_unique<avatar::AnimationSystem>();
        return *own;
    }
};

namespace {

AnimationBridge& bridge(Shared& shared)
{
    if (!shared.animation)
        shared.animation = std::make_shared<AnimationBridge>();
    return *shared.animation;
}

// One native track behind a script AnimationTrack. Its signals come out as
// the instance's events. When the instance goes (nothing in Luau holds it
// any more) a track the scripts loaded is released, finishing a fade it's
// in; one they only saw (GetPlayingAnimationTracks of the native Animate
// controller's) is left to its owner.
class ScriptPlayback final : public AnimationPlayback {
public:
    ScriptPlayback(avatar::TrackPtr track, const std::shared_ptr<AnimationTrack>& instance, bool owned)
        : track_(std::move(track)), owned_(owned)
    {
        std::weak_ptr<AnimationTrack> weak = instance;
        auto relay = [weak](std::string_view event, std::vector<Value> args) {
            if (auto owner = weak.lock())
                owner->fireEvent(event, args);
        };
        connections_.push_back(track_->keyframeReached.connect(
            [relay](const std::string& name) { relay("KeyframeReached", {Value(name)}); }));
        connections_.push_back(track_->didLoop.connect([relay] { relay("DidLoop", {}); }));
        connections_.push_back(track_->stopped.connect([relay] { relay("Stopped", {}); }));
        connections_.push_back(track_->ended.connect([relay] { relay("Ended", {}); }));
        instance_ = std::move(weak);
    }
    ~ScriptPlayback() override
    {
        for (Connection& connection : connections_)
            connection.disconnect();
        if (owned_)
            track_->destroy();
    }
    ScriptPlayback(const ScriptPlayback&) = delete;
    ScriptPlayback& operator=(const ScriptPlayback&) = delete;

    float length() const override { return track_->length(); }
    bool isPlaying() const override { return track_->isPlaying(); }
    bool looped() const override { return track_->looped(); }
    void setLooped(bool looped) override { track_->setLooped(looped); }
    Enum::AnimationPriority priority() const override { return track_->priority(); }
    void setPriority(Enum::AnimationPriority priority) override { track_->setPriority(priority); }
    float speed() const override { return track_->speed(); }
    float timePosition() const override { return track_->timePosition(); }
    void setTimePosition(float time) override { track_->setTimePosition(time); }
    float weightCurrent() const override { return track_->weightCurrent(); }
    float weightTarget() const override { return track_->weightTarget(); }

    avatar::AnimationTrack& track() const { return *track_; }

    // GetMarkerReachedSignal(name): the hub event "MarkerReached:<name>",
    // fed from the native marker signal the first time anyone asks.
    std::string markerEvent(std::string_view name)
    {
        std::string event = std::format("MarkerReached:{}", name);
        if (markers_.insert(std::string(name)).second) {
            std::weak_ptr<AnimationTrack> weak = instance_;
            connections_.push_back(track_->markerReachedSignal(name).connect([weak, event](const std::string& value) {
                if (auto owner = weak.lock())
                    owner->fireEvent(event, std::array{Value(value)});
            }));
        }
        return event;
    }

private:
    avatar::TrackPtr track_;
    bool owned_ = false;
    std::weak_ptr<AnimationTrack> instance_;
    std::vector<Connection> connections_;
    std::set<std::string, std::less<>> markers_;
};

ScriptPlayback* playbackOf(const AnimationTrack& track)
{
    return dynamic_cast<ScriptPlayback*>(track.playback().get());
}

// The Animator a LoadAnimation call on `owner` uses: the Animator itself,
// or the one under a Humanoid or AnimationController.
//
// GUESS: Humanoid:LoadAnimation and AnimationController:LoadAnimation make
// the Animator when there's none (the documented behaviour; the binary's
// deprecated forwarders weren't traced).
Instance* animatorOf(Instance& owner, bool create)
{
    if (owner.isA("Animator"))
        return &owner;
    if (Instance* animator = owner.findFirstChildOfClass("Animator"))
        return animator;
    if (!create)
        return nullptr;
    auto animator = std::make_shared<Animator>();
    animator->setParent(&owner);
    return animator.get();
}

// The rig an Animator moves: the Model holding its Humanoid or AnimationController.
std::shared_ptr<Model> rigOf(const Instance& animator)
{
    const Instance* holder = animator.parent();
    Instance* rig = holder ? holder->parent() : nullptr;
    auto* model = dynamic_cast<Model*>(rig);
    return model ? std::static_pointer_cast<Model>(model->shared_from_this()) : nullptr;
}

// The script instance for a native track, made (and remembered) if scripts
// haven't seen it yet. `loaded`: the scripts' own LoadAnimation made it.
std::shared_ptr<AnimationTrack> instanceFor(AnimationBridge& bridge, const avatar::TrackPtr& native,
                                            Animation* animation, Instance* animator, bool loaded)
{
    std::shared_ptr<Animation> keepAnimation;
    InstancePtr keepAnimator;
    if (auto found = bridge.known.find(native.get()); found != bridge.known.end()) {
        if (found->second.native.lock() == native) {
            if (auto existing = found->second.instance.lock())
                return existing;
            // Scripts let go of it while it kept playing: a new instance for the same track.
            keepAnimation = found->second.animation.lock();
            keepAnimator = found->second.animator.lock();
            if (!animation)
                animation = keepAnimation.get();
            if (!animator)
                animator = keepAnimator.get();
        }
    }
    if (bridge.known.size() > 256 && bridge.known.size() % 256 == 0)
        std::erase_if(bridge.known, [](const auto& item) { return item.second.native.expired(); });
    std::shared_ptr<Animation> made;
    if (!animation) {
        // GUESS: the client's Animation for such a track is named like it,
        // with its id, and parented nowhere.
        made = std::make_shared<Animation>();
        made->setName(native->name());
        made->setAnimationId(native->animationId());
        animation = made.get();
    }
    auto instance = std::make_shared<AnimationTrack>();
    instance->setName(animation->name());
    instance->bind(std::make_shared<ScriptPlayback>(native, instance, loaded), animation);
    // AnimationTrack:Destroy() releases the native track right away (it
    // still finishes a fade-out), not only once Luau collects the instance.
    if (loaded)
        instance->destroying.connect([weak = std::weak_ptr<avatar::AnimationTrack>(native)] {
            if (auto track = weak.lock())
                track->destroy();
        });
    auto& entry = bridge.known[native.get()];
    entry.native = native;
    entry.instance = instance;
    entry.animation
        = animation ? std::static_pointer_cast<Animation>(animation->shared_from_this()) : std::weak_ptr<Animation>();
    if (animator)
        entry.animator = animator->shared_from_this();
    if (made)
        entry.made = std::move(made);
    return instance;
}

// --- LoadAnimation ------------------------------------------------------------------------

// Humanoid/Animator/AnimationController:LoadAnimation(animation). Messages
// from the client's Animator::LoadAnimation (0x10341c6b8).
int loadAnimation(lua_State* L)
{
    Instance& owner = self(L);
    auto* animation = dynamic_cast<Animation*>(toInstance(L, 2));
    if (!animation)
        raise(L, "LoadAnimation requires an Animation object");
    if (animation->animationId().empty())
        raise(L, "LoadAnimation requires the asset id to not be empty");
    Instance* animator = guard(L, [&] { return animatorOf(owner, true); });
    AnimationBridge& b = bridge(Runtime::of(L).shared());
    std::shared_ptr<Model> rig = rigOf(*animator);
    if (!rig) {
        // Nothing to move: a track that never loads.
        auto track = std::make_shared<AnimationTrack>();
        track->setName(animation->name());
        track->bind(nullptr, animation);
        return returnInstance(L, track.get());
    }
    avatar::AnimationSystem& system = b.system();
    avatar::CharacterAnimation& entry = system.add(rig, false);
    avatar::TrackPtr native
        = entry.animator->loadAnimation(system.clips().source(animation->animationId()), animation->name());
    native->setAnimationId(animation->animationId());
    std::shared_ptr<AnimationTrack> track = instanceFor(b, native, animation, animator, true);
    return returnInstance(L, track.get());
}

bool canEmote(const Humanoid& humanoid)
{
    const auto state = humanoid.getState();
    return !humanoid.destroyed() && humanoid.health() > 0 && humanoid.moveDirection().magnitude() < 0.01f
           && !humanoid.sit()
           && (state == Enum::HumanoidStateType::None || state == Enum::HumanoidStateType::Running
               || state == Enum::HumanoidStateType::Landed);
}

using EmoteResult = std::expected<bool, std::string>;
using EmoteReply = std::function<void(EmoteResult)>;

void finishEmote(Runtime& rt, std::weak_ptr<Humanoid> weak, std::string id, std::string name, EmoteReply done,
                 std::function<bool()> active = {})
{
    if (active && !active())
        return;
    auto humanoid = weak.lock();
    if (!humanoid || !canEmote(*humanoid)) {
        done(false);
        return;
    }
    AnimationBridge& b = bridge(rt.shared());
    auto clip = b.system().clips().poll(id);
    if (!clip) {
        done(std::unexpected("PlayEmoteAsync could not load the emote: " + clip.error()));
        return;
    }
    if (!*clip) {
        rt.after(0.0, [&rt, weak, id = std::move(id), name = std::move(name), done = std::move(done),
                       active = std::move(active)] mutable {
            finishEmote(rt, weak, std::move(id), std::move(name), std::move(done), std::move(active));
        });
        return;
    }
    Instance* animator = animatorOf(*humanoid, true);
    auto rig = rigOf(*animator);
    if (!rig) {
        done(std::unexpected("PlayEmoteAsync requires a Humanoid in a character"));
        return;
    }
    if (auto previous = b.emotes.find(humanoid.get()); previous != b.emotes.end()) {
        if (auto* playback = playbackOf(*previous->second.track))
            playback->track().stop(0.1f);
        b.emotes.erase(previous);
    }
    auto animation = std::make_shared<Animation>();
    animation->setName(name);
    animation->setAnimationId(id);
    auto& entry = b.system().add(rig, false);
    auto native = entry.animator->loadAnimation(b.system().clips().source(id), name);
    native->setAnimationId(id);
    native->setPriority(Enum::AnimationPriority::Action);
    auto track = instanceFor(b, native, animation.get(), animator, true);
    b.known[native.get()].made = animation;
    b.emotes.insert_or_assign(humanoid.get(), AnimationBridge::Emote{humanoid, track});
    native->play(0.1f);
    if (!native->isPlaying()) {
        b.emotes.erase(humanoid.get());
        done(false);
        return;
    }
    const std::array args{Value(track)};
    animator->fireEvent("AnimationPlayed", args);
    humanoid->fireEvent("AnimationPlayed", args);
    done(true);
}

int playEmote(lua_State* L)
{
    auto& humanoid = static_cast<Humanoid&>(self(L));
    const std::string name(checkString(L, 2));
    const auto* description = instanceCast<HumanoidDescription>(humanoid.findFirstChildOfClass("HumanoidDescription"));
    const auto* ids = description ? &description->emotes() : nullptr;
    if (!ids || !ids->contains(name) || ids->at(name).empty())
        raise(L, "No emote named '" + name + "' in the HumanoidDescription");
    if (!canEmote(humanoid)) {
        lua_pushboolean(L, false);
        return 1;
    }
    checkYieldable(L);
    const auto& choices = ids->at(name);
    const auto choice = std::next(choices.begin(), static_cast<std::ptrdiff_t>(scriptEntropy() % choices.size()));
    const std::string id = "rbxassetid://" + std::to_string(*choice);
    Runtime& rt = Runtime::of(L);
    bridge(rt.shared()).system().clips().source(id);
    const auto token = rt.suspend(L);
    rt.after(0.0, [&rt, L, token,
                   weak = std::weak_ptr<Humanoid>(std::static_pointer_cast<Humanoid>(humanoid.shared_from_this())), id,
                   name] {
        finishEmote(
            rt, weak, id, name,
            [&rt, L, token](EmoteResult result) {
                if (!rt.isSuspended(L, token))
                    return;
                if (!result)
                    rt.fail(L, token, result.error());
                else
                    rt.wake(
                        L, token,
                        [played = *result](lua_State* thread) {
                            lua_pushboolean(thread, played);
                            return 1;
                        },
                        "Humanoid:PlayEmoteAsync");
            },
            [&rt, L, token] { return rt.isSuspended(L, token); });
    });
    return lua_yield(L, 0);
}

// Animator/Humanoid/AnimationController:GetPlayingAnimationTracks(), in load order.
int getPlayingAnimationTracks(lua_State* L)
{
    Instance& owner = self(L);
    Instance* animator = animatorOf(owner, false);
    std::shared_ptr<Model> rig = animator ? rigOf(*animator) : nullptr;
    std::vector<InstancePtr> tracks;
    if (rig) {
        AnimationBridge& b = bridge(Runtime::of(L).shared());
        if (avatar::CharacterAnimation* entry = b.system().find(*rig))
            for (const avatar::TrackPtr& native : entry->animator->playingTracks())
                tracks.push_back(instanceFor(b, native, nullptr, animator, false));
    }
    return pushInstances(L, tracks);
}

// --- AnimationTrack -----------------------------------------------------------------------

AnimationTrack& selfTrack(lua_State* L)
{
    auto* track = dynamic_cast<AnimationTrack*>(&self(L));
    if (!track)
        expectedColon(L);
    return *track;
}

float optFloat(lua_State* L, int idx, double fallback)
{
    return static_cast<float>(luaL_optnumber(L, idx, fallback));
}

// Play(fadeTime = 0.1, weight = 1, speed = 1). Animator.AnimationPlayed
// (and the deprecated Humanoid/AnimationController one) fires with the
// track once it plays.
//
// GUESS: AnimationPlayed fires on every Play that starts the track, after
// the track limit check (0x10341be92 runs first in Play 0x103411ea0); the
// firing site itself wasn't traced.
int trackPlay(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    const float fadeTime = optFloat(L, 2, 0.1);
    const float weight = optFloat(L, 3, 1.0);
    const float speed = optFloat(L, 4, 1.0);
    ScriptPlayback* playback = playbackOf(track);
    if (!playback)
        return 0;
    playback->track().play(fadeTime, weight, speed);
    if (!playback->track().isPlaying())
        return 0;
    AnimationBridge& b = bridge(Runtime::of(L).shared());
    auto found = b.known.find(&playback->track());
    if (found == b.known.end())
        return 0;
    if (auto animator = found->second.animator.lock()) {
        const std::array args{Value(track.shared_from_this())};
        animator->fireEvent("AnimationPlayed", args);
        if (Instance* holder = animator->parent();
            holder && (holder->isA("Humanoid") || holder->isA("AnimationController")))
            holder->fireEvent("AnimationPlayed", args);
    }
    return 0;
}

// Stop(fadeTime = 0.1): IsPlaying turns false and Stopped fires at once,
// Ended once it has faded out.
int trackStop(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    if (ScriptPlayback* playback = playbackOf(track))
        playback->track().stop(optFloat(L, 2, 0.1));
    return 0;
}

// AdjustSpeed(speed = 1). (The Animate script passes a second argument; the
// client takes only the speed.)
int trackAdjustSpeed(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    if (ScriptPlayback* playback = playbackOf(track)) {
        const float before = playback->track().speed();
        playback->track().adjustSpeed(optFloat(L, 2, 1.0));
        if (before != playback->track().speed())
            track.changed.fire("Speed");
    }
    return 0;
}

// AdjustWeight(weight = 1, fadeTime = 0.1); ignored unless playing.
int trackAdjustWeight(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    if (ScriptPlayback* playback = playbackOf(track))
        playback->track().adjustWeight(optFloat(L, 2, 1.0), optFloat(L, 3, 0.1));
    return 0;
}

int trackMarkerSignal(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    std::string_view name = checkString(L, 2);
    if (ScriptPlayback* playback = playbackOf(track))
        pushHubSignal(L, track, playback->markerEvent(name));
    else
        pushHubSignal(L, track, std::format("MarkerReached:{}", name));
    return 1;
}

// GetTimeOfKeyframe(name): the time of the first keyframe so named.
//
// GUESS: the messages. The client errors when the animation hasn't loaded
// or has no such keyframe (documented); its wording wasn't found.
int trackTimeOfKeyframe(lua_State* L)
{
    AnimationTrack& track = selfTrack(L);
    std::string_view name = checkString(L, 2);
    ScriptPlayback* playback = playbackOf(track);
    avatar::ClipPtr clip = playback ? playback->track().clip() : nullptr;
    if (!clip)
        raise(L, "GetTimeOfKeyframe: the animation has not loaded yet");
    for (const avatar::ClipEvent& event : clip->events)
        if (!event.marker && event.name == name) {
            lua_pushnumber(L, static_cast<double>(event.time));
            return 1;
        }
    raise(L, std::format("GetTimeOfKeyframe: could not find a keyframe named '{}'", name));
}

constexpr auto methods = std::to_array<Method>({
    {"PlayEmote", "Humanoid", playEmote},
    {"PlayEmoteAsync", "Humanoid", playEmote},
    {"LoadAnimation", "Animator", loadAnimation},
    {"LoadAnimation", "Humanoid", loadAnimation},
    {"LoadAnimation", "AnimationController", loadAnimation},
    {"GetPlayingAnimationTracks", "Animator", getPlayingAnimationTracks},
    {"GetPlayingAnimationTracks", "Humanoid", getPlayingAnimationTracks},
    {"GetPlayingAnimationTracks", "AnimationController", getPlayingAnimationTracks},
    {"Play", "AnimationTrack", trackPlay},
    {"Stop", "AnimationTrack", trackStop},
    {"AdjustSpeed", "AnimationTrack", trackAdjustSpeed},
    {"AdjustWeight", "AnimationTrack", trackAdjustWeight},
    {"GetMarkerReachedSignal", "AnimationTrack", trackMarkerSignal},
    {"GetTimeOfKeyframe", "AnimationTrack", trackTimeOfKeyframe},
});

} // namespace

std::span<const Method> animationMethods()
{
    return methods;
}

void setAnimationSystem(Shared& shared, avatar::AnimationSystem* system)
{
    AnimationBridge& b = bridge(shared);
    b.emotes.clear();
    b.external = system;
    b.known.clear();
}

void stepOwnAnimation(Shared& shared, double dt)
{
    if (shared.animation) {
        std::erase_if(shared.animation->emotes, [](auto& entry) {
            const auto humanoid = entry.second.owner.lock();
            auto* playback = playbackOf(*entry.second.track);
            if (!playback)
                return true;
            auto& track = playback->track();
            if ((!humanoid || !canEmote(*humanoid)) && track.isPlaying())
                track.stop(0.1f);
            return !track.isPlaying() && track.weightCurrent() < 0.001f;
        });
    }
    if (shared.animation && !shared.animation->external && shared.animation->own)
        shared.animation->own->step(static_cast<float>(dt));
}

} // namespace lunar::lua

namespace lunar {

void ScriptContext::playChatEmote(std::string name, std::function<void(std::expected<void, std::string>)> done)
{
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto* players = game().getService<Players>();
    auto* local = players->localPlayer();
    auto* character = local ? local->character() : nullptr;
    auto* humanoid = character ? instanceCast<Humanoid>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (!humanoid || !lua::canEmote(*humanoid)) {
        done(std::unexpected("You cannot play an emote right now."));
        return;
    }
    constexpr std::array legacy{"wave", "point", "dance", "dance1", "dance2", "dance3", "laugh", "cheer"};
    if (std::ranges::find(legacy, name) != legacy.end()) {
        if (name == "dance" && humanoid->rigType() == Enum::HumanoidRigType::R6)
            name += std::to_string(1 + lua::scriptEntropy() % 3);
        auto* animate = character->findFirstChild("Animate");
        auto* play = animate ? animate->findFirstChild("PlayEmote") : nullptr;
        if (play && play->isA("BindableFunction"))
            if (auto callback = runtime().shared().callback(*play, "OnInvoke")) {
                lua::invokeCallback(*callback, {lua::packed(Value(name))},
                                    [done = std::move(done)](bool ok, lua::PackedArgs) {
                                        done(ok ? std::expected<void, std::string>{}
                                                : std::unexpected("This character could not play that emote."));
                                    });
                return;
            }
        auto* entry = lua::bridge(runtime().shared()).system().find(*character);
        if (!entry || !entry->animate || !entry->animate->playEmote(name))
            done(std::unexpected("This character does not support that emote."));
        else
            done({});
        return;
    }
    if (!game().getService<StarterPlayer>()->userEmotesEnabled()) {
        done(std::unexpected("Avatar emotes are disabled in this experience."));
        return;
    }
    if (humanoid->rigType() != Enum::HumanoidRigType::R15) {
        done(std::unexpected("Avatar emotes require an R15 character."));
        return;
    }
    const auto* description = instanceCast<HumanoidDescription>(humanoid->findFirstChildOfClass("HumanoidDescription"));
    std::string selected;
    int slot = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), slot);
    if (error == std::errc{} && end == name.data() + name.size()) {
        if (description && description->equippedEmotes().contains(slot))
            selected = description->equippedEmotes().at(slot);
    } else if (description)
        for (const auto& [candidate, ids] : description->emotes()) {
            std::string lower = candidate;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == name) {
                selected = candidate;
                break;
            }
        }
    if (selected.empty() || !description || !description->emotes().contains(selected)
        || description->emotes().at(selected).empty()) {
        done(std::unexpected("That emote is not available in your avatar's emotes."));
        return;
    }
    const auto& choices = description->emotes().at(selected);
    const auto choice = std::next(choices.begin(), static_cast<std::ptrdiff_t>(lua::scriptEntropy() % choices.size()));
    const std::string id = "rbxassetid://" + std::to_string(*choice);
    lua::bridge(runtime().shared()).system().clips().source(id);
    lua::finishEmote(runtime(), std::static_pointer_cast<Humanoid>(humanoid->shared_from_this()), id, selected,
                     [done = std::move(done)](lua::EmoteResult result) {
                         if (!result)
                             done(std::unexpected(result.error()));
                         else if (!*result)
                             done(std::unexpected("You cannot play an emote right now."));
                         else
                             done({});
                     });
}

} // namespace lunar
