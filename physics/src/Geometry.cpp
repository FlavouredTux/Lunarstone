#include <lunar/physics/Geometry.hpp>

#include <lunar/physics/CollisionData.hpp>

#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::physics {

Aabb Aabb::around(const CFrame& cf, Vector3 half)
{
    // Each world axis' reach is the absolute rotation times the half size.
    Vector3 reach;
    reach.x = std::abs(cf.r[0][0]) * half.x + std::abs(cf.r[0][1]) * half.y + std::abs(cf.r[0][2]) * half.z;
    reach.y = std::abs(cf.r[1][0]) * half.x + std::abs(cf.r[1][1]) * half.y + std::abs(cf.r[1][2]) * half.z;
    reach.z = std::abs(cf.r[2][0]) * half.x + std::abs(cf.r[2][1]) * half.y + std::abs(cf.r[2][2]) * half.z;
    return {cf.position - reach, cf.position + reach};
}

std::optional<float> rayAabb(const Aabb& box, Vector3 origin, Vector3 inverseDirection, float maxT)
{
    float t0 = 0.0f;
    float t1 = maxT;
    const std::array<float, 3> o{origin.x, origin.y, origin.z};
    const std::array<float, 3> inv{inverseDirection.x, inverseDirection.y, inverseDirection.z};
    const std::array<float, 3> lo{box.min.x, box.min.y, box.min.z};
    const std::array<float, 3> hi{box.max.x, box.max.y, box.max.z};
    for (std::size_t i = 0; i < 3; ++i) {
        float a = (lo[i] - o[i]) * inv[i];
        float b = (hi[i] - o[i]) * inv[i];
        if (a > b)
            std::swap(a, b);
        // NaN (0 * inf) on an axis the ray runs along inside the slab: no constraint.
        if (a == a)
            t0 = std::max(t0, a);
        if (b == b)
            t1 = std::min(t1, b);
        if (t0 > t1)
            return std::nullopt;
    }
    return t0;
}

namespace {

// Edge list and unique edge directions, once faces and their planes are in.
void addEdges(Polyhedron& poly)
{
    // The face walking each edge downwards, by its corners (upwards), sorted
    // for lookup; the first such face if there are more.
    std::vector<std::pair<std::uint32_t, std::uint16_t>> downwards;
    for (std::size_t f = 0; f < poly.faces.size(); ++f) {
        const auto& indices = poly.faces[f].indices;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const std::uint16_t from = indices[i];
            const std::uint16_t to = indices[(i + 1) % indices.size()];
            if (from > to)
                downwards.emplace_back(std::uint32_t{to} << 16 | from, static_cast<std::uint16_t>(f));
        }
    }
    std::ranges::stable_sort(downwards, {}, &std::pair<std::uint32_t, std::uint16_t>::first);
    bool paired = true;
    for (std::size_t f = 0; f < poly.faces.size(); ++f) {
        const Polyhedron::Face& face = poly.faces[f];
        const auto& indices = face.indices;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const std::uint16_t from = indices[i];
            const std::uint16_t to = indices[(i + 1) % indices.size()];
            // Each edge borders two faces; keep it from the face that walks it upwards.
            if (from < to) {
                poly.edges.push_back({from, to});
                const std::uint32_t key = std::uint32_t{from} << 16 | to;
                const auto other
                    = std::ranges::lower_bound(downwards, key, {}, &std::pair<std::uint32_t, std::uint16_t>::first);
                // Back to back faces (a flat polygon's two sides) have no arc between them.
                if (other == downwards.end() || other->first != key
                    || face.normal.dot(poly.faces[other->second].normal) < -0.9999f)
                    paired = false;
                else
                    poly.edgeFaces.push_back({static_cast<std::uint16_t>(f), other->second});
            }
            const Vector3 edge = safeUnit(poly.vertices[to] - poly.vertices[from]);
            if (edge == Vector3{})
                continue;
            const bool known
                = std::ranges::any_of(poly.edgeDirections, [&](Vector3 e) { return std::abs(e.dot(edge)) > 0.9999f; });
            if (!known)
                poly.edgeDirections.push_back(edge);
        }
    }
    if (!paired)
        poly.edgeFaces.clear();
}

} // namespace

Polyhedron Polyhedron::fromFaces(std::vector<Vector3> vertices, std::vector<std::vector<std::uint16_t>> faces)
{
    Polyhedron poly;
    poly.vertices = std::move(vertices);
    for (auto& indices : faces) {
        const Vector3 a = poly.vertices[indices[0]];
        const Vector3 b = poly.vertices[indices[1]];
        const Vector3 c = poly.vertices[indices[2]];
        const Vector3 normal = (b - a).cross(c - a).unit();
        poly.faces.push_back({normal, normal.dot(a), std::move(indices)});
    }
    addEdges(poly);
    return poly;
}

Polyhedron Polyhedron::fromPlanes(std::vector<Vector3> vertices, std::vector<std::vector<std::uint16_t>> faces,
                                  std::vector<Vector3> normals)
{
    Polyhedron poly;
    poly.vertices = std::move(vertices);
    for (auto [indices, normal] : std::views::zip(faces, normals)) {
        // The plane through the face's outermost corner, so no corner pokes out.
        float offset = -std::numeric_limits<float>::max();
        for (std::uint16_t i : indices)
            offset = std::max(offset, normal.dot(poly.vertices[i]));
        poly.faces.push_back({normal, offset, std::move(indices)});
    }
    addEdges(poly);
    return poly;
}

Vector3 Polyhedron::support(Vector3 dir) const
{
    Vector3 best = vertices.front();
    float bestDot = best.dot(dir);
    for (const Vector3& v : vertices) {
        const float d = v.dot(dir);
        if (d > bestDot) {
            bestDot = d;
            best = v;
        }
    }
    return best;
}

namespace {

constexpr float pi = std::numbers::pi_v<float>;

Polyhedron boxPolyhedron(Vector3 h)
{
    std::vector<Vector3> v;
    v.reserve(8);
    for (int i = 0; i < 8; ++i)
        v.push_back({(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
    return Polyhedron::fromFaces(std::move(v), {{1, 3, 7, 5},   // +X
                                                {0, 4, 6, 2},   // -X
                                                {2, 6, 7, 3},   // +Y
                                                {0, 1, 5, 4},   // -Y
                                                {4, 5, 7, 6},   // +Z
                                                {0, 2, 3, 1}}); // -Z
}

// Slope from the top back edge down to the bottom front edge, like WedgePart.
Polyhedron wedgePolyhedron(Vector3 h)
{
    std::vector<Vector3> v{
        {-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z}, {h.x, h.y, h.z}, {-h.x, h.y, h.z},
    };
    return Polyhedron::fromFaces(std::move(v), {{0, 1, 2, 3}, // bottom
                                                {3, 2, 4, 5}, // back, +Z
                                                {1, 0, 5, 4}, // slope
                                                {1, 4, 2},    // +X
                                                {0, 3, 5}});  // -X
}

// Apex above the +X -Z bottom corner, like CornerWedgePart.
Polyhedron cornerWedgePolyhedron(Vector3 h)
{
    std::vector<Vector3> v{
        {-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z}, {h.x, h.y, -h.z},
    };
    return Polyhedron::fromFaces(std::move(v), {{0, 1, 2, 3}, // bottom
                                                {2, 1, 4},    // +X
                                                {1, 0, 4},    // -Z
                                                {0, 3, 4},    // slope to -X
                                                {3, 2, 4}});  // slope to +Z
}

// Prism along X with the cylinder's radius at its corners.
Polyhedron cylinderPolyhedron(float halfLength, float radius)
{
    std::vector<Vector3> v;
    for (int end = 0; end < 2; ++end)
        for (int i = 0; i < cylinderSegments; ++i) {
            const float a = 2.0f * pi * static_cast<float>(i) / static_cast<float>(cylinderSegments);
            v.push_back({end ? halfLength : -halfLength, radius * std::cos(a), radius * std::sin(a)});
        }
    std::vector<std::vector<std::uint16_t>> faces;
    std::vector<std::uint16_t> low;
    std::vector<std::uint16_t> high;
    constexpr auto n = static_cast<std::uint16_t>(cylinderSegments);
    for (std::uint16_t i = 0; i < n; ++i) {
        const auto next = static_cast<std::uint16_t>((i + 1) % n);
        faces.push_back({i, next, static_cast<std::uint16_t>(next + n), static_cast<std::uint16_t>(i + n)});
        low.push_back(static_cast<std::uint16_t>(n - 1 - i));
        high.push_back(static_cast<std::uint16_t>(i + n));
    }
    faces.push_back(std::move(low));
    faces.push_back(std::move(high));
    return Polyhedron::fromFaces(std::move(v), std::move(faces));
}

std::optional<LocalHit> rayPolyhedron(const Polyhedron& poly, Vector3 origin, Vector3 direction)
{
    float enter = -1.0f;
    float exit = 1.0f;
    Vector3 normal;
    bool inside = true;
    for (const Polyhedron::Face& face : poly.faces) {
        const float dist = face.normal.dot(origin) - face.offset;
        const float speed = face.normal.dot(direction);
        if (dist > 0.0f)
            inside = false;
        if (std::abs(speed) < 1e-12f) {
            if (dist > 0.0f)
                return std::nullopt;
            continue;
        }
        const float t = -dist / speed;
        if (speed < 0.0f) {
            if (t > enter) {
                enter = t;
                normal = face.normal;
            }
        } else {
            exit = std::min(exit, t);
        }
    }
    if (inside || enter < 0.0f || enter > exit || enter > 1.0f)
        return std::nullopt;
    return LocalHit{enter, normal};
}

std::optional<LocalHit> rayBall(float radius, Vector3 origin, Vector3 direction)
{
    const float a = direction.dot(direction);
    const float b = origin.dot(direction);
    const float c = origin.dot(origin) - radius * radius;
    if (c <= 0.0f || a <= 0.0f)
        return std::nullopt; // starts inside
    const float disc = b * b - a * c;
    if (disc < 0.0f)
        return std::nullopt;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t < 0.0f || t > 1.0f)
        return std::nullopt;
    return LocalHit{t, (origin + direction * t).unit()};
}

std::optional<LocalHit> rayCylinder(float halfLength, float radius, Vector3 origin, Vector3 direction)
{
    const float r2 = radius * radius;
    const float radial = origin.y * origin.y + origin.z * origin.z;
    if (radial <= r2 && std::abs(origin.x) <= halfLength)
        return std::nullopt;
    std::optional<LocalHit> best;
    auto consider = [&](float t, Vector3 normal) {
        if (t >= 0.0f && t <= 1.0f && (!best || t < best->t))
            best = LocalHit{t, normal};
    };
    // Caps.
    if (std::abs(direction.x) > 1e-12f)
        for (float side : {-1.0f, 1.0f}) {
            const float t = (side * halfLength - origin.x) / direction.x;
            const Vector3 p = origin + direction * t;
            if (p.y * p.y + p.z * p.z <= r2 && side * direction.x < 0.0f)
                consider(t, {side, 0.0f, 0.0f});
        }
    // Round side.
    const float a = direction.y * direction.y + direction.z * direction.z;
    if (a > 1e-12f) {
        const float b = origin.y * direction.y + origin.z * direction.z;
        const float c = radial - r2;
        const float disc = b * b - a * c;
        if (disc >= 0.0f) {
            const float t = (-b - std::sqrt(disc)) / a;
            const Vector3 p = origin + direction * t;
            if (std::abs(p.x) <= halfLength && c > 0.0f)
                consider(t, Vector3{0.0f, p.y, p.z}.unit());
        }
    }
    return best;
}

} // namespace

float Shape::volume() const
{
    switch (kind) {
    case ShapeKind::Mesh:
        return mesh->volume * scale.x * scale.y * scale.z;
    case ShapeKind::Ball:
        return 4.0f / 3.0f * pi * radius * radius * radius;
    case ShapeKind::Cylinder:
        return pi * radius * radius * size.x;
    case ShapeKind::Wedge:
        return 0.5f * size.x * size.y * size.z;
    case ShapeKind::CornerWedge:
        return size.x * size.y * size.z / 3.0f;
    case ShapeKind::Box:
        break;
    }
    return size.x * size.y * size.z;
}

Vector3 Shape::support(Vector3 dir) const
{
    if (kind == ShapeKind::Ball)
        return safeUnit(dir) * radius;
    if (kind == ShapeKind::Mesh) {
        // Support of the scaled pieces: scale the direction the other way.
        const Vector3 d = mul(dir, scale);
        Vector3 best;
        float bestDot = -std::numeric_limits<float>::max();
        for (const Polyhedron& piece : mesh->pieces) {
            const Vector3 v = piece.support(d);
            if (v.dot(d) > bestDot) {
                bestDot = v.dot(d);
                best = v;
            }
        }
        return mul(best, scale);
    }
    return polyhedron->support(dir);
}

Shape makeShape(ShapeKind kind, Vector3 size)
{
    Shape shape;
    shape.kind = kind;
    shape.size = size;
    const Vector3 h = size * 0.5f;
    switch (kind) {
    case ShapeKind::Ball:
        // Balls are as big as their smallest side.
        shape.radius = std::min({h.x, h.y, h.z});
        break;
    case ShapeKind::Cylinder:
        // Along X, round in Y and Z.
        shape.radius = std::min(h.y, h.z);
        shape.polyhedron = std::make_shared<const Polyhedron>(cylinderPolyhedron(h.x, shape.radius));
        break;
    case ShapeKind::Wedge:
        shape.polyhedron = std::make_shared<const Polyhedron>(wedgePolyhedron(h));
        break;
    case ShapeKind::CornerWedge:
        shape.polyhedron = std::make_shared<const Polyhedron>(cornerWedgePolyhedron(h));
        break;
    case ShapeKind::Box:
    case ShapeKind::Mesh: // without geometry, the box
        shape.kind = ShapeKind::Box;
        shape.polyhedron = std::make_shared<const Polyhedron>(boxPolyhedron(h));
        break;
    }
    return shape;
}

Vector3 meshScale(Vector3 size, Vector3 extent)
{
    // In double like the client; an axis whose extent is lost in rounding
    // against 1 keeps scale 1.
    auto axis = [](float s, float e) {
        const double magnitude = std::abs(static_cast<double>(e));
        if (magnitude == 0.0 || magnitude <= (1.0 + magnitude) * 1e-5)
            return 1.0f;
        return static_cast<float>(static_cast<double>(s) / static_cast<double>(e));
    };
    return {axis(size.x, extent.x), axis(size.y, extent.y), axis(size.z, extent.z)};
}

Shape makeMeshShape(std::shared_ptr<const MeshGeometry> mesh, Vector3 size)
{
    if (!mesh || mesh->pieces.empty())
        return makeShape(ShapeKind::Box, size);
    Shape shape;
    shape.kind = ShapeKind::Mesh;
    shape.size = size;
    shape.scale = meshScale(size, mesh->extent);
    shape.mesh = std::move(mesh);
    shape.skin = meshSkin;
    return shape;
}

namespace {

float polyhedronVolume(const Polyhedron& poly)
{
    // Tetrahedra from the first vertex to every face's fan.
    if (poly.vertices.empty())
        return 0.0f;
    const Vector3 o = poly.vertices.front();
    float volume = 0.0f;
    for (const Polyhedron::Face& face : poly.faces)
        for (std::size_t i = 1; i + 1 < face.indices.size(); ++i) {
            const Vector3 a = poly.vertices[face.indices[0]] - o;
            const Vector3 b = poly.vertices[face.indices[i]] - o;
            const Vector3 c = poly.vertices[face.indices[i + 1]] - o;
            volume += a.dot(b.cross(c)) / 6.0f;
        }
    return std::abs(volume);
}

} // namespace

namespace {

// A blob decoded for its geometry: the data to hull, or nothing (the box)
// with why, when something was wrong.
struct Decoded {
    std::optional<CollisionData> data;
    std::string error;
};

Decoded decodeGeometry(std::string_view blob)
{
    if (blob.empty())
        return {}; // no data: the box, nothing wrong
    auto data = decodeCollisionData(blob);
    if (!data)
        return {std::nullopt, data.error()};
    if (data->block)
        return {};
    // TriangleMesh only takes data from version 3 on (sub_102fac0fa).
    if (data->version < 3)
        return {std::nullopt, std::format("version {} collision data isn't used", data->version)};
    return {std::move(*data), {}};
}

Vector3 geometryCenter(const CollisionData& data)
{
    return (data.boundsMin + data.boundsMax) * 0.5f;
}

Polyhedron hullPiece(const CollisionPiece& piece, Vector3 center)
{
    std::vector<Vector3> points;
    points.reserve(piece.vertices.size());
    for (const Vector3& v : piece.vertices)
        points.push_back(v - center);
    return convexHull(points);
}

// The geometry from the data and its pieces' hulls, in piece order.
std::shared_ptr<const MeshGeometry> assemble(const CollisionData& data, std::span<Polyhedron> hulls, std::string* error)
{
    auto geometry = std::make_shared<MeshGeometry>();
    geometry->extent = data.boundsMax - data.boundsMin;
    float pieceVolume = 0.0f;
    for (Polyhedron& hull : hulls) {
        if (hull.empty())
            continue; // a line or a point: nothing to collide with
        Aabb box;
        for (const Vector3& v : hull.vertices)
            box.expand(v);
        pieceVolume += polyhedronVolume(hull);
        geometry->pieces.push_back(std::move(hull));
        geometry->bounds.push_back(box);
    }
    if (geometry->pieces.empty()) {
        if (error)
            *error = "collision data has no pieces"; // "Decomp data has no children"
        return nullptr;
    }
    geometry->volume = data.mass && data.mass->volume > 0.0f ? data.mass->volume : pieceVolume;
    return geometry;
}

} // namespace

std::shared_ptr<const MeshGeometry> piecesGeometry(std::span<const GeometryPiece> pieces)
{
    std::vector<std::vector<Vector3>> corners;
    Aabb all;
    for (const GeometryPiece& piece : pieces) {
        const Vector3 h = piece.size * 0.5f;
        Polyhedron local;
        switch (piece.kind) {
        case ShapeKind::Box: local = boxPolyhedron(h); break;
        case ShapeKind::Wedge: local = wedgePolyhedron(h); break;
        case ShapeKind::CornerWedge: local = cornerWedgePolyhedron(h); break;
        case ShapeKind::Cylinder: local = cylinderPolyhedron(h.x, std::min(h.y, h.z)); break;
        case ShapeKind::Ball:
        case ShapeKind::Mesh: return nullptr;
        }
        auto& points = corners.emplace_back();
        for (const Vector3& v : local.vertices) {
            points.push_back(piece.cframe.pointToWorldSpace(v));
            all.expand(points.back());
        }
    }
    if (corners.empty())
        return nullptr;
    const Vector3 center = (all.min + all.max) * 0.5f;
    auto geometry = std::make_shared<MeshGeometry>();
    geometry->extent = all.max - all.min;
    for (auto& points : corners) {
        for (Vector3& p : points)
            p = p - center;
        Polyhedron hull = convexHull(points);
        if (hull.empty())
            continue;
        Aabb box;
        for (const Vector3& v : hull.vertices)
            box.expand(v);
        geometry->volume += polyhedronVolume(hull);
        geometry->pieces.push_back(std::move(hull));
        geometry->bounds.push_back(box);
    }
    if (geometry->pieces.empty())
        return nullptr;
    return geometry;
}

std::shared_ptr<const MeshGeometry> meshGeometry(std::string_view blob, std::string* error)
{
    Decoded decoded = decodeGeometry(blob);
    if (!decoded.data) {
        if (error && !decoded.error.empty())
            *error = std::move(decoded.error);
        return nullptr;
    }
    const Vector3 center = geometryCenter(*decoded.data);
    std::vector<Polyhedron> hulls;
    hulls.reserve(decoded.data->pieces.size());
    for (const CollisionPiece& piece : decoded.data->pieces)
        hulls.push_back(hullPiece(piece, center));
    return assemble(*decoded.data, hulls, error);
}

std::shared_ptr<const MeshGeometry> MeshGeometryCache::get(const SharedBytes& blob, std::string* error)
{
    if (!blob || blob->empty())
        return nullptr;
    auto found = byAddress_.find(blob.get());
    if (found == byAddress_.end()) {
        // Same bytes under another pointer (another file, or XML) share too.
        auto same = byContent_.find(*blob);
        std::shared_ptr<const Entry> entry;
        if (same != byContent_.end()) {
            entry = same->second;
        } else {
            auto fresh = std::make_shared<Entry>();
            fresh->blob = blob;
            fresh->geometry = meshGeometry(*blob, &fresh->error);
            entry = fresh;
            byContent_.emplace(*entry->blob, entry);
        }
        found = byAddress_.emplace(blob.get(), Seen{blob, entry}).first;
    }
    const Entry& entry = *found->second.entry;
    if (error && !entry.error.empty())
        *error = entry.error;
    return entry.geometry;
}

void MeshGeometryCache::prepare(std::span<const SharedBytes> blobs, Runner run)
{
    // The bytes not seen yet, each once.
    std::vector<std::shared_ptr<Entry>> fresh;
    std::unordered_map<std::string_view, std::size_t> index;
    for (const SharedBytes& blob : blobs) {
        if (!blob || blob->empty() || byAddress_.contains(blob.get()) || byContent_.contains(*blob))
            continue;
        if (index.try_emplace(*blob, fresh.size()).second) {
            fresh.push_back(std::make_shared<Entry>());
            fresh.back()->blob = blob;
        }
    }
    // Decoded side by side, then every piece of every blob hulled side by
    // side (one big mesh can hold most of the pieces), then put together
    // exactly as meshGeometry does.
    std::vector<Decoded> decoded(fresh.size());
    run(fresh.size(), [&](std::size_t i) { decoded[i] = decodeGeometry(*fresh[i]->blob); });
    std::vector<std::pair<std::size_t, std::size_t>> pieces; // blob, piece
    std::vector<std::size_t> firstHull(fresh.size() + 1, 0);
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        firstHull[i] = pieces.size();
        if (const std::optional<CollisionData>& data = decoded[i].data)
            for (std::size_t k = 0; k < data->pieces.size(); ++k)
                pieces.emplace_back(i, k);
    }
    firstHull[fresh.size()] = pieces.size();
    std::vector<Polyhedron> hulls(pieces.size());
    run(pieces.size(), [&](std::size_t n) {
        const auto [i, k] = pieces[n];
        const CollisionData& data = *decoded[i].data;
        hulls[n] = hullPiece(data.pieces[k], geometryCenter(data));
    });
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        Entry& entry = *fresh[i];
        if (const std::optional<CollisionData>& data = decoded[i].data)
            entry.geometry = assemble(*data, std::span(hulls).subspan(firstHull[i], firstHull[i + 1] - firstHull[i]),
                                      &entry.error);
        else
            entry.error = std::move(decoded[i].error);
        byContent_.emplace(*entry.blob, fresh[i]);
    }
}

Shape shapeOf(const BasePart& part, MeshGeometryCache* cache)
{
    if (const auto* mesh = instanceCast<const TriangleMeshPart>(&part)) {
        // PhysicsData is the older home of the same blob; it only counts
        // when the newer property is missing (a guess: the client migrates
        // one into the other).
        const SharedBytes& blob = mesh->physicalConfigData() && !mesh->physicalConfigData()->empty()
                                      ? mesh->physicalConfigData()
                                      : mesh->physicsData();
        if (!blob)
            return makeShape(ShapeKind::Box, part.size());
        auto geometry = cache ? cache->get(blob) : meshGeometry(*blob);
        return makeMeshShape(std::move(geometry), part.size());
    }
    ShapeKind kind = ShapeKind::Box;
    if (const auto* p = instanceCast<const Part>(&part)) {
        switch (p->shape()) {
        case Enum::PartType::Ball:
            kind = ShapeKind::Ball;
            break;
        case Enum::PartType::Cylinder:
            kind = ShapeKind::Cylinder;
            break;
        case Enum::PartType::Wedge:
            kind = ShapeKind::Wedge;
            break;
        case Enum::PartType::CornerWedge:
            kind = ShapeKind::CornerWedge;
            break;
        case Enum::PartType::Block:
            break;
        }
    } else if (part.isA("WedgePart")) {
        kind = ShapeKind::Wedge;
    } else if (part.isA("CornerWedgePart")) {
        kind = ShapeKind::CornerWedge;
    }
    return makeShape(kind, part.size());
}

std::optional<LocalHit> raycast(const Shape& shape, Vector3 origin, Vector3 direction)
{
    switch (shape.kind) {
    case ShapeKind::Mesh: {
        // Into the pieces' unscaled space: t stays the same along the ray,
        // normals come back through the inverse transpose of the scale.
        const Vector3 inverse{1.0f / shape.scale.x, 1.0f / shape.scale.y, 1.0f / shape.scale.z};
        const Vector3 o = mul(origin, inverse);
        const Vector3 d = mul(direction, inverse);
        const Vector3 invD{1.0f / d.x, 1.0f / d.y, 1.0f / d.z};
        std::optional<LocalHit> best;
        for (auto [piece, box] : std::views::zip(shape.mesh->pieces, shape.mesh->bounds)) {
            const float reach = best ? best->t : 1.0f;
            if (!rayAabb(box, o, invD, reach))
                continue;
            if (auto hit = rayPolyhedron(piece, o, d); hit && hit->t <= reach)
                best = LocalHit{hit->t, safeUnit(mul(hit->normal, inverse))};
        }
        return best;
    }
    case ShapeKind::Ball:
        return rayBall(shape.radius, origin, direction);
    case ShapeKind::Cylinder:
        return rayCylinder(shape.size.x * 0.5f, shape.radius, origin, direction);
    default:
        return rayPolyhedron(*shape.polyhedron, origin, direction);
    }
}

std::optional<float> rayTriangle(Vector3 origin, Vector3 direction, Vector3 a, Vector3 b, Vector3 c)
{
    const Vector3 e1 = b - a;
    const Vector3 e2 = c - a;
    const Vector3 p = direction.cross(e2);
    const float det = e1.dot(p);
    if (std::abs(det) < 1e-12f)
        return std::nullopt;
    const float inv = 1.0f / det;
    const Vector3 s = origin - a;
    const float u = s.dot(p) * inv;
    if (u < 0.0f || u > 1.0f)
        return std::nullopt;
    const Vector3 q = s.cross(e1);
    const float v = direction.dot(q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return std::nullopt;
    const float t = e2.dot(q) * inv;
    if (t < 0.0f || t > 1.0f)
        return std::nullopt;
    return t;
}

} // namespace lunar::physics
