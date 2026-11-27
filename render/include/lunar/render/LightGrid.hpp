#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/LocalLights.hpp>
#include <lunar/render/Scene.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::render {

// A voxel or chunk position in whole cells, in world space.
struct GridPoint {
    int x = 0;
    int y = 0;
    int z = 0;

    friend constexpr bool operator==(const GridPoint&, const GridPoint&) = default;
};

// The client's voxel light grid (RBX::Graphics::LightGridCPU in the 0.742
// macOS player): how much open sky and how much sun reaches each 4-stud
// voxel around the camera, which its shaders sample to darken shaded and
// indoor places (LightGridSkylightTexture: red the sky, green the sun).
//
// The grid is made of chunks of 32 x 16 x 32 voxels (x, y, z), 8 x 4 x 8 of
// them on machines with enough memory (4 x 4 x 4 otherwise), placed by whole
// chunks around the camera's chunk. Every voxel keeps three occupancies,
// one per world axis: how much of the voxel's cross-section seen along
// that axis is blocked (0-255). Parts add theirs, times their opacity, the
// way the client's voxelizer does: per box-shaped piece, the fractions of
// the voxel it overlaps along each of its axes, multiplied in pairs into
// projected areas (RBX::Voxel::Voxelizer, sub_1051b6470). Then, layer by
// layer from the top down (sub_10485a74a, sub_10485f3e8):
//
//   above = (the 8 voxels around this one in the layer above, summed, + 7) / 8
//   sky   = 252 - (255 - above)^4 / 2^24               (what the shaders see)
//   pass  = max(above - max(occX * 180 / 256, occY, occZ * 180 / 256), 0)
//
// with full sky (255) above the grid and around it. Lunarstone's own
// addition: where floors and roofs above shut off the sky overhead, a voxel
// shows the light that came in from the side less its walls (max(occX,
// occZ) * 180 / 256), so the open sky beyond a room's outer wall stays out
// of the room's side of it; what's carried down is the client's. The sun is
// carried the same way along the sun's direction (sub_10485a2fe): each voxel
// takes its three neighbours towards the sun, weighted by how much of the
// light comes through each face, each less what its occupancy holds back
// seen along the sun. With GlobalShadows off both stay full everywhere.
//
// Updates are incremental like the client's: parts are checked a slice per
// frame, and chunks whose parts changed are voxelized again on a worker
// thread, then the light is carried again from the voxels whose occupancy
// changed, as far as what it carries changes (the same light as carrying
// all of it, bit for bit); chunks whose light changed are handed out for
// upload.
class LightGrid {
public:
    static constexpr float voxelStuds = 4.0f;
    static constexpr int chunkX = 32;
    static constexpr int chunkY = 16;
    static constexpr int chunkZ = 32;
    static constexpr std::size_t chunkVoxels = std::size_t{chunkX} * chunkY * chunkZ;

    // What the light depends on besides the parts.
    struct Settings {
        Vector3 sunDirection{0.0f, 1.0f, 0.0f}; // towards the sun
        bool shadows = true;                    // Lighting.GlobalShadows
    };

    // A chunk's texels for the GPU: red the sky coming into a voxel at its
    // top, green sun, blue the sky leaving at its bottom, alpha the height
    // of what's in between (Chunk::blockerHeight; the client keeps only red
    // and green); x fastest, then z,
    // then y (the client's chunk layout, which its 3D texture keeps: u is
    // world x, v world z and w world y); and its local light in the same
    // order, RGBA: the colour, and its strength over 120 in alpha (the
    // client's LightMapTexture).
    struct ChunkTexels {
        GridPoint chunk; // world chunk position
        std::span<const std::uint8_t> texels;
        std::span<const std::uint8_t> light;
    };

    // chunks: the grid's size in chunks; the client's is 8 x 4 x 8.
    explicit LightGrid(GridPoint chunks = {8, 4, 8});
    LightGrid(const LightGrid&) = delete;
    LightGrid& operator=(const LightGrid&) = delete;
    ~LightGrid();

    // Builds everything around `center` now: every chunk voxelized and the
    // light carried, waiting for it.
    void build(std::span<const DrawItem> items, Vector3 center, const Settings& settings,
               std::span<const LocalLight> lights = {});
    // Once a frame: follows the camera, checks a slice of the parts for
    // changes and the lights for moves, and starts (or collects) background
    // work. The first call builds everything. Returns whether there are new
    // texels to upload.
    bool update(std::span<const DrawItem> items, Vector3 center, const Settings& settings,
                std::span<const LocalLight> lights = {});
    // Waits for any background work and takes in what it did.
    void wait();
    // How soon after a pass over the parts began the next one may: what a
    // part does shows in the light up to that much later, and a scene that
    // never stops moving voxelizes its chunks that much less often. 0 looks
    // every frame.
    void setScanInterval(std::chrono::milliseconds interval) { scanInterval_ = interval; }
    // Chunks whose texels changed since the last call. The spans stay valid
    // until the next update or build.
    std::vector<ChunkTexels> takeUploads();

    // The grid's size in voxels, and the world voxel at its low corner.
    GridPoint voxels() const { return {chunks_.x * chunkX, chunks_.y * chunkY, chunks_.z * chunkZ}; }
    GridPoint chunks() const { return chunks_; }
    GridPoint originVoxel() const { return {origin_.x * chunkX, origin_.y * chunkY, origin_.z * chunkZ}; }
    // Whether any light has been worked out yet (and GlobalShadows is on).
    bool ready() const { return built_ && settings_.shadows; }
    // Whether the local light has been worked out (it is with or without
    // GlobalShadows).
    bool lightReady() const { return built_; }

    // At a world voxel inside the grid (outside: 0 occupancy, full light).
    std::uint8_t occupancy(int axis, GridPoint voxel) const;
    std::uint8_t sky(GridPoint voxel) const;      // what comes in at the voxel's top
    std::uint8_t skyBelow(GridPoint voxel) const; // ...and what of it leaves at its bottom
    std::uint8_t sun(GridPoint voxel) const;
    // The local light at a world voxel, as the shaders decode it (0 outside).
    Color3 light(GridPoint voxel) const;
    // The world voxel holding a point.
    static GridPoint voxelAt(Vector3 point);

private:
    // One thing that blocks light, as the voxelizer sees it.
    struct Occluder {
        enum class Shape : std::uint8_t { Box, Wedge, CornerWedge, Cylinder, CylinderY, Ball, Mesh, Surface };
        Mat4 model;
        MeshPtr mesh;                                     // Mesh and Surface: its boxes or its triangles
        std::shared_ptr<const std::vector<Bounds>> boxes; // Mesh: given boxes, else the mesh's
        float opacity = 1.0f;
        Shape shape = Shape::Box;
    };
    // A light's strength at each voxel of a chunk's region (its box there)
    // as lightChunk last worked it out, occlusion and all, what that came
    // from (everything of the light but its colour and brightness: shape),
    // and the colour it was added in with. A chunk keeps one for each
    // light that reached it, in the lights' order, the strengths themselves
    // once it has been lit more than once (within keptStrengthBudget).
    struct KeptStrength {
        const Instance* key = nullptr;
        std::uint64_t shape = 0;
        GridPoint low, high;         // the region, inclusive, in world voxels
        Color3 color;                // LocalLight::gridColor
        std::vector<float> strength; // y, then z, then x; 0 where it gives none; empty if not kept
    };
    struct Chunk {
        GridPoint coord{0x7fffffff, 0, 0};   // world chunk position it holds
        std::vector<std::uint8_t> occupancy; // three planes: x, y, z
        // How high in each voxel what blocks light from above sits, 0 its
        // bottom to 255 its top (the mean of the blockers' middles, weighted
        // by how much they block); 128 with nothing there. Lunarstone's own.
        std::vector<std::uint8_t> blockerHeight;
        std::vector<std::uint8_t> texels;    // sky, sun, sky below, blocker height
        std::vector<std::uint8_t> light;     // local light, RGBA
        // Which voxels the last voxelizing changed the occupancy of: a word
        // per row (z) of each layer (y), a bit per voxel across it (x).
        std::vector<std::uint32_t> occupancyChanged;
        std::vector<Occluder> occluders;
        std::uint64_t signature = 0;
        std::uint64_t lightSignature = 0; // of the lights reaching it, when its light was worked out
        std::vector<KeptStrength> kept;   // what each light added at its last lighting, in order
        bool recorded = false;            // kept describes what light holds (else it's lit in full)
        std::uint32_t relights = 0;       // since it came into the grid
        bool voxelize = false;   // occupancy is out of date
        bool relight = false;    // local light is out of date
        bool changed = false;    // texels to upload
    };
    // A full pass over the parts, spread over frames.
    struct Scan {
        std::size_t next = 0;
        std::size_t itemCount = 0;
        GridPoint origin;
        bool active = false;
        std::vector<std::vector<std::uint32_t>> members; // items per grid slot
        std::vector<std::uint64_t> signatures;
    };
    // Boxes standing in for a mesh, in its own space.
    struct MeshBoxes {
        MeshPtr mesh; // keeps the key's address from being reused
        std::vector<Bounds> boxes;
    };

    std::size_t slotOf(GridPoint chunk) const;
    GridPoint originFor(Vector3 center) const;
    void beginScan(std::size_t itemCount);
    void scanItems(std::span<const DrawItem> items, std::size_t end);
    void finishScan(std::span<const DrawItem> items);
    static bool occluderFor(const DrawItem& item, Occluder& out);
    void moveTo(GridPoint origin);
    void startWork(bool wait);
    void passLoop(const std::stop_token& stop);
    void work(bool sky, bool sun);
    // What of a shadowed light reaches each voxel of the box around it
    // (radius voxels each way from center), 0 to 255.
    struct Occlusion {
        GridPoint center;
        int radius = 0;
        std::vector<float> light; // y, then z, then x
    };
    // The chunks (inclusive) a light's voxels and their blur reach in the
    // grid; false for none.
    bool chunksReached(const LocalLight& light, GridPoint& first, GridPoint& last) const;
    // Marks the chunks whose lights changed (or whose occupancy will).
    void checkLights(std::span<const LocalLight> lights);
    Occlusion occlusionOf(const LocalLight& light) const;
    // Works out a chunk's local light from lights_ (each stopped by its
    // occlusion, where it has one) and the occupancy.
    void lightChunk(Chunk& chunk, std::span<const Occlusion* const> occlusion);
    void voxelizeChunk(Chunk& chunk);
    // Voxels of the grid by layer (y), a bit each: rows (z) of rowWords()
    // words, 64 voxels (x) to a word.
    using LayerMask = std::vector<std::uint64_t>;
    struct Changes {
        std::vector<LayerMask> layers;
        int lowest = 0, highest = -1; // the layers with any, none when highest < lowest
    };
    std::size_t rowWords() const { return (static_cast<std::size_t>(voxels().x) + 63) / 64; }
    // The voxelized chunks' occupancyChanged, as grid layers.
    Changes occupancyChanges(std::span<Chunk* const> chunks) const;
    // Carry all of the light.
    void carrySky();
    void carrySun();
    // Carry it again from where the occupancy changed, as far as what's
    // carried changes; slots whose texels changed are set in touched.
    void carrySkyFrom(const Changes& changes, std::vector<std::uint8_t>& touched);
    void carrySunFrom(const Changes& changes, std::vector<std::uint8_t>& touched);
    // Into the chunks' texels: every chunk, or the slots set in only.
    void storeTexels(const std::vector<std::uint8_t>* only = nullptr);
    const std::vector<Bounds>& boxesOf(const MeshPtr& mesh);

    GridPoint chunks_;
    GridPoint origin_{0x7fffffff, 0, 0}; // world chunk at the low corner
    Settings settings_;
    Settings litWith_; // what the light was last carried with
    bool built_ = false;
    std::vector<Chunk> slots_; // by world chunk, wrapped around the grid
    // The light as carried, by grid voxel ([y][z][x]); copied into chunk
    // texels when it's done.
    std::vector<std::uint8_t> skyGrid_;
    std::vector<std::uint8_t> sunGrid_;
    std::vector<std::uint8_t> skyBelowGrid_; // the sky leaving each voxel downwards
    // What each voxel passes on, kept to carry the light again from part way:
    // the sky down (carrySky's out), the sky straight overhead and the sun.
    std::vector<std::uint8_t> skyCarried_;
    std::vector<std::uint8_t> overheadGrid_;
    std::vector<std::uint8_t> sunCarried_;
    Scan scan_;
    bool scanPending_ = false; // a finished scan waits for the worker
    std::chrono::milliseconds scanInterval_{100};    // GUESS: often enough for 4-stud voxels
    std::chrono::steady_clock::time_point nextScan_; // when the next pass over the parts may begin
    bool skyDirty_ = false;
    bool sunDirty_ = false;
    // The lights the worker lights chunks with (a copy: it runs on), and
    // which of them reach each slot.
    std::vector<LocalLight> lights_;
    // Shadowed lights' occlusion from pass to pass (the worker's alone): a
    // light's changes only with where it is, its range and the occupancy
    // around it, and a light that flickers has its chunks lit again pass
    // after pass, every shadowed light reaching them with it.
    struct KeptOcclusion {
        Vector3 position;
        float range = 0.0f;
        GridPoint origin;
        std::uint64_t occupancy = 0; // occupancyGeneration_ it was worked out at
        Occlusion field;
    };
    std::unordered_map<const Instance*, KeptOcclusion> keptOcclusion_;
    std::uint64_t occupancyGeneration_ = 0; // moves on with any voxelizing
    // What the chunks' kept strengths take, and the most they may.
    static constexpr std::size_t keptStrengthBudget = std::size_t{48} << 20;
    std::atomic<std::size_t> keptStrengthBytes_{0};
    // What a light's strengths in lightChunk depend on, but its colour and
    // brightness: where it is and points, its range and shape, and for a
    // shadowed one the occupancy its occlusion was swept through.
    std::uint64_t strengthShape(const LocalLight& light, bool shadowed) const;
    // The thread the background passes run on, kept from pass to pass:
    // startWork hands it a pass (passSky_, passSun_) and it clears working_
    // when it's done.
    std::mutex passMutex_;
    std::condition_variable_any passQueued_; // the thread waits here for a pass
    std::condition_variable_any passDone_;   // and wait() for it to finish one
    bool passPending_ = false;
    bool passSky_ = false, passSun_ = false;
    std::atomic<bool> working_{false};
    std::jthread worker_; // stopped and joined before the rest of the members above
    // Mesh bounds for the scan (main thread), and the worker's mesh boxes.
    std::unordered_map<const MeshData*, std::pair<MeshPtr, Bounds>> bounds_;
    std::mutex meshMutex_;
    std::unordered_map<const MeshData*, MeshBoxes> meshBoxes_;
};

} // namespace lunar::render
