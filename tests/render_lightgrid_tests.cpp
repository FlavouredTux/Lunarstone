#include "framework.hpp"

#include <lunar/render/LightGrid.hpp>
#include <lunar/render/Primitives.hpp>

#include <memory>
#include <vector>

using lunar::Vector3;
using namespace lunar::render;

namespace {

// A block part of the given size centred at a point.
DrawItem block(Vector3 center, Vector3 size, float transparency = 0.0f)
{
    DrawItem item;
    item.model = Mat4::translation(center) * Mat4::scale(size);
    item.transparency = transparency;
    return item;
}

// The world voxel holding a point.
GridPoint at(float x, float y, float z)
{
    return LightGrid::voxelAt({x, y, z});
}

LightGrid::Settings overhead()
{
    return {.sunDirection = {0.0f, 1.0f, 0.0f}, .shadows = true};
}

} // namespace

TEST(lightgrid_is_open_sky_without_parts)
{
    LightGrid grid;
    grid.build({}, {0.0f, 0.0f, 0.0f}, overhead());
    CHECK(grid.ready());
    CHECK_EQ(grid.voxels(), (GridPoint{256, 64, 256}));
    CHECK_EQ(grid.originVoxel(), (GridPoint{-128, -32, -128}));
    // The client's curve tops out at 252; the sun is whole.
    CHECK_EQ(grid.sky(at(10.0f, 0.0f, 10.0f)), 252);
    CHECK_EQ(grid.sun(at(10.0f, 0.0f, 10.0f)), 255);
    CHECK_EQ(grid.occupancy(1, at(10.0f, 0.0f, 10.0f)), 0);
    // Outside the grid: full light.
    CHECK_EQ(grid.sky(at(10.0f, 1000.0f, 10.0f)), 255);
}

TEST(lightgrid_block_fills_the_voxel_it_covers)
{
    LightGrid grid;
    const std::vector<DrawItem> items{block({10.0f, 6.0f, 14.0f}, {4.0f, 4.0f, 4.0f})};
    grid.build(items, {}, overhead());
    for (int axis = 0; axis < 3; ++axis) {
        CHECK_EQ(grid.occupancy(axis, at(10.0f, 6.0f, 14.0f)), 255);
        CHECK_EQ(grid.occupancy(axis, at(14.0f, 6.0f, 14.0f)), 0);
    }
}

TEST(lightgrid_thin_plate_blocks_along_its_thin_side)
{
    LightGrid grid;
    const std::vector<DrawItem> items{block({10.0f, 6.0f, 14.0f}, {4.0f, 1.0f, 4.0f}),
                                      block({30.0f, 6.0f, 14.0f}, {4.0f, 4.0f, 4.0f}, 0.5f)};
    grid.build(items, {}, overhead());
    // Seen from above it covers the voxel; from the sides a quarter of it.
    CHECK_EQ(grid.occupancy(1, at(10.0f, 6.0f, 14.0f)), 255);
    CHECK_EQ(grid.occupancy(0, at(10.0f, 6.0f, 14.0f)), 63);
    CHECK_EQ(grid.occupancy(2, at(10.0f, 6.0f, 14.0f)), 63);
    // Half transparent: half the occupancy.
    CHECK_EQ(grid.occupancy(1, at(30.0f, 6.0f, 14.0f)), 127);
}

TEST(lightgrid_roof_shades_what_is_under_it)
{
    LightGrid grid;
    const std::vector<DrawItem> items{block({2.0f, 42.0f, 2.0f}, {96.0f, 1.0f, 96.0f})};
    grid.build(items, {}, overhead());
    // Above the roof and well away from it: open sky.
    CHECK_EQ(grid.sky(at(2.0f, 50.0f, 2.0f)), 252);
    CHECK_EQ(grid.sky(at(100.0f, 10.0f, 2.0f)), 252);
    // The voxel holding the roof still sees the light coming down into it.
    CHECK_EQ(grid.sky(at(2.0f, 42.0f, 2.0f)), 252);
    // Right under the middle of it: no sky, no sun.
    CHECK_EQ(grid.sky(at(2.0f, 34.0f, 2.0f)), 0);
    CHECK_EQ(grid.sky(at(2.0f, 2.0f, 2.0f)), 0);
    // (A solid voxel holds back 254 of 255 of the sun, as in the client.)
    CHECK(grid.sun(at(2.0f, 2.0f, 2.0f)) <= 2);
    // Light creeps in a voxel per layer from the edges.
    const int edge = grid.sky(at(46.0f, 34.0f, 2.0f));
    CHECK(edge > 0);
    CHECK(edge < 252);
    CHECK(grid.sky(at(46.0f, 30.0f, 2.0f)) < 252);
}

TEST(lightgrid_keeps_the_sky_beside_a_room_out_of_it)
{
    LightGrid grid;
    // A roofed room whose outer wall stands at the far side of its last
    // voxel (x 48 to 52), with open sky past it; and a wall out in the open.
    const std::vector<DrawItem> items{block({3.0f, 42.0f, 2.0f}, {98.0f, 1.0f, 96.0f}),
                                      block({51.75f, 21.0f, 2.0f}, {0.5f, 42.0f, 96.0f}),
                                      block({-100.25f, 21.0f, 2.0f}, {0.5f, 42.0f, 96.0f})};
    grid.build(items, {}, overhead());
    // Past the wall the sky is open.
    CHECK(grid.sky(at(54.0f, 30.0f, 2.0f)) > 200);
    // The room's side of the wall's voxel: the client shows it the sky
    // beside it, coming in sideways past the wall; it's held back.
    CHECK_EQ(grid.sky(at(49.0f, 30.0f, 2.0f)), 0);
    CHECK_EQ(grid.sky(at(49.0f, 2.0f, 2.0f)), 0);
    // A wall out in the open, with the sky overhead, keeps the client's
    // light: nearly what the air beside it gets.
    CHECK(grid.sky(at(-101.0f, 30.0f, 2.0f)) + 4 >= grid.sky(at(-105.0f, 30.0f, 2.0f)));
}

TEST(lightgrid_wall_shadows_the_sun_along_its_direction)
{
    LightGrid grid;
    // A wall facing +X; the sun low in the +X sky.
    const std::vector<DrawItem> items{block({40.0f, 32.0f, 2.0f}, {4.0f, 64.0f, 96.0f})};
    const LightGrid::Settings settings{.sunDirection = Vector3{1.0f, 0.5f, 0.0f}.unit(), .shadows = true};
    grid.build(items, {}, settings);
    CHECK_EQ(grid.sun(at(60.0f, 2.0f, 2.0f)), 255); // in front of the wall
    CHECK(grid.sun(at(30.0f, 2.0f, 2.0f)) <= 2);    // behind it
    CHECK(grid.sky(at(30.0f, 2.0f, 2.0f)) > 0);     // the sky still reaches in from above
}

TEST(lightgrid_follows_parts_that_move)
{
    LightGrid grid;
    std::vector<DrawItem> items{block({10.0f, 6.0f, 14.0f}, {4.0f, 4.0f, 4.0f})};
    grid.update(items, {}, overhead());
    CHECK_EQ(grid.occupancy(1, at(10.0f, 6.0f, 14.0f)), 255);
    items[0] = block({50.0f, 6.0f, 14.0f}, {4.0f, 4.0f, 4.0f});
    for (int frame = 0; frame < 4; ++frame) {
        grid.update(items, {}, overhead());
        grid.wait();
    }
    CHECK_EQ(grid.occupancy(1, at(10.0f, 6.0f, 14.0f)), 0);
    CHECK_EQ(grid.occupancy(1, at(50.0f, 6.0f, 14.0f)), 255);
    CHECK(!grid.takeUploads().empty());
}

TEST(lightgrid_follows_the_camera_by_whole_chunks)
{
    LightGrid grid;
    const std::vector<DrawItem> items{block({2.0f, 42.0f, 2.0f}, {96.0f, 1.0f, 96.0f})};
    grid.update(items, {}, overhead());
    CHECK_EQ(grid.originVoxel(), (GridPoint{-128, -32, -128}));
    for (int frame = 0; frame < 4; ++frame) {
        grid.update(items, {200.0f, 0.0f, 0.0f}, overhead());
        grid.wait();
    }
    CHECK_EQ(grid.originVoxel(), (GridPoint{-128 + 32, -32, -128}));
    CHECK_EQ(grid.sky(at(2.0f, 2.0f, 2.0f)), 0);
}

TEST(lightgrid_takes_meshes_for_their_collision_boxes)
{
    // A mesh spanning 16 studs, but only its low corner's 4-stud box collides.
    auto mesh = std::make_shared<lunar::MeshData>();
    mesh->vertices.resize(3);
    mesh->vertices[0].position = {0.0f, 0.0f, 0.0f};
    mesh->vertices[1].position = {16.0f, 0.0f, 0.0f};
    mesh->vertices[2].position = {0.0f, 16.0f, 16.0f};
    mesh->indices = {0, 1, 2};
    DrawItem item;
    item.model = Mat4::translation({8.0f, 0.0f, 8.0f});
    item.mesh = mesh;
    item.occluders
        = std::make_shared<std::vector<Bounds>>(std::vector<Bounds>{{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}}});
    LightGrid grid;
    grid.build(std::vector<DrawItem>{item}, {}, overhead());
    CHECK_EQ(grid.occupancy(1, at(10.0f, 2.0f, 10.0f)), 255);
    CHECK_EQ(grid.occupancy(1, at(18.0f, 2.0f, 10.0f)), 0);
    CHECK_EQ(grid.occupancy(1, at(10.0f, 10.0f, 18.0f)), 0);
}

// Parts that move carry the light again only from what they changed, and it
// comes out as carrying all of it does, bit for bit: under a roof, past a
// wall, with the sun overhead, low from either side and from below. Three
// chunks across leaves the last word of each row of the masks part empty.
TEST(lightgrid_carries_moves_as_building_it_again_would)
{
    const GridPoint chunks{3, 2, 3};
    const Vector3 suns[] = {{0.0f, 1.0f, 0.0f},
                            Vector3{1.0f, 0.5f, 0.3f}.unit(),
                            Vector3{-0.4f, 0.8f, -0.9f}.unit(),
                            Vector3{0.3f, -0.6f, 0.5f}.unit()};
    const Vector3 moves[] = {{3.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 7.0f}, {-40.0f, 10.0f, 2.0f}, {0.5f, 0.0f, -0.25f}};
    for (const Vector3& sun : suns) {
        const LightGrid::Settings settings{.sunDirection = sun, .shadows = true};
        std::vector<DrawItem> items{
            block({0.0f, 40.0f, 0.0f}, {120.0f, 2.0f, 120.0f}), // a roof
            block({-30.0f, 10.0f, 5.0f}, {4.0f, 20.0f, 60.0f}), // a wall under it
            block({20.0f, 6.0f, -20.0f}, {6.0f, 6.0f, 6.0f}),   // what moves
            block({-10.0f, 30.0f, 30.0f}, {8.0f, 1.0f, 8.0f}, 0.5f),
        };
        LightGrid moved(chunks);
        moved.setScanInterval({});
        moved.update(items, {}, settings);
        Vector3 at{20.0f, 6.0f, -20.0f};
        for (const Vector3& move : moves) {
            at = at + move;
            items[2] = block(at, {6.0f, 6.0f, 6.0f});
            items[3] = block(Vector3{-10.0f, 30.0f, 30.0f} - move, {8.0f, 1.0f, 8.0f}, 0.5f);
            for (int frame = 0; frame < 3; ++frame) {
                moved.update(items, {}, settings);
                moved.wait();
            }
            LightGrid whole(chunks);
            whole.build(items, {}, settings);
            const GridPoint low = whole.originVoxel(), size = whole.voxels();
            CHECK_EQ(moved.originVoxel(), low);
            std::size_t differ = 0;
            for (int y = low.y; y < low.y + size.y; ++y)
                for (int z = low.z; z < low.z + size.z; ++z)
                    for (int x = low.x; x < low.x + size.x; ++x) {
                        const GridPoint p{x, y, z};
                        differ += moved.sky(p) != whole.sky(p) || moved.sun(p) != whole.sun(p)
                                  || moved.skyBelow(p) != whole.skyBelow(p);
                    }
            CHECK_EQ(differ, std::size_t{0});
        }
    }
}
