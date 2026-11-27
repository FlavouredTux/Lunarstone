#include "framework.hpp"

// Terrain level of detail: the right levels at the right distances, the
// ground covered exactly once, the client's distance table.

#include <lunar/Classes.hpp>
#include <lunar/terrain/TerrainLod.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace lunar;
using namespace lunar::terrain;

namespace {

// Rock up to cell 10 over cells 0 to 255 in X and Z: 8 x 8 blocks.
std::shared_ptr<Terrain> flatGround()
{
    constexpr int size = TerrainChunk::size;
    std::vector<TerrainChunk> chunks;
    for (int cz = 0; cz < 8; ++cz)
        for (int cx = 0; cx < 8; ++cx) {
            TerrainChunk chunk;
            chunk.position = {cx, 0, cz};
            constexpr int layer = size * size;
            chunk.runs.push_back({static_cast<std::uint16_t>(10 * layer), 8, 255, 0}); // rock
            chunk.runs.push_back({static_cast<std::uint16_t>(22 * layer), 0, 0, 0});   // air
            chunks.push_back(std::move(chunk));
        }
    auto terrain = std::make_shared<Terrain>();
    terrain->setChunks(std::move(chunks));
    return terrain;
}

// Which blocks the selection covers, and how often.
std::map<std::array<int, 3>, int> coverage(const TerrainLod& lod)
{
    std::map<std::array<int, 3>, int> blocks;
    for (const auto& [mesh, from, to] : lod.selection()) {
        const int span = 1 << mesh->level;
        if (mesh->solid.empty())
            continue;
        for (int y = 0; y < span; ++y)
            for (int z = 0; z < span; ++z)
                for (int x = 0; x < span; ++x)
                    ++blocks[{mesh->position[0] * span + x, mesh->position[1] * span + y,
                              mesh->position[2] * span + z}];
    }
    return blocks;
}

// Every column of blocks under the ground has something drawn over it.
std::size_t columns(const std::map<std::array<int, 3>, int>& blocks)
{
    std::set<std::array<int, 2>> found;
    for (const auto& [block, count] : blocks)
        if (block[0] >= 0 && block[0] < 8 && block[2] >= 0 && block[2] < 8)
            found.insert({block[0], block[2]});
    return found.size();
}

} // namespace

TEST(terrain_lod_distances_are_the_clients)
{
    CHECK_EQ(lodDistance(0), 0.0f);
    CHECK(std::abs(lodDistance(1) - 240.0f) < 0.01f);
    CHECK(std::abs(lodDistance(2) - 768.0f) < 0.1f);
    CHECK(std::abs(lodDistance(3) - 1929.6f) < 0.5f);
    CHECK(std::abs(lodDistance(4) - 4485.0f) < 1.0f);
}

TEST(terrain_lod_far_away_draws_the_coarsest_level)
{
    auto terrain = flatGround();
    TerrainLod lod(*terrain, {.threads = 2, .levels = 3});
    lod.settle({50000.0f, 45.0f, 50000.0f});
    CHECK(lod.settled());
    const TerrainLod::Stats stats = lod.stats();
    CHECK_EQ(stats.nodes[0], 0u);
    CHECK_EQ(stats.nodes[1], 0u);
    CHECK(stats.nodes[2] >= 4u); // 8 x 8 blocks in nodes of 4 x 4
    for (const auto& [block, count] : coverage(lod))
        CHECK_EQ(count, 1);
    CHECK_EQ(columns(coverage(lod)), 64u);
    CHECK(!lod.update({50000.0f, 45.0f, 50000.0f})); // nothing new
}

TEST(terrain_lod_keeps_full_detail_near_the_camera)
{
    auto terrain = flatGround();
    TerrainLod lod(*terrain, {.threads = 2, .levels = 3});
    lod.settle({50000.0f, 45.0f, 50000.0f});
    const std::size_t farTriangles = lod.stats().triangles;
    // Standing in the corner block. The roots draw on until the finer
    // nodes are in.
    CHECK(!lod.update({20.0f, 45.0f, 20.0f}));
    CHECK(!lod.settled());
    lod.settle({20.0f, 45.0f, 20.0f});
    const TerrainLod::Stats stats = lod.stats();
    CHECK(stats.nodes[0] > 0u);
    CHECK(stats.nodes[1] > 0u); // the far corner stays coarse
    CHECK(stats.triangles > farTriangles);
    const auto blocks = coverage(lod);
    CHECK_EQ(columns(blocks), 64u);
    for (const auto& [block, count] : blocks)
        CHECK_EQ(count, 1);
    // Full detail within lodDistance(1) of the camera.
    for (const auto& [mesh, from, to] : lod.selection())
        if (mesh->level == 0 && !mesh->solid.empty()) {
            CHECK(mesh->position[0] <= 3);
            CHECK(mesh->position[2] <= 3);
        }
    bool cornerFine = false;
    for (const auto& [mesh, from, to] : lod.selection())
        cornerFine = cornerFine || (mesh->level == 0 && mesh->position[0] == 0 && mesh->position[2] == 0);
    CHECK(cornerFine);
}

TEST(terrain_lod_shrinks_distances_to_fit_the_budget)
{
    auto terrain = flatGround();
    TerrainLod lod(*terrain, {.threads = 2, .levels = 3, .drawBudget = 1});
    for (int i = 0; i < 10; ++i)
        lod.settle({20.0f, 45.0f, 20.0f});
    CHECK(lod.stats().distanceScale < 1.0f);
}

TEST(terrain_lod_measures_across_the_ground)
{
    auto terrain = flatGround();
    TerrainLod lod(*terrain, {.threads = 2, .levels = 3});
    lod.settle({20.0f, 5000.0f, 20.0f}); // high above the corner block
    bool cornerFine = false;
    for (const auto& [mesh, from, to] : lod.selection())
        cornerFine = cornerFine || (mesh->level == 0 && mesh->position[0] == 0 && mesh->position[2] == 0);
    CHECK(cornerFine);
}

TEST(terrain_lod_switches_levels_by_dissolving)
{
    auto terrain = flatGround();
    TerrainLod lod(*terrain, {.threads = 2, .levels = 3});
    lod.settle({20.0f, 45.0f, 20.0f});
    for (const auto& [mesh, from, to] : lod.selection()) {
        CHECK_EQ(from, 0.0f);
        CHECK_EQ(to, 1.0f);
    }
    // Off to the far side: the coarse nodes come in and the fine ones go,
    // in complementary dither ranges.
    int fadingUpdates = 0;
    for (int i = 0; i < 2000; ++i) {
        lod.update({50000.0f, 45.0f, 50000.0f});
        std::set<float> in, out;
        for (const auto& [mesh, from, to] : lod.selection()) {
            if (to < 1.0f)
                in.insert(to);
            if (from > 0.0f)
                out.insert(from);
        }
        CHECK(in == out);
        if (!in.empty())
            ++fadingUpdates;
        else if (fadingUpdates > 0 && lod.settled())
            break;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // the workers meshing
    }
    CHECK(fadingUpdates >= 14); // the 15th update of a switch shows the new level whole
    for (const auto& [mesh, from, to] : lod.selection()) {
        CHECK_EQ(mesh->level, 2);
        CHECK_EQ(from, 0.0f);
        CHECK_EQ(to, 1.0f);
    }
}
