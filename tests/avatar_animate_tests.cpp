#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Animate.hpp>
#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/ClipLoader.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

// A one-second clip with an "End" keyframe, standing in for a download.
ClipPtr fakeClip(std::string name, bool loop = true)
{
    auto clip = std::make_shared<AnimationClipData>();
    clip->name = std::move(name);
    clip->length = 1.0f;
    clip->loop = loop;
    clip->priority = Enum::AnimationPriority::Core;
    clip->events.push_back({1.0f, "End", {}, false});
    return clip;
}

struct Fixture {
    std::shared_ptr<Model> character = std::make_shared<Model>();
    std::shared_ptr<Humanoid> humanoid = std::make_shared<Humanoid>();
    ClipLoader clips;
    std::unique_ptr<avatar::Animator> animator;
    std::unique_ptr<AnimateController> animate;

    explicit Fixture(float hipHeight = 2.0f)
        : clips([] {
              assets::AssetConfig config;
              config.download = false;
              return std::make_shared<assets::AssetCache>(config);
          }())
    {
        humanoid->setRigType(Enum::HumanoidRigType::R15);
        humanoid->setHipHeight(hipHeight);
        humanoid->setParent(character.get());
        // Every default animation, already "loaded".
        for (const auto& [name, choices] : defaultAnimations(Enum::HumanoidRigType::R15))
            for (const AnimationChoice& choice : choices)
                clips.insert(choice.animationId, fakeClip(name, name != "jump" && name != "cheer"));
        animator = std::make_unique<avatar::Animator>(*character);
        animate = std::make_unique<AnimateController>(*character, *animator, clips, 7);
    }
};

} // namespace

TEST(avatar_animate_starts_idle)
{
    Fixture f;
    CHECK_EQ(f.animate->currentAnimation(), "idle");
    CHECK(f.animate->pose() == AnimateController::Pose::Standing);
    CHECK(f.animate->currentTrack()->isPlaying());
    CHECK(f.animate->currentTrack()->priority() == Enum::AnimationPriority::Core);
}

TEST(avatar_animate_walk_blends_into_run_by_speed)
{
    Fixture f;
    // Below 0.75 studs/s (times the height scale) it stays idle.
    f.animate->onRunning(0.5f);
    CHECK_EQ(f.animate->currentAnimation(), "idle");

    // 10 studs/s: setAnimationSpeed(10 / 16); rootMotionCompensation gives
    // 0.625 * 1.25 = 0.78125, between the walk (0.5) and run (1) speeds.
    f.animate->onRunning(10.0f);
    CHECK_EQ(f.animate->currentAnimation(), "walk");
    CHECK(f.animate->pose() == AnimateController::Pose::Running);
    const TrackPtr walk = f.animate->currentTrack();
    const TrackPtr run = f.animate->runTrack();
    CHECK(walk && run);
    CHECK(near(walk->weightTarget(), 1.0f - 0.5625f));
    CHECK(near(run->weightTarget(), 0.5625f));
    CHECK(near(walk->speed(), 1.0f));

    // 16 studs/s (the default WalkSpeed) is all run, sped up 1.25 times.
    f.animate->onRunning(16.0f);
    CHECK(f.animate->currentTrack() == walk); // same animation, no restart
    CHECK(near(run->weightTarget(), 1.0f));
    CHECK(near(walk->weightTarget(), 0.0001f));
    CHECK(near(run->speed(), 1.25f));

    // Slow walking: walk only, slowed down.
    f.animate->onRunning(4.0f);
    CHECK(near(walk->weightTarget(), 1.0f));
    CHECK(near(walk->speed(), 0.3125f / 0.5f));

    f.animate->onRunning(0.0f);
    CHECK_EQ(f.animate->currentAnimation(), "idle");
    CHECK(f.animate->runTrack() == nullptr);
}

TEST(avatar_animate_height_scale_follows_hip_height)
{
    // A taller character (HipHeight 3) needs more speed for the same blend:
    // scale = 1 + (3 - 2) * ScaleDampeningPercent / 2, and without an
    // Animate object (no ScaleDampeningPercent) it's 3 / 2 = 1.5.
    Fixture f(3.0f);
    f.animate->onRunning(1.0f); // threshold 0.75 * 1.5 = 1.125
    CHECK_EQ(f.animate->currentAnimation(), "idle");
    f.animate->onRunning(16.0f);
    // 16 / 16 * 1.25 / 1.5 = 0.8333: still blending.
    CHECK(near(f.animate->runTrack()->weightTarget(), (0.833333f - 0.5f) / 0.5f, 1e-3f));
}

TEST(avatar_animate_jump_then_fall)
{
    Fixture f;
    f.animate->onJumping();
    CHECK_EQ(f.animate->currentAnimation(), "jump");
    // Free fall right after a jump keeps the jump for jumpAnimDuration (0.31 s).
    f.animate->onFreeFall();
    CHECK_EQ(f.animate->currentAnimation(), "jump");
    CHECK(f.animate->pose() == AnimateController::Pose::FreeFall);
    f.animate->update(0.2f);
    CHECK_EQ(f.animate->currentAnimation(), "jump");
    f.animate->update(0.3f);
    CHECK_EQ(f.animate->currentAnimation(), "fall");
}

TEST(avatar_animate_other_states)
{
    Fixture f;
    f.animate->onClimbing(5.0f);
    CHECK_EQ(f.animate->currentAnimation(), "climb");
    CHECK(near(f.animate->currentTrack()->speed(), 1.0f)); // 5 / 5
    f.animate->onSwimming(20.0f);
    CHECK_EQ(f.animate->currentAnimation(), "swim");
    CHECK(near(f.animate->currentTrack()->speed(), 2.0f)); // 20 / 10
    f.animate->onSwimming(0.5f);
    CHECK_EQ(f.animate->currentAnimation(), "swimidle");
    f.animate->onSeated();
    f.animate->update(0.1f);
    CHECK_EQ(f.animate->currentAnimation(), "sit");
    f.animate->onStateChanged(HumanoidState::Dead);
    f.animate->update(0.1f);
    CHECK_EQ(f.animate->currentAnimation(), "");
}

TEST(avatar_animate_emotes_only_while_standing)
{
    Fixture f;
    CHECK(f.animate->playEmote("wave"));
    CHECK_EQ(f.animate->currentAnimation(), "wave");
    CHECK(!f.animate->playEmote("nonsense"));
    // A non-looping emote returns to idle at its End keyframe.
    f.animator->step(1.05f);
    CHECK_EQ(f.animate->currentAnimation(), "idle");

    f.animate->onRunning(10.0f);
    CHECK(!f.animate->playEmote("dance"));
    CHECK_EQ(f.animate->currentAnimation(), "walk");
}

TEST(avatar_animate_reads_custom_sets_from_the_animate_object)
{
    auto character = std::make_shared<Model>();
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setRigType(Enum::HumanoidRigType::R15);
    humanoid->setParent(character.get());
    auto script = std::make_shared<LocalScript>();
    script->setName("Animate");
    script->setParent(character.get());
    auto idle = std::make_shared<StringValue>();
    idle->setName("idle");
    idle->setParent(script.get());
    auto custom = std::make_shared<Animation>();
    custom->setName("MyIdle");
    custom->setAnimationId("rbxassetid://1234");
    custom->setParent(idle.get());

    assets::AssetConfig config;
    config.download = false;
    ClipLoader clips(std::make_shared<assets::AssetCache>(config));
    clips.insert("rbxassetid://1234", fakeClip("mine"));
    avatar::Animator animator(*character);
    AnimateController animate(*character, animator, clips);
    CHECK_EQ(animate.currentTrack()->name(), "MyIdle");
    CHECK(animate.currentTrack()->clip() != nullptr);
}

TEST(avatar_animate_holds_an_equipped_tool)
{
    Fixture f;
    f.animate->update(0.2f);
    CHECK(f.animate->toolTrack() == nullptr);

    // A Tool with a Handle in the character: "toolnone" at Idle priority.
    auto tool = std::make_shared<Tool>();
    auto handle = std::make_shared<Part>();
    handle->setName("Handle");
    handle->setParent(tool.get());
    tool->setParent(f.character.get());
    f.animate->update(0.2f);
    CHECK(f.animate->toolTrack() != nullptr);
    CHECK(f.animate->toolTrack()->clip()->name == "toolnone");
    CHECK(f.animate->toolTrack()->isPlaying());
    CHECK(f.animate->toolTrack()->priority() == Enum::AnimationPriority::Idle);

    // A "toolanim" StringValue asks for a slash, then it settles back.
    auto anim = std::make_shared<StringValue>();
    anim->setName("toolanim");
    anim->setValue("Slash");
    anim->setParent(tool.get());
    f.animate->update(0.1f);
    CHECK(f.animate->toolTrack()->clip()->name == "toolslash");
    CHECK(anim->parent() == nullptr);

    // Put away: the hold stops.
    tool->setParent(nullptr);
    f.animate->update(0.2f);
    CHECK(f.animate->toolTrack() == nullptr);
}

// Many clips asked for at once all load (or fail) on the loader's few
// threads: wait() returns with every one finished, and a load from several
// threads at once gives each the same answer.
TEST(avatar_clip_loader_loads_many_on_a_few_threads)
{
    assets::AssetConfig config;
    config.download = false;
    config.cacheDir = "/nonexistent/lunarstone-clip-loader-test";
    ClipLoader clips(std::make_shared<assets::AssetCache>(config));
    clips.insert("rbxassetid://1", fakeClip("one"));
    for (int i = 100; i < 160; ++i)
        CHECK(clips.clip(std::format("rbxassetid://{}", i)) == nullptr); // not in the cache: they fail
    clips.wait();
    for (int i = 100; i < 160; ++i)
        CHECK(!clips.load(std::format("rbxassetid://{}", i)).has_value());
    std::atomic<int> answered = 0;
    {
        std::vector<std::jthread> callers;
        for (int t = 0; t < 6; ++t)
            callers.emplace_back([&] {
                for (int i = 200; i < 220; ++i)
                    if (!clips.load(std::format("rbxassetid://{}", i)).has_value())
                        answered.fetch_add(1);
                if (auto one = clips.load("rbxassetid://1"); one && *one && (*one)->name == "one")
                    answered.fetch_add(1);
            });
    }
    CHECK_EQ(answered.load(), 6 * 21);
}

TEST(avatar_clip_loader_follows_emote_animation_assets_and_rejects_cycles)
{
    const auto path
        = std::filesystem::temp_directory_path()
          / std::format("lunarstone-emote-clips-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{path};
    std::filesystem::create_directories(path / "emotes");
    std::ofstream(path / "emotes/package.rbxmx")
        << R"(<roblox version="4"><Item class="Animation"><Properties><Content name="AnimationId"><url>rbxasset://emotes/clip.rbxmx</url></Content></Properties></Item></roblox>)";
    std::ofstream(path / "emotes/clip.rbxmx")
        << R"(<roblox version="4"><Item class="KeyframeSequence"><Properties><string name="Name">Wave</string></Properties><Item class="Keyframe"><Properties><float name="Time">0</float></Properties></Item><Item class="Keyframe"><Properties><float name="Time">0.5</float></Properties></Item></Item></roblox>)";
    std::ofstream(path / "emotes/cycle.rbxmx")
        << R"(<roblox version="4"><Item class="Animation"><Properties><Content name="AnimationId"><url>rbxasset://emotes/cycle.rbxmx</url></Content></Properties></Item></roblox>)";
    assets::AssetConfig config;
    config.contentDir = path;
    config.cacheDir = path / "cache";
    config.download = false;
    ClipLoader clips(std::make_shared<assets::AssetCache>(config));
    const auto loaded = clips.load("rbxasset://emotes/package.rbxmx");
    CHECK(loaded.has_value());
    CHECK(*loaded != nullptr);
    CHECK_EQ((*loaded)->name, "Wave");
    CHECK(near((*loaded)->length, 0.5f));
    auto polled = clips.poll("rbxasset://emotes/package.rbxmx");
    CHECK(polled.has_value() && *polled == *loaded);
    const auto cyclic = clips.load("rbxasset://emotes/cycle.rbxmx");
    CHECK(!cyclic.has_value());
    CHECK(cyclic.error().find("cyclic") != std::string::npos);
}
