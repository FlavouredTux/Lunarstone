#include <lunar/terrain/TerrainMesher.hpp>

#include <lunar/classes/Services.hpp>
#include <lunar/classes/World.hpp>
#include <lunar/terrain/SmoothMesher.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <utility>

// The client's own mesher (SmoothMesher) run block by block. A block meshes
// the grid edges starting at its own cells, so it reads its 32^3 cells and
// two more on every side: the vertices of the cubes around its edges reach
// one cell out, and water levels look one further. Vertices depend only on
// the cells around them, so blocks sharing a border build bit-identical
// vertices there and there are no cracks. Physics meshes collision through
// the same blocks (meshChunk), as the client's does through the same
// function.
//
// Far away the same runs on coarser grids (level of detail): a level L node
// covers 2^L blocks per side with 32^3 cells 2^L cells wide, each standing
// for the cells it covers, and the client's mesher takes the cell width into
// account. Neighbours at the same level still match; where levels meet,
// skirts hanging off the open border edges hide the cracks.

namespace lunar::terrain {

namespace {

constexpr int blockSize = TerrainMeshChunk::size;
constexpr float studsPerCell = 4.0f;

// The box: cells -2 to 33 of the block.
constexpr int boxMin = -2;
constexpr int boxSide = blockSize + 4;
constexpr std::size_t boxCount = std::size_t{boxSide} * boxSide * boxSide;

constexpr std::size_t boxIndex(int x, int y, int z)
{
    // Always small and non-negative: the arguments stay within the box.
    const int index = (x - boxMin) + boxSide * ((z - boxMin) + boxSide * (y - boxMin));
    return static_cast<std::size_t>(index);
}

constexpr int floorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

using Int3 = std::array<int, 3>;

// Use2022Materials of the place's MaterialService (the engine default when
// there's none), which picks the client's material table and with it how
// materials deform the surface.
bool use2022Materials(const Terrain& terrain)
{
    const Instance* root = &terrain;
    while (root->parent())
        root = root->parent();
    if (const auto* service = dynamic_cast<const MaterialService*>(root->findFirstChild("MaterialService")))
        return service->use2022Materials();
    return MaterialService().use2022Materials();
}

// Per thread scratch space for meshing blocks one after another.
class BlockMesher {
public:
    explicit BlockMesher(const Terrain& terrain)
        : terrain_(terrain), deforms_(materialDeforms(use2022Materials(terrain))), cells_(boxCount)
    {
        for (std::size_t i = 0; i < Terrain::materials.size(); ++i)
            colors_[i] = terrain.getMaterialColor(Terrain::materials[i]);
        waterColor_ = terrain.waterColor();
    }

    TerrainMeshChunk mesh(Int3 position, int level = 0, bool skirts = false)
    {
        TerrainMeshChunk out;
        out.position = position;
        out.level = static_cast<std::uint8_t>(level);
        scale_ = 1 << level;
        base_ = {position[0] * blockSize, position[1] * blockSize, position[2] * blockSize};
        if (level == 0)
            gather(position);
        else
            gatherCoarse();
        SmoothBox box{
            .size = {boxSide, boxSide, boxSide},
            .cells = cells_,
            .origin = {(base_[0] + boxMin) * scale_, (base_[1] + boxMin) * scale_, (base_[2] + boxMin) * scale_},
            .lod = level,
        };
        solid_.vertices.clear();
        solid_.indices.clear();
        water_.vertices.clear();
        water_.indices.clear();
        const SmoothContents contents = mesher_.mesh(box, SmoothPass::Solid, deforms_, solid_);
        if (contents.water)
            mesher_.mesh(box, SmoothPass::Water, deforms_, water_);
        removeOrphans(solid_);
        removeOrphans(water_);
        convert(solid_, out.solid, false);
        assignPalette(out);
        if (skirts)
            // Two of its own cells: enough to reach down to a neighbour one level coarser.
            addSkirts(out.solid, 2.0f * studsPerCell * static_cast<float>(scale_));
        convert(water_, out.water, true);
        // Big maps keep tens of thousands of these around.
        for (MeshData* mesh : {&out.solid, &out.water}) {
            mesh->vertices.shrink_to_fit();
            mesh->indices.shrink_to_fit();
        }
        computeBounds(out);
        return out;
    }

private:
    // Fills the box from the block and its 26 neighbours.
    void gather(Int3 position)
    {
        std::ranges::fill(cells_, SmoothCell{});
        for (int oy = -1; oy <= 1; ++oy)
            for (int oz = -1; oz <= 1; ++oz)
                for (int ox = -1; ox <= 1; ++ox) {
                    const TerrainChunk* chunk
                        = terrain_.findChunk(position[0] + ox, position[1] + oy, position[2] + oz);
                    if (!chunk)
                        continue;
                    // The part of the neighbour inside the box, in its own cells.
                    auto range = [](int offset) {
                        return offset < 0   ? std::pair{blockSize + boxMin, blockSize}
                               : offset > 0 ? std::pair{0, boxSide + boxMin - blockSize}
                                            : std::pair{0, blockSize};
                    };
                    auto [x0, x1] = range(ox);
                    auto [y0, y1] = range(oy);
                    auto [z0, z1] = range(oz);
                    copyRuns(*chunk, {x0, y0, z0}, {x1, y1, z1}, {ox * blockSize, oy * blockSize, oz * blockSize});
                }
    }

    // Copies cells lo to hi (exclusive) of a chunk to the box, shifted by
    // offset. Rows run along X in storage order, so one pass over the runs does.
    void copyRuns(const TerrainChunk& chunk, Int3 lo, Int3 hi, Int3 offset)
    {
        auto run = chunk.runs.begin();
        int runStart = 0;
        for (int y = lo[1]; y < hi[1]; ++y)
            for (int z = lo[2]; z < hi[2]; ++z) {
                int index = lo[0] + blockSize * (z + blockSize * y);
                for (int x = lo[0]; x < hi[0]; ++x, ++index) {
                    while (run != chunk.runs.end() && index >= runStart + run->count) {
                        runStart += run->count;
                        ++run;
                    }
                    if (run == chunk.runs.end())
                        return;
                    cells_[boxIndex(x + offset[0], y + offset[1], z + offset[2])]
                        = {run->material, run->occupancy, run->water};
                }
            }
    }

    // The coarse version of gather(): each cell stands for the scale_^3
    // cells under it. GUESS: the client downsamples its grid into mips
    // (Voxel2::GridDownsampleJob) by rules not rebuilt here; this keeps the
    // surface where the fine cells put it on average. The occupancy is the
    // mean fill, as a distance in coarse cells; the material is the one
    // covering most of the highest solid layer, the one that shows (by
    // volume the ground underneath would win and paint over paths); a cell
    // with more water than solid is water.
    void gatherCoarse()
    {
        constexpr std::size_t materialCount = Terrain::materials.size();
        sums_.assign(boxCount, {});
        coverage_.assign(boxCount * materialCount, 0.0f);
        Int3 fineMin{}, fineMax{}; // the cells under the box, max exclusive
        for (std::size_t a = 0; a < 3; ++a) {
            fineMin[a] = (base_[a] + boxMin) * scale_;
            fineMax[a] = (base_[a] + boxMin + boxSide) * scale_;
        }
        for (int cy = floorDiv(fineMin[1], blockSize); cy <= floorDiv(fineMax[1] - 1, blockSize); ++cy)
            for (int cz = floorDiv(fineMin[2], blockSize); cz <= floorDiv(fineMax[2] - 1, blockSize); ++cz)
                for (int cx = floorDiv(fineMin[0], blockSize); cx <= floorDiv(fineMax[0] - 1, blockSize); ++cx)
                    if (const TerrainChunk* chunk = terrain_.findChunk(cx, cy, cz)) {
                        const Int3 origin{cx * blockSize, cy * blockSize, cz * blockSize};
                        Int3 lo{}, hi{};
                        for (std::size_t a = 0; a < 3; ++a) {
                            lo[a] = std::clamp(fineMin[a] - origin[a], 0, blockSize);
                            hi[a] = std::clamp(fineMax[a] - origin[a], 0, blockSize);
                        }
                        accumulate(*chunk, origin, lo, hi);
                    }

        const auto cells = static_cast<float>(scale_ * scale_ * scale_);
        auto toByte = [](float fill) {
            return static_cast<std::uint8_t>(std::clamp(std::lround(fill * 256.0f) - 1, 0L, 255L));
        };
        for (std::size_t i = 0; i < boxCount; ++i) {
            const auto [solid, water, top] = sums_[i];
            SmoothCell& c = cells_[i];
            c = {};
            if (solid > 0.0f && solid >= water) {
                const std::span<const float> covered(coverage_.data() + i * materialCount, materialCount);
                c.material = static_cast<std::uint8_t>(std::ranges::max_element(covered) - covered.begin());
                c.occupancy = toByte(solid / cells);
                if (water > 0.0f && c.occupancy < 255)
                    c.water = toByte(std::min(1.0f, (solid + water) / cells));
            } else if (water > 0.0f) {
                c.material = 1;
                c.occupancy = toByte(std::min(1.0f, water / cells));
            }
        }
    }

    // Adds a chunk's cells lo to hi (local, exclusive) to the coarse sums.
    void accumulate(const TerrainChunk& chunk, Int3 origin, Int3 lo, Int3 hi)
    {
        if (lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2])
            return;
        // The box cell a cell falls in, along one axis.
        auto cellOf = [&](std::size_t axis, int cell) { return floorDiv(origin[axis] + cell, scale_) - base_[axis]; };
        if (chunk.runs.size() == 1) {
            // All one voxel (common underground): whole box cells at a time.
            Int3 first{}, last{};
            for (std::size_t a = 0; a < 3; ++a) {
                first[a] = cellOf(a, lo[a]);
                last[a] = cellOf(a, hi[a] - 1);
            }
            auto overlap = [&](std::size_t axis, int cell) {
                const int start = (base_[axis] + cell) * scale_ - origin[axis];
                return std::min(hi[axis], start + scale_) - std::max(lo[axis], start);
            };
            for (int y = first[1]; y <= last[1]; ++y)
                for (int z = first[2]; z <= last[2]; ++z)
                    for (int x = first[0]; x <= last[0]; ++x) {
                        const std::size_t cell = boxIndex(x, y, z);
                        const int top = std::min(hi[1], (base_[1] + y + 1) * scale_ - origin[1]) - 1;
                        add(cell, chunk.runs.front(), overlap(0, x) * overlap(1, y) * overlap(2, z));
                        cover(cell, chunk.runs.front(), origin[1] + top, overlap(0, x) * overlap(2, z));
                    }
            return;
        }
        auto run = chunk.runs.begin();
        int runStart = 0;
        for (int y = lo[1]; y < hi[1]; ++y)
            for (int z = lo[2]; z < hi[2]; ++z) {
                const int cy = cellOf(1, y), cz = cellOf(2, z);
                const int rowStart = blockSize * (z + blockSize * y);
                int index = rowStart + lo[0];
                const int rowEnd = rowStart + hi[0];
                while (index < rowEnd) {
                    while (run != chunk.runs.end() && index >= runStart + run->count) {
                        runStart += run->count;
                        ++run;
                    }
                    if (run == chunk.runs.end())
                        return;
                    // The rest of the run in this row, cut at box cell borders.
                    const int pieceEnd = std::min(rowEnd, runStart + run->count);
                    const int cx = cellOf(0, index - rowStart);
                    const int cellEnd = rowStart + (base_[0] + cx + 1) * scale_ - origin[0];
                    const int end = std::min(pieceEnd, cellEnd);
                    const std::size_t cell = boxIndex(cx, cy, cz);
                    add(cell, *run, end - index);
                    cover(cell, *run, origin[1] + y, end - index);
                    index = end;
                }
            }
    }

    void add(std::size_t cell, const TerrainChunk::Run& run, int cells)
    {
        if (run.material == 0 || cells <= 0)
            return;
        const auto count = static_cast<float>(cells);
        const float occupancy = static_cast<float>(run.occupancy + 1) / 256.0f * count;
        Sums& sums = sums_[cell];
        if (run.material == 1) {
            sums.water += occupancy;
            return;
        }
        sums.solid += occupancy;
        if (run.water != 0)
            sums.water += static_cast<float>(run.water + 1) / 256.0f * count;
    }

    // Counts solid cells at height y towards the cell's material if they're
    // as high as any seen in it, starting over when they're higher.
    void cover(std::size_t cell, const TerrainChunk::Run& run, int y, int cells)
    {
        if (run.material < 2 || cells <= 0)
            return;
        constexpr std::size_t count = Terrain::materials.size();
        Sums& sums = sums_[cell];
        if (y < sums.top)
            return;
        if (y > sums.top) {
            sums.top = y;
            std::fill_n(coverage_.begin() + static_cast<std::ptrdiff_t>(cell * count), count, 0.0f);
        }
        coverage_[cell * count + run.material] += static_cast<float>(run.occupancy + 1) * static_cast<float>(cells);
    }

    // The mesher makes a vertex for every cube with a crossed edge, also on
    // the rim of the box where the quads using it belong to the neighbouring
    // block; drop those, as the client's renderer does
    // (FFlagTerrainRemoveOrphanedVertices).
    void removeOrphans(SmoothMesh& mesh)
    {
        remap_.assign(mesh.vertices.size(), 0);
        for (std::uint32_t i : mesh.indices)
            remap_[i] = 1;
        std::uint32_t kept = 0;
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
            if (remap_[i] != 0) {
                mesh.vertices[kept] = mesh.vertices[i];
                remap_[i] = kept++;
            }
        mesh.vertices.resize(kept);
        for (std::uint32_t& i : mesh.indices)
            i = remap_[i];
    }

    // Turns the mesher's vertices into ours: studs, area weighted normals
    // (the client's renderer sums its triangles' unnormalized normals,
    // 0x10515a729, and makes empty ones point up), the material colour, and
    // for solid vertices the palette weights (set by assignPalette).
    void convert(const SmoothMesh& in, MeshData& out, bool water)
    {
        out.vertices.resize(in.vertices.size());
        out.indices = in.indices;
        normals_.assign(in.vertices.size(), Vector3{});
        for (std::size_t t = 0; t + 2 < in.indices.size(); t += 3) {
            const Vector3 a = in.vertices[in.indices[t]].position, b = in.vertices[in.indices[t + 1]].position,
                          c = in.vertices[in.indices[t + 2]].position;
            const Vector3 n = (b - a).cross(c - a);
            for (std::size_t k = 0; k < 3; ++k)
                normals_[in.indices[t + k]] = normals_[in.indices[t + k]] + n;
        }
        for (std::size_t i = 0; i < in.vertices.size(); ++i) {
            const SmoothVertex& v = in.vertices[i];
            const float length = std::sqrt(normals_[i].dot(normals_[i]));
            out.vertices[i] = {
                .position = v.position * studsPerCell,
                .normal = length > 0.0f ? normals_[i] / length : Vector3{0.0f, 1.0f, 0.0f},
                .u = 0.0f,
                .v = 0.0f,
                .color = water || v.material >= colors_.size() ? waterColor_ : colors_[v.material],
            };
        }
    }

    // Each solid vertex carries one material, as in the client, where the
    // shader blends a triangle's three. The palette is the eight materials
    // with the most vertices; the rest fold into the first.
    void assignPalette(TerrainMeshChunk& out)
    {
        constexpr std::size_t count = Terrain::materials.size();
        std::array<std::size_t, count> totals{};
        for (const SmoothVertex& v : solid_.vertices)
            if (v.material < count)
                ++totals[v.material];
        std::array<std::uint8_t, count> order{};
        for (std::size_t i = 0; i < count; ++i)
            order[i] = static_cast<std::uint8_t>(i);
        std::ranges::stable_sort(order, std::greater{}, [&](std::uint8_t m) { return totals[m]; });
        std::size_t used = 0;
        while (used < TerrainMeshChunk::paletteCapacity && totals[order[used]] > 0)
            ++used;
        std::ranges::sort(std::span(order).first(used));
        std::array<std::uint8_t, count> slotOf{};
        for (std::size_t slot = 0; slot < used; ++slot) {
            out.palette[slot] = order[slot];
            slotOf[order[slot]] = static_cast<std::uint8_t>(slot);
        }
        out.paletteSize = static_cast<std::uint8_t>(used);
        for (std::size_t i = 0; i < solid_.vertices.size(); ++i) {
            const std::uint8_t material = solid_.vertices[i].material;
            out.solid.vertices[i].materialWeights = {};
            out.solid.vertices[i].materialWeights[material < count ? slotOf[material] : 0] = 255;
        }
    }

    // Hangs a strip `depth` studs deep (against the vertex normals, into the
    // ground) off every open edge of the mesh. The mesh closes up everywhere
    // but at the block's border, so those are the border edges, and a strip
    // there covers the gap to a neighbour meshed coarser or finer. Skirt
    // vertices copy their edge's normal and materials, so they shade like
    // the edge they hang from.
    static void addSkirts(MeshData& mesh, float depth)
    {
        struct Edge {
            std::uint64_t key;      // both ends, lower first
            std::uint32_t from, to; // as its triangle winds it
        };
        std::vector<Edge> edges;
        edges.reserve(mesh.indices.size());
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t a = mesh.indices[t + k], b = mesh.indices[t + (k + 1) % 3];
                edges.push_back({(std::uint64_t{std::min(a, b)} << 32) | std::max(a, b), a, b});
            }
        std::ranges::sort(edges, {}, &Edge::key);
        std::vector<std::uint32_t> skirtOf(mesh.vertices.size(), 0); // index + 1 of the lowered copy
        auto lowered = [&](std::uint32_t v) {
            if (skirtOf[v] == 0) {
                MeshVertex copy = mesh.vertices[v];
                copy.position = copy.position - copy.normal * depth;
                mesh.vertices.push_back(copy);
                skirtOf[v] = static_cast<std::uint32_t>(mesh.vertices.size());
            }
            return skirtOf[v] - 1;
        };
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i + 1;
            while (j < edges.size() && edges[j].key == edges[i].key)
                ++j;
            if (j == i + 1) {
                // Wound like the triangle it borders, so it faces outwards too.
                const auto [key, a, b] = edges[i];
                const std::uint32_t la = lowered(a), lb = lowered(b);
                mesh.indices.insert(mesh.indices.end(), {a, la, b, b, la, lb});
            }
            i = j;
        }
    }

    static void computeBounds(TerrainMeshChunk& out)
    {
        constexpr float inf = std::numeric_limits<float>::infinity();
        Vector3 lo{inf, inf, inf}, hi{-inf, -inf, -inf};
        for (const MeshData* mesh : {&out.solid, &out.water})
            for (const MeshVertex& v : mesh->vertices) {
                lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
                hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
            }
        if (out.empty())
            lo = hi = {};
        out.boundsMin = lo;
        out.boundsMax = hi;
    }

    const Terrain& terrain_;
    const MaterialDeforms& deforms_;
    std::vector<SmoothCell> cells_;
    SmoothMesher mesher_;
    SmoothMesh solid_, water_;
    std::vector<Vector3> normals_;
    std::vector<std::uint32_t> remap_;
    // Coarse levels only: per box cell, the summed occupancy of solid and of
    // water under it, and of each solid material in its top layer.
    struct Sums {
        float solid = 0.0f;
        float water = 0.0f;
        int top = std::numeric_limits<int>::min(); // highest solid cell's Y
    };
    std::vector<Sums> sums_;
    std::vector<float> coverage_;
    int scale_ = 1; // cells per box cell
    std::array<Color3, Terrain::materials.size()> colors_{};
    Color3 waterColor_;
    Int3 base_{}; // first cell, in box cells
};

// Meshes the blocks over the worker threads, keeping their order.
std::vector<TerrainMeshChunk> meshBlocks(const Terrain& terrain, const std::vector<Int3>& positions,
                                         const MeshOptions& options)
{
    std::vector<TerrainMeshChunk> results(positions.size());
    std::atomic<std::size_t> next = 0;
    auto work = [&] {
        BlockMesher mesher(terrain);
        for (std::size_t i = next++; i < positions.size(); i = next++)
            results[i] = mesher.mesh(positions[i]);
    };
    unsigned threads = options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = static_cast<unsigned>(std::min<std::size_t>(threads, positions.size()));
    if (threads <= 1) {
        work();
        return results;
    }
    {
        std::vector<std::jthread> pool;
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            pool.emplace_back(work);
    }
    return results;
}

} // namespace

std::vector<TerrainMeshChunk> meshTerrain(const Terrain& terrain, const MeshOptions& options)
{
    // A block also owns the edges from its last cells into the next block, so
    // the air blocks just below terrain ones can hold surface too.
    std::vector<Int3> positions;
    positions.reserve(terrain.chunks().size() * 4);
    for (const TerrainChunk& chunk : terrain.chunks()) {
        const Int3 p = chunk.position;
        positions.push_back(p);
        for (int axis = 0; axis < 3; ++axis) {
            Int3 below = p;
            below[static_cast<std::size_t>(axis)] -= 1;
            if (!terrain.findChunk(below[0], below[1], below[2]))
                positions.push_back(below);
        }
    }
    std::ranges::sort(positions);
    const auto [first, last] = std::ranges::unique(positions);
    positions.erase(first, last);

    std::vector<TerrainMeshChunk> chunks = meshBlocks(terrain, positions, options);
    std::erase_if(chunks, [](const TerrainMeshChunk& chunk) { return chunk.empty(); });
    return chunks;
}

TerrainMeshChunk meshChunk(const Terrain& terrain, std::array<int, 3> position)
{
    return BlockMesher(terrain).mesh(position);
}

std::vector<TerrainMeshChunk> remeshRegion(const Terrain& terrain, std::array<int, 3> cellMin,
                                           std::array<int, 3> cellMax, const MeshOptions& options)
{
    // A block reads cells -2 to 33 of its own, so an edited cell matters to
    // the blocks whose box covers it.
    Int3 lo{}, hi{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        lo[axis] = -floorDiv(boxSide + boxMin - 1 - cellMin[axis], blockSize);
        hi[axis] = floorDiv(cellMax[axis] - boxMin, blockSize);
    }
    std::vector<Int3> positions;
    for (int y = lo[1]; y <= hi[1]; ++y)
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int x = lo[0]; x <= hi[0]; ++x)
                positions.push_back({x, y, z});
    return meshBlocks(terrain, positions, options);
}

class NodeMesher::Impl : public BlockMesher {
public:
    using BlockMesher::BlockMesher;
};

NodeMesher::NodeMesher(const Terrain& terrain) : impl_(std::make_unique<Impl>(terrain)) {}

NodeMesher::~NodeMesher() = default;

TerrainMeshChunk NodeMesher::mesh(int level, std::array<int, 3> position, bool skirts)
{
    return impl_->mesh(position, level, skirts);
}

MeshStats measure(const std::vector<TerrainMeshChunk>& chunks)
{
    MeshStats stats;
    stats.chunks = chunks.size();
    for (const TerrainMeshChunk& chunk : chunks)
        for (const MeshData* mesh : {&chunk.solid, &chunk.water}) {
            (mesh == &chunk.solid ? stats.solidTriangles : stats.waterTriangles) += mesh->indices.size() / 3;
            stats.vertices += mesh->vertices.size();
            stats.bytes += mesh->vertices.size() * sizeof(MeshVertex) + mesh->indices.size() * sizeof(std::uint32_t);
        }
    return stats;
}

} // namespace lunar::terrain
