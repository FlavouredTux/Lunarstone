#pragma once

// The physics world of a Workspace: colliders for every part and the
// terrain, assemblies built from joints, raycasts, and a fixed-rate rigid
// body simulation that writes CFrames and velocities back to the parts.

#include <lunar/Classes.hpp>
#include <lunar/physics/Bvh.hpp>
#include <lunar/physics/Collide.hpp>
#include <lunar/physics/Geometry.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lunar::physics {

class ThreadPool;

// The physics rate. 240 Hz is the solver frequency Roblox documents and the
// binary's world step uses; see physics/README in the test pins.
constexpr float stepRate = 240.0f;
constexpr float stepSize = 1.0f / stepRate;
constexpr float maxCatchUpSteps = 8.0f; // see World::advance

// Workspace:Raycast's RaycastParams.
struct RaycastParams {
    std::vector<const Instance*> filterDescendantsInstances;
    Enum::RaycastFilterType filterType = Enum::RaycastFilterType::Exclude;
    bool ignoreWater = false;
    std::string collisionGroup = "Default";
    bool respectCanCollide = false;
};

// Workspace:Raycast's RaycastResult. Instance is the Terrain for terrain hits.
struct RaycastResult {
    BasePart* instance = nullptr;
    Vector3 position;
    Vector3 normal;
    Enum::Material material = Enum::Material::Plastic;
    float distance = 0.0f;
};

// 3x3 matrix for inertia tensors.
struct Mat3 {
    float m[3][3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    Vector3 operator*(Vector3 v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    static Mat3 zero()
    {
        Mat3 z;
        z.m[0][0] = z.m[1][1] = z.m[2][2] = 0.0f;
        return z;
    }
    static Mat3 rotation(const CFrame& cf)
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = cf.r[i][j];
        return r;
    }
    Mat3 transposed() const
    {
        Mat3 t;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t.m[i][j] = m[j][i];
        return t;
    }
    friend Mat3 operator*(const Mat3& a, const Mat3& b)
    {
        Mat3 r = zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }
    friend Mat3 operator+(const Mat3& a, const Mat3& b)
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[i][j] + b.m[i][j];
        return r;
    }
    Mat3 inverse() const;
};

// One rigid assembly: parts held together by joints (Weld, Motor6D,
// WeldConstraint...), moving as one body. Anchored if any part is.
struct Assembly {
    // How a part hangs off the part before it in the joint tree.
    struct Link {
        BasePart* part = nullptr;
        BasePart* parent = nullptr;
        Instance* joint = nullptr; // a JointInstance or rigid Constraint, or null for a WeldConstraint
        bool forward = true;       // parent is the joint's Part0
        CFrame offset;             // parent to part, for WeldConstraints
    };

    // What the sleep test keeps between steps (see World.cpp's sleep notes).
    struct Rest {
        int countdown = 0; // pose samples to go before it may sleep
        int steps = 0;     // simulated steps since the last reset
        int samples = 0;   // pose samples held, up to 3
        Vector3 positions[3];
        CFrame smoothed;         // orientation, smoothed over the samples
        float angular = 0.0f;    // smoothed turn since, times the radius, squared
        float motion = 0.0f;     // the pose test's measure at the last sample
        Vector3 linearFilter;    // filtered change of velocity per step
        Vector3 angularFilter;   // and of angular velocity
        float linearPeak = 0.0f; // their largest squares over the last few steps
        float angularPeak = 0.0f;
        Vector3 lastLinear; // velocities a step ago
        Vector3 lastAngular;
        bool checking = false;     // still, but waits for awake neighbours before sleeping
        bool slept = false;        // asleep as of the last step: clearing asleep wakes it
        std::uint64_t sleptAt = 0; // the step it fell asleep in
    };

    BasePart* root = nullptr;
    std::vector<BasePart*> parts; // root first
    std::vector<Link> links;      // every part but the root, parents before children
    bool anchored = true;
    float mass = 0.0f;
    float inverseMass = 0.0f;
    Vector3 centerOfMass;     // in root space
    Mat3 inverseInertiaLocal; // in root space, about the center of mass
    float radius = 0.0f;      // of a sphere around the center of mass holding every part
    Vector3 linearVelocity;   // of the center of mass
    Vector3 angularVelocity;
    Vector3 force; // accumulated for the next step, cleared after it
    Vector3 torque;
    // Sleeping assemblies aren't simulated. Anything may clear this to wake one.
    bool asleep = false;
    Rest rest;
    float gravityScale = 1.0f; // 0 for assemblies a controller holds up itself

    // The World's own bookkeeping.
    bool dirty = false;           // to be rebuilt from its parts
    std::uint64_t solverStep = 0; // the step solverBody numbers it in
    std::uint32_t solverBody = 0;

    Vector3 worldCenterOfMass() const { return root->cframe().pointToWorldSpace(centerOfMass); }
    Mat3 inverseInertiaWorld() const
    {
        const Mat3 r = Mat3::rotation(root->cframe());
        return r * inverseInertiaLocal * r.transposed();
    }
    Vector3 velocityAt(Vector3 world) const
    {
        return linearVelocity + angularVelocity.cross(world - worldCenterOfMass());
    }
    void applyImpulse(Vector3 impulse, Vector3 at)
    {
        if (anchored)
            return;
        linearVelocity = linearVelocity + impulse * inverseMass;
        angularVelocity = angularVelocity + inverseInertiaWorld() * (at - worldCenterOfMass()).cross(impulse);
    }
};

class World {
public:
    explicit World(Workspace& workspace);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Workspace& workspace() const { return workspace_; }

    // Advances by dt seconds in fixed steps of stepSize; leftover time carries
    // over to the next call. Returns the steps taken.
    int advance(float dt);
    // How far the carried-over time is into the next step, 0..1: for drawing
    // between the last two steps instead of snapping to whole steps.
    float interpolationAlpha() const { return accumulator_ / stepSize; }
    // One fixed step.
    void step();
    // Called at the start of every step, before forces integrate: where
    // Humanoids drive their characters.
    std::function<void(float dt)> onStep;

    // What a union collides as when its collision data is only a hash
    // (the game's walls: 32 hex digits, nothing the client can decode): the
    // geometry of the parts it was made from, or null while that isn't
    // known (it then collides as its box until reshape).
    using UnionGeometry = std::function<std::shared_ptr<const MeshGeometry>(const PartOperation&)>;
    void setUnionGeometry(UnionGeometry geometry) { unionGeometry_ = std::move(geometry); }
    // Reads a part's shape again at the next update (its union's parts arrived).
    void reshape(BasePart& part);

    // Workspace:Raycast. direction's length is the ray's reach.
    std::optional<RaycastResult> raycast(Vector3 origin, Vector3 direction, const RaycastParams& params = {}) const;

    // The assembly a part moves with; null for parts outside the workspace
    // or before the next step picks them up.
    Assembly* assemblyOf(const BasePart* part);
    const std::vector<std::unique_ptr<Assembly>>& assemblies() const { return assemblies_; }
    // Pick up added, removed and changed parts and joints now rather than at
    // the next step.
    void refresh();

    // Material physics for a part: CustomPhysicalProperties or its material's.
    static PhysicalProperties physicalProperties(const BasePart& part);
    // Terrain material at a world point (Air outside solid cells).
    Enum::Material terrainMaterialAt(Vector3 position) const;
    // Fraction of the terrain cell around position filled with water.
    float waterAt(Vector3 position) const;

    // Seats (Seat, VehicleSeat) a part of the character touches or all but
    // touches, whether or not the parts collide: a touch, as Touched sees one.
    std::vector<BasePart*> seatsTouching(const Instance& character);
    // The pairs a character's parts make with the anchored parts that `wants`
    // (a server listening for their Touched: a TouchTransmitter), touching or
    // all but, colliding or not; CanTouch off on either side, or collision
    // groups that don't collide, is no touch (the client's filter 0x1011ee848).
    // Ours first. What the client reports to the server for its character.
    struct Touch {
        BasePart* ours = nullptr;
        BasePart* other = nullptr;
        bool operator==(const Touch&) const = default;
    };
    std::vector<Touch> touches(const Instance& character, const std::function<bool(const BasePart&)>& wants);
    // The same for any parts of ours: a character's and the parts the server
    // gave us besides. Pairs among them are none.
    std::vector<Touch> touches(std::span<const BasePart* const> ours,
                               const std::function<bool(const BasePart&)>& wants);

    std::size_t colliderCount() const { return colliderCount_; }
    // MeshParts and unions with convex pieces, and the distinct geometries they share.
    std::size_t meshColliderCount() const;
    std::size_t meshGeometryCount() const { return meshes_.size(); }
    // Terrain blocks (32^3 cells) meshed for collision so far: they're
    // meshed the first time something comes near them.
    std::size_t terrainBlockCount() const { return terrainBlocks_.size(); }

    // Counters for profiling.
    struct Stats {
        std::size_t updates = 0;    // times parts or joints changed and were picked up
        double updateSeconds = 0.0; // spent picking them up
        double worstUpdateSeconds = 0.0;
        std::size_t staticRebuilds = 0;  // times the anchored parts' tree was rebuilt whole
        std::size_t awakeAssemblies = 0; // simulated in the last step
        std::size_t sleepingAssemblies = 0;
        std::size_t contacts = 0;          // in the last step
        std::size_t constraints = 0;       // simulated Constraint instances
        std::size_t activeConstraints = 0; // of those, solved in the last step
        std::size_t rows = 0;              // their solver rows in the last step
        std::size_t islands = 0;           // bodies solved apart from each other in the last step
        std::size_t steps = 0;             // steps taken
        std::size_t parallelSteps = 0;     // of those, spread over more than one thread
        double stepSeconds = 0.0;          // spent in them
    };
    const Stats& stats() const { return stats_; }

    // Which unanchored parts this world moves. An assembly none of whose
    // parts pass the filter stays where its CFrames put it and collides like
    // an anchored one, whatever moves it (null, the default: every
    // unanchored assembly simulates). A network client simulates only what
    // it owns, its character and what the server hands it: the server and
    // the other players' clients move the rest, which the client only
    // follows.
    void setSimulationFilter(std::function<bool(const BasePart&)> simulates);
    // Asks the filter about this part's assembly again at the next update
    // (what the filter says of it changed: the server gave us the part).
    void refilter(const BasePart& part);

    // Threads a step may spread over, the caller's included: see
    // ThreadPool::defaultThreads. Only steps with enough to do are spread,
    // so a small world stays on the caller. Results never depend on either.
    unsigned threads() const { return threads_; }
    void setThreads(unsigned threads);
    // Spread every step however little it holds (for tests).
    void setAlwaysParallel(bool always) { alwaysParallel_ = always; }

private:
    // Where a collider is found by queries: the tree of anchored parts, the
    // small tree of anchored parts added since it was built, or the moving ones.
    enum class Home : std::uint8_t { None, StaticTree, StaticExtra, Dynamic };
    struct Collider {
        BasePart* part = nullptr; // null: a free slot
        Shape shape;
        Aabb box;
        Assembly* assembly = nullptr;
        std::vector<Instance*> joints; // linked joints this part is in
        Home home = Home::None;
    };
    // A JointInstance or WeldConstraint in the workspace.
    struct Joint {
        BasePart* part0 = nullptr; // as last read
        BasePart* part1 = nullptr;
        Connection connection;
        bool linked = false; // joins two colliders: slot0 and slot1
        std::uint32_t slot0 = 0;
        std::uint32_t slot1 = 0;
    };
    struct TerrainTriangle {
        Vector3 a, b, c;
    };
    struct TerrainBlock {
        std::vector<TerrainTriangle> triangles;
        Bvh bvh;
    };
    struct Contact;
    struct Body;
    struct Island;
    struct CollideBatch;
    struct Row;
    struct Mechanism;
    struct Block;
    static constexpr std::size_t maxBlockRows = 256;
    struct CachedImpulse {
        std::uint64_t key = 0;
        Vector3 position;
        float normal = 0.0f;
        float tangent1 = 0.0f;
        float tangent2 = 0.0f;
    };

    void added(Instance& instance);
    static bool finite(Vector3 v);
    void reportNonFinite(const Assembly& assembly);
    void removing(Instance& instance);
    void partChanged(BasePart& part, std::string_view property);
    void queuePart(BasePart* part);
    void queueJoint(Instance* joint);
    // A joint's C0 or C1 changed: its assembly's parts go where the joint
    // now puts them (placeMovedJoints, in the next update).
    void jointMoved(Instance* joint);
    void placeMovedJoints();
    // Picks up parts and joints that came, went or changed since the last call.
    void update();
    void updateStructure();
    void takeInChanges(); // updateStructure, counted in stats_
    std::uint32_t addCollider(BasePart& part);
    void removeCollider(const BasePart* part);
    void linkJoint(Instance& instance, Joint& joint);
    void unlinkJoint(Instance* instance, Joint& joint);
    void forgetJointParts(Instance* instance, const Joint& joint);
    void markDirty(Assembly* assembly);
    void reassemble();
    std::unique_ptr<Assembly> makeAssembly(std::span<const std::uint32_t> members);
    void rebuildStatic();
    void rebuildSleeping();
    void refitStatic();
    void resetRest(Assembly& assembly);
    void wake(Assembly& assembly);
    void wakeNear(const Aabb& box);
    void updateRest(Assembly& assembly, bool touchingMoving, bool touchingAwake);
    bool moving(const Assembly& assembly) const;
    // Calls fn(slot) for every anchored collider whose box overlaps box.
    template <class Fn>
    void queryStatic(const Aabb& box, Fn fn) const;
    const TerrainBlock& terrainBlock(int x, int y, int z) const;
    // A block's collision triangles, meshed afresh; safe on any thread.
    std::unique_ptr<TerrainBlock> meshTerrainBlock(int x, int y, int z) const;
    // The blocks that can hold terrain surfaces, those within one block of a
    // stored chunk, per axis; first > last on every axis when there are none.
    struct BlockRange {
        std::array<int, 3> first{1, 1, 1};
        std::array<int, 3> last{};
    };
    const BlockRange& terrainRange() const;
    // Calls fn(x, y, z) for every terrain block whose region overlaps box.
    template <class Fn>
    void forTerrainBlocks(const Aabb& box, Fn fn) const;
    void writeBack(Assembly& assembly);
    void placeParts(Assembly& assembly);
    // Where a part of the assembly sits relative to its root, through the
    // joints that hold it there.
    CFrame offsetFromRoot(const Assembly& assembly, const BasePart& part) const;
    bool groupsCollide(const BasePart& a, const BasePart& b) const;
    void collideAll(float dt);
    void collideBatch(CollideBatch& batch, std::size_t first, std::size_t last);
    void solve(float dt);
    void findIslands();
    void solveIsland(const Island& island, float dt);
    void prepareContacts(std::span<const std::uint32_t> contacts, float dt);
    void solveContacts(std::span<const std::uint32_t> contacts, int iteration);
    // Runs fn(i) for i below count on the pool when work (a rough count of
    // pairs or rows) makes it worth it, else on this thread.
    void forEach(std::size_t count, std::size_t work, std::function_ref<void(std::size_t)> fn);
    bool passesFilter(const BasePart& part, const RaycastParams& params) const;
    std::uint32_t bodyOf(Assembly* assembly);

    // Mechanical constraints (Constraints.cpp).
    void mechanismAdded(Instance& instance);
    void mechanismRemoving(Instance& instance);
    // Which constraints act this step, waking what they join.
    void resolveMechanisms();
    void resolve(Mechanism& mechanism);
    // Constrained assemblies fall asleep together or not at all.
    void sleepTogether();
    // Their rows for this step, warm started.
    void buildRows(float dt);
    void buildRows(const Mechanism& mechanism, std::uint32_t index, float dt, std::int32_t ia, std::int32_t ib,
                   std::vector<Row>& out) const;
    void prepareRow(Row& row) const;
    float rowVelocity(const Row& row) const;
    void applyRow(Row& row, float impulse);
    void groupBlocks();
    void factorBlock(Block& block);
    void solveBlock(Block& block);
    void solveRows(std::span<const std::uint32_t> rows);
    void storeRows();

    Workspace& workspace_;
    std::function<bool(const BasePart&)> simulates_; // setSimulationFilter
    UnionGeometry unionGeometry_;                    // setUnionGeometry
    std::vector<Connection> connections_;
    std::unordered_map<const BasePart*, Connection> partConnections_;
    bool writing_ = false;
    float accumulator_ = 0.0f;
    std::uint64_t stepCount_ = 0;

    // Changes waiting for update(), in the order they came.
    bool pending_ = false;
    std::vector<BasePart*> pendingParts_;
    std::unordered_set<const BasePart*> pendingPartSet_;
    std::vector<Instance*> pendingJoints_;
    std::unordered_set<const Instance*> pendingJointSet_;
    std::vector<Instance*> movedJoints_;        // whose C0 or C1 changed
    std::vector<Assembly*> dirtyAssemblies_;    // to be rebuilt from their parts
    std::vector<std::uint32_t> looseColliders_; // new, in no assembly yet
    std::vector<Aabb> pendingWakes_;            // sleepers touching these wake
    std::vector<std::uint32_t> movedStatic_;    // anchored colliders whose part moved
    bool staticMoved_ = false;
    bool noCollideDirty_ = false;
    bool extraDirty_ = false;
    bool dynamicDirty_ = false;
    bool sleepingDirty_ = false;

    std::vector<Collider> colliders_; // by slot
    std::vector<std::uint32_t> freeSlots_;
    std::size_t colliderCount_ = 0;
    std::unordered_map<const BasePart*, std::uint32_t> colliderOf_;
    std::unordered_map<Instance*, Joint> joints_;
    std::unordered_map<const BasePart*, std::vector<Instance*>> jointsByPart_; // every joint naming the part
    std::unordered_map<Instance*, Connection> noCollisionConstraints_;
    Bvh staticBvh_;
    std::size_t staticTreeSize_ = 0; // colliders it was built over
    std::size_t deadStatic_ = 0;     // of those, gone or moving since
    std::vector<std::uint32_t> extraStatic_;
    Bvh extraBvh_;
    std::vector<std::uint32_t> dynamicColliders_; // slots, ascending
    std::vector<std::uint32_t> sleepingColliders_;
    Bvh sleepingBvh_;
    std::vector<std::unique_ptr<Assembly>> assemblies_;
    std::vector<Assembly*> dynamicAssemblies_; // the unanchored ones, in assemblies_' order
    MeshGeometryCache meshes_;                 // blob -> pieces, shared by every part with that blob
    std::set<std::pair<const BasePart*, const BasePart*>> noCollide_; // NoCollisionConstraint pairs, both ways
    std::vector<std::unique_ptr<Mechanism>> mechanisms_; // in the order they came, for a deterministic solve
    // Assemblies (by their root) whose motion went non-finite, told once each.
    std::vector<const Instance*> nonFiniteReported_;
    std::unordered_map<const Instance*, Mechanism*> mechanismOf_;
    std::vector<Mechanism*> activeMechanisms_; // this step's
    std::uint64_t structureEpoch_ = 1;         // counts assembly rebuilds: constraints re-resolve after one

    mutable std::unordered_map<std::uint64_t, std::unique_ptr<TerrainBlock>> terrainBlocks_;
    mutable BlockRange terrainRange_;
    mutable const Terrain* terrainRangeOf_ = nullptr; // the terrain and revision it was found for
    mutable std::uint64_t terrainRevision_ = 0;

    // Per-step scratch, kept for its capacity.
    std::vector<Contact> contacts_;
    std::vector<Body> bodies_;
    std::vector<Row> rows_;
    std::vector<std::uint32_t> seats_; // colliders of Seats and VehicleSeats
    std::vector<Block> blocks_;
    std::vector<Island> islands_;
    std::vector<std::uint32_t> islandContacts_; // by island, each island's in their order
    std::vector<std::uint32_t> islandRows_;     // rows not in a block, the same way
    std::vector<std::uint32_t> islandBlocks_;
    std::vector<std::uint32_t> islandOrder_; // biggest first, for the pool
    std::vector<CollideBatch> collideBatches_;
    std::vector<std::size_t> batchStarts_;                     // where each batch's run of awake_ starts, then the end
    std::vector<std::array<std::int32_t, 2>> mechanismBodies_; // each active mechanism's two bodies, or -1
    std::vector<std::vector<Row>> rowBatches_;                 // their rows, a run of mechanisms each
    std::vector<std::vector<std::uint32_t>> droppedRows_;      // per batch, mechanisms with non-finite rows left out
    std::vector<std::uint32_t> awake_;                         // colliders tested this step
    std::vector<Aabb> awakeBoxes_;
    Bvh awakeBvh_;
    std::vector<CachedImpulse> warmStart_; // last step's impulses, sorted by key
    std::vector<CachedImpulse> nextWarmStart_;
    Stats stats_;

    unsigned threads_ = 1;
    bool alwaysParallel_ = false;
    bool parallelStep_ = false;        // this step used the pool
    std::unique_ptr<ThreadPool> pool_; // started the first time a step is worth spreading
};

} // namespace lunar::physics
