#include "framework.hpp"

// The physics step spread over threads: the pool itself, results that
// don't depend on the thread count, small worlds staying on one thread, and
// (when LUNAR_PHYSICS_PARALLEL_BENCH is set) a benchmark of a busy scene.

#include <lunar/Classes.hpp>
#include <lunar/physics/ThreadPool.hpp>
#include <lunar/physics/World.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::physics;

namespace {

std::shared_ptr<Part> addPart(Instance& parent, Vector3 position, Vector3 size, bool anchored)
{
    auto part = std::make_shared<Part>();
    part->setSize(size);
    part->setPosition(position);
    part->setAnchored(anchored);
    part->setParent(&parent);
    return part;
}

std::shared_ptr<Attachment> attach(BasePart& part, Vector3 world)
{
    auto attachment = std::make_shared<Attachment>();
    attachment->setCFrame(CFrame(part.cframe().pointToObjectSpace(world)));
    attachment->setParent(&part);
    return attachment;
}

// Two layers of grass cells over one chunk: the ground is at y = 8 for x
// and z in [0, 128).
std::shared_ptr<Terrain> makeTerrain()
{
    constexpr int size = TerrainChunk::size;
    TerrainChunk chunk;
    chunk.runs.push_back({static_cast<std::uint16_t>(size * size * 2), 2, 255, 0});
    chunk.runs.push_back({static_cast<std::uint16_t>(size * size * (size - 2)), 0, 0, 0});
    auto terrain = std::make_shared<Terrain>();
    terrain->setChunks({std::move(chunk)});
    return terrain;
}

// A busy scene, side by side cells 6 studs apart over a floor and part of
// the terrain: stacks of three tilted boxes, balls dropped from above, and
// carts (a body on two wheels, hinged, with a spring between them), each
// cell its own island once things settle.
struct Scene {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::vector<std::shared_ptr<Instance>> keep;
    std::vector<BasePart*> parts;

    explicit Scene(int side)
    {
        Workspace& ws = *game->workspace();
        makeTerrain()->setParent(&ws);
        keep.push_back(addPart(ws, {0.0f, -0.5f, 0.0f}, {800.0f, 1.0f, 800.0f}, true));
        const float origin = -3.0f * static_cast<float>(side);
        for (int i = 0; i < side * side; ++i) {
            const float x = origin + 6.0f * static_cast<float>(i % side);
            const int row = i / side;
            const float z = origin + 6.0f * static_cast<float>(row);
            const float ground = x >= 0.0f && z >= 0.0f && x < 128.0f && z < 128.0f ? 8.0f : 0.0f;
            const float tilt = 0.05f * static_cast<float>(i % 7);
            if (i % 5 == 4) {
                cart({x, ground, z});
                continue;
            }
            if (i % 4 == 3) {
                auto ball = addPart(ws, {x, ground + 6.0f, z}, {2.0f, 2.0f, 2.0f}, false);
                ball->setShape(Enum::PartType::Ball);
                ball->setAssemblyLinearVelocity({1.0f, 0.0f, -0.5f});
                add(ball);
            }
            for (int level = 0; level < 3; ++level) {
                auto box = addPart(
                    ws, {x + tilt * static_cast<float>(level), ground + 1.05f + 2.0f * static_cast<float>(level), z},
                    {2.0f, 2.0f, 2.0f}, false);
                box->setCFrame(box->cframe() * CFrame::angles(tilt, 0.3f * static_cast<float>(level), 0.0f));
                add(box);
            }
        }
    }

    void add(const std::shared_ptr<Part>& part)
    {
        keep.push_back(part);
        parts.push_back(part.get());
    }

    void cart(Vector3 at)
    {
        Workspace& ws = *game->workspace();
        auto body = addPart(ws, at + Vector3{0.0f, 2.5f, 0.0f}, {4.0f, 1.0f, 2.0f}, false);
        add(body);
        for (float side : {-1.0f, 1.0f}) {
            auto wheel = addPart(ws, at + Vector3{1.5f * side, 1.0f, 1.5f}, {1.0f, 2.0f, 2.0f}, false);
            wheel->setShape(Enum::PartType::Cylinder);
            add(wheel);
            const Vector3 axle = at + Vector3{1.5f * side, 1.0f, 1.0f};
            auto hinge = std::make_shared<HingeConstraint>();
            auto a0 = attach(*body, axle);
            auto a1 = attach(*wheel, axle);
            hinge->setAttachment0(a0.get());
            hinge->setAttachment1(a1.get());
            hinge->setActuatorType(Enum::ActuatorType::Motor);
            hinge->setAngularVelocity(2.0f * side);
            hinge->setMotorMaxTorque(500.0f);
            hinge->setParent(wheel.get());
            keep.insert(keep.end(), {a0, a1, hinge});
        }
        auto spring = std::make_shared<SpringConstraint>();
        auto s0 = attach(*body, at + Vector3{0.0f, 3.0f, 0.0f});
        auto s1 = attach(*parts[parts.size() - 2], at + Vector3{-1.5f, 1.0f, 1.5f});
        spring->setAttachment0(s0.get());
        spring->setAttachment1(s1.get());
        spring->setStiffness(200.0f);
        spring->setDamping(5.0f);
        spring->setFreeLength(2.0f);
        spring->setParent(body.get());
        keep.insert(keep.end(), {s0, s1, spring});
    }

    // Every part's CFrame and velocities, as bits.
    std::vector<std::uint32_t> state() const
    {
        std::vector<std::uint32_t> bits;
        auto put = [&](float v) { bits.push_back(std::bit_cast<std::uint32_t>(v)); };
        for (const BasePart* part : parts) {
            const CFrame cf = part->cframe();
            for (float v : {cf.position.x, cf.position.y, cf.position.z})
                put(v);
            for (const auto& row : cf.r)
                for (float v : row)
                    put(v);
            const Vector3 linear = part->assemblyLinearVelocity();
            const Vector3 angular = part->assemblyAngularVelocity();
            for (float v : {linear.x, linear.y, linear.z, angular.x, angular.y, angular.z})
                put(v);
        }
        return bits;
    }
};

void wakeAll(World& world)
{
    for (const auto& assembly : world.assemblies())
        if (!assembly->anchored)
            assembly->asleep = false;
}

} // namespace

TEST(physics_thread_pool)
{
    // Every index exactly once, for any count, round after round.
    ThreadPool pool(4);
    CHECK_EQ(pool.threads(), 4u);
    std::vector<std::atomic<int>> hits(64);
    std::vector<int> expected(64, 0);
    for (int round = 0; round < 2000; ++round) {
        const auto count = static_cast<std::size_t>(round % 65);
        pool.run(count, [&](std::size_t i) { hits[i].fetch_add(1, std::memory_order_relaxed); });
        for (std::size_t i = 0; i < count; ++i)
            ++expected[i];
    }
    for (std::size_t i = 0; i < hits.size(); ++i)
        CHECK_EQ(hits[i].load(), expected[i]);

    // A throw comes out of run once the others are done, and the pool goes on.
    bool caught = false;
    try {
        pool.run(100, [](std::size_t i) {
            if (i == 37)
                throw std::runtime_error("37");
        });
    } catch (const std::runtime_error& e) {
        caught = std::string_view(e.what()) == "37";
    }
    CHECK(caught);
    std::atomic<std::size_t> sum = 0;
    pool.run(100, [&](std::size_t i) { sum.fetch_add(i, std::memory_order_relaxed); });
    CHECK_EQ(sum.load(), 4950u);

    // Pools come and go, used or not.
    for (unsigned threads = 1; threads <= 8; ++threads) {
        ThreadPool shortLived(threads);
        if (threads % 2 == 0)
            shortLived.run(16, [&](std::size_t) {});
    }
    CHECK(ThreadPool::defaultThreads() >= 1);
}

TEST(physics_threads_deterministic)
{
    // The same scene on 1, 2, 3 and 8 threads, every step spread, ends up
    // the same to the bit, and so does one spread only when it pays.
    constexpr int steps = 360;
    auto run = [&](unsigned threads, bool always, std::size_t* parallelSteps, std::size_t* islands) {
        Scene scene(6);
        World world(*scene.game->workspace());
        world.setThreads(threads);
        world.setAlwaysParallel(always);
        std::vector<std::uint32_t> trace;
        std::size_t mostIslands = 0;
        for (int i = 0; i < steps; ++i) {
            world.step();
            mostIslands = std::max(mostIslands, world.stats().islands);
            if (i % 60 == 59) {
                const auto state = scene.state();
                trace.insert(trace.end(), state.begin(), state.end());
            }
        }
        if (parallelSteps)
            *parallelSteps = world.stats().parallelSteps;
        if (islands)
            *islands = mostIslands;
        return trace;
    };
    std::size_t islands = 0;
    const auto one = run(1, true, nullptr, &islands);
    CHECK(islands > 10);
    for (unsigned threads : {2u, 3u, 8u}) {
        std::size_t parallel = 0;
        CHECK(run(threads, true, &parallel, nullptr) == one);
        CHECK_EQ(parallel, static_cast<std::size_t>(steps));
    }
    CHECK(run(8, false, nullptr, nullptr) == one);
}

TEST(physics_small_worlds_stay_on_one_thread)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    addPart(ws, {0.0f, -0.5f, 0.0f}, {50.0f, 1.0f, 50.0f}, true);
    std::vector<std::shared_ptr<Part>> boxes;
    boxes.reserve(6);
    for (int i = 0; i < 6; ++i)
        boxes.push_back(addPart(ws, {static_cast<float>(i) * 3.0f, 1.5f, 0.0f}, {2.0f, 2.0f, 2.0f}, false));
    World world(ws);
    world.setThreads(8);
    for (int i = 0; i < 240; ++i)
        world.step();
    CHECK_EQ(world.threads(), 8u);
    CHECK_EQ(world.stats().steps, 240u);
    CHECK_EQ(world.stats().parallelSteps, 0u);
}

// LUNAR_PHYSICS_PARALLEL_BENCH=side (24 by default: 576 cells, about 1500
// bodies) runs the busy scene, kept awake, for LUNAR_PHYSICS_BENCH_SECONDS
// (10) on 1, 2, 4 and 8 threads, or those LUNAR_PHYSICS_BENCH_THREADS lists.
TEST(physics_parallel_bench)
{
    const char* sideText = std::getenv("LUNAR_PHYSICS_PARALLEL_BENCH"); // NOLINT(concurrency-mt-unsafe)
    if (!sideText)
        SKIP("set LUNAR_PHYSICS_PARALLEL_BENCH to run");
    int side = 24;
    std::from_chars(sideText, sideText + std::strlen(sideText), side);
    side = std::max(side, 1);
    float seconds = 10.0f;
    if (const char* s = std::getenv("LUNAR_PHYSICS_BENCH_SECONDS")) // NOLINT(concurrency-mt-unsafe)
        seconds = std::stof(s);
    std::vector<unsigned> counts{1, 2, 4, 8};
    if (const char* list = std::getenv("LUNAR_PHYSICS_BENCH_THREADS")) { // NOLINT(concurrency-mt-unsafe)
        counts.clear();
        for (const auto part : std::string_view(list) | std::views::split(','))
            counts.push_back(static_cast<unsigned>(std::stoi(std::string(part.begin(), part.end()))));
    }
    constexpr float dt = 1.0f / 60.0f;
    const int frames = static_cast<int>(seconds / dt);
    double single = 0.0;
    std::vector<std::uint32_t> first;
    for (unsigned threads : counts) {
        Scene scene(side);
        World world(*scene.game->workspace());
        world.setThreads(threads);
        world.refresh();
        double total = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            wakeAll(world);
            const auto start = std::chrono::steady_clock::now();
            world.advance(dt);
            total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        const double ms = total / frames * 1000.0;
        if (single == 0.0)
            single = ms;
        const World::Stats& stats = world.stats();
        std::println("{} threads: {:.3f} ms a frame, {:.2f}x; {} bodies, {} awake, {} islands, {} contacts, {} rows",
                     threads, ms, single / ms, scene.parts.size(), stats.awakeAssemblies, stats.islands, stats.contacts,
                     stats.rows);
        const auto state = scene.state();
        if (first.empty())
            first = state;
        CHECK(state == first);
    }
}
