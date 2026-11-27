#pragma once

#include <lunar/Math.hpp>
#include <lunar/terrain/TerrainMesher.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lunar {
class Terrain;
}

namespace lunar::terrain {

// Terrain level of detail, the way the client's smooth terrain does it: an
// octree of nodes over the terrain blocks, level L covering 2^L blocks per
// side with cells 2^L cells wide (NodeMesher). Every update picks, from the
// camera position, the coarsest level each part of the terrain may use at
// its distance, so full detail stays near the camera and the whole map still
// fits in a few hundred thousand triangles. Missing meshes are made on
// worker threads, nearest first; until they're in, whatever already covers
// the same ground (the parent, or the children) keeps drawing, so nothing
// opens up. Meshes nothing draws are dropped once the cache is over budget.
//
// From the binary: five levels (TerrainLodLevels 5) and the distances
// (lodDistance). GUESS: the octree of whole 32^3 blocks, the hysteresis
// band, skirts for the seams (production has
// FFlagSmoothClusterTransitionalChunks2 off; the client meshes each chunk's
// outer layer of cubes separately, sub_104a405f9, not rebuilt here), how
// coarse cells are downsampled and the budgets.
struct LodOptions {
    unsigned threads = 0; // mesher threads; 0 uses half the hardware threads
    int levels = 5;       // TerrainLodLevels: 0 (full detail) to levels - 1
    float distanceScale = 1.0f;
    // Bytes of meshes (CPU side, what gets uploaded) drawn at once; over it
    // the distances shrink until the selection fits. GUESS: the client
    // aims at TerrainGpuMeshMemoryTargetMib 32 with its packed vertices,
    // ours are about three times bigger.
    std::size_t drawBudget = std::size_t{128} << 20;
    // Bytes of meshes kept, drawn or not, before the least recently used go.
    std::size_t cacheBudget = std::size_t{384} << 20;
    // Bytes of newly finished meshes taken in per update (at least one), so
    // a frame never uploads a whole map at once.
    std::size_t admitPerUpdate = std::size_t{32} << 20;
    // Updates a level switch takes to dissolve from one mesh to the other
    // (the client's TerrainTransitionFrames); 0 switches at once.
    int transitionFrames = 15;
};

// Studs from the camera to the nearest point of a node, across the ground
// (height doesn't count), past which `level` may be drawn: 0, 240, 768, 1930 and 4485 for levels 0 to 4. The client's
// table, (2.2^level - 1) * 200 (TerrainUsePow2LodDistances off).
float lodDistance(int level);

class TerrainLod {
public:
    using MeshPtr = std::shared_ptr<const TerrainMeshChunk>;

    // A mesh to draw where a screen-space dither value in [0, 1) falls in
    // [ditherFrom, ditherTo): all of it once settled, part of it while it
    // fades in or out, the old and the new mesh in complementary halves.
    struct Draw {
        MeshPtr mesh;
        float ditherFrom = 0.0f;
        float ditherTo = 1.0f;
    };

    // The terrain must outlive this and not change while it exists.
    explicit TerrainLod(const Terrain& terrain, LodOptions options = {});
    TerrainLod(const TerrainLod&) = delete;
    TerrainLod& operator=(const TerrainLod&) = delete;
    ~TerrainLod();

    // Takes in finished meshes, picks what to draw for a camera at `eye` and
    // queues meshing for what it lacks. True if the selection changed,
    // which it does every update while a level switch fades.
    bool update(Vector3 eye);
    // Non-empty meshes to draw, in no particular order.
    const std::vector<Draw>& selection() const { return selection_; }
    // The last update drew everything at the level it wanted.
    bool settled() const { return settled_; }
    // Updates until settled for `eye`, waiting for the workers, and skips
    // the fades.
    void settle(Vector3 eye);

    struct Stats {
        std::array<std::size_t, 8> nodes{}; // drawn per level, empty ones left out
        std::size_t triangles = 0;          // drawn
        std::size_t drawnBytes = 0;
        std::size_t cachedBytes = 0; // every mesh held, drawn or not
        std::size_t queued = 0;
        float distanceScale = 1.0f; // after shrinking to the draw budget
    };
    Stats stats() const;

private:
    enum class State : std::uint8_t {
        Idle,    // no mesh, not asked for
        Queued,  // waiting for or with a worker
        Arrived, // meshed, not taken in yet
        Ready,
    };
    struct Node {
        State state = State::Idle;
        bool split = false; // drawn as its children last time
        MeshPtr mesh;       // Arrived and Ready; null when empty
        std::size_t bytes = 0;
        std::uint64_t lastUsed = 0;  // last update that drew or wanted it
        std::uint64_t fadeStart = 0; // update it started fading in; 0 when it isn't
    };
    struct Fade {
        MeshPtr mesh; // no longer drawn, fading out
        std::uint64_t start = 0;
    };
    struct Request {
        float priority;
        std::uint64_t key;
    };

    bool select(std::uint64_t key, int level, std::array<int, 3> position);
    bool drawReadyBelow(int level, std::array<int, 3> position);
    void draw(std::uint64_t key, Node& node);
    void startFades();
    float fadeProgress(std::uint64_t start) const;
    bool makeSelection();
    void want(std::uint64_t key, Node& node, float distance, int level);
    float distanceTo(int level, std::array<int, 3> position) const;
    void takeArrivals();
    void requeue();
    void evict();
    void work(const std::stop_token& stop);

    const Terrain& terrain_;
    LodOptions options_;
    std::unordered_map<std::uint64_t, Node> nodes_; // every node with terrain under it
    std::vector<std::uint64_t> roots_;
    std::vector<std::uint64_t> chosen_; // keys of the non-empty nodes drawn
    std::vector<Fade> fadingOut_;
    std::vector<Draw> selection_;
    std::vector<std::uint64_t> drawn_; // keys drawn, to spot selection changes
    std::vector<std::uint64_t> drawnBefore_;
    std::vector<Request> requests_;
    std::vector<std::uint64_t> arrived_; // in arrival order
    Vector3 eye_;
    float scale_ = 1.0f;
    std::uint64_t frame_ = 0;
    std::size_t drawnBytes_ = 0;
    std::size_t cachedBytes_ = 0;
    bool settled_ = false;

    // Shared with the workers.
    mutable std::mutex mutex_;
    std::condition_variable_any work_;
    std::condition_variable arrivals_;
    std::vector<Request> queue_; // farthest first: workers take from the back
    std::unordered_set<std::uint64_t> inFlight_;
    std::vector<std::pair<std::uint64_t, MeshPtr>> done_;
    std::vector<std::jthread> workers_; // last: stopped and joined first
};

} // namespace lunar::terrain
