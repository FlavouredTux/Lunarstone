#include "framework.hpp"

// The client's smooth terrain mesher, pinned to the client: every expected
// vertex here comes from running RobloxPlayer 0.742's own mesher
// (sub_105159fb6) under an x86-64 emulator on the same 8^3 cells, with the
// production flags and the 2022 material table. Positions are in cells and
// must match to the bit.

#include <lunar/terrain/SmoothMesher.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <vector>

using namespace lunar;
using namespace lunar::terrain;

namespace {

constexpr std::uint8_t water = 1, grass = 2, concrete = 4, brick = 5, rock = 8;

struct Pinned {
    float x, y, z;
    int material;
};

using Fill = std::function<SmoothCell(int x, int y, int z)>;

std::vector<Pinned> meshBox(const Fill& fill, std::array<int, 3> origin, SmoothPass pass)
{
    constexpr int side = 8;
    std::vector<SmoothCell> cells;
    cells.reserve(std::size_t{side} * side * side);
    for (int y = 0; y < side; ++y)
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x)
                cells.push_back(fill(x, y, z)); // x fastest, then z, then y
    SmoothMesher mesher;
    SmoothMesh mesh;
    mesher.mesh({.size = {side, side, side}, .cells = cells, .origin = origin, .lod = 0}, pass, materialDeforms(true),
                mesh);
    std::vector<Pinned> out;
    out.reserve(mesh.vertices.size());
    for (const SmoothVertex& v : mesh.vertices)
        out.push_back({v.position.x, v.position.y, v.position.z, v.material});
    return out;
}

// The same set of vertices, bit for bit (the client makes no duplicates in these).
bool sameVertices(std::vector<Pinned> mine, std::vector<Pinned> client)
{
    auto key = [](const Pinned& p) { return std::tuple{p.x, p.y, p.z, p.material}; };
    std::ranges::sort(mine, {}, key);
    std::ranges::sort(client, {}, key);
    const auto end = std::ranges::unique(mine, {}, key);
    mine.erase(end.begin(), end.end());
    if (mine.size() != client.size())
        return false;
    for (std::size_t i = 0; i < mine.size(); ++i)
        for (auto field : {&Pinned::x, &Pinned::y, &Pinned::z})
            if (std::bit_cast<std::uint32_t>(mine[i].*field) != std::bit_cast<std::uint32_t>(client[i].*field)
                || mine[i].material != client[i].material)
                return false;
    return true;
}

} // namespace

TEST(smooth_mesher_lone_full_cell_is_a_small_cube)
{
    // A lone full cell: a cube two thirds of a cell wide around its centre (edges crossed a whole cell out from the
    // centre, averaged over three edges per cube).
    static const std::vector<Pinned> client{
        {3.16666675f, 3.16666675f, 3.16666675f, 2}, {3.16666675f, 3.16666675f, 3.83333325f, 2},
        {3.16666675f, 3.83333325f, 3.16666675f, 2}, {3.16666675f, 3.83333325f, 3.83333325f, 2},
        {3.83333325f, 3.16666675f, 3.16666675f, 2}, {3.83333325f, 3.16666675f, 3.83333325f, 2},
        {3.83333325f, 3.83333325f, 3.16666675f, 2}, {3.83333325f, 3.83333325f, 3.83333325f, 2},
    };
    CHECK(sameVertices(
        meshBox(
            [](int x, int y, int z) { return x == 3 && y == 3 && z == 3 ? SmoothCell{grass, 255, 0} : SmoothCell{}; },
            {0, 0, 0}, SmoothPass::Solid),
        client));
}

TEST(smooth_mesher_lone_half_cell_is_half_as_big)
{
    // Half full: the crossings come half as far out.
    static const std::vector<Pinned> client{
        {3.33333349f, 3.33333349f, 3.33333349f, 2}, {3.33333349f, 3.33333349f, 3.66666675f, 2},
        {3.33333349f, 3.66666675f, 3.33333349f, 2}, {3.33333349f, 3.66666675f, 3.66666675f, 2},
        {3.66666675f, 3.33333349f, 3.33333349f, 2}, {3.66666675f, 3.33333349f, 3.66666675f, 2},
        {3.66666675f, 3.66666675f, 3.33333349f, 2}, {3.66666675f, 3.66666675f, 3.66666675f, 2},
    };
    CHECK(sameVertices(
        meshBox(
            [](int x, int y, int z) { return x == 3 && y == 3 && z == 3 ? SmoothCell{grass, 127, 0} : SmoothCell{}; },
            {0, 0, 0}, SmoothPass::Solid),
        client));
}

TEST(smooth_mesher_ground_sits_occupancy_above_the_top_centre)
{
    // Full ground to cell 2, a quarter full layer at 3: the surface is a quarter cell above that layer's centre (3.5 +
    // 0.25), not on any cell face.
    static const std::vector<Pinned> client{
        {2.0f, 3.75f, 2.0f, 2}, {2.0f, 3.75f, 3.0f, 2}, {2.0f, 3.75f, 4.0f, 2}, {2.0f, 3.75f, 5.0f, 2},
        {2.0f, 3.75f, 6.0f, 2}, {3.0f, 3.75f, 2.0f, 2}, {3.0f, 3.75f, 3.0f, 2}, {3.0f, 3.75f, 4.0f, 2},
        {3.0f, 3.75f, 5.0f, 2}, {3.0f, 3.75f, 6.0f, 2}, {4.0f, 3.75f, 2.0f, 2}, {4.0f, 3.75f, 3.0f, 2},
        {4.0f, 3.75f, 4.0f, 2}, {4.0f, 3.75f, 5.0f, 2}, {4.0f, 3.75f, 6.0f, 2}, {5.0f, 3.75f, 2.0f, 2},
        {5.0f, 3.75f, 3.0f, 2}, {5.0f, 3.75f, 4.0f, 2}, {5.0f, 3.75f, 5.0f, 2}, {5.0f, 3.75f, 6.0f, 2},
        {6.0f, 3.75f, 2.0f, 2}, {6.0f, 3.75f, 3.0f, 2}, {6.0f, 3.75f, 4.0f, 2}, {6.0f, 3.75f, 5.0f, 2},
        {6.0f, 3.75f, 6.0f, 2},
    };
    CHECK(sameVertices(meshBox(
                           [](int, int y, int) {
                               return y < 3    ? SmoothCell{grass, 255, 0}
                                      : y == 3 ? SmoothCell{grass, 63, 0}
                                               : SmoothCell{};
                           },
                           {0, 0, 0}, SmoothPass::Solid),
                       client));
}

TEST(smooth_mesher_slope_follows_the_columns)
{
    // Columns stepping up every two cells with alternating top occupancies.
    static const std::vector<Pinned> client{
        {1.875f, 3.375f, 2.0f, 2}, {1.875f, 3.375f, 3.0f, 2}, {1.875f, 3.375f, 4.0f, 2}, {1.875f, 3.375f, 5.0f, 2},
        {1.875f, 3.375f, 6.0f, 2}, {2.375f, 3.625f, 2.0f, 2}, {2.375f, 3.625f, 3.0f, 2}, {2.375f, 3.625f, 4.0f, 2},
        {2.375f, 3.625f, 5.0f, 2}, {2.375f, 3.625f, 6.0f, 2}, {3.0f, 4.0f, 2.0f, 2},     {3.0f, 4.0f, 3.0f, 2},
        {3.0f, 4.0f, 4.0f, 2},     {3.0f, 4.0f, 5.0f, 2},     {3.0f, 4.0f, 6.0f, 2},     {3.875f, 4.375f, 2.0f, 2},
        {3.875f, 4.375f, 3.0f, 2}, {3.875f, 4.375f, 4.0f, 2}, {3.875f, 4.375f, 5.0f, 2}, {3.875f, 4.375f, 6.0f, 2},
        {4.375f, 4.625f, 2.0f, 2}, {4.375f, 4.625f, 3.0f, 2}, {4.375f, 4.625f, 4.0f, 2}, {4.375f, 4.625f, 5.0f, 2},
        {4.375f, 4.625f, 6.0f, 2}, {5.0f, 5.0f, 2.0f, 2},     {5.0f, 5.0f, 3.0f, 2},     {5.0f, 5.0f, 4.0f, 2},
        {5.0f, 5.0f, 5.0f, 2},     {5.0f, 5.0f, 6.0f, 2},     {5.875f, 5.375f, 2.0f, 2}, {5.875f, 5.375f, 3.0f, 2},
        {5.875f, 5.375f, 4.0f, 2}, {5.875f, 5.375f, 5.0f, 2}, {5.875f, 5.375f, 6.0f, 2}, {6.375f, 5.625f, 2.0f, 2},
        {6.375f, 5.625f, 3.0f, 2}, {6.375f, 5.625f, 4.0f, 2}, {6.375f, 5.625f, 5.0f, 2}, {6.375f, 5.625f, 6.0f, 2},
    };
    CHECK(sameVertices(meshBox(
                           [](int x, int y, int) {
                               const int h = 2 + x / 2;
                               return y < h    ? SmoothCell{grass, 255, 0}
                                      : y == h ? SmoothCell{grass, static_cast<std::uint8_t>((x % 2) * 128 + 63), 0}
                                               : SmoothCell{};
                           },
                           {0, 0, 0}, SmoothPass::Solid),
                       client));
}

TEST(smooth_mesher_block_corner_is_bevelled)
{
    // The corner of a full block: flat faces half a cell out, bevelled edges and corner.
    static const std::vector<Pinned> client{
        {2.0f, 2.0f, 4.5f, 2},
        {2.0f, 3.0f, 4.5f, 2},
        {2.0f, 4.0f, 4.0f, 2},
        {2.0f, 4.5f, 2.0f, 2},
        {2.0f, 4.5f, 3.0f, 2},
        {3.0f, 2.0f, 4.5f, 2},
        {3.0f, 3.0f, 4.5f, 2},
        {3.0f, 4.0f, 4.0f, 2},
        {3.0f, 4.5f, 2.0f, 2},
        {3.0f, 4.5f, 3.0f, 2},
        {3.83333325f, 3.83333325f, 3.83333325f, 2},
        {4.0f, 2.0f, 4.0f, 2},
        {4.0f, 3.0f, 4.0f, 2},
        {4.0f, 4.0f, 2.0f, 2},
        {4.0f, 4.0f, 3.0f, 2},
        {4.5f, 2.0f, 2.0f, 2},
        {4.5f, 2.0f, 3.0f, 2},
        {4.5f, 3.0f, 2.0f, 2},
        {4.5f, 3.0f, 3.0f, 2},
    };
    CHECK(sameVertices(
        meshBox([](int x, int y, int z) { return x < 4 && y < 4 && z < 4 ? SmoothCell{grass, 255, 0} : SmoothCell{}; },
                {0, 0, 0}, SmoothPass::Solid),
        client));
}

TEST(smooth_mesher_rock_jitters_by_the_cube_hash)
{
    // Grass meeting rock, away from the origin: vertices take the material of most of their cube's solid corners, and
    // rock moves each vertex by up to 0.299 cells per axis by a hash of the cube's world position (2022 materials).
    static const std::vector<Pinned> client{
        {12.0f, -16.5f, 32.0f, 2},
        {12.0f, -16.5f, 33.0f, 2},
        {12.0f, -16.5f, 34.0f, 2},
        {12.0f, -16.5f, 35.0f, 2},
        {12.0f, -16.5f, 36.0f, 2},
        {13.0f, -16.5f, 32.0f, 2},
        {13.0f, -16.5f, 33.0f, 2},
        {13.0f, -16.5f, 34.0f, 2},
        {13.0f, -16.5f, 35.0f, 2},
        {13.0f, -16.5f, 36.0f, 2},
        {14.0f, -16.5f, 32.0f, 2},
        {14.0f, -16.5f, 33.0f, 2},
        {14.0f, -16.5f, 34.0f, 2},
        {14.0f, -16.5f, 35.0f, 2},
        {14.0f, -16.5f, 36.0f, 2},
        {14.7596273f, -16.5f, 35.8932991f, 8},
        {14.9636507f, -16.5f, 35.0855942f, 8},
        {15.0621452f, -16.5480747f, 33.8370171f, 8},
        {15.0973215f, -16.5f, 31.9026775f, 8},
        {15.1981611f, -16.6606388f, 33.2614784f, 8},
        {15.7549372f, -16.5809059f, 35.9683418f, 8},
        {15.8557768f, -16.5035172f, 34.19347f, 8},
        {15.9613056f, -16.5691795f, 35.2098846f, 8},
        {16.0785599f, -16.5f, 33.0551109f, 8},
        {16.1911259f, -16.5f, 31.8698463f, 8},
    };
    CHECK(sameVertices(
        meshBox([](int x, int y, int) { return y < 3 ? SmoothCell{x < 4 ? grass : rock, 255, 0} : SmoothCell{}; },
                {10, -20, 30}, SmoothPass::Solid),
        client));
}

TEST(smooth_mesher_brick_snaps_to_cell_corners)
{
    // Brick cubifies: every vertex moves to its cube's centre, a cell corner, so a block of brick is a box on the cell
    // faces.
    static const std::vector<Pinned> client{
        {2.0f, 2.0f, 2.0f, 5}, {2.0f, 2.0f, 3.0f, 5}, {2.0f, 2.0f, 4.0f, 5}, {2.0f, 2.0f, 5.0f, 5},
        {2.0f, 3.0f, 2.0f, 5}, {2.0f, 3.0f, 3.0f, 5}, {2.0f, 3.0f, 4.0f, 5}, {2.0f, 3.0f, 5.0f, 5},
        {2.0f, 4.0f, 2.0f, 5}, {2.0f, 4.0f, 3.0f, 5}, {2.0f, 4.0f, 4.0f, 5}, {2.0f, 4.0f, 5.0f, 5},
        {3.0f, 2.0f, 2.0f, 5}, {3.0f, 2.0f, 3.0f, 5}, {3.0f, 2.0f, 4.0f, 5}, {3.0f, 2.0f, 5.0f, 5},
        {3.0f, 3.0f, 2.0f, 5}, {3.0f, 3.0f, 5.0f, 5}, {3.0f, 4.0f, 2.0f, 5}, {3.0f, 4.0f, 3.0f, 5},
        {3.0f, 4.0f, 4.0f, 5}, {3.0f, 4.0f, 5.0f, 5}, {4.0f, 2.0f, 2.0f, 5}, {4.0f, 2.0f, 3.0f, 5},
        {4.0f, 2.0f, 4.0f, 5}, {4.0f, 2.0f, 5.0f, 5}, {4.0f, 3.0f, 2.0f, 5}, {4.0f, 3.0f, 5.0f, 5},
        {4.0f, 4.0f, 2.0f, 5}, {4.0f, 4.0f, 3.0f, 5}, {4.0f, 4.0f, 4.0f, 5}, {4.0f, 4.0f, 5.0f, 5},
        {5.0f, 2.0f, 2.0f, 5}, {5.0f, 2.0f, 3.0f, 5}, {5.0f, 2.0f, 4.0f, 5}, {5.0f, 2.0f, 5.0f, 5},
        {5.0f, 3.0f, 2.0f, 5}, {5.0f, 3.0f, 3.0f, 5}, {5.0f, 3.0f, 4.0f, 5}, {5.0f, 3.0f, 5.0f, 5},
        {5.0f, 4.0f, 2.0f, 5}, {5.0f, 4.0f, 3.0f, 5}, {5.0f, 4.0f, 4.0f, 5}, {5.0f, 4.0f, 5.0f, 5},
    };
    CHECK(sameVertices(meshBox(
                           [](int x, int y, int z) {
                               return x >= 2 && x < 5 && y >= 2 && y < 4 && z >= 2 && z < 5 ? SmoothCell{brick, 255, 0}
                                                                                            : SmoothCell{};
                           },
                           {0, 0, 0}, SmoothPass::Solid),
                       client));
}

TEST(smooth_mesher_concrete_quantizes)
{
    // Concrete quantizes to whole cells around the cube centre: the 0.39 layer rounds to the cell face.
    static const std::vector<Pinned> client{
        {2.0f, 4.0f, 2.0f, 4}, {2.0f, 4.0f, 3.0f, 4}, {2.0f, 4.0f, 4.0f, 4}, {2.0f, 4.0f, 5.0f, 4},
        {2.0f, 4.0f, 6.0f, 4}, {3.0f, 4.0f, 2.0f, 4}, {3.0f, 4.0f, 3.0f, 4}, {3.0f, 4.0f, 4.0f, 4},
        {3.0f, 4.0f, 5.0f, 4}, {3.0f, 4.0f, 6.0f, 4}, {4.0f, 4.0f, 2.0f, 4}, {4.0f, 4.0f, 3.0f, 4},
        {4.0f, 4.0f, 4.0f, 4}, {4.0f, 4.0f, 5.0f, 4}, {4.0f, 4.0f, 6.0f, 4}, {5.0f, 4.0f, 2.0f, 4},
        {5.0f, 4.0f, 3.0f, 4}, {5.0f, 4.0f, 4.0f, 4}, {5.0f, 4.0f, 5.0f, 4}, {5.0f, 4.0f, 6.0f, 4},
        {6.0f, 4.0f, 2.0f, 4}, {6.0f, 4.0f, 3.0f, 4}, {6.0f, 4.0f, 4.0f, 4}, {6.0f, 4.0f, 5.0f, 4},
        {6.0f, 4.0f, 6.0f, 4},
    };
    CHECK(sameVertices(meshBox(
                           [](int, int y, int) {
                               return y < 3    ? SmoothCell{concrete, 255, 0}
                                      : y == 3 ? SmoothCell{concrete, 100, 0}
                                               : SmoothCell{};
                           },
                           {0, 0, 0}, SmoothPass::Solid),
                       client));
}

TEST(smooth_mesher_water_surface_drops_half_a_cell)
{
    // Water over ground: the surface is half a cell below where solid would be, on the top face of the top water cell.
    static const std::vector<Pinned> client{
        {2.0f, 5.0f, 2.0f, 1}, {2.0f, 5.0f, 3.0f, 1}, {2.0f, 5.0f, 4.0f, 1}, {2.0f, 5.0f, 5.0f, 1},
        {2.0f, 5.0f, 6.0f, 1}, {3.0f, 5.0f, 2.0f, 1}, {3.0f, 5.0f, 3.0f, 1}, {3.0f, 5.0f, 4.0f, 1},
        {3.0f, 5.0f, 5.0f, 1}, {3.0f, 5.0f, 6.0f, 1}, {4.0f, 5.0f, 2.0f, 1}, {4.0f, 5.0f, 3.0f, 1},
        {4.0f, 5.0f, 4.0f, 1}, {4.0f, 5.0f, 5.0f, 1}, {4.0f, 5.0f, 6.0f, 1}, {5.0f, 5.0f, 2.0f, 1},
        {5.0f, 5.0f, 3.0f, 1}, {5.0f, 5.0f, 4.0f, 1}, {5.0f, 5.0f, 5.0f, 1}, {5.0f, 5.0f, 6.0f, 1},
        {6.0f, 5.0f, 2.0f, 1}, {6.0f, 5.0f, 3.0f, 1}, {6.0f, 5.0f, 4.0f, 1}, {6.0f, 5.0f, 5.0f, 1},
        {6.0f, 5.0f, 6.0f, 1},
    };
    CHECK(sameVertices(meshBox(
                           [](int, int y, int) {
                               return y < 2   ? SmoothCell{grass, 255, 0}
                                      : y < 5 ? SmoothCell{water, 255, 0}
                                              : SmoothCell{};
                           },
                           {0, 0, 0}, SmoothPass::Water),
                       client));
}

TEST(smooth_mesher_quads_split_and_wind_like_the_client)
{
    // A block corner of grass with rock streaks and a 150 top layer: every
    // triangle, with its winding (rotated to start at its smallest corner).
    using Corner = std::array<float, 3>;
    using Triangle = std::array<Corner, 3>;
    static const std::vector<Triangle> client{
        {{{2.0f, 2.0f, 4.5f}, {3.0f, 2.0f, 4.5f}, {2.0f, 3.0f, 4.29492188f}}},
        {{{2.0f, 3.0f, 4.29492188f}, {3.0f, 2.0f, 4.5f}, {3.0f, 3.0f, 4.29492188f}}},
        {{{2.0f, 3.0f, 4.29492188f}, {3.0f, 3.0f, 4.29492188f}, {2.0f, 3.79492188f, 3.79492188f}}},
        {{{2.0f, 3.79492188f, 3.79492188f}, {3.0f, 3.0f, 4.29492188f}, {3.0f, 3.79492188f, 3.79492188f}}},
        {{{2.0f, 3.79492188f, 3.79492188f}, {3.0f, 3.79492188f, 3.79492188f}, {3.0f, 4.08984375f, 3.0f}}},
        {{{2.0f, 3.79492188f, 3.79492188f}, {3.0f, 4.08984375f, 3.0f}, {2.0f, 4.08984375f, 3.0f}}},
        {{{2.0f, 4.08984375f, 2.0f}, {2.0f, 4.08984375f, 3.0f}, {3.0f, 4.08984375f, 2.0f}}},
        {{{2.0f, 4.08984375f, 3.0f}, {3.0f, 4.08984375f, 3.0f}, {3.0f, 4.08984375f, 2.0f}}},
        {{{3.0f, 2.0f, 4.5f}, {4.03400421f, 1.92143917f, 3.98241186f}, {4.06748056f, 3.08794117f, 3.86580205f}}},
        {{{3.0f, 2.0f, 4.5f}, {4.06748056f, 3.08794117f, 3.86580205f}, {3.0f, 3.0f, 4.29492188f}}},
        {{{3.0f, 3.0f, 4.29492188f}, {3.65323019f, 3.52424979f, 3.94402242f}, {3.0f, 3.79492188f, 3.79492188f}}},
        {{{3.0f, 3.0f, 4.29492188f}, {4.06748056f, 3.08794117f, 3.86580205f}, {3.65323019f, 3.52424979f, 3.94402242f}}},
        {{{3.0f, 3.79492188f, 3.79492188f}, {3.65323019f, 3.52424979f, 3.94402242f}, {3.79492188f, 3.79492188f, 3.0f}}},
        {{{3.0f, 3.79492188f, 3.79492188f}, {3.79492188f, 3.79492188f, 3.0f}, {3.0f, 4.08984375f, 3.0f}}},
        {{{3.0f, 4.08984375f, 2.0f}, {3.0f, 4.08984375f, 3.0f}, {3.79492188f, 3.79492188f, 2.0f}}},
        {{{3.0f, 4.08984375f, 3.0f}, {3.79492188f, 3.79492188f, 3.0f}, {3.79492188f, 3.79492188f, 2.0f}}},
        {{{3.65323019f, 3.52424979f, 3.94402242f}, {4.06748056f, 3.08794117f, 3.86580205f}, {4.29492188f, 3.0f, 3.0f}}},
        {{{3.65323019f, 3.52424979f, 3.94402242f}, {4.29492188f, 3.0f, 3.0f}, {3.79492188f, 3.79492188f, 3.0f}}},
        {{{3.79492188f, 3.79492188f, 2.0f}, {3.79492188f, 3.79492188f, 3.0f}, {4.29492188f, 3.0f, 3.0f}}},
        {{{3.79492188f, 3.79492188f, 2.0f}, {4.29492188f, 3.0f, 3.0f}, {4.29492188f, 3.0f, 2.0f}}},
        {{{4.03400421f, 1.92143917f, 3.98241186f}, {4.5f, 2.0f, 3.0f}, {4.06748056f, 3.08794117f, 3.86580205f}}},
        {{{4.06748056f, 3.08794117f, 3.86580205f}, {4.5f, 2.0f, 3.0f}, {4.29492188f, 3.0f, 3.0f}}},
        {{{4.29492188f, 3.0f, 2.0f}, {4.29492188f, 3.0f, 3.0f}, {4.5f, 2.0f, 3.0f}}},
        {{{4.29492188f, 3.0f, 2.0f}, {4.5f, 2.0f, 3.0f}, {4.5f, 2.0f, 2.0f}}}};
    std::vector<SmoothCell> cells(std::size_t{8} * 8 * 8);
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t z = 0; z < 4; ++z)
            for (std::size_t x = 0; x < 4; ++x)
                cells[x + 8 * (z + 8 * y)]
                    = {(x + z) % 3 == 0 ? rock : grass, static_cast<std::uint8_t>(y < 3 ? 255 : 150), 0};
    SmoothMesher mesher;
    SmoothMesh mesh;
    mesher.mesh({.size = {8, 8, 8}, .cells = cells, .origin = {0, 0, 0}, .lod = 0}, SmoothPass::Solid,
                materialDeforms(true), mesh);
    std::vector<Triangle> mine;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        Triangle t{};
        for (std::size_t k = 0; k < 3; ++k) {
            const Vector3 p = mesh.vertices[mesh.indices[i + k]].position;
            t[k] = {p.x, p.y, p.z};
        }
        Triangle best = t;
        for (std::size_t r = 1; r < 3; ++r)
            best = std::min(best, Triangle{t[r], t[(r + 1) % 3], t[(r + 2) % 3]});
        mine.push_back(best);
    }
    std::ranges::sort(mine);
    CHECK(mine == client);
}
