#include "framework.hpp"

// Smooth terrain meshing: surfaces in the right place, closed, facing out,
// crack free across blocks, water apart from ground.

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/terrain/TerrainMesher.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using namespace lunar;
using namespace lunar::terrain;

namespace {

constexpr std::uint8_t air = 0, water = 1, grass = 2, rock = 8;

struct Cell {
    std::uint8_t material = air;
    std::uint8_t occupancy = 255;
    std::uint8_t water = 0;
};

using CellFn = std::function<Cell(int x, int y, int z)>;

// Terrain whose cells from lo to hi (exclusive) come from fn; the rest is air.
std::shared_ptr<Terrain> makeTerrain(std::array<int, 3> lo, std::array<int, 3> hi, const CellFn& fn)
{
    constexpr int size = TerrainChunk::size;
    auto chunkOf = [](int cell) { return cell >= 0 ? cell / size : -((-cell + size - 1) / size); };
    std::vector<TerrainChunk> chunks;
    for (int cy = chunkOf(lo[1]); cy <= chunkOf(hi[1] - 1); ++cy)
        for (int cz = chunkOf(lo[2]); cz <= chunkOf(hi[2] - 1); ++cz)
            for (int cx = chunkOf(lo[0]); cx <= chunkOf(hi[0] - 1); ++cx) {
                TerrainChunk chunk;
                chunk.position = {cx, cy, cz};
                bool any = false;
                for (int y = 0; y < size; ++y)
                    for (int z = 0; z < size; ++z)
                        for (int x = 0; x < size; ++x) {
                            const int gx = cx * size + x, gy = cy * size + y, gz = cz * size + z;
                            Cell cell;
                            if (gx >= lo[0] && gx < hi[0] && gy >= lo[1] && gy < hi[1] && gz >= lo[2] && gz < hi[2])
                                cell = fn(gx, gy, gz);
                            if (cell.material == air)
                                cell = {air, 0, 0};
                            any = any || cell.material != air;
                            TerrainChunk::Run run{1, cell.material, cell.occupancy, cell.water};
                            auto& runs = chunk.runs;
                            if (!runs.empty() && runs.back().material == run.material
                                && runs.back().occupancy == run.occupancy && runs.back().water == run.water)
                                ++runs.back().count;
                            else
                                runs.push_back(run);
                        }
                if (any)
                    chunks.push_back(std::move(chunk));
            }
    auto terrain = std::make_shared<Terrain>();
    terrain->setChunks(std::move(chunks));
    return terrain;
}

using Key = std::tuple<float, float, float>;

Key keyOf(const Vector3& p)
{
    return {p.x, p.y, p.z};
}

// Joins the blocks' meshes by vertex position into one triangle list.
struct Merged {
    std::vector<Vector3> positions;
    std::vector<std::array<std::size_t, 3>> triangles;
};

Merged merge(const std::vector<TerrainMeshChunk>& chunks, bool waterMesh)
{
    Merged out;
    std::map<Key, std::size_t> ids;
    for (const TerrainMeshChunk& chunk : chunks) {
        const MeshData& mesh = waterMesh ? chunk.water : chunk.solid;
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            std::array<std::size_t, 3> tri{};
            for (std::size_t k = 0; k < 3; ++k) {
                const Vector3 p = mesh.vertices[mesh.indices[i + k]].position;
                auto [it, added] = ids.try_emplace(keyOf(p), out.positions.size());
                if (added)
                    out.positions.push_back(p);
                tri[k] = it->second;
            }
            // The client's mesh has vertices of different cubes at one
            // position; triangles between them have no area.
            if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2])
                out.triangles.push_back(tri);
        }
    }
    return out;
}

// Every edge used once each way: closed, consistently wound, and no cracks
// where blocks meet (cracks would leave edges used one way only).
bool closedAndConsistent(const Merged& mesh)
{
    std::map<std::pair<std::size_t, std::size_t>, int> directed;
    for (const auto& t : mesh.triangles)
        for (std::size_t k = 0; k < 3; ++k)
            ++directed[{t[k], t[(k + 1) % 3]}];
    return std::ranges::all_of(directed, [&](const auto& entry) {
        auto it = directed.find({entry.first.second, entry.first.first});
        return entry.second == 1 && it != directed.end() && it->second == 1;
    });
}

Vector3 faceNormal(const Merged& mesh, const std::array<std::size_t, 3>& t)
{
    const Vector3 a = mesh.positions[t[0]], b = mesh.positions[t[1]], c = mesh.positions[t[2]];
    return (b - a).cross(c - a);
}

// A ball of grass around a cell corner where eight blocks meet.
std::shared_ptr<Terrain> ball(Vector3 centre, float radius)
{
    return makeTerrain({16, 16, 16}, {48, 48, 48}, [=](int x, int y, int z) {
        const Vector3 p{(static_cast<float>(x) + 0.5f) * 4.0f, (static_cast<float>(y) + 0.5f) * 4.0f,
                        (static_cast<float>(z) + 0.5f) * 4.0f};
        const float d = (p - centre).magnitude() - radius;
        const float occupancy = std::clamp(0.5f - d / 4.0f, 0.0f, 1.0f);
        if (occupancy <= 0.0f)
            return Cell{};
        return Cell{grass, static_cast<std::uint8_t>(std::max(0.0f, occupancy * 256.0f - 1.0f)), 0};
    });
}

} // namespace

// Blocks written into the 32-cell chunks, as a server streams terrain in:
// a block lands in its chunk, one written back to air takes the chunk with
// it, and deferred writes wait until they're applied.
TEST(terrain_block_writes_into_chunks)
{
    auto terrain = std::static_pointer_cast<Terrain>(ClassRegistry::find("Terrain")->factory());
    // The bottom layer of a 16-cell cube grass, the rest air.
    Terrain::Block grass{{16, 0, -16}, 16, {{256, 2, 255, 0}, {3840, 0, 0, 0}}};
    CHECK(terrain->write(grass));
    CHECK_EQ(terrain->chunks().size(), std::size_t{1});
    CHECK(terrain->voxelAt(16, 0, -16).material == Enum::Material::Grass);
    CHECK(terrain->voxelAt(31, 0, -1).material == Enum::Material::Grass);
    CHECK(terrain->voxelAt(16, 1, -16).material == Enum::Material::Air);
    CHECK(terrain->voxelAt(15, 0, -16).material == Enum::Material::Air);

    const std::uint64_t revision = terrain->revision();
    terrain->setDeferWrites(true);
    CHECK(terrain->write(Terrain::Block{{16, 0, -16}, 16, {}}));
    CHECK(terrain->hasQueuedWrites());
    CHECK_EQ(terrain->revision(), revision);
    CHECK(terrain->voxelAt(16, 0, -16).material == Enum::Material::Grass);
    terrain->applyQueuedWrites();
    CHECK(!terrain->hasQueuedWrites());
    CHECK(terrain->revision() > revision);
    CHECK(terrain->chunks().empty());

    // Runs that don't cover the cube are refused.
    CHECK(!terrain->write(Terrain::Block{{0, 0, 0}, 16, {{100, 2, 255, 0}}}));
}

// Blocks of random cells at any origin (across chunk edges, below zero) and
// of any size, written one at a time and in deferred batches over each
// other, read back cell by cell as the last write left them; the chunks
// hold runs of distinct cells covering all 32^3, and only chunks with
// something in them are there.
TEST(terrain_block_writes_match_cell_by_cell)
{
    auto terrain = std::static_pointer_cast<Terrain>(ClassRegistry::find("Terrain")->factory());
    std::uint32_t state = 7;
    auto next = [&](std::uint32_t n) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % n;
    };
    struct Cell {
        std::uint8_t material = 0, occupancy = 0, water = 0;
        bool operator==(const Cell&) const = default;
    };
    std::map<std::array<int, 3>, Cell> expected;
    for (int round = 0; round < 40; ++round) {
        Terrain::Block block;
        block.side = round % 5 == 0 ? 7 : round % 7 == 0 ? 20 : 16;
        block.origin
            = {static_cast<int>(next(80)) - 40, static_cast<int>(next(40)) - 20, static_cast<int>(next(80)) - 40};
        const int cells = block.side * block.side * block.side;
        int at = 0;
        while (at < cells) {
            const auto count = std::min<int>(cells - at, 1 + static_cast<int>(next(300)));
            const auto material = static_cast<std::uint8_t>(next(3) == 0 ? 0 : 1 + next(4));
            const TerrainChunk::Run run{static_cast<std::uint16_t>(count), material,
                                        static_cast<std::uint8_t>(material ? 1 + next(255) : 0),
                                        static_cast<std::uint8_t>(next(2) * 255)};
            for (int i = at; i < at + count; ++i) {
                const int x = i % block.side, z = (i / block.side) % block.side, y = i / (block.side * block.side);
                expected[{block.origin[0] + x, block.origin[1] + y, block.origin[2] + z}]
                    = {run.material, run.occupancy, run.water};
            }
            block.runs.push_back(run);
            at += count;
        }
        // Some wait in the queue for a round or two; what waited goes in
        // before a write that doesn't, as the order of writes says.
        const bool defer = round % 3 == 0;
        if (!defer)
            terrain->applyQueuedWrites();
        terrain->setDeferWrites(defer);
        CHECK(terrain->write(block));
        if (defer && round % 2 == 0)
            terrain->applyQueuedWrites();
    }
    terrain->setDeferWrites(false);
    terrain->applyQueuedWrites();

    std::set<std::array<int, 3>> occupiedChunks;
    auto floorDiv = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
    bool same = true;
    for (const auto& [cell, value] : expected) {
        const TerrainVoxel voxel = terrain->voxelAt(cell[0], cell[1], cell[2]);
        same = same && voxel.material == Terrain::materials.at(value.material) && voxel.occupancy == value.occupancy
               && voxel.water == value.water;
        if (value.material != 0)
            occupiedChunks.insert({floorDiv(cell[0], 32), floorDiv(cell[1], 32), floorDiv(cell[2], 32)});
    }
    CHECK(same);
    CHECK_EQ(terrain->chunks().size(), occupiedChunks.size());
    for (const TerrainChunk& chunk : terrain->chunks()) {
        std::size_t covered = 0;
        bool distinct = true;
        for (std::size_t i = 0; i < chunk.runs.size(); ++i) {
            covered += chunk.runs[i].count;
            if (i > 0) {
                const auto& a = chunk.runs[i - 1];
                const auto& b = chunk.runs[i];
                distinct = distinct && !(a.material == b.material && a.occupancy == b.occupancy && a.water == b.water);
            }
        }
        CHECK_EQ(covered, std::size_t{32 * 32 * 32});
        CHECK(distinct);
        CHECK(occupiedChunks.contains(chunk.position));
    }
}

TEST(terrain_empty_gives_nothing)
{
    Terrain terrain;
    CHECK(meshTerrain(terrain).empty());
    CHECK(meshChunk(terrain, {0, 0, 0}).empty());
    auto airOnly = makeTerrain({0, 0, 0}, {8, 8, 8}, [](int, int, int) { return Cell{}; });
    CHECK(meshTerrain(*airOnly).empty());
}

TEST(terrain_ball_is_closed_and_faces_out)
{
    const Vector3 centre{128.0f, 128.0f, 128.0f}; // cells 32: the corner of eight blocks
    auto terrain = ball(centre, 30.0f);
    auto chunks = meshTerrain(*terrain, {.threads = 3});
    CHECK(chunks.size() >= 8u);
    for (const auto& chunk : chunks)
        CHECK(chunk.water.empty());

    Merged mesh = merge(chunks, false);
    CHECK(mesh.triangles.size() > 500u);
    CHECK(closedAndConsistent(mesh));

    for (const auto& t : mesh.triangles) {
        const Vector3 mid = (mesh.positions[t[0]] + mesh.positions[t[1]] + mesh.positions[t[2]]) / 3.0f;
        CHECK(faceNormal(mesh, t).dot(mid - centre) > 0.0f);
        CHECK(std::abs((mid - centre).magnitude() - 30.0f) < 2.0f);
    }
    for (const auto& chunk : chunks)
        for (const MeshVertex& v : chunk.solid.vertices) {
            CHECK(std::abs(v.normal.magnitude() - 1.0f) < 1e-4f);
            CHECK(v.normal.dot((v.position - centre).unit()) > 0.9f);
            CHECK(v.position.x >= chunk.boundsMin.x);
            CHECK(v.position.y <= chunk.boundsMax.y);
        }
}

TEST(terrain_vertex_normals_agree_with_winding)
{
    auto terrain = ball({100.0f, 90.0f, 110.0f}, 21.0f);
    for (const auto& chunk : meshTerrain(*terrain)) {
        const MeshData& mesh = chunk.solid;
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const MeshVertex &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]],
                             &c = mesh.vertices[mesh.indices[i + 2]];
            const Vector3 n = (b.position - a.position).cross(c.position - a.position);
            CHECK(n.dot(a.normal + b.normal + c.normal) > 0.0f);
        }
    }
}

TEST(terrain_flat_ground_lands_where_the_client_puts_it)
{
    // Full grass up to cell 2 (studs 0 to 12), then a half full layer: the
    // surface is half a cell above that layer's centre, at 16 studs.
    auto terrain = makeTerrain({0, -8, 0}, {64, 4, 64},
                               [](int, int y, int) { return y < 3 ? Cell{grass, 255, 0} : Cell{grass, 127, 0}; });
    auto chunks = meshTerrain(*terrain);
    std::size_t top = 0;
    for (const auto& chunk : chunks)
        for (const MeshVertex& v : chunk.solid.vertices)
            if (v.position.x > 8.0f && v.position.x < 248.0f && v.position.z > 8.0f && v.position.z < 248.0f
                && v.normal.y > 0.0f) {
                CHECK_EQ(v.position.y, 16.0f);
                CHECK(v.normal.y > 0.999f);
                ++top;
            }
    CHECK(top > 1000u);

    // Full cells under air: the surface is on the air cell's centre, half a
    // cell above their top (14 studs, not 12).
    auto full = makeTerrain({0, -8, 0}, {64, 3, 64}, [](int, int, int) { return Cell{grass, 255, 0}; });
    for (const auto& chunk : meshTerrain(*full))
        for (const MeshVertex& v : chunk.solid.vertices)
            if (v.position.x > 8.0f && v.position.x < 248.0f && v.position.z > 8.0f && v.position.z < 248.0f
                && v.normal.y > 0.0f)
                CHECK_EQ(v.position.y, 14.0f);
}

TEST(terrain_blocks_share_border_vertices)
{
    // Rolling ground across a 2x2 block area.
    auto terrain = makeTerrain({0, 0, 0}, {64, 32, 64}, [](int x, int y, int z) {
        const float h = 10.0f + 3.0f * std::sin(static_cast<float>(x) * 0.3f) * std::cos(static_cast<float>(z) * 0.2f);
        const float occupancy = std::clamp(h - static_cast<float>(y), 0.0f, 1.0f);
        if (occupancy <= 0.0f)
            return Cell{};
        return Cell{grass, static_cast<std::uint8_t>(std::max(0.0f, occupancy * 256.0f - 1.0f)), 0};
    });
    auto chunks = meshTerrain(*terrain);
    // The surface sits at block borders x = 128 and z = 128 studs; the blocks
    // on each side meet there at the same vertex positions.
    std::map<std::array<int, 3>, std::set<Key>> byBlock;
    for (const auto& chunk : chunks)
        for (const MeshVertex& v : chunk.solid.vertices)
            byBlock[chunk.position].insert(keyOf(v.position));
    std::size_t shared = 0;
    for (const auto& [a, keysA] : byBlock)
        for (const auto& [b, keysB] : byBlock)
            if (a < b)
                for (const Key& k : keysA)
                    shared += keysB.contains(k) ? 1u : 0u;
    CHECK(shared > 30u);

    // The ground is open at the region's edges; across block borders every
    // inner edge must still pair up. (Vertices of different cubes can land
    // on one point, so an edge between merged positions can serve two
    // triangles; the client's mesh does that too.)
    Merged mesh = merge(chunks, false);
    std::map<std::pair<std::size_t, std::size_t>, int> directed;
    for (const auto& t : mesh.triangles)
        for (std::size_t k = 0; k < 3; ++k)
            ++directed[{t[k], t[(k + 1) % 3]}];
    for (const auto& [edge, count] : directed) {
        const Vector3 a = mesh.positions[edge.first], b = mesh.positions[edge.second];
        const bool inner = std::min(a.x, b.x) > 4.0f && std::max(a.x, b.x) < 252.0f && std::min(a.z, b.z) > 4.0f
                           && std::max(a.z, b.z) < 252.0f && std::min(a.y, b.y) > 0.0f;
        if (inner)
            CHECK(directed.contains({edge.second, edge.first}));
    }
}

TEST(terrain_water_is_its_own_mesh)
{
    // Grass up to 8 studs, water on top up to 20.
    auto terrain = makeTerrain({0, 0, 0}, {40, 5, 40},
                               [](int, int y, int) { return y < 2 ? Cell{grass, 255, 0} : Cell{water, 255, 0}; });
    terrain->setWaterColor(Color3::fromRGB(10, 20, 200));
    auto chunks = meshTerrain(*terrain);
    std::size_t solidTop = 0, waterTop = 0;
    for (const auto& chunk : chunks) {
        for (const MeshVertex& v : chunk.solid.vertices)
            if (v.normal.y > 0.999f && v.position.x > 8.0f && v.position.x < 150.0f) {
                CHECK_EQ(v.position.y, 10.0f); // water is air to the ground
                ++solidTop;
            }
        for (const MeshVertex& v : chunk.water.vertices) {
            CHECK(v.color == Color3::fromRGB(10, 20, 200));
            // The water closes against the ground inside it, under the
            // ground's surface (the client turns solid cells beside water
            // that aren't buried in ground into water or air).
            if (v.normal.y < -0.5f)
                CHECK(v.position.y < 10.0f);
            if (v.position.x > 8.0f && v.position.x < 150.0f && v.position.z > 8.0f && v.position.z < 150.0f
                && v.normal.y > 0.5f) {
                CHECK_EQ(v.position.y, 20.0f);
                ++waterTop;
            }
        }
    }
    CHECK(solidTop > 100u);
    CHECK(waterTop > 100u);

    // Water in a partly solid cell counts too.
    auto wet = makeTerrain({0, 0, 0}, {8, 2, 8}, [](int, int y, int) {
        return y == 0 ? Cell{grass, 255, 0} : Cell{grass, 63, 191}; // a quarter grass, three quarters water
    });
    bool anyWater = false;
    for (const auto& chunk : meshTerrain(*wet))
        anyWater = anyWater || !chunk.water.empty();
    CHECK(anyWater);

    // Water occupancy counts like solid's, then the surface drops half a
    // cell: the big battle royale map's sea, stored all but empty, tops out
    // just above its top cell's floor.
    auto sea = makeTerrain({0, 0, 0}, {40, 5, 40},
                           [](int, int y, int) { return y < 2 ? Cell{grass, 255, 0} : Cell{water, 0, 0}; });
    std::size_t seaTop = 0;
    for (const auto& chunk : meshTerrain(*sea))
        for (const MeshVertex& v : chunk.water.vertices)
            if (v.position.x > 8.0f && v.position.x < 150.0f && v.position.z > 8.0f && v.position.z < 150.0f
                && v.position.y > 12.0f) {
                CHECK_EQ(v.position.y, 16.015625f);
                ++seaTop;
            }
    CHECK(seaTop > 100u);
}

TEST(terrain_vertices_take_material_colours)
{
    // Grass for x < 16, rock beyond, flat.
    auto terrain
        = makeTerrain({0, 0, 0}, {32, 2, 8}, [](int x, int, int) { return Cell{x < 16 ? grass : rock, 255, 0}; });
    terrain->setMaterialColor(Enum::Material::Rock, Color3::fromRGB(200, 0, 0));
    const Color3 grassColor = terrain->getMaterialColor(Enum::Material::Grass);
    const Color3 rockColor = Color3::fromRGB(200, 0, 0);
    // Each vertex has one material, the client's (the shader blends across
    // triangles), so its colour is that material's.
    bool sawGrass = false, sawRock = false;
    for (const auto& chunk : meshTerrain(*terrain))
        for (const MeshVertex& v : chunk.solid.vertices) {
            if (v.normal.y < 0.999f)
                continue;
            CHECK(v.color == grassColor || v.color == rockColor);
            if (v.position.x < 56.0f) {
                CHECK(v.color == grassColor);
                sawGrass = true;
            } else if (v.position.x > 72.0f) {
                CHECK(v.color == rockColor);
                sawRock = true;
            }
        }
    CHECK(sawGrass);
    CHECK(sawRock);
}

TEST(terrain_remesh_region_matches_full_mesh)
{
    auto terrain = ball({128.0f, 128.0f, 128.0f}, 30.0f);
    auto all = meshTerrain(*terrain);
    // Cell 32 is block 1's first and within block 0's read range.
    auto region = remeshRegion(*terrain, {32, 32, 32}, {32, 32, 32});
    CHECK_EQ(region.size(), 8u);
    for (const auto& chunk : region) {
        auto it = std::ranges::find(all, chunk.position, &TerrainMeshChunk::position);
        CHECK(it != all.end());
        CHECK(it->solid.indices == chunk.solid.indices);
        CHECK_EQ(it->solid.vertices.size(), chunk.solid.vertices.size());
        for (std::size_t i = 0; i < chunk.solid.vertices.size(); ++i)
            CHECK(it->solid.vertices[i].position == chunk.solid.vertices[i].position);
    }
    // Deep inside a block only that block can change.
    CHECK_EQ(remeshRegion(*terrain, {40, 40, 40}, {50, 50, 50}).size(), 1u);
    CHECK_EQ(remeshRegion(*terrain, {1, 40, 40}, {1, 40, 40}).size(), 2u);

    const MeshStats stats = measure(all);
    CHECK_EQ(stats.chunks, all.size());
    CHECK(stats.solidTriangles > 0u);
    CHECK_EQ(stats.waterTriangles, 0u);
}

TEST(terrain_vertices_carry_material_weights)
{
    // Flat ground, grass west of x = 16 cells and rock east of it.
    auto terrain
        = makeTerrain({0, 0, 0}, {32, 4, 32}, [](int x, int, int) { return Cell{x < 16 ? grass : rock, 255, 0}; });
    const TerrainMeshChunk chunk = meshChunk(*terrain, {0, 0, 0});
    CHECK(!chunk.solid.empty());
    CHECK_EQ(chunk.paletteSize, std::uint8_t{2});
    CHECK_EQ(chunk.palette[0], grass); // ordered by material index
    CHECK_EQ(chunk.palette[1], rock);

    // One material per vertex, all its weight on one slot.
    for (const MeshVertex& v : chunk.solid.vertices) {
        CHECK_EQ(std::ranges::count(v.materialWeights, std::uint8_t{255}), 1);
        CHECK_EQ(std::ranges::count(v.materialWeights, std::uint8_t{0}), 7);
        const float cellX = v.position.x / 4.0f;
        if (v.position.y < 15.0f || cellX < 2.0f || cellX > 30.0f)
            continue; // the top only, away from the block's edges
        if (cellX < 14.0f)
            CHECK_EQ(v.materialWeights[0], std::uint8_t{255});
        else if (cellX > 18.0f)
            CHECK_EQ(v.materialWeights[1], std::uint8_t{255});
    }
    // The boundary blends across the triangles between the two.
    bool sawMix = false;
    for (std::size_t i = 0; i + 2 < chunk.solid.indices.size(); i += 3) {
        const auto& a = chunk.solid.vertices[chunk.solid.indices[i]].materialWeights;
        const auto& b = chunk.solid.vertices[chunk.solid.indices[i + 1]].materialWeights;
        sawMix = sawMix || a != b;
    }
    CHECK(sawMix);
}

TEST(terrain_palette_keeps_the_eight_biggest_materials)
{
    // Ten materials in stripes, the widest ones first.
    auto terrain = makeTerrain({0, 0, 0}, {32, 4, 32}, [](int x, int, int) {
        static constexpr std::array<std::uint8_t, 10> stripes{2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        return Cell{stripes[static_cast<std::size_t>(std::min(x / 3, 9))], 255, 0};
    });
    const TerrainMeshChunk chunk = meshChunk(*terrain, {0, 0, 0});
    CHECK_EQ(chunk.paletteSize, std::uint8_t{TerrainMeshChunk::paletteCapacity});
    for (const MeshVertex& v : chunk.solid.vertices) {
        int sum = 0;
        for (std::uint8_t w : v.materialWeights)
            sum += w;
        CHECK_EQ(sum, 255);
    }
}

TEST(terrain_node_level_zero_is_the_block)
{
    auto terrain = ball({100.0f, 90.0f, 110.0f}, 21.0f);
    NodeMesher mesher(*terrain);
    for (const auto& chunk : meshTerrain(*terrain)) {
        const TerrainMeshChunk node = mesher.mesh(0, chunk.position, false);
        CHECK_EQ(node.solid.vertices.size(), chunk.solid.vertices.size());
        CHECK(node.solid.indices == chunk.solid.indices);
        CHECK(node.palette == chunk.palette);
    }
}

TEST(terrain_coarse_ground_averages_the_cells)
{
    // Grass up to cell 40 over a level 1 node (64 cells a side); its 8 stud
    // cells average two layers each, so 40 full layers make 20 coarse ones.
    // The client's mesher shifts coarse vertices by the extra half cell, so
    // the surface stays where full detail has it, 162 studs. Sand only in a
    // corner, so grass wins.
    auto terrain = makeTerrain({0, 0, 0}, {64, 41, 64}, [](int x, int y, int z) {
        if (y == 40)
            return Cell{};
        return Cell{x < 1 && z < 1 ? std::uint8_t{6} : grass, 255, 0};
    });
    NodeMesher mesher(*terrain);
    const TerrainMeshChunk node = mesher.mesh(1, {0, 0, 0}, false);
    CHECK_EQ(node.level, 1);
    CHECK_EQ(node.paletteSize, 1);
    CHECK_EQ(node.palette[0], grass);
    std::size_t top = 0;
    for (const MeshVertex& v : node.solid.vertices)
        if (v.position.x > 16.0f && v.position.x < 240.0f && v.position.z > 16.0f && v.position.z < 240.0f
            && v.normal.y > 0.0f) {
            CHECK_EQ(v.position.y, 162.0f);
            ++top;
        }
    // 32 x 32 coarse cells: about a quarter of the vertices of the full block.
    CHECK(top > 500u);
    CHECK(top < 1100u);
    CHECK(node.boundsMax.x > 250.0f);
}

TEST(terrain_coarse_neighbours_share_border_vertices)
{
    auto terrain = makeTerrain({0, 0, 0}, {128, 64, 64}, [](int x, int y, int z) {
        const float h = 20.0f + 6.0f * std::sin(static_cast<float>(x) * 0.1f) * std::cos(static_cast<float>(z) * 0.07f);
        const float occupancy = std::clamp(h - static_cast<float>(y), 0.0f, 1.0f);
        if (occupancy <= 0.0f)
            return Cell{};
        return Cell{grass, static_cast<std::uint8_t>(std::max(0.0f, occupancy * 256.0f - 1.0f)), 0};
    });
    NodeMesher mesher(*terrain);
    std::vector<TerrainMeshChunk> nodes{mesher.mesh(1, {0, 0, 0}, false), mesher.mesh(1, {1, 0, 0}, false)};
    std::set<Key> left, right;
    for (const MeshVertex& v : nodes[0].solid.vertices)
        left.insert(keyOf(v.position));
    for (const MeshVertex& v : nodes[1].solid.vertices)
        right.insert(keyOf(v.position));
    std::size_t shared = 0;
    for (const Key& k : left)
        shared += right.contains(k) ? 1u : 0u;
    CHECK(shared > 20u);

    // Inner edges pair up across the border at x = 256 studs.
    Merged mesh = merge(nodes, false);
    std::map<std::pair<std::size_t, std::size_t>, int> directed;
    for (const auto& t : mesh.triangles)
        for (std::size_t k = 0; k < 3; ++k)
            ++directed[{t[k], t[(k + 1) % 3]}];
    for (const auto& [edge, count] : directed) {
        CHECK_EQ(count, 1);
        const Vector3 a = mesh.positions[edge.first], b = mesh.positions[edge.second];
        if (std::min(a.x, b.x) > 8.0f && std::max(a.x, b.x) < 500.0f && std::min(a.z, b.z) > 8.0f
            && std::max(a.z, b.z) < 248.0f)
            CHECK(directed.contains({edge.second, edge.first}));
    }
}

TEST(terrain_skirts_hang_from_the_border_into_the_ground)
{
    // Ground running on past the block on every side, so its mesh is open there.
    auto terrain = makeTerrain({-8, -8, -8}, {40, 11, 40},
                               [](int, int y, int) { return y < 10 ? Cell{grass, 255, 0} : Cell{}; });
    NodeMesher mesher(*terrain);
    const TerrainMeshChunk plain = mesher.mesh(0, {0, 0, 0}, false);
    const TerrainMeshChunk skirted = mesher.mesh(0, {0, 0, 0}, true);
    const std::size_t vertices = plain.solid.vertices.size(), indices = plain.solid.indices.size();
    CHECK(skirted.solid.indices.size() > indices);
    CHECK(std::equal(plain.solid.indices.begin(), plain.solid.indices.end(), skirted.solid.indices.begin()));
    // Each skirt vertex is a copy of a surface vertex, 8 studs (two cells) in.
    for (std::size_t i = vertices; i < skirted.solid.vertices.size(); ++i) {
        const MeshVertex& v = skirted.solid.vertices[i];
        const bool found = std::ranges::any_of(plain.solid.vertices, [&](const MeshVertex& s) {
            return (s.position - s.normal * 8.0f - v.position).magnitude() < 1e-4f
                   && s.materialWeights == v.materialWeights;
        });
        CHECK(found);
    }
    // Skirt faces point away from the block, out through its sides.
    const Vector3 middle{64.0f, 40.0f, 64.0f};
    std::size_t outward = 0;
    for (std::size_t i = indices; i < skirted.solid.indices.size(); i += 3) {
        const Vector3 a = skirted.solid.vertices[skirted.solid.indices[i]].position,
                      b = skirted.solid.vertices[skirted.solid.indices[i + 1]].position,
                      c = skirted.solid.vertices[skirted.solid.indices[i + 2]].position;
        const Vector3 n = (b - a).cross(c - a);
        Vector3 out = (a + b + c) / 3.0f - middle;
        out.y = 0.0f;
        outward += n.dot(out) > 0.0f ? 1u : 0u;
    }
    CHECK_EQ(outward, (skirted.solid.indices.size() - indices) / 3);
}

TEST(terrain_coarse_material_is_the_top_layers)
{
    // One layer of grass over 39 of rock: by volume rock, but grass shows.
    auto terrain = makeTerrain({0, 0, 0}, {64, 41, 64},
                               [](int, int y, int) { return y == 40 ? Cell{} : Cell{y == 39 ? grass : rock, 255, 0}; });
    for (int level = 1; level <= 3; ++level) {
        const TerrainMeshChunk node = NodeMesher(*terrain).mesh(level, {0, 0, 0}, false);
        std::size_t grassy = 0, up = 0;
        for (const MeshVertex& v : node.solid.vertices)
            if (v.normal.y > 0.99f) {
                ++up;
                grassy += node.palette[static_cast<std::size_t>(std::ranges::max_element(v.materialWeights)
                                                                - v.materialWeights.begin())]
                                  == grass
                              ? 1u
                              : 0u;
            }
        CHECK(up > 0u);
        CHECK_EQ(grassy, up);
    }
}

TEST(terrain_ground_painted_under_half_full_is_still_ground)
{
    // A weak brush leaves whole layers under half full (the battle royale
    // map's hills): they are still ground, with a closed top surface, and a
    // ray down through the middle hits it.
    auto terrain
        = makeTerrain({0, -8, 0}, {64, 6, 64}, [](int, int y, int) { return y < 5 ? Cell{grass, 120, 0} : Cell{}; });
    auto chunks = meshTerrain(*terrain);
    std::size_t top = 0;
    for (const auto& chunk : chunks)
        for (const MeshVertex& v : chunk.solid.vertices)
            if (v.position.x > 8.0f && v.position.x < 248.0f && v.position.z > 8.0f && v.position.z < 248.0f
                && v.normal.y > 0.99f) {
                // Every solid cell is solid whatever its occupancy; the top
                // layer's puts the surface 121/256 of a cell above its centre.
                CHECK_EQ(v.position.y, (4.5f + 121.0f / 256.0f) * 4.0f);
                ++top;
            }
    CHECK(top > 1000u);
}
