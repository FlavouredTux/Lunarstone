#include "framework.hpp"

// A physics benchmark on a real place, run only when LUNAR_PHYSICS_BENCH
// names one. It does what the battle royale map's SpawnVehicles script
// does, without scripts so runs repeat exactly: a vehicle from
// ServerStorage.Assets.Vehicles dropped 10 studs above each
// "VehicleSpawner", one a second. LUNAR_PHYSICS_BENCH_SECONDS sets how long
// (70 by default); LUNAR_PHYSICS_THREADS the threads the World spreads over.
// With LUNAR_PHYSICS_BENCH_ALL set, every spawner's vehicle drops in the
// first frame and every unanchored assembly is woken each frame: the whole
// map busy at once.

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/physics/World.hpp>

#include <chrono>
#include <cstdlib>
#include <print>
#include <string>

using namespace lunar;
using namespace lunar::physics;

TEST(physics_vehicle_bench)
{
    const char* path = std::getenv("LUNAR_PHYSICS_BENCH"); // NOLINT(concurrency-mt-unsafe)
    if (!path)
        SKIP("set LUNAR_PHYSICS_BENCH to a place with VehicleSpawners");
    float seconds = 70.0f;
    if (const char* s = std::getenv("LUNAR_PHYSICS_BENCH_SECONDS")) // NOLINT(concurrency-mt-unsafe)
        seconds = std::stof(s);
    auto bytes = readFile(path);
    CHECK(bytes.has_value());
    auto place = loadPlace(*bytes);
    CHECK(place.has_value());
    Workspace& ws = *place->game->workspace();
    Instance* storage = place->game->findService("ServerStorage");
    Instance* vehicles = storage ? storage->findFirstChild("Vehicles", true) : nullptr;
    CHECK(vehicles != nullptr);
    std::vector<BasePart*> spawners;
    for (const InstancePtr& d : ws.descendants())
        if (auto* part = dynamic_cast<BasePart*>(d.get()); part && part->hasTag("VehicleSpawner"))
            spawners.push_back(part);
    Instance* parent = ws.findFirstChild("Vehicles");
    if (!parent)
        parent = &ws;

    World world(ws);
    world.refresh();
    constexpr float dt = 1.0f / 60.0f;
    const int frames = static_cast<int>(seconds / dt);
    const bool all = std::getenv("LUNAR_PHYSICS_BENCH_ALL") != nullptr; // NOLINT(concurrency-mt-unsafe)
    std::size_t next = 0;
    double total = 0.0;
    double worst = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        while ((all ? frame == 0 : frame % 60 == 59) && next < spawners.size()) {
            // The spawner's own vehicle if it names one, else the first (the script picks at random).
            const BasePart& spawner = *spawners[next++];
            Instance* source = vehicles->children().front().get();
            if (auto* named = dynamic_cast<StringValue*>(spawner.findFirstChild("Vehicle")))
                if (Instance* found = vehicles->findFirstChild(named->value()))
                    source = found;
            auto vehicle = std::dynamic_pointer_cast<Model>(source->clone());
            CHECK(vehicle != nullptr);
            const CFrame target = spawner.cframe() * CFrame(0.0f, 10.0f, 0.0f);
            if (BasePart* primary = vehicle->primaryPart())
                vehicle->pivotTo(target * primary->cframe().inverse() * vehicle->getPivot());
            else
                vehicle->pivotTo(target);
            vehicle->setParent(parent);
            if (!all)
                break;
        }
        if (all)
            for (const auto& assembly : world.assemblies())
                if (!assembly->anchored)
                    assembly->asleep = false;
        const auto start = std::chrono::steady_clock::now();
        world.advance(dt);
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        total += spent;
        worst = std::max(worst, spent);
    }
    // Where everything ended up, to tell runs apart.
    double checksum = 0.0;
    for (const auto& assembly : world.assemblies())
        if (!assembly->anchored)
            for (const BasePart* part : assembly->parts) {
                const Vector3 p = part->position();
                checksum += static_cast<double>(p.x) + static_cast<double>(p.y) * 3.0 + static_cast<double>(p.z) * 7.0;
            }
    const World::Stats& stats = world.stats();
    std::println("{} threads, {} vehicles; physics {:.3f} ms a frame on average, {:.3f} ms worst", world.threads(),
                 next, total / frames * 1000.0, worst * 1000.0);
    std::println("{} updates, {:.3f} ms average, {:.3f} ms worst; {} awake, {} asleep; checksum {:.6f}", stats.updates,
                 stats.updates ? stats.updateSeconds / static_cast<double>(stats.updates) * 1000.0 : 0.0,
                 stats.worstUpdateSeconds * 1000.0, stats.awakeAssemblies, stats.sleepingAssemblies, checksum);
    std::println("{} of {} steps spread over threads; {} islands, {} contacts, {} rows in the last",
                 stats.parallelSteps, stats.steps, stats.islands, stats.contacts, stats.rows);
}
