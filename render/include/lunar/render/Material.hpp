#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>

#include <cstdint>

namespace lunar::render {

// Procedural surface detail a material gets in the shader; no textures.
enum class SurfacePattern : std::uint8_t { None, Speckle, Grain, Planks, Bricks, Cells, Blades, Sparkle };

// How a Roblox material looks, approximated: a tint over the part's colour,
// a specular highlight, and whether it glows or lets light through.
struct MaterialLook {
    float tint = 1.0f;            // albedo multiplier
    float specular = 0.0f;        // highlight strength, 0..1
    float shininess = 16.0f;      // Blinn-Phong exponent
    float emissive = 0.0f;        // 1 is unlit at full colour (Neon)
    float minTransparency = 0.0f; // see-through even at Transparency 0 (ForceField)
    SurfacePattern pattern = SurfacePattern::None;
    float patternScale = 1.0f;    // features per stud
    float patternStrength = 0.0f; // how far the pattern darkens/brightens, 0..1
};

MaterialLook materialLook(Enum::Material material);

} // namespace lunar::render
