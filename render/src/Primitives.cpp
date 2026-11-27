#include <lunar/render/Primitives.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace lunar::render {

namespace {

constexpr float h = 0.5f;

class Builder {
public:
    std::uint32_t vertex(Vector3 position, Vector3 normal, float u = 0.0f, float v = 0.0f)
    {
        mesh_.vertices.push_back({position, normal, u, v});
        return static_cast<std::uint32_t>(mesh_.vertices.size() - 1);
    }

    // Winds the triangle so it faces along outward, whatever order it came in.
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c, Vector3 outward)
    {
        const Vector3 pa = mesh_.vertices[a].position;
        const Vector3 n = (mesh_.vertices[b].position - pa).cross(mesh_.vertices[c].position - pa);
        if (n.dot(outward) < 0.0f)
            std::swap(b, c);
        mesh_.indices.insert(mesh_.indices.end(), {a, b, c});
    }

    // A flat polygon (convex, corners in order around it from the bottom
    // left as seen from outside) with its own vertices. Quads get the whole
    // texture, upright; triangles its lower right half.
    void face(std::initializer_list<Vector3> corners)
    {
        static constexpr float uvs[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
        const auto* p = corners.begin();
        const Vector3 normal = (p[1] - p[0]).cross(p[2] - p[0]).unit();
        const auto first = static_cast<std::uint32_t>(mesh_.vertices.size());
        std::size_t n = 0;
        for (Vector3 corner : corners) {
            vertex(corner, normal, uvs[n % 4][0], uvs[n % 4][1]);
            ++n;
        }
        for (std::uint32_t i = 1; i + 1 < corners.size(); ++i)
            triangle(first, first + i, first + i + 1, normal);
    }

    MeshData take() { return std::move(mesh_); }

private:
    MeshData mesh_;
};

// Corners of the unit cube by sign.
constexpr Vector3 corner(float x, float y, float z)
{
    return {x * h, y * h, z * h};
}

MeshData block()
{
    Builder b;
    b.face({corner(1, -1, 1), corner(1, -1, -1), corner(1, 1, -1), corner(1, 1, 1)});     // +X
    b.face({corner(-1, -1, -1), corner(-1, -1, 1), corner(-1, 1, 1), corner(-1, 1, -1)}); // -X
    b.face({corner(-1, 1, 1), corner(1, 1, 1), corner(1, 1, -1), corner(-1, 1, -1)});     // +Y
    b.face({corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), corner(-1, -1, 1)}); // -Y
    b.face({corner(-1, -1, 1), corner(1, -1, 1), corner(1, 1, 1), corner(-1, 1, 1)});     // +Z
    b.face({corner(1, -1, -1), corner(-1, -1, -1), corner(-1, 1, -1), corner(1, 1, -1)}); // -Z
    return b.take();
}

MeshData wedge()
{
    Builder b;
    b.face({corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), corner(-1, -1, 1)}); // bottom
    b.face({corner(-1, -1, 1), corner(1, -1, 1), corner(1, 1, 1), corner(-1, 1, 1)});     // back, +Z
    b.face({corner(1, -1, -1), corner(-1, -1, -1), corner(-1, 1, 1), corner(1, 1, 1)});   // slope
    b.face({corner(1, -1, -1), corner(1, 1, 1), corner(1, -1, 1)});                       // +X
    b.face({corner(-1, -1, -1), corner(-1, -1, 1), corner(-1, 1, 1)});                    // -X
    return b.take();
}

MeshData cornerWedge()
{
    const Vector3 apex = corner(1, 1, -1);
    Builder b;
    b.face({corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), corner(-1, -1, 1)}); // bottom
    b.face({corner(1, -1, 1), corner(1, -1, -1), apex});                                  // +X, vertical
    b.face({corner(1, -1, -1), corner(-1, -1, -1), apex});                                // -Z, vertical
    b.face({corner(-1, -1, -1), corner(-1, -1, 1), apex});                                // slope to -X
    b.face({corner(-1, -1, 1), corner(1, -1, 1), apex});                                  // slope to +Z
    return b.take();
}

// A cylinder along axis 0 (X) or 1 (Y): smooth sides, flat caps.
MeshData cylinder(int axis, std::uint32_t segments)
{
    // (along, around1, around2) -> xyz, keeping the frame right-handed.
    auto place
        = [axis](float a, float c, float s) -> Vector3 { return axis == 0 ? Vector3{a, c, s} : Vector3{s, a, c}; };
    Builder b;
    auto angle = [segments](float i) { return 2.0f * std::numbers::pi_v<float> * i / static_cast<float>(segments); };
    std::vector<std::array<std::uint32_t, 2>> side;
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const float t = angle(static_cast<float>(i));
        const float c = std::cos(t);
        const float s = std::sin(t);
        const Vector3 normal = place(0.0f, c, s);
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        side.push_back(
            {b.vertex(place(-h, c * h, s * h), normal, u, 1.0f), b.vertex(place(h, c * h, s * h), normal, u, 0.0f)});
    }
    for (std::uint32_t i = 0; i < segments; ++i) {
        const float t = angle(static_cast<float>(i) + 0.5f);
        const Vector3 outward = place(0.0f, std::cos(t), std::sin(t));
        b.triangle(side[i][0], side[i + 1][0], side[i + 1][1], outward);
        b.triangle(side[i][0], side[i + 1][1], side[i][1], outward);
    }
    for (float end : {-h, h}) {
        const Vector3 normal = place(end * 2.0f, 0.0f, 0.0f);
        const std::uint32_t centre = b.vertex(place(end, 0.0f, 0.0f), normal, 0.5f, 0.5f);
        std::vector<std::uint32_t> rim;
        for (std::uint32_t i = 0; i <= segments; ++i) {
            const float t = angle(static_cast<float>(i));
            const float c = std::cos(t);
            const float s = std::sin(t);
            rim.push_back(b.vertex(place(end, c * h, s * h), normal, 0.5f + c * 0.5f, 0.5f - s * 0.5f));
        }
        for (std::uint32_t i = 0; i < segments; ++i)
            b.triangle(centre, rim[i], rim[i + 1], normal);
    }
    return b.take();
}

MeshData ball(std::uint32_t segments)
{
    const std::uint32_t rings = std::max(segments / 2, 2u);
    Builder b;
    std::vector<std::uint32_t> grid;
    for (std::uint32_t ring = 0; ring <= rings; ++ring) {
        const float phi = std::numbers::pi_v<float> * static_cast<float>(ring) / static_cast<float>(rings);
        for (std::uint32_t i = 0; i <= segments; ++i) {
            const float theta = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(segments);
            const Vector3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            grid.push_back(b.vertex(n * h, n, static_cast<float>(i) / static_cast<float>(segments),
                                    static_cast<float>(ring) / static_cast<float>(rings)));
        }
    }
    const std::uint32_t row = segments + 1;
    for (std::uint32_t ring = 0; ring < rings; ++ring) {
        for (std::uint32_t i = 0; i < segments; ++i) {
            const std::uint32_t a = grid[ring * row + i];
            const std::uint32_t b2 = grid[ring * row + i + 1];
            const std::uint32_t c = grid[(ring + 1) * row + i];
            const std::uint32_t d = grid[(ring + 1) * row + i + 1];
            const float phi = std::numbers::pi_v<float> * (static_cast<float>(ring) + 0.5f) / static_cast<float>(rings);
            const float theta
                = 2.0f * std::numbers::pi_v<float> * (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
            const Vector3 outward{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            // The pole rows collapse to a point: one triangle each there.
            if (ring > 0)
                b.triangle(a, b2, d, outward);
            if (ring + 1 < rings)
                b.triangle(a, d, c, outward);
        }
    }
    return b.take();
}

} // namespace

MeshData makePrimitive(Primitive primitive, std::uint32_t segments)
{
    segments = std::max(segments, 3u);
    switch (primitive) {
    case Primitive::Block:
        return block();
    case Primitive::Wedge:
        return wedge();
    case Primitive::CornerWedge:
        return cornerWedge();
    case Primitive::Cylinder:
        return cylinder(0, segments);
    case Primitive::CylinderY:
        return cylinder(1, segments);
    case Primitive::Ball:
        return ball(segments);
    }
    return block();
}

const std::shared_ptr<const MeshData>& primitiveMesh(Primitive primitive)
{
    static const std::array<std::shared_ptr<const MeshData>, 6> meshes = [] {
        std::array<std::shared_ptr<const MeshData>, 6> all;
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = std::make_shared<const MeshData>(makePrimitive(static_cast<Primitive>(i)));
        return all;
    }();
    return meshes.at(static_cast<std::size_t>(primitive));
}

} // namespace lunar::render
