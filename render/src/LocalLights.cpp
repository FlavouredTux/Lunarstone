#include <lunar/render/LocalLights.hpp>

#include <lunar/Classes.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lunar::render {

namespace {

// The client's Future falloff (its DefaultUnified*FS Metal shaders, which
// read LightRenderGPU): over the distance d past where the falloff starts,
// 110 / (d^2 + 110), cut off smoothly towards the range.
constexpr float pixelFalloff = 110.0f;
// The light grid's (sub_10486018a and its siblings): 1.35 / (d^2 + 1.35)
// in voxels, 4 studs each.
constexpr float gridFalloff = 1.35f;
constexpr float voxelStuds = 4.0f;

// A face's outward normal in the part's own space: Right, Top, Back are
// +X, +Y, +Z and Left, Bottom, Front -X, -Y, -Z.
Vector3 faceNormal(Enum::NormalId face)
{
    switch (face) {
    case Enum::NormalId::Right:
        return {1.0f, 0.0f, 0.0f};
    case Enum::NormalId::Top:
        return {0.0f, 1.0f, 0.0f};
    case Enum::NormalId::Back:
        return {0.0f, 0.0f, 1.0f};
    case Enum::NormalId::Left:
        return {-1.0f, 0.0f, 0.0f};
    case Enum::NormalId::Bottom:
        return {0.0f, -1.0f, 0.0f};
    case Enum::NormalId::Front:
        return {0.0f, 0.0f, -1.0f};
    }
    return {0.0f, 0.0f, -1.0f};
}

float component(Vector3 v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

Vector3 unitAxis(int axis)
{
    return {axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
}

// From a point to the nearest point of the light (its rectangle's, for a
// SurfaceLight).
Vector3 towards(const LocalLight& light, Vector3 point)
{
    const Vector3 away = point - light.position;
    const Vector3 nearest = light.position + light.axisU * std::clamp(away.dot(light.axisU), -light.halfU, light.halfU)
                            + light.axisV * std::clamp(away.dot(light.axisV), -light.halfV, light.halfV);
    return nearest - point;
}

// 1 at t = 0 down to 0 at t = 1, smoothly.
float window(float t)
{
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

} // namespace

float LocalLight::reach() const
{
    return range + std::sqrt(halfU * halfU + halfV * halfV);
}

Color3 LocalLight::pixelColor() const
{
    const float b = std::min(brightness, 10000.0f);
    auto channel = [b](float c) { return std::pow(std::max(c, 0.0f), 2.2f) * b; };
    return {channel(color.r), channel(color.g), channel(color.b)};
}

Color3 LocalLight::gridColor() const
{
    auto channel = [this](float c) {
        const float q = std::min(std::floor(std::max(c, 0.0f) * 256.0f), 255.0f) / 255.0f;
        return q * q * brightness;
    };
    return {channel(color.r), channel(color.g), channel(color.b)};
}

float LocalLight::pixelStrength(Vector3 point, Vector3 n) const
{
    const Vector3 toLight = towards(*this, point);
    const float distance = std::max(toLight.magnitude(), 1e-3f);
    const Vector3 l = toLight / distance;
    const float start = falloffStart * range;
    const float past = std::clamp(distance, start, range) - start;
    const float w = window(past / std::max(range - start, 1e-4f));
    float spot = 1.0f;
    if (cosHalfAngle != 0.0f) {
        const float c = std::clamp((direction.dot(-l) - cosHalfAngle) / (1.0f - cosHalfAngle), 0.0f, 1.0f);
        spot = c * c;
    }
    // A rectangle lights a little round its edges: N.L wrapped by its size
    // over the distance.
    const float wrap = std::clamp(std::sqrt(halfU * halfV) / distance + n.dot(l), 0.0f, 1.0f);
    return std::min(pixelFalloff * w * spot * wrap / (past * past + pixelFalloff), 1.0f);
}

float LocalLight::gridStrength(Vector3 point) const
{
    // In voxels, as the client works it out.
    const Vector3 d = -towards(*this, point) / voxelStuds;
    const float distance = std::max(d.magnitude(), std::sqrt(0.001f));
    const float r = range / voxelStuds;
    const float start = falloffStart * r;
    const float past = std::max(distance - start, 0.0f);
    const float w = window(std::min(past / std::max(r - start, 1e-4f), 1.0f));
    float cone = 1.0f;
    if (kind != Kind::Point) {
        // Linear across the cone, unlike the per-pixel square.
        const float c = std::min(direction.dot(d) / distance, 1.0f);
        cone = std::max((c - cosHalfAngle) / (1.0f - cosHalfAngle), 0.0f);
    }
    return w * cone * gridFalloff / (past * past + gridFalloff);
}

bool placeLight(const Light& light, LocalLight& out)
{
    if (!light.enabled())
        return false;
    LocalLight placed;
    float angle = 0.0f;
    Enum::NormalId face = Enum::NormalId::Front;
    if (const auto* point = instanceCast<const PointLight>(&light)) {
        placed.range = point->range();
    } else if (const auto* spot = instanceCast<const SpotLight>(&light)) {
        placed.kind = LocalLight::Kind::Spot;
        placed.range = spot->range();
        angle = spot->angle();
        face = spot->face();
    } else if (const auto* area = instanceCast<const SurfaceLight>(&light)) {
        placed.kind = LocalLight::Kind::Surface;
        placed.range = area->range();
        angle = area->angle();
        face = area->face();
    } else {
        return false;
    }
    // [code: 0x10486d134] Range in 0-120; an Angle of 0 lights nothing.
    placed.range = std::clamp(placed.range, 0.0f, 120.0f);
    angle = std::clamp(angle, 0.0f, 180.0f);
    if (placed.range <= 0.0f || (placed.kind != LocalLight::Kind::Point && angle <= 0.0f))
        return false;
    placed.color = light.color();
    placed.brightness = light.brightness();
    placed.shadows = light.shadows();
    placed.key = &light;

    // From the parent part's CFrame, or an Attachment's world CFrame.
    // [code: sub_1010ce506]
    const Instance* parent = light.parent();
    CFrame frame;
    Vector3 size;
    bool attached = false;
    if (const auto* part = instanceCast<const BasePart>(parent)) {
        frame = part->cframe();
        size = part->size();
    } else if (const auto* attachment = instanceCast<const Attachment>(parent)) {
        frame = attachment->worldCFrame();
        attached = true;
    } else {
        return false;
    }
    placed.position = frame.position;
    if (placed.kind != LocalLight::Kind::Point) {
        const Vector3 normal = faceNormal(face);
        placed.direction = frame.vectorToWorldSpace(normal).unit();
        placed.cosHalfAngle
            = std::clamp(std::cos(angle * 0.5f * std::numbers::pi_v<float> / 180.0f), 0.0001f, 0.9999f);
    }
    if (placed.kind == LocalLight::Kind::Surface) {
        // The face's rectangle: its middle, and the part's two other axes
        // (Y and Z for X faces, Z and X for Y faces, X and Y for Z faces);
        // a point's worth under an attachment. [code: 0x10486c9da]
        int axis = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(component(faceNormal(face), i)) > std::abs(component(faceNormal(face), axis)))
                axis = i;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        placed.axisU = frame.vectorToWorldSpace(unitAxis(u)).unit();
        placed.axisV = frame.vectorToWorldSpace(unitAxis(v)).unit();
        if (attached) {
            placed.halfU = placed.halfV = 0.0005f;
        } else {
            placed.position = frame.pointToWorldSpace(faceNormal(face) * (component(size, axis) * 0.5f));
            placed.halfU = component(size, u) * 0.5f;
            placed.halfV = component(size, v) * 0.5f;
        }
    }
    out = placed;
    return true;
}

LocalLights::LocalLights(Instance& workspace)
{
    for (const InstancePtr& d : workspace.descendants())
        add(d.get());
    added_ = workspace.descendantAdded.connect([this](Instance* instance) { add(instance); });
    removing_ = workspace.descendantRemoving.connect([this](Instance* instance) { remove(instance); });
}

LocalLights::~LocalLights()
{
    added_.disconnect();
    removing_.disconnect();
}

void LocalLights::add(Instance* instance)
{
    if (!instanceCast<Light>(instance))
        return;
    if (!index_.try_emplace(instance, entries_.size()).second)
        return;
    entries_.push_back({instance, instance->weak_from_this()});
}

void LocalLights::remove(Instance* instance)
{
    const auto it = index_.find(instance);
    if (it == index_.end())
        return;
    const std::size_t i = it->second;
    index_.erase(it);
    if (i + 1 != entries_.size()) {
        entries_[i] = std::move(entries_.back());
        index_[entries_[i].key] = i;
    }
    entries_.pop_back();
}

void LocalLights::collect(std::vector<LocalLight>& out)
{
    shining_ = 0;
    for (const Entry& entry : entries_) {
        const std::shared_ptr<const Instance> instance = entry.instance.lock();
        if (!instance)
            continue;
        LocalLight light;
        if (!placeLight(static_cast<const Light&>(*instance), light))
            continue;
        if (moving_ && instance->isDescendantOf(moving_))
            light.position = light.position + offset_;
        out.push_back(light);
        ++shining_;
    }
}

} // namespace lunar::render
