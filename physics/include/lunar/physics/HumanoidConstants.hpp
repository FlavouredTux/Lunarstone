#pragma once

// Every number the character controller uses, with where it came from.
// "binary" values were read out of RobloxPlayer 0.742 (Mac, x86_64) at the
// given function/VA so they can be rechecked after an update; "guess" marks
// what couldn't be confirmed there.

#include <array>
#include <cstdint>

namespace lunar::physics::humanoid {

// --- State machine ------------------------------------------------------------------
// The events the state machine reacts to, in priority order: each step the
// first event (lowest number) whose target state is enabled and whose
// condition holds wins. Binary: evaluator 0x10369a54a, conditions in the
// switch at 0x10369b5e0.
enum class Event : std::uint8_t {
    NoHealth,      // Health <= 0
    NoNeck,        // RequiresNeck and no Neck joint
    JumpCommand,   // Humanoid.Jump (see jump rules below)
    Sit,           // Humanoid.Sit
    NoSit,         // !Humanoid.Sit
    PlatformStand, // Humanoid.PlatformStand
    NoPlatformStand,
    Tipped,     // up.y < tippedUpY, unless the state is tip-proof
    Upright,    // up.y > uprightUpY
    FaceLadder, // the climb sensor sees something climbable
    AwayLadder,
    NoFloor,      // no floor under the root part
    OffFloorLong, // no floor for longer than offFloorTime
    OnFloor,      // a floor, and not moving up away from it
    Done,         // the state finished its job (Jumping: impulse applied)
    TimerUp,      // the state's timer ran out
    InWater,
    OutOfWater,
};
constexpr int eventCount = 18;

// Binary: TIPPED compares UpVector.y < 0.7 (0x10369b69a), UPRIGHT > 0.9 (0x10369b730).
constexpr float tippedUpY = 0.7f;
constexpr float uprightUpY = 0.9f;
// Binary: OFF_FLOOR_LONG fires after 0.125 s without a floor (0x10369b648).
constexpr float offFloorTime = 0.125f;
// Binary: JUMP_CMD is refused after 0.13 s without a floor, except from the
// running states and PlatformStanding (0x10369b93b): the coyote window.
constexpr float jumpCoyoteTime = 0.13f;

// State timers set by each state's constructor (binary); 0 for states without one.
constexpr float fallingDownTime = 3.0f; // FallingDown 0x103631880
constexpr float ragdollTime = 5.0f;     // RagdollDefaultTimerTenthSecond = 50
constexpr float jumpingTime = 0.5f;     // Jumping 0x10369e364
constexpr float landedTime = 0.05f;     // Landed 0x1036a3d02

// --- Jump ----------------------------------------------------------------------------
// Binary: Jumping's update (0x10369e3d0) pushes the root's vertical speed up
// to 1.06 times jumpVelocity(); with UseJumpPower off, jumpVelocity() is
// sqrt(2 g JumpHeight) / 1.06 (0x103639abe), so JumpHeight comes out exact
// while JumpPower 50 launches at 53 studs/s. The push is clamped to 1000.
constexpr float jumpBoost = 1.06f;
constexpr float maxJumpVelocity = 1000.0f;
// Binary flag SolidFloorPercentForceApplication = 100: the floor takes the
// opposite impulse in full (10% for non-solid floors).
constexpr float floorReactionShare = 1.0f;

// --- Ground and air controllers (run every 240 Hz step) ---------------------------------
// Binary: RunningBase force callback 0x1036a4366 (constants confirmed, how
// they combine likely). Horizontal: a = 150 (vTarget - v) (gain at
// 0x103696e36), vTarget = the floor's velocity + walk velocity, |a| capped at
// 196.2 * friction * assembly mass on a floor (0x103655b58, 196.2 a constant
// at 0x1074c9dd0, not Workspace.Gravity; friction is the floor's alone: its
// CustomPhysicalProperties when set, else its material's), 143 without one.
constexpr float walkGain = 150.0f;
constexpr float frictionAccelerationScale = 196.2f;
constexpr float noFloorAcceleration = 143.0f;
// Vertical: a = 3000 err - 110 (v - floor v), err (studs below the stand
// height) clamped to [-0.1 d, 1.0 d] with d the hip height (0x1036a4b6c,
// 0x1036a4b74; MaxAltitudePDHipHeightPercent = 100). Gravity is fully taken
// off while running (RunningBaseGravityReductionFactorHundredth = 100).
constexpr float altitudeKp = 3000.0f;
constexpr float altitudeKd = 110.0f;
constexpr float altitudeMinErrorFraction = -0.1f;
constexpr float altitudeMaxErrorFraction = 1.0f;
// Turning: angular acceleration about up = 100 (wanted - current yaw rate),
// clamped to 5000 on the ground (0x1036a4985, 0x1036a49f9) and 160000 in the
// air (Freefall callback 0x103632660).
constexpr float turnGain = 100.0f;
constexpr float groundTurnLimit = 5000.0f;
constexpr float airTurnLimit = 160000.0f;
// AutoRotate (Humanoid 0x1036418e6): wanted yaw rate = 8 x yaw error towards
// the move direction. CameraRelative holds it to the camera's yaw instead
// (HumanoidController::balance).
constexpr float autoRotateRate = 8.0f;
// Floor probing past the stand height, so walking down slopes keeps the floor. Guess.
constexpr float floorSearchExtra = 1.5f;

// --- Balance ---------------------------------------------------------------------------
// The upright controller's PD gains per state (binary, state fields +0xf8 kP
// and +0xfc kD): Balancing default 7000/100 (0x103626d12), Freefall kP 5000,
// GettingUp 5000/300, Landed 7500/50. Treated as angular acceleration per
// radian of tilt and per rad/s, which gives a stiff, slightly underdamped
// spring (an interpretation, not confirmed).
struct BalanceGains {
    float kP;
    float kD;
};
constexpr BalanceGains balanceDefault{7000.0f, 100.0f};
constexpr BalanceGains balanceFreefall{5000.0f, 100.0f};
constexpr BalanceGains balanceGettingUp{5000.0f, 300.0f};
constexpr BalanceGains balanceLanded{7500.0f, 50.0f};

// --- Speed events ----------------------------------------------------------------------
// Binary (0x10369bbb0): while running, climbing or swimming the speed event
// fires again only when the speed (0 below 0.5) changes sign or leaves 5% of
// the last value fired.
constexpr float speedEventFloor = 0.5f;
constexpr float speedEventBand = 0.05f;

// --- Other -----------------------------------------------------------------------------
// MoveTo gives up after 8 s (documented); reach distance is a guess.
constexpr float moveToTimeout = 8.0f;
constexpr float moveToReachDistance = 1.0f;
// Climbing speed as a fraction of WalkSpeed: 0x1036a5f7e scales a speed by
// 0.7 (likely this).
constexpr float climbSpeedFactor = 0.7f;
// The climb sensor's reach in front of the root part. Guess.
constexpr float ladderReach = 1.5f;
// The root part counts as in water above this water fraction. Guess.
constexpr float swimWaterFraction = 0.5f;

// GUESS: how long a seat ignores the character it last seated or let go
// of. The client's Seat waits an FInt's worth of seconds since its last sit
// (Seat vfn 205, the Seat's +0x1c8 time) that isn't in the binary's data.
constexpr float seatDebounce = 1.0f;

} // namespace lunar::physics::humanoid
