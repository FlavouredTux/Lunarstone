#include "framework.hpp"

// Mechanical constraints and legacy movers in the physics world: PrismaticConstraint and
// CylindricalConstraint actuators, limits, the Current* readings, and
// sleeping and waking with what they join.

#include <lunar/Classes.hpp>
#include <lunar/physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <tuple>
#include <vector>

using namespace lunar;
using namespace lunar::physics;

namespace {

bool near(float a, float b, float eps = 1e-3f)
{
    return std::abs(a - b) <= eps;
}

std::shared_ptr<Part> addPart(Instance& parent, Vector3 position, Vector3 size, bool anchored)
{
    auto part = std::make_shared<Part>();
    part->setSize(size);
    part->setPosition(position);
    part->setAnchored(anchored);
    part->setCanCollide(false); // only the constraint acts
    part->setParent(&parent);
    return part;
}

std::shared_ptr<Attachment> attach(BasePart& part, CFrame cframe = {})
{
    auto attachment = std::make_shared<Attachment>();
    attachment->setCFrame(cframe);
    attachment->setParent(&part);
    return attachment;
}

void run(World& world, float seconds)
{
    const int steps = static_cast<int>(std::lround(seconds * stepRate));
    for (int i = 0; i < steps; ++i)
        world.step();
}

// An anchored base and a 2-stud cube (mass 5.6) 10 studs above it, on a
// slider along world X through the cube, with gravity on.
template <class T>
struct Slider {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Part> base = addPart(*game->workspace(), {0.0f, 0.0f, 0.0f}, {4.0f, 1.0f, 4.0f}, true);
    std::shared_ptr<Part> cube = addPart(*game->workspace(), {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    std::shared_ptr<Attachment> a0 = attach(*base, CFrame(0.0f, 10.0f, 0.0f));
    std::shared_ptr<Attachment> a1 = attach(*cube);
    std::shared_ptr<T> constraint = [this] {
        auto c = std::make_shared<T>();
        c->setAttachment0(a0.get());
        c->setAttachment1(a1.get());
        c->setParent(cube.get());
        return c;
    }();
    World world{*game->workspace()};
};

} // namespace

TEST(physics_prismatic_holds_on_its_axis)
{
    Slider<PrismaticConstraint> s;
    // Pushed sideways, spun and dropped: only the motion along the axis is left.
    s.cube->setAssemblyLinearVelocity({3.0f, 5.0f, -4.0f});
    s.cube->setAssemblyAngularVelocity({1.0f, 2.0f, 3.0f});
    run(s.world, 1.0f);
    CHECK(near(s.cube->position().y, 10.0f, 0.01f));
    CHECK(near(s.cube->position().z, 0.0f, 0.01f));
    CHECK(near(s.cube->position().x, 3.0f, 0.02f));
    CHECK(near(s.cube->assemblyLinearVelocity().x, 3.0f, 0.01f));
    CHECK(s.cube->assemblyAngularVelocity().magnitude() < 0.01f);
    CHECK(s.cube->cframe().rightVector().x > 0.9999f);
    CHECK(near(s.constraint->currentPosition(), s.cube->position().x, 1e-3f));
}

TEST(physics_prismatic_motor_limits_acceleration_and_force)
{
    {
        // MotorMaxAcceleration: 20 studs/s^2 up to Velocity 10.
        Slider<PrismaticConstraint> s;
        s.constraint->setActuatorType(Enum::ActuatorType::Motor);
        s.constraint->setVelocity(10.0f);
        s.constraint->setMotorMaxForce(1e6f);
        s.constraint->setMotorMaxAcceleration(20.0f);
        run(s.world, 0.25f);
        CHECK(near(s.cube->assemblyLinearVelocity().x, 5.0f, 0.1f));
        run(s.world, 0.75f);
        CHECK(near(s.cube->assemblyLinearVelocity().x, 10.0f, 0.01f));
        CHECK(near(s.cube->position().y, 10.0f, 0.01f));
        // 2.5 studs speeding up over half a second, 5 at full speed.
        CHECK(near(s.constraint->currentPosition(), 7.5f, 0.1f));
    }
    {
        // MotorMaxForce: 22.4 on a 5.6 mass is 4 studs/s^2.
        Slider<PrismaticConstraint> s;
        s.constraint->setActuatorType(Enum::ActuatorType::Motor);
        s.constraint->setVelocity(10.0f);
        s.constraint->setMotorMaxForce(22.4f);
        run(s.world, 1.0f);
        CHECK(near(s.cube->assemblyLinearVelocity().x, 4.0f, 0.05f));
    }
    {
        // Backwards along the axis, the default MotorMaxForce of 0 does nothing.
        Slider<PrismaticConstraint> s;
        s.constraint->setActuatorType(Enum::ActuatorType::Motor);
        s.constraint->setVelocity(-10.0f);
        run(s.world, 0.5f);
        CHECK(near(s.cube->position().x, 0.0f, 1e-3f));
        s.constraint->setMotorMaxForce(1e6f);
        run(s.world, 0.5f);
        CHECK(near(s.cube->assemblyLinearVelocity().x, -10.0f, 0.01f));
    }
}

TEST(physics_prismatic_servo_reaches_target)
{
    Slider<PrismaticConstraint> s;
    s.constraint->setActuatorType(Enum::ActuatorType::Servo);
    s.constraint->setTargetPosition(5.0f);
    s.constraint->setSpeed(8.0f);
    s.constraint->setServoMaxForce(1e6f);
    float fastest = 0.0f;
    for (int i = 0; i < 240 * 2; ++i) {
        s.world.step();
        fastest = std::max(fastest, std::abs(s.cube->assemblyLinearVelocity().x));
    }
    CHECK(fastest <= 8.0f + 1e-3f); // Speed caps it
    CHECK(fastest > 7.9f);
    CHECK(near(s.constraint->currentPosition(), 5.0f, 0.01f));
    CHECK(std::abs(s.cube->assemblyLinearVelocity().x) < 0.05f);
    // Critically damped: it gets there without going past.
    s.constraint->setTargetPosition(-2.0f);
    float lowest = 5.0f;
    for (int i = 0; i < 240 * 3; ++i) {
        s.world.step();
        lowest = std::min(lowest, s.constraint->currentPosition());
    }
    CHECK(lowest > -2.02f);
    CHECK(near(s.constraint->currentPosition(), -2.0f, 0.01f));
}

TEST(physics_prismatic_limits)
{
    {
        // A motor runs into the upper limit and stays there.
        Slider<PrismaticConstraint> s;
        s.constraint->setActuatorType(Enum::ActuatorType::Motor);
        s.constraint->setVelocity(10.0f);
        s.constraint->setMotorMaxForce(1e6f);
        s.constraint->setLimitsEnabled(true);
        s.constraint->setLowerLimit(-1.0f);
        s.constraint->setUpperLimit(3.0f);
        run(s.world, 1.5f);
        CHECK(near(s.constraint->currentPosition(), 3.0f, 0.02f));
        CHECK(std::abs(s.cube->assemblyLinearVelocity().x) < 0.05f);
    }
    {
        // With Restitution 1 a free slider bounces off the lower one.
        Slider<PrismaticConstraint> s;
        s.constraint->setLimitsEnabled(true);
        s.constraint->setLowerLimit(-2.0f);
        s.constraint->setUpperLimit(2.0f);
        s.constraint->setRestitution(1.0f);
        s.cube->setAssemblyLinearVelocity({-20.0f, 0.0f, 0.0f});
        run(s.world, 0.25f);
        CHECK(s.constraint->currentPosition() > -2.02f);
        CHECK(near(s.cube->assemblyLinearVelocity().x, 20.0f, 0.5f));
    }
}

TEST(physics_cylindrical_turns_about_its_axis)
{
    Slider<CylindricalConstraint> s;
    s.cube->setAssemblyAngularVelocity({2.0f, 1.0f, 0.0f});
    run(s.world, 0.5f);
    // The turn about the axis is kept, the rest is held.
    CHECK(near(s.cube->assemblyAngularVelocity().x, 2.0f, 0.05f));
    CHECK(std::abs(s.cube->assemblyAngularVelocity().y) < 0.01f);
    CHECK(s.cube->cframe().rightVector().x > 0.9999f);
    CHECK(near(s.cube->position().y, 10.0f, 0.01f));
}

TEST(physics_constraint_current_readings)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto a = addPart(ws, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
    auto b = addPart(ws, {3.0f, 4.0f, -2.0f}, {1.0f, 1.0f, 1.0f}, true);
    // Attachment0's primary axis along world -Z.
    auto a0 = attach(*a, CFrame::angles(0.0f, std::numbers::pi_v<float> * 0.5f, 0.0f));
    auto a1 = attach(*b);
    auto prismatic = std::make_shared<PrismaticConstraint>();
    CHECK_EQ(prismatic->currentPosition(), 0.0f); // not attached
    prismatic->setAttachment0(a0.get());
    prismatic->setAttachment1(a1.get());
    CHECK(near(prismatic->currentPosition(), 2.0f));
    prismatic->setEnabled(false);
    CHECK_EQ(prismatic->currentPosition(), 0.0f);

    auto spring = std::make_shared<SpringConstraint>();
    spring->setAttachment0(a0.get());
    spring->setAttachment1(a1.get());
    CHECK(near(spring->currentLength(), std::sqrt(29.0f)));

    // Attachment1's secondary axis 30 degrees round Attachment0's primary one.
    auto hinge = std::make_shared<HingeConstraint>();
    auto h0 = attach(*a);
    auto h1 = attach(*b, CFrame::angles(std::numbers::pi_v<float> / 6.0f, 0.0f, 0.0f));
    hinge->setAttachment0(h0.get());
    hinge->setAttachment1(h1.get());
    CHECK(near(hinge->currentAngle(), 30.0f, 1e-3f));
    h1->setCFrame(CFrame::angles(-std::numbers::pi_v<float> * 0.75f, 0.0f, 0.0f));
    CHECK(near(hinge->currentAngle(), -135.0f, 1e-3f));
}

TEST(physics_constraints_sleep_and_wake)
{
    Slider<PrismaticConstraint> s;
    s.constraint->setActuatorType(Enum::ActuatorType::Motor);
    s.constraint->setMotorMaxForce(1e6f);
    run(s.world, 2.0f);
    // Held still by the motor, it sleeps like anything resting.
    CHECK(s.world.assemblyOf(s.cube.get())->asleep);
    // A new Velocity gets it going.
    s.constraint->setVelocity(4.0f);
    run(s.world, 0.5f);
    CHECK(!s.world.assemblyOf(s.cube.get())->asleep);
    CHECK(near(s.cube->assemblyLinearVelocity().x, 4.0f, 0.01f));

    // Two free bodies on one slider sleep and wake together.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, -1.0f, 0.0f}, {50.0f, 1.0f, 50.0f}, true)->setCanCollide(true);
    auto a = addPart(ws, {0.0f, 0.5f, 0.0f}, {4.0f, 2.0f, 4.0f}, false);
    a->setCanCollide(true);
    auto b = addPart(ws, {0.0f, 4.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    auto c = std::make_shared<PrismaticConstraint>();
    auto a0 = attach(*a, CFrame(0.0f, 3.5f, 0.0f));
    auto a1 = attach(*b);
    c->setAttachment0(a0.get());
    c->setAttachment1(a1.get());
    c->setParent(a.get());
    World world(ws);
    run(world, 3.0f);
    CHECK(world.assemblyOf(a.get())->asleep);
    CHECK(world.assemblyOf(b.get())->asleep);
    CHECK(near(b->position().y, 4.0f, 0.02f)); // held up through the slider
    b->setAssemblyLinearVelocity({2.0f, 0.0f, 0.0f});
    run(world, 0.1f);
    CHECK(!world.assemblyOf(a.get())->asleep);
    CHECK(!world.assemblyOf(b.get())->asleep);

    // Taking the constraint away lets it fall.
    run(world, 3.0f);
    c->setParent(nullptr);
    run(world, 0.5f);
    CHECK(b->position().y < 3.0f);
}

namespace {

// A 2-stud cube (mass 5.6, inertia 3.733 about each axis) hanging from an
// anchored base at (0, 20, 0) on a constraint between the world point
// `pivot` on the base (by default where onCube starts) and the cube's point
// `onCube`.
template <class T>
struct Hung {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Part> base = addPart(*game->workspace(), {0.0f, 20.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
    std::shared_ptr<Part> cube;
    std::shared_ptr<Attachment> a0;
    std::shared_ptr<Attachment> a1;
    std::shared_ptr<T> constraint = std::make_shared<T>();
    std::unique_ptr<World> world;

    Hung(Vector3 cubeAt, Vector3 onCube, std::optional<Vector3> pivot = {})
    {
        cube = addPart(*game->workspace(), cubeAt, {2.0f, 2.0f, 2.0f}, false);
        a0 = attach(*base, CFrame(pivot.value_or(cubeAt + onCube) - base->position()));
        a1 = attach(*cube, CFrame(onCube));
        constraint->setAttachment0(a0.get());
        constraint->setAttachment1(a1.get());
        constraint->setParent(cube.get());
        world = std::make_unique<World>(*game->workspace());
    }
    float gap() const { return (a1->worldPosition() - a0->worldPosition()).magnitude(); }
};

} // namespace

TEST(physics_hinge_motor)
{
    {
        // MotorMaxAcceleration 6 rad/s^2 up to 3 rad/s, about world X through the cube.
        Hung<HingeConstraint> h({0.0f, 10.0f, 0.0f}, {});
        h.constraint->setActuatorType(Enum::ActuatorType::Motor);
        h.constraint->setAngularVelocity(3.0f);
        h.constraint->setMotorMaxTorque(1e6f);
        h.constraint->setMotorMaxAcceleration(6.0f);
        run(*h.world, 0.25f);
        CHECK(near(h.cube->assemblyAngularVelocity().x, 1.5f, 0.03f));
        run(*h.world, 0.75f);
        CHECK(near(h.cube->assemblyAngularVelocity().x, 3.0f, 0.01f));
        CHECK(std::abs(h.cube->assemblyAngularVelocity().y) < 0.01f);
        CHECK(h.gap() < 0.01f); // held up against gravity
        // Turning the way AngularVelocity says moves CurrentAngle the same way.
        const float before = h.constraint->currentAngle();
        h.world->step();
        CHECK(h.constraint->currentAngle() > before);
    }
    {
        // MotorMaxTorque 7.467 on inertia 3.733 is 2 rad/s^2.
        Hung<HingeConstraint> h({0.0f, 10.0f, 0.0f}, {});
        h.constraint->setActuatorType(Enum::ActuatorType::Motor);
        h.constraint->setAngularVelocity(10.0f);
        h.constraint->setMotorMaxTorque(5.6f * 8.0f / 12.0f * 2.0f);
        run(*h.world, 1.0f);
        CHECK(near(h.cube->assemblyAngularVelocity().x, 2.0f, 0.05f));
    }
}

TEST(physics_hinge_servo_and_limits)
{
    {
        // Servo to 90 degrees at AngularSpeed 2 rad/s.
        Hung<HingeConstraint> h({0.0f, 10.0f, 0.0f}, {});
        h.constraint->setActuatorType(Enum::ActuatorType::Servo);
        h.constraint->setTargetAngle(90.0f);
        h.constraint->setAngularSpeed(2.0f);
        h.constraint->setServoMaxTorque(1e6f);
        float fastest = 0.0f;
        for (int i = 0; i < 240 * 2; ++i) {
            h.world->step();
            fastest = std::max(fastest, std::abs(h.cube->assemblyAngularVelocity().x));
        }
        CHECK(fastest <= 2.0f + 1e-3f);
        CHECK(near(h.constraint->currentAngle(), 90.0f, 0.5f));
        // From 170 to -170 the short way, through 180, not back through 0.
        h.constraint->setTargetAngle(170.0f);
        h.constraint->setAngularSpeed(10.0f);
        run(*h.world, 1.0f);
        CHECK(near(h.constraint->currentAngle(), 170.0f, 0.5f));
        h.constraint->setTargetAngle(-170.0f);
        float nearest = 180.0f;
        for (int i = 0; i < 240; ++i) {
            h.world->step();
            nearest = std::min(nearest, std::abs(h.constraint->currentAngle()));
        }
        CHECK(nearest > 160.0f);
        CHECK(near(h.constraint->currentAngle(), -170.0f, 0.5f));
        // With limits the target is held to them.
        h.constraint->setLimitsEnabled(true);
        h.constraint->setLowerAngle(-30.0f);
        h.constraint->setUpperAngle(30.0f);
        h.constraint->setTargetAngle(90.0f);
        run(*h.world, 2.0f);
        CHECK(near(h.constraint->currentAngle(), 30.0f, 0.5f));
    }
    {
        // A pendulum swung hard stays between its limits.
        Hung<HingeConstraint> h({0.0f, 17.0f, 0.0f}, {0.0f, 3.0f, 0.0f});
        h.constraint->setLimitsEnabled(true);
        h.constraint->setLowerAngle(-20.0f);
        h.constraint->setUpperAngle(20.0f);
        h.cube->setAssemblyLinearVelocity({0.0f, 0.0f, 15.0f});
        float widest = 0.0f;
        for (int i = 0; i < 240 * 2; ++i) {
            h.world->step();
            widest = std::max(widest, std::abs(h.constraint->currentAngle()));
        }
        CHECK(widest > 19.0f);
        CHECK(widest < 21.0f);
        CHECK(h.gap() < 0.01f);
    }
}

// The game's side doors (a room's DoorNormal) as the server builds them: an
// unanchored door on a HingeConstraint from its own attachment to an
// anchored Hinge part, a servo to -90 degrees at 5 rad/s with ServoMaxTorque
// 999, limits of +-90 [observed in a replay]. The
// server hands the door's network ownership to the player who opens it, whose
// client swings it: it opens a quarter turn about its hinge and stops there.
namespace {

// CFrame.new(x, y, z, R00, R01, R02, R10, ..., R22), as Roblox prints one.
CFrame cframe12(float x, float y, float z, float r00, float r01, float r02, float r10, float r11, float r12, float r20,
                float r21, float r22)
{
    CFrame cf(x, y, z);
    const float rows[3][3] = {{r00, r01, r02}, {r10, r11, r12}, {r20, r21, r22}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            cf.r[i][j] = rows[i][j];
    return cf;
}

} // namespace

TEST(physics_hinge_servo_swings_a_doors_door_to_its_target)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto door = addPart(ws, {0.0f, 4.0f, 0.0f}, {4.98f, 7.75f, 0.406f}, false);
    door->setCFrame(cframe12(0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f));
    auto hinge = addPart(ws, {0.0f, 4.0f, -2.375f}, {0.234f, 1.45f, 0.484f}, true);
    hinge->setCFrame(cframe12(0.0f, 4.0f, -2.375f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f));
    auto onDoor = attach(*door, cframe12(2.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f));
    auto onHinge
        = attach(*hinge, cframe12(-0.125f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f));
    auto constraint = std::make_shared<HingeConstraint>();
    constraint->setAttachment0(onDoor.get());
    constraint->setAttachment1(onHinge.get());
    constraint->setActuatorType(Enum::ActuatorType::Servo);
    constraint->setTargetAngle(-90.0f);
    constraint->setAngularSpeed(5.0f);
    constraint->setServoMaxTorque(999.0f);
    constraint->setAngularResponsiveness(45.0f);
    constraint->setLimitsEnabled(true);
    constraint->setLowerAngle(-90.0f);
    constraint->setUpperAngle(90.0f);
    constraint->setRestitution(0.5f);
    constraint->setParent(door.get());
    World world(ws);
    const Vector3 pivot = onHinge->worldPosition();
    const Vector3 closedCenter = door->position();
    float fastest = 0.0f;
    float farthest = 0.0f;
    for (int i = 0; i < static_cast<int>(stepRate * 2.0f); ++i) {
        world.step();
        fastest = std::max(fastest, door->assemblyAngularVelocity().magnitude());
        farthest = std::max(farthest, -constraint->currentAngle());
        CHECK((onDoor->worldPosition() - pivot).magnitude() < 0.05f); // on its hinge all the way
    }
    CHECK(near(constraint->currentAngle(), -90.0f, 1.0f));
    CHECK(farthest < 91.0f); // the limit holds it
    CHECK(fastest < 5.0f + 0.05f);
    // A quarter turn about the hinge: the center is as far from it as before,
    // square to where it was.
    const Vector3 was = closedCenter - pivot;
    const Vector3 now = door->position() - pivot;
    CHECK(near(now.magnitude(), was.magnitude(), 0.05f));
    CHECK(std::abs(now.unit().dot(was.unit())) < 0.03f);
    CHECK(near(door->position().y, 4.0f, 0.05f));
}

TEST(physics_spring_oscillates_and_damps)
{
    {
        // Stiffness 56 on mass 5.6: a period of 2 pi / sqrt(10) = 1.987 s.
        // Stretched a stud past FreeLength 4 and let go, it swings 1 stud either side.
        Hung<SpringConstraint> h({0.0f, 15.0f, 0.0f}, {}, Vector3{0.0f, 20.0f, 0.0f});
        h.game->workspace()->setGravity(0.0f);
        h.constraint->setFreeLength(4.0f);
        h.constraint->setStiffness(56.0f);
        h.world = std::make_unique<World>(*h.game->workspace());
        constexpr float period = 1.98692f;
        run(*h.world, period * 0.5f);
        CHECK(near(h.constraint->currentLength(), 3.0f, 0.03f));
        run(*h.world, period * 0.5f);
        CHECK(near(h.constraint->currentLength(), 5.0f, 0.03f));
        // Critical damping (2 sqrt(k m) = 35.4) stops it without overshooting.
        h.constraint->setDamping(35.4f);
        float shortest = 5.0f;
        for (int i = 0; i < 240 * 3; ++i) {
            h.world->step();
            shortest = std::min(shortest, h.constraint->currentLength());
        }
        CHECK(shortest > 3.99f);
        CHECK(near(h.constraint->currentLength(), 4.0f, 0.05f)); // asleep on the slow tail
    }
    {
        // MaxForce caps it: 5.6 on mass 5.6 pulls at 1 stud/s^2 at most.
        Hung<SpringConstraint> h({0.0f, 10.0f, 0.0f}, {}, Vector3{0.0f, 20.0f, 0.0f});
        h.game->workspace()->setGravity(0.0f);
        h.constraint->setFreeLength(1.0f);
        h.constraint->setStiffness(1000.0f);
        h.constraint->setMaxForce(5.6f);
        h.world = std::make_unique<World>(*h.game->workspace());
        run(*h.world, 1.0f);
        CHECK(near(h.cube->assemblyLinearVelocity().y, 1.0f, 0.01f));
    }
    {
        // With limits it can't stretch past MaxLength, whatever hangs on it.
        Hung<SpringConstraint> h({0.0f, 15.0f, 0.0f}, {}, Vector3{0.0f, 20.0f, 0.0f});
        h.constraint->setFreeLength(5.0f);
        h.constraint->setStiffness(1.0f);
        h.constraint->setLimitsEnabled(true);
        h.constraint->setMaxLength(8.0f);
        run(*h.world, 2.0f);
        CHECK(near(h.constraint->currentLength(), 8.0f, 0.05f));
    }
}

TEST(physics_rope_rod_and_ball_socket)
{
    {
        // A rope goes slack above, catches at its Length.
        Hung<RopeConstraint> h({0.0f, 18.0f, 0.0f}, {}, Vector3{0.0f, 20.0f, 0.0f});
        h.constraint->setLength(6.0f);
        run(*h.world, 2.0f);
        CHECK(near(h.constraint->currentDistance(), 6.0f, 0.05f));
        CHECK(near(h.cube->position().y, 14.0f, 0.05f));
    }
    {
        // A rod keeps its Length while swinging round.
        Hung<RodConstraint> h({0.0f, 16.0f, 0.0f}, {}, Vector3{0.0f, 20.0f, 0.0f});
        h.constraint->setLength(4.0f);
        h.cube->setAssemblyLinearVelocity({10.0f, 0.0f, 0.0f});
        float worst = 0.0f;
        for (int i = 0; i < 240 * 2; ++i) {
            h.world->step();
            worst = std::max(worst, std::abs(h.constraint->currentDistance() - 4.0f));
        }
        CHECK(worst < 0.02f);
    }
    {
        // A ball and socket at the cube's corner: the point stays put, the cube swings any way.
        Hung<BallSocketConstraint> h({0.0f, 18.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        h.cube->setAssemblyAngularVelocity({1.0f, 2.0f, 3.0f});
        float worst = 0.0f;
        for (int i = 0; i < 240 * 2; ++i) {
            h.world->step();
            worst = std::max(worst, h.gap());
        }
        CHECK(worst < 0.01f);
        CHECK(h.cube->assemblyAngularVelocity().magnitude() > 0.5f);
    }
}

TEST(physics_light_part_between_heavy_ones)
{
    // A 0.35 mass link hinged between an anchored base and a 700 mass block
    // (a car's knuckle between its frame and a wheel is 77 to 1): solved as
    // one block, the joints hold as if the link weren't there.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto base = addPart(ws, {0.0f, 20.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
    auto link = addPart(ws, {0.0f, 19.0f, 0.0f}, {0.5f, 2.0f, 0.5f}, false);
    auto heavy = addPart(ws, {0.0f, 13.0f, 0.0f}, {10.0f, 10.0f, 10.0f}, false);
    auto hinge = [&](BasePart& a, BasePart& b, Vector3 at) {
        auto h = std::make_shared<HingeConstraint>();
        auto a0 = attach(a, CFrame(at - a.position()));
        auto a1 = attach(b, CFrame(at - b.position()));
        h->setAttachment0(a0.get());
        h->setAttachment1(a1.get());
        h->setParent(&b);
        return std::tuple{h, a0, a1};
    };
    auto [h0, b0, b1] = hinge(*base, *link, {0.0f, 20.0f, 0.0f});
    auto [h1, c0, c1] = hinge(*link, *heavy, {0.0f, 18.0f, 0.0f});
    heavy->setAssemblyLinearVelocity({0.0f, 0.0f, 10.0f});
    World world(ws);
    float worst = 0.0f;
    for (int i = 0; i < 240 * 2; ++i) {
        world.step();
        worst = std::max({worst, (b1->worldPosition() - b0->worldPosition()).magnitude(),
                          (c1->worldPosition() - c0->worldPosition()).magnitude()});
    }
    CHECK(worst < 0.01f);
    CHECK(std::isfinite(heavy->position().y));
}

TEST(physics_forces_within_a_step_dont_bounce)
{
    // A wheel pressed into the ground by a force far beyond its weight (a
    // car's spring on it) rests: the speed the force adds in one step isn't
    // an impact to bounce back from.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, -1.0f, 0.0f}, {50.0f, 2.0f, 50.0f}, true)->setCanCollide(true);
    auto wheel = addPart(ws, {0.0f, 1.5f, 0.0f}, {1.2f, 3.0f, 3.0f}, false);
    wheel->setShape(Enum::PartType::Cylinder);
    wheel->setCanCollide(true);
    World world(ws);
    world.refresh();
    float fastest = 0.0f;
    for (int i = 0; i < 480; ++i) {
        world.assemblyOf(wheel.get())->force = {0.0f, -24600.0f, 0.0f};
        world.step();
        if (i > 240)
            fastest = std::max(fastest, std::abs(wheel->assemblyLinearVelocity().y));
    }
    CHECK(fastest < 0.01f);
}

TEST(physics_constrained_assemblies_sleep_together)
{
    // A heavy block on two light feet, each on a slider with a spring: every
    // step, the ones a constraint joins are asleep together or awake together.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, -1.0f, 0.0f}, {50.0f, 2.0f, 50.0f}, true)->setCanCollide(true);
    auto body = addPart(ws, {0.0f, 4.0f, 0.0f}, {6.0f, 2.0f, 2.0f}, false);
    body->setCustomPhysicalProperties(PhysicalProperties{20.0f, 0.3f, 0.5f, 1.0f, 1.0f});
    std::vector<std::shared_ptr<Instance>> keep;
    std::vector<std::shared_ptr<Part>> feet;
    for (float x : {-2.5f, 2.5f}) {
        auto foot = addPart(ws, {x, 1.0f, 0.0f}, {1.0f, 2.0f, 1.0f}, false);
        foot->setCanCollide(true);
        foot->setCustomPhysicalProperties(PhysicalProperties{20.0f, 0.3f, 0.5f, 1.0f, 1.0f});
        feet.push_back(foot);
        // Sliding up and down under the body, pushed down by a spring.
        const CFrame down = CFrame::angles(0.0f, 0.0f, -std::numbers::pi_v<float> * 0.5f);
        auto s0 = attach(*body, CFrame(x, 0.0f, 0.0f) * down);
        auto s1 = attach(*foot, CFrame(0.0f, 3.0f, 0.0f) * down);
        auto slide = std::make_shared<PrismaticConstraint>();
        slide->setAttachment0(s0.get());
        slide->setAttachment1(s1.get());
        slide->setParent(foot.get());
        auto spring = std::make_shared<SpringConstraint>();
        auto p0 = attach(*body, CFrame(x, 0.0f, 0.5f));
        auto p1 = attach(*foot, CFrame(0.0f, 0.0f, 0.5f));
        spring->setAttachment0(p0.get());
        spring->setAttachment1(p1.get());
        spring->setFreeLength(4.0f);
        spring->setStiffness(20000.0f);
        spring->setDamping(3000.0f);
        spring->setParent(foot.get());
        keep.insert(keep.end(), {s0, s1, slide, p0, p1, spring});
    }
    World world(ws);
    bool together = true;
    for (int i = 0; i < 240 * 4; ++i) {
        world.step();
        const bool asleep = world.assemblyOf(body.get())->asleep;
        for (const auto& foot : feet)
            together = together && world.assemblyOf(foot.get())->asleep == asleep;
    }
    CHECK(together);
    CHECK(world.assemblyOf(body.get())->asleep);
}

TEST(physics_body_velocity)
{
    {
        // A strong one holds the velocity, gravity or not.
        auto game = DataModel::create();
        auto cube = addPart(*game->workspace(), {0.0f, 100.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
        auto mover = std::make_shared<BodyVelocity>();
        mover->setVelocity({5.0f, -10.0f, 0.0f});
        mover->setMaxForce({1e6f, 1e6f, 1e6f});
        mover->setParent(cube.get());
        World world(*game->workspace());
        run(world, 0.5f);
        CHECK(near(cube->assemblyLinearVelocity().x, 5.0f, 1e-3f));
        CHECK(near(cube->assemblyLinearVelocity().y, -10.0f, 1e-3f));
        // A new Velocity takes at once.
        mover->setVelocity({0.0f, 3.0f, 0.0f});
        world.step();
        CHECK(near(cube->assemblyLinearVelocity().y, 3.0f, 1e-3f));
        CHECK(near(cube->assemblyLinearVelocity().x, 0.0f, 1e-3f));
    }
    {
        // MaxForce per axis: 5.6 * 98.1 holds back half of gravity on a 5.6 mass,
        // and none on X stops nothing there.
        auto game = DataModel::create();
        auto cube = addPart(*game->workspace(), {0.0f, 100.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
        cube->setAssemblyLinearVelocity({4.0f, 0.0f, 0.0f});
        auto mover = std::make_shared<BodyVelocity>();
        mover->setVelocity({});
        mover->setMaxForce({0.0f, 5.6f * 98.1f, 0.0f});
        mover->setParent(cube.get());
        World world(*game->workspace());
        run(world, 1.0f);
        CHECK(near(cube->assemblyLinearVelocity().y, -98.1f, 0.1f));
        CHECK(near(cube->assemblyLinearVelocity().x, 4.0f, 1e-3f));
        // Taken out, the part falls freely again.
        mover->setParent(nullptr);
        run(world, 0.5f);
        CHECK(near(cube->assemblyLinearVelocity().y, -98.1f - 98.1f, 0.2f));
    }
}

TEST(physics_body_position)
{
    {
        // Held up against gravity and pulled to its Position, whatever the mass.
        for (float size : {1.0f, 4.0f}) {
            auto game = DataModel::create();
            auto cube = addPart(*game->workspace(), {0.0f, 50.0f, 0.0f}, {size, size, size}, false);
            auto mover = std::make_shared<BodyPosition>();
            mover->setPosition({10.0f, 60.0f, -5.0f});
            mover->setMaxForce({1e7f, 1e7f, 1e7f});
            mover->setParent(cube.get());
            World world(*game->workspace());
            float furthest = 0.0f;
            for (int i = 0; i < 240 * 3; ++i) {
                world.step();
                furthest = std::max(furthest, cube->position().x);
            }
            CHECK((cube->position() - Vector3{10.0f, 60.0f, -5.0f}).magnitude() < 0.05f);
            CHECK(furthest < 10.01f); // the default P and D come out overdamped
        }
    }
    {
        // Too weak to hold the weight, it sags; with no force allowed on Y it falls.
        auto game = DataModel::create();
        auto cube = addPart(*game->workspace(), {0.0f, 50.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
        auto mover = std::make_shared<BodyPosition>();
        mover->setPosition({0.0f, 50.0f, 0.0f});
        mover->setMaxForce({4000.0f, 0.0f, 4000.0f});
        mover->setParent(cube.get());
        World world(*game->workspace());
        run(world, 1.0f);
        CHECK(near(cube->assemblyLinearVelocity().y, -196.2f, 0.1f));
        CHECK(near(cube->position().x, 0.0f, 1e-3f));
    }
}

namespace {

// The angle between two frames' orientations, in degrees.
float turnBetween(const CFrame& a, const CFrame& b)
{
    const CFrame d = b.rotation() * a.rotation().inverse();
    const float trace = d.r[0][0] + d.r[1][1] + d.r[2][2];
    return std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
}

// A free cube turned by an AlignOrientation toward an anchored base's
// attachment, in zero gravity. `onCube` puts Attachment0 on the cube.
struct Aligned {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Part> base = addPart(*game->workspace(), {0.0f, 0.0f, 0.0f}, {4.0f, 1.0f, 4.0f}, true);
    std::shared_ptr<Part> cube = addPart(*game->workspace(), {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    std::shared_ptr<Attachment> onBase = attach(*base, CFrame::fromOrientation(0.5f, 1.2f, -0.3f));
    std::shared_ptr<Attachment> onCube = attach(*cube);
    std::shared_ptr<AlignOrientation> align = std::make_shared<AlignOrientation>();
    std::unique_ptr<World> world;

    explicit Aligned(bool cubeFirst)
    {
        game->workspace()->setGravity(0.0f);
        align->setAttachment0(cubeFirst ? onCube.get() : onBase.get());
        align->setAttachment1(cubeFirst ? onBase.get() : onCube.get());
        align->setResponsiveness(20.0f);
        align->setParent(cube.get());
        world = std::make_unique<World>(*game->workspace());
    }
};

} // namespace

TEST(physics_align_orientation)
{
    {
        // Attachment1 on the cube turns to face as Attachment0 does, the
        // turn no faster than MaxAngularVelocity.
        Aligned a(false);
        a.align->setReactionTorqueEnabled(true);
        a.align->setMaxAngularVelocity(3.0f);
        CHECK(turnBetween(a.onCube->worldCFrame(), a.onBase->worldCFrame()) > 60.0f);
        float fastest = 0.0f;
        for (int i = 0; i < 240 * 3; ++i) {
            a.world->step();
            fastest = std::max(fastest, a.cube->assemblyAngularVelocity().magnitude());
        }
        CHECK(fastest <= 3.0f + 1e-2f);
        CHECK(turnBetween(a.onCube->worldCFrame(), a.onBase->worldCFrame()) < 1.0f);
        // It stays a turn: the cube hasn't moved.
        CHECK(near(a.cube->position().y, 10.0f, 1e-3f));
    }
    {
        // Without the reaction only Attachment0's part turns: the cube as
        // Attachment1 is left alone, as Attachment0 it turns.
        Aligned left(false);
        run(*left.world, 1.0f);
        CHECK(turnBetween(left.cube->cframe(), CFrame()) < 0.01f);
        Aligned turned(true);
        run(*turned.world, 2.0f);
        CHECK(turnBetween(turned.onCube->worldCFrame(), turned.onBase->worldCFrame()) < 1.0f);
    }
    {
        // PrimaryAxisOnly lines up the primary axes and leaves the cube free about them.
        Aligned a(true);
        a.align->setPrimaryAxisOnly(true);
        run(*a.world, 2.0f);
        CHECK(a.onCube->worldCFrame().rightVector().dot(a.onBase->worldCFrame().rightVector()) > 0.9998f);
        // Spun about that axis, nothing holds it back.
        const Vector3 axis = a.onBase->worldCFrame().rightVector();
        a.cube->setAssemblyAngularVelocity(axis * 2.0f);
        run(*a.world, 0.5f);
        CHECK(near(a.cube->assemblyAngularVelocity().dot(axis), 2.0f, 0.05f));
    }
    {
        // OneAttachment turns Attachment0 to the constraint's CFrame.
        auto game = DataModel::create();
        game->workspace()->setGravity(0.0f);
        auto cube = addPart(*game->workspace(), {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
        auto a0 = attach(*cube);
        auto align = std::make_shared<AlignOrientation>();
        align->setMode(Enum::OrientationAlignmentMode::OneAttachment);
        align->setAttachment0(a0.get());
        align->setCFrame(CFrame::fromOrientation(0.0f, 1.0f, 0.0f));
        align->setResponsiveness(20.0f);
        align->setParent(cube.get());
        World world(*game->workspace());
        run(world, 2.0f);
        CHECK(turnBetween(a0->worldCFrame(), align->cframe()) < 1.0f);
    }
}

// A mover a script gave a NaN (the game's dash once did, through something of
// ours) doesn't take its part with it: the axis that's NaN is left out, the
// others still hold, and a part that does go non-finite stays put.
TEST(physics_non_finite_mover_is_left_out)
{
    auto game = DataModel::create();
    auto cube = addPart(*game->workspace(), {0.0f, 100.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    auto mover = std::make_shared<BodyVelocity>();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    mover->setVelocity({nan, 0.0f, 0.0f});
    mover->setMaxForce({1e6f, 1e6f, 1e6f});
    mover->setParent(cube.get());
    World world(*game->workspace());
    run(world, 0.5f);
    const Vector3 at = cube->position();
    CHECK(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z));
    CHECK(near(at.y, 100.0f, 1e-2f)); // Y's row (Velocity 0) still holds it up
    // Fixed, it holds again.
    mover->setVelocity({0.0f, 2.0f, 0.0f});
    run(world, 0.5f);
    CHECK(near(cube->assemblyLinearVelocity().y, 2.0f, 1e-3f));

    // Motion that's NaN by the time it's integrated keeps the part where it was.
    cube->setAssemblyLinearVelocity({nan, 0.0f, 0.0f});
    mover->setParent(nullptr);
    const Vector3 before = cube->position();
    world.step();
    CHECK(std::isfinite(cube->position().x));
    CHECK(near(cube->position().y, before.y, 1.0f));
}
