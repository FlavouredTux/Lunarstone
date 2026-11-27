#include <lunar/physics/World.hpp>

#include "Solver.hpp"

#include <lunar/Seats.hpp>
#include <lunar/physics/Materials.hpp>
#include <lunar/physics/ThreadPool.hpp>
#include <lunar/terrain/TerrainMesher.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <print>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace lunar::physics {

namespace {

// Solver tuning. Not Roblox's (its solver is its own); chosen for steady stacks at 240 Hz.
constexpr int solverIterations = 10;
constexpr float contactMargin = 0.02f; // studs of gap still treated as touching
constexpr float penetrationSlop = 0.01f;
constexpr float baumgarte = 0.2f;
constexpr float restitutionThreshold = 2.0f; // studs/s; slower impacts don't bounce

// Sleep, after the client's SleepStage (RBX::SleepStage, step sub_10125ef30)
// and the per-body tests it reads:
//  - Pose history (sub_1031fa462, sub_1031fa4c8, sub_1031fa73c): a sample
//    every AssemblyHistorySkipSize = 8 steps; after a reset nothing sleeps
//    until AssemblyHistoryBufferSize = 15 samples went by (0.5 s at 240 Hz).
//    Linear motion is ((15 - 1) / 3)^2 times the largest squared distance of
//    the last 3 samples from their centroid; angular motion smooths the
//    orientation with SimSolverImprovedPartialSleepAngularWeight = 0.8 and
//    keeps 0.8 (m + 0.2 (turn * radius)^2). Still below
//    SimSolverImprovedPartialSleepTolerance^2 = 0.05^2.
//  - Accelerations (sub_1053be126): the change of velocity per step, filtered
//    as 0.9 f + 0.1 dv, its square's peak over 4-step windows. May sleep when
//    |f|^2 <= 1e-6 and (radius |f_angular|)^2 <= 10^2 * 1e-6
//    (SimSleepAngularAccelerationMultiplier), sub_1053bc038.
//  - Neighbours (sub_1012609ec): an awake neighbour moving more than twice
//    the tolerance keeps it awake; a still awake one sends it to Sleep
//    Checking; otherwise it sleeps.
//  - Sleep Checking (sub_10125f412), every 4th step: back awake when the
//    peak (sub_1053be352) is over 50 * 0.01^2; asleep once it's under
//    0.01^2 and no awake neighbour is left.
// GUESS: the radius (the client's body+0x38) is that of a sphere around
// the center of mass holding every part, and the sampled position is the
// center of mass.
constexpr int historySkip = 8;
constexpr int historyBuffer = 15;
constexpr int historyHeld = 3;
constexpr float angularWeight = 0.8f;
constexpr float stillTolerance = 0.05f * 0.05f;
constexpr float movingTolerance = 2.0f * stillTolerance;
constexpr float accelerationLimit = 1e-6f;
constexpr float angularAccelerationLimit = 10.0f * 10.0f * 1e-6f;
constexpr int checkInterval = 4;
constexpr float checkWake = 50.0f * 0.01f * 0.01f;
constexpr float checkSleep = 0.01f * 0.01f;
// GUESS: what wakes a sleeper on contact. The client pushes wakes through
// the contact graph; here a sleeper wakes when something moving touches it,
// and otherwise holds like an anchored part. Moving is the neighbour test's
// measure, or (between its samples) a speed that would pass it: three
// samples 8 steps apart spread 8 v dt, and ((14 / 3) 8 v dt)^2 > 0.005 for
// v > 0.4545 studs/s.
constexpr float wakeSpeed = 0.4545f;

// Spreading a step over threads. Awake colliders go to the narrow phase in
// runs of collideBatchSize; a phase goes to the pool once its work (awake
// colliders times collideCost, or contacts and rows) reaches parallelWork.
// GUESS: tuned on the battle royale map's vehicles, where handing a phase
// out and gathering it back costs about what 100 contacts take to solve.
constexpr std::size_t collideBatchSize = 16;
constexpr std::size_t collideCost = 4;
constexpr std::size_t parallelWork = 256;
// GUESS: meshing a terrain block for collision takes about what 128 contacts
// do, so two blocks are worth handing out.
constexpr std::size_t terrainMeshCost = 128;
// GUESS: decoding a collision blob or hulling one of its pieces, about 16 contacts' worth.
constexpr std::size_t hullCost = 16;

std::uint64_t pairKey(std::size_t a, std::size_t b)
{
    return (static_cast<std::uint64_t>(a) << 32) ^ static_cast<std::uint64_t>(b);
}

// Rotation by angle |w| about w.
CFrame axisAngle(Vector3 w)
{
    const float angle = w.magnitude();
    if (angle < 1e-9f)
        return {};
    const Vector3 k = w / angle;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;
    CFrame r;
    r.r[0][0] = t * k.x * k.x + c;
    r.r[0][1] = t * k.x * k.y - s * k.z;
    r.r[0][2] = t * k.x * k.z + s * k.y;
    r.r[1][0] = t * k.x * k.y + s * k.z;
    r.r[1][1] = t * k.y * k.y + c;
    r.r[1][2] = t * k.y * k.z - s * k.x;
    r.r[2][0] = t * k.x * k.z - s * k.y;
    r.r[2][1] = t * k.y * k.z + s * k.x;
    r.r[2][2] = t * k.z * k.z + c;
    return r;
}

// Gram-Schmidt, so rounding never shears a part.
CFrame orthonormalized(CFrame cf)
{
    Vector3 x = cf.rightVector().unit();
    Vector3 y = cf.upVector();
    y = (y - x * x.dot(y)).unit();
    const Vector3 z = x.cross(y);
    cf.r[0][0] = x.x;
    cf.r[1][0] = x.y;
    cf.r[2][0] = x.z;
    cf.r[0][1] = y.x;
    cf.r[1][1] = y.y;
    cf.r[2][1] = y.z;
    cf.r[0][2] = z.x;
    cf.r[1][2] = z.y;
    cf.r[2][2] = z.z;
    return cf;
}

Mat3 shapeInertia(const Shape& shape, float mass)
{
    Mat3 i = Mat3::zero();
    const Vector3 s = shape.size;
    switch (shape.kind) {
    case ShapeKind::Ball:
        i.m[0][0] = i.m[1][1] = i.m[2][2] = 0.4f * mass * shape.radius * shape.radius;
        break;
    case ShapeKind::Cylinder: {
        const float r2 = shape.radius * shape.radius;
        i.m[0][0] = 0.5f * mass * r2;
        i.m[1][1] = i.m[2][2] = mass * (3.0f * r2 + s.x * s.x) / 12.0f;
        break;
    }
    default:
        // Wedges are close enough to their box.
        i.m[0][0] = mass * (s.y * s.y + s.z * s.z) / 12.0f;
        i.m[1][1] = mass * (s.x * s.x + s.z * s.z) / 12.0f;
        i.m[2][2] = mass * (s.x * s.x + s.y * s.y) / 12.0f;
        break;
    }
    return i;
}

bool isJointProperty(std::string_view property)
{
    return property == "Part0" || property == "Part1" || property == "Enabled" || property == "Attachment0"
           || property == "Attachment1" || property == "IsKinematic";
}

// The constraints that hold their parts as rigidly as a Motor6D or Weld,
// making one assembly of them: a kinematic AnimationConstraint (the joints of
// upgraded avatars: Root, Waist, Neck... between the RigAttachments, with
// the animated Transform between them) and a RigidConstraint (the accessory
// holder on those avatars) [capture: a live server's R15 characters].
// GUESS: that the client's solver treats them as assembly joints, as the
// Roblox documentation describes kinematic AnimationConstraints.
bool rigidConstraint(const Instance& instance)
{
    if (const auto* animation = instanceCast<const AnimationConstraint>(&instance))
        return animation->isKinematic();
    return instance.isA("RigidConstraint");
}

// An attachment's part (through attachments under attachments) and its
// CFrame in that part's space.
std::pair<BasePart*, CFrame> attachmentFrame(const Attachment* attachment)
{
    CFrame frame;
    const Instance* node = attachment;
    while (const auto* a = instanceCast<const Attachment>(node)) {
        frame = a->cframe() * frame;
        node = a->parent();
    }
    return {const_cast<BasePart*>(instanceCast<const BasePart>(node)), frame};
}

// Part0 (Attachment0's part) to Part1 for a rigid constraint.
CFrame rigidConstraintOffset(const Constraint& constraint)
{
    const CFrame frame0 = attachmentFrame(constraint.attachment0()).second;
    const CFrame frame1 = attachmentFrame(constraint.attachment1()).second;
    if (const auto* animation = instanceCast<const AnimationConstraint>(&constraint))
        return frame0 * animation->transform() * frame1.inverse();
    return frame0 * frame1.inverse();
}

bool isStructuralProperty(std::string_view property)
{
    return property == "Size" || property == "Anchored" || property == "CanCollide" || property == "Shape"
           || property == "Massless" || property == "CustomPhysicalProperties" || property == "Material"
           || property == "RootPriority" || property == "PhysicalConfigData" || property == "PhysicsData";
}

} // namespace

Mat3 Mat3::inverse() const
{
    const auto& a = m;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                      - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                      + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::abs(det) < 1e-20f)
        return zero();
    const float inv = 1.0f / det;
    Mat3 r;
    r.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv;
    r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    r.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv;
    r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    r.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv;
    r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
    return r;
}

struct World::Contact {
    Assembly* a = nullptr;
    Assembly* b = nullptr; // null: static (or a sleeper nothing woke)
    std::uint32_t ia = 0;  // a's body
    std::int32_t ib = -1;  // b's body, -1 for none
    Vector3 point;
    Vector3 normal; // from a to b
    float depth = 0.0f;
    float friction = 0.0f;
    float restitution = 0.0f;
    Vector3 ra, rb;
    Vector3 t1, t2;
    float normalMass = 0.0f;
    float tangentMass1 = 0.0f;
    float tangentMass2 = 0.0f;
    float bias = 0.0f;
    float jn = 0.0f;
    float jt1 = 0.0f;
    float jt2 = 0.0f;
    std::uint64_t key = 0;
};

// One run of awake colliders' contacts, kept apart so runs can go on
// different threads and still join in the same order.
struct World::CollideBatch {
    std::vector<Contact> contacts;
    std::vector<Assembly*> woken;
    std::vector<Manifold> manifolds;
};

World::World(Workspace& workspace) : workspace_(workspace), threads_(ThreadPool::defaultThreads())
{
    connections_.push_back(workspace.descendantAdded.connect([this](Instance* instance) { added(*instance); }));
    connections_.push_back(workspace.descendantRemoving.connect([this](Instance* instance) { removing(*instance); }));
    for (const InstancePtr& descendant : workspace.descendants())
        added(*descendant);
}

World::~World()
{
    for (Connection& c : connections_)
        c.disconnect();
    for (auto& [part, c] : partConnections_)
        c.disconnect();
    for (auto& [instance, joint] : joints_)
        joint.connection.disconnect();
    for (auto& [instance, c] : noCollisionConstraints_)
        c.disconnect();
    for (const auto& mechanism : mechanisms_) {
        mechanism->changed.disconnect();
        for (Connection& c : mechanism->attachmentChanged)
            c.disconnect();
    }
}

void World::added(Instance& instance)
{
    if (auto* part = instanceCast<BasePart>(&instance)) {
        if (part->isA("Terrain"))
            return;
        Connection& connection = partConnections_[part];
        connection.disconnect();
        connection = part->changed.connect([this, part](std::string_view property) { partChanged(*part, property); });
        queuePart(part);
        return;
    }
    if (instance.isA("JointInstance") || instance.isA("WeldConstraint") || instance.isA("AnimationConstraint")
        || instance.isA("RigidConstraint")) {
        Joint& joint = joints_[&instance];
        joint.connection.disconnect();
        joint.connection = instance.changed.connect([this, at = &instance](std::string_view property) {
            if (isJointProperty(property))
                queueJoint(at);
            else if (property == "C0" || property == "C1")
                jointMoved(at);
        });
        queueJoint(&instance);
        return;
    }
    if (instance.isA("NoCollisionConstraint")) {
        Connection& connection = noCollisionConstraints_[&instance];
        connection.disconnect();
        connection = instance.changed.connect([this](std::string_view property) {
            if (isJointProperty(property))
                noCollideDirty_ = pending_ = true;
        });
        noCollideDirty_ = pending_ = true;
        return;
    }
    mechanismAdded(instance);
}

// The instance is still whole here, but may be gone by the next update, so
// everything that remembers it lets go now.
void World::removing(Instance& instance)
{
    if (auto* part = instanceCast<BasePart>(&instance)) {
        if (auto it = partConnections_.find(part); it != partConnections_.end()) {
            it->second.disconnect();
            partConnections_.erase(it);
        }
        pendingPartSet_.erase(part);
        removeCollider(part);
        return;
    }
    if (auto it = joints_.find(&instance); it != joints_.end()) {
        unlinkJoint(&instance, it->second);
        forgetJointParts(&instance, it->second);
        it->second.connection.disconnect();
        joints_.erase(it);
        pendingJointSet_.erase(&instance);
        std::erase(movedJoints_, &instance);
        return;
    }
    if (auto it = noCollisionConstraints_.find(&instance); it != noCollisionConstraints_.end()) {
        it->second.disconnect();
        noCollisionConstraints_.erase(it);
        noCollideDirty_ = pending_ = true;
        return;
    }
    mechanismRemoving(instance);
}

void World::partChanged(BasePart& part, std::string_view property)
{
    if (writing_)
        return;
    if (isStructuralProperty(property)) {
        queuePart(&part);
        return;
    }
    // A script's move (BasePart::CarryAssembly) carries the assembly as its
    // joints are now, so whatever changed since the last step (anywhere in
    // the Workspace) is taken in first. Skipped, the part went alone: the game
    // anchors a drawer's Main and tweens its CFrame, and on every frame a
    // change was waiting (its Heartbeat handlers rejoin our character's
    // CollisionPart, after the physics step and before the tweens [replay])
    // the drawer front and knobs welded to Main stayed behind, left wherever
    // the last carried frame had put them.
    if (property == "CFrame" && pending_ && BasePart::carryingAssembly())
        takeInChanges();
    auto it = colliderOf_.find(&part);
    if (it == colliderOf_.end())
        return; // not picked up yet; it will be read whole
    Assembly* assembly = colliders_[it->second].assembly;
    if (!assembly)
        return;
    if (property == "CFrame") {
        // Setting any part's CFrame moves its whole assembly there, joints and
        // all, anchored or not, as in Roblox: the game turns the character by
        // setting the CFrame of a part welded to the root, and carries its
        // anchored first-person rig the same way.
        const CFrame& cf = part.cframe();
        const bool finite = std::isfinite(cf.position.x) && std::isfinite(cf.position.y)
                            && std::isfinite(cf.position.z) && std::isfinite(cf.lookVector().x);
        // Only a script's move (BasePart::CarryAssembly), and only while the
        // assembly's joints are current (brought up to date above).
        const bool carries = BasePart::carryingAssembly() && !pending_ && assembly->root
                             && assembly->parts.size() > 1 && finite;
        if (carries) {
            writing_ = true;
            if (&part != assembly->root)
                assembly->root->setCFrame(part.cframe() * offsetFromRoot(*assembly, part).inverse());
            placeParts(*assembly);
            writing_ = false;
        }
        if (assembly->anchored) {
            // Refit the static tree before the next query.
            if (carries) {
                for (BasePart* moved : assembly->parts)
                    if (auto found = colliderOf_.find(moved); found != colliderOf_.end())
                        movedStatic_.push_back(found->second);
            } else {
                movedStatic_.push_back(it->second);
            }
            staticMoved_ = true;
        } else {
            assembly->asleep = false;
        }
    } else if (property == "AssemblyLinearVelocity" && !assembly->anchored) {
        assembly->linearVelocity = part.assemblyLinearVelocity();
        assembly->asleep = false;
    } else if (property == "AssemblyAngularVelocity" && !assembly->anchored) {
        assembly->angularVelocity = part.assemblyAngularVelocity();
        assembly->asleep = false;
    }
}

void World::reshape(BasePart& part)
{
    if (colliderOf_.contains(&part))
        queuePart(&part);
}

void World::queuePart(BasePart* part)
{
    if (pendingPartSet_.insert(part).second)
        pendingParts_.push_back(part);
    pending_ = true;
}

void World::queueJoint(Instance* joint)
{
    if (pendingJointSet_.insert(joint).second)
        pendingJoints_.push_back(joint);
    pending_ = true;
}

void World::jointMoved(Instance* joint)
{
    if (!writing_)
        movedJoints_.push_back(joint);
}

// A script moving a joint (TweenService on a Weld's C0) moves the parts it
// holds, as in Roblox. A moving assembly is placed from its joints after
// every step anyway (writeBack), but an anchored one never is: a temporary prop welds its two rock slabs to an anchored Center and tweens the
// welds' C0 to swing them up out of the ground, and they stayed buried,
// under the floor, while the server's invisible copy stood there to collide
// with [replay]. A sleeping one wakes and is placed at its next step.
void World::placeMovedJoints()
{
    std::ranges::sort(movedJoints_);
    const auto [first, last] = std::ranges::unique(movedJoints_);
    movedJoints_.erase(first, last);
    std::vector<Assembly*> placed;
    for (Instance* instance : movedJoints_) {
        auto it = joints_.find(instance);
        if (it == joints_.end() || !it->second.linked)
            continue;
        Assembly* assembly = colliders_[it->second.slot0].assembly;
        if (!assembly || !assembly->root || std::ranges::find(placed, assembly) != placed.end())
            continue;
        placed.push_back(assembly);
        if (!assembly->anchored) {
            if (assembly->asleep)
                wake(*assembly);
            continue;
        }
        writing_ = true;
        placeParts(*assembly);
        writing_ = false;
        for (BasePart* part : assembly->parts)
            if (auto found = colliderOf_.find(part); found != colliderOf_.end())
                movedStatic_.push_back(found->second);
        staticMoved_ = true;
    }
    movedJoints_.clear();
}

PhysicalProperties World::physicalProperties(const BasePart& part)
{
    if (const auto& custom = part.customPhysicalProperties())
        return *custom;
    return defaultPhysicalProperties(part.material());
}

void World::refresh()
{
    update();
}

void World::takeInChanges()
{
    const auto start = std::chrono::steady_clock::now();
    updateStructure();
    const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ++stats_.updates;
    stats_.updateSeconds += spent;
    stats_.worstUpdateSeconds = std::max(stats_.worstUpdateSeconds, spent);
}

void World::update()
{
    if (pending_)
        takeInChanges();
    if (!movedJoints_.empty())
        placeMovedJoints();
    if (staticMoved_)
        refitStatic();
    if (extraDirty_) {
        // Anchored parts added since the big tree was built, in a small one.
        std::ranges::sort(extraStatic_);
        const auto [first, last] = std::ranges::unique(extraStatic_);
        extraStatic_.erase(first, last);
        std::erase_if(extraStatic_, [&](std::uint32_t slot) {
            return !colliders_[slot].part || colliders_[slot].home != Home::StaticExtra;
        });
        std::vector<Aabb> boxes;
        boxes.reserve(extraStatic_.size());
        for (std::uint32_t slot : extraStatic_)
            boxes.push_back(colliders_[slot].box);
        extraBvh_.build(boxes, extraStatic_);
        extraDirty_ = false;
    }
}

// Only what changed is redone: new parts get shapes, joints link and unlink
// colliders, and only assemblies that lost or gained parts or joints are
// rebuilt. The anchored parts' big tree takes additions in a small side tree
// and is rebuilt whole only once that grows (or enough of it is gone).
void World::updateStructure()
{
    pending_ = false;
    std::vector<BasePart*> parts;
    parts.swap(pendingParts_);
    // New mesh geometry is decoded and hulled on the pool first: a big place
    // brings thousands of meshes at once.
    std::vector<SharedBytes> blobs;
    for (BasePart* part : parts)
        if (const auto* mesh = pendingPartSet_.contains(part) ? instanceCast<const TriangleMeshPart>(part) : nullptr)
            blobs.push_back(mesh->physicalConfigData() && !mesh->physicalConfigData()->empty()
                                ? mesh->physicalConfigData()
                                : mesh->physicsData());
    meshes_.prepare(blobs, [&](std::size_t count, std::function_ref<void(std::size_t)> fn) {
        forEach(count, count * hullCost, fn);
    });
    for (BasePart* part : parts) {
        if (!pendingPartSet_.erase(part))
            continue;         // a repeat, or gone again
        removeCollider(part); // re-read whole
        looseColliders_.push_back(addCollider(*part));
        if (auto it = jointsByPart_.find(part); it != jointsByPart_.end())
            for (Instance* joint : it->second)
                queueJoint(joint);
    }

    std::vector<Instance*> joints;
    joints.swap(pendingJoints_);
    for (Instance* instance : joints) {
        auto it = joints_.find(instance);
        if (!pendingJointSet_.erase(instance) || it == joints_.end())
            continue;
        Joint& joint = it->second;
        unlinkJoint(instance, joint);
        forgetJointParts(instance, joint);
        BasePart* p0 = nullptr;
        BasePart* p1 = nullptr;
        bool enabled = false;
        if (auto* j = instanceCast<JointInstance>(instance)) {
            p0 = j->part0();
            p1 = j->part1();
            enabled = j->enabled();
        } else if (auto* w = instanceCast<WeldConstraint>(instance)) {
            p0 = w->part0();
            p1 = w->part1();
            enabled = w->enabled();
        } else if (auto* c = instanceCast<Constraint>(instance)) {
            p0 = attachmentFrame(c->attachment0()).first;
            p1 = attachmentFrame(c->attachment1()).first;
            enabled = c->enabled() && rigidConstraint(*c) && c->attachment0() && c->attachment1();
        }
        joint.part0 = p0;
        joint.part1 = p1;
        if (p0)
            jointsByPart_[p0].push_back(instance);
        if (p1 && p1 != p0)
            jointsByPart_[p1].push_back(instance);
        if (enabled && p0 && p1 && p0 != p1)
            linkJoint(*instance, joint);
    }
    pending_ = false; // the joints queued above are done

    if (noCollideDirty_) {
        noCollideDirty_ = false;
        noCollide_.clear();
        for (const auto& [instance, connection] : noCollisionConstraints_)
            if (auto* nc = instanceCast<NoCollisionConstraint>(instance);
                nc && nc->enabled() && nc->part0() && nc->part1()) {
                noCollide_.insert({nc->part0(), nc->part1()});
                noCollide_.insert({nc->part1(), nc->part0()});
            }
    }

    reassemble();

    if (deadStatic_ * 4 > staticTreeSize_ || extraStatic_.size() > std::max<std::size_t>(512, staticTreeSize_ / 8))
        rebuildStatic();
    if (dynamicDirty_) {
        dynamicDirty_ = false;
        dynamicColliders_.clear();
        for (std::uint32_t slot = 0; slot < colliders_.size(); ++slot)
            if (colliders_[slot].part && colliders_[slot].home == Home::Dynamic)
                dynamicColliders_.push_back(slot);
        sleepingDirty_ = true;
    }
    if (!pendingWakes_.empty()) {
        if (sleepingDirty_)
            rebuildSleeping();
        for (const Aabb& box : pendingWakes_)
            wakeNear(box);
        pendingWakes_.clear();
    }
}

std::uint32_t World::addCollider(BasePart& part)
{
    std::uint32_t slot = 0;
    if (freeSlots_.empty()) {
        slot = static_cast<std::uint32_t>(colliders_.size());
        colliders_.emplace_back();
    } else {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    }
    Collider& c = colliders_[slot];
    c.part = &part;
    c.shape = shapeOf(part, &meshes_);
    if (unionGeometry_ && c.shape.kind == ShapeKind::Box)
        if (const auto* operation = instanceCast<const PartOperation>(&part)) {
            const SharedBytes& blob = operation->physicalConfigData() && !operation->physicalConfigData()->empty()
                                          ? operation->physicalConfigData()
                                          : operation->physicsData();
            std::string error;
            // Data that decoded as the box (BLOCK) stays the box; only data the client couldn't use is replaced.
            if (blob && !blob->empty() && !meshes_.get(blob, &error) && !error.empty())
                if (auto geometry = unionGeometry_(*operation))
                    c.shape = makeMeshShape(std::move(geometry), part.size());
        }
    c.box = Aabb::around(part.cframe(), c.shape.boundsHalfExtents());
    c.home = Home::None;
    colliderOf_[&part] = slot;
    if (isSeat(part))
        seats_.push_back(slot);
    ++colliderCount_;
    // Sleepers it lands in wake, as they would for any new contact.
    pendingWakes_.push_back(c.box.grown(contactMargin));
    return slot;
}

void World::removeCollider(const BasePart* part)
{
    auto it = colliderOf_.find(part);
    if (it == colliderOf_.end())
        return;
    const std::uint32_t slot = it->second;
    colliderOf_.erase(it);
    Collider& c = colliders_[slot];
    markDirty(c.assembly);
    // Whatever rested on it wakes up and falls.
    pendingWakes_.push_back(c.box.grown(contactMargin));
    while (!c.joints.empty()) {
        Instance* joint = c.joints.back();
        unlinkJoint(joint, joints_.at(joint));
    }
    switch (c.home) {
    case Home::StaticTree:
        ++deadStatic_;
        break;
    case Home::StaticExtra:
        extraDirty_ = true;
        break;
    case Home::Dynamic:
        dynamicDirty_ = true;
        break;
    case Home::None:
        std::erase(looseColliders_, slot);
        break;
    }
    std::erase(seats_, slot);
    c = Collider{};
    freeSlots_.push_back(slot);
    --colliderCount_;
    pending_ = true;
}

void World::linkJoint(Instance& instance, Joint& joint)
{
    auto i0 = colliderOf_.find(joint.part0);
    auto i1 = colliderOf_.find(joint.part1);
    if (i0 == colliderOf_.end() || i1 == colliderOf_.end())
        return;
    joint.linked = true;
    joint.slot0 = i0->second;
    joint.slot1 = i1->second;
    for (std::uint32_t slot : {joint.slot0, joint.slot1}) {
        colliders_[slot].joints.push_back(&instance);
        markDirty(colliders_[slot].assembly);
    }
}

void World::unlinkJoint(Instance* instance, Joint& joint)
{
    if (!joint.linked)
        return;
    joint.linked = false;
    for (std::uint32_t slot : {joint.slot0, joint.slot1}) {
        std::erase(colliders_[slot].joints, instance);
        markDirty(colliders_[slot].assembly);
    }
}

void World::forgetJointParts(Instance* instance, const Joint& joint)
{
    for (const BasePart* part : {joint.part0, joint.part1}) {
        if (!part)
            continue;
        if (auto it = jointsByPart_.find(part); it != jointsByPart_.end()) {
            std::erase(it->second, instance);
            if (it->second.empty())
                jointsByPart_.erase(it);
        }
    }
}

void World::markDirty(Assembly* assembly)
{
    if (!assembly || assembly->dirty)
        return;
    assembly->dirty = true;
    dirtyAssemblies_.push_back(assembly);
    pending_ = true;
}

void World::reassemble()
{
    if (dirtyAssemblies_.empty() && looseColliders_.empty())
        return;
    // Velocities of the assemblies being replaced, kept for the new ones by root part.
    std::unordered_map<const BasePart*, std::pair<Vector3, Vector3>> velocities;
    std::vector<std::uint32_t> work;
    work.swap(looseColliders_);
    for (const Assembly* assembly : dirtyAssemblies_) {
        if (!assembly->anchored)
            velocities[assembly->root] = {assembly->linearVelocity, assembly->angularVelocity};
        // Parts that left are no longer colliders (or are someone else's at a reused address).
        for (const BasePart* part : assembly->parts)
            if (auto it = colliderOf_.find(part);
                it != colliderOf_.end() && colliders_[it->second].assembly == assembly)
                work.push_back(it->second);
    }
    std::ranges::sort(work);

    // Joined colliders, flooded through linked joints. Joints only link or
    // unlink with both sides' assemblies marked, so this stays inside them.
    std::vector<std::uint8_t> seen(colliders_.size(), 0);
    std::vector<std::vector<std::uint32_t>> groups;
    for (std::uint32_t start : work) {
        if (seen[start])
            continue;
        seen[start] = 1;
        std::vector<std::uint32_t> members{start};
        for (std::size_t k = 0; k < members.size(); ++k)
            for (Instance* instance : colliders_[members[k]].joints) {
                const Joint& joint = joints_.at(instance);
                const std::uint32_t other = joint.slot0 == members[k] ? joint.slot1 : joint.slot0;
                if (!seen[other]) {
                    seen[other] = 1;
                    members.push_back(other);
                }
            }
        std::ranges::sort(members);
        groups.push_back(std::move(members));
    }

    ++structureEpoch_; // constraints may join other assemblies now
    std::erase_if(assemblies_, [](const std::unique_ptr<Assembly>& assembly) { return assembly->dirty; });
    dirtyAssemblies_.clear();
    for (const auto& members : groups) {
        auto assembly = makeAssembly(members);
        if (auto it = velocities.find(assembly->root); it != velocities.end() && !assembly->anchored) {
            assembly->linearVelocity = it->second.first;
            assembly->angularVelocity = it->second.second;
        }
        resetRest(*assembly);
        for (std::uint32_t slot : members) {
            Collider& c = colliders_[slot];
            c.assembly = assembly.get();
            if (assembly->anchored) {
                if (c.home == Home::Dynamic)
                    dynamicDirty_ = true;
                if (c.home == Home::Dynamic || c.home == Home::None) {
                    c.home = Home::StaticExtra;
                    c.box = Aabb::around(c.part->cframe(), c.shape.boundsHalfExtents());
                    extraStatic_.push_back(slot);
                    extraDirty_ = true;
                }
            } else if (c.home != Home::Dynamic) {
                if (c.home == Home::StaticTree)
                    ++deadStatic_;
                else if (c.home == Home::StaticExtra)
                    extraDirty_ = true;
                c.home = Home::Dynamic;
                dynamicDirty_ = true;
            }
        }
        assemblies_.push_back(std::move(assembly));
    }
    dynamicAssemblies_.clear();
    for (const auto& assembly : assemblies_)
        if (!assembly->anchored)
            dynamicAssemblies_.push_back(assembly.get());
}

std::unique_ptr<Assembly> World::makeAssembly(std::span<const std::uint32_t> members)
{
    auto assembly = std::make_unique<Assembly>();
    assembly->anchored = std::ranges::any_of(members, [&](std::uint32_t i) { return colliders_[i].part->anchored(); })
                         || (simulates_ && std::ranges::none_of(members, [&](std::uint32_t i) {
                                return simulates_(*colliders_[i].part);
                            }));
    // Root: an anchored part, then RootPriority, then not Massless, then
    // HumanoidRootPart, then the biggest.
    auto rank = [&](std::uint32_t i) {
        const BasePart& p = *colliders_[i].part;
        const Vector3 s = p.size();
        return std::tuple(p.anchored(), p.rootPriority(), !p.massless(), p.name() == "HumanoidRootPart",
                          s.x * s.y * s.z);
    };
    const std::uint32_t rootIndex = *std::ranges::max_element(members, {}, rank);
    assembly->root = colliders_[rootIndex].part;

    // Joint tree from the root, breadth first.
    std::vector<std::uint32_t> order{rootIndex};
    std::set<std::uint32_t> seen{rootIndex};
    for (std::size_t k = 0; k < order.size(); ++k) {
        const std::uint32_t at = order[k];
        for (Instance* instance : colliders_[at].joints) {
            const Joint& joint = joints_.at(instance);
            const bool isPart0 = joint.slot0 == at;
            const std::uint32_t other = isPart0 ? joint.slot1 : joint.slot0;
            if (!seen.insert(other).second)
                continue;
            order.push_back(other);
            // WeldConstraints hold the parts where they are now.
            const CFrame offset = joint.part0->cframe().inverse() * joint.part1->cframe();
            Assembly::Link link;
            link.part = colliders_[other].part;
            link.parent = colliders_[at].part;
            link.joint = instanceCast<JointInstance>(instance) || instanceCast<Constraint>(instance) ? instance : nullptr;
            link.forward = isPart0;
            link.offset = isPart0 ? offset : offset.inverse();
            assembly->links.push_back(link);
        }
    }
    for (std::uint32_t i : order)
        assembly->parts.push_back(colliders_[i].part);

    if (!assembly->anchored) {
        // Mass properties in root space; massless parts count only as the root.
        const CFrame rootInverse = assembly->root->cframe().inverse();
        float mass = 0.0f;
        Vector3 weighted;
        std::vector<std::pair<std::uint32_t, float>> masses;
        for (std::uint32_t i : order) {
            const BasePart& p = *colliders_[i].part;
            if (p.massless() && &p != assembly->root)
                continue;
            const float m = physicalProperties(p).density * colliders_[i].shape.volume();
            masses.emplace_back(i, m);
            mass += m;
            weighted = weighted + rootInverse.pointToWorldSpace(p.cframe().position) * m;
        }
        mass = std::max(mass, 1e-6f);
        assembly->mass = mass;
        assembly->inverseMass = 1.0f / mass;
        assembly->centerOfMass = weighted / mass;
        Mat3 inertia = Mat3::zero();
        for (const auto& [i, m] : masses) {
            const CFrame local = rootInverse * colliders_[i].part->cframe();
            const Mat3 r = Mat3::rotation(local);
            inertia = inertia + r * shapeInertia(colliders_[i].shape, m) * r.transposed();
            const Vector3 d = local.position - assembly->centerOfMass;
            const float d2 = d.dot(d);
            Mat3 shift = Mat3::zero();
            const float dv[3]{d.x, d.y, d.z};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    shift.m[a][b] = m * ((a == b ? d2 : 0.0f) - dv[a] * dv[b]);
            inertia = inertia + shift;
        }
        assembly->inverseInertiaLocal = inertia.inverse();
        for (std::uint32_t i : order) {
            const CFrame local = rootInverse * colliders_[i].part->cframe();
            const float reach = (local.position - assembly->centerOfMass).magnitude()
                                + colliders_[i].shape.boundsHalfExtents().magnitude();
            assembly->radius = std::max(assembly->radius, reach);
        }
        assembly->linearVelocity = assembly->root->assemblyLinearVelocity();
        assembly->angularVelocity = assembly->root->assemblyAngularVelocity();
    }
    return assembly;
}

void World::rebuildStatic()
{
    std::vector<Aabb> boxes;
    std::vector<std::uint32_t> ids;
    for (std::uint32_t slot = 0; slot < colliders_.size(); ++slot) {
        Collider& c = colliders_[slot];
        if (!c.part || (c.home != Home::StaticTree && c.home != Home::StaticExtra))
            continue;
        c.home = Home::StaticTree;
        ids.push_back(slot);
        boxes.push_back(c.box);
    }
    staticBvh_.build(boxes, ids);
    staticTreeSize_ = ids.size();
    deadStatic_ = 0;
    extraStatic_.clear();
    extraDirty_ = true;
    ++stats_.staticRebuilds;
}

void World::rebuildSleeping()
{
    sleepingDirty_ = false;
    std::vector<Aabb> boxes;
    std::vector<std::uint32_t> ids;
    for (std::uint32_t slot : dynamicColliders_) {
        Collider& c = colliders_[slot];
        if (!c.assembly->asleep || !c.part->canCollide())
            continue;
        c.box = Aabb::around(c.part->cframe(), c.shape.boundsHalfExtents());
        ids.push_back(slot);
        boxes.push_back(c.box.grown(contactMargin));
    }
    sleepingBvh_.build(boxes, ids);
}

void World::refitStatic()
{
    staticMoved_ = false;
    std::ranges::sort(movedStatic_);
    const auto [first, last] = std::ranges::unique(movedStatic_);
    movedStatic_.erase(first, last);
    // Many moved: one pass over the whole tree beats walking up from each.
    const bool whole = movedStatic_.size() * 8 > staticTreeSize_;
    for (std::uint32_t slot : movedStatic_) {
        Collider& c = colliders_[slot];
        if (!c.part || (c.home != Home::StaticTree && c.home != Home::StaticExtra))
            continue;
        c.box = Aabb::around(c.part->cframe(), c.shape.boundsHalfExtents());
        if (c.home == Home::StaticExtra)
            extraDirty_ = true;
        else if (!whole)
            staticBvh_.update(slot, c.box);
    }
    movedStatic_.clear();
    if (whole) {
        std::vector<Aabb> boxes(colliders_.size());
        for (std::uint32_t slot = 0; slot < colliders_.size(); ++slot)
            if (colliders_[slot].home == Home::StaticTree)
                boxes[slot] = colliders_[slot].box;
        staticBvh_.refit(boxes);
    }
}

template <class Fn>
void World::queryStatic(const Aabb& box, Fn fn) const
{
    staticBvh_.query(box, [&](std::uint32_t slot) {
        if (colliders_[slot].home == Home::StaticTree)
            fn(slot);
    });
    extraBvh_.query(box, [&](std::uint32_t slot) {
        if (colliders_[slot].home == Home::StaticExtra)
            fn(slot);
    });
}

void World::resetRest(Assembly& assembly)
{
    Assembly::Rest& rest = assembly.rest;
    rest = {};
    rest.countdown = historyBuffer;
    rest.smoothed = assembly.root->cframe().rotation();
    rest.lastLinear = assembly.linearVelocity;
    rest.lastAngular = assembly.angularVelocity;
}

void World::wake(Assembly& assembly)
{
    assembly.asleep = false;
    resetRest(assembly);
}

void World::wakeNear(const Aabb& box)
{
    sleepingBvh_.query(box, [&](std::uint32_t slot) {
        const Collider& c = colliders_[slot];
        if (c.part && c.home == Home::Dynamic && c.assembly->asleep)
            wake(*c.assembly);
    });
}

bool World::moving(const Assembly& assembly) const
{
    if (assembly.asleep || assembly.rest.checking)
        return false;
    // Last step's velocities: this step's already carry its gravity.
    const float speed = assembly.rest.lastLinear.magnitude() + assembly.radius * assembly.rest.lastAngular.magnitude();
    return assembly.rest.motion > movingTolerance || speed > wakeSpeed;
}

// One simulated step of the sleep test; see the notes at the top.
void World::updateRest(Assembly& assembly, bool touchingMoving, bool touchingAwake)
{
    Assembly::Rest& rest = assembly.rest;
    rest.linearFilter = rest.linearFilter * 0.9f + (assembly.linearVelocity - rest.lastLinear) * 0.1f;
    rest.angularFilter = rest.angularFilter * 0.9f + (assembly.angularVelocity - rest.lastAngular) * 0.1f;
    rest.lastLinear = assembly.linearVelocity;
    rest.lastAngular = assembly.angularVelocity;
    const float linear = rest.linearFilter.dot(rest.linearFilter);
    const float angular = rest.angularFilter.dot(rest.angularFilter);
    const bool windowStart = stepCount_ % checkInterval == 0;
    rest.linearPeak = windowStart ? linear : std::max(rest.linearPeak, linear);
    rest.angularPeak = windowStart ? angular : std::max(rest.angularPeak, angular);

    if (++rest.steps % historySkip == 0) {
        // A pose sample: spread of the last few positions, and turn from the smoothed orientation.
        for (int i = historyHeld - 1; i > 0; --i)
            rest.positions[i] = rest.positions[i - 1];
        rest.positions[0] = assembly.worldCenterOfMass();
        rest.samples = std::min(rest.samples + 1, historyHeld);
        Vector3 centroid;
        for (int i = 0; i < rest.samples; ++i)
            centroid = centroid + rest.positions[i];
        centroid = centroid / static_cast<float>(rest.samples);
        float spread = 0.0f;
        for (int i = 0; i < rest.samples; ++i)
            spread = std::max(spread, (rest.positions[i] - centroid).dot(rest.positions[i] - centroid));
        constexpr float spreadScale = static_cast<float>(historyBuffer - 1) / static_cast<float>(historyHeld);
        const float lin = spreadScale * spreadScale * spread;

        const CFrame current = assembly.root->cframe().rotation();
        // GUESS: which side the 0.8 weights (the client blends quaternions; this is the same on matrices).
        CFrame blended;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                blended.r[i][j] = (1.0f - angularWeight) * current.r[i][j] + angularWeight * rest.smoothed.r[i][j];
        rest.smoothed = orthonormalized(blended);
        const CFrame turn = current * rest.smoothed.inverse();
        const float cosine = std::clamp((turn.r[0][0] + turn.r[1][1] + turn.r[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
        const float swept = std::acos(cosine) * assembly.radius;
        rest.angular = angularWeight * (rest.angular + (1.0f - angularWeight) * swept * swept);
        rest.motion = lin + rest.angular;
        rest.countdown = std::max(rest.countdown - 1, 0);
    }

    if (assembly.gravityScale <= 0.0f) {
        rest.checking = false; // held up by a controller
        return;
    }
    const float radius2 = assembly.radius * assembly.radius;
    if (!rest.checking) {
        const bool still = rest.countdown == 0 && rest.motion < stillTolerance;
        const bool calm
            = rest.linearPeak <= accelerationLimit && radius2 * rest.angularPeak <= angularAccelerationLimit;
        if (!still || !calm || touchingMoving)
            return;
        if (touchingAwake) {
            rest.checking = true;
            return;
        }
    } else {
        if (!windowStart)
            return;
        const float peak = std::max(radius2 * rest.angularPeak, rest.linearPeak);
        if (peak > checkWake || touchingMoving) {
            rest.checking = false;
            return;
        }
        if (peak > checkSleep || touchingAwake)
            return;
    }
    assembly.asleep = true;
    rest.checking = false;
    rest.slept = true;
    rest.sleptAt = stepCount_;
    assembly.linearVelocity = {};
    assembly.angularVelocity = {};
    sleepingDirty_ = true;
}

namespace {

// A terrain block is 32 cells of 4 studs.
constexpr float blockStuds = 4.0f * static_cast<float>(TerrainChunk::size);

std::uint64_t blockKey(int x, int y, int z)
{
    auto bits = [](int v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v) & 0x1FFFFFu); };
    return bits(x) << 42 | bits(y) << 21 | bits(z);
}

// The blocks a span of studs along one axis reaches, a block's mesh reaching
// a cell past its low side, clipped to [first, last] (none when first > last).
std::pair<int, int> blockSpan(float min, float max, int first, int last)
{
    const float a = (min - 4.0f) / blockStuds;
    const float b = (max + 4.0f) / blockStuds;
    if (!(a <= b) || first > last)
        return {1, 0};
    // Clamped before converting, so far-flung parts can't overflow the ints.
    const int from = static_cast<int>(std::floor(std::max(a, static_cast<float>(first) - 1.0f)));
    const int to = static_cast<int>(std::floor(std::min(b, static_cast<float>(last) + 1.0f)));
    return {std::max(from, first), std::min(to, last)};
}

} // namespace

const World::BlockRange& World::terrainRange() const
{
    const Terrain* terrain = workspace_.terrain();
    const std::uint64_t revision = terrain ? terrain->revision() : 0;
    if (terrain == terrainRangeOf_ && revision == terrainRevision_)
        return terrainRange_;
    // The cells changed (a server streams them in as the game goes): what
    // was meshed of them goes with the range, and is meshed again as it's reached.
    terrainRangeOf_ = terrain;
    terrainRevision_ = revision;
    terrainBlocks_.clear();
    terrainRange_ = {};
    const std::vector<TerrainChunk>* chunks = terrain ? &terrain->chunks() : nullptr;
    if (!chunks || chunks->empty())
        return terrainRange_;
    terrainRange_.first.fill(std::numeric_limits<int>::max());
    terrainRange_.last.fill(std::numeric_limits<int>::min());
    for (const TerrainChunk& chunk : *chunks)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            terrainRange_.first[axis] = std::min(terrainRange_.first[axis], chunk.position[axis] - 1);
            terrainRange_.last[axis] = std::max(terrainRange_.last[axis], chunk.position[axis] + 1);
        }
    return terrainRange_;
}

const World::TerrainBlock& World::terrainBlock(int x, int y, int z) const
{
    // Far from every chunk there's nothing, and nothing worth remembering:
    // parts falling out of the world would otherwise leave a trail of empty blocks.
    static const TerrainBlock nothing;
    const BlockRange& range = terrainRange();
    if (x < range.first[0] || x > range.last[0] || y < range.first[1] || y > range.last[1] || z < range.first[2]
        || z > range.last[2])
        return nothing;
    auto& slot = terrainBlocks_[blockKey(x, y, z)];
    if (!slot)
        slot = meshTerrainBlock(x, y, z);
    return *slot;
}

std::unique_ptr<World::TerrainBlock> World::meshTerrainBlock(int x, int y, int z) const
{
    auto block = std::make_unique<TerrainBlock>();
    const Terrain* terrain = workspace_.terrain();
    // Surfaces near a block's edge can come from its neighbours' cells, so
    // only blocks with no stored neighbour at all are certainly empty.
    bool near = false;
    for (int dx = -1; dx <= 1 && !near; ++dx)
        for (int dy = -1; dy <= 1 && !near; ++dy)
            for (int dz = -1; dz <= 1 && !near; ++dz)
                near = terrain->findChunk(x + dx, y + dy, z + dz) != nullptr;
    if (!near)
        return block;
    const terrain::TerrainMeshChunk chunk = terrain::meshChunk(*terrain, {x, y, z});
    const auto& v = chunk.solid.vertices;
    const auto& ix = chunk.solid.indices;
    std::vector<Aabb> boxes;
    block->triangles.reserve(ix.size() / 3);
    boxes.reserve(ix.size() / 3);
    for (std::size_t i = 0; i + 2 < ix.size(); i += 3) {
        const TerrainTriangle t{v[ix[i]].position, v[ix[i + 1]].position, v[ix[i + 2]].position};
        Aabb box;
        box.expand(t.a);
        box.expand(t.b);
        box.expand(t.c);
        block->triangles.push_back(t);
        boxes.push_back(box);
    }
    block->bvh.build(boxes);
    return block;
}

template <class Fn>
void World::forTerrainBlocks(const Aabb& box, Fn fn) const
{
    if (!workspace_.terrain())
        return;
    // A block's mesh reaches a little past its low sides: look one cell further.
    const BlockRange& range = terrainRange();
    const auto [x0, x1] = blockSpan(box.min.x, box.max.x, range.first[0], range.last[0]);
    const auto [y0, y1] = blockSpan(box.min.y, box.max.y, range.first[1], range.last[1]);
    const auto [z0, z1] = blockSpan(box.min.z, box.max.z, range.first[2], range.last[2]);
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                fn(x, y, z);
}

std::size_t World::meshColliderCount() const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(colliders_, [](const Collider& c) { return c.part && c.shape.kind == ShapeKind::Mesh; }));
}

Assembly* World::assemblyOf(const BasePart* part)
{
    auto it = colliderOf_.find(part);
    return it == colliderOf_.end() ? nullptr : colliders_[it->second].assembly;
}

void World::placeParts(Assembly& assembly)
{
    for (const Assembly::Link& link : assembly.links) {
        CFrame cf;
        if (auto* constraint = instanceCast<Constraint>(link.joint)) {
            const CFrame rel = rigidConstraintOffset(*constraint); // Part0 to Part1
            cf = link.parent->cframe() * (link.forward ? rel : rel.inverse());
        } else if (auto* joint = instanceCast<JointInstance>(link.joint)) {
            CFrame transform;
            if (auto* motor = instanceCast<Motor6D>(joint))
                transform = motor->transform();
            const CFrame rel = joint->c0() * transform * joint->c1().inverse(); // Part0 to Part1
            cf = link.parent->cframe() * (link.forward ? rel : rel.inverse());
        } else {
            cf = link.parent->cframe() * link.offset;
        }
        link.part->setCFrame(cf);
    }
}

CFrame World::offsetFromRoot(const Assembly& assembly, const BasePart& part) const
{
    // placeParts' walk with the root at the origin, as far as the part.
    std::unordered_map<const BasePart*, CFrame> placed{{assembly.root, CFrame{}}};
    for (const Assembly::Link& link : assembly.links) {
        const auto parent = placed.find(link.parent);
        if (parent == placed.end())
            continue;
        CFrame rel;
        if (auto* constraint = instanceCast<Constraint>(link.joint)) {
            rel = rigidConstraintOffset(*constraint);
            rel = link.forward ? rel : rel.inverse();
        } else if (auto* joint = instanceCast<JointInstance>(link.joint)) {
            CFrame transform;
            if (auto* motor = instanceCast<Motor6D>(joint))
                transform = motor->transform();
            rel = joint->c0() * transform * joint->c1().inverse();
            rel = link.forward ? rel : rel.inverse();
        } else {
            rel = link.offset;
        }
        const CFrame at = parent->second * rel;
        if (link.part == &part)
            return at;
        placed.emplace(link.part, at);
    }
    return {};
}

bool World::finite(Vector3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void World::reportNonFinite(const Assembly& assembly)
{
    const Instance* root = assembly.root;
    if (std::ranges::find(nonFiniteReported_, root) != nonFiniteReported_.end())
        return;
    nonFiniteReported_.push_back(root);
    auto text = [](Vector3 v) { return std::format("({}, {}, {})", v.x, v.y, v.z); };
    std::println(stderr, "physics: {} went non-finite (velocity {}, spin {}, force {}, torque {}), kept where it was",
                 root->fullName(), text(assembly.linearVelocity), text(assembly.angularVelocity),
                 text(assembly.force), text(assembly.torque));
    for (const auto& m : mechanisms_) {
        if (m->a != &assembly && m->b != &assembly)
            continue;
        std::string what = std::format("physics:   {} {}", m->instance->className(), m->instance->fullName());
        if (const auto* mover = instanceCast<const BodyVelocity>(m->instance))
            what += std::format(" Velocity {} MaxForce {}", text(mover->velocity()), text(mover->maxForce()));
        else if (const auto* holder = instanceCast<const BodyPosition>(m->instance))
            what += std::format(" Position {} MaxForce {}", text(holder->position()), text(holder->maxForce()));
        for (const Attachment* attachment : m->attachments)
            if (attachment && !finite(attachment->worldCFrame().position))
                what += std::format(" (attachment {} at {})", attachment->fullName(),
                                    text(attachment->worldCFrame().position));
        std::println(stderr, "{}", what);
    }
}

void World::writeBack(Assembly& assembly)
{
    writing_ = true;
    placeParts(assembly);
    for (BasePart* part : assembly.parts) {
        part->setAssemblyLinearVelocity(assembly.linearVelocity);
        part->setAssemblyAngularVelocity(assembly.angularVelocity);
    }
    writing_ = false;
}

int World::advance(float dt)
{
    accumulator_ += dt;
    int steps = 0;
    // At most a 30 Hz frame's worth of steps (8): when a frame runs long the
    // simulation falls behind real time instead of catching up, which would
    // make the next frame longer still. The client throttles physics under
    // load the same way. GUESS: the exact limit.
    accumulator_ = std::min(accumulator_, maxCatchUpSteps * stepSize);
    while (accumulator_ >= stepSize) {
        step();
        accumulator_ -= stepSize;
        ++steps;
    }
    return steps;
}

// Contacts of every awake collider: against anchored parts, other awake
// colliders, sleepers and the terrain. A sleeper hit by something moving
// wakes and takes part from this step on; otherwise it holds like an
// anchored part. The colliders go in fixed runs, each with its contacts
// kept apart and joined in run order after: the same contacts in the same
// order on any number of threads.
void World::collideAll(float dt)
{
    contacts_.clear();
    // Awake colliders' boxes, swept by their velocity.
    awake_.clear();
    awakeBoxes_.clear();
    for (std::uint32_t i : dynamicColliders_) {
        Collider& c = colliders_[i];
        if (c.assembly->asleep)
            continue;
        c.box = Aabb::around(c.part->cframe(), c.shape.boundsHalfExtents());
        if (!c.part->canCollide())
            continue;
        const float sweep = c.assembly->velocityAt(c.part->cframe().position).magnitude() * dt;
        awakeBoxes_.push_back(c.box.grown(contactMargin + sweep));
        awake_.push_back(i);
    }
    awakeBvh_.build(awakeBoxes_);
    if (sleepingDirty_)
        rebuildSleeping();
    // The terrain blocks they reach are meshed now, side by side on the pool
    // (a part falling through a big map can reach thousands at once): the
    // runs only read them.
    if (const Terrain* terrain = workspace_.terrain()) {
        std::vector<std::array<int, 3>> missing;
        for (std::size_t k = 0; k < awake_.size(); ++k)
            if (groupsCollide(*colliders_[awake_[k]].part, *terrain))
                forTerrainBlocks(awakeBoxes_[k], [&](int x, int y, int z) {
                    if (!terrainBlocks_.contains(blockKey(x, y, z)))
                        missing.push_back({x, y, z});
                });
        std::ranges::sort(missing);
        missing.erase(std::ranges::unique(missing).begin(), missing.end());
        std::vector<std::unique_ptr<TerrainBlock>> meshed(missing.size());
        forEach(missing.size(), missing.size() * terrainMeshCost,
                [&](std::size_t i) { meshed[i] = meshTerrainBlock(missing[i][0], missing[i][1], missing[i][2]); });
        for (std::size_t i = 0; i < missing.size(); ++i)
            terrainBlocks_[blockKey(missing[i][0], missing[i][1], missing[i][2])] = std::move(meshed[i]);
    }

    // Runs of awake colliders of about the same work each: a mesh's every
    // convex piece is a narrow phase of its own, so a few furniture meshes
    // can outweigh dozens of boxes. Batches only split the work; the
    // contacts come out in the same order whatever the split.
    auto cost = [&](std::uint32_t slot) {
        const Shape& shape = colliders_[slot].shape;
        return collideCost * (1 + (shape.kind == ShapeKind::Mesh ? shape.mesh->pieces.size() : 0));
    };
    std::size_t total = 0;
    for (std::uint32_t slot : awake_)
        total += cost(slot);
    const std::size_t perBatch = std::max(collideBatchSize * collideCost, total / (std::size_t{threads_} * 4));
    batchStarts_.clear();
    std::size_t filled = perBatch;
    for (std::size_t k = 0; k < awake_.size(); ++k) {
        if (filled >= perBatch) {
            batchStarts_.push_back(k);
            filled = 0;
        }
        filled += cost(awake_[k]);
    }
    batchStarts_.push_back(awake_.size());
    const std::size_t batches = batchStarts_.size() - 1;
    if (collideBatches_.size() < batches)
        collideBatches_.resize(batches);
    forEach(batches, total,
            [&](std::size_t n) { collideBatch(collideBatches_[n], batchStarts_[n], batchStarts_[n + 1]); });
    for (std::size_t n = 0; n < batches; ++n)
        contacts_.insert(contacts_.end(), collideBatches_[n].contacts.begin(), collideBatches_[n].contacts.end());
    for (std::size_t n = 0; n < batches; ++n)
        for (Assembly* assembly : collideBatches_[n].woken)
            if (assembly->asleep)
                wake(*assembly);
}

bool World::groupsCollide(const BasePart& a, const BasePart& b) const
{
    if (a.collisionGroup() == "Default" && b.collisionGroup() == "Default")
        return true;
    return workspace_.collisionGroupsCollide(a.collisionGroup(), b.collisionGroup());
}

// The contacts of awake_[first, last). Reads the World, writes only the batch.
void World::collideBatch(CollideBatch& batch, std::size_t first, std::size_t last)
{
    batch.contacts.clear();
    batch.woken.clear();
    const auto& excluded = noCollide_;

    // The terrain material of a cell, remembered while one collider's contacts come in.
    struct {
        int x = 0, y = 0, z = 0;
        bool valid = false;
        Enum::Material material = Enum::Material::Air;
    } lastCell;
    auto terrainMaterial = [&](Vector3 p) {
        auto cell = [](float v) { return static_cast<int>(std::floor(v / 4.0f)); };
        const int x = cell(p.x);
        const int y = cell(p.y);
        const int z = cell(p.z);
        if (!lastCell.valid || lastCell.x != x || lastCell.y != y || lastCell.z != z)
            lastCell = {x, y, z, true, terrainMaterialAt(p)};
        return lastCell.material;
    };

    auto addContacts
        = [&](Assembly* a, Assembly* b, const BasePart& pa, const BasePart* pb, const Manifold& m, std::uint64_t key) {
              const PhysicalProperties ma = physicalProperties(pa);
              const PhysicalProperties mb
                  = pb ? physicalProperties(*pb) : defaultPhysicalProperties(terrainMaterial(m.points[0].position));
              const float friction = combineFriction(ma, mb);
              const float restitution = combineElasticity(ma, mb);
              for (int i = 0; i < m.count; ++i) {
                  Contact c;
                  c.a = a;
                  c.b = b && !b->anchored ? b : nullptr;
                  c.point = m.points[static_cast<std::size_t>(i)].position;
                  c.depth = m.points[static_cast<std::size_t>(i)].depth;
                  c.normal = m.normal;
                  c.friction = friction;
                  c.restitution = restitution;
                  c.key = key;
                  batch.contacts.push_back(c);
              }
          };

    std::vector<Manifold>& manifolds = batch.manifolds;
    manifolds.clear();
    auto addAll = [&](Assembly* a, Assembly* b, const BasePart& pa, const BasePart* pb, std::uint64_t key) {
        for (const Manifold& m : manifolds)
            if (m.count > 0)
                addContacts(a, b, pa, pb, m, key);
        manifolds.clear();
    };
    const Terrain* terrain = workspace_.terrain();
    for (std::size_t k = first; k < last; ++k) {
        const std::uint32_t i = awake_[k];
        const Collider& c = colliders_[i];
        const Aabb box = awakeBoxes_[k];
        const CFrame cf = c.part->cframe();
        const bool pushes = moving(*c.assembly);
        lastCell.valid = false;
        queryStatic(box, [&](std::uint32_t s) {
            const Collider& o = colliders_[s];
            if (!o.part->canCollide() || !groupsCollide(*c.part, *o.part) || excluded.contains({c.part, o.part}))
                return;
            collide(c.shape, cf, o.shape, o.part->cframe(), manifolds, contactMargin);
            addAll(c.assembly, nullptr, *c.part, o.part, pairKey(i, s));
        });
        awakeBvh_.query(box, [&](std::uint32_t d) {
            const std::uint32_t j = awake_[d];
            const Collider& o = colliders_[j];
            // Each pair once: the lower slot handles it.
            if (o.assembly == c.assembly || j < i)
                return;
            if (!groupsCollide(*c.part, *o.part) || excluded.contains({c.part, o.part}))
                return;
            collide(c.shape, cf, o.shape, o.part->cframe(), manifolds, contactMargin);
            addAll(c.assembly, o.assembly, *c.part, o.part, pairKey(i, j));
        });
        sleepingBvh_.query(box, [&](std::uint32_t j) {
            const Collider& o = colliders_[j];
            if (!o.part || o.home != Home::Dynamic || !o.assembly->asleep || !o.part->canCollide())
                return;
            if (!groupsCollide(*c.part, *o.part) || excluded.contains({c.part, o.part}))
                return;
            collide(c.shape, cf, o.shape, o.part->cframe(), manifolds, contactMargin);
            if (pushes && !manifolds.empty())
                batch.woken.push_back(o.assembly);
            addAll(c.assembly, pushes ? o.assembly : nullptr, *c.part, o.part, pairKey(i, j));
        });
        if (terrain && groupsCollide(*c.part, *terrain)) {
            forTerrainBlocks(box, [&](int bx, int by, int bz) {
                // Meshed in collideAll: other threads may be reading the map.
                const auto found = terrainBlocks_.find(blockKey(bx, by, bz));
                if (found == terrainBlocks_.end() || !found->second)
                    return;
                const TerrainBlock& block = *found->second;
                block.bvh.query(box, [&](std::uint32_t t) {
                    const TerrainTriangle& tri = block.triangles[t];
                    collideTriangle(c.shape, cf, tri.a, tri.b, tri.c, manifolds, contactMargin);
                    addAll(c.assembly, nullptr, *c.part, nullptr,
                           pairKey(i, 0x80000000u + t) ^ reinterpret_cast<std::uintptr_t>(&block));
                });
            });
        }
    }
}

void World::forEach(std::size_t count, std::size_t work, std::function_ref<void(std::size_t)> fn)
{
    if (threads_ > 1 && count > 1 && (alwaysParallel_ || work >= parallelWork)) {
        if (!pool_)
            pool_ = std::make_unique<ThreadPool>(threads_);
        parallelStep_ = true;
        pool_->run(count, fn);
        return;
    }
    for (std::size_t i = 0; i < count; ++i)
        fn(i);
}

void World::setSimulationFilter(std::function<bool(const BasePart&)> simulates)
{
    simulates_ = std::move(simulates);
    // Every assembly is rebuilt at the next update, moving or held by the new filter.
    for (const auto& assembly : assemblies_)
        markDirty(assembly.get());
}

void World::refilter(const BasePart& part)
{
    markDirty(assemblyOf(&part));
}

void World::setThreads(unsigned threads)
{
    threads_ = std::max(threads, 1u);
    pool_.reset();
}

std::uint32_t World::bodyOf(Assembly* assembly)
{
    if (assembly->solverStep != stepCount_) {
        assembly->solverStep = stepCount_;
        assembly->solverBody = static_cast<std::uint32_t>(bodies_.size());
        Body body;
        body.assembly = assembly;
        body.linear = assembly->linearVelocity;
        body.angular = assembly->angularVelocity;
        body.startLinear = assembly->rest.lastLinear;
        body.startAngular = assembly->rest.lastAngular;
        body.center = assembly->worldCenterOfMass();
        body.inverseInertia = assembly->inverseInertiaWorld();
        body.inverseMass = assembly->inverseMass;
        body.awake = !assembly->rest.checking;
        body.moving = moving(*assembly);
        bodies_.push_back(body);
    }
    return assembly->solverBody;
}

void World::solve(float dt)
{
    // Bodies: every assembly in a contact or a constraint, numbered as they come.
    bodies_.clear();
    for (Contact& c : contacts_) {
        c.ia = bodyOf(c.a);
        c.ib = c.b ? static_cast<std::int32_t>(bodyOf(c.b)) : -1;
    }
    buildRows(dt);
    // What the sleep test needs to know about neighbours: what touches it
    // and what a constraint holds it to.
    auto neighbours = [&](std::int32_t ia, std::int32_t ib) {
        if (ia < 0 || ib < 0)
            return;
        Body& a = bodies_[static_cast<std::size_t>(ia)];
        Body& b = bodies_[static_cast<std::size_t>(ib)];
        a.touchingMoving = a.touchingMoving || b.moving;
        a.touchingAwake = a.touchingAwake || b.awake;
        b.touchingMoving = b.touchingMoving || a.moving;
        b.touchingAwake = b.touchingAwake || a.awake;
    };
    for (const Contact& c : contacts_)
        neighbours(static_cast<std::int32_t>(c.ia), c.ib);
    for (const Row& row : rows_)
        neighbours(row.a, row.b);

    findIslands();
    stats_.islands = islandOrder_.size();
    forEach(islandOrder_.size(), contacts_.size() + rows_.size(),
            [&](std::size_t n) { solveIsland(islands_[islandOrder_[n]], dt); });

    nextWarmStart_.clear();
    for (const Contact& c : contacts_)
        nextWarmStart_.push_back({c.key, c.point, c.jn, c.jt1, c.jt2});
    std::ranges::stable_sort(nextWarmStart_, {}, &CachedImpulse::key);
    warmStart_.swap(nextWarmStart_);
    storeRows();
    for (const Body& body : bodies_) {
        body.assembly->linearVelocity = body.linear;
        body.assembly->angularVelocity = body.angular;
    }
}

// Islands of this step's bodies (see World::Island), numbered in body
// order, with their contacts, rows and blocks listed in the order made.
void World::findIslands()
{
    std::vector<std::uint32_t> parent(bodies_.size() + 1);
    std::ranges::iota(parent, 0u);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    auto join = [&](std::int32_t a, std::int32_t b) {
        if (a >= 0 && b >= 0)
            parent[find(static_cast<std::uint32_t>(a))] = find(static_cast<std::uint32_t>(b));
    };
    for (const Contact& c : contacts_)
        join(static_cast<std::int32_t>(c.ia), c.ib);
    for (const Row& row : rows_)
        join(row.a, row.b);
    // A row with the world on both sides (none should be) goes with a body
    // past the last: its own island.
    const auto none = static_cast<std::uint32_t>(bodies_.size());
    auto bodyOfRow = [&](const Row& row) {
        return row.a >= 0 ? static_cast<std::uint32_t>(row.a) : row.b >= 0 ? static_cast<std::uint32_t>(row.b) : none;
    };

    islands_.clear();
    std::vector<std::int32_t> islandOf(parent.size(), -1);
    auto islandAt = [&](std::uint32_t body) -> Island& {
        std::int32_t& n = islandOf[find(body)];
        if (n < 0) {
            n = static_cast<std::int32_t>(islands_.size());
            islands_.emplace_back();
        }
        return islands_[static_cast<std::size_t>(n)];
    };
    for (std::uint32_t body = 0; body < bodies_.size(); ++body)
        islandAt(body);
    // Counted, then laid out by island (a counting sort keeps their order).
    for (const Contact& c : contacts_)
        ++islandAt(c.ia).contacts;
    for (const Row& row : rows_)
        if (row.block < 0)
            ++islandAt(bodyOfRow(row)).rows;
    for (const Block& block : blocks_)
        if (!block.rows.empty())
            ++islandAt(bodyOfRow(rows_[block.rows.front()])).blocks;
    std::uint32_t contacts = 0;
    std::uint32_t rows = 0;
    std::uint32_t blocks = 0;
    for (Island& island : islands_) {
        island.firstContact = std::exchange(contacts, contacts + island.contacts);
        island.firstRow = std::exchange(rows, rows + island.rows);
        island.firstBlock = std::exchange(blocks, blocks + island.blocks);
        island.work = island.contacts + island.rows;
        island.contacts = island.rows = island.blocks = 0; // counted again as they're placed
    }
    islandContacts_.resize(contacts);
    islandRows_.resize(rows);
    islandBlocks_.resize(blocks);
    for (std::uint32_t i = 0; i < contacts_.size(); ++i) {
        Island& island = islandAt(contacts_[i].ia);
        islandContacts_[island.firstContact + island.contacts++] = i;
    }
    for (std::uint32_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].block < 0) {
            Island& island = islandAt(bodyOfRow(rows_[i]));
            islandRows_[island.firstRow + island.rows++] = i;
        }
    for (std::uint32_t i = 0; i < blocks_.size(); ++i)
        if (const Block& block = blocks_[i]; !block.rows.empty()) {
            Island& island = islandAt(bodyOfRow(rows_[block.rows.front()]));
            islandBlocks_[island.firstBlock + island.blocks++] = i;
            island.work += block.rows.size() * block.rows.size();
        }
    // Islands with nothing to solve (a body only woken this step) drop out;
    // the rest go biggest first, so a big one doesn't start last.
    islandOrder_.clear();
    for (std::uint32_t i = 0; i < islands_.size(); ++i)
        if (islands_[i].work > 0)
            islandOrder_.push_back(i);
    std::ranges::stable_sort(islandOrder_, std::greater{}, [&](std::uint32_t i) { return islands_[i].work; });
}

// One island's whole solve: factor its blocks, prepare its contacts, then
// iterate. Touches only the island's bodies, contacts, rows and blocks.
void World::solveIsland(const Island& island, float dt)
{
    const std::span<const std::uint32_t> contacts(islandContacts_.data() + island.firstContact, island.contacts);
    const std::span<const std::uint32_t> rows(islandRows_.data() + island.firstRow, island.rows);
    const std::span<const std::uint32_t> blocks(islandBlocks_.data() + island.firstBlock, island.blocks);
    for (std::uint32_t b : blocks)
        factorBlock(blocks_[b]);
    prepareContacts(contacts, dt);
    for (int iteration = 0; iteration < solverIterations; ++iteration) {
        for (std::uint32_t b : blocks)
            solveBlock(blocks_[b]);
        solveRows(rows);
        solveContacts(contacts, iteration);
    }
}

// Effective masses, bias, warm start.
void World::prepareContacts(std::span<const std::uint32_t> contacts, float dt)
{
    for (std::uint32_t index : contacts) {
        Contact& c = contacts_[index];
        Body& a = bodies_[c.ia];
        Body* b = c.ib >= 0 ? &bodies_[static_cast<std::size_t>(c.ib)] : nullptr;
        c.ra = c.point - a.center;
        c.rb = b ? c.point - b->center : Vector3{};
        const Mat3& iA = a.inverseInertia;
        const Mat3 iB = b ? b->inverseInertia : Mat3::zero();
        const float mB = b ? b->inverseMass : 0.0f;
        auto effective = [&](Vector3 dir) {
            const Vector3 ca = c.ra.cross(dir);
            const Vector3 cb = c.rb.cross(dir);
            const float k = a.inverseMass + mB + ca.dot(iA * ca) + cb.dot(iB * cb);
            return k > 0.0f ? 1.0f / k : 0.0f;
        };
        c.normalMass = effective(c.normal);
        c.t1 = std::abs(c.normal.x) < 0.9f ? c.normal.cross({1.0f, 0.0f, 0.0f}).unit()
                                           : c.normal.cross({0.0f, 1.0f, 0.0f}).unit();
        c.t2 = c.normal.cross(c.t1);
        c.tangentMass1 = effective(c.t1);
        c.tangentMass2 = effective(c.t2);
        // An impact is how fast they met as the step began: what this step's
        // forces add (a car's springs pressing its wheels down) isn't one.
        // GUESS: the client's restitution reads velocities from before its forces too.
        const Vector3 vb = b ? b->startVelocityAt(c.point) : Vector3{};
        const float approaching = (vb - a.startVelocityAt(c.point)).dot(c.normal);
        c.bias = baumgarte / dt * std::max(c.depth - penetrationSlop, 0.0f);
        if (approaching < -restitutionThreshold)
            c.bias = std::max(c.bias, -c.restitution * approaching);
        // A gap closes no faster than it would this step.
        if (c.depth < 0.0f)
            c.bias = c.depth / dt;

        const auto cached = std::ranges::equal_range(warmStart_, c.key, {}, &CachedImpulse::key);
        for (const CachedImpulse& impulse : cached)
            if ((impulse.position - c.point).dot(impulse.position - c.point) < 0.01f) {
                c.jn = impulse.normal;
                c.jt1 = impulse.tangent1;
                c.jt2 = impulse.tangent2;
                const Vector3 j = c.normal * c.jn + c.t1 * c.jt1 + c.t2 * c.jt2;
                a.applyImpulse(-j, c.point);
                if (b)
                    b->applyImpulse(j, c.point);
                break;
            }
    }
}

// One sweep over the contacts.
void World::solveContacts(std::span<const std::uint32_t> contacts, int iteration)
{
    // Alternating the order keeps one corner from always going first and tipping the box.
    for (std::size_t n = 0; n < contacts.size(); ++n) {
        Contact& c = contacts_[contacts[iteration % 2 == 0 ? n : contacts.size() - 1 - n]];
        Body& a = bodies_[c.ia];
        Body* b = c.ib >= 0 ? &bodies_[static_cast<std::size_t>(c.ib)] : nullptr;
        auto relative = [&] {
            const Vector3 vb = b ? b->velocityAt(c.point) : Vector3{};
            return vb - a.velocityAt(c.point);
        };
        // Friction first, bounded by the current normal impulse.
        const float limit = c.friction * c.jn;
        for (int axis = 0; axis < 2; ++axis) {
            const Vector3 t = axis == 0 ? c.t1 : c.t2;
            float& acc = axis == 0 ? c.jt1 : c.jt2;
            const float mass = axis == 0 ? c.tangentMass1 : c.tangentMass2;
            const float delta = -relative().dot(t) * mass;
            const float next = std::clamp(acc + delta, -limit, limit);
            const float applied = next - acc;
            acc = next;
            a.applyImpulse(-t * applied, c.point);
            if (b)
                b->applyImpulse(t * applied, c.point);
        }
        const float vn = relative().dot(c.normal);
        const float delta = (c.bias - vn) * c.normalMass;
        const float next = std::max(c.jn + delta, 0.0f);
        const float applied = next - c.jn;
        c.jn = next;
        a.applyImpulse(-c.normal * applied, c.point);
        if (b)
            b->applyImpulse(c.normal * applied, c.point);
    }
}

void World::step()
{
    const auto start = std::chrono::steady_clock::now();
    parallelStep_ = false;
    update();
    const float dt = stepSize;
    if (onStep) {
        onStep(dt);
        update(); // controllers break joints (a Humanoid dying): links mustn't outlive them
    }
    ++stepCount_;
    resolveMechanisms();

    const Vector3 gravity{0.0f, -workspace_.gravity(), 0.0f};
    for (Assembly* assembly : dynamicAssemblies_) {
        if (assembly->anchored)
            continue;
        if (assembly->force != Vector3{} || assembly->torque != Vector3{})
            assembly->asleep = false;
        if (assembly->asleep)
            continue;
        if (assembly->rest.slept) {
            // Woken from outside (a write, a controller, a force).
            assembly->rest.slept = false;
            resetRest(*assembly);
        }
        assembly->linearVelocity = assembly->linearVelocity
                                   + (gravity * assembly->gravityScale + assembly->force * assembly->inverseMass) * dt;
        assembly->angularVelocity = assembly->angularVelocity + assembly->inverseInertiaWorld() * assembly->torque * dt;
        assembly->force = {};
        assembly->torque = {};
    }

    collideAll(dt);
    solve(dt);

    // Integrate positions and write back.
    stats_.awakeAssemblies = 0;
    stats_.sleepingAssemblies = 0;
    stats_.contacts = contacts_.size();
    stats_.constraints = mechanisms_.size();
    stats_.activeConstraints = activeMechanisms_.size();
    stats_.rows = rows_.size();
    for (Assembly* assembly : dynamicAssemblies_) {
        if (assembly->anchored)
            continue;
        if (assembly->asleep) {
            ++stats_.sleepingAssemblies;
            continue;
        }
        ++stats_.awakeAssemblies;
        const CFrame root = assembly->root->cframe();
        const Vector3 com = root.pointToWorldSpace(assembly->centerOfMass) + assembly->linearVelocity * dt;
        CFrame rotated = orthonormalized(axisAngle(assembly->angularVelocity * dt) * root.rotation());
        rotated.position = com - rotated.vectorToWorldSpace(assembly->centerOfMass);
        // Motion that isn't finite would put the assembly nowhere (and with
        // it whatever it touches next): it stays where it was, at rest, and
        // what was acting on it is told once.
        if (!finite(assembly->linearVelocity) || !finite(assembly->angularVelocity) || !finite(rotated.position)
            || !finite(rotated.lookVector()) || !finite(rotated.upVector())) {
            reportNonFinite(*assembly);
            assembly->linearVelocity = {};
            assembly->angularVelocity = {};
            continue;
        }
        writing_ = true;
        assembly->root->setCFrame(rotated);
        writing_ = false;
        writeBack(*assembly);

        const Body* body = assembly->solverStep == stepCount_ ? &bodies_[assembly->solverBody] : nullptr;
        updateRest(*assembly, body && body->touchingMoving, body && body->touchingAwake);

        // Workspace.FallenPartsDestroyHeight.
        if (workspace_.fallHeightEnabled() && rotated.position.y < workspace_.fallenPartsDestroyHeight()) {
            // Hold on to them all first: a part going can take parts below it along.
            std::vector<InstancePtr> fallen;
            fallen.reserve(assembly->parts.size());
            for (BasePart* part : assembly->parts)
                fallen.push_back(part->shared_from_this());
            for (const InstancePtr& part : fallen)
                if (!part->destroyed())
                    part->destroy();
            assembly->anchored = true; // gone until the next update drops it
        }
    }
    sleepTogether();
    ++stats_.steps;
    if (parallelStep_)
        ++stats_.parallelSteps;
    stats_.stepSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<BasePart*> World::seatsTouching(const Instance& character)
{
    update();
    std::vector<BasePart*> found;
    if (seats_.empty())
        return found;
    constexpr float reach = 0.1f; // GUESS: how near counts as touching
    // The character's colliders and the box round them all.
    std::vector<std::pair<const Collider*, Aabb>> limbs;
    Aabb all;
    for (const InstancePtr& d : character.descendants())
        if (auto it = colliderOf_.find(instanceCast<const BasePart>(d.get())); it != colliderOf_.end()) {
            const Collider& limb = colliders_[it->second];
            const Aabb box = Aabb::around(limb.part->cframe(), limb.shape.boundsHalfExtents());
            limbs.emplace_back(&limb, box);
            all.expand(box.min);
            all.expand(box.max);
        }
    std::vector<Manifold> manifolds;
    for (std::uint32_t seatSlot : seats_) {
        const Collider& seat = colliders_[seatSlot];
        const Aabb seatBox = Aabb::around(seat.part->cframe(), seat.shape.boundsHalfExtents()).grown(reach);
        if (limbs.empty() || !seatBox.overlaps(all))
            continue;
        for (const auto& [limb, box] : limbs) {
            if (!seatBox.overlaps(box))
                continue;
            manifolds.clear();
            collide(limb->shape, limb->part->cframe(), seat.shape, seat.part->cframe(), manifolds, reach);
            if (std::ranges::any_of(manifolds, [](const Manifold& m) { return m.count > 0; })) {
                found.push_back(seat.part);
                break;
            }
        }
    }
    return found;
}

std::vector<World::Touch> World::touches(const Instance& character, const std::function<bool(const BasePart&)>& wants)
{
    std::vector<const BasePart*> limbs;
    for (const InstancePtr& d : character.descendants())
        if (const auto* part = instanceCast<const BasePart>(d.get()))
            limbs.push_back(part);
    return touches(limbs, wants);
}

std::vector<World::Touch> World::touches(std::span<const BasePart* const> ours,
                                         const std::function<bool(const BasePart&)>& wants)
{
    update();
    std::vector<Touch> found;
    constexpr float reach = 0.1f; // GUESS: as seatsTouching
    std::vector<Manifold> manifolds;
    for (const BasePart* part : ours) {
        auto it = colliderOf_.find(part);
        if (it == colliderOf_.end())
            continue;
        const Collider& limb = colliders_[it->second];
        if (!limb.part->canTouch())
            continue;
        const Aabb box = Aabb::around(limb.part->cframe(), limb.shape.boundsHalfExtents()).grown(reach);
        queryStatic(box, [&](std::uint32_t slot) {
            const Collider& other = colliders_[slot];
            if (!other.part->canTouch() || std::ranges::contains(ours, other.part) || !wants(*other.part)
                || !groupsCollide(*limb.part, *other.part))
                return;
            manifolds.clear();
            collide(limb.shape, limb.part->cframe(), other.shape, other.part->cframe(), manifolds, reach);
            if (std::ranges::any_of(manifolds, [](const Manifold& m) { return m.count > 0; }))
                found.push_back({limb.part, other.part});
        });
    }
    return found;
}

bool World::passesFilter(const BasePart& part, const RaycastParams& params) const
{
    if (params.respectCanCollide && !part.canCollide())
        return false;
    // CanQuery only applies to parts that don't collide.
    if (!part.canCollide() && !part.canQuery())
        return false;
    if (params.collisionGroup != part.collisionGroup()
        && !workspace_.collisionGroupsCollide(params.collisionGroup, part.collisionGroup()))
        return false;
    const bool listed = std::ranges::any_of(params.filterDescendantsInstances, [&](const Instance* i) {
        return i && (i == &part || part.isDescendantOf(i));
    });
    return params.filterType == Enum::RaycastFilterType::Include ? listed : !listed;
}

std::optional<RaycastResult> World::raycast(Vector3 origin, Vector3 direction, const RaycastParams& params) const
{
    const_cast<World*>(this)->update();
    std::optional<RaycastResult> best;
    float bestT = 1.0f;
    auto tryCollider = [&](const Collider& c, float maxT) {
        if (!passesFilter(*c.part, params))
            return maxT;
        const CFrame cf = c.part->cframe();
        const Vector3 localOrigin = cf.pointToObjectSpace(origin);
        const Vector3 localDir = cf.vectorToObjectSpace(direction);
        if (auto hit = physics::raycast(c.shape, localOrigin, localDir); hit && hit->t <= maxT) {
            bestT = hit->t;
            best = RaycastResult{c.part, origin + direction * hit->t, cf.vectorToWorldSpace(hit->normal),
                                 c.part->material(), direction.magnitude() * hit->t};
            return hit->t;
        }
        return maxT;
    };
    auto tryStatic = [&](std::uint32_t slot, float maxT, Home home) {
        return colliders_[slot].home == home ? tryCollider(colliders_[slot], std::min(maxT, bestT)) : maxT;
    };
    staticBvh_.raycast(origin, direction,
                       [&](std::uint32_t slot, float maxT) { return tryStatic(slot, maxT, Home::StaticTree); });
    extraBvh_.raycast(origin, direction,
                      [&](std::uint32_t slot, float maxT) { return tryStatic(slot, maxT, Home::StaticExtra); });
    sleepingBvh_.raycast(origin, direction, [&](std::uint32_t slot, float maxT) {
        const Collider& c = colliders_[slot];
        if (!c.part || c.home != Home::Dynamic || !c.assembly->asleep)
            return maxT;
        return tryCollider(c, std::min(maxT, bestT));
    });
    const Vector3 inv{1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z};
    for (std::uint32_t i : dynamicColliders_) {
        const Collider& c = colliders_[i];
        if (c.assembly->asleep && c.part->canCollide())
            continue; // in the sleeping tree
        const Aabb box = Aabb::around(c.part->cframe(), c.shape.boundsHalfExtents());
        if (rayAabb(box, origin, inv, bestT))
            tryCollider(c, bestT);
    }
    if (Terrain* terrain = workspace_.terrain(); terrain && passesFilter(*terrain, params)) {
        auto visit = [&](int bx, int by, int bz) {
            // Only blocks the ray passes through get meshed.
            const Vector3 lo{static_cast<float>(bx) * blockStuds - 4.0f, static_cast<float>(by) * blockStuds - 4.0f,
                             static_cast<float>(bz) * blockStuds - 4.0f};
            const Aabb region{lo, lo + Vector3{blockStuds + 8.0f, blockStuds + 8.0f, blockStuds + 8.0f}};
            if (!rayAabb(region, origin, inv, bestT))
                return;
            const TerrainBlock& block = terrainBlock(bx, by, bz);
            block.bvh.raycast(origin, direction, [&](std::uint32_t t, float maxT) {
                const TerrainTriangle& tri = block.triangles[t];
                auto hit = rayTriangle(origin, direction, tri.a, tri.b, tri.c);
                if (!hit || *hit > maxT || *hit >= bestT)
                    return maxT;
                bestT = *hit;
                Vector3 normal = (tri.b - tri.a).cross(tri.c - tri.a).unit();
                if (normal.dot(direction) > 0.0f)
                    normal = -normal;
                const Vector3 position = origin + direction * *hit;
                best = RaycastResult{terrain, position, normal, terrainMaterialAt(position - normal * 0.5f),
                                     direction.magnitude() * *hit};
                return *hit;
            });
        };
        // The blocks along the ray, slab by slab: a long diagonal ray's box
        // holds far more blocks than it passes. Each level keeps the 4 stud
        // reach past a block's sides, so nothing the region test passes is skipped.
        const BlockRange& blocks = terrainRange();
        auto span = [&](int axis, int cell, float t0, float t1) {
            const float o = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
            const float d = axis == 0 ? direction.x : axis == 1 ? direction.y : direction.z;
            if (d == 0.0f)
                return std::pair{t0, t1};
            const float a = (static_cast<float>(cell) * blockStuds - 4.0f - o) / d;
            const float b = (static_cast<float>(cell + 1) * blockStuds + 4.0f - o) / d;
            return std::pair{std::max(t0, std::min(a, b)), std::min(t1, std::max(a, b))};
        };
        auto at = [&](int axis, float t) {
            const Vector3 p = origin + direction * t;
            return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
        };
        auto range = [&](int axis, float t0, float t1) {
            const float a = at(axis, t0);
            const float b = at(axis, t1);
            const auto i = static_cast<std::size_t>(axis);
            return blockSpan(std::min(a, b), std::max(a, b), blocks.first[i], blocks.last[i]);
        };
        const auto [x0, x1] = range(0, 0.0f, bestT);
        for (int bx = x0; bx <= x1; ++bx) {
            const auto [tx0, tx1] = span(0, bx, 0.0f, bestT);
            if (tx0 > tx1)
                continue;
            const auto [y0, y1] = range(1, tx0, tx1);
            for (int by = y0; by <= y1; ++by) {
                const auto [ty0, ty1] = span(1, by, tx0, tx1);
                if (ty0 > ty1)
                    continue;
                const auto [z0, z1] = range(2, ty0, ty1);
                for (int bz = z0; bz <= z1; ++bz)
                    visit(bx, by, bz);
            }
        }
        if (!params.ignoreWater) {
            // Water surface: the first step from a cell without water into one with it.
            const float length = direction.magnitude();
            const Vector3 dirUnit = direction / std::max(length, 1e-9f);
            const float reach = length * bestT;
            const bool startWet = waterAt(origin) > 0.0f;
            // Samples in one cell all read the same voxel; look it up once.
            auto cell = [](Vector3 p) {
                return std::array{static_cast<int>(std::floor(p.x / 4.0f)), static_cast<int>(std::floor(p.y / 4.0f)),
                                  static_cast<int>(std::floor(p.z / 4.0f))};
            };
            std::array<int, 3> lastCell{};
            bool lastWet = false;
            for (int stud = 0; static_cast<float>(stud) <= reach && !startWet; ++stud) {
                const auto d = static_cast<float>(stud);
                const Vector3 p = origin + dirUnit * d;
                if (stud == 0 || cell(p) != lastCell) {
                    lastCell = cell(p);
                    lastWet = waterAt(p) > 0.0f;
                }
                if (lastWet) {
                    best = RaycastResult{terrain, p, -dirUnit, Enum::Material::Water, d};
                    break;
                }
            }
        }
    }
    return best;
}

Enum::Material World::terrainMaterialAt(Vector3 position) const
{
    const Terrain* terrain = workspace_.terrain();
    if (!terrain)
        return Enum::Material::Air;
    auto cell = [](float v) { return static_cast<int>(std::floor(v / 4.0f)); };
    const int x = cell(position.x);
    const int y = cell(position.y);
    const int z = cell(position.z);
    TerrainVoxel voxel = terrain->voxelAt(x, y, z);
    if (voxel.material != Enum::Material::Air && voxel.material != Enum::Material::Water)
        return voxel.material;
    // The surface can sit in a neighbour's cell: take the fullest solid one around.
    float bestOccupancy = 0.0f;
    Enum::Material best = Enum::Material::Air;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
                const TerrainVoxel v = terrain->voxelAt(x + dx, y + dy, z + dz);
                if (v.material == Enum::Material::Water || v.material == Enum::Material::Air)
                    continue;
                if (v.occupancyFraction() > bestOccupancy) {
                    bestOccupancy = v.occupancyFraction();
                    best = v.material;
                }
            }
    return best;
}

float World::waterAt(Vector3 position) const
{
    const Terrain* terrain = workspace_.terrain();
    if (!terrain)
        return 0.0f;
    auto cell = [](float v) { return static_cast<int>(std::floor(v / 4.0f)); };
    const TerrainVoxel v = terrain->voxelAt(cell(position.x), cell(position.y), cell(position.z));
    // Water cells are full whatever their occupancy, as the terrain mesher
    // draws them (the big battle royale map's sea is stored all but empty).
    if (v.material == Enum::Material::Water)
        return 1.0f;
    return v.water > 0 ? static_cast<float>(v.water + 1) / 256.0f : 0.0f;
}

} // namespace lunar::physics
