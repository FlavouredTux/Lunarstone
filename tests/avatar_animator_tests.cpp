#include "framework.hpp"

#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/Clip.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

// LowerTorso and UpperTorso joined by a Waist Motor6D.
struct Rig {
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<Part> lower = std::make_shared<Part>();
    std::shared_ptr<Part> upper = std::make_shared<Part>();
    std::shared_ptr<Motor6D> waist = std::make_shared<Motor6D>();

    Rig()
    {
        lower->setName("LowerTorso");
        lower->setParent(model.get());
        upper->setName("UpperTorso");
        upper->setParent(model.get());
        waist->setName("Waist");
        waist->setPart0(lower.get());
        waist->setPart1(upper.get());
        waist->setParent(upper.get());
    }

    float waistAngle() const { return toRotationVector(waist->transform()).x; }
};

// A clip holding the waist at `angle` about X (and `offset` up), with the
// given events.
ClipPtr holdClip(float angle, float length = 1.0f, bool loop = true,
                 Enum::AnimationPriority priority = Enum::AnimationPriority::Core, float offset = 0.0f,
                 std::vector<ClipEvent> events = {})
{
    auto clip = std::make_shared<AnimationClipData>();
    clip->length = length;
    clip->loop = loop;
    clip->priority = priority;
    JointCurve curve{"UpperTorso", "LowerTorso", {}};
    curve.keys.push_back({0.0f, {0.0f, offset, 0.0f}, {angle, 0.0f, 0.0f}});
    clip->joints.push_back(std::move(curve));
    clip->events = std::move(events);
    return clip;
}

} // namespace

TEST(avatar_track_fades_linearly_to_its_target)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(holdClip(1.0f));
    track->play(0.4f);
    CHECK(track->isPlaying());
    CHECK_EQ(track->weightCurrent(), 0.0f);
    animator.step(0.1f);
    CHECK(near(track->weightCurrent(), 0.25f));
    // No fade: the target is set now, the weight follows at the next step
    // (0x103412788 and the step 0x103410fbc).
    track->adjustWeight(0.5f, 0.0f);
    CHECK(near(track->weightCurrent(), 0.25f));
    CHECK_EQ(track->weightTarget(), 0.5f);
    animator.step(0.0f);
    CHECK_EQ(track->weightCurrent(), 0.5f);
    track->adjustWeight(1.0f, 0.2f);
    animator.step(0.1f);
    CHECK(near(track->weightCurrent(), 0.75f));
    animator.step(0.5f);
    CHECK_EQ(track->weightCurrent(), 1.0f);
}

TEST(avatar_track_play_restarts_and_stop_fades_out)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(holdClip(1.0f, 2.0f));
    track->play(0.0f);
    animator.step(0.5f);
    CHECK(near(track->timePosition(), 0.5f));
    track->play(0.0f); // already playing: starts over
    CHECK_EQ(track->timePosition(), 0.0f);

    int stopped = 0;
    int ended = 0;
    track->stopped.connect([&] { ++stopped; });
    track->ended.connect([&] { ++ended; });
    track->stop(0.2f);
    // Stopped fires at once; the track still blends in while fading.
    CHECK(!track->isPlaying());
    CHECK_EQ(stopped, 1);
    CHECK(track->isActive());
    animator.step(0.1f);
    CHECK(near(track->weightCurrent(), 0.5f));
    CHECK(near(rig.waistAngle(), 0.5f));
    CHECK_EQ(ended, 0);
    animator.step(0.1f);
    CHECK_EQ(ended, 1);
    CHECK(!track->isActive());
    CHECK(near(rig.waistAngle(), 0.0f)); // back to identity
    // Stopping again fires nothing; AdjustWeight while stopped does nothing.
    track->stop();
    track->adjustWeight(1.0f);
    CHECK_EQ(stopped, 1);
    CHECK_EQ(track->weightTarget(), 0.0f);
}

TEST(avatar_track_non_looped_end_holds_and_fades_over_0_3s)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track
        = animator.loadAnimation(holdClip(1.0f, 1.0f, false, Enum::AnimationPriority::Core, 0.0f, {{1.0f, "End"}}));
    std::vector<std::string> reached;
    int loops = 0;
    int stopped = 0;
    track->keyframeReached.connect([&](const std::string& name) { reached.push_back(name); });
    track->didLoop.connect([&] { ++loops; });
    track->stopped.connect([&] { ++stopped; });
    track->play(0.0f);
    animator.step(0.6f);
    animator.step(0.6f);
    // Clamped at the end, End fired, DidLoop fired once, stopped.
    CHECK_EQ(track->timePosition(), 1.0f);
    CHECK_EQ(reached, std::vector<std::string>{"End"});
    CHECK_EQ(loops, 1);
    CHECK_EQ(stopped, 1);
    CHECK(!track->isPlaying());
    animator.step(0.15f);
    CHECK(near(track->weightCurrent(), 0.5f));
    animator.step(0.15f);
    CHECK(!track->isActive());
    CHECK_EQ(reached.size(), 1u);
}

// Stop on a track that is already fading out (here the 0.3 s fade of a
// non-looped track that ran out) starts the fade again from where the weight
// is, with the new time (0x103410bbc sets the target and fade time whether
// or not the track plays). Stopped fires only once.
TEST(avatar_track_stop_restarts_a_fade_out_already_running)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(holdClip(1.0f, 0.5f, false));
    int stopped = 0;
    track->stopped.connect([&] { ++stopped; });
    track->play(0.0f);
    animator.step(0.5f); // ran out: fading over 0.3 s
    CHECK(!track->isPlaying());
    animator.step(0.15f);
    CHECK(near(track->weightCurrent(), 0.5f));
    track->stop(0.1f);
    CHECK_EQ(stopped, 1);
    animator.step(0.05f);
    CHECK(near(track->weightCurrent(), 0.25f));
    CHECK(near(rig.waistAngle(), 0.25f));
    animator.step(0.05f);
    CHECK(!track->isActive());
}

// Play and Stop without a fade move WeightCurrent at the next step, not at
// the call (0x103411ea0 and 0x103410bbc only set the target).
TEST(avatar_track_zero_fade_weight_moves_at_the_next_step)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(holdClip(1.0f));
    track->play(0.0f, 0.8f);
    CHECK_EQ(track->weightCurrent(), 0.0f);
    CHECK_EQ(track->weightTarget(), 0.8f);
    animator.step(0.0f);
    CHECK_EQ(track->weightCurrent(), 0.8f);
    track->stop(0.0f);
    CHECK_EQ(track->weightCurrent(), 0.8f);
    animator.step(0.0f);
    CHECK_EQ(track->weightCurrent(), 0.0f);
    CHECK(!track->isActive());
}

TEST(avatar_track_loops_and_fires_events_in_the_swept_window)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    std::vector<ClipEvent> events{{0.0f, "Start"}, {0.5f, "Step", "left", true}, {0.5f, "Mid"}, {1.0f, "End"}};
    TrackPtr track = animator.loadAnimation(holdClip(0.0f, 1.0f, true, Enum::AnimationPriority::Core, 0.0f, events));
    std::vector<std::string> order;
    track->keyframeReached.connect([&](const std::string& name) { order.push_back(name); });
    track->markerReachedSignal("Step").connect([&](const std::string& value) { order.push_back("marker:" + value); });
    int loops = 0;
    track->didLoop.connect([&] { ++loops; });

    track->play(0.0f);
    animator.step(0.25f); // [0, 0.25): Start
    CHECK_EQ(order, (std::vector<std::string>{"Start"}));
    order.clear();
    animator.step(0.5f); // [0.25, 0.75): marker before keyframe
    CHECK_EQ(order, (std::vector<std::string>{"marker:left", "Mid"}));
    order.clear();
    animator.step(0.5f); // wraps: [0.75, 1] then [0, 0.25)
    CHECK(near(track->timePosition(), 0.25f));
    CHECK_EQ(order, (std::vector<std::string>{"End", "Start"}));
    CHECK_EQ(loops, 1);
    order.clear();
    // Setting the time jumps without events.
    track->setTimePosition(0.9f);
    animator.step(0.05f);
    CHECK(order.empty());
}

TEST(avatar_track_negative_speed_runs_backwards)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track
        = animator.loadAnimation(holdClip(0.0f, 1.0f, false, Enum::AnimationPriority::Core, 0.0f, {{0.5f, "Mid"}}));
    std::vector<std::string> order;
    track->keyframeReached.connect([&](const std::string& name) { order.push_back(name); });
    track->play(0.0f, 1.0f, -1.0f);
    CHECK_EQ(track->timePosition(), 1.0f);
    animator.step(0.6f);
    CHECK(near(track->timePosition(), 0.4f));
    CHECK_EQ(order, std::vector<std::string>{"Mid"});
    animator.step(0.6f);
    CHECK_EQ(track->timePosition(), 0.0f);
    CHECK(!track->isPlaying());
}

TEST(avatar_track_defaults_come_from_the_clip)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr pending = animator.loadAnimation([] { return ClipPtr(); }, "pending");
    CHECK(pending->priority() == Enum::AnimationPriority::Action);
    CHECK(!pending->looped());
    CHECK_EQ(pending->length(), 0.0f);
    TrackPtr loaded = animator.loadAnimation(holdClip(0.0f, 2.0f, true, Enum::AnimationPriority::Movement));
    CHECK(loaded->priority() == Enum::AnimationPriority::Movement);
    CHECK(loaded->looped());
    loaded->setPriority(Enum::AnimationPriority::Action4);
    CHECK(loaded->priority() == Enum::AnimationPriority::Action4);
}

TEST(avatar_blend_higher_priority_overrides_lower)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr core = animator.loadAnimation(holdClip(1.0f));
    TrackPtr action = animator.loadAnimation(holdClip(0.5f, 1.0f, true, Enum::AnimationPriority::Action));
    core->play(0.0f);
    action->play(0.0f);
    animator.step(0.0f);
    CHECK(near(rig.waistAngle(), 0.5f));

    // Half an Action leaves the other half to Core.
    action->adjustWeight(0.5f, 0.0f);
    animator.step(0.0f);
    CHECK(near(rig.waistAngle(), 0.75f, 2e-3f));

    // Core is the lowest priority, below Idle.
    TrackPtr idle = animator.loadAnimation(holdClip(-0.5f, 1.0f, true, Enum::AnimationPriority::Idle));
    action->stop(0.0f);
    idle->play(0.0f);
    animator.step(0.0f);
    CHECK(near(rig.waistAngle(), -0.5f));
}

TEST(avatar_blend_same_priority_averages_by_weight)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr a = animator.loadAnimation(holdClip(0.2f, 1.0f, true, Enum::AnimationPriority::Core, 2.0f));
    TrackPtr b = animator.loadAnimation(holdClip(0.6f, 1.0f, true, Enum::AnimationPriority::Core, 0.0f));
    a->play(0.0f);
    b->play(0.0f);
    animator.step(0.0f);
    // Weights add past 1, so the layer is normalised: an even average.
    CHECK(near(rig.waistAngle(), 0.4f, 2e-3f));
    CHECK(near(rig.waist->transform().position.y, 1.0f));

    b->adjustWeight(3.0f, 0.0f);
    animator.step(0.0f);
    CHECK(near(rig.waist->transform().position.y, 0.5f));
}

TEST(avatar_blend_partial_weight_scales_from_rest)
{
    Rig rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(holdClip(0.8f, 1.0f, true, Enum::AnimationPriority::Core, 1.0f));
    track->play(0.0f, 0.5f);
    animator.step(0.0f);
    CHECK(near(rig.waistAngle(), 0.4f));
    CHECK(near(rig.waist->transform().position.y, 0.5f));

    // Below 0.001 a track is left out altogether.
    track->adjustWeight(0.0005f, 0.0f);
    animator.step(0.0f);
    CHECK(near(rig.waistAngle(), 0.0f));
}

namespace {

// An R15-style root: HumanoidRootPart to LowerTorso by the Root Motor6D,
// LowerTorso to UpperTorso by the Waist, and the Humanoid.
struct R15Root {
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<Part> hrp = std::make_shared<Part>();
    std::shared_ptr<Part> lower = std::make_shared<Part>();
    std::shared_ptr<Part> upper = std::make_shared<Part>();
    std::shared_ptr<Motor6D> root = std::make_shared<Motor6D>();
    std::shared_ptr<Motor6D> waist = std::make_shared<Motor6D>();
    std::shared_ptr<Humanoid> humanoid = std::make_shared<Humanoid>();

    R15Root()
    {
        hrp->setName("HumanoidRootPart");
        lower->setName("LowerTorso");
        upper->setName("UpperTorso");
        for (const auto& part : {hrp, lower, upper})
            part->setParent(model.get());
        root->setName("Root");
        root->setPart0(hrp.get());
        root->setPart1(lower.get());
        root->setParent(lower.get());
        waist->setName("Waist");
        waist->setPart0(lower.get());
        waist->setPart1(upper.get());
        waist->setParent(upper.get());
        humanoid->setRigType(Enum::HumanoidRigType::R15);
        humanoid->setHipHeight(2.0f);
        humanoid->setParent(model.get());
    }
};

// Both joints lifted by `lift` studs, the R15 jump's 0.867 by default.
ClipPtr liftClip(float lift = 0.867f)
{
    auto clip = std::make_shared<AnimationClipData>();
    clip->length = 1.0f;
    clip->priority = Enum::AnimationPriority::Core;
    for (auto [part, parent] : {std::pair{"LowerTorso", "HumanoidRootPart"}, std::pair{"UpperTorso", "LowerTorso"}}) {
        JointCurve curve{part, parent, {}};
        curve.keys.push_back({0.0f, {0.0f, lift, 0.0f}, {}});
        clip->joints.push_back(std::move(curve));
    }
    return clip;
}

} // namespace

// The Animator scales what it writes into Motor6D.Transform's translation
// (0x103422589..0x10342273e, applied at 0x103423401): the root joint
// (HumanoidRootPart to LowerTorso, found by name at 0x10341ea9c) by
// HipHeight / 2 when the Humanoid has AutomaticScalingEnabled, every other
// joint by the model's Model:GetScale(). A shorter avatar lifts its hips
// less in the jump.
TEST(avatar_root_translation_scales_with_hip_height)
{
    R15Root rig;
    avatar::Animator animator(*rig.model);
    TrackPtr track = animator.loadAnimation(liftClip());
    track->play(0.0f);
    animator.step(0.0f);
    CHECK(near(rig.root->transform().position.y, 0.867f));
    CHECK(near(rig.waist->transform().position.y, 0.867f));

    rig.humanoid->setHipHeight(1.35f);
    animator.step(0.0f);
    CHECK(near(rig.root->transform().position.y, 0.867f * 0.675f));
    CHECK(near(rig.waist->transform().position.y, 0.867f));

    // A scaled model: the other joints follow its scale; the root still
    // follows the hip height (ScaleTo scales HipHeight itself).
    rig.model->setScaleFactor(2.0f);
    animator.step(0.0f);
    CHECK(near(rig.root->transform().position.y, 0.867f * 0.675f));
    CHECK(near(rig.waist->transform().position.y, 2.0f * 0.867f));

    // Without automatic scaling the root is a joint like the others.
    rig.humanoid->setAutomaticScalingEnabled(false);
    animator.step(0.0f);
    CHECK(near(rig.root->transform().position.y, 2.0f * 0.867f));
}

TEST(avatar_animator_drives_animation_constraints)
{
    // An upgraded avatar: the waist is an AnimationConstraint between the
    // torsos' attachments, matched to the clip's UpperTorso pose by the
    // attachments' parts; its Transform gets the pose.
    auto model = std::make_shared<Model>();
    auto lower = std::make_shared<Part>();
    lower->setName("LowerTorso");
    lower->setParent(model.get());
    auto upper = std::make_shared<Part>();
    upper->setName("UpperTorso");
    upper->setParent(model.get());
    auto a0 = std::make_shared<Attachment>();
    a0->setParent(lower.get());
    auto a1 = std::make_shared<Attachment>();
    a1->setParent(upper.get());
    auto waist = std::make_shared<AnimationConstraint>();
    waist->setName("Waist");
    waist->setAttachment0(a0.get());
    waist->setAttachment1(a1.get());
    waist->setParent(upper.get());

    avatar::Animator animator(*model);
    TrackPtr track = animator.loadAnimation(holdClip(0.6f));
    track->play(0.0f);
    animator.step(0.1f);
    CHECK(near(toRotationVector(waist->transform()).x, 0.6f));
    track->stop(0.0f);
    animator.step(0.1f);
    CHECK(near(toRotationVector(waist->transform()).x, 0.0f));
}
