#pragma once

// What World.cpp and Constraints.cpp share inside the solver: the bodies it
// works on, the scalar velocity rows mechanical constraints are made of, and
// the record kept for each simulated Constraint instance.

#include <lunar/physics/World.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace lunar::physics {

// An assembly in the solver: velocities and the mass properties in world
// space, worked out once per step (they hold still until positions integrate).
struct World::Body {
    Assembly* assembly = nullptr;
    Vector3 linear;
    Vector3 angular;
    Vector3 startLinear; // when the step began, before its forces and spring impulses
    Vector3 startAngular;
    Vector3 center;
    Mat3 inverseInertia;
    float inverseMass = 0.0f;
    bool awake = false;  // Awake rather than Sleep Checking
    bool moving = false; // see World::moving
    bool touchingMoving = false;
    bool touchingAwake = false;

    Vector3 velocityAt(Vector3 world) const { return linear + angular.cross(world - center); }
    Vector3 startVelocityAt(Vector3 world) const { return startLinear + startAngular.cross(world - center); }
    void applyImpulse(Vector3 impulse, Vector3 at)
    {
        linear = linear + impulse * inverseMass;
        angular = angular + inverseInertia * (at - center).cross(impulse);
    }
};

// One scalar velocity constraint between bodies a and b (either may be -1:
// the world). Its Jacobian is [-linear, -angularA, linear, angularB]: an
// impulse j pushes b along linear and turns it about angularB, and a the
// other way. The solver drives J v to target with the accumulated impulse
// held within [lower, upper], like a row of the client's PGS solver.
struct World::Row {
    std::int32_t a = -1;
    std::int32_t b = -1;
    Vector3 linear;
    Vector3 angularA;
    Vector3 angularB;
    float target = 0.0f;
    float lower = -std::numeric_limits<float>::infinity();
    float upper = std::numeric_limits<float>::infinity();
    float mass = 0.0f; // 1 / J M^-1 J^T
    float impulse = 0.0f;
    std::uint32_t mechanism = 0; // index into activeMechanisms_
    std::uint8_t slot = 0;       // where its impulse is kept between steps
    std::int32_t block = -1;     // the rigid block it's solved in, or -1 to iterate on its own
};

// Rigid rows solved together (see World::groupBlocks).
struct World::Block {
    std::vector<std::uint32_t> rows;
    // LDL^T of J M^-1 J^T, row major, in double: a light part between heavy
    // ones puts entries 1e7 apart in it.
    std::vector<double> factor;
    std::vector<double> scratch;
};

// Bodies joined by contacts or rows, through anything but the world: no
// impulse crosses from one island to another, so each is solved on its own,
// on whichever thread, and comes out as it would in one sweep over them all.
// Its contacts, free rows and blocks are ranges of World::islandContacts_,
// islandRows_ and islandBlocks_, each in the order they were made.
struct World::Island {
    std::uint32_t firstContact = 0;
    std::uint32_t contacts = 0;
    std::uint32_t firstRow = 0;
    std::uint32_t rows = 0;
    std::uint32_t firstBlock = 0;
    std::uint32_t blocks = 0;
    std::size_t work = 0; // a rough cost, to hand out the big ones first
};

// A Constraint the World simulates, and what it was attached to this step.
struct World::Mechanism {
    static constexpr std::size_t slots = 10;

    Instance* instance = nullptr;
    Connection changed;
    std::array<Attachment*, 2> attachments{};
    std::array<Connection, 2> attachmentChanged;
    bool touched = true;             // changed since its assemblies were last woken for it
    std::uint64_t resolvedEpoch = 0; // World::structureEpoch_ when last resolved
    // Resolved each step. The sides are null for the world: the terrain,
    // anchored parts. Part null: not attached to anything simulated.
    std::array<BasePart*, 2> parts{};
    Assembly* a = nullptr;
    Assembly* b = nullptr;
    std::array<float, slots> impulses{}; // last step's, by row slot, for warm starting
    std::array<bool, slots> used{};      // which slots had a row last step
};

} // namespace lunar::physics
