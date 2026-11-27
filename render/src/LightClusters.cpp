#include <lunar/render/LightClusters.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lunar::render {

std::uint32_t LightClusters::sliceOf(float depth)
{
    const float s = std::log2(std::max(depth, 1e-4f) * sliceDepth) * sliceScale + 1.0f;
    return static_cast<std::uint32_t>(std::clamp(s, 0.0f, static_cast<float>(slices - 1)));
}

float LightClusters::sliceStart(std::uint32_t slice)
{
    return slice == 0 ? 0.0f : std::exp2((static_cast<float>(slice) - 1.0f) / sliceScale) / sliceDepth;
}

void LightClusters::build(std::span<const LocalLight> lights, const Camera& camera, std::uint32_t width,
                          std::uint32_t height)
{
    tilesX = std::max((width + tilePixels - 1) / tilePixels, 1u);
    tilesY = std::max((height + tilePixels - 1) / tilePixels, 1u);
    const std::size_t count = std::size_t{tilesX} * tilesY * slices;
    cells.assign(count, 0);
    indices.clear();
    if (lights.empty() || width == 0 || height == 0)
        return;

    const Mat4 view = camera.view();
    const float tanHalf = std::tan(camera.fieldOfView * std::numbers::pi_v<float> / 360.0f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const auto w = static_cast<float>(width), h = static_cast<float>(height);
    // Each light's tiles in each slice it reaches: the part of its sphere
    // between the slice's depths is no wider than its widest cross-section
    // there, and the corners of the view-space box around that bound its
    // pixels. A light whose sphere is one box over every slice it touches
    // reaches far fewer pixels at its near and far ends than in its middle.
    struct Box {
        std::uint32_t light, z, x0, x1, y0, y1;
    };
    std::vector<Box> boxes;
    boxes.reserve(lights.size() * 4);
    const auto tile = [](float p, std::uint32_t tiles) {
        return std::min(static_cast<std::uint32_t>(std::max(p, 0.0f)) / tilePixels, tiles - 1);
    };
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const LocalLight& light = lights[i];
        const Vector3 c = view.transformPoint(light.position);
        const float r = light.reach();
        const float depth = -c.z;
        if (depth + r <= camera.nearPlane)
            continue; // behind the camera
        const std::uint32_t z0 = sliceOf(std::max(depth - r, camera.nearPlane)), z1 = sliceOf(depth + r);
        for (std::uint32_t z = z0; z <= z1; ++z) {
            const float lo = std::max({sliceStart(z), camera.nearPlane, depth - r});
            const float hi = z + 1 < slices ? std::min(sliceStart(z + 1), depth + r) : depth + r;
            if (lo > hi)
                continue;
            const float dz = std::clamp(depth, lo, hi) - depth;
            const float across = std::sqrt(std::max(r * r - dz * dz, 0.0f));
            float x0 = w, x1 = 0.0f, y0 = h, y1 = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const float px = c.x + ((k & 1) ? across : -across);
                const float py = c.y + ((k & 2) ? across : -across);
                const float pd = (k & 4) ? hi : lo;
                const float sx = (0.5f + 0.5f * px / (pd * tanHalf * aspect)) * w;
                const float sy = (0.5f - 0.5f * py / (pd * tanHalf)) * h;
                x0 = std::min(x0, sx);
                x1 = std::max(x1, sx);
                y0 = std::min(y0, sy);
                y1 = std::max(y1, sy);
            }
            if (x1 < 0.0f || y1 < 0.0f || x0 >= w || y0 >= h)
                continue; // off screen at these depths
            boxes.push_back({static_cast<std::uint32_t>(i), z, tile(x0, tilesX), tile(x1, tilesX), tile(y0, tilesY),
                             tile(y1, tilesY)});
        }
    }

    // Counted first, then laid out, then filled.
    std::vector<std::uint32_t> counts(count, 0);
    auto cellOf = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (std::size_t{z} * tilesY + y) * tilesX + x;
    };
    for (const Box& b : boxes)
        for (std::uint32_t y = b.y0; y <= b.y1; ++y)
            for (std::uint32_t x = b.x0; x <= b.x1; ++x)
                ++counts[cellOf(x, y, b.z)];
    std::uint32_t next = 0;
    for (std::size_t c = 0; c < count; ++c) {
        const std::uint32_t n = std::min({counts[c], maxPerCluster, maxIndices - next});
        cells[c] = n | (next << 12);
        next += n;
        counts[c] = 0; // how many are in so far
    }
    indices.assign(next, 0);
    for (const Box& b : boxes)
        for (std::uint32_t y = b.y0; y <= b.y1; ++y)
            for (std::uint32_t x = b.x0; x <= b.x1; ++x) {
                const std::size_t c = cellOf(x, y, b.z);
                if (counts[c] < (cells[c] & 4095u))
                    indices[(cells[c] >> 12) + counts[c]++] = b.light;
            }
}

std::span<const std::uint32_t> LightClusters::at(std::uint32_t x, std::uint32_t y, float depth) const
{
    if (tilesX == 0)
        return {};
    const std::uint32_t tx = std::min(x / tilePixels, tilesX - 1), ty = std::min(y / tilePixels, tilesY - 1);
    const std::uint32_t cell = cells[(std::size_t{sliceOf(depth)} * tilesY + ty) * tilesX + tx];
    return std::span(indices).subspan(cell >> 12, cell & 4095u);
}

} // namespace lunar::render
