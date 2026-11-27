#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>
#include <lunar/render/Camera.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lunar::render {

// How far from the camera a mesh's coarser levels of detail take over, as
// the client picks them (RBX::Graphics::InstancedCluster2 in 0.742): level
// k + 1 once the camera is meshLodDistances[k] studs from the part's box.
// These are FInt CSGLevelOfDetailSwitchingDistance, ...L12, ...L23 and ...L34,
// and FInt MeshNLoDs (5) caps the table at five levels; a mesh with fewer
// stays at its last. They hold at the default field of view of 70 degrees:
// the client scales them by tan(35 degrees) / tan(FieldOfView / 2), so
// zooming in keeps the detail further out.
//
// Meshes whose levels come with how far each strays from the full one
// (MeshData::lodErrors; in the client, the ones it transcoded) go by screen
// size instead: the coarsest level whose error covers at most
// meshLodPixelError pixels at the part's distance and scale (FInt
// QuadradicErrorPercentPerPixel, 120, in hundredths of a pixel).
inline constexpr std::array<float, 4> meshLodDistances{500.0f, 1000.0f, 2000.0f, 3000.0f};
inline constexpr float meshLodPixelError = 1.2f;

// Picks levels of detail for one view, so the per-item work is a few
// multiplies.
class LodSelector {
public:
    // viewportHeight in pixels, for the screen-size rule.
    // pixelError: the screen error levelForError allows (GraphicsSettings).
    explicit LodSelector(const Camera& camera, float viewportHeight = 720.0f, float pixelError = meshLodPixelError);

    // The level to draw a mesh with `levels` levels at, for a part whose
    // world-space box is center +- halfExtent.
    std::uint32_t level(Vector3 center, Vector3 halfExtent, std::uint32_t levels,
                        Enum::RenderFidelity fidelity = Enum::RenderFidelity::Automatic) const;
    // The same from the squared distance between the camera and the box.
    std::uint32_t levelAt(float squaredDistance, std::uint32_t levels,
                          Enum::RenderFidelity fidelity = Enum::RenderFidelity::Automatic) const;
    // By screen size, for levels that stray errors[k] from the full one
    // (errors[0], the full level's, is ignored), on a part that scales the
    // mesh by up to `scale`.
    std::uint32_t levelForError(float squaredDistance, float scale, std::span<const float> errors,
                                Enum::RenderFidelity fidelity = Enum::RenderFidelity::Automatic) const;

    Vector3 eye() const { return eye_; }

private:
    Vector3 eye_;
    std::array<float, meshLodDistances.size()> squared_{};
    float errorPerDistance_ = 0.0f; // squared: what meshLodPixelError covers one stud away
};

// Squared distance from a point to a box (zero inside it).
float squaredDistanceToBox(Vector3 point, Vector3 center, Vector3 halfExtent);

// Studs the client draws parts out to at a graphics quality level (1 to 21,
// Enum.QualityLevel; anything else counts as 21): the frame rate manager's
// table for desktops with 4 GB and 4 cores or more. The top levels draw
// everything there is (100000 studs).
float qualityDrawDistance(int qualityLevel);

} // namespace lunar::render
