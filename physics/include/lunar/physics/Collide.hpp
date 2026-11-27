#pragma once

// Narrow phase: contact points between two placed shapes.

#include <lunar/physics/Geometry.hpp>

#include <array>
#include <vector>

namespace lunar::physics {

struct ContactPoint {
    Vector3 position;   // world, halfway between the surfaces
    float depth = 0.0f; // penetration, positive when overlapping
};

struct Manifold {
    Vector3 normal; // world, unit, pointing from A to B
    std::array<ContactPoint, 4> points{};
    int count = 0;
};

// Contacts between a and b placed by their CFrames, one manifold per pair of
// touching convex pieces (meshes are made of many), appended to out. Points
// closer than `margin` count too (with negative depth), so resting contact
// is steady.
void collide(const Shape& a, const CFrame& ca, const Shape& b, const CFrame& cb, std::vector<Manifold>& out,
             float margin = 0.02f);
// Just the deepest manifold; false when they're apart.
bool collide(const Shape& a, const CFrame& ca, const Shape& b, const CFrame& cb, Manifold& out, float margin = 0.02f);

// A shape against one triangle (world space, either side solid), for terrain.
void collideTriangle(const Shape& a, const CFrame& ca, Vector3 t0, Vector3 t1, Vector3 t2, std::vector<Manifold>& out,
                     float margin = 0.02f);
bool collideTriangle(const Shape& a, const CFrame& ca, Vector3 t0, Vector3 t1, Vector3 t2, Manifold& out,
                     float margin = 0.02f);

} // namespace lunar::physics
