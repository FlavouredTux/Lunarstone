#include "framework.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Animate.hpp>
#include <lunar/avatar/Clip.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

std::shared_ptr<Keyframe> addKeyframe(KeyframeSequence& sequence, float time, std::string name = "Keyframe")
{
    auto keyframe = std::make_shared<Keyframe>();
    keyframe->setTime(time);
    keyframe->setName(std::move(name));
    keyframe->setParent(&sequence);
    return keyframe;
}

// Root > LowerTorso > UpperTorso, with the UpperTorso pose turned by `angle`
// about X.
std::shared_ptr<Pose> addTorso(Keyframe& keyframe, float angle,
                               Enum::PoseEasingStyle style = Enum::PoseEasingStyle::Linear)
{
    auto root = std::make_shared<Pose>();
    root->setName("HumanoidRootPart");
    root->setWeight(0.0f);
    root->setParent(&keyframe);
    auto lower = std::make_shared<Pose>();
    lower->setName("LowerTorso");
    lower->setParent(root.get());
    auto upper = std::make_shared<Pose>();
    upper->setName("UpperTorso");
    upper->setCFrame(CFrame::angles(angle, 0.0f, 0.0f));
    upper->setEasingStyle(style);
    upper->setParent(lower.get());
    return upper;
}

std::size_t jointIndex(const AnimationClipData& clip, std::string_view part)
{
    for (std::size_t i = 0; i < clip.joints.size(); ++i)
        if (clip.joints[i].part == part)
            return i;
    return clip.joints.size();
}

// The sampled joint's turn about X; NaN when the joint isn't animated.
float angleAboutX(const AnimationClipData& clip, std::size_t joint, float t)
{
    auto sample = clip.sample(joint, t);
    return sample ? toRotationVector(sample->rotation).x : std::nanf("");
}

std::filesystem::path contentDir()
{
    return assets::AssetConfig::fromEnvironment().contentDir;
}

} // namespace

TEST(avatar_clip_sorts_keyframes_and_finds_joints)
{
    KeyframeSequence sequence;
    sequence.setLoop(false);
    sequence.setPriority(Enum::AnimationPriority::Movement);
    // Out of order, like the default animations store them.
    addTorso(*addKeyframe(sequence, 1.0f, "End"), 1.0f);
    addTorso(*addKeyframe(sequence, 0.0f), 0.0f);
    addTorso(*addKeyframe(sequence, 0.5f), 0.2f);

    ClipPtr clip = compileKeyframeSequence(sequence);
    CHECK(!clip->loop);
    CHECK(clip->priority == Enum::AnimationPriority::Movement);
    CHECK_EQ(clip->length, 1.0f);
    // The weightless root pose is dropped, LowerTorso and UpperTorso stay.
    CHECK_EQ(clip->joints.size(), 2u);
    const std::size_t upper = jointIndex(*clip, "UpperTorso");
    CHECK(upper < clip->joints.size());
    CHECK_EQ(clip->joints[upper].parent, "LowerTorso");
    CHECK_EQ(clip->joints[jointIndex(*clip, "LowerTorso")].parent, "HumanoidRootPart");
    CHECK_EQ(clip->joints[upper].keys.size(), 3u);
    CHECK_EQ(clip->joints[upper].keys[1].time, 0.5f);

    // Linear between keys, held after the last one.
    CHECK(near(angleAboutX(*clip, upper, 0.25f), 0.1f));
    CHECK(near(angleAboutX(*clip, upper, 0.75f), 0.6f));
    CHECK(near(angleAboutX(*clip, upper, 5.0f), 1.0f));

    CHECK_EQ(clip->events.size(), 1u);
    CHECK_EQ(clip->events[0].name, "End");
    CHECK_EQ(clip->events[0].time, 1.0f);
}

TEST(avatar_clip_bridges_missing_poses_and_uses_the_earlier_easing)
{
    KeyframeSequence sequence;
    addTorso(*addKeyframe(sequence, 0.0f), 0.0f, Enum::PoseEasingStyle::Constant);
    addKeyframe(sequence, 0.5f); // no poses at all
    addTorso(*addKeyframe(sequence, 1.0f), 1.0f);
    ClipPtr clip = compileKeyframeSequence(sequence);
    const std::size_t upper = jointIndex(*clip, "UpperTorso");
    // Constant In from the first key: straight to the next key that has the joint.
    CHECK(near(angleAboutX(*clip, upper, 0.25f), 1.0f));
    CHECK(near(angleAboutX(*clip, upper, 0.75f), 1.0f));
}

TEST(avatar_clip_joint_missing_from_the_first_keyframe_waits)
{
    KeyframeSequence sequence;
    auto first = addKeyframe(sequence, 0.0f);
    auto lonely = std::make_shared<Pose>();
    lonely->setName("Head");
    lonely->setParent(first.get());
    addTorso(*addKeyframe(sequence, 0.5f), 0.5f);
    ClipPtr clip = compileKeyframeSequence(sequence);
    const std::size_t upper = jointIndex(*clip, "UpperTorso");
    // Before its first key the joint isn't animated at all.
    CHECK(!clip->sample(upper, 0.25f).has_value());
    CHECK(clip->sample(upper, 0.5f).has_value());
    CHECK(clip->sample(jointIndex(*clip, "Head"), 0.25f).has_value());
}

TEST(avatar_clip_markers_become_events)
{
    KeyframeSequence sequence;
    auto keyframe = addKeyframe(sequence, 0.3f);
    auto marker = std::make_shared<KeyframeMarker>();
    marker->setName("Step");
    marker->setValue("left");
    marker->setParent(keyframe.get());
    ClipPtr clip = compileKeyframeSequence(sequence);
    CHECK_EQ(clip->events.size(), 1u);
    CHECK(clip->events[0].marker);
    CHECK_EQ(clip->events[0].name, "Step");
    CHECK_EQ(clip->events[0].value, "left");
}

TEST(avatar_content_animate_children_match_the_script_tables)
{
    const auto path = contentDir() / "avatar/animations/humanoidR15AnimateChildren.rbxm";
    if (!std::filesystem::exists(path))
        SKIP("no RobloxPlayer content folder");
    auto bytes = readFile(path);
    CHECK(bytes.has_value());
    auto model = loadModel(*bytes);
    CHECK(model.has_value());
    const Instance& folder = *model->roots.front();
    // The animation sets the client gives the Animate script.
    const auto* walk = folder.findFirstChild("walk");
    CHECK(walk != nullptr);
    const auto* walkAnim = dynamic_cast<const Animation*>(walk->findFirstChild("WalkAnim"));
    CHECK(walkAnim != nullptr);
    CHECK_EQ(walkAnim->animationId(), "http://www.roblox.com/asset/?id=913402848");
    const auto* idle = folder.findFirstChild("idle");
    CHECK_EQ(idle->children().size(), 2u);
    const auto* scale = dynamic_cast<const NumberValue*>(folder.findFirstChild("ScaleDampeningPercent"));
    CHECK(scale != nullptr);
    CHECK_EQ(scale->value(), 1.0);
    // Every set the folder configures is one the controller knows.
    for (const InstancePtr& child : folder.children())
        if (child->isA("StringValue"))
            CHECK(defaultAnimations(Enum::HumanoidRigType::R15).contains(child->name()));
}

TEST(avatar_cached_walk_animation_compiles)
{
    // Only when an earlier run downloaded the default walk into the cache.
    assets::AssetConfig config = assets::AssetConfig::fromEnvironment();
    config.download = false;
    assets::AssetCache cache(config);
    auto bytes = cache.fetch("rbxassetid://913402848");
    if (!bytes)
        SKIP("default walk animation not in the asset cache");
    auto model = loadModel(**bytes);
    CHECK(model.has_value());
    ClipPtr clip = compileAnimationAsset(model->roots);
    CHECK(clip != nullptr);
    CHECK(clip->loop);
    CHECK(clip->priority == Enum::AnimationPriority::Core);
    CHECK(near(clip->length, 0.625f, 1e-3f));
    CHECK(jointIndex(*clip, "RightUpperLeg") < clip->joints.size());
    bool hasEnd = false;
    for (const ClipEvent& event : clip->events)
        hasEnd = hasEnd || event.name == "End";
    CHECK(hasEnd);
}
