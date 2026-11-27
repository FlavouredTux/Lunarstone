#include <lunar/terrain/SmoothMesher.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

// Built from the client's sub_105159fb6 and what it calls: 0x105156da7
// classifies the cells, 0x10515751d makes the vertices and quads,
// 0x10516865a splits a quad, 0x105155693 builds the cube tables at start-up.
// Float operations keep the client's order so positions come out bit for bit
// (the tests compare against values from running that code).

namespace lunar::terrain {

namespace {

// The flags these read, at their production values (clientsettings, October
// 2026): DebugTerrainDontFlipQuads, AllowDontDeformMaterials and
// DebugRenderingSetDeterministic off. FFlagEnableMaterialSlotTable is on,
// which picks the newer deformation code (five kinds, shift spanning
// [-amount, amount]); FFlagTerrainFixWaterLevel(Max) only feed the result
// flags. TerrainCoarseLodEdgeClamp is off.
constexpr bool dontFlipQuads = false;

// The twelve cube edges as (x, y, z) offsets of their two corners
// (0x1067d05b0), in the order the cube tables number their bits.
struct Edge {
    std::uint8_t a[3];
    std::uint8_t b[3];
};
constexpr std::array<Edge, 12> edges{{
    {{0, 0, 0}, {1, 0, 0}},
    {{1, 0, 0}, {1, 1, 0}},
    {{0, 1, 0}, {1, 1, 0}},
    {{0, 0, 0}, {0, 1, 0}},
    {{0, 0, 1}, {1, 0, 1}},
    {{1, 0, 1}, {1, 1, 1}},
    {{0, 1, 1}, {1, 1, 1}},
    {{0, 0, 1}, {0, 1, 1}},
    {{0, 0, 0}, {0, 0, 1}},
    {{1, 0, 0}, {1, 0, 1}},
    {{1, 1, 0}, {1, 1, 1}},
    {{0, 1, 0}, {0, 1, 1}},
}};

// A cube's index is its corners' states in base 3, corner (x, y, z) being
// digit 4x + 2y + z. The tables give the crossed edges: the solid pass
// crosses wherever the states differ, the water pass only between water and
// air (0x105155693).
struct CubeTables {
    std::array<std::uint16_t, 6561> solid{};
    std::array<std::uint16_t, 6561> water{};
};

const CubeTables& cubeTables()
{
    static const CubeTables tables = [] {
        CubeTables t;
        for (int index = 0; index < 6561; ++index) {
            std::array<int, 8> state{};
            int rest = index;
            for (int digit = 0; digit < 8; ++digit) {
                state[static_cast<std::size_t>(digit)] = rest % 3;
                rest /= 3;
            }
            auto corner
                = [&](const std::uint8_t* c) { return state[static_cast<std::size_t>(4 * c[0] + 2 * c[1] + c[2])]; };
            std::uint16_t solid = 0, water = 0;
            for (std::size_t e = 0; e < edges.size(); ++e) {
                const int sa = corner(edges[e].a), sb = corner(edges[e].b);
                const auto bit = static_cast<std::uint16_t>(1u << e);
                if (sa != sb)
                    solid |= bit;
                if ((sa == 1 && sb == 0) || (sa == 0 && sb == 1))
                    water |= bit;
            }
            t.solid[static_cast<std::size_t>(index)] = solid;
            t.water[static_cast<std::size_t>(index)] = water;
        }
        return t;
    }();
    return tables;
}

// The quad split (0x1067d08e0): six corners of the quad per (diagonal, flip).
constexpr std::array<std::array<std::array<std::uint8_t, 6>, 2>, 2> quadSplit{{
    {{{1, 0, 2, 2, 0, 3}, {1, 2, 0, 0, 2, 3}}},
    {{{0, 3, 1, 1, 3, 2}, {0, 1, 3, 3, 1, 2}}},
}};

// The cube hash materials jitter by (0x105158b95): boost-style
// hash_combine of the cube's world coordinates, then Thomas Wang's 32-bit
// integer hash.
std::uint32_t cubeHash(int x, int y, int z)
{
    constexpr std::uint64_t golden = 0x9e3779b9u;
    const std::uint64_t hx = static_cast<std::uint64_t>(static_cast<std::int64_t>(x)) + golden;
    std::uint64_t h = (hx << 6) + (hx >> 2) + (static_cast<std::uint64_t>(static_cast<std::int64_t>(y)) + golden);
    h ^= hx;
    const std::uint32_t hz = static_cast<std::uint32_t>(z) + static_cast<std::uint32_t>(golden);
    std::uint32_t a = static_cast<std::uint32_t>(h >> 2) + ((static_cast<std::uint32_t>(h) << 6) + hz);
    a ^= static_cast<std::uint32_t>(h);
    std::uint32_t c = ~(a << 15) + a;
    a = (c >> 10) ^ c;
    a *= 9;
    c = (a >> 6) ^ a;
    a = ~(c << 11) + c;
    return (a >> 16) ^ a;
}

// Rounds half away from zero the way the client does: add +-0.5 in double
// and truncate.
float roundAway(float v)
{
    const double d = static_cast<double>(v) + (v < 0.0f ? -0.5 : 0.5);
    return static_cast<float>(static_cast<std::int32_t>(d));
}

MaterialDeforms makeDeforms(std::initializer_list<std::pair<int, MaterialDeform>> entries)
{
    MaterialDeforms d{};
    for (auto [index, deform] : entries)
        d[static_cast<std::size_t>(index)] = deform;
    return d;
}

// The material of a vertex: the one covering most of its cube's solid
// corners, by occupancy, merged pairwise in the client's order (corners
// gathered as table 0x1067d08c0 lists them; ties go to the lower index).
struct Vote {
    int weight = 0;
    std::uint8_t material = 0;
};

Vote merge(Vote l, Vote r)
{
    if (l.material == r.material)
        return {l.weight + r.weight, l.material};
    if (l.weight == r.weight)
        return {l.weight, std::min(l.material, r.material)};
    return l.weight > r.weight ? l : r;
}

constexpr std::array<std::array<std::uint8_t, 3>, 8> gatherOrder{{
    {0, 0, 0},
    {1, 0, 0},
    {1, 1, 0},
    {0, 1, 0},
    {0, 0, 1},
    {1, 0, 1},
    {1, 1, 1},
    {0, 1, 1},
}};

// Moves a vertex (in cube-local cells, the cube `half` wide on each side of
// its centre) the way its material asks (0x1051592c5: the slot table code,
// FFlagEnableMaterialSlotTable).
void deform(const MaterialDeform& d, std::uint32_t hash, float half, float& x, float& y, float& z)
{
    switch (d.kind) {
    case Deform::Shift: {
        auto unit = [](std::uint32_t byte) {
            const float v = static_cast<float>(byte) / 255.0f;
            return (v + v) + -1.0f;
        };
        x += unit(hash & 0xff) * d.amount;
        y += unit((hash >> 8) & 0xff) * d.amount;
        z += unit((hash >> 16) & 0xff) * d.amount;
        break;
    }
    case Deform::Cubify:
        x += (half - x) * d.amount;
        y += (half - y) * d.amount;
        z += (half - z) * d.amount;
        break;
    case Deform::Quantize: {
        const float inverseStep = 1.0f / d.amount;
        x = roundAway((x - half) * inverseStep) * d.amount + half;
        y = roundAway((y - half) * inverseStep) * d.amount + half;
        z = roundAway((z - half) * inverseStep) * d.amount + half;
        break;
    }
    case Deform::Barrel:
        y += (half - y) * d.amount;
        break;
    case Deform::Water:
        y -= d.amount;
        break;
    case Deform::None:
        break;
    }
}

} // namespace

const MaterialDeforms& materialDeforms(bool use2022Materials)
{
    // PlatformContent/pc/terrain/materials2022.json and materials.json.
    static const MaterialDeforms v2022 = makeDeforms({
        {1, {Deform::Water, 0.5f}},
        {3, {Deform::Shift, 0.0f}},
        {4, {Deform::Quantize, 1.0f}},
        {5, {Deform::Cubify, 1.0f}},
        {7, {Deform::Cubify, 1.0f}},
        {8, {Deform::Shift, 0.299f}},
        {18, {Deform::Shift, 0.149f}},
        {22, {Deform::Quantize, 0.25f}},
    });
    static const MaterialDeforms pre2022 = makeDeforms({
        {1, {Deform::Water, 0.5f}},
        {3, {Deform::Shift, 0.1f}},
        {4, {Deform::Quantize, 0.5f}},
        {5, {Deform::Cubify, 1.0f}},
        {7, {Deform::Cubify, 1.0f}},
        {8, {Deform::Shift, 0.2998f}},
        {13, {Deform::Shift, 0.2f}},
        {15, {Deform::Shift, 0.1f}},
        {17, {Deform::Quantize, 0.1499f}},
        {18, {Deform::Shift, 0.1499f}},
        {22, {Deform::Quantize, 0.25f}},
    });
    return use2022Materials ? v2022 : pre2022;
}

SmoothContents SmoothMesher::mesh(const SmoothBox& box, SmoothPass pass, const MaterialDeforms& deforms,
                                  SmoothMesh& out)
{
    SmoothContents contents;
    if (box.size[0] < 4 || box.size[1] < 4 || box.size[2] < 4)
        return contents;
    const bool water = pass == SmoothPass::Water;
    contents = classify(box, water);
    if (water)
        settleWater(box.size);
    else if (!contents.solid)
        return contents;
    const auto first = static_cast<std::uint32_t>(out.vertices.size());
    makeVertices(box, water, deforms, out);
    makeQuads(box.size, water, first, out);
    return contents;
}

std::size_t SmoothMesher::index(const std::array<int, 3>& size, int x, int y, int z)
{
    // Always non-negative: the arguments stay within the box.
    auto u = [](int v) { return static_cast<std::size_t>(v); };
    return u(x) + u(size[0]) * (u(z) + u(size[2]) * u(y));
}

// 0x105156da7 for voxel input: solid 2, water 1, air 0. The solid pass reads
// water cells as air; the water pass reads a solid cell holding water as
// water filled to its water level, flagged as partly solid.
SmoothContents SmoothMesher::classify(const SmoothBox& box, bool water)
{
    SmoothContents contents;
    const std::size_t count = box.cells.size();
    cells_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SmoothCell& c = box.cells[i];
        Cell& m = cells_[i];
        m.occupancy = c.occupancy;
        m.flags = 0;
        m.state = 0;
        if (c.material >= 2) {
            contents.solid = true;
            // A solid cell holding water asks for the water pass too: the
            // grid's water channel flag (Grid_NEW sets it, +0x15) with
            // FFlagTerrainFixWaterLevel on.
            contents.water = contents.water || c.water != 0;
            if (!water)
                m.state = 2;
            else if (c.water != 0) {
                m.state = 1;
                m.occupancy = c.water;
                m.flags = c.occupancy != 0 ? solidFlag : 0;
            } else {
                m.state = 2;
                m.flags = solidFlag;
            }
        } else {
            contents.water = contents.water || c.material == 1;
            m.state = water && c.material == 1 ? 1 : 0;
        }
    }
    return contents;
}

// The rest of 0x105156da7's water pass: solid cells beside water fill up to
// the highest water level beside them (full under water), then solid cells
// that aren't wholly buried in ground turn to air, so the water closes
// against the ground inside it rather than stopping at its surface.
void SmoothMesher::settleWater(const std::array<int, 3>& size)
{
    const int sx = size[0], sy = size[1], sz = size[2];
    scratch_ = cells_;
    for (int y = 1; y < sy - 1; ++y)
        for (int z = 1; z < sz - 1; ++z)
            for (int x = 1; x < sx - 1; ++x) {
                const Cell& c = cells_[index(size, x, y, z)];
                if (c.state == 0 || (c.state == 1 && !(c.flags & solidFlag)))
                    continue;
                Cell& target = scratch_[index(size, x, y, z)];
                if (cells_[index(size, x, y + 1, z)].state == 1) {
                    target.state = 1;
                    target.occupancy = 0xff;
                    continue;
                }
                bool beside = false;
                std::uint8_t level = 0;
                for (auto [dx, dz] : {std::pair{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}}) {
                    const Cell& n = cells_[index(size, x + dx, y, z + dz)];
                    beside = beside || (n.state & 1) != 0;
                    if (n.state == 1)
                        level = std::max(level, n.occupancy);
                }
                if (beside) {
                    target.state = 1;
                    target.occupancy = level;
                }
            }
    cells_ = scratch_;
    for (int y = 1; y < sy - 1; ++y)
        for (int z = 1; z < sz - 1; ++z)
            for (int x = 1; x < sx - 1; ++x) {
                Cell& c = cells_[index(size, x, y, z)];
                if (c.state != 2)
                    continue;
                auto at = [&](int dx, int dy, int dz) { return scratch_[index(size, x + dx, y + dy, z + dz)].state; };
                const bool buried = c.occupancy == 0xff && at(-1, 0, 0) != 0 && at(1, 0, 0) != 0 && at(0, -1, 0) == 2
                                    && at(0, 1, 0) != 0 && at(0, 0, -1) != 0 && at(0, 0, 1) != 0;
                if (!buried) {
                    c.state = 0;
                    c.occupancy = 0;
                }
            }
}

// One vertex per cube with a crossed edge (0x10515751d): the mean of the
// crossings, an edge crossed occupancy of the way from its fuller end.
void SmoothMesher::makeVertices(const SmoothBox& box, bool water, const MaterialDeforms& deforms, SmoothMesh& out)
{
    const std::array<int, 3>& size = box.size;
    const auto& table = water ? cubeTables().water : cubeTables().solid;
    const int lod = box.lod;
    const auto scale = static_cast<float>(1 << lod);
    const float half = scale * 0.5f;
    // Coarse cubes move along their crossings' directions by the extra half
    // cell, so a coarse surface lands where the fine one would.
    const float shift = (0.5f - 0.5f / scale) * scale;
    const auto first = static_cast<std::uint32_t>(out.vertices.size());
    cubeVertex_.assign(cells_.size(), 0);
    for (int y = margin; y < size[1] - margin - 1; ++y)
        for (int z = margin; z < size[2] - margin - 1; ++z)
            for (int x = margin; x < size[0] - margin - 1; ++x) {
                int cubeIndex = 0;
                for (int corner = 7; corner >= 0; --corner)
                    cubeIndex
                        = cubeIndex * 3
                          + cells_[index(size, x + (corner >> 2), y + ((corner >> 1) & 1), z + (corner & 1))].state;
                const std::uint16_t mask = table[static_cast<std::size_t>(cubeIndex)];
                if (mask == 0)
                    continue;
                cubeVertex_[index(size, x, y, z)] = static_cast<std::uint32_t>(out.vertices.size()) - first + 1;

                float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
                std::array<float, 3> direction{};
                int crossings = 0;
                for (std::size_t e = 0; e < edges.size(); ++e) {
                    if (!(mask & (1u << e)))
                        continue;
                    const Edge& edge = edges[e];
                    const Cell& a = cells_[index(size, x + edge.a[0], y + edge.a[1], z + edge.a[2])];
                    const Cell& b = cells_[index(size, x + edge.b[0], y + edge.b[1], z + edge.b[2])];
                    const float t = a.state > b.state ? static_cast<float>(a.occupancy + 1) * 0.00390625f
                                                      : static_cast<float>(b.occupancy + 1) * -0.00390625f + 1.0f;
                    const std::size_t axis = edge.a[2] != edge.b[2] ? 2 : edge.a[1] != edge.b[1] ? 1 : 0;
                    direction[axis] += a.state > b.state ? -1.0f : 1.0f;
                    auto along = [&](std::size_t k) {
                        const float from = static_cast<float>(edge.a[k]) * scale;
                        return (static_cast<float>(edge.b[k]) * scale - from) * t + from;
                    };
                    sumX += along(0);
                    sumY += along(1);
                    sumZ += along(2);
                    ++crossings;
                }
                const float inverse = 1.0f / static_cast<float>(crossings);
                float lx = direction[0] * inverse * shift + sumX * inverse;
                float ly = direction[1] * inverse * shift + sumY * inverse;
                float lz = direction[2] * inverse * shift + sumZ * inverse;

                std::array<Vote, 8> votes{};
                int voters = 0;
                for (auto [dx, dy, dz] : gatherOrder) {
                    const SmoothCell& c = box.cells[index(size, x + dx, y + dy, z + dz)];
                    if (c.material >= 2)
                        votes[static_cast<std::size_t>(voters++)] = {c.occupancy + 1, c.material};
                }
                std::uint8_t material = 1;
                if (voters != 0 && !water) {
                    Vote best = merge(merge(votes[0], votes[1]), merge(votes[2], votes[3]));
                    if (voters >= 5)
                        best = merge(best, merge(merge(votes[4], votes[5]), merge(votes[6], votes[7])));
                    material = best.material;
                }

                if (!water) {
                    const std::uint32_t hash
                        = cubeHash((box.origin[0] >> lod) + x, (box.origin[1] >> lod) + y, (box.origin[2] >> lod) + z);
                    deform(deforms[material < deforms.size() ? material : 0], hash, half, lx, ly, lz);
                }

                // Fine cubes keep their vertex inside the cube.
                auto place = [&](std::size_t k, int cell, float local) {
                    const float base = static_cast<float>(box.origin[k]) + static_cast<float>(cell) * scale;
                    return lod > 0 ? base + local + half : base + std::min(scale, std::max(0.0f, local)) + 0.5f;
                };
                Vector3 position{place(0, x, lx), place(1, y, ly), place(2, z, lz)};
                if (water)
                    position.y += -0.5f; // water sits half a cell lower
                const bool border = x == margin || y == margin || z == margin || x == size[0] - margin - 2
                                    || y == size[1] - margin - 2 || z == size[2] - margin - 2;
                out.vertices.push_back({position, material, border});
            }
}

// One quad per crossed edge starting in the meshed range, joining the four
// cubes around it and split along the diagonal whose ends share a material.
void SmoothMesher::makeQuads(const std::array<int, 3>& size, bool water, std::uint32_t first, SmoothMesh& out)
{
    auto vertexOf = [&](int x, int y, int z) { return first + cubeVertex_[index(size, x, y, z)] - 1; };
    auto material = [&](std::uint32_t i) { return out.vertices[i].material; };
    for (int y = margin + 1; y < size[1] - margin - 1; ++y)
        for (int z = margin + 1; z < size[2] - margin - 1; ++z)
            for (int x = margin + 1; x < size[0] - margin - 1; ++x) {
                const Cell& here = cells_[index(size, x, y, z)];
                for (int axis = 0; axis < 3; ++axis) {
                    const Cell& next = cells_[index(size, x + (axis == 0), y + (axis == 1), z + (axis == 2))];
                    const bool crossed
                        = water ? (here.state == 1 && next.state == 0) || (here.state == 0 && next.state == 1)
                                : here.state != next.state;
                    if (!crossed)
                        continue;
                    std::array<std::uint32_t, 4> v{};
                    if (axis == 0)
                        v = {vertexOf(x, y, z), vertexOf(x, y - 1, z), vertexOf(x, y - 1, z - 1),
                             vertexOf(x, y, z - 1)};
                    else if (axis == 1)
                        v = {vertexOf(x, y, z), vertexOf(x, y, z - 1), vertexOf(x - 1, y, z - 1),
                             vertexOf(x - 1, y, z)};
                    else
                        v = {vertexOf(x, y, z), vertexOf(x - 1, y, z), vertexOf(x - 1, y - 1, z),
                             vertexOf(x, y - 1, z)};
                    const bool shared = material(v[1]) == material(v[3])
                                        && (material(v[2]) == material(v[1]) || material(v[0]) == material(v[1]));
                    const auto& split = quadSplit[!dontFlipQuads && shared ? 1 : 0][here.state > next.state ? 1 : 0];
                    for (std::uint8_t corner : split)
                        out.indices.push_back(v[corner]);
                }
            }
}

} // namespace lunar::terrain
