#pragma once

// Collision geometry: bounding boxes, convex shapes in part space and exact
// ray tests against them.

#include <lunar/Classes.hpp>
#include <lunar/Math.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar::physics {

constexpr Vector3 min(Vector3 a, Vector3 b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
constexpr Vector3 max(Vector3 a, Vector3 b)
{
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
constexpr Vector3 mul(Vector3 a, Vector3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline Vector3 abs(Vector3 v)
{
    return {std::abs(v.x), std::abs(v.y), std::abs(v.z)};
}
// The unit vector, or zero for (nearly) zero vectors.
inline Vector3 safeUnit(Vector3 v)
{
    const float m = v.magnitude();
    return m > 1e-12f ? v / m : Vector3{};
}

struct Aabb {
    Vector3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
    Vector3 max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max()};

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    Vector3 center() const { return (min + max) * 0.5f; }
    Vector3 extent() const { return max - min; }
    float surfaceArea() const
    {
        const Vector3 e = extent();
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }
    void expand(Vector3 p)
    {
        min = physics::min(min, p);
        max = physics::max(max, p);
    }
    void expand(const Aabb& o)
    {
        min = physics::min(min, o.min);
        max = physics::max(max, o.max);
    }
    Aabb grown(float margin) const
    {
        return {min - Vector3{margin, margin, margin}, max + Vector3{margin, margin, margin}};
    }
    bool overlaps(const Aabb& o) const
    {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y && min.z <= o.max.z
               && max.z >= o.min.z;
    }
    bool contains(Vector3 p) const
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }

    // Box around a local box of half size `half` placed by cf.
    static Aabb around(const CFrame& cf, Vector3 half);
};

// Slab test: the entry fraction along origin + t * direction (t in [0, maxT]), if the ray hits.
std::optional<float> rayAabb(const Aabb& box, Vector3 origin, Vector3 inverseDirection, float maxT);

// A convex polyhedron. Faces wind counter-clockwise seen from outside.
struct Polyhedron {
    struct Face {
        Vector3 normal;      // outward, unit
        float offset = 0.0f; // normal.dot(p) == offset on the face
        std::vector<std::uint16_t> indices;
    };

    std::vector<Vector3> vertices;
    std::vector<Face> faces;
    std::vector<std::array<std::uint16_t, 2>> edges; // each once
    std::vector<Vector3> edgeDirections;             // unique (up to sign), unit
    // The faces each edge borders, in the order of edges: the one walking it
    // upwards first. Only set when every edge borders two faces that aren't
    // back to back (not for a flat, two-sided polygon); collision then skips
    // the edge pairs whose Gauss map arcs don't cross.
    std::vector<std::array<std::uint16_t, 2>> edgeFaces;

    // Builds faces' planes and the edge list from vertices and face indices.
    static Polyhedron fromFaces(std::vector<Vector3> vertices, std::vector<std::vector<std::uint16_t>> faces);
    // Same with each face's outward normal given (faces whose first three
    // corners are in a line can't work theirs out).
    static Polyhedron fromPlanes(std::vector<Vector3> vertices, std::vector<std::vector<std::uint16_t>> faces,
                                 std::vector<Vector3> normals);

    bool empty() const { return vertices.empty(); }

    // The vertex furthest along dir.
    Vector3 support(Vector3 dir) const;
};

// The convex hull of points: merged coplanar faces, only the corners kept.
// Points in one plane give a two-sided polygon; fewer than that, nothing.
Polyhedron convexHull(std::span<const Vector3> points);

// A MeshPart's or union's collision geometry, from its CSGPHS blob: the
// convex hull of each stored piece, centered on the box around all of them
// and unscaled. Parts scale it onto their Size (see meshScale), so one
// geometry serves every part that shares the blob.
struct MeshGeometry {
    std::vector<Polyhedron> pieces;
    std::vector<Aabb> bounds; // each piece's, same space
    Vector3 extent;           // size of the box around every piece
    float volume = 0.0f;      // unscaled: the stored volume, else the pieces'
};

// The collision margin of mesh pieces. The client makes each piece a
// btConvexHullShape with margin 0.05 (sub_1033b74a0 passes the constant at
// 0x1064b8260 to its constructor, sub_103398d14) and collides convex pairs
// with Bullet's GJK (btConvexConvexAlgorithm from sub_1033ad1d8,
// btGjkPairDetector reading both margins in sub_1033b120a), which puts the
// surface that far out from the points. Its own shapes get the same 0.05
// margin (BulletConvexShape::getMargin) but are shrunk by it first
// (sub_10339a21e), so only mesh pieces end up bigger. Rays use the bare pieces.
constexpr float meshSkin = 0.05f;

// Scale from a mesh geometry to a part: Size / extent per axis, or 1 where
// the extent is (nearly) zero. TriangleMesh's scale, sub_1033997f2.
Vector3 meshScale(Vector3 size, Vector3 extent);

// What a part collides as. Roblox's own shapes are exact, except cylinders,
// which are prisms for contacts (rays hit the round surface). MeshParts and
// unions collide as their stored convex pieces, or as their box when the
// file says BLOCK or has no usable data.
enum class ShapeKind : std::uint8_t { Box, Wedge, CornerWedge, Cylinder, Ball, Mesh };

struct Shape {
    ShapeKind kind = ShapeKind::Box;
    Vector3 size{1.0f, 1.0f, 1.0f}; // the part's Size
    float radius = 0.5f;            // Ball, and Cylinder's round section
    // Every kind but Ball and Mesh has its polyhedral form here (cylinders as prisms).
    std::shared_ptr<const Polyhedron> polyhedron;
    // Mesh: shared pieces, and their scale onto this part.
    std::shared_ptr<const MeshGeometry> mesh;
    Vector3 scale{1.0f, 1.0f, 1.0f};
    // How far contacts reach out from the polyhedra (meshSkin for meshes).
    float skin = 0.0f;

    Vector3 halfExtents() const { return size * 0.5f; }
    // Half size of the box everything that can touch the shape lies in.
    Vector3 boundsHalfExtents() const { return size * 0.5f + Vector3{skin, skin, skin}; }
    float volume() const;
    // The point furthest along dir, in part space.
    Vector3 support(Vector3 dir) const;
};

// Segments around a cylinder's prism.
constexpr int cylinderSegments = 16;

Shape makeShape(ShapeKind kind, Vector3 size);
Shape makeMeshShape(std::shared_ptr<const MeshGeometry> mesh, Vector3 size);

// Decoded and hulled collision blobs, kept per blob so the thousands of
// parts that share one (trees, walls) share the geometry too.
class MeshGeometryCache {
public:
    // The geometry a MeshPart or union collides as, or null for its box.
    // error, when given, says why a blob that should have had geometry fell
    // back to the box (as the client warns "Invalid mesh collision data").
    std::shared_ptr<const MeshGeometry> get(const SharedBytes& blob, std::string* error = nullptr);

    // Builds the geometry of the blobs it hasn't seen, so get finds them
    // ready: run(count, fn) calls fn(i) for every i below count, on whatever
    // threads it likes (a place's thousands of meshes load side by side).
    using Runner = std::function_ref<void(std::size_t count, std::function_ref<void(std::size_t)> fn)>;
    void prepare(std::span<const SharedBytes> blobs, Runner run);

    std::size_t size() const { return byContent_.size(); }

private:
    struct Entry {
        SharedBytes blob; // keeps the key's bytes alive
        std::shared_ptr<const MeshGeometry> geometry;
        std::string error;
    };
    struct Seen {
        SharedBytes blob; // held so its address can't be reused for other bytes
        std::shared_ptr<const Entry> entry;
    };
    std::unordered_map<const std::string*, Seen> byAddress_;
    std::unordered_map<std::string_view, std::shared_ptr<const Entry>> byContent_;
};

// Builds a blob's geometry without caching; null for the box.
std::shared_ptr<const MeshGeometry> meshGeometry(std::string_view blob, std::string* error = nullptr);

// A primitive at a place in a mesh's own space: what a union was made of.
struct GeometryPiece {
    ShapeKind kind = ShapeKind::Box; // Box, Wedge, CornerWedge or Cylinder
    CFrame cframe;
    Vector3 size;
};

// Geometry from primitives, centered on the box around them like a blob's:
// a union collides as the parts it was made from when its own collision
// data is only a hash. Null without pieces or with a kind it can't hull (a
// Ball, a Mesh).
std::shared_ptr<const MeshGeometry> piecesGeometry(std::span<const GeometryPiece> pieces);

// The shape Roblox gives the part: Part.Shape, WedgePart, CornerWedgePart,
// and for MeshParts and unions their PhysicalConfigData (TriangleMeshPart's
// choice in sub_101153352: no data or BLOCK is the Size box, so is data the
// client can't use). The cache, when given, shares mesh geometry.
Shape shapeOf(const BasePart& part, MeshGeometryCache* cache = nullptr);

struct LocalHit {
    float t = 0.0f; // fraction of the ray
    Vector3 normal; // part space
};

// Ray in part space, origin + t * direction for t in [0, 1]. Rays starting
// inside a shape don't hit it, as with Workspace:Raycast.
std::optional<LocalHit> raycast(const Shape& shape, Vector3 origin, Vector3 direction);

// Möller-Trumbore, both sides; t in [0, 1] along direction.
std::optional<float> rayTriangle(Vector3 origin, Vector3 direction, Vector3 a, Vector3 b, Vector3 c);

} // namespace lunar::physics
