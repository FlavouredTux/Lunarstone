#include <lunar/render/Graphics.hpp>

namespace lunar::render {

GraphicsSettings graphicsSettings(GraphicsPreset preset)
{
    GraphicsSettings settings;
    switch (preset) {
    case GraphicsPreset::Potato:
        settings = graphicsSettings(GraphicsPreset::Minimal);
        // Every texture a 16-texel smear, meshes at their coarsest, the
        // world gone past 250 studs, a tenth of the particles.
        settings.textureQuality = 0;
        settings.textureSide = 16;
        settings.lodPixelError = meshLodPixelError * 16.0f;
        settings.drawDistance = 250.0f;
        settings.terrainDistance = 0.25f;
        settings.particleDensity = 0.1f;
        settings.potato = true;
        break;
    case GraphicsPreset::Minimal:
        // No sun shadows (their pass and every pixel's taps), the plain
        // shading for every part, coarser meshes, nearer parts and terrain,
        // textures a quarter the size.
        settings.shadows = false;
        settings.sharpReflections = false;
        settings.physicalShading = false;
        // Local lights only through the light map, even with Future: per
        // pixel they were two thirds of the game's opaque pass at 4K (2.5 of 3.7
        // ms). GUESS: the client's low quality levels light with the voxel
        // grid alone whatever the Technology.
        settings.pixelLights = false;
        settings.lodPixelError = meshLodPixelError * 4.0f;
        settings.drawDistance = 450.0f; // the client's quality level 4
        settings.textureQuality = 1;
        settings.terrainDistance = 0.5f;
        // Half the particles. GUESS: the client's particle budget at its low
        // quality levels.
        settings.particleDensity = 0.5f;
        // No bloom, blur, sun rays or depth of field: the client's lower
        // quality levels leave them out too.
        settings.postEffects = false;
        break;
    case GraphicsPreset::Average:
        break;
    case GraphicsPreset::Max:
        // Sharper shadows twice as far, finer meshes, farther terrain.
        settings.shadowResolution = 4096;
        settings.shadowDistance = 320.0f;
        settings.lodPixelError = meshLodPixelError * 0.5f;
        settings.terrainDistance = 1.5f;
        break;
    }
    return settings;
}

std::optional<GraphicsPreset> parseGraphicsPreset(std::string_view name)
{
    if (name == "minimal")
        return GraphicsPreset::Minimal;
    if (name == "average")
        return GraphicsPreset::Average;
    if (name == "max")
        return GraphicsPreset::Max;
    if (name == "potato")
        return GraphicsPreset::Potato;
    return std::nullopt;
}

} // namespace lunar::render
