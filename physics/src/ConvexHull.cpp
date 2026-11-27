#include <lunar/physics/Geometry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::physics {

namespace {

constexpr std::size_t none = static_cast<std::size_t>(-1);

// Double precision while building, so nearly coplanar points sort out the
// same way however the hull grows.
struct Vec {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec() = default;
    Vec(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit Vec(Vector3 v) : x(v.x), y(v.y), z(v.z) {}

    friend Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec operator*(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    double dot(Vec o) const { return x * o.x + y * o.y + z * o.z; }
    Vec cross(Vec o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(dot(*this)); }
    Vec unit() const
    {
        const double l = length();
        return l > 0.0 ? *this * (1.0 / l) : Vec{};
    }
    Vector3 toFloat() const { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }
};

struct Tri {
    std::array<std::size_t, 3> v{};
    Vec normal;
    double offset = 0.0;
    bool alive = true;
    std::vector<std::size_t> outside;

    double distance(Vec p) const { return normal.dot(p) - offset; }
};

Tri makeTri(const std::vector<Vec>& p, std::size_t a, std::size_t b, std::size_t c)
{
    Tri t;
    t.v = {a, b, c};
    t.normal = (p[b] - p[a]).cross(p[c] - p[a]).unit();
    t.offset = t.normal.dot(p[a]);
    return t;
}

// Orders the points of one flat face counter-clockwise seen along normal.
void sortAround(std::vector<std::size_t>& ring, const std::vector<Vec>& p, Vec normal)
{
    Vec center;
    for (std::size_t i : ring)
        center = center + p[i];
    center = center * (1.0 / static_cast<double>(ring.size()));
    Vec u = (p[ring[0]] - center).unit();
    if (u.length() == 0.0)
        u = std::abs(normal.x) < 0.9 ? normal.cross({1.0, 0.0, 0.0}).unit() : normal.cross({0.0, 1.0, 0.0}).unit();
    const Vec w = normal.cross(u);
    std::ranges::sort(ring, {}, [&](std::size_t i) {
        const Vec d = p[i] - center;
        return std::atan2(d.dot(w), d.dot(u));
    });
}

// Points that all lie in one plane: their polygon as a thin slab, so it has
// side faces for rays and contacts like any other piece.
Polyhedron flatHull(const std::vector<Vec>& p, Vec normal, double eps)
{
    // Monotone chain in the plane's own coordinates.
    const Vec u = (std::abs(normal.x) < 0.9 ? normal.cross({1.0, 0.0, 0.0}) : normal.cross({0.0, 1.0, 0.0})).unit();
    const Vec w = normal.cross(u);
    std::vector<std::size_t> order(p.size());
    std::ranges::iota(order, std::size_t{0});
    auto key = [&](std::size_t i) { return std::pair(p[i].dot(u), p[i].dot(w)); };
    std::ranges::sort(order, {}, key);
    auto turn = [&](std::size_t a, std::size_t b, std::size_t c) {
        const auto [ax, ay] = key(a);
        const auto [bx, by] = key(b);
        const auto [cx, cy] = key(c);
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    std::vector<std::size_t> ring;
    for (int pass = 0; pass < 2; ++pass) {
        const std::size_t start = ring.size();
        for (std::size_t i : order) {
            while (ring.size() >= start + 2 && turn(ring[ring.size() - 2], ring.back(), i) <= eps * eps)
                ring.pop_back();
            ring.push_back(i);
        }
        ring.pop_back();
        std::ranges::reverse(order);
    }
    if (ring.size() < 3)
        return {};
    // The chain runs counter-clockwise around normal (u, w, normal is right-handed).
    const double half = std::max(eps * 10.0, 1e-6);
    const auto count = static_cast<std::uint16_t>(ring.size());
    std::vector<Vector3> vertices;
    for (double side : {half, -half})
        for (std::size_t i : ring)
            vertices.push_back((p[i] + normal * side).toFloat());
    std::vector<std::vector<std::uint16_t>> faces;
    std::vector<Vector3> normals;
    std::vector<std::uint16_t> top;
    std::vector<std::uint16_t> bottom;
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto next = static_cast<std::uint16_t>((i + 1) % count);
        top.push_back(i);
        bottom.push_back(static_cast<std::uint16_t>(count + count - 1 - i));
        faces.push_back({i, static_cast<std::uint16_t>(i + count), static_cast<std::uint16_t>(next + count), next});
        normals.push_back((p[ring[next]] - p[ring[i]]).cross(normal).unit().toFloat());
    }
    faces.push_back(std::move(top));
    normals.push_back(normal.toFloat());
    faces.push_back(std::move(bottom));
    normals.push_back((normal * -1.0).toFloat());
    return Polyhedron::fromPlanes(std::move(vertices), std::move(faces), std::move(normals));
}

} // namespace

Polyhedron convexHull(std::span<const Vector3> points)
{
    // Unique points; the tolerance scales with the hull's size.
    std::vector<Vec> p;
    double extent = 0.0;
    for (const Vector3& v : points) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            continue;
        p.emplace_back(v);
        extent = std::max({extent, std::abs(static_cast<double>(v.x)), std::abs(static_cast<double>(v.y)),
                           std::abs(static_cast<double>(v.z))});
    }
    const double eps = std::max(extent, 1e-3) * 1e-6;
    std::ranges::sort(p, {}, [](Vec v) { return std::tuple(v.x, v.y, v.z); });
    const auto duplicates = std::ranges::unique(p, [&](Vec a, Vec b) { return (a - b).length() <= eps; });
    p.erase(duplicates.begin(), duplicates.end());
    if (p.size() < 3)
        return {};
    const std::size_t n = p.size();

    // Starting tetrahedron from extreme points.
    std::size_t i0 = 0;
    std::size_t i1 = none;
    double best = eps;
    for (std::size_t i = 0; i < n; ++i)
        if (double d = (p[i] - p[i0]).length(); d > best) {
            best = d;
            i1 = i;
        }
    if (i1 == none)
        return {};
    std::size_t i2 = none;
    best = eps;
    const Vec axis = (p[i1] - p[i0]).unit();
    for (std::size_t i = 0; i < n; ++i)
        if (double d = (p[i] - p[i0]).cross(axis).length(); d > best) {
            best = d;
            i2 = i;
        }
    if (i2 == none)
        return {}; // all on a line
    const Vec planeNormal = (p[i1] - p[i0]).cross(p[i2] - p[i0]).unit();
    std::size_t i3 = none;
    best = eps;
    for (std::size_t i = 0; i < n; ++i)
        if (double d = std::abs((p[i] - p[i0]).dot(planeNormal)); d > best) {
            best = d;
            i3 = i;
        }
    if (i3 == none)
        return flatHull(p, planeNormal, eps);

    std::vector<Tri> tris;
    // Each directed edge's live triangle, to find a face's neighbours.
    std::unordered_map<std::uint64_t, std::size_t> edgeOwner;
    edgeOwner.reserve(n * 6 + 12);
    auto edgeKey = [](std::size_t a, std::size_t b) { return static_cast<std::uint64_t>(a) << 32 | b; };
    auto addTri = [&](std::size_t a, std::size_t b, std::size_t c) {
        const std::size_t t = tris.size();
        tris.push_back(makeTri(p, a, b, c));
        for (std::size_t e = 0; e < 3; ++e)
            edgeOwner[edgeKey(tris[t].v[e], tris[t].v[(e + 1) % 3])] = t;
        return t;
    };
    if ((p[i3] - p[i0]).dot(planeNormal) > 0.0)
        std::swap(i1, i2); // keep the fourth point behind the first face
    addTri(i0, i1, i2);
    addTri(i0, i3, i1);
    addTri(i1, i3, i2);
    addTri(i2, i3, i0);

    auto assign = [&](std::size_t point, std::span<const std::size_t> candidates) {
        double far = eps;
        std::size_t owner = tris.size();
        for (std::size_t t : candidates)
            if (double d = tris[t].distance(p[point]); d > far) {
                far = d;
                owner = t;
            }
        if (owner < tris.size())
            tris[owner].outside.push_back(point);
    };
    {
        const std::array<std::size_t, 4> first{0, 1, 2, 3};
        for (std::size_t i = 0; i < n; ++i)
            if (i != i0 && i != i1 && i != i2 && i != i3)
                assign(i, first);
    }

    // Reused from one point to the next: faces seen in this round (by
    // round number), the faces the eye sees, their edges sorted, and so on.
    std::vector<std::uint32_t> seenIn;
    std::uint32_t round = 0;
    std::vector<std::size_t> visible;
    std::vector<std::pair<std::size_t, std::size_t>> visibleEdges;
    std::vector<std::size_t> orphans;
    std::vector<std::size_t> created;
    for (std::size_t current = 0; current < tris.size(); ++current) {
        if (!tris[current].alive || tris[current].outside.empty())
            continue;
        // The furthest point in front of this face joins the hull.
        const auto& out = tris[current].outside;
        const std::size_t eye
            = *std::ranges::max_element(out, {}, [&](std::size_t i) { return tris[current].distance(p[i]); });

        // The faces the eye sees, flooded out from this one across shared
        // edges: with nearly coplanar points a face elsewhere on the hull
        // can also test as seen, and taking it too broke the hull into
        // thousands of overlapping triangles. Sorted by index, the order
        // the faces were made in.
        ++round;
        seenIn.resize(tris.size(), 0);
        visible.assign({current});
        seenIn[current] = round;
        for (std::size_t k = 0; k < visible.size(); ++k) {
            const Tri& face = tris[visible[k]];
            for (std::size_t e = 0; e < 3; ++e) {
                auto across = edgeOwner.find(edgeKey(face.v[(e + 1) % 3], face.v[e]));
                if (across == edgeOwner.end() || seenIn[across->second] == round)
                    continue;
                seenIn[across->second] = round;
                if (tris[across->second].alive && tris[across->second].distance(p[eye]) > eps)
                    visible.push_back(across->second);
            }
        }
        std::ranges::sort(visible);
        // Horizon: edges of visible faces whose other side isn't visible.
        visibleEdges.clear();
        for (std::size_t t : visible)
            for (std::size_t e = 0; e < 3; ++e)
                visibleEdges.emplace_back(tris[t].v[e], tris[t].v[(e + 1) % 3]);
        std::ranges::sort(visibleEdges);
        orphans.clear();
        created.clear();
        for (std::size_t t : visible) {
            tris[t].alive = false;
            for (std::size_t e = 0; e < 3; ++e) {
                const std::size_t a = tris[t].v[e];
                const std::size_t b = tris[t].v[(e + 1) % 3];
                if (auto owner = edgeOwner.find(edgeKey(a, b)); owner != edgeOwner.end() && owner->second == t)
                    edgeOwner.erase(owner);
            }
        }
        for (std::size_t t : visible) {
            for (std::size_t e = 0; e < 3; ++e) {
                const std::size_t a = tris[t].v[e];
                const std::size_t b = tris[t].v[(e + 1) % 3];
                if (!std::ranges::binary_search(visibleEdges, std::pair{b, a}))
                    created.push_back(addTri(a, b, eye));
            }
            orphans.insert(orphans.end(), tris[t].outside.begin(), tris[t].outside.end());
            tris[t].outside.clear();
        }
        for (std::size_t point : orphans)
            if (point != eye)
                assign(point, created);
    }

    // Merge coplanar triangles into one face each.
    struct Group {
        Vec normal;
        double offset = 0.0;
        std::vector<std::size_t> points; // sorted and unique once all are in
        Vec weighted;
    };
    std::vector<Group> groups;
    for (const Tri& t : tris) {
        if (!t.alive)
            continue;
        // A sliver through three all but collinear points (rings of a pipe
        // whose heights are off by float noise) has no real plane: its
        // normal is noise and would cut into the hull. Its points belong to
        // the faces on either side.
        const Vec ab = p[t.v[1]] - p[t.v[0]];
        const Vec ac = p[t.v[2]] - p[t.v[0]];
        const double longest = std::max({ab.length(), ac.length(), (p[t.v[2]] - p[t.v[1]]).length()});
        if (ab.cross(ac).length() <= eps * 10.0 * longest)
            continue;
        auto it = std::ranges::find_if(groups, [&](const Group& g) {
            return g.normal.dot(t.normal) > 1.0 - 1e-6 && std::abs(g.offset - t.offset) <= eps * 10.0;
        });
        if (it == groups.end()) {
            groups.push_back({t.normal, t.offset, {}, {}});
            it = groups.end() - 1;
        }
        const double area = (p[t.v[1]] - p[t.v[0]]).cross(p[t.v[2]] - p[t.v[0]]).length();
        it->weighted = it->weighted + t.normal * area;
        it->points.insert(it->points.end(), t.v.begin(), t.v.end());
    }

    std::vector<std::int32_t> remap(n, -1);
    std::vector<Vector3> vertices;
    std::vector<std::vector<std::uint16_t>> faces;
    std::vector<Vector3> normals;
    for (Group& g : groups) {
        const Vec normal = g.weighted.unit().length() > 0.0 ? g.weighted.unit() : g.normal;
        std::ranges::sort(g.points);
        const auto [repeats, end] = std::ranges::unique(g.points);
        g.points.erase(repeats, end);
        std::vector<std::size_t> ring = std::move(g.points);
        sortAround(ring, p, normal);
        std::vector<std::uint16_t> face;
        for (std::size_t i : ring) {
            if (remap[i] < 0) {
                remap[i] = static_cast<std::int32_t>(vertices.size());
                vertices.push_back(p[i].toFloat());
            }
            face.push_back(static_cast<std::uint16_t>(remap[i]));
        }
        faces.push_back(std::move(face));
        normals.push_back(normal.toFloat());
    }
    return Polyhedron::fromPlanes(std::move(vertices), std::move(faces), std::move(normals));
}

} // namespace lunar::physics
