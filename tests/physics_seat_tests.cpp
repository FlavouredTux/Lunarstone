#include "framework.hpp"

// Seats: a character sits on the seat it touches (the SeatWeld, Sit,
// SeatPart, Occupant and the Seated event), gets up on a jump or when the
// weld goes, rides along with the seat, and drives a VehicleSeat.

#include <lunar/Classes.hpp>
#include <lunar/Seats.hpp>
#include <lunar/physics/Character.hpp>
#include <lunar/physics/HumanoidController.hpp>
#include <lunar/physics/World.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using namespace lunar;
using namespace lunar::physics;
using State = Enum::HumanoidStateType;

namespace {

bool near(float a, float b, float eps)
{
    return std::abs(a - b) <= eps;
}

// A placeholder character dropped from above onto a 4x1x4 seat at the origin.
template <class SeatType>
struct SeatRig {
    std::shared_ptr<DataModel> game = DataModel::create();
    Workspace& ws = *game->workspace();
    std::shared_ptr<SeatType> seat = std::make_shared<SeatType>();
    std::shared_ptr<Model> character = buildPlaceholderCharacter("Player1");
    Humanoid& humanoid = *static_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid"));
    BasePart& root = *humanoid.rootPart();
    std::unique_ptr<World> world;
    std::unique_ptr<HumanoidController> controller;
    std::vector<std::pair<bool, BasePart*>> seated;

    explicit SeatRig(bool anchored = true)
    {
        auto plate = std::make_shared<Part>();
        plate->setSize({512.0f, 20.0f, 512.0f});
        plate->setPosition({0.0f, -10.0f, 0.0f});
        plate->setAnchored(true);
        plate->setParent(&ws);
        seat->setSize({4.0f, 1.0f, 4.0f});
        seat->setPosition({0.0f, 0.5f, 0.0f});
        seat->setAnchored(anchored);
        seat->setParent(&ws);
        character->pivotTo(CFrame(0.0f, 5.0f, 0.0f) * character->getPivot());
        character->setParent(&ws);
        humanoid.events.seated.connect([this](bool active, BasePart* part) { seated.emplace_back(active, part); });
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
    bool sitting() const { return humanoid.sit() && humanoid.seatPart() == seat.get(); }
};

} // namespace

TEST(physics_seat_sits_on_touch_and_jumps_off)
{
    SeatRig<Seat> r;
    r.run(1.0f);
    CHECK(r.sitting());
    CHECK(r.seat->occupant() == &r.humanoid);
    CHECK(r.humanoid.getState() == State::Seated);
    CHECK_EQ(r.seated.size(), 1u);
    CHECK(r.seated.front().first);
    CHECK(r.seated.front().second == r.seat.get());
    // The client's SeatWeld, in the seat.
    Weld* weld = seatWeld(*r.seat, r.humanoid);
    CHECK(weld != nullptr);
    CHECK(weld->name() == "SeatWeld");
    CHECK(weld->parent() == r.seat.get());
    CHECK(weld->part0() == r.seat.get());
    CHECK(weld->part1() == &r.root);
    const CFrame turn = CFrame::angles(-std::numbers::pi_v<float> * 0.5f, 0.0f, 0.0f);
    CHECK((weld->c0().position - Vector3{0.0f, 0.5f, 0.0f}).magnitude() < 1e-5f);
    CHECK(weld->c0().rotation().lookVector().y < -0.999f); // looking down
    CHECK((weld->c1().rotation().lookVector() - turn.lookVector()).magnitude() < 1e-5f);
    // Held upright above the seat by the weld.
    const float above = r.root.position().y - 1.0f;
    CHECK(near(above, -weld->c1().position.y, 1e-3f));
    CHECK(r.root.cframe().upVector().y > 0.999f);

    // A jump gets it up and away.
    r.humanoid.setJump(true);
    r.run(2.0f / stepRate);
    r.humanoid.setJump(false);
    CHECK(!r.humanoid.sit());
    CHECK(r.humanoid.seatPart() == nullptr);
    CHECK(r.seat->occupant() == nullptr);
    CHECK(seatWeld(*r.seat, r.humanoid) == nullptr);
    const State up = r.humanoid.getState();
    const bool airborne = up == State::Jumping || up == State::Freefall;
    CHECK(airborne);
    CHECK_EQ(r.seated.size(), 2u);
    CHECK(!r.seated.back().first);
    r.run(0.15f);
    CHECK(r.root.assemblyLinearVelocity().y > 0.0f); // still going up
    CHECK(!r.humanoid.sit());
    // Landing back on it a while later, it sits again.
    r.run(3.0f);
    CHECK(r.sitting());
}

TEST(physics_seat_scripts_and_disabled)
{
    {
        // Seat:Sit from a script, then a script destroying the weld: up again.
        SeatRig<Seat> r;
        r.seat->setPosition({20.0f, 0.5f, 0.0f}); // out of reach
        r.run(0.5f);
        CHECK(!r.humanoid.sit());
        CHECK(sit(*r.seat, r.humanoid));
        r.run(0.5f);
        CHECK(r.sitting());
        CHECK(near(r.root.position().x, 20.0f, 1e-3f));
        CHECK(!sit(*r.seat, *static_cast<Humanoid*>(r.humanoid.clone().get()))); // taken
        seatWeld(*r.seat, r.humanoid)->destroy();
        r.run(0.1f);
        CHECK(!r.humanoid.sit());
        CHECK(r.seat->occupant() == nullptr);
        // Humanoid.Sit = false from a script gets it up too.
        CHECK(sit(*r.seat, r.humanoid));
        r.run(0.1f);
        r.humanoid.setSit(false);
        r.run(0.1f);
        CHECK(r.humanoid.seatPart() == nullptr);
        CHECK(seatWeld(*r.seat, r.humanoid) == nullptr);
    }
    {
        // A Disabled seat is only a part to stand on, but Sit still seats.
        SeatRig<Seat> r;
        r.seat->setDisabled(true);
        r.run(1.0f);
        CHECK(!r.humanoid.sit());
        CHECK(r.humanoid.getState() != State::Seated);
        CHECK(sit(*r.seat, r.humanoid));
        r.run(0.1f);
        CHECK(r.sitting());
    }
}

TEST(physics_seat_rides_along)
{
    // An unanchored VehicleSeat carried along at 5 studs/s takes its occupant with it.
    SeatRig<VehicleSeat> r(false);
    auto mover = std::make_shared<BodyVelocity>();
    mover->setVelocity({0.0f, 0.0f, 0.0f});
    mover->setMaxForce({1e7f, 0.0f, 1e7f});
    mover->setParent(r.seat.get());
    r.run(1.0f);
    CHECK(r.sitting());
    CHECK(r.seat->occupant() == &r.humanoid);
    mover->setVelocity({5.0f, 0.0f, 0.0f});
    r.run(2.0f);
    CHECK(r.sitting());
    CHECK(near(r.root.position().x, r.seat->position().x, 1e-2f));
    CHECK(r.seat->position().x > 9.0f);
}

TEST(physics_vehicle_seat_takes_controls)
{
    VehicleSeat seat;
    driveVehicleSeat(seat, {1.0f, 0.0f, -1.0f}); // W and D
    CHECK_EQ(seat.throttleFloat(), 1.0f);
    CHECK_EQ(seat.steerFloat(), 1.0f);
    CHECK_EQ(seat.throttle(), 1);
    CHECK_EQ(seat.steer(), 1);
    driveVehicleSeat(seat, {-0.3f, 0.0f, 0.8f}); // a stick half back, a little left
    CHECK_EQ(seat.throttleFloat(), -0.8f);
    CHECK_EQ(seat.steerFloat(), -0.3f);
    CHECK_EQ(seat.throttle(), -1);
    CHECK_EQ(seat.steer(), 0);
}
