#include <lunar/render/LightGrid.hpp>

#include <lunar/render/Primitives.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

namespace lunar::render {

namespace {

constexpr int layerVoxels = LightGrid::chunkX * LightGrid::chunkZ;
// The light map's alpha is the light's strength over this, its colour the
// rest (the client's shaders decode rgb * a * 120). [code: sub_10486214e]
constexpr float lightMapScale = 120.0f;

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int wrap(int a, int n)
{
    const int m = a % n;
    return m < 0 ? m + n : m;
}

// The most whole voxels from the world's middle a position counts as (a
// billion studs): far enough that no part, light or camera worth lighting
// is past it, near enough that chunk coordinates times a chunk's voxels,
// and a grid's worth more, stay well inside an int.
constexpr int voxelLimit = 1 << 28;
// The grid follows the camera only this far out, so the lights reaching it
// never come near the limit either.
constexpr float followLimit = static_cast<float>(voxelLimit / 2) * LightGrid::voxelStuds;

// v rounded down, saturated at the limit; NaN is the low end (a float past
// an int's range doesn't convert).
int floorToInt(float v)
{
    if (!(v > -static_cast<float>(voxelLimit)))
        return -voxelLimit;
    if (v >= static_cast<float>(voxelLimit))
        return voxelLimit;
    return static_cast<int>(std::floor(v));
}

// Threads for the grid's work: half the cores, like the terrain's workers.
unsigned workerThreads()
{
    return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
}

// fn(i) for every i below count, spread over the worker threads.
void parallelFor(std::size_t count, std::function_ref<void(std::size_t)> fn)
{
    const auto threads = static_cast<unsigned>(std::min<std::size_t>(workerThreads(), count));
    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    auto run = [&] {
        for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1))
            fn(i);
    };
    std::vector<std::jthread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t)
        pool.emplace_back(run);
    run();
}

Vector3 column(const Mat4& m, std::size_t c)
{
    return {m.m[c][0], m.m[c][1], m.m[c][2]};
}

// The client's skylight curve (sub_10485f3e8's table): what the shaders
// see for the light coming down into a voxel.
std::uint8_t skyCurve(int above)
{
    const auto d = static_cast<std::uint32_t>(255 - above);
    return static_cast<std::uint8_t>(252 - (d * d * d * d >> 24));
}

// A box-shaped piece in world space: centre, unit axes and half sizes.
struct Box {
    Vector3 center;
    std::array<Vector3, 3> axis;
    std::array<float, 3> half{};
};

Box boxFrom(const Mat4& model, const Bounds& local)
{
    Box box;
    box.center = model.transformPoint(local.center());
    const Vector3 extent = local.extent() * 0.5f;
    const float halves[3] = {extent.x, extent.y, extent.z};
    for (std::size_t i = 0; i < 3; ++i) {
        const Vector3 c = column(model, i);
        const float length = c.magnitude();
        box.axis[i]
            = length > 1e-12f ? c / length : Vector3{i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
        box.half[i] = length * halves[i];
    }
    return box;
}

// Whether a point in the shape's unit box ([-0.5, 0.5] on each axis) is
// inside it; Primitives.hpp has the shapes' conventions.
using ShapeTest = bool (*)(Vector3);
bool insideWedge(Vector3 p)
{
    return p.y <= p.z;
}
bool insideCornerWedge(Vector3 p)
{
    return p.y + 0.5f <= std::min(p.x + 0.5f, 0.5f - p.z);
}
bool insideCylinder(Vector3 p)
{
    return p.y * p.y + p.z * p.z <= 0.25f;
}
bool insideCylinderY(Vector3 p)
{
    return p.x * p.x + p.z * p.z <= 0.25f;
}
bool insideBall(Vector3 p)
{
    return p.dot(p) <= 0.25f;
}

// Adds a box's projected areas to the voxels of one chunk the way the
// client's voxelizer does (sub_1051b6470 and sub_1051be55d): along each of
// the box's axes, the overlap with the voxel in voxel units, raw = half
// size + half the voxel - distance, gives o = clamp(raw, 0, size) (how much
// of the voxel it covers) and c = clamp(raw / size, 0, 1) (how much of the
// box's thickness is in this voxel), sizes in voxels clamped to [0.0125, 1].
// The areas seen along its axes are o1 o2 c0, o0 o2 c1 and o0 o1 c2, turned
// onto the world's axes by the absolute rotation, times the opacity and
// 255.5, added and clamped at 255. GUESS: for boxes turned off the world's
// axes the client takes another path (sub_1051b55c4); here the voxel's
// extent along each box axis widens the overlap instead. Other shapes keep
// the share of 27 points in the voxel that their shape holds of those the
// box holds.
//
// Lunarstone's own addition: heights, when given, gathers how high in each
// voxel what blocks light from above sits (Chunk::blockerHeight).
struct BlockerHeights {
    std::vector<float> weight; // y-plane occupancy added, per voxel
    std::vector<float> sum;    // ...times the height of its middle in the voxel, 0 at the bottom, 1 at the top
    void add(std::size_t index, float area, float height)
    {
        weight[index] += area;
        sum[index] += area * height;
    }
};

void addBox(std::uint8_t* planes, GridPoint chunkVoxel, const Box& box, float opacity255, ShapeTest shape,
            BlockerHeights* heights)
{
    Vector3 reach;
    float extent[3]{};     // the voxel's extent along each box axis, in voxels
    float halfVoxels[3]{}; // the box's half size in voxels
    float size[3]{};       // its size in voxels, clamped to [0.0125, 1]
    for (std::size_t i = 0; i < 3; ++i) {
        const Vector3 a = box.axis[i];
        extent[i] = std::abs(a.x) + std::abs(a.y) + std::abs(a.z);
        const float studs = std::max(box.half[i] * 2.0f, 0.05f);
        halfVoxels[i] = studs * 0.125f;
        size[i] = std::min(studs * 0.25f, 1.0f);
        reach = reach + Vector3{std::abs(a.x), std::abs(a.y), std::abs(a.z)} * box.half[i];
    }
    const GridPoint low = LightGrid::voxelAt(box.center - reach);
    const GridPoint high = LightGrid::voxelAt(box.center + reach);
    const int x0 = std::max(low.x, chunkVoxel.x), x1 = std::min(high.x, chunkVoxel.x + LightGrid::chunkX - 1);
    const int y0 = std::max(low.y, chunkVoxel.y), y1 = std::min(high.y, chunkVoxel.y + LightGrid::chunkY - 1);
    const int z0 = std::max(low.z, chunkVoxel.z), z1 = std::min(high.z, chunkVoxel.z + LightGrid::chunkZ - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const Vector3 center{(static_cast<float>(x) + 0.5f) * LightGrid::voxelStuds,
                                     (static_cast<float>(y) + 0.5f) * LightGrid::voxelStuds,
                                     (static_cast<float>(z) + 0.5f) * LightGrid::voxelStuds};
                const Vector3 d = center - box.center;
                float o[3]{}, c[3]{};
                bool empty = false;
                for (std::size_t i = 0; i < 3; ++i) {
                    const float raw = halfVoxels[i] + extent[i] * 0.5f - std::abs(d.dot(box.axis[i])) * 0.25f;
                    if (raw <= 0.0f) {
                        empty = true;
                        break;
                    }
                    const float limit = std::min(size[i], extent[i]);
                    o[i] = std::min(raw, limit) / extent[i];
                    c[i] = std::min(raw / limit, 1.0f);
                }
                if (empty)
                    continue;
                float share = 1.0f;
                if (shape) {
                    int inBox = 0, inShape = 0;
                    for (int k = 0; k < 27; ++k) {
                        const int ox = (k % 3) - 1, oy = ((k / 3) % 3) - 1, oz = (k / 9) - 1;
                        const Vector3 offset{static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
                        const Vector3 q = d + offset * (LightGrid::voxelStuds / 3.0f);
                        Vector3 u;
                        float* uc[3] = {&u.x, &u.y, &u.z};
                        bool inside = true;
                        for (std::size_t i = 0; i < 3; ++i) {
                            *uc[i] = q.dot(box.axis[i]) / std::max(box.half[i] * 2.0f, 1e-6f);
                            inside = inside && std::abs(*uc[i]) <= 0.5f;
                        }
                        inBox += inside ? 1 : 0;
                        inShape += inside && shape(u) ? 1 : 0;
                    }
                    if (inBox > 0)
                        share = static_cast<float>(inShape) / static_cast<float>(inBox);
                }
                const float local[3] = {o[1] * o[2] * c[0], o[0] * o[2] * c[1], o[0] * o[1] * c[2]};
                const std::size_t index = static_cast<std::size_t>(y - chunkVoxel.y) * layerVoxels
                                          + static_cast<std::size_t>(z - chunkVoxel.z) * LightGrid::chunkX
                                          + static_cast<std::size_t>(x - chunkVoxel.x);
                for (std::size_t w = 0; w < 3; ++w) {
                    float area = 0.0f;
                    for (std::size_t i = 0; i < 3; ++i) {
                        const Vector3 a = box.axis[i];
                        area += std::abs(w == 0 ? a.x : w == 1 ? a.y : a.z) * local[i];
                    }
                    const int add = static_cast<int>(area * share * opacity255);
                    std::uint8_t& v = planes[w * LightGrid::chunkVoxels + index];
                    v = static_cast<std::uint8_t>(std::min(v + add, 255));
                    if (w == 1 && heights && add > 0) {
                        const float bottom = center.y - LightGrid::voxelStuds * 0.5f;
                        const float lo = std::max(box.center.y - reach.y, bottom);
                        const float hi = std::min(box.center.y + reach.y, bottom + LightGrid::voxelStuds);
                        heights->add(index, static_cast<float>(add),
                                     std::clamp((lo + hi) * 0.5f - bottom, 0.0f, LightGrid::voxelStuds)
                                         / LightGrid::voxelStuds);
                    }
                }
            }
        }
    }
}

// Terrain, from its surface: every bit of the surface blocks a voxel by
// its area seen along each axis. GUESS: the client voxelizes terrain from
// its voxels' occupancy (4-stud cells, like the grid's), which only the
// terrain library has; light reaching the surface from above is stopped
// the same way.
void addSurface(std::span<float> scratch, GridPoint chunkVoxel, const Mat4& model, const MeshData& mesh,
                BlockerHeights& heights)
{
    const Vector3 low{static_cast<float>(chunkVoxel.x) * LightGrid::voxelStuds,
                      static_cast<float>(chunkVoxel.y) * LightGrid::voxelStuds,
                      static_cast<float>(chunkVoxel.z) * LightGrid::voxelStuds};
    const Vector3 high = low
                         + Vector3{LightGrid::chunkX * LightGrid::voxelStuds, LightGrid::chunkY * LightGrid::voxelStuds,
                                   LightGrid::chunkZ * LightGrid::voxelStuds};
    constexpr float faceArea = LightGrid::voxelStuds * LightGrid::voxelStuds;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t ia = mesh.indices[t], ib = mesh.indices[t + 1], ic = mesh.indices[t + 2];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size())
            continue;
        const Vector3 a = model.transformPoint(mesh.vertices[ia].position);
        const Vector3 b = model.transformPoint(mesh.vertices[ib].position);
        const Vector3 c = model.transformPoint(mesh.vertices[ic].position);
        if (std::max({a.x, b.x, c.x}) < low.x || std::min({a.x, b.x, c.x}) >= high.x
            || std::max({a.y, b.y, c.y}) < low.y || std::min({a.y, b.y, c.y}) >= high.y
            || std::max({a.z, b.z, c.z}) < low.z || std::min({a.z, b.z, c.z}) >= high.z)
            continue;
        const Vector3 n = (b - a).cross(c - a);
        const float doubleArea = n.magnitude();
        if (doubleArea <= 1e-8f)
            continue;
        const float longest = std::max({(b - a).magnitude(), (c - b).magnitude(), (a - c).magnitude()});
        const int k = std::clamp(static_cast<int>(std::ceil(longest / 2.0f)), 1, 64);
        // k * k equal sub-triangles; one point each (their centroids).
        const float share = doubleArea * 0.5f / static_cast<float>(k * k) / faceArea;
        const float area[3] = {std::abs(n.x) / doubleArea * share, std::abs(n.y) / doubleArea * share,
                               std::abs(n.z) / doubleArea * share};
        const Vector3 u = (b - a) / static_cast<float>(k), v = (c - a) / static_cast<float>(k);
        auto add = [&](Vector3 p) {
            const GridPoint g = LightGrid::voxelAt(p);
            const int x = g.x - chunkVoxel.x, y = g.y - chunkVoxel.y, z = g.z - chunkVoxel.z;
            if (x < 0 || y < 0 || z < 0 || x >= LightGrid::chunkX || y >= LightGrid::chunkY || z >= LightGrid::chunkZ)
                return;
            const std::size_t index = static_cast<std::size_t>(y) * layerVoxels
                                      + static_cast<std::size_t>(z) * LightGrid::chunkX + static_cast<std::size_t>(x);
            for (std::size_t w = 0; w < 3; ++w)
                scratch[w * LightGrid::chunkVoxels + index] += area[w];
            heights.add(index, area[1] * 255.5f,
                        (p.y - static_cast<float>(g.y) * LightGrid::voxelStuds) / LightGrid::voxelStuds);
        };
        for (int i = 0; i < k; ++i) {
            for (int j = 0; i + j < k; ++j) {
                const Vector3 base = a + u * static_cast<float>(i) + v * static_cast<float>(j);
                add(base + (u + v) * (1.0f / 3.0f));
                if (i + j + 1 < k)
                    add(base + (u + v) * (2.0f / 3.0f));
            }
        }
    }
}

// The client's sun weights (sub_10485a440, sub_104866e32): with the sun
// mostly along axis M, the two others' slopes a and b against it give
// a/2 - ab/6 and b/2 - ab/6 of the light through their faces and the rest
// through M's, rounded to 256ths that add up to 256.
std::array<int, 3> sunWeights(Vector3 d)
{
    const float m[3] = {std::abs(d.x), std::abs(d.y), std::abs(d.z)};
    const int major = m[1] >= m[0] && m[1] >= m[2] ? 1 : (m[2] >= m[0] && m[2] >= m[1] ? 2 : 0);
    const int i = major == 0 ? 1 : 0;
    const int j = major == 2 ? 1 : 2;
    const float a = m[i] / m[major], b = m[j] / m[major];
    const float wi = 0.5f * a - a * b / 6.0f, wj = 0.5f * b - a * b / 6.0f, wm = 1.0f - wi - wj;
    auto quantize = [](float w) { return static_cast<int>(std::lround(std::clamp(w, 0.0f, 1.0f) * 256.0f)); };
    const int qi = quantize(wi), qj = quantize(wj), qm = quantize(wm);
    const int outM = std::max((256 - qi - qj - qm) / 3 + qm, 0);
    const int outI = std::max((256 - (qi + qj + outM)) / 2 + qi, 0);
    const int outJ = std::max(256 - outM - outI, 0);
    std::array<int, 3> weights{};
    weights[static_cast<std::size_t>(i)] = outI;
    weights[static_cast<std::size_t>(j)] = outJ;
    weights[static_cast<std::size_t>(major)] = outM;
    return weights;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

} // namespace

LightGrid::LightGrid(GridPoint chunks) : chunks_(chunks)
{
    const std::size_t count = static_cast<std::size_t>(chunks_.x) * static_cast<std::size_t>(chunks_.y)
                              * static_cast<std::size_t>(chunks_.z);
    slots_.resize(count);
    for (Chunk& chunk : slots_) {
        chunk.occupancy.assign(3 * chunkVoxels, 0);
        chunk.blockerHeight.assign(chunkVoxels, 128);
        chunk.texels.assign(4 * chunkVoxels, 255);
        chunk.light.assign(4 * chunkVoxels, 0);
        chunk.occupancyChanged.assign(static_cast<std::size_t>(chunkY) * chunkZ, 0);
    }
    const GridPoint v = voxels();
    skyGrid_.assign(static_cast<std::size_t>(v.x) * static_cast<std::size_t>(v.y) * static_cast<std::size_t>(v.z), 255);
    sunGrid_ = skyGrid_;
    skyBelowGrid_ = skyGrid_;
    skyCarried_ = skyGrid_;
    overheadGrid_ = skyGrid_;
    sunCarried_ = skyGrid_;
}

LightGrid::~LightGrid()
{
    wait();
    // Told under the lock, so the thread can't miss it between its check and its wait.
    worker_.request_stop();
    {
        const std::scoped_lock lock(passMutex_);
        passQueued_.notify_all();
    }
    if (worker_.joinable())
        worker_.join();
}

GridPoint LightGrid::voxelAt(Vector3 point)
{
    return {floorToInt(point.x / voxelStuds), floorToInt(point.y / voxelStuds), floorToInt(point.z / voxelStuds)};
}

std::size_t LightGrid::slotOf(GridPoint chunk) const
{
    const int slot
        = wrap(chunk.x, chunks_.x) + chunks_.x * (wrap(chunk.z, chunks_.z) + chunks_.z * wrap(chunk.y, chunks_.y));
    return static_cast<std::size_t>(slot);
}

GridPoint LightGrid::originFor(Vector3 center) const
{
    // A camera nowhere (NaN: one following a dead character's head flung
    // hundreds of millions of studs away comes out so) or that far out
    // leaves the grid where it is, or at the world's middle before it's
    // anywhere.
    if (!(std::abs(center.x) < followLimit && std::abs(center.y) < followLimit && std::abs(center.z) < followLimit))
        return origin_.x != 0x7fffffff ? origin_ : originFor({});
    // The camera's chunk sits just past the middle (the client starts the
    // grid half its size before it).
    const GridPoint v = voxelAt(center);
    return {floorDiv(v.x, chunkX) - chunks_.x / 2, floorDiv(v.y, chunkY) - chunks_.y / 2,
            floorDiv(v.z, chunkZ) - chunks_.z / 2};
}

std::uint8_t LightGrid::occupancy(int axis, GridPoint voxel) const
{
    const GridPoint low = originVoxel(), size = voxels();
    if (voxel.x < low.x || voxel.y < low.y || voxel.z < low.z || voxel.x >= low.x + size.x || voxel.y >= low.y + size.y
        || voxel.z >= low.z + size.z || axis < 0 || axis > 2)
        return 0;
    const GridPoint chunk{floorDiv(voxel.x, chunkX), floorDiv(voxel.y, chunkY), floorDiv(voxel.z, chunkZ)};
    const std::size_t index = static_cast<std::size_t>(wrap(voxel.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(voxel.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(voxel.x, chunkX));
    return slots_[slotOf(chunk)].occupancy[static_cast<std::size_t>(axis) * chunkVoxels + index];
}

std::uint8_t LightGrid::sky(GridPoint voxel) const
{
    const GridPoint low = originVoxel(), size = voxels();
    if (!ready() || voxel.x < low.x || voxel.y < low.y || voxel.z < low.z || voxel.x >= low.x + size.x
        || voxel.y >= low.y + size.y || voxel.z >= low.z + size.z)
        return 255;
    const GridPoint chunk{floorDiv(voxel.x, chunkX), floorDiv(voxel.y, chunkY), floorDiv(voxel.z, chunkZ)};
    const std::size_t index = static_cast<std::size_t>(wrap(voxel.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(voxel.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(voxel.x, chunkX));
    return slots_[slotOf(chunk)].texels[index * 4];
}

std::uint8_t LightGrid::sun(GridPoint voxel) const
{
    const GridPoint low = originVoxel(), size = voxels();
    if (!ready() || voxel.x < low.x || voxel.y < low.y || voxel.z < low.z || voxel.x >= low.x + size.x
        || voxel.y >= low.y + size.y || voxel.z >= low.z + size.z)
        return 255;
    const GridPoint chunk{floorDiv(voxel.x, chunkX), floorDiv(voxel.y, chunkY), floorDiv(voxel.z, chunkZ)};
    const std::size_t index = static_cast<std::size_t>(wrap(voxel.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(voxel.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(voxel.x, chunkX));
    return slots_[slotOf(chunk)].texels[index * 4 + 1];
}

std::uint8_t LightGrid::skyBelow(GridPoint voxel) const
{
    const GridPoint low = originVoxel(), size = voxels();
    if (!ready() || voxel.x < low.x || voxel.y < low.y || voxel.z < low.z || voxel.x >= low.x + size.x
        || voxel.y >= low.y + size.y || voxel.z >= low.z + size.z)
        return 255;
    const GridPoint chunk{floorDiv(voxel.x, chunkX), floorDiv(voxel.y, chunkY), floorDiv(voxel.z, chunkZ)};
    const std::size_t index = static_cast<std::size_t>(wrap(voxel.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(voxel.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(voxel.x, chunkX));
    return slots_[slotOf(chunk)].texels[index * 4 + 2];
}

Color3 LightGrid::light(GridPoint voxel) const
{
    const GridPoint low = originVoxel(), size = voxels();
    if (!built_ || voxel.x < low.x || voxel.y < low.y || voxel.z < low.z || voxel.x >= low.x + size.x
        || voxel.y >= low.y + size.y || voxel.z >= low.z + size.z)
        return {};
    const GridPoint chunk{floorDiv(voxel.x, chunkX), floorDiv(voxel.y, chunkY), floorDiv(voxel.z, chunkZ)};
    const std::size_t index = static_cast<std::size_t>(wrap(voxel.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(voxel.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(voxel.x, chunkX));
    const std::uint8_t* t = slots_[slotOf(chunk)].light.data() + index * 4;
    const float scale = static_cast<float>(t[3]) / 255.0f * lightMapScale / 255.0f;
    return {t[0] * scale, t[1] * scale, t[2] * scale};
}

void LightGrid::build(std::span<const DrawItem> items, Vector3 center, const Settings& settings,
                      std::span<const LocalLight> lights)
{
    wait();
    settings_ = settings;
    moveTo(originFor(center));
    beginScan(items.size());
    scanItems(items, items.size());
    finishScan(items);
    checkLights(lights);
    skyDirty_ = sunDirty_ = true;
    startWork(true);
    built_ = true;
}

bool LightGrid::update(std::span<const DrawItem> items, Vector3 center, const Settings& settings,
                       std::span<const LocalLight> lights)
{
    if (!built_) {
        build(items, center, settings, lights);
        return true;
    }
    const bool shadowsChanged = settings.shadows != settings_.shadows;
    settings_ = settings;
    if (shadowsChanged)
        skyDirty_ = sunDirty_ = true;
    // Without GlobalShadows or lights the grid stays as it is: full sky and
    // sun, no local light.
    if (!settings_.shadows && lights.empty() && lights_.empty() && !shadowsChanged && !working_)
        return std::ranges::any_of(slots_, &Chunk::changed);
    const Vector3 was = litWith_.sunDirection, now = settings_.sunDirection;
    // GUESS: the client carries the sun again once its direction moved past
    // a threshold (sub_10485b9a6); half a degree here.
    if (was.magnitude() > 0.0f && now.magnitude() > 0.0f && was.unit().dot(now.unit()) < 0.99996f)
        sunDirty_ = true;

    const GridPoint origin = originFor(center);
    if (!working_ && origin != origin_) {
        moveTo(origin);
        scan_ = {};
    }
    // A slice of the parts a frame, the whole set every eight frames, and
    // a new pass no sooner than scanInterval after the last one began.
    const auto checkedAt = std::chrono::steady_clock::now();
    if (!scanPending_ && (scan_.active || checkedAt >= nextScan_)) {
        if (!scan_.active || scan_.itemCount != items.size() || scan_.origin != origin_) {
            beginScan(items.size());
            nextScan_ = checkedAt + scanInterval_;
        }
        const std::size_t slice = std::max<std::size_t>((items.size() + 7) / 8, 4096);
        scanItems(items, std::min(items.size(), scan_.next + slice));
        if (scan_.next >= items.size())
            scanPending_ = true;
    }
    if (scanPending_ && !working_) {
        if (scan_.itemCount == items.size())
            finishScan(items);
        scan_ = {};
        scanPending_ = false;
    }
    if (!working_)
        checkLights(lights);
    if (!working_
        && (skyDirty_ || sunDirty_ || std::ranges::any_of(slots_, &Chunk::voxelize)
            || std::ranges::any_of(slots_, &Chunk::relight)))
        startWork(false);
    return !working_ && std::ranges::any_of(slots_, &Chunk::changed);
}

void LightGrid::beginScan(std::size_t itemCount)
{
    scan_ = {};
    scan_.active = true;
    scan_.itemCount = itemCount;
    scan_.origin = origin_;
    scan_.members.resize(slots_.size());
    scan_.signatures.assign(slots_.size(), 0);
}

// What an item blocks light as, or false for nothing.
bool LightGrid::occluderFor(const DrawItem& item, Occluder& out)
{
    static const std::array<const MeshData*, 6> primitives{
        primitiveMesh(Primitive::Block).get(),       primitiveMesh(Primitive::Wedge).get(),
        primitiveMesh(Primitive::CornerWedge).get(), primitiveMesh(Primitive::Cylinder).get(),
        primitiveMesh(Primitive::CylinderY).get(),   primitiveMesh(Primitive::Ball).get(),
    };
    // GUESS: like shadows, parts with CastShadow off leave the grid alone.
    if (item.decal || item.terrainWater || !item.castShadow || item.transparency >= 1.0f)
        return false;
    out = {.model = item.model, .opacity = 1.0f - std::clamp(item.transparency, 0.0f, 1.0f)};
    if (item.terrain) {
        out.shape = Occluder::Shape::Surface;
        out.mesh = item.mesh;
        return item.mesh != nullptr;
    }
    if (item.mesh) {
        const auto found = std::ranges::find(primitives, item.mesh.get());
        if (found == primitives.end()) {
            out.shape = Occluder::Shape::Mesh;
            out.mesh = item.mesh;
            out.boxes = item.occluders;
        } else {
            out.shape = static_cast<Occluder::Shape>(found - primitives.begin());
        }
    }
    return true;
}

// Which chunks each item reaches, and a signature of the items per chunk
// to tell which changed.
void LightGrid::scanItems(std::span<const DrawItem> items, std::size_t end)
{
    if (!scan_.active)
        beginScan(items.size());
    const GridPoint low{origin_.x * chunkX, origin_.y * chunkY, origin_.z * chunkZ};
    const GridPoint size = voxels();
    const MeshData* lastMesh = nullptr;
    Bounds lastBounds;
    for (; scan_.next < end; ++scan_.next) {
        const DrawItem& item = items[scan_.next];
        if (item.decal || item.terrainWater || !item.castShadow || item.transparency >= 1.0f
            || (item.terrain && !item.mesh))
            continue;
        // The world box around it, from the mesh's bounds.
        Bounds local{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        if (item.mesh) {
            if (item.mesh.get() != lastMesh) {
                auto [it, inserted] = bounds_.try_emplace(item.mesh.get());
                if (inserted || it->second.first != item.mesh) {
                    it->second.first = item.mesh;
                    it->second.second = meshBounds(item.mesh.get());
                }
                lastMesh = item.mesh.get();
                lastBounds = it->second.second;
            }
            local = lastBounds;
        }
        const Vector3 center = item.model.transformPoint(local.center());
        const Vector3 h = local.extent() * 0.5f;
        Vector3 reach;
        for (std::size_t c = 0; c < 3; ++c) {
            const Vector3 axis = column(item.model, c);
            const float half = c == 0 ? h.x : c == 1 ? h.y : h.z;
            reach = reach + Vector3{std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)} * half;
        }
        const GridPoint vlow = voxelAt(center - reach), vhigh = voxelAt(center + reach);
        if (vhigh.x < low.x || vhigh.y < low.y || vhigh.z < low.z || vlow.x >= low.x + size.x
            || vlow.y >= low.y + size.y || vlow.z >= low.z + size.z)
            continue;
        std::uint64_t hash
            = mix(std::bit_cast<std::uint64_t>(item.mesh.get()), std::bit_cast<std::uint64_t>(item.occluders.get()));
        for (const auto& col : item.model.m)
            for (float f : col)
                hash = mix(hash, std::bit_cast<std::uint32_t>(f));
        hash = mix(hash, std::bit_cast<std::uint32_t>(item.transparency));
        const int cx0 = floorDiv(std::max(vlow.x, low.x), chunkX);
        const int cx1 = floorDiv(std::min(vhigh.x, low.x + size.x - 1), chunkX);
        const int cy0 = floorDiv(std::max(vlow.y, low.y), chunkY);
        const int cy1 = floorDiv(std::min(vhigh.y, low.y + size.y - 1), chunkY);
        const int cz0 = floorDiv(std::max(vlow.z, low.z), chunkZ);
        const int cz1 = floorDiv(std::min(vhigh.z, low.z + size.z - 1), chunkZ);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cz = cz0; cz <= cz1; ++cz)
                for (int cx = cx0; cx <= cx1; ++cx) {
                    const std::size_t slot = slotOf({cx, cy, cz});
                    scan_.members[slot].push_back(static_cast<std::uint32_t>(scan_.next));
                    scan_.signatures[slot] += hash;
                }
    }
}

// Chunks whose items changed take the items as they are now.
void LightGrid::finishScan(std::span<const DrawItem> items)
{
    if (!scan_.active || scan_.origin != origin_) {
        scan_ = {};
        return;
    }
    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        Chunk& chunk = slots_[slot];
        if (chunk.signature == scan_.signatures[slot] && !chunk.voxelize)
            continue;
        chunk.signature = scan_.signatures[slot];
        chunk.occluders.clear();
        for (std::uint32_t index : scan_.members[slot]) {
            Occluder occluder;
            if (index < items.size() && occluderFor(items[index], occluder))
                chunk.occluders.push_back(std::move(occluder));
        }
        chunk.voxelize = true;
    }
    scan_ = {};
    // Meshes no part uses any more.
    std::erase_if(bounds_, [](const auto& entry) { return entry.second.first.use_count() <= 1; });
    std::scoped_lock lock(meshMutex_);
    std::erase_if(meshBoxes_, [](const auto& entry) { return entry.second.mesh.use_count() <= 1; });
}

void LightGrid::moveTo(GridPoint origin)
{
    origin_ = origin;
    for (int y = 0; y < chunks_.y; ++y)
        for (int z = 0; z < chunks_.z; ++z)
            for (int x = 0; x < chunks_.x; ++x) {
                const GridPoint coord{origin.x + x, origin.y + y, origin.z + z};
                Chunk& chunk = slots_[slotOf(coord)];
                if (chunk.coord == coord)
                    continue;
                // A chunk that wrapped around: full light until it's worked out.
                chunk.coord = coord;
                for (const KeptStrength& kept : chunk.kept)
                    keptStrengthBytes_ -= kept.strength.size() * sizeof(float);
                chunk.kept.clear();
                chunk.recorded = false;
                chunk.relights = 0;
                chunk.occluders.clear();
                chunk.signature = 0;
                chunk.voxelize = true;
                std::ranges::fill(chunk.occupancy, std::uint8_t{0});
                std::ranges::fill(chunk.blockerHeight, std::uint8_t{128});
                std::ranges::fill(chunk.texels, std::uint8_t{255});
                std::ranges::fill(chunk.light, std::uint8_t{0});
                chunk.lightSignature = 0;
                chunk.relight = true;
                chunk.changed = true;
                skyDirty_ = sunDirty_ = true;
            }
}

void LightGrid::startWork(bool wait)
{
    const bool sky = skyDirty_, sun = sunDirty_;
    skyDirty_ = sunDirty_ = false;
    litWith_ = settings_;
    if (wait) {
        work(sky, sun);
        return;
    }
    working_ = true;
    {
        const std::scoped_lock lock(passMutex_);
        passPending_ = true;
        passSky_ = sky;
        passSun_ = sun;
    }
    if (!worker_.joinable())
        worker_ = std::jthread([this](const std::stop_token& stop) { passLoop(stop); });
    else
        passQueued_.notify_one();
}

void LightGrid::passLoop(const std::stop_token& stop)
{
    std::unique_lock lock(passMutex_);
    while (passQueued_.wait(lock, stop, [&] { return passPending_; })) {
        const bool sky = passSky_, sun = passSun_;
        lock.unlock();
        work(sky, sun);
        lock.lock();
        // Done under the lock, so wait() sees it with what the pass wrote.
        passPending_ = false;
        working_ = false;
        passDone_.notify_all();
    }
}

void LightGrid::wait()
{
    std::unique_lock lock(passMutex_);
    passDone_.wait(lock, [&] { return !passPending_; });
    working_ = false;
}

void LightGrid::work(bool sky, bool sun)
{
    std::vector<Chunk*> dirty;
    for (Chunk& chunk : slots_)
        if (chunk.voxelize)
            dirty.push_back(&chunk);
    // A shadowed light's way through a chunk changes with its occupancy:
    // every chunk it reaches is lit again.
    if (!dirty.empty()) {
        for (const LocalLight& light : lights_) {
            GridPoint c0, c1;
            if (!light.shadows || light.kind == LocalLight::Kind::Surface || !chunksReached(light, c0, c1))
                continue;
            const bool through = std::ranges::any_of(dirty, [&](const Chunk* changed) {
                return changed->coord.x >= c0.x && changed->coord.x <= c1.x && changed->coord.y >= c0.y
                       && changed->coord.y <= c1.y && changed->coord.z >= c0.z && changed->coord.z <= c1.z;
            });
            if (!through)
                continue;
            for (int cy = c0.y; cy <= c1.y; ++cy)
                for (int cz = c0.z; cz <= c1.z; ++cz)
                    for (int cx = c0.x; cx <= c1.x; ++cx)
                        slots_[slotOf({cx, cy, cz})].relight = true;
        }
    }
    parallelFor(dirty.size(), [&](std::size_t i) { voxelizeChunk(*dirty[i]); });
    if (!dirty.empty())
        ++occupancyGeneration_;
    // Parts that moved carry the light again from what they changed; the
    // grid following the camera, the sun turning or the shadows coming on
    // carry all of it.
    const bool partly = !dirty.empty() && !sky && !sun && litWith_.shadows;
    std::vector<std::uint8_t> touched;
    if (!dirty.empty() && !partly)
        sky = sun = true;
    std::vector<Chunk*> relit;
    for (Chunk& chunk : slots_)
        if (chunk.relight)
            relit.push_back(&chunk);
    if (!litWith_.shadows) {
        std::ranges::fill(skyGrid_, std::uint8_t{255});
        std::ranges::fill(skyBelowGrid_, std::uint8_t{255});
        std::ranges::fill(sunGrid_, std::uint8_t{255});
    } else if (partly) {
        touched.assign(slots_.size(), 0);
        for (const Chunk* chunk : dirty)
            touched[static_cast<std::size_t>(chunk - slots_.data())] = 1; // its blocker heights
        const Changes changes = occupancyChanges(dirty);
        carrySkyFrom(changes, touched);
        carrySunFrom(changes, touched);
    } else {
        // The sun goes one voxel after another; the sky spreads over threads.
        std::jthread sunThread;
        if (sun)
            sunThread = std::jthread([this] { carrySun(); });
        if (sky)
            carrySky();
    }
    if (!relit.empty()) {
        // Shadowed point and spot lights are stopped by what's around them
        // (SurfaceLights never are); the rest shine through.
        // [code: 0x10486df4f, sub_10486524c]
        // Only those reaching a chunk lit again need their occlusion worked
        // out: lightChunk reads a light's only where it reaches, which
        // chunksReached covers (a light flickering relights a chunk or two,
        // and each field is tens of thousands of voxels swept).
        auto reachesRelit = [&](const LocalLight& light) {
            GridPoint c0, c1;
            if (!chunksReached(light, c0, c1))
                return false;
            return std::ranges::any_of(relit, [&](const Chunk* chunk) {
                return chunk->coord.x >= c0.x && chunk->coord.x <= c1.x && chunk->coord.y >= c0.y
                       && chunk->coord.y <= c1.y && chunk->coord.z >= c0.z && chunk->coord.z <= c1.z;
            });
        };
        std::vector<std::size_t> shadowed;
        for (std::size_t i = 0; i < lights_.size(); ++i)
            if (lights_[i].shadows && lights_[i].kind != LocalLight::Kind::Surface && reachesRelit(lights_[i]))
                shadowed.push_back(i);
        std::vector<Occlusion> fields(shadowed.size());
        std::vector<const Occlusion*> byLight(lights_.size(), nullptr);
        // Each one kept from an earlier pass where nothing it depends on moved.
        std::vector<std::size_t> fresh;
        for (std::size_t i = 0; i < shadowed.size(); ++i) {
            const LocalLight& light = lights_[shadowed[i]];
            const auto kept = light.key ? keptOcclusion_.find(light.key) : keptOcclusion_.end();
            if (kept != keptOcclusion_.end() && kept->second.position == light.position
                && kept->second.range == light.range && kept->second.origin == origin_
                && kept->second.occupancy == occupancyGeneration_)
                byLight[shadowed[i]] = &kept->second.field;
            else
                fresh.push_back(i);
        }
        parallelFor(fresh.size(), [&](std::size_t f) { fields[fresh[f]] = occlusionOf(lights_[shadowed[fresh[f]]]); });
        for (std::size_t i : fresh) {
            const LocalLight& light = lights_[shadowed[i]];
            if (!light.key) {
                byLight[shadowed[i]] = &fields[i];
                continue;
            }
            KeptOcclusion& kept = keptOcclusion_[light.key];
            kept = {light.position, light.range, origin_, occupancyGeneration_, std::move(fields[i])};
            byLight[shadowed[i]] = &kept.field;
        }
        // Lights that are gone go.
        if (keptOcclusion_.size() > lights_.size())
            std::erase_if(keptOcclusion_, [&](const auto& entry) {
                return std::ranges::none_of(lights_, [&](const LocalLight& l) { return l.key == entry.first; });
            });
        parallelFor(relit.size(), [&](std::size_t i) { lightChunk(*relit[i], byLight); });
    }
    // Lights alone changed nothing the texels hold (sky, sun, blockers).
    if (sky || sun || !dirty.empty())
        storeTexels(partly ? &touched : nullptr);
}

void LightGrid::voxelizeChunk(Chunk& chunk)
{
    const std::vector<std::uint8_t> before = chunk.occupancy;
    std::ranges::fill(chunk.occupancy, std::uint8_t{0});
    chunk.voxelize = false;
    const GridPoint base{chunk.coord.x * chunkX, chunk.coord.y * chunkY, chunk.coord.z * chunkZ};
    std::vector<float> surfaces;
    BlockerHeights heights{std::vector<float>(chunkVoxels, 0.0f), std::vector<float>(chunkVoxels, 0.0f)};
    for (const Occluder& o : chunk.occluders) {
        const float opacity255 = o.opacity * 255.5f;
        switch (o.shape) {
        case Occluder::Shape::Box:
            addBox(chunk.occupancy.data(), base, boxFrom(o.model, {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}),
                   opacity255, nullptr, &heights);
            break;
        case Occluder::Shape::Wedge:
        case Occluder::Shape::CornerWedge:
        case Occluder::Shape::Cylinder:
        case Occluder::Shape::CylinderY:
        case Occluder::Shape::Ball: {
            static constexpr ShapeTest tests[]
                = {insideWedge, insideCornerWedge, insideCylinder, insideCylinderY, insideBall};
            addBox(chunk.occupancy.data(), base, boxFrom(o.model, {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}),
                   opacity255, tests[static_cast<int>(o.shape) - 1], &heights);
            break;
        }
        case Occluder::Shape::Mesh:
            for (const Bounds& box : o.boxes ? *o.boxes : boxesOf(o.mesh))
                addBox(chunk.occupancy.data(), base, boxFrom(o.model, box), opacity255, nullptr, &heights);
            break;
        case Occluder::Shape::Surface:
            if (surfaces.empty())
                surfaces.assign(3 * chunkVoxels, 0.0f);
            addSurface(surfaces, base, o.model, *o.mesh, heights);
            break;
        }
    }
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const int add = static_cast<int>(std::min(surfaces[i], 1.0f) * 255.5f);
        chunk.occupancy[i] = static_cast<std::uint8_t>(std::min(chunk.occupancy[i] + add, 255));
    }
    for (std::size_t i = 0; i < chunkVoxels; ++i) {
        const float height = heights.weight[i] > 0.0f ? std::clamp(heights.sum[i] / heights.weight[i], 0.0f, 1.0f) : 0.5f;
        chunk.blockerHeight[i] = static_cast<std::uint8_t>(height * 255.0f + 0.5f);
    }
    // What the light is carried again from.
    static_assert(chunkX == 32, "a row of a chunk is a word of occupancyChanged");
    std::ranges::fill(chunk.occupancyChanged, 0u);
    for (std::size_t i = 0; i < chunkVoxels; ++i)
        if (chunk.occupancy[i] != before[i] || chunk.occupancy[chunkVoxels + i] != before[chunkVoxels + i]
            || chunk.occupancy[2 * chunkVoxels + i] != before[2 * chunkVoxels + i])
            chunk.occupancyChanged[i / chunkX] |= 1u << (i % chunkX);
}

// Boxes standing in for a mesh. The client voxelizes a MeshPart as the
// boxes around its collision geometry's convex pieces (sub_1051b7700 via
// sub_1033b73da). GUESS: without them here, the mesh's box is cut into up
// to 4 cells along its longest side (as many along the others as fit) and
// each cell the surface passes through becomes the box around the surface
// in it.
const std::vector<Bounds>& LightGrid::boxesOf(const MeshPtr& mesh)
{
    {
        std::scoped_lock lock(meshMutex_);
        if (auto found = meshBoxes_.find(mesh.get()); found != meshBoxes_.end() && found->second.mesh == mesh)
            return found->second.boxes;
    }
    std::vector<Bounds> boxes;
    const Bounds bounds = meshBounds(mesh.get());
    const Vector3 extent = bounds.extent();
    const float longest = std::max({extent.x, extent.y, extent.z, 1e-6f});
    const float cell = longest / 4.0f;
    const int n[3] = {std::clamp(static_cast<int>(std::ceil(extent.x / cell - 0.01f)), 1, 4),
                      std::clamp(static_cast<int>(std::ceil(extent.y / cell - 0.01f)), 1, 4),
                      std::clamp(static_cast<int>(std::ceil(extent.z / cell - 0.01f)), 1, 4)};
    const Vector3 step{std::max(extent.x, 1e-6f) / static_cast<float>(n[0]),
                       std::max(extent.y, 1e-6f) / static_cast<float>(n[1]),
                       std::max(extent.z, 1e-6f) / static_cast<float>(n[2])};
    std::vector<Bounds> cells(static_cast<std::size_t>(n[0] * n[1] * n[2]));
    std::vector<bool> used(cells.size(), false);
    auto add = [&](Vector3 p) {
        const int x = std::clamp(static_cast<int>((p.x - bounds.min.x) / step.x), 0, n[0] - 1);
        const int y = std::clamp(static_cast<int>((p.y - bounds.min.y) / step.y), 0, n[1] - 1);
        const int z = std::clamp(static_cast<int>((p.z - bounds.min.z) / step.z), 0, n[2] - 1);
        const auto i
            = static_cast<std::size_t>(x)
              + static_cast<std::size_t>(n[0])
                    * (static_cast<std::size_t>(y) + static_cast<std::size_t>(n[1]) * static_cast<std::size_t>(z));
        if (!used[i]) {
            cells[i] = {p, p};
            used[i] = true;
            return;
        }
        Bounds& b = cells[i];
        b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    };
    const float spacing = std::min({step.x, step.y, step.z}) * 0.5f;
    const MeshData& m = *mesh;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const std::uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
        if (ia >= m.vertices.size() || ib >= m.vertices.size() || ic >= m.vertices.size())
            continue;
        const Vector3 a = m.vertices[ia].position, b = m.vertices[ib].position, c = m.vertices[ic].position;
        const float edge = std::max({(b - a).magnitude(), (c - b).magnitude(), (a - c).magnitude()});
        const int k = std::clamp(static_cast<int>(std::ceil(edge / spacing)), 1, 16);
        for (int i = 0; i <= k; ++i)
            for (int j = 0; i + j <= k; ++j)
                add(a + (b - a) * (static_cast<float>(i) / static_cast<float>(k))
                    + (c - a) * (static_cast<float>(j) / static_cast<float>(k)));
    }
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (used[i])
            boxes.push_back(cells[i]);
    if (boxes.empty())
        boxes.push_back(bounds);
    // Another thread may have worked them out meanwhile: the first one stays,
    // since that thread may be reading it.
    std::scoped_lock lock(meshMutex_);
    auto [it, inserted] = meshBoxes_.try_emplace(mesh.get());
    if (inserted || it->second.mesh != mesh) {
        it->second.mesh = mesh;
        it->second.boxes = std::move(boxes);
    }
    return it->second.boxes;
}

void LightGrid::carrySky()
{
    const GridPoint size = voxels();
    const auto width = static_cast<std::size_t>(size.x) + 2;
    // What comes down out of each voxel of the layer above, with a ring of
    // full sky around (the client's missing neighbours are full sky too).
    std::vector<std::uint8_t> above(width * static_cast<std::size_t>(size.z + 2), 255);
    std::vector<std::uint8_t> below = above;
    // Lunarstone's own: how much of the sky straight overhead each voxel of
    // the layer above still has, stopped only by what lies across a column
    // (floors, roofs), not by walls standing in it.
    std::vector<std::uint8_t> overheadAbove = above;
    std::vector<std::uint8_t> overheadBelow = above;
    const unsigned threads = std::min<unsigned>(workerThreads(), static_cast<unsigned>(size.z));
    int y = size.y - 1;
    auto layerDone = [&]() noexcept {
        std::swap(above, below);
        std::swap(overheadAbove, overheadBelow);
        --y;
    };
    std::barrier sync(static_cast<std::ptrdiff_t>(threads), layerDone);
    auto run = [&](unsigned t) {
        const int z0 = size.z * static_cast<int>(t) / static_cast<int>(threads);
        const int z1 = size.z * static_cast<int>(t + 1) / static_cast<int>(threads);
        while (y >= 0) {
            const int layer = y;
            const int cy = floorDiv(layer, chunkY);
            const std::size_t inLayer = static_cast<std::size_t>(wrap(layer, chunkY)) * layerVoxels;
            for (int z = z0; z < z1; ++z) {
                const std::uint8_t* up = above.data() + static_cast<std::size_t>(z + 1) * width + 1;
                std::uint8_t* out = below.data() + static_cast<std::size_t>(z + 1) * width + 1;
                const std::uint8_t* overheadUp = overheadAbove.data() + static_cast<std::size_t>(z + 1) * width + 1;
                std::uint8_t* overheadOut = overheadBelow.data() + static_cast<std::size_t>(z + 1) * width + 1;
                const std::size_t rowStart = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(size.z)
                                              + static_cast<std::size_t>(z))
                                             * static_cast<std::size_t>(size.x);
                std::uint8_t* sky = skyGrid_.data() + rowStart;
                std::uint8_t* skyBelow = skyBelowGrid_.data() + rowStart;
                std::uint8_t* carried = skyCarried_.data() + rowStart;
                std::uint8_t* overhead = overheadGrid_.data() + rowStart;
                for (int cx = 0; cx < chunks_.x; ++cx) {
                    const Chunk& chunk
                        = slots_[slotOf({origin_.x + cx, origin_.y + cy, origin_.z + floorDiv(z, chunkZ)})];
                    const std::size_t row = inLayer + static_cast<std::size_t>(wrap(z, chunkZ)) * chunkX;
                    const std::uint8_t* occX = chunk.occupancy.data() + row;
                    const std::uint8_t* occY = occX + chunkVoxels;
                    const std::uint8_t* occZ = occY + chunkVoxels;
                    for (int i = 0; i < chunkX; ++i) {
                        const std::ptrdiff_t x = cx * chunkX + i;
                        const int sum = up[x - 1 - static_cast<std::ptrdiff_t>(width)]
                                        + up[x - static_cast<std::ptrdiff_t>(width)]
                                        + up[x + 1 - static_cast<std::ptrdiff_t>(width)] + up[x - 1] + up[x + 1]
                                        + up[x - 1 + static_cast<std::ptrdiff_t>(width)]
                                        + up[x + static_cast<std::ptrdiff_t>(width)]
                                        + up[x + 1 + static_cast<std::ptrdiff_t>(width)];
                        const int light = (sum + 7) >> 3;
                        const int walls = std::max(occX[i] * 180 >> 8, occZ[i] * 180 >> 8);
                        // Lunarstone's own: of what this voxel shows, the
                        // light that came in from the side, with the sky
                        // overhead shut off, is held back by its own walls,
                        // which stand all the way up it. The client shows a
                        // room's voxels along an outer wall the open sky
                        // beyond it: the game's rooms, with the void all round
                        // them, went teal along their walls. Under open sky
                        // nothing changes, and what's carried down (out) is
                        // still the client's.
                        const int sideways = std::max(light - int{overheadUp[x]}, 0);
                        sky[x] = skyCurve(light - std::min(walls, sideways));
                        overheadOut[x] = occY[i] > walls
                                             ? static_cast<std::uint8_t>(std::max(overheadUp[x] - occY[i], 0))
                                             : overheadUp[x];
                        const int held = std::max(walls, int{occY[i]});
                        out[x] = static_cast<std::uint8_t>(std::max(light - held, 0));
                        skyBelow[x] = skyCurve(out[x]);
                        carried[x] = out[x];
                        overhead[x] = overheadOut[x];
                    }
                }
            }
            sync.arrive_and_wait();
        }
    };
    std::vector<std::jthread> pool;
    for (unsigned t = 1; t < threads; ++t)
        pool.emplace_back(run, t);
    run(0);
}

void LightGrid::carrySun()
{
    const GridPoint size = voxels();
    const Vector3 d = litWith_.sunDirection;
    if (d.magnitude() < 1e-6f) {
        std::ranges::fill(sunGrid_, std::uint8_t{255});
        std::ranges::fill(sunCarried_, std::uint8_t{255});
        return;
    }
    const Vector3 u = d.unit();
    const std::array<int, 3> w = sunWeights(u);
    // How much of a voxel's occupancy along each axis stands in the sun's way.
    const int along[3] = {static_cast<int>(std::abs(u.x) * 255.0f), static_cast<int>(std::abs(u.y) * 255.0f),
                          static_cast<int>(std::abs(u.z) * 255.0f)};
    const int sx = u.x > 0.0f ? 1 : -1, sy = u.y >= 0.0f ? 1 : -1, sz = u.z > 0.0f ? 1 : -1;
    const auto layerSize = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.z);
    // What leaves each voxel of the layer nearer the sun, and of this one.
    std::vector<std::uint8_t> previous(layerSize, 255), current(layerSize, 255);
    for (int step = 0; step < size.y; ++step) {
        const int y = sy > 0 ? size.y - 1 - step : step;
        const int cy = floorDiv(y, chunkY);
        const std::size_t inLayer = static_cast<std::size_t>(wrap(y, chunkY)) * layerVoxels;
        for (int zs = 0; zs < size.z; ++zs) {
            const int z = sz > 0 ? size.z - 1 - zs : zs;
            const int zn = z + sz;
            const std::uint8_t* rowBefore
                = zn >= 0 && zn < size.z
                      ? current.data() + static_cast<std::size_t>(zn) * static_cast<std::size_t>(size.x)
                      : nullptr;
            const std::uint8_t* rowAbove
                = previous.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x);
            std::uint8_t* row = current.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x);
            const std::size_t rowStart
                = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.z) + static_cast<std::size_t>(z))
                  * static_cast<std::size_t>(size.x);
            std::uint8_t* sunRow = sunGrid_.data() + rowStart;
            std::uint8_t* carriedRow = sunCarried_.data() + rowStart;
            const std::size_t rowInChunk = inLayer + static_cast<std::size_t>(wrap(z, chunkZ)) * chunkX;
            const int cz = floorDiv(z, chunkZ);
            for (int xs = 0; xs < size.x; ++xs) {
                const int x = sx > 0 ? size.x - 1 - xs : xs;
                const int xn = x + sx;
                const int fromX = xn >= 0 && xn < size.x ? row[xn] : 255;
                const int fromZ = rowBefore ? rowBefore[x] : 255;
                const int fromY = rowAbove[x];
                const int light = (w[0] * fromX + w[1] * fromY + w[2] * fromZ + 255) >> 8;
                sunRow[x] = static_cast<std::uint8_t>(std::min(light, 255));
                const Chunk& chunk = slots_[slotOf({origin_.x + floorDiv(x, chunkX), origin_.y + cy, origin_.z + cz})];
                const std::size_t i = rowInChunk + static_cast<std::size_t>(wrap(x, chunkX));
                const int held = (chunk.occupancy[i] * along[0] + chunk.occupancy[chunkVoxels + i] * along[1]
                                  + chunk.occupancy[2 * chunkVoxels + i] * along[2] + 127)
                                 >> 8;
                row[x] = static_cast<std::uint8_t>(std::clamp(light - held, 0, 255));
                carriedRow[x] = row[x];
            }
        }
        std::swap(previous, current);
    }
}

LightGrid::Changes LightGrid::occupancyChanges(std::span<Chunk* const> chunks) const
{
    const GridPoint size = voxels();
    const std::size_t words = rowWords();
    Changes changes;
    changes.layers.assign(static_cast<std::size_t>(size.y), LayerMask(static_cast<std::size_t>(size.z) * words, 0));
    changes.lowest = size.y;
    for (const Chunk* chunk : chunks) {
        const GridPoint low{(chunk->coord.x - origin_.x) * chunkX, (chunk->coord.y - origin_.y) * chunkY,
                            (chunk->coord.z - origin_.z) * chunkZ};
        for (int y = 0; y < chunkY; ++y)
            for (int z = 0; z < chunkZ; ++z) {
                const std::uint32_t bits
                    = chunk->occupancyChanged[static_cast<std::size_t>(y) * chunkZ + static_cast<std::size_t>(z)];
                if (bits == 0)
                    continue;
                const int layer = low.y + y;
                const auto x = static_cast<std::size_t>(low.x);
                changes.layers[static_cast<std::size_t>(layer)][static_cast<std::size_t>(low.z + z) * words + x / 64]
                    |= std::uint64_t{bits} << (x % 64);
                changes.lowest = std::min(changes.lowest, layer);
                changes.highest = std::max(changes.highest, layer);
            }
    }
    return changes;
}

namespace {

// The next set bit of a row at or past `from`, going `step` (1 or -1)
// along it; -1 when there's none before its end.
int nextSet(const std::uint64_t* row, std::size_t words, int from, int step)
{
    if (from < 0 || static_cast<std::size_t>(from) >= words * 64)
        return -1;
    auto w = static_cast<std::size_t>(from) / 64;
    const auto b = static_cast<unsigned>(from) % 64;
    if (step > 0) {
        std::uint64_t bits = row[w] & (~std::uint64_t{0} << b);
        while (bits == 0) {
            if (++w == words)
                return -1;
            bits = row[w];
        }
        return static_cast<int>(w * 64) + std::countr_zero(bits);
    }
    std::uint64_t bits = row[w] & (b == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (b + 1)) - 1);
    while (bits == 0) {
        if (w-- == 0)
            return -1;
        bits = row[w];
    }
    return static_cast<int>(w * 64) + 63 - std::countl_zero(bits);
}

} // namespace

// carrySky's arithmetic for the voxels that can come out differently: those
// whose occupancy changed and those under and beside a voxel that passes on
// something new, layer by layer down to where nothing new comes through.
void LightGrid::carrySkyFrom(const Changes& changes, std::vector<std::uint8_t>& touched)
{
    if (changes.highest < changes.lowest)
        return;
    const GridPoint size = voxels();
    const std::size_t words = rowWords();
    const auto rows = static_cast<std::size_t>(size.z);
    auto at = [&](int x, int y, int z) {
        return (static_cast<std::size_t>(y) * rows + static_cast<std::size_t>(z)) * static_cast<std::size_t>(size.x)
               + static_cast<std::size_t>(x);
    };
    LayerMask passed(rows * words, 0), wider(rows * words, 0), need(rows * words, 0);
    bool anyPassed = false;
    for (int y = changes.highest; y >= 0; --y) {
        if (!anyPassed && y < changes.lowest)
            break;
        // What to work out again: this layer's changed occupancy, and the
        // 3 x 3 under each voxel above whose carried or overhead sky changed.
        need = changes.layers[static_cast<std::size_t>(y)];
        if (anyPassed) {
            for (std::size_t z = 0; z < rows; ++z)
                for (std::size_t w = 0; w < words; ++w) {
                    const std::uint64_t v = passed[z * words + w];
                    const std::uint64_t fromLow = w > 0 ? passed[z * words + w - 1] >> 63 : 0;
                    const std::uint64_t fromHigh = w + 1 < words ? passed[z * words + w + 1] << 63 : 0;
                    wider[z * words + w] = v | (v << 1) | fromLow | (v >> 1) | fromHigh;
                }
            for (std::size_t z = 0; z < rows; ++z)
                for (std::size_t w = 0; w < words; ++w)
                    need[z * words + w] |= wider[z * words + w] | (z > 0 ? wider[(z - 1) * words + w] : 0)
                                           | (z + 1 < rows ? wider[(z + 1) * words + w] : 0);
        }
        std::ranges::fill(passed, 0);
        anyPassed = false;
        const int cy = y / chunkY;
        const std::size_t inLayer = static_cast<std::size_t>(y % chunkY) * layerVoxels;
        // What the voxel above passed down: full sky above the grid and around it.
        auto carriedAbove = [&](int x, int z) -> int {
            if (y == size.y - 1 || x < 0 || z < 0 || x >= size.x || z >= size.z)
                return 255;
            return skyCarried_[at(x, y + 1, z)];
        };
        for (int z = 0; z < size.z; ++z) {
            const std::uint64_t* row = need.data() + static_cast<std::size_t>(z) * words;
            for (int x = nextSet(row, words, 0, 1); x >= 0 && x < size.x; x = nextSet(row, words, x + 1, 1)) {
                const std::size_t slot = slotOf({origin_.x + x / chunkX, origin_.y + cy, origin_.z + z / chunkZ});
                const Chunk& chunk = slots_[slot];
                const std::size_t i
                    = inLayer + static_cast<std::size_t>(z % chunkZ) * chunkX + static_cast<std::size_t>(x % chunkX);
                const int occX = chunk.occupancy[i], occY = chunk.occupancy[chunkVoxels + i],
                          occZ = chunk.occupancy[2 * chunkVoxels + i];
                const int sum = carriedAbove(x - 1, z - 1) + carriedAbove(x, z - 1) + carriedAbove(x + 1, z - 1)
                                + carriedAbove(x - 1, z) + carriedAbove(x + 1, z) + carriedAbove(x - 1, z + 1)
                                + carriedAbove(x, z + 1) + carriedAbove(x + 1, z + 1);
                const int overheadUp = y == size.y - 1 ? 255 : overheadGrid_[at(x, y + 1, z)];
                const int light = (sum + 7) >> 3;
                const int walls = std::max(occX * 180 >> 8, occZ * 180 >> 8);
                const int sideways = std::max(light - overheadUp, 0);
                const std::uint8_t sky = skyCurve(light - std::min(walls, sideways));
                const auto overhead
                    = static_cast<std::uint8_t>(occY > walls ? std::max(overheadUp - occY, 0) : overheadUp);
                const auto out = static_cast<std::uint8_t>(std::max(light - std::max(walls, occY), 0));
                const std::uint8_t below = skyCurve(out);
                const std::size_t v = at(x, y, z);
                if (skyGrid_[v] != sky || skyBelowGrid_[v] != below) {
                    skyGrid_[v] = sky;
                    skyBelowGrid_[v] = below;
                    touched[slot] = 1;
                }
                if (skyCarried_[v] != out || overheadGrid_[v] != overhead) {
                    skyCarried_[v] = out;
                    overheadGrid_[v] = overhead;
                    passed[static_cast<std::size_t>(z) * words + static_cast<std::size_t>(x) / 64]
                        |= std::uint64_t{1} << (static_cast<unsigned>(x) % 64);
                    anyPassed = true;
                }
            }
        }
    }
}

// carrySun's sweep for the voxels that can come out differently: those
// whose occupancy changed, those after a voxel the sun's new light leaves
// along the sweep (next in the row, in the next row, in the next layer),
// on until nothing new comes through.
void LightGrid::carrySunFrom(const Changes& changes, std::vector<std::uint8_t>& touched)
{
    const Vector3 d = litWith_.sunDirection;
    if (changes.highest < changes.lowest || d.magnitude() < 1e-6f)
        return; // without a sun it's full everywhere, whatever is in the way
    const GridPoint size = voxels();
    const std::size_t words = rowWords();
    const auto rows = static_cast<std::size_t>(size.z);
    const Vector3 u = d.unit();
    const std::array<int, 3> w = sunWeights(u);
    const int along[3] = {static_cast<int>(std::abs(u.x) * 255.0f), static_cast<int>(std::abs(u.y) * 255.0f),
                          static_cast<int>(std::abs(u.z) * 255.0f)};
    const int sx = u.x > 0.0f ? 1 : -1, sy = u.y >= 0.0f ? 1 : -1, sz = u.z > 0.0f ? 1 : -1;
    auto at = [&](int x, int y, int z) {
        return (static_cast<std::size_t>(y) * rows + static_cast<std::size_t>(z)) * static_cast<std::size_t>(size.x)
               + static_cast<std::size_t>(x);
    };
    auto carriedAt = [&](int x, int y, int z) -> int {
        if (x < 0 || y < 0 || z < 0 || x >= size.x || y >= size.y || z >= size.z)
            return 255;
        return sunCarried_[at(x, y, z)];
    };
    // The layers in the sweep's order, from the first that changed.
    const int first = sy > 0 ? changes.highest : changes.lowest;
    LayerMask before(rows * words, 0), passed(rows * words, 0); // the layer nearer the sun's, this one's
    std::vector<std::uint64_t> need(words), rowBefore(words, 0), rowPassed(words, 0);
    bool anyBefore = false;
    for (int y = first; y >= 0 && y < size.y; y -= sy) {
        if (!anyBefore && (sy > 0 ? y < changes.lowest : y > changes.highest))
            break;
        std::ranges::fill(passed, 0);
        bool anyPassed = false;
        const LayerMask& occupied = changes.layers[static_cast<std::size_t>(y)];
        const int cy = y / chunkY;
        const std::size_t inLayer = static_cast<std::size_t>(y % chunkY) * layerVoxels;
        std::ranges::fill(rowBefore, 0);
        for (int zs = 0; zs < size.z; ++zs) {
            const int z = sz > 0 ? size.z - 1 - zs : zs;
            const std::size_t r = static_cast<std::size_t>(z) * words;
            bool any = false;
            for (std::size_t k = 0; k < words; ++k) {
                need[k] = occupied[r + k] | rowBefore[k] | (anyBefore ? before[r + k] : 0);
                any = any || need[k] != 0;
            }
            std::ranges::fill(rowPassed, 0);
            if (any) {
                // Along the row away from the sun, as carrySun goes; a voxel
                // whose light changed sends the next one round too.
                const int step = -sx;
                int cursor = sx > 0 ? size.x - 1 : 0;
                bool carry = false;
                while (cursor >= 0 && cursor < size.x) {
                    const int x = carry ? cursor : nextSet(need.data(), words, cursor, step);
                    if (x < 0 || x >= size.x)
                        break;
                    const int fromX = carriedAt(x + sx, y, z);
                    const int fromY = carriedAt(x, y + sy, z);
                    const int fromZ = carriedAt(x, y, z + sz);
                    const int light = (w[0] * fromX + w[1] * fromY + w[2] * fromZ + 255) >> 8;
                    const auto sun = static_cast<std::uint8_t>(std::min(light, 255));
                    const std::size_t slot = slotOf({origin_.x + x / chunkX, origin_.y + cy, origin_.z + z / chunkZ});
                    const Chunk& chunk = slots_[slot];
                    const std::size_t i = inLayer + static_cast<std::size_t>(z % chunkZ) * chunkX
                                          + static_cast<std::size_t>(x % chunkX);
                    const int held = (chunk.occupancy[i] * along[0] + chunk.occupancy[chunkVoxels + i] * along[1]
                                      + chunk.occupancy[2 * chunkVoxels + i] * along[2] + 127)
                                     >> 8;
                    const auto carried = static_cast<std::uint8_t>(std::clamp(light - held, 0, 255));
                    const std::size_t v = at(x, y, z);
                    if (sunGrid_[v] != sun) {
                        sunGrid_[v] = sun;
                        touched[slot] = 1;
                    }
                    carry = sunCarried_[v] != carried;
                    if (carry) {
                        sunCarried_[v] = carried;
                        rowPassed[static_cast<std::size_t>(x) / 64] |= std::uint64_t{1}
                                                                       << (static_cast<unsigned>(x) % 64);
                        anyPassed = true;
                    }
                    cursor = x + step;
                }
            }
            std::ranges::copy(rowPassed, passed.begin() + static_cast<std::ptrdiff_t>(r));
            rowBefore = rowPassed;
        }
        std::swap(before, passed);
        anyBefore = anyPassed;
    }
}

void LightGrid::storeTexels(const std::vector<std::uint8_t>* only)
{
    const GridPoint size = voxels();
    for (int cy = 0; cy < chunks_.y; ++cy)
        for (int cz = 0; cz < chunks_.z; ++cz)
            for (int cx = 0; cx < chunks_.x; ++cx) {
                const std::size_t slot = slotOf({origin_.x + cx, origin_.y + cy, origin_.z + cz});
                if (only && !(*only)[slot])
                    continue;
                Chunk& chunk = slots_[slot];
                bool changed = false;
                for (int y = 0; y < chunkY; ++y)
                    for (int z = 0; z < chunkZ; ++z) {
                        const std::size_t from
                            = (static_cast<std::size_t>(cy * chunkY + y) * static_cast<std::size_t>(size.z)
                               + static_cast<std::size_t>(cz * chunkZ + z))
                                  * static_cast<std::size_t>(size.x)
                              + static_cast<std::size_t>(cx * chunkX);
                        std::uint8_t* out
                            = chunk.texels.data()
                              + (static_cast<std::size_t>(y) * layerVoxels + static_cast<std::size_t>(z) * chunkX) * 4;
                        for (int x = 0; x < chunkX; ++x) {
                            const std::uint8_t s = skyGrid_[from + static_cast<std::size_t>(x)];
                            const std::uint8_t g = sunGrid_[from + static_cast<std::size_t>(x)];
                            const std::uint8_t b = skyBelowGrid_[from + static_cast<std::size_t>(x)];
                            const std::uint8_t h = chunk.blockerHeight[static_cast<std::size_t>(y) * layerVoxels
                                                                       + static_cast<std::size_t>(z) * chunkX
                                                                       + static_cast<std::size_t>(x)];
                            const auto at = static_cast<std::size_t>(x) * 4;
                            changed = changed || out[at] != s || out[at + 1] != g || out[at + 2] != b
                                      || out[at + 3] != h;
                            out[at] = s;
                            out[at + 1] = g;
                            out[at + 2] = b;
                            out[at + 3] = h;
                        }
                    }
                chunk.changed = chunk.changed || changed;
            }
}

bool LightGrid::chunksReached(const LocalLight& light, GridPoint& first, GridPoint& last) const
{
    // A voxel more all round: the blur reaches that far.
    const GridPoint low = originVoxel(), size = voxels();
    const float reach = light.reach() + voxelStuds;
    const GridPoint a = voxelAt(light.position - Vector3{reach, reach, reach});
    const GridPoint b = voxelAt(light.position + Vector3{reach, reach, reach});
    if (b.x < low.x || b.y < low.y || b.z < low.z || a.x >= low.x + size.x || a.y >= low.y + size.y
        || a.z >= low.z + size.z)
        return false;
    first = {floorDiv(std::max(a.x, low.x), chunkX), floorDiv(std::max(a.y, low.y), chunkY),
             floorDiv(std::max(a.z, low.z), chunkZ)};
    last = {floorDiv(std::min(b.x, low.x + size.x - 1), chunkX), floorDiv(std::min(b.y, low.y + size.y - 1), chunkY),
            floorDiv(std::min(b.z, low.z + size.z - 1), chunkZ)};
    return true;
}

void LightGrid::checkLights(std::span<const LocalLight> lights)
{
    lights_.assign(lights.begin(), lights.end());
    std::vector<std::uint64_t> signatures(slots_.size(), 0);
    for (const LocalLight& light : lights_) {
        std::uint64_t hash = mix(std::bit_cast<std::uint64_t>(light.key),
                                 static_cast<std::uint64_t>(light.kind) * 2 + (light.shadows ? 1 : 0));
        for (float f : {light.position.x, light.position.y, light.position.z, light.color.r, light.color.g,
                        light.color.b, light.brightness, light.range, light.direction.x, light.direction.y,
                        light.direction.z, light.cosHalfAngle, light.axisU.x, light.axisU.y, light.axisU.z,
                        light.axisV.x, light.axisV.y, light.axisV.z, light.halfU, light.halfV})
            hash = mix(hash, std::bit_cast<std::uint32_t>(f));
        GridPoint c0, c1;
        if (!chunksReached(light, c0, c1))
            continue;
        for (int cy = c0.y; cy <= c1.y; ++cy)
            for (int cz = c0.z; cz <= c1.z; ++cz)
                for (int cx = c0.x; cx <= c1.x; ++cx)
                    signatures[slotOf({cx, cy, cz})] += hash | 1u;
    }
    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        Chunk& chunk = slots_[slot];
        if (chunk.lightSignature == signatures[slot])
            continue;
        chunk.lightSignature = signatures[slot];
        chunk.relight = true;
    }
}

LightGrid::Occlusion LightGrid::occlusionOf(const LocalLight& light) const
{
    // How much of a shadowed light gets through to each voxel around it
    // (0-255), swept out from its voxel an octant at a time the client's
    // way (sub_104868398 and its siblings): each voxel takes what reaches
    // its three neighbours a step nearer the light, weighted by the square
    // of how far it is from the light along each axis, less its own
    // occupancy along the way the same way weighted.
    Occlusion out;
    const int r = static_cast<int>(std::ceil(light.range / voxelStuds)) + 1;
    out.center = voxelAt(light.position);
    out.radius = r;
    const auto side = static_cast<std::size_t>(2 * r + 1);
    out.light.assign(side * side * side, 0.0f);
    const GridPoint low = originVoxel(), size = voxels();
    auto occupancyAt = [&](GridPoint v, int axis) -> float {
        if (v.x < low.x || v.y < low.y || v.z < low.z || v.x >= low.x + size.x || v.y >= low.y + size.y
            || v.z >= low.z + size.z)
            return 0.0f;
        const Chunk& in = slots_[slotOf({floorDiv(v.x, chunkX), floorDiv(v.y, chunkY), floorDiv(v.z, chunkZ)})];
        const std::size_t i = static_cast<std::size_t>(wrap(v.y, chunkY)) * layerVoxels
                              + static_cast<std::size_t>(wrap(v.z, chunkZ)) * chunkX
                              + static_cast<std::size_t>(wrap(v.x, chunkX));
        return in.occupancy[static_cast<std::size_t>(axis) * chunkVoxels + i];
    };
    auto at = [&](int i, int j, int k) -> float& {
        return out.light[(static_cast<std::size_t>(j + r) * side + static_cast<std::size_t>(k + r)) * side
                         + static_cast<std::size_t>(i + r)];
    };
    const Vector3 from = light.position / voxelStuds;
    for (int ai = 0; ai <= r; ++ai)
        for (int aj = 0; aj <= r; ++aj)
            for (int ak = 0; ak <= r; ++ak)
                for (int si = ai == 0 ? 1 : -1; si <= 1; si += 2)
                    for (int sj = aj == 0 ? 1 : -1; sj <= 1; sj += 2)
                        for (int sk = ak == 0 ? 1 : -1; sk <= 1; sk += 2) {
                            const int i = ai * si, j = aj * sj, k = ak * sk;
                            if (ai == 0 && aj == 0 && ak == 0) {
                                at(0, 0, 0) = 255.0f;
                                continue;
                            }
                            const GridPoint v{out.center.x + i, out.center.y + j, out.center.z + k};
                            const Vector3 d = Vector3{static_cast<float>(v.x) + 0.5f, static_cast<float>(v.y) + 0.5f,
                                                      static_cast<float>(v.z) + 0.5f}
                                              - from;
                            // Only axes it's a step away from the light along.
                            // GUESS: the client's weights for the others.
                            const float wx = ai > 0 ? d.x * d.x : 0.0f;
                            const float wy = aj > 0 ? d.y * d.y : 0.0f;
                            const float wz = ak > 0 ? d.z * d.z : 0.0f;
                            const float total = wx + wy + wz;
                            if (total <= 0.0f) {
                                at(i, j, k) = ai > 0 ? at(i - si, j, k) : aj > 0 ? at(i, j - sj, k) : at(i, j, k - sk);
                                continue;
                            }
                            const float came = (ai > 0 ? wx * at(i - si, j, k) : 0.0f)
                                               + (aj > 0 ? wy * at(i, j - sj, k) : 0.0f)
                                               + (ak > 0 ? wz * at(i, j, k - sk) : 0.0f);
                            const float held = wx * occupancyAt(v, 0) + wy * occupancyAt(v, 1) + wz * occupancyAt(v, 2);
                            at(i, j, k) = std::max((came - held) / total, 0.0f);
                        }
    return out;
}

std::uint64_t LightGrid::strengthShape(const LocalLight& light, bool shadowed) const
{
    std::uint64_t hash = mix(static_cast<std::uint64_t>(light.kind), shadowed ? 1 : 0);
    for (float f : {light.position.x, light.position.y, light.position.z, light.range, light.direction.x,
                    light.direction.y, light.direction.z, light.cosHalfAngle, light.axisU.x, light.axisU.y,
                    light.axisU.z, light.axisV.x, light.axisV.y, light.axisV.z, light.halfU, light.halfV})
        hash = mix(hash, std::bit_cast<std::uint32_t>(f));
    if (shadowed) {
        hash = mix(hash, occupancyGeneration_);
        for (int v : {origin_.x, origin_.y, origin_.z})
            hash = mix(hash, static_cast<std::uint32_t>(v));
    }
    return hash;
}

void LightGrid::lightChunk(Chunk& chunk, std::span<const Occlusion* const> occlusion)
{
    chunk.relight = false;
    ++chunk.relights;
    // The chunk and a voxel all round, for the blur, in world voxels.
    constexpr int px = chunkX + 2, py = chunkY + 2, pz = chunkZ + 2;
    const GridPoint base{chunk.coord.x * chunkX - 1, chunk.coord.y * chunkY - 1, chunk.coord.z * chunkZ - 1};
    const GridPoint last{base.x + px - 1, base.y + py - 1, base.z + pz - 1};

    // The lights reaching it, in order, and what each adds now.
    struct Reach {
        std::size_t light;
        GridPoint low, high; // its region, inclusive
        std::uint64_t shape;
        Color3 color;
    };
    std::vector<Reach> reaching;
    bool keyed = true;
    for (std::size_t li = 0; li < lights_.size(); ++li) {
        const LocalLight& light = lights_[li];
        const float reach = light.reach();
        const GridPoint a = voxelAt(light.position - Vector3{reach, reach, reach});
        const GridPoint b = voxelAt(light.position + Vector3{reach, reach, reach});
        const GridPoint low{std::max(a.x, base.x), std::max(a.y, base.y), std::max(a.z, base.z)};
        const GridPoint high{std::min(b.x, last.x), std::min(b.y, last.y), std::min(b.z, last.z)};
        if (low.x > high.x || low.y > high.y || low.z > high.z)
            continue;
        reaching.push_back({li, low, high, strengthShape(light, occlusion[li] != nullptr), light.gridColor()});
        keyed = keyed && light.key;
    }

    // Each voxel's sum is what the lights covering it add, in order, so
    // where none of what a light adds changed since the last lighting, the
    // sums and so the texels there are what they were: only the box around
    // what changed is worked out again, bit for bit as the whole would be.
    struct Box {
        GridPoint low{0x7fffffff, 0x7fffffff, 0x7fffffff}, high{-0x7fffffff, -0x7fffffff, -0x7fffffff};
        bool empty() const { return low.x > high.x; }
        void add(GridPoint l, GridPoint h)
        {
            low = {std::min(low.x, l.x), std::min(low.y, l.y), std::min(low.z, l.z)};
            high = {std::max(high.x, h.x), std::max(high.y, h.y), std::max(high.z, h.z)};
        }
    };
    Box dirty;
    bool full = !chunk.recorded || !keyed || reaching.size() != chunk.kept.size();
    for (std::size_t i = 0; i < reaching.size() && !full; ++i) {
        const Reach& r = reaching[i];
        const KeptStrength& k = chunk.kept[i];
        if (k.key != lights_[r.light].key) {
            full = true;
            break;
        }
        if (k.shape != r.shape || k.low != r.low || k.high != r.high || k.color.r != r.color.r || k.color.g != r.color.g
            || k.color.b != r.color.b) {
            dirty.add(k.low, k.high);
            dirty.add(r.low, r.high);
        }
    }
    if (full)
        dirty.add(base, last);
    else if (dirty.empty())
        return; // the same lights adding the same: the same texels
    // The texels whose blur reads a changed sum, and the sums they read.
    const GridPoint outLow{std::max(dirty.low.x - 1, base.x + 1), std::max(dirty.low.y - 1, base.y + 1),
                           std::max(dirty.low.z - 1, base.z + 1)};
    const GridPoint outHigh{std::min(dirty.high.x + 1, last.x - 1), std::min(dirty.high.y + 1, last.y - 1),
                            std::min(dirty.high.z + 1, last.z - 1)};
    const GridPoint inLow{outLow.x - 1, outLow.y - 1, outLow.z - 1};
    const GridPoint inHigh{outHigh.x + 1, outHigh.y + 1, outHigh.z + 1};
    const int ix = inHigh.x - inLow.x + 1, iy = inHigh.y - inLow.y + 1, iz = inHigh.z - inLow.z + 1;

    // What the chunk keeps now, counted before the lights below move their
    // strengths out of it.
    std::size_t keptBefore = 0;
    for (const KeptStrength& k : chunk.kept)
        keptBefore += k.strength.size() * sizeof(float);
    std::vector<float> sum;
    std::vector<KeptStrength> kept;
    kept.reserve(reaching.size());
    for (std::size_t i = 0; i < reaching.size(); ++i) {
        const Reach& r = reaching[i];
        const LocalLight& light = lights_[r.light];
        const Occlusion* field = occlusion[r.light];
        const Color3 color = r.color;
        // What of its region the sums here need.
        const int x0 = std::max(r.low.x, inLow.x), x1 = std::min(r.high.x, inHigh.x);
        const int y0 = std::max(r.low.y, inLow.y), y1 = std::min(r.high.y, inHigh.y);
        const int z0 = std::max(r.low.z, inLow.z), z1 = std::min(r.high.z, inHigh.z);
        KeptStrength record{light.key, r.shape, r.low, r.high, color, {}};
        // What it added last time: in the same place in the order, or
        // after lights came or went, wherever it was.
        KeptStrength* before = nullptr;
        if (!full)
            before = &chunk.kept[i];
        else if (light.key && chunk.recorded)
            for (KeptStrength& k : chunk.kept)
                if (k.key == light.key) {
                    before = &k;
                    break;
                }
        if (x0 > x1 || y0 > y1 || z0 > z1) {
            // Unchanged and nowhere near what changed.
            if (before)
                record.strength = std::move(before->strength);
            kept.push_back(std::move(record));
            continue;
        }
        if (sum.empty())
            sum.assign(std::size_t{3} * static_cast<std::size_t>(ix) * static_cast<std::size_t>(iy)
                           * static_cast<std::size_t>(iz),
                       0.0f);
        auto add = [&](int x, int y, int z, float strength) {
            const std::size_t at = ((static_cast<std::size_t>(y - inLow.y) * static_cast<std::size_t>(iz)
                                     + static_cast<std::size_t>(z - inLow.z))
                                        * static_cast<std::size_t>(ix)
                                    + static_cast<std::size_t>(x - inLow.x))
                                   * 3;
            sum[at] += color.r * strength;
            sum[at + 1] += color.g * strength;
            sum[at + 2] += color.b * strength;
        };
        const auto width = static_cast<std::size_t>(r.high.x - r.low.x + 1);
        const auto depth = static_cast<std::size_t>(r.high.z - r.low.z + 1);
        // Its strengths from an earlier lighting, when nothing they come
        // from changed (a light dimming or changing colour keeps them).
        if (light.key)
            if (KeptStrength* k = before;
                k && !k->strength.empty() && k->shape == r.shape && k->low == r.low && k->high == r.high) {
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z) {
                        const float* strength
                            = k->strength.data()
                              + (static_cast<std::size_t>(y - r.low.y) * depth + static_cast<std::size_t>(z - r.low.z))
                                    * width
                              + static_cast<std::size_t>(x0 - r.low.x);
                        for (int x = x0; x <= x1; ++x, ++strength)
                            if (*strength > 0.0f)
                                add(x, y, z, *strength);
                    }
                record.strength = std::move(k->strength);
                kept.push_back(std::move(record));
                continue;
            }
        // Kept for a chunk lit more than once, within the budget, when the
        // whole of its region is worked out.
        const bool whole
            = x0 == r.low.x && x1 == r.high.x && y0 == r.low.y && y1 == r.high.y && z0 == r.low.z && z1 == r.high.z;
        const std::size_t region = width * static_cast<std::size_t>(r.high.y - r.low.y + 1) * depth;
        std::vector<float> keep;
        if (light.key && whole && chunk.relights > 1
            && keptStrengthBytes_.load(std::memory_order_relaxed) + region * sizeof(float) <= keptStrengthBudget)
            keep.assign(region, 0.0f);
        // A light from a point gives nothing from its range out: each row
        // only across its sphere (and a voxel more, for float error).
        const bool round = light.kind != LocalLight::Kind::Surface && light.halfU == 0.0f && light.halfV == 0.0f;
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z) {
                int from = x0, to = x1;
                if (round) {
                    const float ey = (static_cast<float>(y) + 0.5f) * voxelStuds - light.position.y;
                    const float ez = (static_cast<float>(z) + 0.5f) * voxelStuds - light.position.z;
                    const float left = light.range * light.range - ey * ey - ez * ez;
                    if (left < -2.0f * voxelStuds * light.range - 4.0f * voxelStuds * voxelStuds)
                        continue;
                    const float half = std::sqrt(std::max(left, 0.0f)) / voxelStuds + 1.0f;
                    const float middle = light.position.x / voxelStuds - 0.5f;
                    from = std::max(x0, static_cast<int>(std::floor(middle - half)));
                    to = std::min(x1, static_cast<int>(std::ceil(middle + half)));
                }
                for (int x = from; x <= to; ++x) {
                    const Vector3 center{(static_cast<float>(x) + 0.5f) * voxelStuds,
                                         (static_cast<float>(y) + 0.5f) * voxelStuds,
                                         (static_cast<float>(z) + 0.5f) * voxelStuds};
                    float strength = light.gridStrength(center);
                    if (strength <= 0.0f)
                        continue;
                    if (field) {
                        const int fi = x - field->center.x, fj = y - field->center.y, fk = z - field->center.z;
                        const int fr = field->radius;
                        if (std::abs(fi) > fr || std::abs(fj) > fr || std::abs(fk) > fr)
                            continue;
                        const auto side = static_cast<std::size_t>(2 * fr + 1);
                        strength *= field->light[(static_cast<std::size_t>(fj + fr) * side
                                                  + static_cast<std::size_t>(fk + fr))
                                                     * side
                                                 + static_cast<std::size_t>(fi + fr)]
                                    / 255.0f;
                    }
                    if (!keep.empty())
                        keep[(static_cast<std::size_t>(y - r.low.y) * depth + static_cast<std::size_t>(z - r.low.z))
                                 * width
                             + static_cast<std::size_t>(x - r.low.x)] = strength;
                    add(x, y, z, strength);
                }
            }
        record.strength = std::move(keep);
        kept.push_back(std::move(record));
    }
    std::size_t bytes = 0;
    for (const KeptStrength& k : kept)
        bytes += k.strength.size() * sizeof(float);
    keptStrengthBytes_ -= keptBefore;
    keptStrengthBytes_ += bytes;
    chunk.kept = std::move(kept);
    chunk.recorded = keyed;

    // A 3 x 3 x 3 box blur, across x and z then up y, and encoded the
    // client's way (sub_10486214e): alpha the brightest channel (at most
    // 120) over 120 in 255ths rounded up, 13 at least, the colour the rest.
    // Over the output box only, each value summed in the same order as over
    // the whole chunk.
    const int ox = outHigh.x - outLow.x + 1, oz = outHigh.z - outLow.z + 1;
    const int fy = iy; // the blur across x and z, for every layer the one up y reads
    std::vector<float> flat;
    if (!sum.empty()) {
        flat.assign(std::size_t{3} * static_cast<std::size_t>(fy) * static_cast<std::size_t>(oz)
                        * static_cast<std::size_t>(ox),
                    0.0f);
        const std::size_t run = std::size_t{3} * static_cast<std::size_t>(ox);
        for (int y = 0; y < fy; ++y)
            for (int z = 0; z < oz; ++z) {
                float* to = flat.data()
                            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(oz) + static_cast<std::size_t>(z))
                                  * run;
                for (int dz = 0; dz < 3; ++dz)
                    for (int dx = 0; dx < 3; ++dx) {
                        // Input z, x of output (z, 0): outLow - inLow - 1 + d = d.
                        const float* from = sum.data()
                                            + ((static_cast<std::size_t>(y) * static_cast<std::size_t>(iz)
                                                + static_cast<std::size_t>(z + dz))
                                                   * static_cast<std::size_t>(ix)
                                               + static_cast<std::size_t>(dx))
                                                  * 3;
                        for (std::size_t i = 0; i < run; ++i)
                            to[i] += from[i] / 9.0f;
                    }
            }
    }
    // std::lround and std::ceil of what's never negative here, without the
    // libm calls: a float plus a half is exact in a double, and a float
    // below 2^23 truncates exactly.
    auto roundUp = [](float v) {
        const auto t = static_cast<float>(static_cast<int>(v));
        return t < v ? t + 1.0f : t;
    };
    auto nearest = [](float v) { return static_cast<long>(static_cast<double>(v) + 0.5); };
    bool changed = false;
    for (int y = outLow.y; y <= outHigh.y; ++y)
        for (int z = outLow.z; z <= outHigh.z; ++z)
            for (int x = outLow.x; x <= outHigh.x; ++x) {
                std::uint8_t texel[4]{0, 0, 0, 13};
                if (!flat.empty()) {
                    float c[3]{};
                    for (int dy = 0; dy < 3; ++dy) {
                        const std::size_t from
                            = ((static_cast<std::size_t>(y - outLow.y + dy) * static_cast<std::size_t>(oz)
                                + static_cast<std::size_t>(z - outLow.z))
                                   * static_cast<std::size_t>(ox)
                               + static_cast<std::size_t>(x - outLow.x))
                              * 3;
                        for (int k = 0; k < 3; ++k)
                            c[k] += flat[from + static_cast<std::size_t>(k)] / 3.0f;
                    }
                    for (float& v : c)
                        v = std::clamp(v, 0.0f, lightMapScale);
                    const float alpha
                        = roundUp(std::max(std::max({c[0], c[1], c[2]}) * 255.0f / lightMapScale, 12.75f));
                    for (int k = 0; k < 3; ++k)
                        texel[k] = static_cast<std::uint8_t>(
                            std::clamp(nearest(c[k] * (255.0f * 255.0f / lightMapScale) / alpha), 0l, 255l));
                    texel[3] = static_cast<std::uint8_t>(std::min(alpha, 255.0f));
                }
                std::uint8_t* out
                    = chunk.light.data()
                      + (static_cast<std::size_t>(y - base.y - 1) * layerVoxels
                         + static_cast<std::size_t>(z - base.z - 1) * chunkX + static_cast<std::size_t>(x - base.x - 1))
                            * 4;
                changed = changed || std::memcmp(out, texel, 4) != 0;
                std::memcpy(out, texel, 4);
            }
    chunk.changed = chunk.changed || changed;
}

std::vector<LightGrid::ChunkTexels> LightGrid::takeUploads()
{
    std::vector<ChunkTexels> uploads;
    if (working_)
        return uploads;
    for (Chunk& chunk : slots_) {
        if (!chunk.changed)
            continue;
        chunk.changed = false;
        uploads.push_back({chunk.coord, chunk.texels, chunk.light});
    }
    return uploads;
}

} // namespace lunar::render
