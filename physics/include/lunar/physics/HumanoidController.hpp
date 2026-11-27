#pragma once

// The engine side of a Humanoid: its state machine and the forces that walk,
// jump, climb and swim its character, run once per physics step. Matches the
// client's RBX::HUMAN state classes where the binary told us how (see
// HumanoidConstants.hpp for every value and where it came from).

#include <lunar/Classes.hpp>
#include <lunar/physics/World.hpp>

#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace lunar::physics {

// Registers itself as the Humanoid's driver for its lifetime, so scripts'
// ChangeState and friends reach it.
class HumanoidController final : public HumanoidDriver {
public:
    HumanoidController(World& world, Humanoid& humanoid);
    ~HumanoidController();
    HumanoidController(const HumanoidController&) = delete;
    HumanoidController& operator=(const HumanoidController&) = delete;

    Humanoid& humanoid() const { return humanoid_; }

    // Humanoid:Move: walk this way (world space, or camera-relative using
    // Workspace.CurrentCamera's yaw). Longer than a stud per second it's
    // cut down to a unit vector.
    void move(Vector3 direction, bool relativeToCamera) override;
    void move(Vector3 direction) { move(direction, false); }
    // MoveDirection to what move() said, for a character held still (its
    // root anchored, or the server moving it), which step() doesn't walk:
    // the client's Humanoid:Move sets it however the character is held, and
    // The game lets you out of a wardrobe when it goes past 0.5.
    void publishMoveDirection();
    // Humanoid:MoveTo: walk to a point (relative to part, if given) until
    // reached or 8 seconds pass; MoveToFinished fires either way.
    void moveTo(Vector3 point, BasePart* part) override;
    void moveTo(Vector3 point) { moveTo(point, nullptr); }
    // Humanoid:ChangeState, if the state is enabled.
    void changeState(Enum::HumanoidStateType state) override;
    // Humanoid:SetStateEnabled / GetStateEnabled (kept on the Humanoid).
    void setStateEnabled(Enum::HumanoidStateType state, bool enabled) { humanoid_.setStateEnabled(state, enabled); }
    bool stateEnabled(Enum::HumanoidStateType state) const { return humanoid_.stateEnabled(state); }
    // UserGameSettings.RotationType CameraRelative (first person, shift
    // lock): face this direction instead of the way it walks. nullopt goes
    // back to facing the move direction.
    void setFacing(std::optional<Vector3> direction) { facing_ = direction; }

    // One physics step; World::onStep calls it.
    void step(float dt);

    struct Floor {
        BasePart* part = nullptr;
        // The part, held weakly: the floor is found in one step and read
        // at the start of the next, and the part may be gone by then.
        std::weak_ptr<const Instance> held;
        BasePart* livePart() const { return held.expired() ? nullptr : part; }
        Vector3 position;
        Vector3 normal;
        Enum::Material material = Enum::Material::Air;
        float distance = 0.0f; // from the root part's centre down to the floor
    };
    const std::optional<Floor>& floor() const { return floor_; }
    // Why the state machine killed it ("no health", "no neck"); empty while alive.
    std::string_view deathCause() const { return deathCause_; }

private:
    // What each state keeps (the binary's HumanoidState fields).
    struct StateData {
        float timer = 0.0f;       // counts down; TimerUp at 0
        float noFloorTime = 0.0f; // seconds without a floor, carried across states
        bool done = false;        // Done event
        bool noTip = false;       // Tipped is ignored (a jump out of water)
    };

    void setState(Enum::HumanoidStateType next);
    bool condition(int event, Enum::HumanoidStateType state, const Assembly& assembly) const;
    std::optional<Floor> findFloor(const BasePart& root) const;
    bool findLadder(const BasePart& root) const;
    bool inWater(const BasePart& root) const;
    bool neckMissing() const;
    void keepLimbsFromColliding();
    float standHeight(const BasePart& root) const;
    float jumpVelocity() const;
    Vector3 walkDirection();
    void updateState(Assembly& assembly, BasePart& root, Vector3 walk, float dt);
    // Sits down on a seat the character touches, gets up when it jumps or
    // its SeatWeld goes.
    void updateSeat();
    void balance(Assembly& assembly, const BasePart& root, Vector3 facing, float dt) const;
    void fireStateSignal(Enum::HumanoidStateType state, bool active, float speed);
    void fireSpeedSignal(float speed);
    float stateSpeed(const Assembly& assembly) const;

    World& world_;
    Humanoid& humanoid_;
    Connection healthConnection_;
    Vector3 moveDirection_;
    std::optional<Vector3> facing_;
    std::optional<Floor> floor_;
    StateData data_;
    std::optional<float> lastSpeed_;
    bool dead_ = false;
    // MoveTo.
    std::optional<Vector3> moveToPoint_;
    BasePart* moveToPart_ = nullptr;
    float moveToTime_ = 0.0f;
    // Seats.
    float clock_ = 0.0f;                                  // seconds stepped
    std::string_view deathCause_;
    bool neckSeen_ = false; // the character has had its Neck (only losing it kills)
    std::unordered_map<const BasePart*, float> lastSeat_; // when each seat last took or let go of it
};

} // namespace lunar::physics
