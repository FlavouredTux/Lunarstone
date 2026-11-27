#pragma once

#include <lunar/Instance.hpp>
#include <lunar/Math.hpp>
#include <lunar/Signal.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lunar {
class Light;
} // namespace lunar

namespace lunar::render {

// A PointLight, SpotLight or SurfaceLight where it is this frame, as the
// client keeps its lights (RBX::Graphics::LightObject in the 0.742 macOS
// player): a point, or for a SurfaceLight a rectangle (the face), with a
// range, a colour and a cone. It lights two ways: through the light grid's
// voxels (gridColor, gridStrength) and per pixel with Future lighting
// (pixelColor, and lights.glsl's localLightStrength, which pixelLight
// mirrors).
struct LocalLight {
    enum class Kind : std::uint8_t { Point, Spot, Surface };
    Kind kind = Kind::Point;
    Vector3 position;     // the point, or the middle of the rectangle
    Color3 color;         // Light.Color
    float brightness = 0; // Light.Brightness
    float range = 0.0f;   // studs, 0 to 120; nothing past it
    Vector3 direction;    // where a spot or surface light shines (unit); zero for a point light
    // Of its cone, the cosine of half Angle (the client's 0.0001 to 0.9999);
    // 0 for a point light, which has none.
    float cosHalfAngle = 0.0f;
    // A SurfaceLight's rectangle: its two axes (unit) and half sizes along
    // them; zero for the others.
    Vector3 axisU;
    Vector3 axisV;
    float halfU = 0.0f;
    float halfV = 0.0f;
    bool shadows = false;          // Light.Shadows
    const Instance* key = nullptr; // the Light, to tell lights apart

    // Where its falloff starts, as a share of the range: full strength
    // (before the facing) out to there. [code: sub_10486e184]
    static constexpr float falloffStart = 0.71f;

    // How far its light reaches from position: the range, plus the
    // rectangle's half diagonal.
    float reach() const;
    // Its colour per pixel: Color to the power 2.2 times Brightness (at
    // most 10000). [code: 0x1047e67c3]
    Color3 pixelColor() const;
    // Its colour in the light grid: Color to 8 bits, squared, times
    // Brightness. [code: 0x10485f71c]
    Color3 gridColor() const;
    // How strongly it lights a surface at a point facing n per pixel, 0 to
    // 1 (times pixelColor): the client's Future falloff, cone and N.L, as
    // lights.glsl has them; no shadows.
    float pixelStrength(Vector3 point, Vector3 n) const;
    // How strongly it reaches a point in the light grid (times gridColor),
    // before the walls between: the grid's falloff and cone, which don't
    // care which way a surface faces.
    float gridStrength(Vector3 point) const;
};

// Where a Light shines from this frame, or false when it doesn't shine:
// off, out of range or not under a part or an attachment.
bool placeLight(const Light& light, LocalLight& out);

// Something that hands MeshRenderer the frame's lights (setLights).
class LightSource {
public:
    virtual ~LightSource() = default;
    // Appends this frame's lights; called once per frame.
    virtual void collect(std::vector<LocalLight>& out) = 0;
};

// The PointLights, SpotLights and SurfaceLights below the workspace, now and
// as they come and go (its DescendantAdded and DescendantRemoving), placed
// where their parts and attachments are each frame.
class LocalLights : public LightSource {
public:
    explicit LocalLights(Instance& workspace);
    ~LocalLights() override;
    LocalLights(const LocalLights&) = delete;
    LocalLights& operator=(const LocalLights&) = delete;

    // Lights below `moving` shine shifted by `offset`, the way
    // PlaceScene::followParts draws a character between physics steps.
    void follow(const Instance* moving, Vector3 offset)
    {
        moving_ = moving;
        offset_ = offset;
    }

    void collect(std::vector<LocalLight>& out) override;

    // Lights known, and how many shone in the last collect.
    std::size_t count() const { return entries_.size(); }
    std::size_t shining() const { return shining_; }

private:
    struct Entry {
        const Instance* key = nullptr;
        std::weak_ptr<const Instance> instance;
    };
    void add(Instance* instance);
    void remove(Instance* instance);

    Connection added_;
    Connection removing_;
    std::vector<Entry> entries_;
    std::unordered_map<const Instance*, std::size_t> index_;
    std::size_t shining_ = 0;
    const Instance* moving_ = nullptr;
    Vector3 offset_;
};

} // namespace lunar::render
