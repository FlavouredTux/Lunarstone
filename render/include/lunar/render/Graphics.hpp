#pragma once

// Lunarstone's own graphics presets (--graphics): not the client's quality
// levels, but a few settings that trade looks for frames.

#include <lunar/render/Lod.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace lunar::render {

// Raytracing is to come. Potato, below Minimal, gives up every look there
// is for frames.
enum class GraphicsPreset : std::uint8_t { Minimal, Average, Max, Potato };

// What a preset sets. The defaults are Average: everything as the renderer
// draws without a preset.
struct GraphicsSettings {
    bool shadows = true;
    std::uint32_t shadowResolution = 2048;   // texels on a side of the sun's shadow map
    float shadowDistance = 160.0f;           // studs from the camera the map covers
    bool sharpReflections = true;            // the skybox in reflections, else only its blurred average
    bool physicalShading = true;             // SurfaceAppearance and normal, roughness, metalness maps
    bool pixelLights = true;                 // Future's local lights per pixel near the camera, else the light map's
    float lodPixelError = meshLodPixelError; // screen error a mesh's level of detail may show, pixels
    float drawDistance = 100000.0f;          // studs to the farthest parts drawn
    int textureQuality = 3;                  // TextureQualityLevel at most: 0 (Low) to 3 (VeryHigh)
    float terrainDistance = 1.0f;            // terrain's levels of detail switch this much farther out
    float particleDensity = 1.0f;            // share of the particles emitters emit (ParticleSettings::density)
    bool postEffects = true;                 // Lighting's bloom, blur, sun rays and depth of field (PostPass)
    std::uint32_t textureSide = 0;           // the longest side a texture is drawn at, 0 for no cap
    // The scene at half the render scale, scaled up blocky, and lit by the
    // sun and the sky alone in a few hard steps (mesh.frag).
    bool potato = false;
    // Sun shadows by ray queries instead of the shadow map (--raytracing);
    // the context has to have been made with ContextOptions::rayTracing, and
    // a renderer made on it traces whatever this says. No preset sets it.
    bool rayTracing = false;
    // With ray tracing, every surface a mirror by its Fresnel term, for the
    // look of it (--reflect-everything). No preset sets it.
    bool reflectEverything = false;
    // With ray tracing, light from all around (Ambient, the sky's, the
    // light map's) kept out of corners and from under things by rays
    // (--ambient-occlusion). The client has none. No preset sets it.
    bool ambientOcclusion = false;
    // With ray tracing, every light near the camera casts shadows, also
    // those whose Shadows is off (--shadow-all-lights). No preset sets it.
    bool shadowAllLights = false;
    // The 3D image's size against the target's, scaled up onto it before
    // anything is drawn over it (the GUI stays sharp). No preset sets it.
    float renderScale = 1.0f;

    friend bool operator==(const GraphicsSettings&, const GraphicsSettings&) = default;
};

// Minimal for the most frames, Average as things should look, Max with
// everything up, Potato for even more frames and no looks at all.
GraphicsSettings graphicsSettings(GraphicsPreset preset);

// "minimal", "average", "max" or "potato".
std::optional<GraphicsPreset> parseGraphicsPreset(std::string_view name);

} // namespace lunar::render
