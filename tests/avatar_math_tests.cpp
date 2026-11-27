#include "framework.hpp"

#include <lunar/avatar/Clip.hpp>
#include <lunar/avatar/Quaternion.hpp>
#include <lunar/avatar/Rig.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

bool near(Vector3 a, Vector3 b, float tolerance = 1e-4f)
{
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

bool near(const CFrame& a, const CFrame& b, float tolerance = 1e-4f)
{
    if (!near(a.position, b.position, tolerance))
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!near(a.r[i][j], b.r[i][j], tolerance))
                return false;
    return true;
}

} // namespace

TEST(avatar_quaternion_round_trips_cframes)
{
    const CFrame samples[] = {
        CFrame(),
        CFrame::angles(0.3f, -1.2f, 2.5f),
        CFrame::angles(std::numbers::pi_v<float>, 0.0f, 0.0f),
        CFrame::angles(0.0f, std::numbers::pi_v<float> - 0.01f, 0.0f),
        CFrame::angles(-2.0f, 0.1f, -3.0f),
    };
    for (const CFrame& cf : samples) {
        CHECK(near(Quaternion::fromCFrame(cf).toCFrame(), cf));
        CHECK(near(fromRotationVector(toRotationVector(cf)).toCFrame(), cf));
    }
}

TEST(avatar_rotation_vector_is_axis_times_angle)
{
    // CFrame.Angles(0, 0, 1) turns 1 radian about +Z.
    CHECK(near(toRotationVector(CFrame::angles(0.0f, 0.0f, 1.0f)), {0.0f, 0.0f, 1.0f}));
    CHECK(near(toRotationVector(CFrame::angles(-0.5f, 0.0f, 0.0f)), {-0.5f, 0.0f, 0.0f}));
    CHECK(near(toRotationVector(CFrame()), {}));
}

TEST(avatar_rotation_blend_goes_the_short_way)
{
    // +170 and -170 degrees about Y are 20 degrees apart through 180.
    const float a = 170.0f * std::numbers::pi_v<float> / 180.0f;
    const Vector3 from{0.0f, a, 0.0f};
    const Vector3 to{0.0f, -a, 0.0f};
    const CFrame halfway = blendRotations(from, to, 0.5f).toCFrame();
    CHECK(near(halfway, CFrame::angles(0.0f, std::numbers::pi_v<float>, 0.0f), 1e-3f));
    // Plain cases are a straight blend of the rotation vectors.
    CHECK(near(blendRotations({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.25f).toCFrame(),
               CFrame::angles(0.0f, 0.0f, 0.25f)));
}

TEST(avatar_joint_offset_is_c0_transform_c1_inverse)
{
    auto model = std::make_shared<Model>();
    auto root = std::make_shared<Part>();
    root->setName("HumanoidRootPart");
    root->setCFrame(CFrame(1.0f, 2.0f, 3.0f) * CFrame::angles(0.0f, 0.7f, 0.0f));
    root->setParent(model.get());
    auto limb = std::make_shared<Part>();
    limb->setParent(model.get());
    auto hand = std::make_shared<Part>();
    hand->setParent(model.get());

    auto motor = std::make_shared<Motor6D>();
    motor->setPart0(root.get());
    motor->setPart1(limb.get());
    motor->setC0(CFrame(0.0f, 1.0f, 0.0f));
    motor->setC1(CFrame(0.0f, -0.5f, 0.0f) * CFrame::angles(0.2f, 0.0f, 0.0f));
    motor->setTransform(CFrame::angles(0.0f, 0.0f, 0.4f));
    motor->setParent(limb.get());
    // A weld the other way round: Part1 is the one already placed.
    auto weld = std::make_shared<Weld>();
    weld->setPart0(hand.get());
    weld->setPart1(limb.get());
    weld->setC0(CFrame(0.0f, 0.3f, 0.0f));
    weld->setParent(hand.get());

    CHECK_EQ(solveCharacterJoints(*model), 2u);
    const CFrame expectedLimb = root->cframe() * motor->c0() * motor->transform() * motor->c1().inverse();
    CHECK(near(limb->cframe(), expectedLimb));
    // limb = hand * C0 * C1^-1, so hand = limb * C1 * C0^-1.
    CHECK(near(hand->cframe(), expectedLimb * weld->c1() * weld->c0().inverse()));
}

// In this setup, a moving NPC model holds an anchored
// Figure and the SeekRig model, whose AnimationController animates it; the
// rig's PrimaryPart Root hangs off Figure by a ManualWeld (Part0 Root,
// Part1 Figure) and the server moves Figure. Figure is the assembly's root:
// the rig follows it, and Figure isn't put back where Root was.
TEST(avatar_joint_solver_starts_from_an_anchored_part)
{
    auto seek = std::make_shared<Model>();
    auto figure = std::make_shared<Part>();
    figure->setName("Figure");
    figure->setAnchored(true);
    figure->setCFrame(CFrame(0.0f, -80.0f, 0.0f));
    figure->setParent(seek.get());
    auto rig = std::make_shared<Model>();
    rig->setName("SeekRig");
    rig->setParent(seek.get());
    auto root = std::make_shared<Part>();
    root->setName("Root");
    root->setCFrame(CFrame(0.0f, -78.0f, 0.0f));
    root->setParent(rig.get());
    rig->setPrimaryPart(root.get());
    auto torso = std::make_shared<Part>();
    torso->setName("UpperTorso");
    torso->setParent(rig.get());
    auto weld = std::make_shared<ManualWeld>();
    weld->setPart0(root.get());
    weld->setPart1(figure.get());
    weld->setC1(CFrame(0.0f, 2.0f, 0.0f)); // Figure = Root * C1:Inverse(): Root 2 studs over Figure
    weld->setParent(root.get());
    auto motor = std::make_shared<Motor6D>();
    motor->setName("Root");
    motor->setPart0(root.get());
    motor->setPart1(torso.get());
    motor->setTransform(CFrame(0.0f, -8.0f, 0.0f)); // the intro: under the floor
    motor->setParent(root.get());

    CharacterJointSolver solver(rig);
    CHECK_EQ(solver.solve(), 2u);
    CHECK(near(figure->cframe(), CFrame(0.0f, -80.0f, 0.0f)));
    CHECK(near(root->cframe(), CFrame(0.0f, -78.0f, 0.0f)));
    CHECK(near(torso->cframe(), CFrame(0.0f, -86.0f, 0.0f)));

    // The server moves Figure up into the room: the rig goes with it.
    figure->setCFrame(CFrame(15.0f, 20.0f, 0.0f));
    solver.solve();
    CHECK(near(figure->cframe(), CFrame(15.0f, 20.0f, 0.0f)));
    CHECK(near(root->cframe(), CFrame(15.0f, 22.0f, 0.0f)));
    CHECK(near(torso->cframe(), CFrame(15.0f, 14.0f, 0.0f)));
    // The same from a fresh solve.
    root->setCFrame({});
    CHECK_EQ(solveCharacterJoints(*rig), 2u);
    CHECK(near(root->cframe(), CFrame(15.0f, 22.0f, 0.0f)));
    CHECK(near(figure->cframe(), CFrame(15.0f, 20.0f, 0.0f)));

    // Unanchored, Figure goes back to hanging off Root.
    figure->setAnchored(false);
    root->setCFrame(CFrame(0.0f, 5.0f, 0.0f));
    solver.solve();
    CHECK(near(figure->cframe(), CFrame(0.0f, 3.0f, 0.0f)));
}

TEST(avatar_cached_joint_solver_matches_a_fresh_solve)
{
    // A root, a limb on a Motor6D and a hand welded the other way round, as
    // above; the cached solver places them exactly like a fresh solve, also
    // after the animation moves, a joint changes, and parts come and go.
    auto model = std::make_shared<Model>();
    auto root = std::make_shared<Part>();
    root->setName("HumanoidRootPart");
    root->setCFrame(CFrame(1.0f, 2.0f, 3.0f) * CFrame::angles(0.0f, 0.7f, 0.0f));
    root->setParent(model.get());
    auto limb = std::make_shared<Part>();
    limb->setParent(model.get());
    auto hand = std::make_shared<Part>();
    hand->setParent(model.get());
    auto motor = std::make_shared<Motor6D>();
    motor->setPart0(root.get());
    motor->setPart1(limb.get());
    motor->setC0(CFrame(0.0f, 1.0f, 0.0f));
    motor->setC1(CFrame(0.0f, -0.5f, 0.0f) * CFrame::angles(0.2f, 0.0f, 0.0f));
    motor->setParent(limb.get());
    auto weld = std::make_shared<Weld>();
    weld->setPart0(hand.get());
    weld->setPart1(limb.get());
    weld->setC0(CFrame(0.0f, 0.3f, 0.0f));
    weld->setParent(hand.get());

    CharacterJointSolver solver(model);
    // Parts the solve doesn't reach stay put, so only the hand's welded state is compared.
    auto same = [&](std::size_t expected, bool handFollows = true) {
        const std::size_t placed = solver.solve();
        const CFrame cachedLimb = limb->cframe();
        const CFrame cachedHand = hand->cframe();
        limb->setCFrame({});
        if (handFollows)
            hand->setCFrame({});
        CHECK_EQ(solveCharacterJoints(*model), expected);
        CHECK_EQ(placed, expected);
        CHECK(cachedLimb == limb->cframe());
        CHECK(cachedHand == hand->cframe());
    };
    same(2);
    motor->setTransform(CFrame::angles(0.0f, 0.0f, 0.4f));
    same(2);
    // A joint let go: only the limb follows.
    weld->setEnabled(false);
    same(1, false);
    weld->setEnabled(true);
    same(2);
    // A new part welded on (a WeldConstraint keeps where it is).
    auto hat = std::make_shared<Part>();
    hat->setCFrame(CFrame(0.0f, 5.0f, 0.0f));
    hat->setParent(model.get());
    auto hold = std::make_shared<WeldConstraint>();
    hold->setPart0(hat.get());
    hold->setPart1(root.get());
    hold->setParent(hat.get());
    same(3);
    root->setCFrame(CFrame(10.0f, 0.0f, 0.0f));
    same(3);
    // The motor drives another part now; the old limb stays where it was.
    auto other = std::make_shared<Part>();
    other->setParent(model.get());
    motor->setPart1(other.get());
    const CFrame limbBefore = limb->cframe();
    CHECK_EQ(solver.solve(), 2u);
    CHECK(limb->cframe() == limbBefore);
    CHECK(other->cframe() == root->cframe() * motor->c0() * motor->transform() * motor->c1().inverse());
    hat->destroy();
    CHECK_EQ(solver.solve(), 1u);
}

TEST(avatar_constraint_joints_place_parts_like_motors)
{
    // An upgraded avatar's joint: an AnimationConstraint between two
    // RigAttachments in place of the Motor6D, Transform between them; an
    // accessory held by a RigidConstraint. A nested attachment counts from
    // its part. Both kinds follow the constraint's Enabled.
    auto model = std::make_shared<Model>();
    auto root = std::make_shared<Part>();
    root->setName("HumanoidRootPart");
    root->setCFrame(CFrame(1.0f, 2.0f, 3.0f) * CFrame::angles(0.0f, 0.7f, 0.0f));
    root->setParent(model.get());
    auto limb = std::make_shared<Part>();
    limb->setParent(model.get());
    auto hat = std::make_shared<Part>();
    hat->setParent(model.get());
    auto attach = [](Part& part, const CFrame& frame) {
        auto a = std::make_shared<Attachment>();
        a->setCFrame(frame);
        a->setParent(&part);
        return a;
    };
    auto a0 = attach(*root, CFrame(0.0f, 1.0f, 0.0f));
    auto a1 = attach(*limb, CFrame(0.0f, -0.5f, 0.0f) * CFrame::angles(0.2f, 0.0f, 0.0f));
    auto joint = std::make_shared<AnimationConstraint>();
    joint->setAttachment0(a0.get());
    joint->setAttachment1(a1.get());
    joint->setIsKinematic(true);
    joint->setTransform(CFrame::angles(0.0f, 0.0f, 0.4f));
    joint->setParent(limb.get());
    auto outer = attach(*limb, CFrame(0.0f, 0.5f, 0.0f));
    auto inner = std::make_shared<Attachment>();
    inner->setCFrame(CFrame(0.1f, 0.0f, 0.0f));
    inner->setParent(outer.get());
    auto handle = attach(*hat, CFrame(0.0f, -0.2f, 0.0f));
    auto rigid = std::make_shared<RigidConstraint>();
    rigid->setAttachment0(inner.get());
    rigid->setAttachment1(handle.get());
    rigid->setParent(hat.get());

    CHECK_EQ(solveCharacterJoints(*model), 2u);
    const CFrame expectedLimb = root->cframe() * a0->cframe() * joint->transform() * a1->cframe().inverse();
    CHECK(near(limb->cframe(), expectedLimb));
    CHECK(near(hat->cframe(), expectedLimb * outer->cframe() * inner->cframe() * handle->cframe().inverse()));
    CHECK(constraintOffset(*rigid).has_value());

    // The cached solver agrees, and lets go when the joint is disabled.
    CharacterJointSolver solver(model);
    limb->setCFrame({});
    joint->setTransform(CFrame::angles(0.3f, 0.0f, 0.0f));
    CHECK_EQ(solver.solve(), 2u);
    CHECK(near(limb->cframe(), root->cframe() * a0->cframe() * joint->transform() * a1->cframe().inverse()));
    joint->setEnabled(false);
    CHECK_EQ(solver.solve(), 0u);
    // Without its attachments there is no offset.
    rigid->setAttachment0(nullptr);
    CHECK(!constraintOffset(*rigid));
}

TEST(avatar_build_rig_from_attachments)
{
    auto model = std::make_shared<Model>();
    auto root = std::make_shared<Part>();
    root->setName("HumanoidRootPart");
    root->setParent(model.get());
    auto torso = std::make_shared<Part>();
    torso->setName("LowerTorso");
    torso->setParent(model.get());
    auto a0 = std::make_shared<Attachment>();
    a0->setName("RootRigAttachment");
    a0->setCFrame(CFrame(0.0f, -1.0f, 0.0f));
    a0->setParent(root.get());
    auto a1 = std::make_shared<Attachment>();
    a1->setName("RootRigAttachment");
    a1->setCFrame(CFrame(0.0f, 0.2f, 0.0f));
    a1->setParent(torso.get());

    auto joints = buildRigFromAttachments(*model);
    CHECK_EQ(joints.size(), 1u);
    Motor6D* root6d = joints.front();
    CHECK_EQ(root6d->name(), "Root");
    CHECK(root6d->parent() == torso.get());
    CHECK(root6d->part0() == root.get());
    CHECK(root6d->part1() == torso.get());
    CHECK(near(root6d->c0(), a0->cframe()));
    CHECK(near(root6d->c1(), a1->cframe()));
    // Running it again reuses the joint.
    CHECK(buildRigFromAttachments(*model).front() == root6d);
}
