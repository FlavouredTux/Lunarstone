#include "framework.hpp"

// The physics world: raycasts with RaycastParams, assemblies from joints,
// gravity, resting contact, terrain.

#include <lunar/Classes.hpp>
#include <lunar/physics/World.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>

using namespace lunar;
using namespace lunar::physics;

namespace {

bool near(float a, float b, float eps = 1e-3f)
{
    return std::abs(a - b) <= eps;
}

std::shared_ptr<Part> addPart(Instance& parent, Vector3 position, Vector3 size, bool anchored,
                              std::string name = "Part")
{
    auto part = std::make_shared<Part>();
    part->setName(std::move(name));
    part->setSize(size);
    part->setPosition(position);
    part->setAnchored(anchored);
    part->setParent(&parent);
    return part;
}

// One chunk of terrain: two layers of grass cells (y 0 to 8 studs) and, for
// x >= 64 studs, two layers of water above them.
std::shared_ptr<Terrain> makeTerrain(std::uint8_t waterOccupancy = 255)
{
    constexpr int size = TerrainChunk::size;
    TerrainChunk chunk;
    for (int y = 0; y < size; ++y)
        for (int z = 0; z < size; ++z)
            for (int x = 0; x < size; ++x) {
                TerrainChunk::Run run{1, 0, 0, 0};
                if (y < 2)
                    run = {1, 2, 255, 0}; // grass
                else if (y < 4 && x >= 16)
                    run = {1, 1, waterOccupancy, 0}; // water
                auto& runs = chunk.runs;
                if (!runs.empty() && runs.back().material == run.material && runs.back().occupancy == run.occupancy)
                    ++runs.back().count;
                else
                    runs.push_back(run);
            }
    auto terrain = std::make_shared<Terrain>();
    terrain->setChunks({std::move(chunk)});
    return terrain;
}

} // namespace

TEST(physics_step_rate)
{
    // Roblox's solver runs at 240 Hz.
    CHECK_EQ(stepRate, 240.0f);
    auto game = DataModel::create();
    World world(*game->workspace());
    CHECK_EQ(world.advance(1.0f / 60.0f), 4);
    CHECK_EQ(world.advance(1.0f / 30.0f), 8);
    // A long frame or a stall catches up 8 steps at most.
    CHECK_EQ(world.advance(0.2f), 8);
    CHECK_EQ(world.advance(2.0f), 8);
    // Leftovers carry over.
    CHECK_EQ(world.advance(1.0f / 480.0f), 0);
    CHECK_EQ(world.advance(1.0f / 480.0f), 1);
}

TEST(physics_raycast_params)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto floor = addPart(ws, {0.0f, 0.0f, 0.0f}, {20.0f, 1.0f, 20.0f}, true, "Floor");
    auto ghost = addPart(ws, {0.0f, 5.0f, 0.0f}, {4.0f, 1.0f, 4.0f}, true, "Ghost");
    ghost->setCanCollide(false);
    World world(ws);

    auto hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == ghost.get());
    CHECK(near(hit->position.y, 5.5f));
    CHECK(near(hit->distance, 4.5f));
    CHECK(hit->normal == (Vector3{0.0f, 1.0f, 0.0f}));

    RaycastParams params;
    params.respectCanCollide = true;
    hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f}, params);
    CHECK(hit);
    CHECK(hit->instance == floor.get());
    CHECK(near(hit->position.y, 0.5f));

    // CanQuery off hides a non-colliding part from every query.
    ghost->setCanQuery(false);
    hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == floor.get());
    ghost->setCanQuery(true);

    params = {};
    params.filterDescendantsInstances = {ghost.get()};
    hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f}, params);
    CHECK(hit);
    CHECK(hit->instance == floor.get());
    params.filterType = Enum::RaycastFilterType::Include;
    params.filterDescendantsInstances = {floor.get()};
    hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f}, params);
    CHECK(hit);
    CHECK(hit->instance == floor.get());
    // Out of reach.
    CHECK(!world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -4.0f, 0.0f}));

    // Collision groups that don't collide with the ray's group are skipped.
    auto groups = ws.collisionGroups();
    groups.push_back({"Walls", 1, 0xFFFFFFFE});
    groups[0].mask = 0xFFFFFFFD;
    ws.setCollisionGroups(groups);
    ghost->setCollisionGroup("Walls");
    hit = world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == floor.get());
}

TEST(physics_gravity_free_fall)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto ball = addPart(ws, {0.0f, 1000.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    World world(ws);
    CHECK_EQ(ws.gravity(), 196.2f);
    for (int i = 0; i < 240; ++i)
        world.step();
    // One second: v = g t exactly; semi-implicit Euler drops g dt^2 n(n+1)/2.
    CHECK(near(ball->assemblyLinearVelocity().y, -196.2f, 0.01f));
    const float expected = 1000.0f - 196.2f * stepSize * stepSize * 240.0f * 241.0f / 2.0f;
    CHECK(near(ball->position().y, expected, 0.01f));
}

TEST(physics_resting_contact)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, 0.0f, 0.0f}, {50.0f, 1.0f, 50.0f}, true);
    auto box = addPart(ws, {0.0f, 3.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    // A stack of three on the side.
    std::shared_ptr<Part> stack[3];
    for (int i = 0; i < 3; ++i)
        stack[i] = addPart(ws, {10.0f, 1.5f + 2.0f * static_cast<float>(i), 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    World world(ws);
    for (int i = 0; i < 240 * 4; ++i)
        world.step();
    CHECK(near(box->position().y, 1.5f, 0.03f));
    CHECK(near(box->position().x, 0.0f, 0.02f));
    CHECK(box->assemblyLinearVelocity().magnitude() < 0.1f);
    for (int i = 0; i < 3; ++i) {
        CHECK(near(stack[i]->position().y, 1.5f + 2.0f * static_cast<float>(i), 0.08f));
        CHECK(near(stack[i]->position().x, 10.0f, 0.05f));
        CHECK(stack[i]->cframe().upVector().y > 0.999f);
    }
}

TEST(physics_welds_make_assemblies)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto model = std::make_shared<Model>();
    model->setParent(&ws);
    auto a = addPart(*model, {0.0f, 50.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "A");
    auto b = addPart(*model, {3.0f, 50.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "B");
    auto c = addPart(*model, {0.0f, 53.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false, "C");
    auto weld = std::make_shared<Weld>();
    weld->setPart0(a.get());
    weld->setPart1(b.get());
    weld->setC0(CFrame(1.5f, 0.0f, 0.0f));
    weld->setC1(CFrame(-1.5f, 0.0f, 0.0f));
    weld->setParent(a.get());
    auto constraint = std::make_shared<WeldConstraint>();
    constraint->setPart0(a.get());
    constraint->setPart1(c.get());
    constraint->setParent(a.get());
    World world(ws);
    world.refresh();
    Assembly* assembly = world.assemblyOf(a.get());
    CHECK(assembly);
    CHECK(assembly == world.assemblyOf(b.get()));
    CHECK(assembly == world.assemblyOf(c.get()));
    CHECK_EQ(assembly->parts.size(), 3u);
    // The biggest part is the root (A and B tie; either is fine), mass sums the densities.
    CHECK(near(assembly->mass, 0.7f * (8.0f + 8.0f + 1.0f), 1e-4f));
    for (int i = 0; i < 60; ++i)
        world.step();
    // Fell together, offsets kept.
    CHECK(a->position().y < 49.0f);
    CHECK(near(b->position().x - a->position().x, 3.0f, 1e-3f));
    CHECK(near(c->position().y - a->position().y, 3.0f, 1e-3f));
    CHECK(near(b->position().y, a->position().y, 1e-3f));

    // Anchoring one part anchors the whole assembly.
    c->setAnchored(true);
    const float y = a->position().y;
    for (int i = 0; i < 60; ++i)
        world.step();
    CHECK(world.assemblyOf(a.get())->anchored);
    CHECK_EQ(a->position().y, y);
}

TEST(physics_friction_and_slide)
{
    // A box sliding on plastic stops; Workspace gravity drives the normal force.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, 0.0f, 0.0f}, {200.0f, 1.0f, 200.0f}, true);
    auto box = addPart(ws, {0.0f, 1.5f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    World world(ws);
    world.step();
    box->setAssemblyLinearVelocity({20.0f, 0.0f, 0.0f});
    for (int i = 0; i < 240 * 3; ++i)
        world.step();
    // Plastic on plastic: mu = 0.3, so it stops after v^2 / (2 mu g) = 400 / 117.72 = 3.4 studs.
    CHECK(box->assemblyLinearVelocity().magnitude() < 0.1f);
    CHECK(near(box->position().x, 400.0f / (2.0f * 0.3f * 196.2f), 0.3f));
}

TEST(physics_terrain_collision)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    makeTerrain()->setParent(&ws);
    auto box = addPart(ws, {40.0f, 20.0f, 40.0f}, {2.0f, 2.0f, 2.0f}, false);
    World world(ws);

    // Rays hit the smooth surface half a cell above the solid cells, where
    // the client's mesher puts it.
    auto hit = world.raycast({40.0f, 30.0f, 40.0f}, {0.0f, -50.0f, 0.0f}, [&] {
        RaycastParams p;
        p.filterDescendantsInstances = {box.get()};
        return p;
    }());
    CHECK(hit);
    CHECK(hit->instance == ws.terrain());
    CHECK(near(hit->position.y, 10.0f, 0.5f));
    CHECK(hit->normal.y > 0.99f);
    CHECK(hit->material == Enum::Material::Grass);
    CHECK_EQ(world.terrainMaterialAt({40.0f, 4.0f, 40.0f}), Enum::Material::Grass);

    // Water: hit unless IgnoreWater.
    hit = world.raycast({80.0f, 30.0f, 40.0f}, {0.0f, -50.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->material == Enum::Material::Water);
    CHECK(near(hit->position.y, 16.0f, 1.01f));
    RaycastParams dry;
    dry.ignoreWater = true;
    hit = world.raycast({80.0f, 30.0f, 40.0f}, {0.0f, -50.0f, 0.0f}, dry);
    CHECK(hit);
    CHECK(hit->material == Enum::Material::Grass);
    CHECK(world.waterAt({80.0f, 12.0f, 40.0f}) > 0.99f);
    CHECK_EQ(world.waterAt({40.0f, 12.0f, 40.0f}), 0.0f);
    {
        // Water cells are full whatever their occupancy, as they're drawn:
        // the big battle royale map's sea is stored all but empty.
        auto seaGame = DataModel::create();
        makeTerrain(0)->setParent(seaGame->workspace());
        World sea(*seaGame->workspace());
        CHECK(sea.waterAt({80.0f, 12.0f, 40.0f}) > 0.99f);
    }

    // A dropped box comes to rest on the terrain surface.
    for (int i = 0; i < 240 * 3; ++i)
        world.step();
    CHECK(near(box->position().y, 11.0f, 0.5f));
    CHECK(box->assemblyLinearVelocity().magnitude() < 0.2f);
}

// Terrain a server streams in after the world was made, and after the
// world had looked there and found nothing, is solid all the same [live:
// The Mimic's players fell through the streamed-in ground].
TEST(physics_terrain_streamed_in_later_is_solid)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    makeTerrain()->setParent(&ws);
    ws.setFallHeightEnabled(false);
    World world(ws);
    auto* terrain = ws.terrain();
    CHECK(terrain != nullptr);
    if (!terrain)
        return;
    CHECK(!world.raycast({300.0f, 40.0f, 300.0f}, {0.0f, -80.0f, 0.0f}));
    // A 16-cell block of rock, its bottom 8 layers full, under that ray.
    CHECK(terrain->write(Terrain::Block{{72, -8, 72}, 16, {{2048, 8, 255, 0}, {2048, 0, 0, 0}}}));
    const auto hit = world.raycast({300.0f, 40.0f, 300.0f}, {0.0f, -80.0f, 0.0f});
    CHECK(hit.has_value());
    CHECK(hit && hit->instance == terrain && hit->material == Enum::Material::Rock);
    auto box = addPart(ws, {300.0f, 10.0f, 300.0f}, {2.0f, 2.0f, 2.0f}, false);
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    CHECK(box->position().y > -2.0f && box->position().y < 4.0f);
}

TEST(physics_terrain_far_away_stays_unmeshed)
{
    // A big part falling far from the only chunk, and long rays out there:
    // no blocks get meshed (or remembered) away from the terrain.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    makeTerrain()->setParent(&ws);
    ws.setFallHeightEnabled(false);
    auto slab = addPart(ws, {5000.0f, 2000.0f, -3000.0f}, {500.0f, 1.0f, 500.0f}, false);
    World world(ws);
    for (int i = 0; i < 240; ++i)
        world.step();
    CHECK(slab->position().y < 1950.0f); // fell about 98 studs
    CHECK(!world.raycast({-9000.0f, 5000.0f, 9000.0f}, {18000.0f, 0.0f, -18000.0f}));
    CHECK(!world.raycast({1e9f, 1e9f, 1e9f}, {-1e9f, 0.0f, 0.0f}));
    CHECK_EQ(world.terrainBlockCount(), 0u);

    // Near the chunk, blocks still get meshed and the box lands.
    auto box = addPart(ws, {40.0f, 20.0f, 40.0f}, {2.0f, 2.0f, 2.0f}, false);
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    CHECK(near(box->position().y, 11.0f, 0.5f));
    CHECK(world.terrainBlockCount() > 0u);
    CHECK(world.terrainBlockCount() <= 27u); // the chunk's block and its neighbours at most
}

TEST(physics_anchored_parts_move)
{
    // Scripts moving anchored parts: rays and contacts follow.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto platform = addPart(ws, {0.0f, 0.0f, 0.0f}, {10.0f, 1.0f, 10.0f}, true);
    World world(ws);
    CHECK(world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f}));
    platform->setPosition({100.0f, 0.0f, 0.0f});
    CHECK(!world.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f}));
    auto hit = world.raycast({100.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == platform.get());
}

TEST(physics_simulation_filter_holds_what_others_move)
{
    // A network client moves only what it owns: the rest stays where the
    // network puts it and is solid to what does move.
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto ours = addPart(ws, {0.0f, 100.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "Ours");
    auto theirs = addPart(ws, {10.0f, 100.0f, 0.0f}, {4.0f, 1.0f, 4.0f}, false, "Theirs");
    World world(ws);
    world.setSimulationFilter([&](const BasePart& part) { return &part == ours.get(); });
    for (int i = 0; i < 240; ++i)
        world.step();
    CHECK(ours->position().y < 50.0f);
    CHECK(near(theirs->position().y, 100.0f));
    CHECK_EQ(theirs->assemblyLinearVelocity().magnitude(), 0.0f);
    // The network moving it: what lands on it rests there.
    theirs->setPosition({0.0f, 20.0f, 0.0f});
    ours->setPosition({0.0f, 22.0f, 0.0f});
    ours->setAssemblyLinearVelocity({});
    for (int i = 0; i < 240; ++i)
        world.step();
    CHECK(near(theirs->position().y, 20.0f));
    CHECK(near(ours->position().y, 21.5f, 0.05f));
    // No filter again: everything falls.
    world.setSimulationFilter(nullptr);
    for (int i = 0; i < 120; ++i)
        world.step();
    CHECK(theirs->position().y < 15.0f);
}

TEST(physics_resting_assemblies_sleep)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto floor = addPart(ws, {0.0f, 0.0f, 0.0f}, {50.0f, 1.0f, 50.0f}, true);
    auto box = addPart(ws, {0.0f, 1.6f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    std::shared_ptr<Part> stack[2];
    for (int i = 0; i < 2; ++i)
        stack[i] = addPart(ws, {10.0f, 1.5f + 2.0f * static_cast<float>(i), 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    World world(ws);
    world.refresh();
    // Not before the pose history has filled (15 samples of 8 steps).
    for (int i = 0; i < 100; ++i)
        world.step();
    CHECK(!world.assemblyOf(box.get())->asleep);
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    CHECK(world.assemblyOf(box.get())->asleep);
    CHECK(world.assemblyOf(stack[0].get())->asleep);
    CHECK(world.assemblyOf(stack[1].get())->asleep);
    CHECK_EQ(world.stats().awakeAssemblies, 0u);

    // A velocity write wakes it.
    box->setAssemblyLinearVelocity({0.0f, 30.0f, 0.0f});
    world.step();
    CHECK(!world.assemblyOf(box.get())->asleep);
    CHECK(box->position().y > 1.6f);

    // Something moving knocks the stack awake.
    auto ball = addPart(ws, {16.0f, 2.5f, 0.0f}, {2.0f, 2.0f, 2.0f}, false);
    world.step();
    ball->setAssemblyLinearVelocity({-30.0f, 0.0f, 0.0f});
    for (int i = 0; i < 60; ++i)
        world.step();
    CHECK(!world.assemblyOf(stack[1].get())->asleep);
    CHECK(stack[1]->position().x < 9.9f);

    // Taking the floor away wakes what slept on it.
    for (int i = 0; i < 240 * 3; ++i)
        world.step();
    CHECK(world.assemblyOf(box.get())->asleep);
    floor->setParent(nullptr);
    for (int i = 0; i < 60; ++i)
        world.step();
    CHECK(box->position().y < 0.0f);
}

TEST(physics_incremental_changes)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto floor = addPart(ws, {0.0f, 0.0f, 0.0f}, {50.0f, 1.0f, 50.0f}, true);
    World world(ws);
    for (int i = 0; i < 10; ++i)
        world.step();
    CHECK_EQ(world.colliderCount(), 1u);

    // Parts and joints added later are picked up; a weld joins assemblies.
    auto a = addPart(ws, {0.0f, 5.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "A");
    auto b = addPart(ws, {3.0f, 5.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "B");
    world.refresh();
    CHECK_EQ(world.colliderCount(), 3u);
    CHECK(world.assemblyOf(a.get()) != world.assemblyOf(b.get()));
    auto weld = std::make_shared<WeldConstraint>();
    weld->setPart0(a.get());
    weld->setPart1(b.get());
    weld->setParent(a.get());
    world.refresh();
    CHECK(world.assemblyOf(a.get()) == world.assemblyOf(b.get()));
    CHECK_EQ(world.assemblyOf(a.get())->parts.size(), 2u);
    weld->setEnabled(false);
    world.refresh();
    CHECK(world.assemblyOf(a.get()) != world.assemblyOf(b.get()));
    weld->setEnabled(true);
    world.refresh();
    CHECK(world.assemblyOf(a.get()) == world.assemblyOf(b.get()));
    weld->setParent(nullptr);
    world.refresh();
    CHECK(world.assemblyOf(a.get()) != world.assemblyOf(b.get()));

    // An anchored part added after the first build is collided with, and stops being when it goes.
    auto shelf = addPart(ws, {20.0f, 10.0f, 0.0f}, {6.0f, 1.0f, 6.0f}, true, "Shelf");
    auto crate = addPart(ws, {20.0f, 12.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "Crate");
    for (int i = 0; i < 240; ++i)
        world.step();
    CHECK(near(crate->position().y, 11.5f, 0.05f));
    auto hit = world.raycast({20.0f, 20.0f, 0.0f}, {0.0f, -30.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == crate.get());
    shelf->destroy();
    for (int i = 0; i < 240 * 2; ++i)
        world.step();
    CHECK(near(crate->position().y, 1.5f, 0.05f));
    CHECK_EQ(world.colliderCount(), 4u);

    // Anchoring and unanchoring move a part between the trees.
    crate->setAnchored(true);
    world.refresh();
    CHECK(world.assemblyOf(crate.get())->anchored);
    hit = world.raycast({20.0f, 20.0f, 0.0f}, {0.0f, -30.0f, 0.0f});
    CHECK(hit);
    CHECK(hit->instance == crate.get());
    crate->setAnchored(false);
    crate->setPosition({20.0f, 30.0f, 0.0f});
    for (int i = 0; i < 10; ++i)
        world.step();
    CHECK(crate->position().y < 30.0f);
}

// A script setting the CFrame of a part welded into an assembly moves the
// whole assembly there, as in Roblox: the game turns the character by setting a
// welded part's CFrame every frame, not the root's. The engine's own
// placements (the joint solver, replication) move only the part.
TEST(physics_setting_a_welded_part_moves_its_assembly)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, 0.0f, 0.0f}, {50.0f, 1.0f, 50.0f}, true);
    auto a = addPart(ws, {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, false, "A");
    auto b = addPart(ws, {3.0f, 10.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false, "B");
    auto weld = std::make_shared<WeldConstraint>();
    weld->setPart0(a.get());
    weld->setPart1(b.get());
    weld->setParent(a.get());
    World world(ws);
    world.step();
    const Assembly* assembly = world.assemblyOf(a.get());
    CHECK(assembly && assembly->parts.size() == 2u);
    BasePart* root = assembly->root;
    BasePart* other = root == a.get() ? b.get() : a.get();
    const CFrame rel = root->cframe().inverse() * other->cframe();

    // Turned half round and moved, through the part that isn't the root.
    const CFrame wanted = CFrame(Vector3{10.0f, 10.0f, 5.0f}) * CFrame::angles(0.0f, 3.14159265f, 0.0f);
    {
        const BasePart::CarryAssembly script; // as part.CFrame = ... from Luau
        other->setCFrame(wanted);
    }
    CHECK(((root->cframe() * rel).position - wanted.position).magnitude() < 1e-3f);
    world.step();
    // The weld holds it there: the step doesn't put it back.
    CHECK((other->position() - wanted.position).magnitude() < 0.1f);
    CHECK(other->cframe().lookVector().dot(wanted.lookVector()) > 0.999f);
    CHECK(((root->cframe().inverse() * other->cframe()).position - rel.position).magnitude() < 1e-3f);
}

// The same for an anchored assembly: the game carries its anchored first-person
// rig (Workspace.Camera.BaseRig) by setting a welded part's CFrame.
TEST(physics_setting_a_welded_part_moves_an_anchored_assembly)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto root = addPart(ws, {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, true, "Root");
    auto arm = addPart(ws, {3.0f, 10.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false, "Arm");
    auto weld = std::make_shared<WeldConstraint>();
    weld->setPart0(root.get());
    weld->setPart1(arm.get());
    weld->setParent(root.get());
    World world(ws);
    world.step();
    {
        const BasePart::CarryAssembly script;
        arm->setCFrame(CFrame(Vector3{13.0f, 10.0f, 0.0f}));
    }
    CHECK((root->position() - Vector3{10.0f, 10.0f, 0.0f}).magnitude() < 1e-3f);
    world.step();
    CHECK((arm->position() - Vector3{13.0f, 10.0f, 0.0f}).magnitude() < 1e-3f);
    CHECK((root->position() - Vector3{10.0f, 10.0f, 0.0f}).magnitude() < 1e-3f);
}

// a temporary scene prop: two rock slabs welded (Weld, slab as Part0) to an
// anchored Center under the floor, their welds' C0 tweened to swing them
// up [User Error Interface]. A changed C0 moves the parts it holds also on
// an anchored assembly, which no step places, and the slab is found there.
TEST(physics_a_weld_c0_change_moves_an_anchored_assembly)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto center = addPart(ws, {0.0f, 10.0f, 0.0f}, {1.5f, 1.5f, 1.5f}, true, "Center");
    auto slab = addPart(ws, {0.0f, 10.0f, -7.5f}, {18.0f, 8.0f, 14.0f}, false, "Slab"); // where the weld holds it
    auto weld = std::make_shared<Weld>();
    weld->setPart0(slab.get());
    weld->setPart1(center.get());
    weld->setC0(CFrame(Vector3{0.0f, 0.0f, 7.5f}));
    weld->setParent(slab.get());
    World world(ws);
    world.step();
    CHECK((slab->position() - Vector3{0.0f, 10.0f, -7.5f}).magnitude() < 1e-3f);

    // Up on end: Part0 * C0 = Part1, so the slab is at Center * C0:Inverse().
    weld->setC0(CFrame(Vector3{0.0f, 6.75f, 12.0f}) * CFrame::angles(-std::numbers::pi_v<float> / 2, 0.0f, 0.0f));
    world.step();
    CHECK((slab->position() - Vector3{0.0f, 22.0f, -6.75f}).magnitude() < 1e-3f);
    CHECK(std::abs(slab->cframe().upVector().z) > 0.999f);
    CHECK((center->position() - Vector3{0.0f, 10.0f, 0.0f}).magnitude() < 1e-3f);
    // The anchored tree knows where it went.
    auto hit = world.raycast({0.0f, 40.0f, -6.75f}, {0.0f, -25.0f, 0.0f});
    CHECK(hit && hit->instance == slab.get());
    CHECK(hit && near(hit->position.y, 22.0f + 7.0f));
}

// The game's drawers: the server anchors a drawer's Main (unanchored until
// then, on a PrismaticConstraint) and every client tweens its CFrame open
// and shut, the drawer front (End) and its Knobs welded to it with
// ManualWelds [capture]. A live game changes something structural most
// frames; the carry has to take that in rather than move Main alone, which
// left the front and knobs behind wherever the last carried frame put them.
TEST(physics_script_move_carries_the_assembly_with_changes_pending)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto main = addPart(ws, {0.0f, 3.0f, 0.0f}, {2.0f, 0.5f, 2.0f}, false, "Main");
    auto front = addPart(ws, {1.0f, 3.1f, 0.0f}, {0.2f, 0.7f, 2.6f}, false, "End");
    auto knobs = addPart(ws, {1.15f, 3.06f, 0.0f}, {0.2f, 0.3f, 1.8f}, false, "Knobs");
    for (const auto& part : {front, knobs}) {
        auto weld = std::make_shared<ManualWeld>();
        weld->setPart0(part.get());
        weld->setPart1(main.get());
        weld->setC0(CFrame{});
        weld->setC1(main->cframe().inverse() * part->cframe());
        weld->setParent(part.get());
    }
    World world(ws);
    world.step();
    const CFrame frontOffset = main->cframe().inverse() * front->cframe();
    const CFrame knobsOffset = main->cframe().inverse() * knobs->cframe();
    auto held = [&] {
        return ((main->cframe() * frontOffset).position - front->position()).magnitude() < 1e-3f
               && ((main->cframe() * knobsOffset).position - knobs->position()).magnitude() < 1e-3f;
    };

    main->setAnchored(true); // the server, before the first tween
    const CFrame shut = main->cframe();
    const CFrame open = shut * CFrame(Vector3{1.4f, 0.0f, 0.0f});
    for (int frame = 1; frame <= 24; ++frame) {
        // Something else changes between the tween's frames.
        addPart(ws, {50.0f + static_cast<float>(frame), 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
        const float t = static_cast<float>(frame) / 24.0f;
        const CFrame to = frame <= 12 ? shut * CFrame(Vector3{1.4f * t * 2.0f, 0.0f, 0.0f})
                                      : open * CFrame(Vector3{-1.4f * (t - 0.5f) * 2.0f, 0.0f, 0.0f});
        {
            const BasePart::CarryAssembly script; // a tween's step
            main->setCFrame(to);
        }
        CHECK(held());
        if (frame % 5 == 0)
            world.step();
    }
    CHECK((main->position() - shut.position).magnitude() < 1e-3f);
    CHECK(held()); // shut again, front and knobs with it
    world.step();
    CHECK(held());
}

TEST(physics_engine_placements_dont_carry_the_assembly)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto root = addPart(ws, {0.0f, 10.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, true, "Root");
    auto arm = addPart(ws, {3.0f, 10.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false, "Arm");
    auto weld = std::make_shared<WeldConstraint>();
    weld->setPart0(root.get());
    weld->setPart1(arm.get());
    weld->setParent(root.get());
    World world(ws);
    world.step();
    arm->setCFrame(CFrame(Vector3{13.0f, 10.0f, 0.0f})); // no CarryAssembly: the engine's own
    CHECK((root->position() - Vector3{0.0f, 10.0f, 0.0f}).magnitude() < 1e-3f);
}

// A union whose collision data is only a hash (the game's walls) collides as
// the parts it was made from once the app can say what they were, scaled
// onto its Size, and as its box before that.
TEST(physics_union_with_a_hash_collides_as_its_parts)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    // A wall with a doorway, twice the size it was made at.
    auto wall = std::make_shared<UnionOperation>();
    wall->setName("Wall");
    wall->setAnchored(true);
    wall->setSize({32.0f, 24.0f, 2.0f});
    wall->setCFrame(CFrame(Vector3{0.0f, 12.0f, 0.0f}));
    wall->setPhysicalConfigData(std::make_shared<const std::string>("1edd5c57d0534c3ce572826e8bb4aae5"));
    wall->setParent(&ws);
    const GeometryPiece pieces[] = {
        {ShapeKind::Box, CFrame(Vector3{0.0f, 4.0f, 0.0f}), {16.0f, 4.0f, 1.0f}},  // over the doorway
        {ShapeKind::Box, CFrame(Vector3{-5.0f, -2.0f, 0.0f}), {6.0f, 8.0f, 1.0f}}, // left of it
        {ShapeKind::Box, CFrame(Vector3{5.0f, -2.0f, 0.0f}), {6.0f, 8.0f, 1.0f}},  // right of it
    };
    bool known = false;
    World world(ws);
    world.setUnionGeometry([&](const PartOperation&) { return known ? piecesGeometry(pieces) : nullptr; });
    world.step();

    // The doorway is 8 studs wide and 16 high at this size, from y 0 to 16.
    auto through = [&](float x, float y) {
        return world.raycast({x, y, 10.0f}, {0.0f, 0.0f, -20.0f}).has_value();
    };
    CHECK(through(0.0f, 4.0f)); // the box, until the parts are known
    known = true;
    world.reshape(*wall);
    world.step();
    CHECK(!through(0.0f, 4.0f));  // through the doorway
    CHECK(!through(3.5f, 14.0f)); // near its top corner
    CHECK(through(10.0f, 4.0f));  // the wall beside it
    CHECK(through(0.0f, 20.0f));  // and above it
}

// The touches a client reports for its character: a limb in a sensor the
// server listens to (the game's side doors: CanCollide off, CanTouch on, a
// TouchTransmitter), none once it's out or the sensor has CanTouch off.
TEST(physics_character_touches_listened_parts)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto character = std::make_shared<Model>();
    character->setName("Character");
    character->setParent(&ws);
    auto limb = addPart(*character, {0.0f, 3.0f, 0.0f}, {2.0f, 2.0f, 1.0f}, true, "Limb");
    auto sensor = addPart(ws, {0.0f, 3.0f, 3.0f}, {5.0f, 8.0f, 10.0f}, true, "Hidden");
    sensor->setCanCollide(false);
    addPart(ws, {0.0f, 3.0f, -1.0f}, {4.0f, 4.0f, 1.0f}, true, "Wall"); // touching, but nobody listens
    World world(ws);
    auto listened = [&](const BasePart& part) { return &part == sensor.get(); };

    auto touches = world.touches(*character, listened);
    CHECK_EQ(touches.size(), std::size_t{1});
    CHECK(touches.at(0) == (World::Touch{limb.get(), sensor.get()}));
    limb->setCFrame(CFrame(Vector3{0.0f, 3.0f, -20.0f}));
    CHECK(world.touches(*character, listened).empty());
    limb->setCFrame(CFrame(Vector3{0.0f, 3.0f, 3.0f})); // well inside
    CHECK_EQ(world.touches(*character, listened).size(), std::size_t{1});
    sensor->setCanTouch(false);
    CHECK(world.touches(*character, listened).empty());
}

// The same for parts of ours outside a character (the server gave them to
// us): a part set inside a listened one touches it, and our parts don't
// touch each other.
TEST(physics_given_parts_touch_listened_parts)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto given = addPart(ws, {67.0f, 350.0f, 67.0f}, {4.0f, 1.0f, 2.0f}, false, "itsstarting");
    auto target = addPart(ws, {20.0f, 420.0f, 67.0f}, {4.0f, 1.0f, 2.0f}, true, "itsjustgettingstarted");
    auto alsoOurs = addPart(ws, {67.0f, 351.0f, 67.0f}, {4.0f, 1.0f, 2.0f}, true, "AlsoOurs"); // on it
    World world(ws);
    auto listened = [&](const BasePart&) { return true; };
    const std::vector<const BasePart*> ours{given.get(), alsoOurs.get()};

    CHECK(world.touches(ours, listened).empty());
    given->setCFrame(CFrame(Vector3{20.0f, 420.0f, 67.0f}));
    auto touches = world.touches(ours, listened);
    CHECK_EQ(touches.size(), std::size_t{1});
    CHECK(touches.at(0) == (World::Touch{given.get(), target.get()}));
}
