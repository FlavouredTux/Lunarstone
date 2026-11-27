// Mechanical constraints between attachments, solved as velocity rows next
// to the contacts.
//
// The client builds each Constraint instance out of solver primitives
// (RBX::Solver::Constraint*) whose rows its PGS solver drives toward a target
// velocity within impulse bounds. What each row asks for is read from their
// row builders (vfn 11) and how the instances set them up:
//  - A SlidingBallConstraint (sub_101146fc4) is a rigid part holding
//    Attachment1 on Attachment0's primary axis, a
//    ConstraintSingleAxisLinearVelocity motor, a ConstraintLinearServoMotor
//    and a ConstraintInsideOfSphere for its limits. A HingeConstraint
//    (sub_1010f7100) is a ball and socket with the axes lined up, a
//    ConstraintAngularVelocity motor, a ConstraintServoMotor and an angular
//    limit; a CylindricalConstraint (sub_10110075a) is both actuator sets.
//  - Motors (sub_10536b99a, sub_105358afc): target = v + clamp(goal - v,
//    +-MaxAcceleration dt), the impulse within +-MaxForce (MaxTorque) dt, v
//    the velocity along the row when the step starts.
//  - Servos (sub_1053666c2, sub_10536b10e): with e the error to the target,
//    target = clamp(v + dt (k e - c v), +-Speed), the impulse within
//    +-ServoMaxForce (ServoMaxTorque) dt, k = Responsiveness^2 / 4 and
//    c = Responsiveness: a critically damped approach at Responsiveness / 2
//    rad/s. Angular errors take the short way round unless the limits are
//    in the way.
//  - Limits: a one dimensional sphere of radius |upper - lower| / 2 around
//    the middle of the limits, with the constraint's Restitution.
//  - The legacy BodyVelocity (RBX::BodyVelocity vfn 83) is a
//    ConstraintLinearVelocity (sub_105367500): a row along each world axis
//    at the center of mass, target Velocity, the impulse within MaxForce dt
//    on that axis. P plays no part.
//  - BodyPosition (vfn 83) is a ConstraintLinearSpring (sub_105366da8): rows
//    along the world axes through its part's position, each targeting
//    v - dt (P x + D v) for x the part's offset from Position: P and D are
//    stiffness and damping per unit mass, first held to what a step can
//    take (sub_104d56df6). The impulse is within MaxForce dt per axis.
//  - A SpringConstraint (sub_10114b60a) is a ConstraintSingleAxisSpring
//    (sub_10536bf8a): a fixed impulse dt clamp(Stiffness (L - FreeLength) +
//    Damping L', +-MaxForce) along the line between the attachments, L held
//    within [MinLength, MaxLength] when LimitsEnabled (else [0, inf)), plus
//    a ConstraintInsideOfSphere keeping L within those limits.
//  - AlignOrientation turns Attachment1 to face as Attachment0 does (or
//    to its CFrame with one attachment), turning only Attachment0's side
//    unless ReactionTorqueEnabled. GUESS: its solver primitives (set up in
//    RBX::Constraints::AlignOrientation vfn 98) aren't read yet; here it
//    is the servos' approach about the axis of the turn, at most
//    MaxAngularVelocity, the impulse within MaxTorque dt, rigid with
//    RigidityEnabled. PrimaryAxisOnly lines up the primary axes alone.

#include "Solver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <print>
#include <string_view>

namespace lunar::physics {

namespace {

// GUESS: how fast the rigid rows pull drifted attachments back together.
// The client's constraint stabilisation is its own; this is the contacts'.
constexpr float jointBaumgarte = 0.2f;
constexpr float restitutionThreshold = 2.0f; // studs/s, as for contacts
constexpr float floatMax = std::numeric_limits<float>::max();
constexpr float pi = std::numbers::pi_v<float>;
constexpr float radians = pi / 180.0f;
// Active mechanisms build their rows in runs of rowBatchSize, spread over
// threads once their count times rowCost reaches the World's parallelWork.
// GUESS: sized like the narrow phase's runs of colliders (World.cpp).
constexpr std::size_t rowBatchSize = 16;
constexpr std::size_t rowCost = 4;

// Instances the World simulates.
bool simulated(const Instance& instance)
{
    return instance.isA("BodyVelocity") || instance.isA("BodyPosition") || instance.isA("SlidingBallConstraint")
           || instance.isA("HingeConstraint") || instance.isA("SpringConstraint") || instance.isA("RopeConstraint")
           || instance.isA("RodConstraint") || instance.isA("BallSocketConstraint") || instance.isA("AlignOrientation");
}

// The part an attachment moves with: its parent, or a bone's part.
BasePart* attachedPart(const Attachment& attachment)
{
    Instance* parent = attachment.parent();
    while (parent && instanceCast<Attachment>(parent))
        parent = parent->parent();
    return instanceCast<BasePart>(parent);
}

// The client clamps actuator strengths into [0, FLT_MAX] (sub_101146fc4,
// sub_1010f7100), so inf and negative values stay usable.
float strength(float value)
{
    return std::clamp(value, 0.0f, floatMax);
}

// A legacy mover's stiffness k and damping c (per unit mass) held to what
// a step dt can follow, as sub_104d56df6 does: an underdamped spring turns
// at most pi/6 a step, an overdamped one's c dt / 2 stays within 0.8.
void holdGains(float& k, float& c, float dt)
{
    const float discriminant = 4.0f * k - c * c;
    float scale = 1.0f;
    if (discriminant >= 0.0f) {
        const float turn = std::sqrt(discriminant) * 0.5f * dt;
        if (turn > pi / 6.0f)
            scale = pi / 6.0f / turn;
    } else if (c * 0.5f > 0.8f / dt) {
        scale = 0.8f / (c * 0.5f * dt);
    }
    c *= scale;
    k *= scale * scale;
}

float component(Vector3 v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

// The rotation taking frame a's orientation to b's, as an axis times its angle.
Vector3 rotationBetween(const CFrame& a, const CFrame& b)
{
    const CFrame d = b.rotation() * a.rotation().inverse();
    const float trace = d.r[0][0] + d.r[1][1] + d.r[2][2];
    const float angle = std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f));
    const Vector3 axis{d.r[2][1] - d.r[1][2], d.r[0][2] - d.r[2][0], d.r[1][0] - d.r[0][1]};
    const float length = axis.magnitude();
    if (angle < 1e-6f || length < 1e-9f)
        return {};
    return axis * (angle / length);
}

// Into [-pi, pi].
float wrapAngle(float angle)
{
    return angle - 2.0f * pi * std::round(angle / (2.0f * pi));
}

// Attachment1's secondary axis s1 from a reference s0 about an axis, as
// HingeConstraint.CurrentAngle reads it (Attachment0's secondary and
// primary axes), in radians.
float angleBetween(Vector3 axis, Vector3 s0, Vector3 s1)
{
    return std::atan2(axis.dot(s0.cross(s1)), s0.dot(s1));
}

} // namespace

void World::mechanismAdded(Instance& instance)
{
    if (!simulated(instance) || mechanismOf_.contains(&instance))
        return;
    auto mechanism = std::make_unique<Mechanism>();
    mechanism->instance = &instance;
    // Any change wakes what it joins: a new Velocity has to get going.
    mechanism->changed = instance.changed.connect([m = mechanism.get()](std::string_view) { m->touched = true; });
    mechanismOf_[&instance] = mechanism.get();
    mechanisms_.push_back(std::move(mechanism));
}

void World::mechanismRemoving(Instance& instance)
{
    auto it = mechanismOf_.find(&instance);
    if (it == mechanismOf_.end())
        return;
    Mechanism* mechanism = it->second;
    mechanismOf_.erase(it);
    // What it held is free now. The parts are only looked up, never touched:
    // they may be gone already.
    for (const BasePart* part : mechanism->parts)
        if (auto c = colliderOf_.find(part); c != colliderOf_.end() && colliders_[c->second].assembly)
            colliders_[c->second].assembly->asleep = false;
    mechanism->changed.disconnect();
    for (Connection& c : mechanism->attachmentChanged)
        c.disconnect();
    std::erase(activeMechanisms_, mechanism);
    std::erase_if(mechanisms_, [&](const std::unique_ptr<Mechanism>& m) { return m.get() == mechanism; });
}

// Attached when it's Enabled with both attachments on parts the World
// simulates (or the terrain), on two different assemblies that aren't both
// anchored.
void World::resolve(Mechanism& m)
{
    m.resolvedEpoch = structureEpoch_;
    m.parts = {};
    m.a = m.b = nullptr;
    if (instanceCast<BodyMover>(m.instance)) {
        // A legacy mover pushes the part it's in against the world.
        auto* part = instanceCast<BasePart>(m.instance->parent());
        auto it = part ? colliderOf_.find(part) : colliderOf_.end();
        if (it == colliderOf_.end() || !colliders_[it->second].assembly || colliders_[it->second].assembly->anchored)
            return;
        m.parts = {part, part};
        m.b = colliders_[it->second].assembly;
        return;
    }
    auto* constraint = static_cast<Constraint*>(m.instance);
    const std::array<Attachment*, 2> attachments{constraint->attachment0(), constraint->attachment1()};
    for (std::size_t i = 0; i < 2; ++i) {
        if (attachments[i] == m.attachments[i])
            continue;
        // Moving an attachment moves the constraint: watch the new ones.
        m.attachmentChanged[i].disconnect();
        m.attachments[i] = attachments[i];
        if (attachments[i])
            m.attachmentChanged[i]
                = attachments[i]->changed.connect([at = &m](std::string_view) { at->touched = true; });
        m.touched = true;
    }
    // An AlignOrientation in OneAttachment mode turns Attachment0 toward a
    // CFrame: the world is its other side.
    const auto* align = instanceCast<const AlignOrientation>(constraint);
    const bool one = align && align->mode() == Enum::OrientationAlignmentMode::OneAttachment;
    if (!constraint->enabled() || !attachments[0] || (!attachments[1] && !one))
        return;
    std::array<Assembly*, 2> sides{};
    std::array<BasePart*, 2> parts{};
    for (std::size_t i = 0; i < (one ? 1u : 2u); ++i) {
        BasePart* part = attachedPart(*attachments[i]);
        if (!part)
            return;
        if (part != workspace_.terrain()) {
            auto it = colliderOf_.find(part);
            if (it == colliderOf_.end() || !colliders_[it->second].assembly)
                return; // outside the Workspace, or not picked up yet
            Assembly* assembly = colliders_[it->second].assembly;
            sides[i] = assembly->anchored ? nullptr : assembly;
        }
        parts[i] = part;
    }
    if (one)
        parts[1] = parts[0];
    if (sides[0] == sides[1])
        return; // both anchored, or one rigid assembly: nothing to solve
    m.parts = parts;
    m.a = sides[0];
    m.b = sides[1];
}

void World::resolveMechanisms()
{
    activeMechanisms_.clear();
    for (const auto& owned : mechanisms_) {
        Mechanism& m = *owned;
        // What it joins only changes when assemblies are rebuilt or it or
        // its attachments change.
        if (m.touched || m.resolvedEpoch != structureEpoch_)
            resolve(m);
        if (!m.parts[0]) {
            m.used.fill(false);
            continue;
        }
        auto asleep = [](const Assembly* x) { return x && x->asleep; };
        // Truly awake, not just checking whether it may sleep.
        auto awake = [](const Assembly* x) { return x && !x->asleep && !x->rest.checking; };
        if (m.touched) {
            m.touched = false;
            for (Assembly* x : {m.a, m.b})
                if (x)
                    x->asleep = false;
        } else if (asleep(m.b) && awake(m.a)) {
            m.b->asleep = false; // constrained assemblies wake together
        } else if (asleep(m.a) && awake(m.b)) {
            m.a->asleep = false;
        }
        // Sleeping or anchored on both sides: nothing moves.
        if ((!m.a || m.a->asleep) && (!m.b || m.b->asleep)) {
            m.used.fill(false);
            continue;
        }
        activeMechanisms_.push_back(&m);
    }
}

// Each assembly's sleep test runs on its own, so a car's frame may pass it
// a step before its wheels do. Asleep, it would hold them like an anchored
// part while their springs still pushed: what was balanced no longer is.
// So one that fell asleep this step next to one still awake goes back to
// checking, history kept, until they're all ready in the same step.
void World::sleepTogether()
{
    auto justSlept = [&](const Assembly* x) { return x && x->asleep && x->rest.sleptAt == stepCount_; };
    auto awake = [](const Assembly* x) { return x && !x->asleep && !x->anchored; };
    for (bool changed = true; changed;) {
        changed = false;
        for (Mechanism* m : activeMechanisms_)
            for (Assembly* x : {m->a, m->b}) {
                Assembly* other = x == m->a ? m->b : m->a;
                if (!justSlept(x) || !awake(other))
                    continue;
                x->asleep = false;
                x->rest.slept = false;
                x->rest.checking = true;
                changed = true;
            }
    }
}

float World::rowVelocity(const Row& row) const
{
    float v = 0.0f;
    if (row.b >= 0) {
        const Body& b = bodies_[static_cast<std::size_t>(row.b)];
        v += row.linear.dot(b.linear) + row.angularB.dot(b.angular);
    }
    if (row.a >= 0) {
        const Body& a = bodies_[static_cast<std::size_t>(row.a)];
        v -= row.linear.dot(a.linear) + row.angularA.dot(a.angular);
    }
    return v;
}

void World::applyRow(Row& row, float impulse)
{
    if (row.b >= 0) {
        Body& b = bodies_[static_cast<std::size_t>(row.b)];
        b.linear = b.linear + row.linear * (impulse * b.inverseMass);
        b.angular = b.angular + b.inverseInertia * (row.angularB * impulse);
    }
    if (row.a >= 0) {
        Body& a = bodies_[static_cast<std::size_t>(row.a)];
        a.linear = a.linear - row.linear * (impulse * a.inverseMass);
        a.angular = a.angular - a.inverseInertia * (row.angularA * impulse);
    }
}

void World::prepareRow(Row& row) const
{
    float k = 0.0f;
    if (row.a >= 0) {
        const Body& a = bodies_[static_cast<std::size_t>(row.a)];
        k += a.inverseMass * row.linear.dot(row.linear) + row.angularA.dot(a.inverseInertia * row.angularA);
    }
    if (row.b >= 0) {
        const Body& b = bodies_[static_cast<std::size_t>(row.b)];
        k += b.inverseMass * row.linear.dot(row.linear) + row.angularB.dot(b.inverseInertia * row.angularB);
    }
    row.mass = k > 1e-12f ? 1.0f / k : 0.0f;
}

void World::buildRows(float dt)
{
    // Bodies are numbered here, in mechanism order; the build only reads
    // them. A sleeper next to one only checking whether it may sleep holds
    // still.
    auto side = [&](Assembly* x) { return x && !x->asleep ? static_cast<std::int32_t>(bodyOf(x)) : std::int32_t{-1}; };
    mechanismBodies_.clear();
    for (const Mechanism* m : activeMechanisms_) {
        const std::int32_t a = side(m->a);
        mechanismBodies_.push_back({a, side(m->b)});
    }
    // In fixed runs of mechanisms, joined in run order: the same rows in
    // the same order on any number of threads.
    const std::size_t count = activeMechanisms_.size();
    const std::size_t batches = (count + rowBatchSize - 1) / rowBatchSize;
    if (rowBatches_.size() < batches) {
        rowBatches_.resize(batches);
        droppedRows_.resize(batches);
    }
    forEach(batches, count * rowCost, [&](std::size_t n) {
        std::vector<Row>& out = rowBatches_[n];
        out.clear();
        droppedRows_[n].clear();
        for (std::size_t i = n * rowBatchSize; i < std::min(count, (n + 1) * rowBatchSize); ++i)
            buildRows(*activeMechanisms_[i], static_cast<std::uint32_t>(i), dt, mechanismBodies_[i][0],
                      mechanismBodies_[i][1], out);
        // A row of non-finite numbers (a mover a script gave a NaN, an
        // attachment at one) would make every body it reaches NaN: it's
        // left out, as if its constraint were off. GUESS: what the client
        // does with one; it doesn't lose the character to it.
        std::erase_if(out, [&](const Row& r) {
            auto finite = [](Vector3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
            const bool bad = !std::isfinite(r.target) || std::isnan(r.lower) || std::isnan(r.upper)
                             || !finite(r.linear) || !finite(r.angularA) || !finite(r.angularB);
            if (bad)
                droppedRows_[n].push_back(r.mechanism);
            return bad;
        });
        for (Row& row : out)
            prepareRow(row);
    });
    rows_.clear();
    for (std::size_t n = 0; n < batches; ++n) {
        rows_.insert(rows_.end(), rowBatches_[n].begin(), rowBatches_[n].end());
        // Which constraint that was, said once.
        for (std::uint32_t index : droppedRows_[n]) {
            const Instance* instance = activeMechanisms_[index]->instance;
            if (std::ranges::find(nonFiniteReported_, instance) != nonFiniteReported_.end())
                continue;
            nonFiniteReported_.push_back(instance);
            std::println(stderr, "physics: {} {} isn't finite: left out until it is", instance->className(),
                         instance->fullName());
        }
    }
    // Warm start from last step's impulses, as the contacts do. A fixed
    // impulse (a spring's) goes in whole.
    for (Row& row : rows_) {
        const Mechanism& m = *activeMechanisms_[row.mechanism];
        if (row.lower == row.upper)
            row.impulse = row.lower;
        else if (m.used[row.slot])
            row.impulse = std::clamp(m.impulses[row.slot], row.lower, row.upper);
        else
            continue;
        applyRow(row, row.impulse);
    }
    groupBlocks();
}

// The client solves joints exactly (an LDL factorisation inside its PGS
// solver, RBX::Solver::LDLPGSSolverMT) and iterates only on contacts and
// bounded rows: a light part between heavy ones, a wheel on a steering
// knuckle on a car's frame, then holds as firmly as a single joint does.
// Here the rigid rows of each group of bodies they join make one block
// whose J M^-1 J^T is factored once a step (factorBlock); each iteration
// solves the whole block for the impulse change that meets all its targets
// at once (solveBlock). A block's bodies are all in one island.
void World::groupBlocks()
{
    blocks_.clear();
    // Bodies joined through rigid rows, union-find.
    std::vector<std::int32_t> group(bodies_.size());
    std::ranges::iota(group, 0);
    auto find = [&](std::int32_t x) {
        while (group[static_cast<std::size_t>(x)] != x)
            x = group[static_cast<std::size_t>(x)]
                = group[static_cast<std::size_t>(group[static_cast<std::size_t>(x)])];
        return x;
    };
    // GUESS: only rows with no bounds go in. Bounded ones (motors, servos,
    // limits) that held last step would converge faster in the block too,
    // but solving them there and clamping after can diverge.
    auto rigid = [](const Row& row) { return std::isinf(row.lower) && std::isinf(row.upper); };
    for (const Row& row : rows_)
        if (rigid(row) && row.a >= 0 && row.b >= 0)
            group[static_cast<std::size_t>(find(row.a))] = find(row.b);
    std::vector<std::int32_t> blockOf(bodies_.size(), -1);
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        row.block = -1;
        if (!rigid(row))
            continue;
        const std::int32_t root = find(row.a >= 0 ? row.a : row.b);
        std::int32_t& b = blockOf[static_cast<std::size_t>(root)];
        if (b < 0) {
            b = static_cast<std::int32_t>(blocks_.size());
            blocks_.emplace_back();
        }
        row.block = b;
        blocks_[static_cast<std::size_t>(b)].rows.push_back(i);
    }
    for (Block& block : blocks_) {
        const std::size_t n = block.rows.size();
        if (n == 1 || n > maxBlockRows) {
            for (std::uint32_t i : block.rows)
                rows_[i].block = -1; // a lone row needs no block; a huge one is left to iterate
            block.rows.clear();
        }
    }
}

void World::factorBlock(Block& block)
{
    const std::size_t n = block.rows.size();
    // J M^-1 J^T: rows meet through the bodies they share.
    auto& m = block.factor;
    m.assign(n * n, 0.0);
    auto entry = [&](const Row& r, const Row& c) {
        // Each row's Jacobian is -[linear, angularA] on a and [linear, angularB] on b.
        double sum = 0.0;
        for (int sr = 0; sr < 2; ++sr)
            for (int sc = 0; sc < 2; ++sc) {
                const std::int32_t body = sr == 0 ? r.a : r.b;
                if (body < 0 || body != (sc == 0 ? c.a : c.b))
                    continue;
                const Body& b = bodies_[static_cast<std::size_t>(body)];
                const Vector3 angR = sr == 0 ? r.angularA : r.angularB;
                const Vector3 angC = sc == 0 ? c.angularA : c.angularB;
                const double term = static_cast<double>(b.inverseMass * r.linear.dot(c.linear))
                                    + static_cast<double>(angR.dot(b.inverseInertia * angC));
                sum += sr == sc ? term : -term;
            }
        return sum;
    };
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j <= i; ++j)
            m[i * n + j] = entry(rows_[block.rows[i]], rows_[block.rows[j]]);
    // LDL^T in place: L below the diagonal, D on it. A row whose pivot
    // all but vanishes repeats the others (a loop closing on itself): it
    // is left out of the block, its D stored as 0, rather than letting a
    // tiny pivot blow the factor up.
    for (std::size_t j = 0; j < n; ++j) {
        const double diagonal = m[j * n + j];
        double d = diagonal;
        for (std::size_t k = 0; k < j; ++k)
            d -= m[j * n + k] * m[j * n + k] * m[k * n + k];
        const bool redundant = !(d > diagonal * 1e-6) || !(diagonal > 1e-30);
        m[j * n + j] = redundant ? 0.0 : d;
        for (std::size_t i = j + 1; i < n; ++i) {
            if (redundant) {
                m[i * n + j] = 0.0;
                continue;
            }
            double v = m[i * n + j];
            for (std::size_t k = 0; k < j; ++k)
                v -= m[i * n + k] * m[j * n + k] * m[k * n + k];
            m[i * n + j] = v / d;
        }
    }
}

void World::solveBlock(Block& block)
{
    const std::size_t n = block.rows.size();
    const auto& m = block.factor;
    auto& x = block.scratch;
    x.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Row& row = rows_[block.rows[i]];
        x[i] = static_cast<double>(row.target) - static_cast<double>(rowVelocity(row));
    }
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t k = 0; k < i; ++k)
            x[i] -= m[i * n + k] * x[k];
    for (std::size_t i = 0; i < n; ++i)
        x[i] = m[i * n + i] > 0.0 ? x[i] / m[i * n + i] : 0.0;
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t k = i + 1; k < n; ++k)
            x[i] -= m[k * n + i] * x[k];
    for (std::size_t i = 0; i < n; ++i) {
        Row& row = rows_[block.rows[i]];
        const auto change = static_cast<float>(x[i]);
        applyRow(row, change);
        row.impulse += change;
    }
}

// One mechanism's rows, onto out, between bodies ia and ib (-1: the world).
// Reads the World and writes only out, so mechanisms build on any thread.
void World::buildRows(const Mechanism& m, std::uint32_t index, float dt, std::int32_t ia, std::int32_t ib,
                      std::vector<Row>& out) const
{
    const Vector3 centerA = ia >= 0 ? bodies_[static_cast<std::size_t>(ia)].center : Vector3{};
    const Vector3 centerB = ib >= 0 ? bodies_[static_cast<std::size_t>(ib)].center : Vector3{};
    const float stiffness = jointBaumgarte / dt;

    auto row = [&](std::uint8_t slot) {
        Row r;
        r.a = ia;
        r.b = ib;
        r.mechanism = index;
        r.slot = slot;
        return r;
    };
    // Along n, through a point of each body.
    auto linear = [&](std::uint8_t slot, Vector3 n, Vector3 atA, Vector3 atB) {
        Row r = row(slot);
        r.linear = n;
        r.angularA = (atA - centerA).cross(n);
        r.angularB = (atB - centerB).cross(n);
        return r;
    };
    // About an axis.
    auto angular = [&](std::uint8_t slot, Vector3 axis) {
        Row r = row(slot);
        r.angularA = axis;
        r.angularB = axis;
        return r;
    };
    // Rigid: drive the error to 0 with no bound on the impulse.
    auto hold = [&](Row r, float error) {
        r.target = -stiffness * error;
        out.push_back(r);
    };
    auto motor = [&](Row r, float goal, float acceleration, float force) {
        const float v = rowVelocity(r);
        const float reach = strength(acceleration) * dt;
        r.target = v + std::clamp(goal - v, -reach, reach);
        r.upper = strength(force) * dt;
        r.lower = -r.upper;
        out.push_back(r);
    };
    // GUESS: the client can integrate the approach in substeps
    // (sub_1053b8f78, behind a flag); one step of it is the same at 240 Hz.
    auto servo = [&](Row r, float error, float responsiveness, float speed, float force) {
        const float v = rowVelocity(r);
        const float reach = strength(speed);
        r.target = std::clamp(v + dt * (0.25f * responsiveness * responsiveness * error - responsiveness * v), -reach,
                              reach);
        r.upper = strength(force) * dt;
        r.lower = -r.upper;
        out.push_back(r);
    };
    // One sided: J v > 0 opens the gap. Speculative while it's open, bouncing
    // with the restitution when it closes fast.
    auto limit = [&](Row r, float gap, float restitution) {
        const float closing = rowVelocity(r);
        r.target = gap >= 0.0f ? -gap / dt : -stiffness * gap;
        if (gap < 0.01f && closing < -restitutionThreshold)
            r.target = std::max(r.target, -restitution * closing);
        r.lower = 0.0f;
        out.push_back(r);
    };
    // Limits [lower, upper] on a position along a row: the nearer one.
    auto range = [&](Row r, float position, float lower, float upper, float restitution) {
        const float middle = (lower + upper) * 0.5f;
        const float half = std::abs(upper - lower) * 0.5f;
        const float off = position - middle;
        if (off >= 0.0f) {
            r.linear = -r.linear;
            r.angularA = -r.angularA;
            r.angularB = -r.angularB;
        }
        limit(r, half - std::abs(off), restitution);
    };

    if (instanceCast<BodyMover>(m.instance)) {
        if (ib < 0)
            return;
        constexpr std::array<Vector3, 3> axes{Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f},
                                              Vector3{0.0f, 0.0f, 1.0f}};
        if (auto* mover = instanceCast<BodyVelocity>(m.instance)) {
            for (int i = 0; i < 3; ++i) {
                Row r = linear(static_cast<std::uint8_t>(i), axes[static_cast<std::size_t>(i)], centerB, centerB);
                r.target = component(mover->velocity(), i);
                r.upper = strength(component(mover->maxForce(), i)) * dt;
                r.lower = -r.upper;
                out.push_back(r);
            }
        } else if (auto* holder = instanceCast<BodyPosition>(m.instance)) {
            float k = holder->p();
            float c = holder->d();
            holdGains(k, c, dt);
            const Vector3 at = m.parts[1]->position();
            const Vector3 off = at - holder->position();
            for (int i = 0; i < 3; ++i) {
                Row r = linear(static_cast<std::uint8_t>(i), axes[static_cast<std::size_t>(i)], at, at);
                // GUESS: from the velocity the step began with, so the row
                // also carries the part's weight and holds it at Position.
                const float v = component(bodies_[static_cast<std::size_t>(ib)].startVelocityAt(at), i);
                r.target = v - dt * (k * component(off, i) + c * v);
                r.upper = strength(component(holder->maxForce(), i)) * dt;
                r.lower = -r.upper;
                out.push_back(r);
            }
        }
        return;
    }
    if (auto* align = instanceCast<AlignOrientation>(m.instance)) {
        const CFrame f0 = m.attachments[0]->worldCFrame();
        const bool one = align->mode() == Enum::OrientationAlignmentMode::OneAttachment;
        const CFrame f1 = one ? align->cframe() : m.attachments[1]->worldCFrame();
        // The turn Attachment1 has left to face as Attachment0 does.
        Vector3 turn = rotationBetween(f1, f0);
        std::array<Vector3, 3> axes{Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, Vector3{0.0f, 0.0f, 1.0f}};
        std::size_t count = 3;
        if (align->primaryAxisOnly()) {
            // Only the primary axes: the turn between them, free about them.
            const Vector3 x0 = f0.rightVector();
            const Vector3 x1 = f1.rightVector();
            const Vector3 cross = x1.cross(x0);
            const float sine = cross.magnitude();
            const float angle = std::atan2(sine, x1.dot(x0));
            turn = sine > 1e-6f ? cross * (angle / sine) : Vector3{};
            axes = {f0.upVector(), x0.cross(f0.upVector()), x0};
            count = 2;
        }
        std::array<Row, 3> rows{};
        std::array<float, 3> targets{};
        float speed = 0.0f;
        const float responsiveness = align->responsiveness();
        for (std::size_t i = 0; i < count; ++i) {
            // Increasing J v turns side b (Attachment1) about the axis.
            rows[i] = angular(static_cast<std::uint8_t>(i), axes[i]);
            // Without the reaction only Attachment0's side is turned.
            if (!align->reactionTorqueEnabled())
                rows[i].b = -1;
            const float error = turn.dot(axes[i]);
            if (align->rigidityEnabled()) {
                hold(rows[i], -error);
                continue;
            }
            // The servos' approach, as servo() does, per axis.
            const float v = rowVelocity(rows[i]);
            targets[i] = v + dt * (0.25f * responsiveness * responsiveness * error - responsiveness * v);
            speed += targets[i] * targets[i];
        }
        if (align->rigidityEnabled())
            return;
        // MaxAngularVelocity holds the whole turn's speed, not each axis'.
        speed = std::sqrt(speed);
        const float most = strength(align->maxAngularVelocity());
        const float scale = speed > most ? most / speed : 1.0f;
        for (std::size_t i = 0; i < count; ++i) {
            rows[i].target = targets[i] * scale;
            rows[i].upper = strength(align->maxTorque()) * dt;
            rows[i].lower = -rows[i].upper;
            out.push_back(rows[i]);
        }
        return;
    }
    const CFrame f0 = m.attachments[0]->worldCFrame();
    const CFrame f1 = m.attachments[1]->worldCFrame();
    const Vector3 x0 = f0.rightVector();
    const Vector3 y0 = f0.upVector();
    const Vector3 z0 = x0.cross(y0);
    const Vector3 x1 = f1.rightVector();
    const Vector3 y1 = f1.upVector();
    const Vector3 z1 = x1.cross(y1);
    const Vector3 p0 = f0.position;
    const Vector3 p1 = f1.position;
    const Vector3 d = p1 - p0;
    // Attachment1 on Attachment0's primary axis.
    auto onAxis = [&] {
        hold(linear(0, y0, p1, p1), d.dot(y0));
        hold(linear(1, z0, p1, p1), d.dot(z0));
    };
    // The attachments at one point.
    auto together = [&] {
        hold(linear(0, {1.0f, 0.0f, 0.0f}, p0, p1), d.x);
        hold(linear(1, {0.0f, 1.0f, 0.0f}, p0, p1), d.y);
        hold(linear(2, {0.0f, 0.0f, 1.0f}, p0, p1), d.z);
    };
    // Attachment1's primary axis lined up with an axis, free to turn about
    // it; reference is perpendicular to the axis. For a small turn t about u,
    // axis x x1 is t u.
    auto alignAxes = [&](std::uint8_t slot, Vector3 axis, Vector3 reference) {
        const Vector3 e = axis.cross(x1);
        const Vector3 third = axis.cross(reference);
        hold(angular(slot, reference), e.dot(reference));
        hold(angular(static_cast<std::uint8_t>(slot + 1), third), e.dot(third));
    };
    // Turning about an axis, measured from a reference to Attachment1's
    // secondary axis, with HingeConstraint's actuator set.
    auto turning = [&](std::uint8_t slot, Vector3 axis, Vector3 reference, Enum::ActuatorType type, float velocity,
                       float acceleration, float torque, float target, float speed, float servoTorque,
                       float responsiveness, bool limited, float lower, float upper, float restitution) {
        const float angle = angleBetween(axis, reference, y1);
        lower *= radians;
        upper *= radians;
        if (type == Enum::ActuatorType::Motor) {
            motor(angular(slot, axis), velocity, acceleration, torque);
        } else if (type == Enum::ActuatorType::Servo) {
            // The short way round, unless that crosses the limits.
            target = limited ? std::clamp(target * radians, std::min(lower, upper), std::max(lower, upper))
                             : target * radians;
            float error = wrapAngle(target - angle);
            if (limited) {
                const float middle = (lower + upper) * 0.5f;
                const float from = wrapAngle(angle - middle);
                const float to = wrapAngle(target - middle);
                error = to - from;
            }
            servo(angular(slot, axis), error, responsiveness, speed, servoTorque);
        }
        if (limited) {
            const float middle = (lower + upper) * 0.5f;
            range(angular(static_cast<std::uint8_t>(slot + 1), axis), middle + wrapAngle(angle - middle), lower, upper,
                  restitution);
        }
    };
    // Along Attachment0's axis, with SlidingBallConstraint's actuator set.
    auto sliding = [&](const SlidingBallConstraint& s) {
        const float position = d.dot(x0); // CurrentPosition
        Row axial = linear(5, x0, p1, p1);
        if (s.actuatorType() == Enum::ActuatorType::Motor)
            motor(axial, s.velocity(), s.motorMaxAcceleration(), s.motorMaxForce());
        else if (s.actuatorType() == Enum::ActuatorType::Servo)
            servo(axial, s.targetPosition() - position, s.linearResponsiveness(), s.speed(), s.servoMaxForce());
        if (s.limitsEnabled())
            range(linear(6, x0, p1, p1), position, s.lowerLimit(), s.upperLimit(), s.restitution());
    };
    // Along the line between the attachments; nothing when they meet.
    const float length = d.magnitude();
    const Vector3 n = length > 1e-4f ? d / length : Vector3{};

    if (auto* prismatic = instanceCast<PrismaticConstraint>(m.instance)) {
        onAxis();
        // Facing the same way. For a small turn t about u this error is t u.
        const Vector3 e = (x0.cross(x1) + y0.cross(y1) + z0.cross(z1)) * 0.5f;
        hold(angular(2, x0), e.dot(x0));
        hold(angular(3, y0), e.dot(y0));
        hold(angular(4, z0), e.dot(z0));
        sliding(*prismatic);
    } else if (auto* cylinder = instanceCast<CylindricalConstraint>(m.instance)) {
        // It turns about Attachment0's primary axis tilted InclinationAngle
        // toward its secondary one (sub_1010f860e), measured from the
        // secondary axis tilted the same way (sub_1010f86b8). GUESS:
        // Attachment1's primary axis is held along it.
        const float inclination = cylinder->inclinationAngle() * radians;
        const Vector3 axis = x0 * std::cos(inclination) + y0 * std::sin(inclination);
        const Vector3 reference = y0 * std::cos(inclination) - x0 * std::sin(inclination);
        onAxis();
        alignAxes(2, axis, reference);
        sliding(*cylinder);
        turning(7, axis, reference, cylinder->angularActuatorType(), cylinder->angularVelocity(),
                cylinder->motorMaxAngularAcceleration(), cylinder->motorMaxTorque(), cylinder->targetAngle(),
                cylinder->angularSpeed(), cylinder->servoMaxTorque(), cylinder->angularResponsiveness(),
                cylinder->angularLimitsEnabled(), cylinder->lowerAngle(), cylinder->upperAngle(),
                cylinder->angularRestitution());
    } else if (auto* hinge = instanceCast<HingeConstraint>(m.instance)) {
        together();
        alignAxes(3, x0, y0);
        turning(5, x0, y0, hinge->actuatorType(), hinge->angularVelocity(), hinge->motorMaxAcceleration(),
                hinge->motorMaxTorque(), hinge->targetAngle(), hinge->angularSpeed(), hinge->servoMaxTorque(),
                hinge->angularResponsiveness(), hinge->limitsEnabled(), hinge->lowerAngle(), hinge->upperAngle(),
                hinge->restitution());
    } else if (instanceCast<BallSocketConstraint>(m.instance)) {
        // GUESS: its cone and twist limits aren't simulated.
        together();
    } else if (length > 1e-4f) {
        if (auto* spring = instanceCast<SpringConstraint>(m.instance)) {
            // The client gives up below 0.1 studs, where the direction is unsure.
            if (length > 0.1f) {
                const bool limited = spring->limitsEnabled();
                const float held = std::clamp(length, limited ? spring->minLength() : 0.0f,
                                              limited ? spring->maxLength() : floatMax);
                Row r = linear(0, n, p0, p1);
                prepareRow(r);
                // GUESS: damping is held to what stops the motion within the
                // step (Damping dt <= the effective mass). Past that the
                // explicit force overshoots and grows each step; the client
                // jitters there too, but a light part shouldn't explode.
                const float damping = std::min(spring->damping(), r.mass / dt);
                const float force = spring->stiffness() * (held - spring->freeLength()) + damping * rowVelocity(r);
                const float most = strength(spring->maxForce());
                r.upper = r.lower = -std::clamp(force, -most, most) * dt;
                out.push_back(r);
            }
            if (spring->limitsEnabled())
                range(linear(1, n, p0, p1), length, spring->minLength(), spring->maxLength(), 0.0f);
        } else if (auto* rope = instanceCast<RopeConstraint>(m.instance)) {
            // GUESS: winches aren't simulated; the rope is Length long.
            limit(linear(0, -n, p0, p1), rope->length() - length, rope->restitution());
        } else if (auto* rod = instanceCast<RodConstraint>(m.instance)) {
            // GUESS: its angle limits aren't simulated.
            hold(linear(0, n, p0, p1), length - rod->length());
        }
    }
}

// Rows not in a block, one at a time.
void World::solveRows(std::span<const std::uint32_t> rows)
{
    for (std::uint32_t index : rows) {
        Row& row = rows_[index];
        const float delta = (row.target - rowVelocity(row)) * row.mass;
        const float next = std::clamp(row.impulse + delta, row.lower, row.upper);
        applyRow(row, next - row.impulse);
        row.impulse = next;
    }
}

void World::storeRows()
{
    for (Mechanism* m : activeMechanisms_)
        m->used.fill(false);
    for (const Row& row : rows_) {
        Mechanism& m = *activeMechanisms_[row.mechanism];
        m.impulses[row.slot] = row.impulse;
        m.used[row.slot] = true;
    }
}

} // namespace lunar::physics
