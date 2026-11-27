#include <lunar/render/MapMesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>
#include <utility>

namespace lunar::render {

namespace {

constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

std::uint8_t unorm8(float v)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

Color3 times(Color3 a, Color3 b)
{
    return {a.r * b.r, a.g * b.g, a.b * b.b};
}

Color3 mix(Color3 a, Color3 b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Half the size of the world-space box around an object-space box of half
// size `half` that m moves.
Vector3 boxHalfExtent(const Mat4& m, Vector3 half)
{
    auto row = [&](std::size_t r) {
        return std::abs(m.m[0][r]) * half.x + std::abs(m.m[1][r]) * half.y + std::abs(m.m[2][r]) * half.z;
    };
    return {row(0), row(1), row(2)};
}

void grow(Bounds& bounds, bool& any, Vector3 p)
{
    if (!any) {
        bounds = {p, p};
        any = true;
        return;
    }
    bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
    bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
}

// The unit cube's corners and faces, wound counter-clockwise from outside.
constexpr std::array<Vector3, 8> cubeCorners{{
    {-0.5f, -0.5f, -0.5f},
    {0.5f, -0.5f, -0.5f},
    {0.5f, 0.5f, -0.5f},
    {-0.5f, 0.5f, -0.5f},
    {-0.5f, -0.5f, 0.5f},
    {0.5f, -0.5f, 0.5f},
    {0.5f, 0.5f, 0.5f},
    {-0.5f, 0.5f, 0.5f},
}};
constexpr std::array<std::uint32_t, 36> cubeIndices{
    0, 2, 1, 0, 3, 2, // -Z
    4, 5, 6, 4, 6, 7, // +Z
    0, 4, 7, 0, 7, 3, // -X
    1, 2, 6, 1, 6, 5, // +X
    3, 7, 6, 3, 6, 2, // +Y
    0, 1, 5, 0, 5, 4, // -Y
};

// The index range of a mesh's coarsest level of detail.
std::span<const std::uint32_t> coarsest(const MeshData& mesh)
{
    if (mesh.lodEnds.empty())
        return mesh.indices;
    const std::size_t end = std::min<std::size_t>(mesh.lodEnds.back(), mesh.lodIndices.size());
    const std::size_t begin = mesh.lodEnds.size() >= 2 ? std::min<std::size_t>(mesh.lodEnds.end()[-2], end) : 0;
    if (begin == end)
        return mesh.indices;
    return std::span(mesh.lodIndices).subspan(begin, end - begin);
}

std::uint64_t tileKey(Vector3 point, float tileSize)
{
    const int x = static_cast<int>(std::floor(point.x / tileSize));
    const int z = static_cast<int>(std::floor(point.z / tileSize));
    return (std::uint64_t{std::bit_cast<std::uint32_t>(x)} << 32) | std::bit_cast<std::uint32_t>(z);
}

MapInstance instanceOf(const Mat4& model, Color3 color)
{
    MapInstance out;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 4; ++c)
            out.rows[r][c] = model.m[c][r];
    out.color = packMapColor(color);
    return out;
}

} // namespace

namespace {

// How light a colour looks, sRGB weights on the encoded values (close enough here).
float lightness(Color3 c)
{
    return (0.2126f * c.r) + (0.7152f * c.g) + (0.0722f * c.b);
}

// A grey of value v with a touch of green, and `hue` of the colour's own tint.
Color3 tactical(Color3 color, float v, float hue)
{
    const float l = std::max(lightness(color), 1e-3f);
    const Color3 grey{v * 0.93f, v, v * 0.94f};
    const Color3 tinted{std::min(color.r / l * v, 1.0f), std::min(color.g / l * v, 1.0f),
                        std::min(color.b / l * v, 1.0f)};
    return mix(grey, tinted, hue);
}

} // namespace

Color3 mapPartColor(Color3 color)
{
    return tactical(color, 0.2f + (0.36f * lightness(color)), 0.12f);
}

Color3 mapTerrainColor(Color3 color)
{
    return tactical(color, 0.1f + (0.16f * lightness(color)), 0.18f);
}

std::uint32_t packMapColor(Color3 color, MapKind kind)
{
    return std::uint32_t{unorm8(color.r)} | std::uint32_t{unorm8(color.g)} << 8 | std::uint32_t{unorm8(color.b)} << 16
           | std::uint32_t{static_cast<std::uint8_t>(kind)} << 24;
}

Color3 unpackMapColor(std::uint32_t color)
{
    return {static_cast<float>(color & 0xFF) / 255.0f, static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>((color >> 16) & 0xFF) / 255.0f};
}

MapMeshBuilder::MapMeshBuilder(MapMeshOptions options) : options_(options)
{
    Shape cube;
    for (const Vector3& p : cubeCorners)
        cube.vertices.push_back({{p.x, p.y, p.z}, 0xFFFFFFFFu});
    cube.indices.assign(cubeIndices.begin(), cubeIndices.end());
    cube.edges = edges(cubeCorners, cubeIndices, 30.0f);
    cube.bounds = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    shapes_.push_back(std::move(cube));
}

MapMeshBuilder::Clustered MapMeshBuilder::cluster(std::span<const Vector3> positions, std::span<const Color3> colors,
                                                  std::span<const std::uint32_t> indices, float cell, bool worldGrid)
{
    Clustered out;
    if (positions.empty() || !(cell > 0.0f))
        return out;
    // Cell coordinates are kept in 21 bits each: from the lowest corner, or
    // around the origin.
    Vector3 low = positions.front();
    for (const Vector3& p : positions)
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
    const float bias = worldGrid ? 1048576.0f : 0.0f;
    if (worldGrid)
        low = {};
    // Each corner's cell, numbered in the order first seen.
    struct Sum {
        Vector3 position;
        Color3 color{0.0f, 0.0f, 0.0f};
        float count = 0.0f;
    };
    std::unordered_map<std::uint64_t, std::uint32_t> cells;
    std::vector<Sum> sums;
    std::vector<std::uint32_t> clusterOf(positions.size(), none);
    auto clusterFor = [&](std::uint32_t index) {
        std::uint32_t& to = clusterOf[index];
        if (to != none)
            return to;
        const Vector3 p = positions[index];
        auto coord = [&](float v, float from) {
            return static_cast<std::uint64_t>(std::clamp(std::floor((v - from) / cell) + bias, 0.0f, 2097151.0f));
        };
        const std::uint64_t key = coord(p.x, low.x) | coord(p.y, low.y) << 21 | coord(p.z, low.z) << 42;
        auto [it, added] = cells.try_emplace(key, static_cast<std::uint32_t>(sums.size()));
        if (added)
            sums.emplace_back();
        Sum& sum = sums[it->second];
        sum.position = sum.position + p;
        const Color3 c = colors.empty() ? Color3{1.0f, 1.0f, 1.0f} : colors[index];
        sum.color = {sum.color.r + c.r, sum.color.g + c.g, sum.color.b + c.b};
        sum.count += 1.0f;
        to = it->second;
        return to;
    };
    std::unordered_set<std::uint64_t> seen;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() || indices[t + 2] >= positions.size())
            continue;
        const std::uint32_t a = clusterFor(indices[t]);
        const std::uint32_t b = clusterFor(indices[t + 1]);
        const std::uint32_t c = clusterFor(indices[t + 2]);
        if (a == b || b == c || c == a)
            continue;
        // Either side is the same triangle to the map, which draws both.
        std::array<std::uint32_t, 3> sorted{a, b, c};
        std::ranges::sort(sorted);
        const std::uint64_t key
            = std::uint64_t{sorted[0]} | std::uint64_t{sorted[1]} << 21 | std::uint64_t{sorted[2]} << 42;
        if (!seen.insert(key).second)
            continue;
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    // Only the clusters triangles use, numbered afresh.
    std::vector<std::uint32_t> remap(sums.size(), none);
    for (std::uint32_t& index : out.indices) {
        std::uint32_t& to = remap[index];
        if (to == none) {
            to = static_cast<std::uint32_t>(out.positions.size());
            const Sum& sum = sums[index];
            Vector3 p = sum.position * (1.0f / sum.count);
            if (worldGrid) {
                // On the world grid, across the ground at the cell's middle,
                // so pieces clustered apart meet exactly.
                p.x = (std::floor(p.x / cell) + 0.5f) * cell;
                p.z = (std::floor(p.z / cell) + 0.5f) * cell;
            }
            out.positions.push_back(p);
            out.colors.push_back({sum.color.r / sum.count, sum.color.g / sum.count, sum.color.b / sum.count});
        }
        index = to;
    }
    return out;
}

std::vector<std::uint32_t> MapMeshBuilder::edges(std::span<const Vector3> positions,
                                                 std::span<const std::uint32_t> indices,
                                                 std::optional<float> foldDegrees)
{
    struct Edge {
        Vector3 normal; // the first face's
        bool keep = false;
        int faces = 0;
    };
    std::unordered_map<std::uint64_t, Edge> found;
    std::vector<std::uint64_t> order;
    const float cosFold = foldDegrees ? std::cos(*foldDegrees * std::numbers::pi_v<float> / 180.0f) : 0.0f;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t corner[3] = {indices[t], indices[t + 1], indices[t + 2]};
        if (corner[0] >= positions.size() || corner[1] >= positions.size() || corner[2] >= positions.size())
            continue;
        Vector3 normal
            = (positions[corner[1]] - positions[corner[0]]).cross(positions[corner[2]] - positions[corner[0]]);
        const float length = normal.magnitude();
        normal = length > 0.0f ? normal * (1.0f / length) : Vector3{0, 1, 0};
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = std::min(corner[e], corner[(e + 1) % 3]);
            const std::uint32_t b = std::max(corner[e], corner[(e + 1) % 3]);
            const std::uint64_t key = std::uint64_t{a} << 32 | b;
            auto [it, added] = found.try_emplace(key);
            Edge& edge = it->second;
            if (added) {
                edge.normal = normal;
                order.push_back(key);
            } else if (std::abs(edge.normal.dot(normal)) < cosFold) {
                // Both sides are drawn, so a face and its flip are the same.
                edge.keep = true;
            }
            ++edge.faces;
        }
    }
    std::vector<std::uint32_t> out;
    for (const std::uint64_t key : order) {
        const Edge& edge = found[key];
        if (foldDegrees && !edge.keep && edge.faces >= 2)
            continue;
        out.push_back(static_cast<std::uint32_t>(key >> 32));
        out.push_back(static_cast<std::uint32_t>(key & 0xFFFFFFFFu));
    }
    return out;
}

MapMeshBuilder::Source& MapMeshBuilder::source(const MeshPtr& mesh)
{
    auto [it, added] = sources_.try_emplace(mesh.get());
    Source& found = it->second;
    if (added) {
        found.mesh = mesh;
        bool any = false;
        for (const std::uint32_t index : coarsest(*mesh))
            if (index < mesh->vertices.size())
                grow(found.bounds, any, mesh->vertices[index].position);
        found.empty = !any;
    }
    return found;
}

Color3 MapMeshBuilder::textureMean(const DrawItem& item)
{
    const lunar::Image* image = item.texture.get();
    auto it = means_.find(image);
    if (it == means_.end()) {
        // A sparse grid of samples is plenty for one colour.
        Mean mean{.image = item.texture, .rgb = {1.0f, 1.0f, 1.0f}};
        if (!image->empty() && image->rgba.size() >= image->byteSize()) {
            constexpr std::uint32_t grid = 32;
            double r = 0, g = 0, b = 0, a = 0, pr = 0, pg = 0, pb = 0;
            const std::uint32_t nx = std::min(grid, image->width);
            const std::uint32_t ny = std::min(grid, image->height);
            for (std::uint32_t y = 0; y < ny; ++y)
                for (std::uint32_t x = 0; x < nx; ++x) {
                    const std::size_t px
                        = (std::size_t{x} * image->width / nx) + (std::size_t{y} * image->height / ny) * image->width;
                    const std::uint8_t* t = &image->rgba[px * 4];
                    const double alpha = t[3] / 255.0;
                    r += t[0] / 255.0;
                    g += t[1] / 255.0;
                    b += t[2] / 255.0;
                    a += alpha;
                    pr += t[0] / 255.0 * alpha;
                    pg += t[1] / 255.0 * alpha;
                    pb += t[2] / 255.0 * alpha;
                }
            const double n = static_cast<double>(nx) * ny;
            mean.alpha = static_cast<float>(a / n);
            if (item.alphaMode == TextureAlpha::Overlay) {
                mean.rgb = {static_cast<float>(pr / n), static_cast<float>(pg / n), static_cast<float>(pb / n)};
            } else if (item.alphaMode == TextureAlpha::Transparency && a > 0.0) {
                // Only what shows: clear texels are often black.
                mean.rgb = {static_cast<float>(pr / a), static_cast<float>(pg / a), static_cast<float>(pb / a)};
            } else {
                mean.rgb = {static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)};
            }
        }
        it = means_.emplace(image, std::move(mean)).first;
    }
    const Mean& mean = it->second;
    switch (item.alphaMode) {
    case TextureAlpha::Overlay:
        // mix(colour, texel * tint, alpha), averaged: rgb is premultiplied.
        return {item.color.r * (1.0f - mean.alpha) + mean.rgb.r * item.textureColor.r,
                item.color.g * (1.0f - mean.alpha) + mean.rgb.g * item.textureColor.g,
                item.color.b * (1.0f - mean.alpha) + mean.rgb.b * item.textureColor.b};
    case TextureAlpha::TintMask:
        return times(mean.rgb, mix({1.0f, 1.0f, 1.0f}, item.textureColor, mean.alpha));
    case TextureAlpha::Transparency:
    case TextureAlpha::Opaque:
        break;
    }
    return times(mean.rgb, item.textureColor);
}

bool MapMeshBuilder::addItem(const DrawItem& item, std::weak_ptr<const Instance> owner)
{
    if (item.decal || item.terrain || item.terrainWater)
        return false;
    if (item.transparency > options_.maxTransparency) {
        ++stats_.clear;
        return false;
    }
    Source* mesh = item.mesh ? &source(item.mesh) : nullptr;
    if (mesh && mesh->empty)
        return false;
    const Bounds local = mesh ? mesh->bounds : Bounds{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const Vector3 half = boxHalfExtent(item.model, local.extent() * 0.5f);
    const float longest = 2.0f * std::max({half.x, half.y, half.z});
    if (longest < options_.minSize) {
        ++stats_.small;
        return false;
    }
    const Color3 color = mapPartColor(item.texture ? textureMean(item) : item.color);
    // Foliage: cards cut out of a mostly clear texture. Boiled down they
    // turn into a solid canopy hiding the ground, so a thin post (the lower
    // part of the mesh's height) stands for each instead. GUESS: the look is ours.
    if (mesh && item.texture && item.alphaMode == TextureAlpha::Transparency
        && means_.at(item.texture.get()).alpha < options_.foliageAlpha) {
        const Vector3 e = local.extent();
        const Vector3 base{local.center().x, local.min.y + (e.y * 0.3f), local.center().z};
        pending_.push_back({item.model * Mat4::translation(base) * Mat4::scale({e.x * 0.06f, e.y * 0.6f, e.z * 0.06f}),
                            color, nullptr, std::move(owner)});
        ++stats_.foliage;
        return true;
    }
    if (mesh)
        mesh->worldLongest = std::max(mesh->worldLongest, longest);
    pending_.push_back({item.model, color, item.mesh, std::move(owner)});
    ++stats_.parts;
    return true;
}

void MapMeshBuilder::addBox(const Mat4& model, Color3 color, std::weak_ptr<const Instance> owner)
{
    pending_.push_back({model, mapPartColor(color), nullptr, std::move(owner)});
}

void MapMeshBuilder::appendWorld(std::span<const Vector3> positions, std::span<const Color3> colors,
                                 std::span<const std::uint32_t> indices, MapKind kind)
{
    // World meshes are big: split them by triangle so the tiles stay small.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byTile;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Vector3 c
            = (positions[indices[t]] + positions[indices[t + 1]] + positions[indices[t + 2]]) * (1.0f / 3.0f);
        auto& list = byTile[tileKey(c, options_.tileSize)];
        list.insert(list.end(), indices.begin() + static_cast<std::ptrdiff_t>(t),
                    indices.begin() + static_cast<std::ptrdiff_t>(t + 3));
    }
    std::vector<std::uint32_t> remap(positions.size(), none);
    std::vector<std::uint32_t> touched;
    for (auto& [key, list] : byTile) {
        Tile& tile = tiles_[key];
        touched.clear();
        const auto first = static_cast<std::uint32_t>(tile.vertices.size());
        const std::size_t firstIndex = tile.indices.size();
        for (const std::uint32_t index : list) {
            std::uint32_t& to = remap[index];
            if (to == none) {
                to = static_cast<std::uint32_t>(tile.vertices.size());
                const Vector3 p = positions[index];
                tile.vertices.push_back({{p.x, p.y, p.z}, packMapColor(colors[index], kind)});
                grow(tile.bounds, tile.any, p);
                touched.push_back(index);
            }
            tile.indices.push_back(to);
        }
        // Terrain shows its triangles as a wireframe.
        if (kind == MapKind::Terrain) {
            std::vector<Vector3> local;
            local.reserve(tile.vertices.size() - first);
            for (std::size_t v = first; v < tile.vertices.size(); ++v)
                local.push_back(
                    {tile.vertices[v].position[0], tile.vertices[v].position[1], tile.vertices[v].position[2]});
            std::vector<std::uint32_t> piece(tile.indices.begin() + static_cast<std::ptrdiff_t>(firstIndex),
                                             tile.indices.end());
            for (std::uint32_t& index : piece)
                index -= first;
            for (const std::uint32_t index : edges(local, piece, std::nullopt))
                tile.edges.push_back(index + first);
        }
        for (const std::uint32_t index : touched)
            remap[index] = none;
    }
}

void MapMeshBuilder::addTerrain(const MeshData& solid, std::span<const std::uint8_t> palette,
                                std::span<const Color3> materialColors)
{
    if (solid.empty())
        return;
    std::vector<Vector3> positions(solid.vertices.size());
    std::vector<Color3> colors(solid.vertices.size(), Color3{0.5f, 0.5f, 0.5f});
    for (std::size_t i = 0; i < solid.vertices.size(); ++i) {
        const MeshVertex& v = solid.vertices[i];
        positions[i] = v.position;
        const auto heaviest
            = static_cast<std::size_t>(std::ranges::max_element(v.materialWeights) - v.materialWeights.begin());
        if (heaviest < palette.size() && palette[heaviest] < materialColors.size())
            colors[i] = materialColors[palette[heaviest]];
        colors[i] = mapTerrainColor(colors[i]);
    }
    appendWorld(positions, colors, solid.indices, MapKind::Terrain);
    stats_.terrain += solid.indices.size() / 3;
}

void MapMeshBuilder::addWater(const MeshData& water, float cellStuds)
{
    terrainCell_ = cellStuds;
    if (water.empty())
        return;
    std::vector<Vector3> positions(water.vertices.size());
    for (std::size_t i = 0; i < water.vertices.size(); ++i)
        positions[i] = water.vertices[i].position;
    const Clustered flat = cluster(positions, {}, water.indices, cellStuds * options_.waterCells, true);
    const std::vector<Color3> colors(flat.positions.size(), options_.water);
    appendWorld(flat.positions, colors, flat.indices, MapKind::Water);
    stats_.water += flat.indices.size() / 3;
}

MapMesh MapMeshBuilder::finish()
{
    MapMesh out;
    bool any = false;

    // World triangles, tile by tile in a fixed order.
    std::vector<std::uint64_t> keys;
    for (const auto& [key, tile] : tiles_)
        if (!tile.indices.empty())
            keys.push_back(key);
    std::ranges::sort(keys);
    for (const std::uint64_t key : keys) {
        Tile& tile = tiles_[key];
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        MapTile& range = out.tiles.emplace_back();
        range.bounds = tile.bounds;
        range.firstIndex = static_cast<std::uint32_t>(out.indices.size());
        range.indexCount = static_cast<std::uint32_t>(tile.indices.size());
        out.vertices.insert(out.vertices.end(), tile.vertices.begin(), tile.vertices.end());
        for (const std::uint32_t index : tile.indices)
            out.indices.push_back(base + index);
        range.firstEdge = static_cast<std::uint32_t>(out.indices.size());
        range.edgeCount = static_cast<std::uint32_t>(tile.edges.size());
        for (const std::uint32_t index : tile.edges)
            out.indices.push_back(base + index);
        grow(out.bounds, any, tile.bounds.min);
        grow(out.bounds, any, tile.bounds.max);
        stats_.triangles += tile.indices.size() / 3;
    }

    // Each mesh's shape, clustered for its biggest instance (again only
    // when that needs a finer grid than last time).
    for (auto& [key, source] : sources_) {
        if (source.worldLongest <= 0.0f || source.box)
            continue;
        const float longestLocal
            = std::max({source.bounds.extent().x, source.bounds.extent().y, source.bounds.extent().z});
        const int cells = std::clamp(static_cast<int>(std::ceil(source.worldLongest / options_.cellStuds)),
                                     options_.minCells, options_.maxCells);
        if (cells <= source.cells)
            continue;
        const MeshData& mesh = *source.mesh;
        std::vector<Vector3> positions(mesh.vertices.size());
        std::vector<Color3> colors(mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            positions[i] = mesh.vertices[i].position;
            colors[i] = mesh.vertices[i].color;
        }
        const Clustered clustered
            = cluster(positions, colors, coarsest(mesh), longestLocal / static_cast<float>(cells));
        source.cells = cells;
        if (clustered.indices.empty()) {
            source.box = true; // too thin to keep a triangle: its box will do
            continue;
        }
        Shape shape;
        bool grown = false;
        for (std::size_t i = 0; i < clustered.positions.size(); ++i) {
            const Vector3 p = clustered.positions[i];
            // Vertex colours keep only their lightness; the instance brings the palette.
            const float l = std::clamp(lightness(clustered.colors[i]), 0.0f, 1.0f);
            shape.vertices.push_back({{p.x, p.y, p.z}, packMapColor({l, l, l})});
            grow(shape.bounds, grown, p);
        }
        shape.indices = clustered.indices;
        shape.edges = edges(clustered.positions, clustered.indices, 30.0f);
        source.shape = static_cast<std::uint32_t>(shapes_.size());
        shapes_.push_back(std::move(shape));
        ++stats_.meshes;
    }

    // Instances by shape, then tile.
    struct Placed {
        std::uint32_t shape;
        std::uint64_t tile;
        MapInstance instance;
        Bounds bounds;
        std::weak_ptr<const Instance> owner;
    };
    std::vector<Placed> placed;
    placed.reserve(pending_.size());
    for (const Pending& p : pending_) {
        Mat4 model = p.model;
        std::uint32_t shape = 0;
        Bounds local{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        if (p.mesh) {
            const Source& source = sources_.at(p.mesh.get());
            if (source.box) {
                model = model * Mat4::translation(source.bounds.center()) * Mat4::scale(source.bounds.extent());
                ++stats_.boxes;
            } else {
                shape = source.shape;
                local = source.bounds;
            }
        }
        const Vector3 center = model.transformPoint(local.center());
        const Vector3 half = boxHalfExtent(model, local.extent() * 0.5f);
        placed.push_back({shape,
                          tileKey(center, options_.tileSize),
                          instanceOf(model, p.color),
                          {center - half, center + half},
                          p.owner});
    }
    std::ranges::sort(placed, [](const Placed& a, const Placed& b) {
        return a.shape != b.shape ? a.shape < b.shape : a.tile < b.tile;
    });

    // Only the shapes in use go into the mesh, renumbered.
    std::vector<std::uint32_t> shapeIndex(shapes_.size(), none);
    out.instances.push_back(MapInstance{}); // the identity, for the world triangles
    out.owners.emplace_back();
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const Placed& p = placed[i];
        if (shapeIndex[p.shape] == none) {
            const Shape& shape = shapes_[p.shape];
            shapeIndex[p.shape] = static_cast<std::uint32_t>(out.shapes.size());
            MapShape& range = out.shapes.emplace_back();
            range.firstIndex = static_cast<std::uint32_t>(out.indices.size());
            range.indexCount = static_cast<std::uint32_t>(shape.indices.size());
            range.vertexOffset = static_cast<std::int32_t>(out.vertices.size());
            range.bounds = shape.bounds;
            out.indices.insert(out.indices.end(), shape.indices.begin(), shape.indices.end());
            range.firstEdge = static_cast<std::uint32_t>(out.indices.size());
            range.edgeCount = static_cast<std::uint32_t>(shape.edges.size());
            out.indices.insert(out.indices.end(), shape.edges.begin(), shape.edges.end());
            out.vertices.insert(out.vertices.end(), shape.vertices.begin(), shape.vertices.end());
        }
        const std::uint32_t shape = shapeIndex[p.shape];
        if (i == 0 || p.shape != placed[i - 1].shape || p.tile != placed[i - 1].tile)
            out.batches.push_back({shape, static_cast<std::uint32_t>(out.instances.size()), 0, p.bounds});
        MapBatch& batch = out.batches.back();
        ++batch.instanceCount;
        bool grown = true;
        grow(batch.bounds, grown, p.bounds.min);
        grow(batch.bounds, grown, p.bounds.max);
        out.instances.push_back(p.instance);
        out.owners.push_back(p.owner);
        grow(out.bounds, any, p.bounds.min);
        grow(out.bounds, any, p.bounds.max);
        stats_.triangles += out.shapes[shape].indexCount / 3;
    }

    out.stats = stats_;
    out.terrainCell = terrainCell_;
    pending_.clear();
    tiles_.clear();
    stats_ = {};
    for (auto& [key, source] : sources_)
        source.worldLongest = 0.0f;
    return out;
}

} // namespace lunar::render
