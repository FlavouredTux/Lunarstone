#include <lunar/physics/Collide.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace lunar::physics {

namespace {

// Four floats in one SSE register (GCC's vector extension): the projections
// below handle four vertices at a time.
using Float4 = float __attribute__((vector_size(16)));

constexpr Float4 splat(float v)
{
    return Float4{v, v, v, v};
}

// a < b ? a : b and a > b ? a : b per lane, which is exactly what minps and
// maxps do (GCC only picks those for the plain comparisons under fast math).
Float4 lesser(Float4 a, Float4 b)
{
#if defined(__SSE__)
    return _mm_min_ps(a, b);
#else
    return a < b ? a : b;
#endif
}

Float4 greater(Float4 a, Float4 b)
{
#if defined(__SSE__)
    return _mm_max_ps(a, b);
#else
    return a > b ? a : b;
#endif
}

// A polyhedron moved into world space.
struct WorldPoly {
    std::vector<Vector3> vertices;
    struct Face {
        Vector3 normal;
        float offset;
        std::span<const std::uint16_t> indices;
    };
    std::vector<Face> faces;
    std::span<const std::array<std::uint16_t, 2>> edges;
    std::span<const std::array<std::uint16_t, 2>> edgeFaces; // see Polyhedron::edgeFaces
    std::vector<Vector3> edgeDirections;
    Vector3 center;
    Aabb bounds; // around the vertices
    // The vertices again by coordinate, four to an element, the last element
    // padded with copies of the last vertex (which leave every extreme as it is).
    std::vector<Float4> xs, ys, zs;

    // Fills xs, ys and zs from vertices; every place* ends with it.
    void pack()
    {
        const std::size_t n = vertices.size();
        const std::size_t groups = (n + 3) / 4;
        xs.resize(groups);
        ys.resize(groups);
        zs.resize(groups);
        for (std::size_t g = 0; g < groups; ++g)
            for (std::size_t k = 0; k < 4; ++k) {
                const Vector3& v = vertices[std::min(g * 4 + k, n - 1)];
                xs[g][k] = v.x;
                ys[g][k] = v.y;
                zs[g][k] = v.z;
            }
    }

    // The least and the greatest of axis.dot(v) over the vertices. The same
    // sums in the same order as Vector3::dot, so the results are exactly the
    // ones a vertex at a time gives.
    float lowest(Vector3 axis) const
    {
        const Float4 ax = splat(axis.x);
        const Float4 ay = splat(axis.y);
        const Float4 az = splat(axis.z);
        Float4 lo = splat(std::numeric_limits<float>::max());
        for (std::size_t g = 0; g < xs.size(); ++g) {
            const Float4 d = ax * xs[g] + ay * ys[g] + az * zs[g];
            lo = lesser(d, lo);
        }
        return std::min({lo[0], lo[1], lo[2], lo[3]});
    }
    float highest(Vector3 axis) const
    {
        const Float4 ax = splat(axis.x);
        const Float4 ay = splat(axis.y);
        const Float4 az = splat(axis.z);
        Float4 hi = splat(-std::numeric_limits<float>::max());
        for (std::size_t g = 0; g < xs.size(); ++g) {
            const Float4 d = ax * xs[g] + ay * ys[g] + az * zs[g];
            hi = greater(d, hi);
        }
        return std::max({hi[0], hi[1], hi[2], hi[3]});
    }
};

// Placing fills `w` in place: its vectors keep their capacity from one use to the next.
void place(WorldPoly& w, const Polyhedron& poly, const CFrame& cf)
{
    w.vertices.clear();
    w.bounds = {};
    for (const Vector3& v : poly.vertices) {
        w.vertices.push_back(cf.pointToWorldSpace(v));
        w.bounds.expand(w.vertices.back());
    }
    w.faces.clear();
    for (const Polyhedron::Face& f : poly.faces) {
        const Vector3 n = cf.vectorToWorldSpace(f.normal);
        w.faces.push_back({n, f.offset + n.dot(cf.position), f.indices});
    }
    w.edges = poly.edges;
    w.edgeFaces = poly.edgeFaces;
    w.edgeDirections.clear();
    for (const Vector3& e : poly.edgeDirections)
        w.edgeDirections.push_back(cf.vectorToWorldSpace(e));
    w.center = cf.position;
    w.pack();
}

// A mesh piece: scaled per axis, then placed. Normals go through the inverse
// transpose of the scale; the center is the piece's own, since it isn't at
// the part's origin (the edge test orients its axes by the centers).
void place(WorldPoly& w, const Polyhedron& poly, const CFrame& cf, Vector3 scale)
{
    const Vector3 inverse{1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z};
    w.vertices.clear();
    w.bounds = {};
    Vector3 sum;
    for (const Vector3& v : poly.vertices) {
        w.vertices.push_back(cf.pointToWorldSpace(mul(v, scale)));
        sum = sum + w.vertices.back();
        w.bounds.expand(w.vertices.back());
    }
    w.faces.clear();
    for (const Polyhedron::Face& f : poly.faces) {
        const Vector3 n = cf.vectorToWorldSpace(safeUnit(mul(f.normal, inverse)));
        float offset = -std::numeric_limits<float>::max();
        for (std::uint16_t i : f.indices)
            offset = std::max(offset, n.dot(w.vertices[i]));
        w.faces.push_back({n, offset, f.indices});
    }
    w.edges = poly.edges;
    w.edgeFaces = poly.edgeFaces;
    w.edgeDirections.clear();
    for (const Vector3& e : poly.edgeDirections)
        w.edgeDirections.push_back(cf.vectorToWorldSpace(safeUnit(mul(e, scale))));
    w.center = sum / static_cast<float>(std::max<std::size_t>(w.vertices.size(), 1));
    w.pack();
}

// A shape's pieces placed by one CFrame, each placed (and its box found) the
// first time it's asked for. The same shape meets many others in a step (a
// car against the ground's triangles, a mesh's pieces against another's), so
// a few recently placed shapes are kept per thread.
class PlacedShape {
public:
    bool matches(const Shape& shape, const CFrame& cf) const
    {
        return owner_.get() == geometry(shape) && scale_ == shape.scale && size_ == shape.size && cf_ == cf;
    }
    void reset(const Shape& shape, const CFrame& cf)
    {
        // Holding the geometry keeps its address from being reused for another.
        if (shape.kind == ShapeKind::Mesh)
            owner_ = shape.mesh;
        else
            owner_ = shape.polyhedron;
        shape_ = &shape;
        scale_ = shape.scale;
        size_ = shape.size;
        cf_ = cf;
        const std::size_t n = shape.kind == ShapeKind::Mesh ? shape.mesh->pieces.size() : 1;
        if (pieces_.size() < n)
            pieces_.resize(n);
        state_.assign(n, 0);
        boxes_.resize(n);
    }
    void rebind(const Shape& shape) { shape_ = &shape; }
    std::size_t size() const { return state_.size(); }
    // A mesh piece's box, as forEachPiece tests it.
    const Aabb& box(std::size_t i)
    {
        if (!(state_[i] & hasBox)) {
            const Shape& shape = *shape_;
            const Aabb& local = shape.mesh->bounds[i];
            CFrame at = cf_;
            at.position = cf_.pointToWorldSpace(mul(local.center(), shape.scale));
            boxes_[i] = Aabb::around(at, abs(mul(local.extent() * 0.5f, shape.scale))).grown(shape.skin);
            state_[i] |= hasBox;
        }
        return boxes_[i];
    }
    const WorldPoly& piece(std::size_t i)
    {
        if (!(state_[i] & hasPiece)) {
            const Shape& shape = *shape_;
            if (shape.kind == ShapeKind::Mesh)
                place(pieces_[i], shape.mesh->pieces[i], cf_, shape.scale);
            else
                place(pieces_[i], *shape.polyhedron, cf_);
            state_[i] |= hasPiece;
        }
        return pieces_[i];
    }
    std::uint64_t lastUse = 0;

    static const void* geometry(const Shape& shape)
    {
        return shape.kind == ShapeKind::Mesh ? static_cast<const void*>(shape.mesh.get())
                                             : static_cast<const void*>(shape.polyhedron.get());
    }

private:
    static constexpr std::uint8_t hasBox = 1;
    static constexpr std::uint8_t hasPiece = 2;

    std::shared_ptr<const void> owner_;
    const Shape* shape_ = nullptr;
    Vector3 scale_;
    Vector3 size_;
    CFrame cf_;
    std::vector<WorldPoly> pieces_;
    std::vector<Aabb> boxes_;
    std::vector<std::uint8_t> state_;
};

// Placed shapes are kept per thread in sets of four, a set chosen by the
// geometry and where it is: a step meets the same furniture meshes and
// floors over and over, and anchored ones never move, so they stay placed
// from one step to the next. Within a set the least recently used goes, so
// the shape asked for just before stays.
PlacedShape& placed(const Shape& shape, const CFrame& cf)
{
    constexpr std::size_t ways = 4;
    constexpr std::size_t sets = 8;
    thread_local std::array<PlacedShape, ways * sets> cache;
    thread_local std::uint64_t clock = 0;
    std::uint64_t key = reinterpret_cast<std::uintptr_t>(PlacedShape::geometry(shape)) >> 4;
    for (float v : {cf.position.x, cf.position.y, cf.position.z})
        key = (key ^ std::bit_cast<std::uint32_t>(v)) * 0x9E3779B97F4A7C15ull;
    const std::span<PlacedShape> set(cache.data() + ((key >> 32) % sets) * ways, ways);
    PlacedShape* oldest = &set[0];
    for (PlacedShape& entry : set) {
        if (entry.matches(shape, cf)) {
            entry.lastUse = ++clock;
            entry.rebind(shape);
            return entry;
        }
        if (entry.lastUse < oldest->lastUse)
            oldest = &entry;
    }
    oldest->reset(shape, cf);
    oldest->lastUse = ++clock;
    return *oldest;
}

// Calls fn(piece) for each convex piece of a polyhedral shape whose box
// reaches region: the shape itself, or a mesh's pieces near region.
template <class Fn>
void forEachPiece(const Shape& shape, const CFrame& cf, const Aabb& region, Fn fn)
{
    PlacedShape& pieces = placed(shape, cf);
    if (shape.kind != ShapeKind::Mesh) {
        fn(pieces.piece(0));
        return;
    }
    for (std::size_t i = 0; i < pieces.size(); ++i)
        if (pieces.box(i).overlaps(region))
            fn(pieces.piece(i));
}

// The triangle's two faces and three edges, as a flat polyhedron in world
// space: what Polyhedron::fromFaces makes of faces {0, 1, 2} and {0, 2, 1}
// with every edge kept, placed without moving it.
void placeTriangle(WorldPoly& w, Vector3 a, Vector3 b, Vector3 c)
{
    static constexpr std::array<std::uint16_t, 3> front{0, 1, 2};
    static constexpr std::array<std::uint16_t, 3> back{0, 2, 1};
    static constexpr std::array<std::array<std::uint16_t, 2>, 3> edges{{{0, 1}, {1, 2}, {0, 2}}};
    w.vertices.assign({a, b, c});
    w.bounds = {};
    for (const Vector3& v : w.vertices)
        w.bounds.expand(v);
    const Vector3 n = (b - a).cross(c - a).unit();
    const Vector3 m = (c - a).cross(b - a).unit();
    w.faces.assign({{n, n.dot(a), front}, {m, m.dot(a), back}});
    w.edges = edges;
    w.edgeFaces = {};
    w.edgeDirections.clear();
    for (auto [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
        const Vector3 edge = safeUnit(to - from);
        if (edge == Vector3{})
            continue;
        const bool known
            = std::ranges::any_of(w.edgeDirections, [&](Vector3 e) { return std::abs(e.dot(edge)) > 0.9999f; });
        if (!known)
            w.edgeDirections.push_back(edge);
    }
    w.center = (a + b + c) / 3.0f;
    w.pack();
}

void addPoint(Manifold& out, Vector3 position, float depth)
{
    if (std::cmp_less(out.count, out.points.size())) {
        out.points[static_cast<std::size_t>(out.count++)] = {position, depth};
        return;
    }
    // Full: replace the shallowest point if this one is deeper.
    auto shallowest = std::ranges::min_element(out.points, {}, &ContactPoint::depth);
    if (depth > shallowest->depth)
        *shallowest = {position, depth};
}

// Keeps four well spread points out of many: the deepest, the one furthest
// from it, then the two that widen the area most.
void reduce(std::vector<ContactPoint>& points, Manifold& out)
{
    // Clipping along coincident edges repeats corners; a repeat would weigh a corner twice.
    thread_local std::vector<ContactPoint> unique;
    unique.clear();
    for (const ContactPoint& p : points)
        if (std::ranges::none_of(unique, [&](const ContactPoint& q) {
                return (p.position - q.position).dot(p.position - q.position) < 1e-6f;
            }))
            unique.push_back(p);
    points.swap(unique);
    if (points.size() <= 4) {
        for (const ContactPoint& p : points)
            addPoint(out, p.position, p.depth);
        return;
    }
    std::array<ContactPoint, 4> chosen{};
    chosen[0] = (*std::ranges::max_element(points, {}, &ContactPoint::depth));
    auto furthest = [&](auto score) {
        // Every score can be negative (all points on one side), so start from
        // a real point rather than null.
        const ContactPoint* best = &points.front();
        float bestScore = -std::numeric_limits<float>::infinity();
        for (const ContactPoint& p : points) {
            const float s = score(p);
            if (s > bestScore) {
                bestScore = s;
                best = &p;
            }
        }
        return *best;
    };
    chosen[1] = furthest([&](const ContactPoint& p) {
        const Vector3 d = p.position - chosen[0].position;
        return d.dot(d);
    });
    const Vector3 a = chosen[0].position;
    const Vector3 b = chosen[1].position;
    const Vector3 n = out.normal;
    chosen[2] = furthest([&](const ContactPoint& p) { return (b - a).cross(p.position - a).dot(n); });
    chosen[3] = furthest([&](const ContactPoint& p) { return -(b - a).cross(p.position - a).dot(n); });
    for (const ContactPoint& p : chosen)
        addPoint(out, p.position, p.depth);
}

// Clips a polygon to the half space n.p <= d.
void clip(std::vector<Vector3>& polygon, Vector3 n, float d)
{
    thread_local std::vector<Vector3> result;
    result.clear();
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vector3 p = polygon[i];
        const Vector3 q = polygon[(i + 1) % polygon.size()];
        const float dp = n.dot(p) - d;
        const float dq = n.dot(q) - d;
        if (dp <= 0.0f)
            result.push_back(p);
        if ((dp < 0.0f) != (dq < 0.0f) && dp != dq)
            result.push_back(p + (q - p) * (dp / (dp - dq)));
    }
    polygon.swap(result);
}

// Face contact: `ref`'s face `face` against the most opposed face of `inc`.
// normal points out of ref; flip says the manifold normal is reversed (ref is B).
// skin: how much the two surfaces stand out from their polyhedra together.
void faceContact(const WorldPoly& ref, std::size_t face, const WorldPoly& inc, bool flip, float margin, float skin,
                 Manifold& out)
{
    const WorldPoly::Face& rf = ref.faces[face];
    std::size_t incident = 0;
    float mostOpposed = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < inc.faces.size(); ++i) {
        const float d = inc.faces[i].normal.dot(rf.normal);
        if (d < mostOpposed) {
            mostOpposed = d;
            incident = i;
        }
    }
    thread_local std::vector<Vector3> polygon;
    polygon.clear();
    for (std::uint16_t index : inc.faces[incident].indices)
        polygon.push_back(inc.vertices[index]);
    // Side planes along each reference edge.
    const auto& ri = rf.indices;
    for (std::size_t i = 0; i < ri.size() && !polygon.empty(); ++i) {
        const Vector3 p = ref.vertices[ri[i]];
        const Vector3 q = ref.vertices[ri[(i + 1) % ri.size()]];
        const Vector3 side = (q - p).cross(rf.normal);
        const float len = side.magnitude();
        if (len < 1e-9f)
            continue;
        clip(polygon, side / len, (side / len).dot(p));
    }
    out.normal = flip ? -rf.normal : rf.normal;
    thread_local std::vector<ContactPoint> points;
    points.clear();
    for (const Vector3& p : polygon) {
        const float depth = rf.offset - rf.normal.dot(p) + skin;
        if (depth >= -margin)
            points.push_back({p + rf.normal * (depth * 0.5f), depth});
    }
    reduce(points, out);
}

// Closest points between segments p1-q1 and p2-q2.
std::pair<Vector3, Vector3> closestSegments(Vector3 p1, Vector3 q1, Vector3 p2, Vector3 q2)
{
    const Vector3 d1 = q1 - p1;
    const Vector3 d2 = q2 - p2;
    const Vector3 r = p1 - p2;
    const float a = d1.dot(d1);
    const float e = d2.dot(d2);
    const float f = d2.dot(r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f)
        return {p1, p2};
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = d1.dot(r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = d1.dot(d2);
            const float denom = a * e - b * b;
            s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return {p1 + d1 * s, p2 + d2 * t};
}

// The edge of poly running along dir (either way) that reaches furthest along n.
std::pair<Vector3, Vector3> supportEdge(const WorldPoly& poly, Vector3 dir, Vector3 n)
{
    std::pair<Vector3, Vector3> best{poly.vertices[0], poly.vertices[0]};
    float bestDot = -std::numeric_limits<float>::max();
    for (const auto& [i, j] : poly.edges) {
        const Vector3 a = poly.vertices[i];
        const Vector3 b = poly.vertices[j];
        const Vector3 e = b - a;
        const float len = e.magnitude();
        if (len < 1e-9f || std::abs(e.dot(dir)) < 0.999f * len)
            continue;
        const float d = n.dot(a + b);
        if (d > bestDot) {
            bestDot = d;
            best = {a, b};
        }
    }
    return best;
}

// skin: the surfaces stand that far out from the polyhedra, together (mesh
// pieces carry a margin, see meshSkin), so every separation shrinks by it.
bool polyPoly(const WorldPoly& a, const WorldPoly& b, float margin, Manifold& out, float skin = 0.0f)
{
    // Separation along each candidate axis; the largest decides. Any axis
    // that separates ends it, so the side with fewer faces (a terrain
    // triangle's single plane) goes first.
    auto faceAxes = [&](const WorldPoly& from, const WorldPoly& other, float& best, std::size_t& face) {
        for (std::size_t i = 0; i < from.faces.size(); ++i) {
            const float sep = other.lowest(from.faces[i].normal) - from.faces[i].offset - skin;
            if (sep > margin)
                return false;
            if (sep > best) {
                best = sep;
                face = i;
            }
        }
        return true;
    };
    float bestFaceA = -std::numeric_limits<float>::max();
    std::size_t faceA = 0;
    float bestFaceB = -std::numeric_limits<float>::max();
    std::size_t faceB = 0;
    if (b.faces.size() < a.faces.size()) {
        if (!faceAxes(b, a, bestFaceB, faceB) || !faceAxes(a, b, bestFaceA, faceA))
            return false;
    } else if (!faceAxes(a, b, bestFaceA, faceA) || !faceAxes(b, a, bestFaceB, faceB)) {
        return false;
    }
    float bestEdge = -std::numeric_limits<float>::max();
    Vector3 edgeAxis;
    // The winning edges' corners (the Gauss map way), else their directions' indices.
    std::array<Vector3, 4> corners{};
    std::size_t edgeA = 0;
    std::size_t edgeB = 0;
    // Every pair of edge directions projects both polyhedra, four vertices at
    // a time; the Gauss map way only takes a few products per pair of edges,
    // so it wins unless the shapes are small (boxes, wedges).
    const std::size_t directionCost
        = a.edgeDirections.size() * b.edgeDirections.size() * (a.vertices.size() + b.vertices.size());
    const bool gaussMap
        = !a.edgeFaces.empty() && !b.edgeFaces.empty() && a.edges.size() * b.edges.size() * 16 < directionCost;
    if (gaussMap) {
        // An edge pair can only give the least separation if it makes a face
        // of the Minkowski difference: if the arcs between their faces'
        // normals cross on the unit sphere, B's normals turned around
        // (Gregorius, "The Separating Axis Test between Convex Polyhedra").
        // The normal of an arc's great circle is the second normal crossed
        // with the first: along the edge, against the way the first face
        // walks it (counter-clockwise from outside). The edge itself gives
        // it far more precisely than the cross of two nearly equal normals.
        struct Arc {
            Vector3 from, to, across;
        };
        thread_local std::vector<Arc> arcsB;
        arcsB.clear();
        for (std::size_t j = 0; j < b.edges.size(); ++j) {
            const auto [f, g] = b.edgeFaces[j];
            // Turning both normals around leaves their cross as it was.
            arcsB.push_back(
                {-b.faces[f].normal, -b.faces[g].normal, b.vertices[b.edges[j][0]] - b.vertices[b.edges[j][1]]});
        }
        for (std::size_t i = 0; i < a.edges.size(); ++i) {
            const Vector3 na = a.faces[a.edgeFaces[i][0]].normal;
            const Vector3 nb = a.faces[a.edgeFaces[i][1]].normal;
            const Vector3 a0 = a.vertices[a.edges[i][0]];
            const Vector3 a1 = a.vertices[a.edges[i][1]];
            const Vector3 ea = a1 - a0;
            const Vector3 bxa = -ea;
            // Out of A at this edge: between its two faces.
            const Vector3 outward = na + nb;
            for (std::size_t j = 0; j < arcsB.size(); ++j) {
                const Arc& arc = arcsB[j];
                const float cba = arc.from.dot(bxa);
                const float dba = arc.to.dot(bxa);
                const float adc = na.dot(arc.across);
                const float bdc = nb.dot(arc.across);
                if (cba * dba >= 0.0f || adc * bdc >= 0.0f || cba * bdc <= 0.0f)
                    continue;
                const Vector3 b0 = b.vertices[b.edges[j][0]];
                const Vector3 b1 = b.vertices[b.edges[j][1]];
                const Vector3 eb = b1 - b0;
                Vector3 n = ea.cross(eb);
                const float len = n.magnitude();
                if (len < 1e-4f * std::sqrt(ea.dot(ea) * eb.dot(eb)))
                    continue;
                n = n / len;
                if (n.dot(outward) < 0.0f)
                    n = -n;
                // Both edges are their polyhedra's extremes along n.
                const float sep = n.dot(b0 - a0) - skin;
                if (sep > margin)
                    return false;
                if (sep > bestEdge) {
                    bestEdge = sep;
                    edgeAxis = n;
                    corners = {a0, a1, b0, b1};
                }
            }
        }
    } else {
        const Vector3 between = b.center - a.center;
        for (std::size_t i = 0; i < a.edgeDirections.size(); ++i)
            for (std::size_t j = 0; j < b.edgeDirections.size(); ++j) {
                Vector3 n = a.edgeDirections[i].cross(b.edgeDirections[j]);
                const float len = n.magnitude();
                if (len < 1e-4f)
                    continue;
                n = n / len;
                if (n.dot(between) < 0.0f)
                    n = -n;
                const float sep = b.lowest(n) - a.highest(n) - skin;
                if (sep > margin)
                    return false;
                if (sep > bestEdge) {
                    bestEdge = sep;
                    edgeAxis = n;
                    edgeA = i;
                    edgeB = j;
                }
            }
    }

    // Prefer faces: they give whole manifolds, and edges only win clearly.
    constexpr float relTol = 0.95f;
    constexpr float absTol = 0.01f;
    const float bestFace = std::max(bestFaceA, bestFaceB);
    if (bestEdge > relTol * bestFace + absTol) {
        if (!gaussMap) {
            const auto [a0, a1] = supportEdge(a, a.edgeDirections[edgeA], edgeAxis);
            const auto [b0, b1] = supportEdge(b, b.edgeDirections[edgeB], -edgeAxis);
            corners = {a0, a1, b0, b1};
        }
        const auto [pa, pb] = closestSegments(corners[0], corners[1], corners[2], corners[3]);
        out.normal = edgeAxis;
        out.count = 0;
        addPoint(out, (pa + pb) * 0.5f, -bestEdge);
        return true;
    }
    out.count = 0;
    if (bestFaceB > relTol * bestFaceA + absTol)
        faceContact(b, faceB, a, true, margin, skin, out);
    else
        faceContact(a, faceA, b, false, margin, skin, out);
    return out.count > 0;
}

// The point of poly closest to p (p outside), by checking every face in front of it.
Vector3 closestOnPoly(const WorldPoly& poly, Vector3 p)
{
    Vector3 best = p;
    float bestDist = std::numeric_limits<float>::max();
    for (const WorldPoly::Face& f : poly.faces) {
        const float d = f.normal.dot(p) - f.offset;
        if (d < 0.0f)
            continue;
        Vector3 q = p - f.normal * d;
        // Inside the face polygon?
        bool inside = true;
        const auto& ix = f.indices;
        for (std::size_t i = 0; i < ix.size(); ++i) {
            const Vector3 a = poly.vertices[ix[i]];
            const Vector3 b = poly.vertices[ix[(i + 1) % ix.size()]];
            if ((b - a).cross(q - a).dot(f.normal) < 0.0f) {
                inside = false;
                break;
            }
        }
        if (!inside) {
            float edgeBest = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < ix.size(); ++i) {
                const Vector3 a = poly.vertices[ix[i]];
                const Vector3 b = poly.vertices[ix[(i + 1) % ix.size()]];
                const Vector3 ab = b - a;
                const float t = std::clamp((p - a).dot(ab) / std::max(ab.dot(ab), 1e-12f), 0.0f, 1.0f);
                const Vector3 c = a + ab * t;
                const float dist = (p - c).dot(p - c);
                if (dist < edgeBest) {
                    edgeBest = dist;
                    q = c;
                }
            }
        }
        const float dist = (p - q).dot(p - q);
        if (dist < bestDist) {
            bestDist = dist;
            best = q;
        }
    }
    return best;
}

// Sphere (center c, radius r) against a polyhedron; normal from the polyhedron to the sphere.
bool spherePoly(Vector3 c, float r, const WorldPoly& poly, float margin, Vector3& normal, Vector3& point, float& depth)
{
    float maxDist = -std::numeric_limits<float>::max();
    std::size_t maxFace = 0;
    for (std::size_t i = 0; i < poly.faces.size(); ++i) {
        const float d = poly.faces[i].normal.dot(c) - poly.faces[i].offset;
        if (d > maxDist) {
            maxDist = d;
            maxFace = i;
        }
    }
    if (maxDist > r + margin)
        return false;
    if (maxDist <= 0.0f) {
        normal = poly.faces[maxFace].normal;
        depth = r - maxDist;
        point = c - normal * (maxDist + depth * 0.5f);
        return true;
    }
    const Vector3 q = closestOnPoly(poly, c);
    const Vector3 d = c - q;
    const float dist = d.magnitude();
    if (dist > r + margin || dist < 1e-9f)
        return false;
    normal = d / dist;
    depth = r - dist;
    point = (q + (c - normal * r)) * 0.5f;
    return true;
}

} // namespace

namespace {

Aabb worldBounds(const Shape& shape, const CFrame& cf)
{
    return Aabb::around(cf, shape.boundsHalfExtents());
}

} // namespace

void collide(const Shape& a, const CFrame& ca, const Shape& b, const CFrame& cb, std::vector<Manifold>& out,
             float margin)
{
    const bool ballA = a.kind == ShapeKind::Ball;
    const bool ballB = b.kind == ShapeKind::Ball;
    Manifold m;
    if (ballA && ballB) {
        const Vector3 d = cb.position - ca.position;
        const float dist = d.magnitude();
        const float sum = a.radius + b.radius;
        if (dist > sum + margin)
            return;
        m.normal = dist > 1e-9f ? d / dist : Vector3{0.0f, 1.0f, 0.0f};
        const Vector3 pa = ca.position + m.normal * a.radius;
        const Vector3 pb = cb.position - m.normal * b.radius;
        addPoint(m, (pa + pb) * 0.5f, sum - dist);
        out.push_back(m);
        return;
    }
    if (ballA || ballB) {
        const Shape& ball = ballA ? a : b;
        const CFrame& ballCf = ballA ? ca : cb;
        const Shape& other = ballA ? b : a;
        const CFrame& otherCf = ballA ? cb : ca;
        forEachPiece(other, otherCf, worldBounds(ball, ballCf).grown(margin), [&](const WorldPoly& poly) {
            Vector3 normal;
            Vector3 point;
            float depth = 0.0f;
            if (!spherePoly(ballCf.position, ball.radius + other.skin, poly, margin, normal, point, depth))
                return;
            // normal points from the polyhedron to the ball.
            Manifold piece;
            piece.normal = ballA ? -normal : normal;
            addPoint(piece, point, depth);
            out.push_back(piece);
        });
        return;
    }
    const Aabb boxB = worldBounds(b, cb).grown(margin);
    forEachPiece(a, ca, boxB, [&](const WorldPoly& pa) {
        // Only b's pieces near this piece of a.
        const Aabb near = pa.bounds.grown(a.skin);
        if (!near.overlaps(boxB))
            return;
        forEachPiece(b, cb, near.grown(margin), [&](const WorldPoly& pb) {
            if (polyPoly(pa, pb, margin, m, a.skin + b.skin))
                out.push_back(m);
        });
    });
}

bool collide(const Shape& a, const CFrame& ca, const Shape& b, const CFrame& cb, Manifold& out, float margin)
{
    std::vector<Manifold> all;
    collide(a, ca, b, cb, all, margin);
    out.count = 0;
    if (all.empty())
        return false;
    // One manifold: the deepest contact's.
    out = *std::ranges::max_element(all, {}, [](const Manifold& m) {
        float deepest = -std::numeric_limits<float>::max();
        for (int i = 0; i < m.count; ++i)
            deepest = std::max(deepest, m.points[static_cast<std::size_t>(i)].depth);
        return deepest;
    });
    return true;
}

void collideTriangle(const Shape& a, const CFrame& ca, Vector3 t0, Vector3 t1, Vector3 t2, std::vector<Manifold>& out,
                     float margin)
{
    thread_local WorldPoly b;
    placeTriangle(b, t0, t1, t2);
    const Aabb triBox = b.bounds;
    if (a.kind == ShapeKind::Ball) {
        Vector3 normal;
        Vector3 point;
        float depth = 0.0f;
        if (!spherePoly(ca.position, a.radius, b, margin, normal, point, depth))
            return;
        Manifold m;
        m.normal = -normal;
        addPoint(m, point, depth);
        out.push_back(m);
        return;
    }
    forEachPiece(a, ca, triBox.grown(margin), [&](const WorldPoly& pa) {
        Manifold m;
        if (polyPoly(pa, b, margin, m, a.skin))
            out.push_back(m);
    });
}

bool collideTriangle(const Shape& a, const CFrame& ca, Vector3 t0, Vector3 t1, Vector3 t2, Manifold& out, float margin)
{
    std::vector<Manifold> all;
    collideTriangle(a, ca, t0, t1, t2, all, margin);
    out.count = 0;
    if (all.empty())
        return false;
    out = all.front();
    return true;
}

} // namespace lunar::physics
