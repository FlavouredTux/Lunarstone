#include <lunar/render/Lod.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace lunar::render {

namespace {

// The frame rate manager's per-quality-level settings (0.742, the table
// picked with 4 GB and 4 cores or more), row = level: the distance parts are
// drawn out to, before FInt FRMDrawDistanceScalePercent (100). Row 0 is what
// it starts from.
constexpr std::array<float, 22> drawDistances{
    100000.0f, 200.0f,  250.0f,  300.0f,  450.0f,  470.0f,  550.0f,  570.0f,  600.0f,    600.0f,    700.0f,
    1131.0f,   1600.0f, 2263.0f, 2263.0f, 3200.0f, 4525.0f, 6400.0f, 9051.0f, 100000.0f, 100000.0f, 100000.0f,
};

} // namespace

LodSelector::LodSelector(const Camera& camera, float viewportHeight, float pixelError) : eye_(camera.cframe.position)
{
    // The client multiplies by tan(35 degrees) and the projection's
    // 1 / tan(FieldOfView / 2). GUESS: that field (camera + 0xe4) is
    // 1 / tan(FieldOfView / 2); the screen-error rule only makes sense with it.
    constexpr float degrees = std::numbers::pi_v<float> / 180.0f;
    const float halfFov = std::clamp(camera.fieldOfView, 1.0f, 179.0f) * 0.5f * degrees;
    const float scale = std::tan(35.0f * degrees) / std::tan(halfFov);
    for (std::size_t i = 0; i < squared_.size(); ++i)
        squared_[i] = meshLodDistances[i] * scale * meshLodDistances[i] * scale;
    // One stud away, a pixel spans 2 tan(fov / 2) / height studs; the client
    // squares it (QuadradicErrorPercentPerPixel's helper: (2 * 1.2 / (h / tan))^2).
    const float perPixel = 2.0f * std::tan(halfFov) / std::max(viewportHeight, 1.0f);
    errorPerDistance_ = pixelError * perPixel * pixelError * perPixel;
}

std::uint32_t LodSelector::levelForError(float squaredDistance, float scale, std::span<const float> errors,
                                         Enum::RenderFidelity fidelity) const
{
    const auto levels = static_cast<std::uint32_t>(errors.size());
    if (levels <= 1 || fidelity == Enum::RenderFidelity::Precise)
        return 0;
    if (fidelity == Enum::RenderFidelity::Performance)
        return levels - 1;
    // The coarsest level that is still fine, looking from the coarsest down.
    const float allowed = errorPerDistance_ * squaredDistance / std::max(scale * scale, 1e-5f);
    for (std::uint32_t level = levels - 1; level > 0; --level)
        if (errors[level] * errors[level] <= allowed)
            return level;
    return 0;
}

std::uint32_t LodSelector::levelAt(float squaredDistance, std::uint32_t levels, Enum::RenderFidelity fidelity) const
{
    if (levels <= 1 || fidelity == Enum::RenderFidelity::Precise)
        return 0;
    // GUESS: Performance always draws the coarsest level there is.
    if (fidelity == Enum::RenderFidelity::Performance)
        return levels - 1;
    std::uint32_t level = 0;
    while (level < squared_.size() && squared_[level] <= squaredDistance)
        ++level;
    return std::min(level, levels - 1);
}

std::uint32_t LodSelector::level(Vector3 center, Vector3 halfExtent, std::uint32_t levels,
                                 Enum::RenderFidelity fidelity) const
{
    if (levels <= 1)
        return 0;
    return levelAt(squaredDistanceToBox(eye_, center, halfExtent), levels, fidelity);
}

float squaredDistanceToBox(Vector3 point, Vector3 center, Vector3 halfExtent)
{
    const float x = std::max(std::abs(point.x - center.x) - halfExtent.x, 0.0f);
    const float y = std::max(std::abs(point.y - center.y) - halfExtent.y, 0.0f);
    const float z = std::max(std::abs(point.z - center.z) - halfExtent.z, 0.0f);
    return x * x + y * y + z * z;
}

float qualityDrawDistance(int qualityLevel)
{
    if (qualityLevel < 1 || std::cmp_greater_equal(qualityLevel, drawDistances.size()))
        return drawDistances.back();
    return drawDistances[static_cast<std::size_t>(qualityLevel)];
}

} // namespace lunar::render
