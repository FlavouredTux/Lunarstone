#include "framework.hpp"

#include <lunar/assets/MeshSimplify.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace lunar;
using namespace lunar::assets;

namespace {

// A square grid of n x n quads in the z = 0 plane, facing +z, one vertex per
// grid point unless seamAt (a column, if not 0) splits it: the vertices left
// of that column map to u in [0, 0.4], the ones right of it to [0.6, 1], and
// the column itself has a copy for each side.
MeshData grid(std::size_t n, std::size_t seamAt = 0)
{
    MeshData mesh;
    const std::size_t points = (n + 1) * (n + 1);
    std::vector<std::uint32_t> left(points);
    std::vector<std::uint32_t> right(points);
    const auto size = static_cast<float>(n);
    auto add = [&](std::size_t x, std::size_t y, float u) {
        mesh.vertices.push_back({.position = {static_cast<float>(x), static_cast<float>(y), 0.0f},
                                 .normal = {0.0f, 0.0f, 1.0f},
                                 .u = u,
                                 .v = static_cast<float>(y) / size});
        return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
    };
    for (std::size_t y = 0; y <= n; ++y)
        for (std::size_t x = 0; x <= n; ++x) {
            const std::size_t i = (y * (n + 1)) + x;
            if (seamAt == 0) {
                left[i] = right[i] = add(x, y, static_cast<float>(x) / size);
                continue;
            }
            if (x <= seamAt)
                left[i] = add(x, y, 0.4f * static_cast<float>(x) / static_cast<float>(seamAt));
            if (x >= seamAt)
                right[i] = add(x, y, 0.6f + (0.4f * static_cast<float>(x - seamAt) / static_cast<float>(n - seamAt)));
        }
    for (std::size_t y = 0; y < n; ++y)
        for (std::size_t x = 0; x < n; ++x) {
            const auto& side = seamAt > 0 && x >= seamAt ? right : left;
            auto at = [&](std::size_t dx, std::size_t dy) { return side[((y + dy) * (n + 1)) + x + dx]; };
            mesh.indices.insert(mesh.indices.end(), {at(0, 0), at(1, 0), at(1, 1), at(0, 0), at(1, 1), at(0, 1)});
        }
    return mesh;
}

std::size_t levelTriangles(const MeshData& mesh, std::size_t level)
{
    if (level == 0)
        return mesh.indices.size() / 3;
    const std::uint32_t begin = level == 1 ? 0 : mesh.lodEnds[level - 2];
    return (mesh.lodEnds[level - 1] - begin) / 3;
}

// Every coarse face references real vertices and still faces +z.
void checkLevelsFaceUp(const MeshData& mesh)
{
    for (std::size_t i = 0; i < mesh.lodIndices.size(); i += 3) {
        CHECK(mesh.lodIndices[i] < mesh.vertices.size());
        CHECK(mesh.lodIndices[i + 1] < mesh.vertices.size());
        CHECK(mesh.lodIndices[i + 2] < mesh.vertices.size());
        const Vector3 a = mesh.vertices[mesh.lodIndices[i]].position;
        const Vector3 b = mesh.vertices[mesh.lodIndices[i + 1]].position;
        const Vector3 c = mesh.vertices[mesh.lodIndices[i + 2]].position;
        const Vector3 e1 = b - a;
        const Vector3 e2 = c - a;
        CHECK(e1.x * e2.y - e1.y * e2.x > 0.0f);
    }
}

} // namespace

TEST(mesh_lods_simplify_a_flat_grid)
{
    MeshData mesh = grid(20);
    generateLods(mesh);
    CHECK_EQ(mesh.levels(), 5u);
    CHECK_EQ(mesh.lodErrors.size(), 4u);
    // Each level gets near the client's shares (50, 25, 10 and 5 percent).
    CHECK(levelTriangles(mesh, 1) <= 400u);
    CHECK(levelTriangles(mesh, 2) <= 200u);
    CHECK(levelTriangles(mesh, 3) <= 80u);
    CHECK(levelTriangles(mesh, 4) <= 40u);
    checkLevelsFaceUp(mesh);
    // A plane stays a plane, and its outline holds.
    for (float error : mesh.lodErrors)
        CHECK(error < 1e-3f);
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (std::size_t i = mesh.lodEnds[2]; i < mesh.lodEnds[3]; ++i) {
        const Vector3 p = mesh.vertices[mesh.lodIndices[i]].position;
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    CHECK_EQ(minX, 0.0f);
    CHECK_EQ(maxX, 20.0f);
    CHECK_EQ(minY, 0.0f);
    CHECK_EQ(maxY, 20.0f);
}

TEST(mesh_lods_slide_along_uv_seams)
{
    MeshData mesh = grid(20, 10);
    generateLods(mesh);
    CHECK(mesh.levels() >= 4u);
    CHECK(levelTriangles(mesh, 3) <= 80u);
    checkLevelsFaceUp(mesh);
    // No face mixes the two sides' UVs.
    for (std::size_t i = 0; i < mesh.lodIndices.size(); i += 3) {
        int leftCorners = 0;
        for (std::size_t k = 0; k < 3; ++k)
            leftCorners += mesh.vertices[mesh.lodIndices[i + k]].u <= 0.4f ? 1 : 0;
        CHECK(leftCorners % 3 == 0);
    }
}

TEST(mesh_lods_ignore_split_normals)
{
    // A gently bumpy grid, flat shaded: every face has vertices of its own.
    const MeshData smooth = grid(16);
    MeshData mesh;
    for (std::size_t i = 0; i < smooth.indices.size(); i += 3) {
        Vector3 p[3];
        for (std::size_t k = 0; k < 3; ++k) {
            p[k] = smooth.vertices[smooth.indices[i + k]].position;
            p[k].z = 0.05f * std::sin(p[k].x * 0.4f) * std::cos(p[k].y * 0.3f);
        }
        const Vector3 e1 = p[1] - p[0];
        const Vector3 e2 = p[2] - p[0];
        Vector3 normal{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        normal = normal / normal.magnitude();
        for (const Vector3& corner : p) {
            mesh.vertices.push_back(
                {.position = corner, .normal = normal, .u = corner.x / 16.0f, .v = corner.y / 16.0f});
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
        }
    }
    generateLods(mesh);
    CHECK(mesh.levels() >= 3u);
    CHECK(levelTriangles(mesh, 2) <= 128u);
    checkLevelsFaceUp(mesh);
    CHECK(mesh.lodErrors.back() < 0.2f);
}

TEST(mesh_lods_leave_small_and_leveled_meshes_alone)
{
    MeshData small = grid(4); // 32 triangles
    generateLods(small);
    CHECK_EQ(small.levels(), 1u);

    MeshData leveled = grid(20);
    leveled.lodIndices = {0, 1, 2};
    leveled.lodEnds = {3};
    generateLods(leveled);
    CHECK_EQ(leveled.levels(), 2u);
    CHECK(leveled.lodErrors.empty());
}
