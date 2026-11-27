#include "TerrainExport.hpp"

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/terrain/TerrainMesher.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace {

using lunar::Color3;
using lunar::MeshData;
using lunar::Vector3;
using lunar::terrain::TerrainMeshChunk;

bool writeObj(const std::filesystem::path& path, const std::vector<TerrainMeshChunk>& chunks)
{
    std::ofstream out(path, std::ios::binary);
    std::string buffer;
    std::size_t base = 1;
    auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };
    for (bool water : {false, true}) {
        std::format_to(std::back_inserter(buffer), "o terrain_{}\n", water ? "water" : "solid");
        for (const TerrainMeshChunk& chunk : chunks) {
            const MeshData& mesh = water ? chunk.water : chunk.solid;
            for (const lunar::MeshVertex& v : mesh.vertices)
                std::format_to(std::back_inserter(buffer), "v {} {} {} {:.3f} {:.3f} {:.3f}\nvn {:.4f} {:.4f} {:.4f}\n",
                               v.position.x, v.position.y, v.position.z, v.color.r, v.color.g, v.color.b, v.normal.x,
                               v.normal.y, v.normal.z);
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                const std::size_t a = base + mesh.indices[i], b = base + mesh.indices[i + 1],
                                  c = base + mesh.indices[i + 2];
                std::format_to(std::back_inserter(buffer), "f {}//{} {}//{} {}//{}\n", a, a, b, b, c, c);
            }
            base += mesh.vertices.size();
            if (buffer.size() > (std::size_t{1} << 22))
                flush();
        }
    }
    flush();
    return static_cast<bool>(out);
}

// An orthographic view: screen right and up, and the direction towards the viewer.
struct View {
    Vector3 right;
    Vector3 up;
    Vector3 toViewer;
};

struct Layer {
    std::vector<float> depth;
    std::vector<Color3> color;
};

// Draws the meshes into a width x height region of the image with a depth
// buffer, the water as a see-through layer over the ground.
void rasterize(const std::vector<TerrainMeshChunk>& chunks, const View& view, lunar::render::PixelImage& image,
               std::uint32_t left, std::uint32_t width)
{
    constexpr float inf = std::numeric_limits<float>::infinity();
    float minR = inf, maxR = -inf, minU = inf, maxU = -inf;
    for (const TerrainMeshChunk& chunk : chunks)
        for (const MeshData* mesh : {&chunk.solid, &chunk.water})
            for (const lunar::MeshVertex& v : mesh->vertices) {
                minR = std::min(minR, v.position.dot(view.right));
                maxR = std::max(maxR, v.position.dot(view.right));
                minU = std::min(minU, v.position.dot(view.up));
                maxU = std::max(maxU, v.position.dot(view.up));
            }
    if (minR > maxR)
        return;
    const std::uint32_t height = image.height;
    const float scale = 0.96f
                        * std::min(static_cast<float>(width) / (maxR - minR),
                                   static_cast<float>(height) / std::max(maxU - minU, 1.0f));
    const float offsetX = (static_cast<float>(width) - (maxR - minR) * scale) / 2.0f;
    const float offsetY = (static_cast<float>(height) - (maxU - minU) * scale) / 2.0f;

    const std::size_t pixels = std::size_t{width} * height;
    Layer solid{std::vector<float>(pixels, -inf), std::vector<Color3>(pixels, Color3{0.11f, 0.12f, 0.14f})};
    Layer water{std::vector<float>(pixels, -inf), std::vector<Color3>(pixels)};
    const Vector3 light = Vector3{-0.5f, 0.8f, -0.35f}.unit();

    struct Projected {
        float x, y, depth;
    };
    auto project = [&](Vector3 p) {
        return Projected{offsetX + (p.dot(view.right) - minR) * scale, offsetY + (maxU - p.dot(view.up)) * scale,
                         p.dot(view.toViewer)};
    };
    auto draw = [&](const MeshData& mesh, Layer& layer, bool isWater) {
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const std::array<const lunar::MeshVertex*, 3> v{&mesh.vertices[mesh.indices[i]],
                                                            &mesh.vertices[mesh.indices[i + 1]],
                                                            &mesh.vertices[mesh.indices[i + 2]]};
            const std::array<Projected, 3> p{project(v[0]->position), project(v[1]->position), project(v[2]->position)};
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::abs(area) < 1e-9f)
                continue;
            const auto x0 = static_cast<int>(std::max(0.0f, std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
            const auto x1 = static_cast<int>(
                std::min(static_cast<float>(width) - 1.0f, std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
            const auto y0 = static_cast<int>(std::max(0.0f, std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
            const auto y1 = static_cast<int>(
                std::min(static_cast<float>(height) - 1.0f, std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    auto edge = [&](const Projected& a, const Projected& b) {
                        return ((b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y)) / area;
                    };
                    const float w0 = edge(p[1], p[2]), w1 = edge(p[2], p[0]), w2 = edge(p[0], p[1]);
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                        continue;
                    const float depth = w0 * p[0].depth + w1 * p[1].depth + w2 * p[2].depth;
                    const std::size_t at = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
                    if (depth <= layer.depth[at] || (isWater && depth <= solid.depth[at]))
                        continue;
                    layer.depth[at] = depth;
                    const Vector3 n = v[0]->normal * w0 + v[1]->normal * w1 + v[2]->normal * w2;
                    const float shade = 0.3f + 0.7f * std::max(0.0f, n.dot(light) / std::max(n.magnitude(), 1e-6f));
                    const Color3 c{w0 * v[0]->color.r + w1 * v[1]->color.r + w2 * v[2]->color.r,
                                   w0 * v[0]->color.g + w1 * v[1]->color.g + w2 * v[2]->color.g,
                                   w0 * v[0]->color.b + w1 * v[1]->color.b + w2 * v[2]->color.b};
                    layer.color[at] = {c.r * shade, c.g * shade, c.b * shade};
                }
        }
    };
    for (const TerrainMeshChunk& chunk : chunks)
        draw(chunk.solid, solid, false);
    for (const TerrainMeshChunk& chunk : chunks)
        draw(chunk.water, water, true);

    auto byte = [](float f) { return static_cast<std::uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = std::size_t{y} * width + x;
            Color3 c = solid.color[at];
            if (water.depth[at] > solid.depth[at]) {
                constexpr float alpha = 0.6f;
                const Color3 w = water.color[at];
                c = {c.r + (w.r - c.r) * alpha, c.g + (w.g - c.g) * alpha, c.b + (w.b - c.b) * alpha};
            }
            image.pixels[std::size_t{y} * image.width + left + x] = {byte(c.r), byte(c.g), byte(c.b), 255};
        }
}

bool writePreview(const std::filesystem::path& path, const std::vector<TerrainMeshChunk>& chunks)
{
    constexpr std::uint32_t half = 1200;
    lunar::render::PixelImage image{half * 2, half, std::vector<lunar::render::Rgba>(std::size_t{half} * 2 * half)};
    const float elevation = 35.0f * std::numbers::pi_v<float> / 180.0f;
    const View top{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}};
    // Looking north from the south, 35 degrees down.
    const View tilted{{1.0f, 0.0f, 0.0f},
                      {0.0f, std::cos(elevation), -std::sin(elevation)},
                      {0.0f, std::sin(elevation), std::cos(elevation)}};
    std::jthread first([&] { rasterize(chunks, top, image, 0, half); });
    rasterize(chunks, tilted, image, half, half);
    first.join();
    const std::string encoded = lunar::render::encodePng(image);
    std::ofstream out(path, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    return static_cast<bool>(out);
}

double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int exportTerrain(const std::filesystem::path& placePath, const std::filesystem::path& objPath,
                  const std::optional<std::filesystem::path>& previewPath)
{
    auto start = std::chrono::steady_clock::now();
    auto bytes = lunar::readFile(placePath);
    if (!bytes) {
        std::println(stderr, "{}", bytes.error());
        return 1;
    }
    auto place = lunar::loadPlace(*bytes);
    if (!place) {
        std::println(stderr, "{}: {}", placePath.string(), place.error());
        return 1;
    }
    const lunar::Terrain* terrain = place->game->workspace()->terrain();
    if (!terrain) {
        std::println(stderr, "{}: no Terrain", placePath.string());
        return 1;
    }
    std::size_t runs = 0;
    for (const lunar::TerrainChunk& chunk : terrain->chunks())
        runs += chunk.runs.size();
    std::println("loaded in {:.2f} s: {} voxel chunks, {} runs", secondsSince(start), terrain->chunks().size(), runs);

    start = std::chrono::steady_clock::now();
    const auto chunks = lunar::terrain::meshTerrain(*terrain);
    const double meshSeconds = secondsSince(start);
    const auto stats = lunar::terrain::measure(chunks);
    std::println(
        "meshed in {:.2f} s on {} threads: {} mesh chunks, {} solid + {} water triangles, {} vertices, {:.1f} MiB",
        meshSeconds, std::thread::hardware_concurrency(), stats.chunks, stats.solidTriangles, stats.waterTriangles,
        stats.vertices, static_cast<double>(stats.bytes) / (1024.0 * 1024.0));

    start = std::chrono::steady_clock::now();
    if (!writeObj(objPath, chunks)) {
        std::println(stderr, "{}: can't write", objPath.string());
        return 1;
    }
    std::println("wrote {} in {:.2f} s", objPath.string(), secondsSince(start));
    if (previewPath) {
        if (!writePreview(*previewPath, chunks)) {
            std::println(stderr, "{}: can't write", previewPath->string());
            return 1;
        }
        std::println("wrote {}", previewPath->string());
    }
    return 0;
}
