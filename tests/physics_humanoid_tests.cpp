#include "framework.hpp"

// The Humanoid character controller: standing at HipHeight, walking at
// WalkSpeed, jumping to the height JumpPower gives, and the state sequence
// and events of a jump.

#include <lunar/Classes.hpp>
#include <lunar/physics/Character.hpp>
#include <lunar/physics/HumanoidConstants.hpp>
#include <lunar/physics/HumanoidController.hpp>
#include <lunar/physics/Materials.hpp>
#include <lunar/physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::physics;
using State = Enum::HumanoidStateType;

namespace {

bool near(float a, float b, float eps)
{
    return std::abs(a - b) <= eps;
}

struct Rig {
    std::shared_ptr<DataModel> game = DataModel::create();
    Workspace& ws = *game->workspace();
    std::shared_ptr<Model> character = buildPlaceholderCharacter("Player1");
    Humanoid& humanoid = *static_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid"));
    BasePart& root = *humanoid.rootPart();
    std::unique_ptr<World> world;
    std::unique_ptr<HumanoidController> controller;

    Rig()
    {
        auto plate = std::make_shared<Part>();
        plate->setName("Baseplate");
        plate->setSize({512.0f, 20.0f, 512.0f});
        plate->setPosition({0.0f, -10.0f, 0.0f});
        plate->setAnchored(true);
        plate->setParent(&ws);
        character->setParent(&ws);
        world = std::make_unique<World>(ws);
        controller = std::make_unique<HumanoidController>(*world, humanoid);
        world->onStep = [this](float dt) { controller->step(dt); };
    }

    void run(float seconds) const
    {
        const int steps = static_cast<int>(std::lround(seconds * stepRate));
        for (int i = 0; i < steps; ++i)
            world->step();
    }
};

} // namespace

TEST(humanoid_placeholder_rig)
{
    auto character = buildPlaceholderCharacter("Dummy");
    auto* humanoid = static_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid"));
    CHECK(humanoid);
    CHECK(humanoid->rigType() == Enum::HumanoidRigType::R15);
    CHECK_EQ(humanoid->hipHeight(), 2.0f);
    CHECK(humanoid->rootPart());
    CHECK(humanoid->rootPart()->size() == (Vector3{2.0f, 2.0f, 1.0f}));
    CHECK(character->primaryPart() == humanoid->rootPart());
    CHECK(character->findFirstChild("Head"));
    CHECK(character->findFirstChild("LeftFoot"));
}

TEST(humanoid_stands_at_hip_height)
{
    Rig rig;
    rig.run(1.0f);
    CHECK(rig.humanoid.getState() == State::Running);
    // Root centre: floor + HipHeight + half the root's height.
    // The hip correction leaves errors under 0.05 / 0.8 studs alone.
    CHECK(near(rig.root.position().y, 3.0f, 0.0625f));
    CHECK(rig.root.assemblyLinearVelocity().magnitude() < 0.05f);
    CHECK(rig.controller->floor());
    CHECK(rig.humanoid.floorMaterial() == Enum::Material::Plastic);
    CHECK(rig.root.cframe().upVector().y > 0.9999f);
}

TEST(humanoid_walks_at_walk_speed)
{
    Rig rig;
    rig.run(0.5f);
    std::vector<float> speeds;
    rig.humanoid.events.running.connect([&](float s) { speeds.push_back(s); });
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(1.0f);
    const Vector3 v = rig.root.assemblyLinearVelocity();
    CHECK(near(std::hypot(v.x, v.z), 16.0f, 0.01f));
    CHECK(near(rig.humanoid.moveDirection().z, -1.0f, 1e-6f));
    CHECK(!speeds.empty());
    // Re-fired only when leaving 5% of the last value (0x10369bbb0).
    CHECK(near(speeds.back(), 16.0f, 16.0f * 0.05f));
    // Faces where it walks.
    CHECK(rig.root.cframe().lookVector().z < -0.999f);
    // Diagonal input isn't faster.
    rig.controller->move({1.0f, 0.0f, -1.0f});
    rig.run(1.0f);
    const Vector3 d = rig.root.assemblyLinearVelocity();
    CHECK(near(std::hypot(d.x, d.z), 16.0f, 0.01f));
    // Stopping.
    rig.controller->move({});
    rig.run(1.0f);
    CHECK(rig.root.assemblyLinearVelocity().magnitude() < 0.05f);
}

// Anchored, the character is held still, but MoveDirection still says what
// it's told to walk: the game anchors you in a wardrobe and lets you out when
// MoveDirection goes past 0.5, so it has to follow the keys, up and back
// down, rather than stay at what it was when the anchoring came.
TEST(humanoid_anchored_move_direction_follows_input)
{
    Rig rig;
    rig.run(0.5f);
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(0.25f);
    rig.root.setAnchored(true);
    rig.controller->move({});
    rig.run(0.25f);
    CHECK(rig.humanoid.moveDirection().magnitude() < 1e-6f);
    const Vector3 held = rig.root.position();
    rig.controller->move({1.0f, 0.0f, 0.0f});
    rig.run(0.25f);
    CHECK(near(rig.humanoid.moveDirection().x, 1.0f, 1e-6f));
    CHECK((rig.root.position() - held).magnitude() < 1e-6f);
    rig.controller->move({});
    rig.run(0.25f);
    CHECK(rig.humanoid.moveDirection().magnitude() < 1e-6f);
}

// A live server's R15 character has every limb colliding; the Humanoid
// takes the arms and legs out, so a ledge under the hip height doesn't catch
// the feet and the floor probe carries the character up it.
TEST(humanoid_walks_up_a_step_with_colliding_limbs)
{
    Rig rig;
    for (const InstancePtr& child : rig.character->children())
        if (auto* part = dynamic_cast<BasePart*>(child.get()))
            part->setCanCollide(true);
    auto step = std::make_shared<Part>();
    step->setSize({20.0f, 1.0f, 40.0f});
    step->setPosition({0.0f, 0.5f, -27.0f}); // its face at z = -7, its top at y = 1
    step->setAnchored(true);
    step->setParent(&rig.ws);
    rig.run(0.5f);
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(1.5f);
    CHECK(rig.root.position().z < -15.0f); // 24 studs on, unless the step stopped it
    CHECK(near(rig.root.position().y, 4.0f, 0.25f));
    CHECK(!static_cast<BasePart*>(rig.character->findFirstChild("LeftFoot"))->canCollide());
    CHECK(static_cast<BasePart*>(rig.character->findFirstChild("UpperTorso"))->canCollide());
}

TEST(humanoid_jump_height_and_states)
{
    Rig rig;
    rig.run(0.5f);
    std::vector<std::pair<State, State>> changes;
    std::vector<std::string> events;
    rig.humanoid.events.stateChanged.connect([&](State a, State b) {
        changes.emplace_back(a, b);
        events.emplace_back("StateChanged");
    });
    rig.humanoid.events.jumping.connect([&](bool on) { events.emplace_back(on ? "Jumping" : "Jumping-off"); });
    rig.humanoid.events.freeFalling.connect(
        [&](bool on) { events.emplace_back(on ? "FreeFalling" : "FreeFalling-off"); });
    rig.humanoid.events.running.connect([&](float) { events.emplace_back("Running"); });

    const float startY = rig.root.position().y;
    rig.humanoid.setJump(true);
    float apex = startY;
    for (int i = 0; i < static_cast<int>(stepRate); ++i) {
        rig.world->step();
        apex = std::max(apex, rig.root.position().y);
        if (rig.humanoid.getState() == State::Freefall)
            rig.humanoid.setJump(false);
    }
    // JumpPower 50 launches at 1.06 x 50 = 53 studs/s (Jumping 0x10369e3d0):
    // under Gravity 196.2 that peaks 53^2 / 2g = 7.16 studs up.
    // Stepping at 240 Hz adds about half a step's climb, v dt / 2.
    CHECK(near(apex - startY, 53.0f * 53.0f / (2.0f * 196.2f) + 53.0f * stepSize / 2.0f, 0.03f));
    CHECK(changes.size() >= 4);
    CHECK(changes[0] == std::pair(State::Running, State::Jumping));
    CHECK(changes[1] == std::pair(State::Jumping, State::Freefall));
    CHECK(changes[2] == std::pair(State::Freefall, State::Landed));
    CHECK(changes[3] == std::pair(State::Landed, State::Running));
    CHECK(near(rig.root.position().y, startY, 0.07f));
    // Each change: the old state's event off, the new one's on, then StateChanged.
    const std::vector<std::string> expected{
        "Running",         "Jumping",     "StateChanged", // Running -> Jumping
        "Jumping-off",     "FreeFalling", "StateChanged", // Jumping -> Freefall
        "FreeFalling-off", "FreeFalling", "StateChanged", // Freefall -> Landed
        "FreeFalling-off", "Running",     "StateChanged", // Landed -> Running
    };
    CHECK(events.size() >= expected.size());
    CHECK(std::equal(expected.begin(), expected.end(), events.begin()));
}

TEST(humanoid_jump_height_property)
{
    Rig rig;
    rig.humanoid.setUseJumpPower(false);
    rig.humanoid.setJumpHeight(7.2f);
    rig.run(0.5f);
    const float startY = rig.root.position().y;
    rig.humanoid.setJump(true);
    float apex = startY;
    for (int i = 0; i < static_cast<int>(stepRate); ++i) {
        rig.world->step();
        apex = std::max(apex, rig.root.position().y);
        rig.humanoid.setJump(false);
    }
    CHECK(near(apex - startY, 7.2f, 0.15f));
}

TEST(humanoid_falls_off_ledge_and_dies)
{
    Rig rig;
    rig.run(0.2f);
    rig.root.setPosition({0.0f, 50.0f, 0.0f});
    // Freefall only after 0.125 s without a floor.
    rig.run(0.1f);
    CHECK(rig.humanoid.getState() == State::Running);
    rig.run(0.05f);
    CHECK(rig.humanoid.getState() == State::Freefall);
    bool died = false;
    rig.humanoid.events.died.connect([&] { died = true; });
    rig.humanoid.setHealth(0.0f);
    rig.run(0.1f);
    CHECK(died);
    CHECK(rig.humanoid.getState() == State::Dead);
}

TEST(humanoid_binary_constants)
{
    using namespace lunar::physics::humanoid;
    // RobloxPlayer 0.742; sources in HumanoidConstants.hpp.
    CHECK_EQ(jumpBoost, 1.06f);
    CHECK_EQ(offFloorTime, 0.125f);
    CHECK_EQ(jumpCoyoteTime, 0.13f);
    CHECK_EQ(jumpingTime, 0.5f);
    CHECK_EQ(landedTime, 0.05f);
    CHECK_EQ(fallingDownTime, 3.0f);
    CHECK_EQ(tippedUpY, 0.7f);
    CHECK_EQ(uprightUpY, 0.9f);
    CHECK_EQ(walkGain, 150.0f);
    CHECK_EQ(noFloorAcceleration, 143.0f);
    CHECK_EQ(altitudeKp, 3000.0f);
    CHECK_EQ(altitudeKd, 110.0f);
    CHECK_EQ(autoRotateRate, 8.0f);
}

TEST(humanoid_material_table)
{
    // PhysicalProperties.new(material), from the client's table.
    const PhysicalProperties plastic = defaultPhysicalProperties(Enum::Material::Plastic);
    CHECK_EQ(plastic.density, 0.7f);
    CHECK_EQ(plastic.friction, 0.3f);
    CHECK_EQ(plastic.elasticity, 0.5f);
    const PhysicalProperties ice = defaultPhysicalProperties(Enum::Material::Ice);
    CHECK_EQ(ice.friction, 0.02f);
    CHECK_EQ(ice.frictionWeight, 3.0f);
    const PhysicalProperties rubber = defaultPhysicalProperties(Enum::Material::Rubber);
    CHECK_EQ(rubber.friction, 1.5f);
    CHECK_EQ(rubber.elasticity, 0.95f);
    // Weighted by FrictionWeight: ice wins over plastic.
    CHECK(near(combineFriction(ice, plastic), (0.02f * 3.0f + 0.3f) / 4.0f, 1e-6f));
}

TEST(humanoid_walk_acceleration_needs_friction)
{
    // The walk acceleration is capped at 196.2 x friction x mass: on a nearly
    // frictionless floor the character can hardly get going.
    auto speedAfter = [](std::optional<PhysicalProperties> floor) {
        Rig rig;
        static_cast<Part*>(rig.ws.findFirstChild("Baseplate"))->setCustomPhysicalProperties(floor);
        rig.run(0.5f);
        rig.controller->move({0.0f, 0.0f, -1.0f});
        rig.run(0.1f);
        const Vector3 v = rig.root.assemblyLinearVelocity();
        return std::hypot(v.x, v.z);
    };
    const float plastic = speedAfter(std::nullopt);
    PhysicalProperties slick;
    slick.friction = 0.0f;
    slick.frictionWeight = 100.0f;
    const float frictionless = speedAfter(slick);
    CHECK(plastic > 15.0f);
    CHECK(frictionless < 2.0f);
}

TEST(humanoid_walk_ignores_its_own_friction)
{
    // The game gives the root part Friction 0 at FrictionWeight 100; the walk
    // still grips, since only the floor's friction caps it (0x103655b58).
    Rig rig;
    PhysicalProperties slick;
    slick.friction = 0.0f;
    slick.frictionWeight = 100.0f;
    rig.root.setCustomPhysicalProperties(slick);
    rig.run(0.5f);
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(0.1f);
    const Vector3 v = rig.root.assemblyLinearVelocity();
    CHECK(std::hypot(v.x, v.z) > 15.0f);
    rig.controller->move({});
    rig.run(0.2f);
    CHECK(rig.root.assemblyLinearVelocity().magnitude() < 1.0f);
}

TEST(humanoid_coyote_jump)
{
    // Off a ledge, a jump still works within 0.13 s.
    Rig rig;
    rig.run(0.5f);
    rig.root.setPosition({0.0f, 40.0f, 0.0f});
    rig.run(0.05f);
    CHECK(rig.humanoid.getState() == State::Running);
    rig.humanoid.setJump(true);
    rig.world->step();
    CHECK(rig.humanoid.getState() == State::Jumping);
}

TEST(humanoid_upgraded_avatar_joints)
{
    // A live server's R15 characters come with kinematic AnimationConstraints
    // between the RigAttachments instead of Motor6Ds, and accessories held by
    // a RigidConstraint [capture]: they still make one assembly, which stands,
    // walks and keeps its Neck, and an animated Transform poses the limb.
    Rig rig;
    std::vector<std::shared_ptr<Motor6D>> motors;
    for (const InstancePtr& d : rig.character->descendants())
        if (auto motor = std::dynamic_pointer_cast<Motor6D>(d))
            motors.push_back(motor);
    CHECK(motors.size() >= 10);
    std::shared_ptr<AnimationConstraint> neck;
    for (const auto& motor : motors) {
        auto a0 = std::make_shared<Attachment>();
        a0->setName(motor->name() + "RigAttachment");
        a0->setCFrame(motor->c0());
        a0->setParent(motor->part0());
        auto a1 = std::make_shared<Attachment>();
        a1->setName(motor->name() + "RigAttachment");
        a1->setCFrame(motor->c1());
        a1->setParent(motor->part1());
        auto joint = std::make_shared<AnimationConstraint>();
        joint->setName(motor->name());
        joint->setIsKinematic(true);
        joint->setAttachment0(a0.get());
        joint->setAttachment1(a1.get());
        joint->setParent(motor->part1());
        if (motor->name() == "Neck")
            neck = joint;
        motor->destroy();
    }
    CHECK(neck);
    // A hat on the head.
    BasePart& head = *static_cast<BasePart*>(rig.character->findFirstChild("Head"));
    auto handle = std::make_shared<Part>();
    handle->setName("Handle");
    handle->setSize({1.0f, 1.0f, 1.0f});
    handle->setCFrame(head.cframe() * CFrame(0.0f, 1.0f, 0.0f));
    handle->setCanCollide(false);
    handle->setParent(rig.character.get());
    auto hatAttachment = std::make_shared<Attachment>();
    hatAttachment->setParent(handle.get());
    auto headAttachment = std::make_shared<Attachment>();
    headAttachment->setCFrame(CFrame(0.0f, 1.0f, 0.0f));
    headAttachment->setParent(&head);
    auto holder = std::make_shared<RigidConstraint>();
    holder->setAttachment0(headAttachment.get());
    holder->setAttachment1(hatAttachment.get());
    holder->setParent(handle.get());

    rig.world->refresh();
    const Assembly* assembly = rig.world->assemblyOf(&rig.root);
    CHECK(assembly && !assembly->anchored);
    CHECK(rig.world->assemblyOf(&head) == assembly);
    CHECK(rig.world->assemblyOf(handle.get()) == assembly);
    rig.run(1.0f);
    CHECK(rig.humanoid.getState() == State::Running);
    CHECK(near(rig.root.position().y, 3.0f, 0.0625f));
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(1.0f);
    CHECK(near(rig.root.assemblyLinearVelocity().magnitude(), 16.0f, 0.05f));
    CHECK(rig.humanoid.getState() == State::Running);
    // The hat stays on the head.
    CHECK((handle->position() - head.cframe().pointToWorldSpace({0.0f, 1.0f, 0.0f})).magnitude() < 1e-3f);
    // Turning the Neck's Transform turns the head with the next step.
    neck->setTransform(CFrame::angles(0.0f, 1.0f, 0.0f));
    rig.run(1.0f / 60.0f);
    const BasePart& upper = *static_cast<BasePart*>(rig.character->findFirstChild("UpperTorso"));
    const CFrame expected = upper.cframe() * neck->attachment0()->cframe() * neck->transform()
                            * neck->attachment1()->cframe().inverse();
    CHECK((head.position() - expected.position).magnitude() < 1e-3f);
    CHECK(head.cframe().lookVector().dot(expected.lookVector()) > 0.9999f);
    // Without its Neck the Humanoid dies, as with a Motor6D one.
    neck->destroy();
    rig.run(0.1f);
    CHECK(rig.humanoid.getState() == State::Dead);
}

TEST(humanoid_live_world_moves_only_our_character)
{
    // The live client's world (LiveCharacter): our character walks, someone
    // else's, unanchored but the network's to move, stays where it's put and
    // stops ours like a wall.
    Rig rig;
    auto other = buildPlaceholderCharacter("Someone");
    other->pivotTo(CFrame(0.0f, 3.0f, -10.0f) * other->getPivot().rotation());
    other->setParent(&rig.ws);
    rig.world->setSimulationFilter(
        [&](const BasePart& part) { return part.isDescendantOf(rig.character.get()); });
    const BasePart& theirRoot = *static_cast<BasePart*>(other->findFirstChild("HumanoidRootPart"));
    const Vector3 theirs = theirRoot.position();
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(2.0f);
    CHECK((theirRoot.position() - theirs).magnitude() < 1e-4f);
    CHECK(rig.root.position().z > -10.0f + 0.5f);
}

TEST(humanoid_survives_its_floor_being_destroyed_between_steps)
{
    // The floor is found in one step and read (for a seat) at the start of
    // the next; a part freed in between (a teleport tearing the elevator
    // down) must not be touched.
    Rig rig;
    auto platform = std::make_shared<Part>();
    platform->setSize({8.0f, 1.0f, 8.0f});
    platform->setPosition({0.0f, 10.0f, 0.0f});
    platform->setAnchored(true);
    platform->setParent(&rig.ws);
    rig.root.setPosition({0.0f, 14.0f, 0.0f});
    rig.run(1.0f);
    CHECK(rig.controller->floor().has_value());
    CHECK(rig.controller->floor()->livePart() == platform.get());
    platform->destroy();
    platform.reset();
    CHECK(rig.controller->floor()->livePart() == nullptr);
    rig.run(1.0f); // falls to the baseplate
    CHECK(rig.root.position().y < 5.0f);
}

TEST(humanoid_without_a_neck_yet_lives)
{
    // A character whose Neck hasn't arrived (or a game rig without one)
    // isn't killed for it; losing a neck it had still kills (above).
    Rig rig;
    for (const InstancePtr& d : rig.character->descendants())
        if (d->name() == "Neck")
            d->destroy();
    rig.run(0.5f);
    CHECK(rig.humanoid.getState() != State::Dead);
}

// RotationType CameraRelative (shift lock, first person): held to the
// camera's yaw, not easing towards it, standing or walking sideways.
TEST(humanoid_faces_the_camera_when_camera_relative)
{
    Rig rig;
    rig.run(0.5f);
    rig.controller->setFacing(Vector3{1.0f, -0.3f, 0.0f});
    rig.world->step();
    CHECK(rig.root.cframe().lookVector().x > 0.99f);
    rig.controller->setFacing(Vector3{0.0f, 0.0f, 1.0f});
    rig.world->step();
    CHECK(rig.root.cframe().lookVector().z > 0.99f);
    rig.controller->setFacing(Vector3{1.0f, -0.3f, 0.0f});
    rig.run(1.0f);
    CHECK(rig.root.cframe().lookVector().x > 0.99f);
    rig.controller->move({0.0f, 0.0f, -1.0f});
    rig.run(1.0f);
    CHECK(rig.root.cframe().lookVector().x > 0.99f);
    const Vector3 v = rig.root.assemblyLinearVelocity();
    CHECK(near(v.z, -16.0f, 0.05f));
    // Off again: back to facing the walk.
    rig.controller->setFacing(std::nullopt);
    rig.run(1.0f);
    CHECK(rig.root.cframe().lookVector().z < -0.99f);
}
