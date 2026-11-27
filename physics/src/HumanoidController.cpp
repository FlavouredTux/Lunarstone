#include <lunar/physics/HumanoidController.hpp>

#include <lunar/Seats.hpp>

#include <lunar/physics/HumanoidConstants.hpp>
#include <lunar/physics/Materials.hpp>
#include <lunar/physics/PlayerCamera.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace lunar::physics {

using State = Enum::HumanoidStateType;
using namespace humanoid;

namespace {

constexpr std::size_t stateSlots = 19;
using Row = std::array<std::int8_t, eventCount>;

// One row of the transition table: event -> target state, -1 for none.
constexpr Row row(std::initializer_list<std::pair<Event, State>> entries)
{
    Row r{};
    r.fill(-1);
    for (const auto& [event, state] : entries)
        r[static_cast<std::size_t>(event)] = static_cast<std::int8_t>(state);
    return r;
}

// Binary: the table at 0x1064d6300 (rows of 19 int32, row = state + 1).
constexpr std::array<Row, stateSlots> transitions = [] {
    using E = Event;
    using S = State;
    std::array<Row, stateSlots> t{};
    for (Row& r : t)
        r = row({});
    t[static_cast<std::size_t>(S::FallingDown)] = row({{E::NoHealth, S::Dead},
                                                       {E::NoNeck, S::Dead},
                                                       {E::Sit, S::Seated},
                                                       {E::PlatformStand, S::PlatformStanding},
                                                       {E::TimerUp, S::GettingUp},
                                                       {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Ragdoll)] = row(
        {{E::NoHealth, S::Dead}, {E::NoNeck, S::Dead}, {E::JumpCommand, S::GettingUp}, {E::TimerUp, S::GettingUp}});
    t[static_cast<std::size_t>(S::GettingUp)] = row({{E::NoHealth, S::Dead},
                                                     {E::NoNeck, S::Dead},
                                                     {E::Sit, S::Seated},
                                                     {E::PlatformStand, S::PlatformStanding},
                                                     {E::Upright, S::Running},
                                                     {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Jumping)] = row({{E::NoHealth, S::Dead},
                                                   {E::NoNeck, S::Dead},
                                                   {E::Sit, S::Seated},
                                                   {E::PlatformStand, S::PlatformStanding},
                                                   {E::NoFloor, S::Freefall},
                                                   {E::OffFloorLong, S::Freefall},
                                                   {E::Done, S::Freefall},
                                                   {E::TimerUp, S::Freefall}});
    t[static_cast<std::size_t>(S::Swimming)] = row({{E::NoHealth, S::Dead},
                                                    {E::NoNeck, S::Dead},
                                                    {E::JumpCommand, S::Jumping},
                                                    {E::Sit, S::Seated},
                                                    {E::PlatformStand, S::PlatformStanding},
                                                    {E::OutOfWater, S::GettingUp}});
    t[static_cast<std::size_t>(S::Freefall)] = row({{E::NoHealth, S::Dead},
                                                    {E::NoNeck, S::Dead},
                                                    {E::Sit, S::Seated},
                                                    {E::PlatformStand, S::PlatformStanding},
                                                    {E::FaceLadder, S::Climbing},
                                                    {E::OnFloor, S::Landed},
                                                    {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Flying)] = row({{E::NoHealth, S::Dead},
                                                  {E::NoNeck, S::Dead},
                                                  {E::Sit, S::Seated},
                                                  {E::PlatformStand, S::PlatformStanding},
                                                  {E::Tipped, S::FallingDown},
                                                  {E::FaceLadder, S::Climbing},
                                                  {E::OnFloor, S::Running},
                                                  {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Landed)] = row({{E::NoHealth, S::Dead},
                                                  {E::NoNeck, S::Dead},
                                                  {E::Sit, S::Seated},
                                                  {E::PlatformStand, S::PlatformStanding},
                                                  {E::Tipped, S::FallingDown},
                                                  {E::OffFloorLong, S::Freefall},
                                                  {E::TimerUp, S::Running},
                                                  {E::InWater, S::Swimming}});
    const Row running = row({{E::NoHealth, S::Dead},
                             {E::NoNeck, S::Dead},
                             {E::JumpCommand, S::Jumping},
                             {E::Sit, S::Seated},
                             {E::PlatformStand, S::PlatformStanding},
                             {E::Tipped, S::FallingDown},
                             {E::FaceLadder, S::Climbing},
                             {E::OffFloorLong, S::Freefall},
                             {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Running)] = running;
    Row noPhysics = running;
    noPhysics[static_cast<std::size_t>(E::Tipped)] = -1;
    t[static_cast<std::size_t>(S::RunningNoPhysics)] = noPhysics;
    t[static_cast<std::size_t>(S::StrafingNoPhysics)] = noPhysics;
    t[static_cast<std::size_t>(S::Climbing)] = row({{E::NoHealth, S::Dead},
                                                    {E::NoNeck, S::Dead},
                                                    {E::JumpCommand, S::Jumping},
                                                    {E::Sit, S::Seated},
                                                    {E::PlatformStand, S::PlatformStanding},
                                                    {E::Tipped, S::FallingDown},
                                                    {E::AwayLadder, S::Running},
                                                    {E::InWater, S::Swimming}});
    t[static_cast<std::size_t>(S::Seated)] = row({{E::NoHealth, S::Dead},
                                                  {E::NoNeck, S::Dead},
                                                  {E::JumpCommand, S::Jumping},
                                                  {E::NoSit, S::Running},
                                                  {E::PlatformStand, S::PlatformStanding}});
    t[static_cast<std::size_t>(S::PlatformStanding)] = row({{E::NoHealth, S::Dead},
                                                            {E::NoNeck, S::Dead},
                                                            {E::JumpCommand, S::Jumping},
                                                            {E::Sit, S::Seated},
                                                            {E::NoPlatformStand, S::Running}});
    t[static_cast<std::size_t>(S::Physics)] = row({{E::NoHealth, S::Dead}, {E::NoNeck, S::Dead}});
    return t;
}();

std::size_t index(State s)
{
    return static_cast<std::size_t>(s);
}

Vector3 horizontal(Vector3 v)
{
    return {v.x, 0.0f, v.z};
}

bool runningState(State s)
{
    return s == State::Running || s == State::RunningNoPhysics || s == State::StrafingNoPhysics;
}

// The state's countdown, set by its constructor in the binary.
float timerFor(State s)
{
    switch (s) {
    case State::FallingDown:
        return fallingDownTime;
    case State::Ragdoll:
        return ragdollTime;
    case State::Jumping:
        return jumpingTime;
    case State::Landed:
        return landedTime;
    default:
        return 0.0f;
    }
}

} // namespace

HumanoidController::HumanoidController(World& world, Humanoid& humanoid) : world_(world), humanoid_(humanoid)
{
    humanoid.setDriver(this);
    healthConnection_ = humanoid.changed.connect([this](std::string_view property) {
        if (property == "Health")
            humanoid_.events.healthChanged.fire(humanoid_.health());
    });
}

HumanoidController::~HumanoidController()
{
    healthConnection_.disconnect();
    if (humanoid_.driver() == this)
        humanoid_.setDriver(nullptr);
}

void HumanoidController::move(Vector3 direction, bool relativeToCamera)
{
    if (relativeToCamera) {
        if (const Camera* camera = world_.workspace().currentCamera()) {
            // Yaw only, like Humanoid:Move with relativeToCamera.
            const Vector3 forward = safeUnit(horizontal(camera->cframe().lookVector()));
            const Vector3 right = forward.cross({0.0f, 1.0f, 0.0f});
            direction = right * direction.x + forward * -direction.z + Vector3{0.0f, direction.y, 0.0f};
        }
    }
    if (direction.magnitude() > 1.0f)
        direction = direction.unit();
    moveDirection_ = direction;
    if (direction != Vector3{} && moveToPoint_) {
        moveToPoint_.reset();
        humanoid_.events.moveToFinished.fire(false);
    }
}

void HumanoidController::publishMoveDirection()
{
    // A MoveTo walks towards a point, which a character held still doesn't get nearer.
    humanoid_.updateMovement(moveToPoint_ ? humanoid_.moveDirection() : moveDirection_, humanoid_.floorMaterial());
}

void HumanoidController::moveTo(Vector3 point, BasePart* part)
{
    moveToPart_ = part;
    moveToPoint_ = part ? part->cframe().pointToObjectSpace(point) : point;
    moveToTime_ = 0.0f;
    humanoid_.setWalkToPoint(point);
    humanoid_.setWalkToPart(part);
}

void HumanoidController::changeState(State state)
{
    if (stateEnabled(state))
        setState(state);
}

float HumanoidController::stateSpeed(const Assembly& assembly) const
{
    Vector3 v = assembly.linearVelocity;
    if (BasePart* part = floor_ ? floor_->livePart() : nullptr)
        if (const Assembly* fa = world_.assemblyOf(part); fa && !fa->anchored)
            v = v - fa->velocityAt(floor_->position);
    switch (humanoid_.getState()) {
    case State::Climbing:
        return v.y;
    case State::Swimming:
        return v.magnitude();
    default:
        return horizontal(v).magnitude();
    }
}

void HumanoidController::fireStateSignal(State state, bool active, float speed)
{
    // Binary 0x103697264: which event each state drives.
    HumanoidEvents& e = humanoid_.events;
    switch (state) {
    case State::Freefall:
    case State::Flying:
    case State::Landed:
        e.freeFalling.fire(active);
        break;
    case State::Running:
    case State::RunningNoPhysics:
        e.running.fire(speed);
        break;
    case State::Climbing:
        e.climbing.fire(speed);
        break;
    case State::Swimming:
        e.swimming.fire(speed);
        break;
    case State::Jumping:
        e.jumping.fire(active);
        break;
    case State::GettingUp:
        e.gettingUp.fire(active);
        break;
    case State::FallingDown:
        e.fallingDown.fire(active);
        break;
    case State::Ragdoll:
    case State::Physics:
        e.ragdoll.fire(active);
        break;
    case State::StrafingNoPhysics:
        e.strafing.fire(active);
        break;
    case State::Seated:
        e.seated.fire(active, active ? humanoid_.seatPart() : nullptr);
        break;
    case State::PlatformStanding:
        e.platformStanding.fire(active);
        break;
    case State::Dead:
        if (active)
            e.died.fire();
        break;
    default:
        break;
    }
}

void HumanoidController::setState(State next)
{
    // Binary changeState 0x10369b38e: the no-physics running states become Running.
    if (next == State::RunningNoPhysics || next == State::StrafingNoPhysics)
        next = State::Running;
    const State old = humanoid_.getState();
    if (old == next)
        return;
    StateData fresh;
    fresh.timer = timerFor(next);
    fresh.noFloorTime = data_.noFloorTime;
    fresh.noTip = data_.noTip;
    // A jump out of water can't tip the character over (binary). Clearing
    // that again on the ground is a guess.
    if (next == State::Jumping && old == State::Swimming)
        fresh.noTip = true;
    if (next == State::Landed || runningState(next))
        fresh.noTip = false;
    data_ = fresh;
    humanoid_.assignState(next);

    const BasePart* root = humanoid_.rootPart();
    const Assembly* assembly = root ? world_.assemblyOf(root) : nullptr;
    const float speed = assembly ? stateSpeed(*assembly) : 0.0f;
    // Binary postStep 0x103696f32: the old state's event off, the new one's on, then StateChanged.
    fireStateSignal(old, false, speed);
    fireStateSignal(next, true, speed);
    humanoid_.events.stateChanged.fire(old, next);
    lastSpeed_ = std::abs(speed) < speedEventFloor ? 0.0f : speed;

    if (next == State::Dead) {
        dead_ = true;
        if (humanoid_.breakJointsOnDeath() && humanoid_.parent())
            for (const InstancePtr& d : humanoid_.parent()->descendants())
                if (d->isA("JointInstance") || d->isA("WeldConstraint"))
                    d->setParent(nullptr);
    }
}

void HumanoidController::fireSpeedSignal(float speed)
{
    // Binary 0x10369bbb0: again only on a sign change or leaving 5% of the last value.
    const State s = humanoid_.getState();
    if (!runningState(s) && s != State::Climbing && s != State::Swimming)
        return;
    const float value = std::abs(speed) < speedEventFloor ? 0.0f : speed;
    const float last = lastSpeed_.value_or(0.0f);
    const bool signChanged = (value > 0.0f) != (last > 0.0f) || (value < 0.0f) != (last < 0.0f);
    const float lo = std::min(last * (1.0f - speedEventBand), last * (1.0f + speedEventBand));
    const float hi = std::max(last * (1.0f - speedEventBand), last * (1.0f + speedEventBand));
    if (!lastSpeed_ || signChanged || value < lo || value > hi) {
        lastSpeed_ = value;
        fireStateSignal(s, true, value);
    }
}

float HumanoidController::standHeight(const BasePart& root) const
{
    // Binary 0x1036993a0: half the root part, HipHeight, and for R6 the longer leg.
    float legs = 0.0f;
    if (humanoid_.rigType() == Enum::HumanoidRigType::R6 && humanoid_.parent())
        for (const char* name : {"Left Leg", "Right Leg"})
            if (auto* leg = instanceCast<BasePart>(humanoid_.parent()->findFirstChild(name)))
                legs = std::max(legs, leg->size().y);
    return 0.5f * root.size().y + humanoid_.hipHeight() + legs;
}

float HumanoidController::jumpVelocity() const
{
    // Binary 0x103639abe.
    if (humanoid_.useJumpPower())
        return humanoid_.jumpPower();
    const float v = std::sqrt(2.0f * world_.workspace().gravity() * std::max(humanoid_.jumpHeight(), 0.0f));
    return std::clamp(v / jumpBoost, 0.0f, maxJumpVelocity);
}

std::optional<HumanoidController::Floor> HumanoidController::findFloor(const BasePart& root) const
{
    RaycastParams params;
    params.respectCanCollide = true;
    if (Instance* character = humanoid_.parent())
        params.filterDescendantsInstances.push_back(character);
    const CFrame cf = root.cframe();
    const float reach = standHeight(root) + floorSearchExtra;
    // The centre and four points around it, so ledges hold the character up. Layout guessed.
    const Vector3 half = root.size() * 0.5f;
    const Vector3 offsets[]{
        {0.0f, 0.0f, 0.0f},
        {half.x * 0.5f, 0.0f, half.z * 0.5f},
        {-half.x * 0.5f, 0.0f, half.z * 0.5f},
        {half.x * 0.5f, 0.0f, -half.z * 0.5f},
        {-half.x * 0.5f, 0.0f, -half.z * 0.5f},
    };
    std::optional<Floor> best;
    for (const Vector3& offset : offsets) {
        const Vector3 origin = cf.position + horizontal(cf.vectorToWorldSpace(offset));
        auto hit = world_.raycast(origin, {0.0f, -reach, 0.0f}, params);
        if (!hit)
            continue;
        if (!best || hit->distance < best->distance)
            best = Floor{.part = hit->instance,
                         .held = hit->instance ? hit->instance->weak_from_this() : std::weak_ptr<const Instance>(),
                         .position = hit->position,
                         .normal = hit->normal,
                         .material = hit->material,
                         .distance = hit->distance};
    }
    return best;
}

bool HumanoidController::findLadder(const BasePart& root) const
{
    // A TrussPart just in front of the root part. The binary's sensor
    // (0x103635db0) also finds ladders made of stacked parts; not yet.
    RaycastParams params;
    params.respectCanCollide = true;
    if (Instance* character = humanoid_.parent())
        params.filterDescendantsInstances.push_back(character);
    const CFrame cf = root.cframe();
    const Vector3 ahead = horizontal(cf.lookVector()) * (0.5f * root.size().z + ladderReach);
    for (float height : {-1.0f, 0.0f, 1.0f}) {
        auto hit = world_.raycast(cf.position + Vector3{0.0f, height, 0.0f}, ahead, params);
        if (hit && hit->instance->isA("TrussPart"))
            return true;
    }
    return false;
}

bool HumanoidController::inWater(const BasePart& root) const
{
    return world_.waterAt(root.cframe().position) > swimWaterFraction;
}

bool HumanoidController::neckMissing() const
{
    Instance* character = humanoid_.parent();
    if (!character)
        return true;
    for (const InstancePtr& d : character->descendants()) {
        if (d->name() != "Neck")
            continue;
        if (auto* joint = instanceCast<JointInstance>(d.get()))
            return !joint->part0() || !joint->part1();
        // Upgraded avatars' Neck is an AnimationConstraint between the RigAttachments [capture].
        if (const auto* constraint = instanceCast<const AnimationConstraint>(d.get())) {
            const Attachment* a0 = constraint->attachment0();
            const Attachment* a1 = constraint->attachment1();
            return !constraint->enabled() || a0 == nullptr || a1 == nullptr || a0->parent() == nullptr
                   || a1->parent() == nullptr;
        }
    }
    return true;
}

bool HumanoidController::condition(int event, State state, const Assembly& assembly) const
{
    const BasePart& root = *humanoid_.rootPart();
    switch (static_cast<Event>(event)) {
    case Event::NoHealth:
        return humanoid_.health() <= 0.0f;
    case Event::NoNeck:
        // A neck that broke: one the character had. A character that hasn't
        // got its Neck yet (still arriving from the server, or a game's own
        // rig: the game's Hotel) lives. GUESS: the client reacts to the joint
        // going rather than checking for it.
        return humanoid_.requiresNeck() && neckSeen_ && neckMissing();
    case Event::JumpCommand: {
        // Binary 0x10369b876.
        if (!humanoid_.jump() || jumpVelocity() <= 0.0f)
            return false;
        if (data_.noFloorTime > jumpCoyoteTime && !runningState(state) && state != State::PlatformStanding)
            return false;
        if (state == State::Climbing && !findLadder(root))
            return false;
        if (runningState(state) && floor_ && floor_->normal.y <= std::cos(humanoid_.maxSlopeAngle() * camera::degrees))
            return false;
        return true;
    }
    case Event::Sit:
        return humanoid_.sit();
    case Event::NoSit:
        return !humanoid_.sit();
    case Event::PlatformStand:
        return humanoid_.platformStand();
    case Event::NoPlatformStand:
        return !humanoid_.platformStand();
    case Event::Tipped:
        return root.cframe().upVector().y < tippedUpY && !data_.noTip;
    case Event::Upright:
        return root.cframe().upVector().y > uprightUpY;
    case Event::FaceLadder:
        return findLadder(root);
    case Event::AwayLadder:
        return !findLadder(root);
    case Event::NoFloor:
        return !floor_;
    case Event::OffFloorLong:
        return data_.noFloorTime > offFloorTime;
    case Event::OnFloor: {
        if (!floor_)
            return false;
        Vector3 floorVelocity;
        if (floor_->part)
            if (const Assembly* fa = world_.assemblyOf(floor_->part); fa && !fa->anchored)
                floorVelocity = fa->velocityAt(floor_->position);
        if (assembly.linearVelocity.y - floorVelocity.y > 0.0f)
            return false;
        // From the air, only once the floor is at standing height: the probe
        // reaches further (to keep the floor walking down slopes), and landing
        // early switches gravity off up there, leaving the altitude spring,
        // which may only pull down slowly, to lower the character the rest of
        // the way. GUESS: the client lands within the stand height.
        const bool airborne = state == State::Freefall || state == State::Flying;
        return !airborne || floor_->distance <= standHeight(root);
    }
    case Event::Done:
        return data_.done;
    case Event::TimerUp:
        return data_.timer <= 0.0f;
    case Event::InWater:
        return inWater(root);
    case Event::OutOfWater:
        return !inWater(root);
    }
    return false;
}

Vector3 HumanoidController::walkDirection()
{
    if (!moveToPoint_)
        return moveDirection_;
    const Vector3 target = moveToPart_ ? moveToPart_->cframe().pointToWorldSpace(*moveToPoint_) : *moveToPoint_;
    BasePart* root = humanoid_.rootPart();
    const Vector3 to = horizontal(target - root->cframe().position);
    if (to.magnitude() < moveToReachDistance) {
        moveToPoint_.reset();
        humanoid_.events.moveToFinished.fire(true);
        return {};
    }
    return to.unit();
}

void HumanoidController::balance(Assembly& assembly, const BasePart& root, Vector3 facing, float dt) const
{
    BalanceGains gains = balanceDefault;
    switch (humanoid_.getState()) {
    case State::Freefall:
        gains = balanceFreefall;
        break;
    case State::GettingUp:
        gains = balanceGettingUp;
        break;
    case State::Landed:
        gains = balanceLanded;
        break;
    default:
        break;
    }
    // Tilt: the rotation taking the root's up to world up, as axis times angle.
    const Vector3 up = root.cframe().upVector();
    const Vector3 worldUp{0.0f, 1.0f, 0.0f};
    const Vector3 axis = up.cross(worldUp);
    const float angle = std::atan2(axis.magnitude(), up.dot(worldUp));
    const Vector3 tilt = safeUnit(axis) * angle;
    const Vector3 w = assembly.angularVelocity;
    const Vector3 wobble = w - worldUp * w.y;
    // PD, with the damping implicit so the stiff gains stay stable at the step size.
    const Vector3 corrected = (wobble + tilt * (gains.kP * dt)) / (1.0f + gains.kD * dt);

    // AutoRotate (binary 0x1036418e6): the wanted yaw rate is 8 x the yaw
    // error (4 x facing the camera), reached through a turning gain of 100.
    float wanted = 0.0f;
    float turn = 0.0f;
    const Vector3 look = horizontal(root.cframe().lookVector());
    const bool turning = humanoid_.autoRotate() && facing.magnitude() > 1e-3f && look.magnitude() > 1e-3f;
    if (turning) {
        const Vector3 want = facing.unit();
        const Vector3 have = look.unit();
        turn = std::atan2(have.cross(want).y, have.dot(want));
        wanted = autoRotateRate * turn;
    }
    const bool onGround = runningState(humanoid_.getState()) || humanoid_.getState() == State::Landed;
    const float limit = onGround ? groundTurnLimit : airTurnLimit;
    float yawRate = w.y + std::clamp(turnGain * (wanted - w.y), -limit, limit) * dt;
    // Camera-relative (shift lock, first person) holds the character to the
    // camera's yaw, as the client does: no easing towards it, the whole turn
    // this step. GUESS on the mechanism: 0x103641a87 scales a Humanoid field
    // (+0x1ac) we haven't traced, and the newer path (0x10364220b) takes the
    // camera's look (+0x658) as the facing outright; the client's shift lock
    // shows no lag behind the mouse [live].
    if (turning && facing_ && dt > 0.0f)
        yawRate = turn / dt;
    assembly.angularVelocity = corrected + worldUp * yawRate;
}

void HumanoidController::updateState(Assembly& assembly, BasePart& root, Vector3 walk, float dt)
{
    const State state = humanoid_.getState();
    const float speed = humanoid_.walkSpeed();
    const float g = world_.workspace().gravity();
    Vector3 velocity = assembly.linearVelocity;
    const Vector3 target = horizontal(walk) * speed;
    // Binary RunningBase / Freefall force callbacks: a = 150 (target - v), capped.
    auto steer = [&](Vector3 want, float cap) {
        Vector3 a = (want - horizontal(velocity)) * walkGain;
        if (a.magnitude() > cap)
            a = a.unit() * cap;
        velocity = velocity + a * dt;
    };
    auto airControl = [&] { steer(target, noFloorAcceleration); };
    bool balanced = true;
    const Vector3 facing = facing_ ? horizontal(*facing_) : horizontal(walk);

    switch (state) {
    case State::Running:
    case State::RunningNoPhysics:
    case State::StrafingNoPhysics:
    case State::Landed: {
        // On the ground: walk velocity carried by a moving floor, capped by friction,
        // and the altitude spring holding the root part at the stand height.
        assembly.gravityScale = 0.0f;
        if (!floor_) {
            // Off the floor, but not for long enough to fall yet.
            assembly.gravityScale = 1.0f;
            airControl();
            break;
        }
        Vector3 floorVelocity;
        PhysicalProperties floorMaterial = defaultPhysicalProperties(floor_->material);
        if (floor_->part && !floor_->part->isA("Terrain")) {
            floorMaterial = World::physicalProperties(*floor_->part);
            if (Assembly* fa = world_.assemblyOf(floor_->part); fa && !fa->anchored)
                floorVelocity = fa->velocityAt(floor_->position);
        }
        // The floor's friction alone: the character's own (the game gives its root
        // part Friction 0 at FrictionWeight 100) doesn't enter into it.
        steer(target + horizontal(floorVelocity), frictionAccelerationScale * floorMaterial.friction * assembly.mass);
        const float d = standHeight(root) - 0.5f * root.size().y;
        const float error = std::clamp(standHeight(root) - floor_->distance, altitudeMinErrorFraction * d,
                                       altitudeMaxErrorFraction * d);
        velocity.y += (altitudeKp * error - altitudeKd * (velocity.y - floorVelocity.y)) * dt;
        break;
    }
    case State::Jumping: {
        assembly.gravityScale = 1.0f;
        if (!data_.done) {
            // Binary 0x10369e3d0: vertical speed up to 1.06 x jumpVelocity in one
            // push, compensating this step's gravity; the floor takes the reaction.
            const float launch = std::min(jumpBoost * jumpVelocity(), maxJumpVelocity);
            const float dv = launch - velocity.y;
            if (dv > 1e-6f) {
                velocity.y += dv + g * dt;
                if (floor_ && floor_->part)
                    if (Assembly* fa = world_.assemblyOf(floor_->part); fa && !fa->anchored)
                        fa->applyImpulse(Vector3{0.0f, -dv * assembly.mass * floorReactionShare, 0.0f},
                                         floor_->position);
            }
            data_.done = true;
        }
        airControl();
        break;
    }
    case State::Freefall:
    case State::Flying:
        assembly.gravityScale = 1.0f;
        airControl();
        break;
    case State::Climbing: {
        // Up or down with the move input's push towards the ladder. Speeds guessed.
        assembly.gravityScale = 0.0f;
        const Vector3 toward = safeUnit(horizontal(root.cframe().lookVector()));
        const float push = horizontal(walk).dot(toward);
        const float climb = speed * climbSpeedFactor;
        velocity = toward * (push > 0.0f ? 2.0f : 0.0f);
        velocity.y = push > 0.1f ? climb : (push < -0.1f ? -climb : 0.0f);
        break;
    }
    case State::Swimming:
        // Guess: WalkSpeed in 3D, Jump swims up.
        assembly.gravityScale = 0.0f;
        velocity = walk * speed;
        if (humanoid_.jump())
            velocity.y = speed;
        break;
    case State::GettingUp:
        assembly.gravityScale = 1.0f;
        break;
    default:
        // FallingDown, Ragdoll, Physics, PlatformStanding, Seated, Dead: physics alone.
        assembly.gravityScale = 1.0f;
        balanced = false;
        break;
    }
    assembly.linearVelocity = velocity;
    assembly.asleep = false;
    if (balanced)
        balance(assembly, root, facing, dt);
}

void HumanoidController::updateSeat()
{
    if (BasePart* seat = humanoid_.seatPart()) {
        // Up on a jump, a script clearing Sit, or the weld going.
        const bool held = seatWeld(*seat, humanoid_) && seat->isDescendantOf(&world_.workspace());
        if (dead_ || !humanoid_.sit() || humanoid_.jump() || !held) {
            lastSeat_[seat] = clock_;
            leaveSeat(humanoid_);
            world_.refresh();
        }
        return;
    }
    if (dead_ || humanoid_.sit() || humanoid_.health() <= 0.0f)
        return;
    Instance* character = humanoid_.parent();
    auto trySit = [&](BasePart& seat) {
        if (seatDisabled(seat) || seatOccupant(seat))
            return false;
        if (auto it = lastSeat_.find(&seat); it != lastSeat_.end() && clock_ - it->second <= seatDebounce)
            return false;
        if (!sit(seat, humanoid_))
            return false;
        lastSeat_[&seat] = clock_;
        world_.refresh();
        return true;
    };
    // Standing on one: the character hovers at its hip height, so its legs
    // only graze what it stands on. GUESS: the client's touch counts that.
    BasePart* floorPart = floor_ ? floor_->livePart() : nullptr;
    if (floorPart && isSeat(*floorPart) && floor_->distance <= standHeight(*humanoid_.rootPart()) + 0.5f
        && trySit(*floorPart))
        return;
    if (character)
        for (BasePart* seat : world_.seatsTouching(*character))
            if (trySit(*seat))
                return;
}

// The Humanoid keeps a living character's arms and legs from colliding,
// setting their CanCollide back to false whatever a script or the server set
// (live R15 characters arrive with every limb colliding [live]): only the
// root, the torso and the head meet the world, and the floor probe carries
// the character up steps its feet would catch on. GUESS: the binary's limb
// list wasn't traced; this is the documented behaviour, by body part name.
void HumanoidController::keepLimbsFromColliding()
{
    static constexpr std::array<std::string_view, 16> limbs{
        "LeftUpperArm",  "LeftLowerArm",  "LeftHand",      "RightUpperArm", "RightLowerArm", "RightHand",
        "LeftUpperLeg",  "LeftLowerLeg",  "LeftFoot",      "RightUpperLeg", "RightLowerLeg", "RightFoot",
        "Left Arm",      "Right Arm",     "Left Leg",      "Right Leg",
    };
    Instance* character = humanoid_.parent();
    if (!character || humanoid_.getState() == State::Dead || humanoid_.getState() == State::Physics)
        return;
    for (const InstancePtr& child : character->children())
        if (auto* part = instanceCast<BasePart>(child.get()); part && part->canCollide())
            if (std::ranges::contains(limbs, std::string_view(part->name())))
                part->setCanCollide(false);
}

void HumanoidController::step(float dt)
{
    BasePart* root = humanoid_.rootPart();
    if (!root)
        return;
    clock_ += dt;
    keepLimbsFromColliding();
    updateSeat();
    Assembly* assembly = world_.assemblyOf(root);
    if (!assembly)
        return;
    // In an anchored seat it still sits and gets up; otherwise anchored means
    // held still, MoveDirection still following what it's told to walk. It
    // stayed at whatever it was when the anchoring came: out of a game
    // wardrobe at once if you were walking in, never if you weren't.
    if (assembly->anchored) {
        publishMoveDirection();
        if (!humanoid_.seatPart())
            return;
    }
    if (humanoid_.getState() == State::None)
        setState(State::Running);

    if (!neckSeen_ && !neckMissing())
        neckSeen_ = true;
    // Binary preStep 0x1036994ba: floor search and timers.
    floor_ = findFloor(*root);
    data_.noFloorTime = floor_ ? 0.0f : data_.noFloorTime + dt;
    data_.timer -= dt;

    // Binary 0x10369a54a: the first event, in order, that leads to an enabled state.
    if (humanoid_.evaluateStateMachine()) {
        const State state = humanoid_.getState();
        const Row& targets = transitions.at(index(state));
        for (int event = 0; event < eventCount; ++event) {
            const std::int8_t target = targets.at(static_cast<std::size_t>(event));
            if (target < 0 || target == static_cast<std::int8_t>(state) || !stateEnabled(static_cast<State>(target)))
                continue;
            if (condition(event, state, *assembly)) {
                if (static_cast<State>(target) == State::Dead)
                    deathCause_ = static_cast<Event>(event) == Event::NoHealth ? "no health"
                                  : static_cast<Event>(event) == Event::NoNeck ? "no neck"
                                                                               : "the state machine";
                setState(static_cast<State>(target));
                break;
            }
        }
    }
    if (dead_) {
        assembly->gravityScale = 1.0f;
        return;
    }
    if (assembly->anchored)
        return;

    const Vector3 walk = walkDirection();
    if (moveToPoint_) {
        moveToTime_ += dt;
        if (moveToTime_ > moveToTimeout) {
            moveToPoint_.reset();
            humanoid_.events.moveToFinished.fire(false);
        }
    }
    updateState(*assembly, *root, walk, dt);
    humanoid_.updateMovement(walk, floor_ ? floor_->material : Enum::Material::Air);
    fireSpeedSignal(stateSpeed(*assembly));
}

} // namespace lunar::physics
