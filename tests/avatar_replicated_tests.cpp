#include "framework.hpp"

// Other players' tracks as their Animators replicate them
// (Animator.OnCombinedUpdate), played on our copy of the rig.

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/Quaternion.hpp>
#include <lunar/avatar/ReplicatedAnimation.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <utility>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

ClipPtr holdClip(float angle, float length = 1.0f, bool loop = true)
{
    auto clip = std::make_shared<AnimationClipData>();
    clip->length = length;
    clip->loop = loop;
    clip->priority = Enum::AnimationPriority::Core;
    JointCurve curve{"UpperTorso", "LowerTorso", {}};
    curve.keys.push_back({0.0f, {}, {angle, 0.0f, 0.0f}});
    clip->joints.push_back(std::move(curve));
    return clip;
}

// Someone else's character: LowerTorso and UpperTorso joined by a Waist
// Motor6D, a Humanoid with an Animator. Clips are inserted by hand.
struct Fixture {
    AnimationSystem system{[] {
        assets::AssetConfig config;
        config.download = false;
        return std::make_shared<assets::AssetCache>(config);
    }()};
    ReplicatedAnimation replicated{system};
    std::shared_ptr<Model> rig = std::make_shared<Model>();
    std::shared_ptr<Motor6D> waist = std::make_shared<Motor6D>();
    std::shared_ptr<lunar::Animator> animator = std::make_shared<lunar::Animator>();

    Fixture()
    {
        auto lower = std::make_shared<Part>();
        lower->setName("LowerTorso");
        lower->setParent(rig.get());
        auto upper = std::make_shared<Part>();
        upper->setName("UpperTorso");
        upper->setParent(rig.get());
        waist->setName("Waist");
        waist->setPart0(lower.get());
        waist->setPart1(upper.get());
        waist->setParent(upper.get());
        rig->setPrimaryPart(lower.get());
        auto humanoid = std::make_shared<Humanoid>();
        humanoid->setParent(rig.get());
        animator->setParent(humanoid.get());
        system.clips().insert("rbxassetid://507766388", holdClip(0.5f));
        system.clips().insert("rbxassetid://913402848", holdClip(1.0f));
    }

    float waistAngle() const { return toRotationVector(waist->transform()).x; }
};

// An update as a live server sends it: the walk starting, new to the sender.
TrackUpdate walkStarts()
{
    TrackUpdate u;
    u.animationId = "http://www.roblox.com/asset/?id=913402848";
    u.playing = true;
    u.fadeTime = 0.0f;
    u.weight = 1.0f;
    u.speed = 1.25f;
    u.looped = true;
    u.changed = TrackUpdate::PlayState | TrackUpdate::Speed | TrackUpdate::Looping | TrackUpdate::Priority;
    return u;
}

} // namespace

TEST(avatar_replicated_update_from_event_arguments)
{
    // The shapes the network decodes OnCombinedUpdate into [capture].
    const std::array<Value, 9> args{Value(std::string("http://www.roblox.com/asset/?id=1826752440")),
                                    Value(true),
                                    Value(0.1),
                                    Value(1.0),
                                    Value(1.056),
                                    Value(0.013),
                                    Value(1000.0),
                                    Value(true),
                                    Value(85.0)};
    auto update = TrackUpdate::fromArguments(args);
    CHECK(update.has_value());
    CHECK_EQ(update->animationId, "http://www.roblox.com/asset/?id=1826752440");
    CHECK(update->playing && update->looped);
    CHECK(near(update->fadeTime, 0.1f) && near(update->weight, 1.0f) && near(update->speed, 1.056f));
    CHECK(near(update->timePosition, 0.013f));
    CHECK(update->priority == Enum::AnimationPriority::Core);
    CHECK_EQ(update->changed, TrackUpdate::PlayState | TrackUpdate::Speed | TrackUpdate::Looping
                                  | TrackUpdate::Priority);
    // Too few, or the wrong kinds.
    CHECK(!TrackUpdate::fromArguments(std::span(args).first(8)));
    auto wrong = args;
    wrong[1] = Value(1.0);
    CHECK(!TrackUpdate::fromArguments(wrong));
}

TEST(avatar_replicated_tracks_play_adjust_and_stop)
{
    Fixture f;
    f.replicated.apply(*f.animator, walkStarts());
    TrackPtr walk = f.replicated.track(*f.animator, "rbxassetid://913402848");
    CHECK(walk && walk->isPlaying());
    CHECK(near(walk->speed(), 1.25f));
    CHECK(walk->looped());
    f.system.step(1.0f / 60.0f);
    CHECK(near(f.waistAngle(), 1.0f));

    // Only what is flagged changes: an unflagged Looping reads false on the
    // wire and leaves the track looping.
    TrackUpdate faster = walkStarts();
    faster.changed = TrackUpdate::Speed;
    faster.speed = 0.5f;
    faster.looped = false;
    faster.timePosition = 0.4f;
    const float before = walk->timePosition();
    f.replicated.apply(*f.animator, faster);
    CHECK(near(walk->speed(), 0.5f));
    CHECK(walk->looped());
    CHECK(near(walk->timePosition(), before)); // still playing: our own clock

    TrackUpdate lighter = walkStarts();
    lighter.changed = TrackUpdate::Weight;
    lighter.weight = 0.25f;
    lighter.fadeTime = 0.0f;
    f.replicated.apply(*f.animator, lighter);
    CHECK(near(walk->weightTarget(), 0.25f));

    TrackUpdate stops = walkStarts();
    stops.playing = false;
    stops.weight = 0.0f;
    stops.fadeTime = 0.2f;
    stops.changed = TrackUpdate::PlayState;
    f.replicated.apply(*f.animator, stops);
    CHECK(!walk->isPlaying());
    f.system.step(0.25f);
    CHECK(!walk->isActive());
    CHECK(near(f.waistAngle(), 0.0f));
}

// On the character this client owns, an update for a track our own scripts
// are playing is the server relaying it (any spelling of the id); one for
// anything else is the server's own (the game's moves) and plays.
TEST(avatar_replicated_tells_relayed_tracks_from_the_servers)
{
    Fixture f;
    CharacterAnimation& entry = f.system.add(f.rig, false);
    TrackPtr ours = entry.animator->loadAnimation(holdClip(0.5f), "Dash");
    ours->setAnimationId("rbxassetid://913402848");
    CHECK(!f.replicated.playsLocally(*f.animator, "http://www.roblox.com/asset/?id=913402848"));
    ours->play(0.0f);
    CHECK(f.replicated.playsLocally(*f.animator, "http://www.roblox.com/asset/?id=913402848"));
    CHECK(!f.replicated.playsLocally(*f.animator, "rbxassetid://507766388"));
    // A track an update made doesn't count as ours.
    ours->stop(0.0f);
    f.replicated.apply(*f.animator, walkStarts());
    CHECK(!f.replicated.playsLocally(*f.animator, "rbxassetid://913402848"));
}

// a temporary scene prop, as a live server sent it for the caster's Animator
// [capture: 4.87 s]: the move's track announced, then
// played, then a second track of the same animation announced, not playing.
// That announcement is another track: the first one plays on (the effect
// script checks IsPlaying before it builds the rocks).
TEST(avatar_replicated_another_track_of_a_playing_animation_leaves_it_playing)
{
    Fixture f;
    TrackUpdate announced;
    announced.animationId = "rbxassetid://913402848";
    announced.playing = false;
    announced.fadeTime = 0.0f;
    announced.weight = 0.0f;
    announced.priority = Enum::AnimationPriority::Action;
    announced.changed = TrackUpdate::Looping | TrackUpdate::Priority;
    f.replicated.apply(*f.animator, announced);
    TrackPtr move = f.replicated.track(*f.animator, "rbxassetid://913402848");
    CHECK(move && !move->isPlaying());

    TrackUpdate plays = announced;
    plays.playing = true;
    plays.fadeTime = 0.1f;
    plays.weight = 1.0f;
    plays.changed = TrackUpdate::PlayState;
    f.replicated.apply(*f.animator, plays);
    CHECK(move->isPlaying());

    f.replicated.apply(*f.animator, announced); // the second track
    CHECK(move->isPlaying());
    f.system.step(0.2f);
    CHECK(move->isPlaying());
    CHECK(near(f.waistAngle(), 1.0f));

    // A flagged stop still stops it.
    TrackUpdate stops = announced;
    stops.fadeTime = 0.1f;
    stops.changed = TrackUpdate::PlayState;
    f.replicated.apply(*f.animator, stops);
    CHECK(!move->isPlaying());
}

TEST(avatar_replicated_track_joined_late_starts_where_the_sender_is)
{
    // We missed the update that started it: the next one still says it plays.
    Fixture f;
    TrackUpdate idle;
    idle.animationId = "rbxassetid://507766388";
    idle.playing = true;
    idle.fadeTime = 0.0f;
    idle.timePosition = 0.6f;
    idle.changed = TrackUpdate::Speed;
    f.replicated.apply(*f.animator, idle);
    TrackPtr track = f.replicated.track(*f.animator, "http://www.roblox.com/asset/?id=507766388");
    CHECK(track && track->isPlaying());
    CHECK(near(track->timePosition(), 0.6f));
    f.system.step(1.0f / 60.0f);
    CHECK(near(f.waistAngle(), 0.5f));

    // An Animator outside a rig has nothing to move.
    auto loose = std::make_shared<lunar::Animator>();
    f.replicated.apply(*loose, walkStarts());
    CHECK(!f.replicated.track(*loose, "rbxassetid://913402848"));
}

TEST(avatar_track_sender_sends_what_the_genuine_client_does)
{
    // Our own Animator, as Animate drives it: two tracks loaded, the idle
    // played; then the walk starts, its speed changes, the idle stops.
    Fixture f;
    CharacterAnimation& entry = f.system.add(f.rig, false);
    TrackPtr idle = entry.animator->loadAnimation(f.system.clips().source("rbxassetid://507766388"), "idle");
    idle->setAnimationId("http://www.roblox.com/asset/?id=507766388");
    TrackPtr walk = entry.animator->loadAnimation(f.system.clips().source("rbxassetid://913402848"), "walk");
    walk->setAnimationId("http://www.roblox.com/asset/?id=913402848");
    TrackPtr unnamed = entry.animator->loadAnimation(holdClip(0.1f), "native"); // no AnimationId: never sent
    idle->play(0.2f);

    TrackSender sender;
    auto first = sender.collect(*entry.animator, 1.0f / 60.0f);
    CHECK_EQ(first.size(), std::size_t{2});
    // 81 for the idle that plays, 80 for the walk Animate only loaded.
    CHECK_EQ(first[0].animationId, idle->animationId());
    CHECK_EQ(first[0].changed, std::uint32_t{81});
    CHECK(first[0].playing && first[0].looped);
    CHECK(near(first[0].fadeTime, 0.2f) && near(first[0].weight, 1.0f) && near(first[0].speed, 1.0f));
    CHECK_EQ(first[1].changed, std::uint32_t{80});
    CHECK(!first[1].playing && first[1].looped);
    CHECK(near(first[1].weight, 0.0f) && near(first[1].fadeTime, 0.0f));
    // Nothing changed: nothing goes.
    CHECK(sender.collect(*entry.animator, 1.0f / 60.0f).empty());

    walk->play(0.1f);
    walk->adjustSpeed(1.3f);
    idle->stop(0.1f);
    auto second = sender.collect(*entry.animator, 1.0f / 60.0f);
    CHECK_EQ(second.size(), std::size_t{2});
    CHECK_EQ(second[0].animationId, idle->animationId());
    CHECK_EQ(second[0].changed, TrackUpdate::PlayState);
    CHECK(!second[0].playing && near(second[0].weight, 0.0f) && !second[0].looped);
    CHECK_EQ(second[1].changed, TrackUpdate::PlayState | TrackUpdate::Speed);
    CHECK(second[1].playing && near(second[1].speed, 1.3f));

    walk->adjustSpeed(0.8f);
    walk->adjustWeight(0.5f, 0.3f);
    // Weight and speed alone wait out the interval since the track's last
    // update, then go with the latest values.
    CHECK(sender.collect(*entry.animator, 1.0f / 60.0f).empty());
    auto third = sender.collect(*entry.animator, TrackSender::kAdjustInterval);
    CHECK_EQ(third.size(), std::size_t{1});
    CHECK_EQ(third[0].changed, TrackUpdate::Speed | TrackUpdate::Weight);
    CHECK(near(third[0].weight, 0.5f) && near(third[0].fadeTime, 0.3f) && near(third[0].speed, 0.8f));
    // A Play on a track already playing starts it over: PlayState again.
    walk->play(0.1f);
    CHECK_EQ(sender.collect(*entry.animator, 1.0f / 60.0f).at(0).changed, TrackUpdate::PlayState);

    // What the server relays plays the same on another client.
    Fixture other;
    for (const auto& batch : {first, second, third})
        for (const TrackUpdate& u : batch) {
            auto back = TrackUpdate::fromArguments(u.arguments());
            CHECK(back && back->changed == u.changed && back->animationId == u.animationId);
            other.replicated.apply(*other.animator, *back);
        }
    TrackPtr remoteWalk = other.replicated.track(*other.animator, walk->animationId());
    TrackPtr remoteIdle = other.replicated.track(*other.animator, idle->animationId());
    CHECK(remoteWalk && remoteWalk->isPlaying() && near(remoteWalk->speed(), 0.8f));
    CHECK(near(remoteWalk->weightTarget(), 0.5f));
    CHECK(remoteIdle && !remoteIdle->isPlaying());
}

TEST(avatar_track_sender_waits_for_the_clip)
{
    // A track whose animation is still downloading goes out once it has
    // loaded (its Looping and Priority are the clip's), or after a second.
    Fixture f;
    CharacterAnimation& entry = f.system.add(f.rig, false);
    TrackPtr late = entry.animator->loadAnimation(f.system.clips().source("rbxassetid://1"), "late");
    late->setAnimationId("rbxassetid://1");
    TrackSender sender;
    CHECK(sender.collect(*entry.animator, 0.5f).empty());
    f.system.clips().insert("rbxassetid://1", holdClip(0.2f, 1.0f, false));
    auto loaded = sender.collect(*entry.animator, 0.1f);
    CHECK_EQ(loaded.size(), std::size_t{1});
    CHECK(!loaded[0].looped && loaded[0].priority == Enum::AnimationPriority::Core);

    TrackPtr never = entry.animator->loadAnimation(f.system.clips().source("rbxassetid://2"), "never");
    never->setAnimationId("rbxassetid://2");
    CHECK(sender.collect(*entry.animator, 0.6f).empty());
    CHECK_EQ(sender.collect(*entry.animator, 0.6f).size(), std::size_t{1});
}
