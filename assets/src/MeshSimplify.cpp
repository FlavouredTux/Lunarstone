#include <lunar/assets/MeshSimplify.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lunar::assets {

namespace {

// A symmetric 4x4 error quadric (Garland and Heckbert) and the area it was
// summed over: error(p) is the mean squared distance from p to the planes.
struct Quadric {
    double xx = 0.0, xy = 0.0, xz = 0.0, xw = 0.0;
    double yy = 0.0, yz = 0.0, yw = 0.0;
    double zz = 0.0, zw = 0.0;
    double ww = 0.0;
    double weight = 0.0;

    // The plane a x + b y + c z + d = 0 (unit normal), weighted.
    void addPlane(double a, double b, double c, double d, double w)
    {
        xx += w * a * a;
        xy += w * a * b;
        xz += w * a * c;
        xw += w * a * d;
        yy += w * b * b;
        yz += w * b * c;
        yw += w * b * d;
        zz += w * c * c;
        zw += w * c * d;
        ww += w * d * d;
        weight += w;
    }

    Quadric& operator+=(const Quadric& o)
    {
        xx += o.xx;
        xy += o.xy;
        xz += o.xz;
        xw += o.xw;
        yy += o.yy;
        yz += o.yz;
        yw += o.yw;
        zz += o.zz;
        zw += o.zw;
        ww += o.ww;
        weight += o.weight;
        return *this;
    }

    double error(Vector3 p) const
    {
        const double x = p.x;
        const double y = p.y;
        const double z = p.z;
        const double e = xx * x * x + 2.0 * xy * x * y + 2.0 * xz * x * z + 2.0 * xw * x + yy * y * y + 2.0 * yz * y * z
                         + 2.0 * yw * y + zz * z * z + 2.0 * zw * z + ww;
        return weight > 0.0 ? std::max(e, 0.0) / weight : 0.0;
    }
};

// Moving a border vertex along its border costs this much more than moving
// it within its faces' planes, so outlines hold. GUESS: a common choice.
constexpr double borderWeight = 10.0;

enum class Kind : std::uint8_t {
    Free,   // inside the surface with one set of attributes: may go to any neighbour
    Border, // on an open edge with one border edge each way: may slide along it
    Seam,   // where two sets of attributes (UV, colour) meet along a line: may slide along it
    Locked, // a seam or border corner, a non-manifold edge, or gone: stays
};

// A place, or a place and the attributes a vertex has there (UV and colour;
// normals may split freely, flat-shaded meshes split them at every corner).
struct WeldKey {
    std::array<std::uint32_t, 8> bits{};
    bool operator==(const WeldKey&) const = default;
};

struct WeldHash {
    std::size_t operator()(const WeldKey& k) const
    {
        std::uint64_t h = 0x9e3779b97f4a7c15ull;
        for (std::uint32_t b : k.bits)
            h = (h ^ b) * 0xbf58476d1ce4e5b9ull + (h >> 29);
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

std::uint32_t bits(float value)
{
    return std::bit_cast<std::uint32_t>(value + 0.0f); // +0 and -0 are the same
}

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b)
{
    return a < b ? (std::uint64_t{a} << 32) | b : (std::uint64_t{b} << 32) | a;
}

Vector3 cross(Vector3 a, Vector3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

// What the faces on one edge (between welded places) have at its ends.
struct EdgeInfo {
    std::uint32_t count = 0;                           // faces on it
    std::array<std::uint32_t, 2> lowWedge{none, none}; // their wedges at the lower-numbered end
    std::array<std::uint32_t, 2> highWedge{none, none};

    // Whether the faces either side disagree on the attributes at an end.
    bool seamAtLow() const { return count == 2 && lowWedge[0] != lowWedge[1]; }
    bool seamAtHigh() const { return count == 2 && highWedge[0] != highWedge[1]; }
};

class Simplifier {
public:
    explicit Simplifier(const MeshData& mesh) : mesh_(mesh)
    {
        weld();
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const std::array<std::uint32_t, 3> t{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
            if (t[0] >= mesh.vertices.size() || t[1] >= mesh.vertices.size() || t[2] >= mesh.vertices.size())
                continue;
            const std::uint32_t a = position_[t[0]];
            const std::uint32_t b = position_[t[1]];
            const std::uint32_t c = position_[t[2]];
            if (a == b || b == c || a == c)
                continue;
            triangles_.push_back(t);
        }
        alive_.assign(triangles_.size(), 1);
        live_ = triangles_.size();
        classify();
        buildQuadrics();
    }

    std::size_t triangles() const { return live_; }
    double error() const { return std::sqrt(maxError_); }

    // Collapses edges, cheapest first, until at most target triangles are
    // left or nothing more can go.
    void reduce(std::size_t target)
    {
        while (live_ > target) {
            if (!pass(target))
                break;
        }
    }

    void append(std::vector<std::uint32_t>& out) const
    {
        for (std::size_t t = 0; t < triangles_.size(); ++t)
            if (alive_[t] != 0)
                out.insert(out.end(), triangles_[t].begin(), triangles_[t].end());
    }

private:
    struct Candidate {
        double cost;
        std::uint32_t from;
        std::uint32_t to;
    };

    void weld()
    {
        std::unordered_map<WeldKey, std::uint32_t, WeldHash> places;
        std::unordered_map<WeldKey, std::uint32_t, WeldHash> wedges;
        places.reserve(mesh_.vertices.size());
        wedges.reserve(mesh_.vertices.size());
        position_.resize(mesh_.vertices.size());
        wedge_.resize(mesh_.vertices.size());
        for (std::size_t i = 0; i < mesh_.vertices.size(); ++i) {
            const MeshVertex& v = mesh_.vertices[i];
            WeldKey key{{bits(v.position.x), bits(v.position.y), bits(v.position.z)}};
            auto [place, addedPlace] = places.try_emplace(key, static_cast<std::uint32_t>(points_.size()));
            if (addedPlace)
                points_.push_back(v.position);
            position_[i] = place->second;
            key.bits[3] = bits(v.u);
            key.bits[4] = bits(v.v);
            key.bits[5] = bits(v.color.r);
            key.bits[6] = bits(v.color.g);
            key.bits[7] = bits(v.color.b);
            wedge_[i] = wedges.try_emplace(key, static_cast<std::uint32_t>(wedges.size())).first->second;
        }
        // Each place's render vertices, to pick the one a moved corner takes.
        start_.assign(points_.size() + 1, 0);
        for (std::uint32_t p : position_)
            ++start_[p + 1];
        for (std::size_t p = 0; p < points_.size(); ++p)
            start_[p + 1] += start_[p];
        at_.resize(position_.size());
        std::vector<std::uint32_t> fill(start_.begin(), start_.end() - 1);
        for (std::uint32_t v = 0; v < position_.size(); ++v)
            at_[fill[position_[v]]++] = v;
    }

    // Edge face counts and the wedges at their ends, over the live faces.
    std::unordered_map<std::uint64_t, EdgeInfo> edgeInfo() const
    {
        std::unordered_map<std::uint64_t, EdgeInfo> edges;
        edges.reserve(live_ * 3);
        for (std::size_t t = 0; t < triangles_.size(); ++t) {
            if (alive_[t] == 0)
                continue;
            const auto& tri = triangles_[t];
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t va = tri[k];
                const std::uint32_t vb = tri[(k + 1) % 3];
                const std::uint32_t a = position_[va];
                const std::uint32_t b = position_[vb];
                EdgeInfo& e = edges[edgeKey(a, b)];
                if (e.count < 2) {
                    e.lowWedge[e.count] = wedge_[a < b ? va : vb];
                    e.highWedge[e.count] = wedge_[a < b ? vb : va];
                }
                ++e.count;
            }
        }
        return edges;
    }

    // What edgeInfo() has for the edge between `from` and `to`, from the
    // live faces around `from` (in ascending order, as edgeInfo reads them):
    // every face on the edge is one of those. A pass asks this for every
    // neighbour of every place, and a map of every edge cost more to build
    // and look up than these few faces to walk.
    EdgeInfo edgeBetween(std::uint32_t from, std::uint32_t to, std::span<const std::uint32_t> fromTriangles) const
    {
        EdgeInfo e;
        const bool fromLow = from < to;
        for (std::uint32_t t : fromTriangles) {
            const auto& tri = triangles_[t];
            std::uint32_t fromVertex = none;
            std::uint32_t toVertex = none;
            for (std::uint32_t v : tri) {
                const std::uint32_t p = position_[v];
                if (p == from)
                    fromVertex = v;
                else if (p == to)
                    toVertex = v;
            }
            if (toVertex == none)
                continue;
            if (e.count < 2) {
                e.lowWedge[e.count] = wedge_[fromLow ? fromVertex : toVertex];
                e.highWedge[e.count] = wedge_[fromLow ? toVertex : fromVertex];
            }
            ++e.count;
        }
        return e;
    }

    static bool seamAt(const EdgeInfo& e, std::uint32_t point, std::uint32_t other)
    {
        return point < other ? e.seamAtLow() : e.seamAtHigh();
    }

    // Which places may move, and how (Kind).
    void classify()
    {
        kind_.assign(points_.size(), Kind::Free);
        std::vector<std::uint32_t> firstWedge(points_.size(), none);
        std::vector<std::uint8_t> wedges(points_.size(), 0);
        for (const auto& t : triangles_)
            for (std::uint32_t v : t) {
                const std::uint32_t p = position_[v];
                if (firstWedge[p] == none) {
                    firstWedge[p] = wedge_[v];
                    wedges[p] = 1;
                } else if (firstWedge[p] != wedge_[v]) {
                    wedges[p] = 2; // two or more; seam edges sort it out below
                }
            }
        const auto edges = edgeInfo();
        std::vector<std::uint8_t> borderEdges(points_.size(), 0);
        std::vector<std::uint8_t> seamEdges(points_.size(), 0);
        auto bump = [](std::uint8_t& n) { n = static_cast<std::uint8_t>(std::min(n + 1, 3)); };
        for (const auto& [key, e] : edges) {
            const auto a = static_cast<std::uint32_t>(key >> 32);
            const auto b = static_cast<std::uint32_t>(key & 0xffffffffu);
            if (e.count > 2) {
                kind_[a] = kind_[b] = Kind::Locked;
                continue;
            }
            if (e.count == 1) {
                bump(borderEdges[a]);
                bump(borderEdges[b]);
            }
            if (e.seamAtLow())
                bump(seamEdges[a]);
            if (e.seamAtHigh())
                bump(seamEdges[b]);
        }
        for (std::size_t p = 0; p < points_.size(); ++p) {
            if (kind_[p] == Kind::Locked)
                continue;
            if (borderEdges[p] != 0)
                kind_[p] = borderEdges[p] == 2 && wedges[p] == 1 ? Kind::Border : Kind::Locked;
            else if (wedges[p] > 1)
                // A seam running through: two wedges, split along two edges.
                // More (a corner where seams meet) stays.
                kind_[p] = seamEdges[p] == 2 ? Kind::Seam : Kind::Locked;
        }
        // A place with two wedges and two seam edges may still have three
        // wedges if one repeats; count them properly for the seams.
        std::vector<std::uint32_t> secondWedge(points_.size(), none);
        for (const auto& t : triangles_)
            for (std::uint32_t v : t) {
                const std::uint32_t p = position_[v];
                if (kind_[p] != Kind::Seam || wedge_[v] == firstWedge[p])
                    continue;
                if (secondWedge[p] == none)
                    secondWedge[p] = wedge_[v];
                else if (secondWedge[p] != wedge_[v])
                    kind_[p] = Kind::Locked;
            }
        border_.clear();
        for (const auto& [key, e] : edges)
            if (e.count == 1)
                border_.insert(key);
    }

    void buildQuadrics()
    {
        quadrics_.assign(points_.size(), {});
        for (const auto& t : triangles_) {
            const Vector3 a = points_[position_[t[0]]];
            const Vector3 b = points_[position_[t[1]]];
            const Vector3 c = points_[position_[t[2]]];
            Vector3 n = cross(b - a, c - a);
            const float length = n.magnitude();
            if (length <= 0.0f)
                continue;
            n = n / length;
            const double area = 0.5 * static_cast<double>(length);
            const double d = -static_cast<double>(n.dot(a));
            for (std::size_t k = 0; k < 3; ++k)
                quadrics_[position_[t[k]]].addPlane(n.x, n.y, n.z, d, area);
            // Border edges get a plane through them, square to the face.
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t p = position_[t[k]];
                const std::uint32_t q = position_[t[(k + 1) % 3]];
                if (!border_.contains(edgeKey(p, q)))
                    continue;
                const Vector3 edge = points_[q] - points_[p];
                Vector3 m = cross(edge, n);
                const float mLength = m.magnitude();
                if (mLength <= 0.0f)
                    continue;
                m = m / mLength;
                const double w = borderWeight * static_cast<double>(edge.dot(edge));
                const double md = -static_cast<double>(m.dot(points_[p]));
                quadrics_[p].addPlane(m.x, m.y, m.z, md, w);
                quadrics_[q].addPlane(m.x, m.y, m.z, md, w);
            }
        }
    }

    double cost(std::uint32_t from, std::uint32_t to) const
    {
        const Vector3 p = points_[to];
        return std::max(quadrics_[from].error(p), quadrics_[to].error(p));
    }

    // Whether `from` may move onto `to` by its kind: borders and seams only
    // slide along themselves.
    bool allowed(std::uint32_t from, std::uint32_t to, const EdgeInfo& edge) const
    {
        switch (kind_[from]) {
        case Kind::Free:
            return true;
        case Kind::Border:
            return edge.count == 1;
        case Kind::Seam:
            // Both ends must split the same way, or one side's UVs would
            // stretch over the other's.
            return seamAt(edge, from, to) && seamAt(edge, to, from);
        case Kind::Locked:
            break;
        }
        return false;
    }

    // One round of collapses that don't touch each other; false if none could.
    bool pass(std::size_t target)
    {
        // Each position's live triangles.
        std::vector<std::uint32_t> start(points_.size() + 1, 0);
        for (std::size_t t = 0; t < triangles_.size(); ++t)
            if (alive_[t] != 0)
                for (std::uint32_t v : triangles_[t])
                    ++start[position_[v] + 1];
        for (std::size_t p = 0; p < points_.size(); ++p)
            start[p + 1] += start[p];
        std::vector<std::uint32_t> around(start.back());
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (std::size_t t = 0; t < triangles_.size(); ++t)
            if (alive_[t] != 0)
                for (std::uint32_t v : triangles_[t])
                    around[fill[position_[v]]++] = static_cast<std::uint32_t>(t);

        auto trianglesOf = [&](std::uint32_t p) {
            return std::span<const std::uint32_t>(around).subspan(start[p], start[p + 1] - start[p]);
        };
        std::vector<Candidate> candidates;
        // The edges from one place to its neighbours, as edgeInfo() has
        // them: gathered from its faces (each face on an edge from it is one
        // of them, in ascending order as edgeInfo reads them) into a list
        // indexed through a per-place slot, emptied for the next place.
        std::vector<std::uint32_t> slot(points_.size(), none);
        std::vector<EdgeInfo> local;
        std::vector<std::uint32_t> localPlaces;
        for (std::uint32_t from = 0; from < points_.size(); ++from) {
            if (kind_[from] == Kind::Locked || start[from] == start[from + 1])
                continue;
            for (std::uint32_t i = start[from]; i < start[from + 1]; ++i) {
                const auto& tri = triangles_[around[i]];
                std::uint32_t fromVertex = none;
                for (std::uint32_t v : tri)
                    if (position_[v] == from)
                        fromVertex = v;
                for (std::uint32_t v : tri) {
                    const std::uint32_t to = position_[v];
                    if (to == from)
                        continue;
                    if (slot[to] == none) {
                        slot[to] = static_cast<std::uint32_t>(local.size());
                        local.emplace_back();
                        localPlaces.push_back(to);
                    }
                    EdgeInfo& e = local[slot[to]];
                    if (e.count < 2) {
                        e.lowWedge[e.count] = wedge_[from < to ? fromVertex : v];
                        e.highWedge[e.count] = wedge_[from < to ? v : fromVertex];
                    }
                    ++e.count;
                }
            }
            Candidate best{std::numeric_limits<double>::infinity(), from, from};
            for (std::uint32_t i = start[from]; i < start[from + 1]; ++i) {
                for (std::uint32_t v : triangles_[around[i]]) {
                    const std::uint32_t to = position_[v];
                    if (to == from || !allowed(from, to, local[slot[to]]))
                        continue;
                    const double c = cost(from, to);
                    if (c < best.cost)
                        best = {c, from, to};
                }
            }
            if (best.to != from)
                candidates.push_back(best);
            for (std::uint32_t p : localPlaces)
                slot[p] = none;
            local.clear();
            localPlaces.clear();
        }
        std::ranges::sort(candidates, {}, &Candidate::cost);

        std::vector<std::uint8_t> touched(points_.size(), 0);
        std::vector<std::uint32_t> ring;
        bool collapsed = false;
        for (const Candidate& c : candidates) {
            if (live_ <= target)
                break;
            if (touched[c.from] != 0 || touched[c.to] != 0)
                continue;
            // c.from is untouched, so its faces are as they were when the
            // pass began, and the edge with them.
            if (!collapse(c, trianglesOf(c.from), trianglesOf(c.to),
                          edgeBetween(c.from, c.to, trianglesOf(c.from)).count == 1, ring))
                continue;
            collapsed = true;
            maxError_ = std::max(maxError_, c.cost);
            for (std::uint32_t p : ring)
                touched[p] = 1;
        }
        return collapsed;
    }

    // The render vertex at `to` a corner (render vertex `from`) moving there
    // takes: of those with the wedge the faces being removed pair its wedge
    // with, the one whose normal is closest.
    std::uint32_t replacement(std::uint32_t from, std::uint32_t to, std::uint32_t targetWedge) const
    {
        std::uint32_t best = none;
        float bestDot = -std::numeric_limits<float>::infinity();
        for (std::uint32_t i = start_[to]; i < start_[to + 1]; ++i) {
            const std::uint32_t v = at_[i];
            if (wedge_[v] != targetWedge)
                continue;
            const float d = mesh_.vertices[v].normal.dot(mesh_.vertices[from].normal);
            if (d > bestDot) {
                bestDot = d;
                best = v;
            }
        }
        return best;
    }

    // Moves `from` onto `to` if that keeps the surface manifold and turns no
    // face over (or nearly); ring gets every position involved.
    bool collapse(const Candidate& c, std::span<const std::uint32_t> fromTriangles,
                  std::span<const std::uint32_t> toTriangles, bool borderEdge, std::vector<std::uint32_t>& ring)
    {
        // Link condition: the two may share only the neighbours across the
        // faces on their edge (two inside, one along a border).
        ring.clear();
        for (std::uint32_t t : fromTriangles)
            for (std::uint32_t v : triangles_[t])
                ring.push_back(position_[v]);
        std::ranges::sort(ring);
        ring.erase(std::ranges::unique(ring).begin(), ring.end());
        std::size_t shared = 0;
        std::vector<std::uint32_t> seen;
        for (std::uint32_t t : toTriangles)
            for (std::uint32_t v : triangles_[t]) {
                const std::uint32_t p = position_[v];
                if (p != c.from && p != c.to && std::ranges::binary_search(ring, p)
                    && std::ranges::find(seen, p) == seen.end()) {
                    seen.push_back(p);
                    ++shared;
                }
            }
        if (shared > (borderEdge ? 1u : 2u))
            return false;

        // The faces being removed pair each of `from`'s wedges with one of
        // `to`'s: the rest of `from`'s faces follow that pairing.
        std::array<std::pair<std::uint32_t, std::uint32_t>, 2> pairing{{{none, none}, {none, none}}};
        for (std::uint32_t t : fromTriangles) {
            const auto& tri = triangles_[t];
            std::uint32_t fromVertex = none;
            std::uint32_t toVertex = none;
            for (std::uint32_t v : tri) {
                if (position_[v] == c.from)
                    fromVertex = v;
                else if (position_[v] == c.to)
                    toVertex = v;
            }
            if (toVertex == none)
                continue;
            const std::uint32_t w = wedge_[fromVertex];
            auto slot = std::ranges::find_if(pairing, [&](const auto& p) { return p.first == w || p.first == none; });
            if (slot == pairing.end())
                return false;
            if (slot->first == none)
                *slot = {w, wedge_[toVertex]};
            else if (slot->second != wedge_[toVertex])
                return false; // one wedge going two ways: a seam ending here
        }

        const Vector3 target = points_[c.to];
        std::vector<std::pair<std::uint32_t, std::uint32_t>> moves; // face, new render vertex
        for (std::uint32_t t : fromTriangles) {
            const auto& tri = triangles_[t];
            if (position_[tri[0]] == c.to || position_[tri[1]] == c.to || position_[tri[2]] == c.to)
                continue;
            Vector3 p[3];
            Vector3 q[3];
            std::uint32_t fromVertex = none;
            for (std::size_t k = 0; k < 3; ++k) {
                p[k] = points_[position_[tri[k]]];
                q[k] = p[k];
                if (position_[tri[k]] == c.from) {
                    q[k] = target;
                    fromVertex = tri[k];
                }
            }
            const Vector3 before = cross(p[1] - p[0], p[2] - p[0]);
            const Vector3 after = cross(q[1] - q[0], q[2] - q[0]);
            // Turning a face by more than about 75 degrees counts as a flip:
            // past that, slivers stand up on edge.
            if (before.dot(after) <= 0.25f * before.magnitude() * after.magnitude())
                return false;
            const auto pair
                = std::ranges::find(pairing, wedge_[fromVertex], &std::pair<std::uint32_t, std::uint32_t>::first);
            if (pair == pairing.end())
                return false;
            const std::uint32_t moved = replacement(fromVertex, c.to, pair->second);
            if (moved == none)
                return false;
            moves.emplace_back(t, moved);
        }

        for (std::uint32_t t : fromTriangles) {
            const auto& tri = triangles_[t];
            if (position_[tri[0]] == c.to || position_[tri[1]] == c.to || position_[tri[2]] == c.to) {
                alive_[t] = 0;
                --live_;
            }
        }
        for (const auto& [t, moved] : moves)
            for (std::uint32_t& v : triangles_[t])
                if (position_[v] == c.from)
                    v = moved;
        quadrics_[c.to] += quadrics_[c.from];
        kind_[c.from] = Kind::Locked; // gone
        ring.push_back(c.to);
        return true;
    }

    const MeshData& mesh_;
    std::vector<std::uint32_t> position_; // render vertex -> welded place
    std::vector<std::uint32_t> wedge_;    // render vertex -> place and attributes
    std::vector<std::uint32_t> start_;    // place -> its render vertices, at_[start_[p], start_[p + 1])
    std::vector<std::uint32_t> at_;
    std::vector<Vector3> points_;
    std::vector<Kind> kind_;
    std::unordered_set<std::uint64_t> border_;
    std::vector<Quadric> quadrics_;
    std::vector<std::array<std::uint32_t, 3>> triangles_;
    std::vector<std::uint8_t> alive_;
    std::size_t live_ = 0;
    double maxError_ = 0.0;
};

} // namespace

void generateLods(MeshData& mesh)
{
    const std::size_t full = mesh.indices.size() / 3;
    if (!mesh.lodEnds.empty() || full < lodMinimumTriangles)
        return;
    Simplifier simplifier(mesh);
    std::size_t previous = full;
    for (float share : lodTriangleShares) {
        const auto target = static_cast<std::size_t>(std::ceil(static_cast<float>(full) * share));
        simplifier.reduce(target);
        const std::size_t reached = simplifier.triangles();
        // Not much of a step down: this level and coarser ones aren't worth it.
        if (reached == 0 || reached * 5 > previous * 4)
            break;
        simplifier.append(mesh.lodIndices);
        mesh.lodEnds.push_back(static_cast<std::uint32_t>(mesh.lodIndices.size()));
        mesh.lodErrors.push_back(static_cast<float>(simplifier.error()));
        previous = reached;
        if (reached * 2 > target * 3)
            break; // stuck well short of the target; coarser ones won't get further
    }
}

} // namespace lunar::assets
