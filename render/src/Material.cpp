#include <lunar/render/Material.hpp>

namespace lunar::render {

MaterialLook materialLook(Enum::Material material)
{
    using enum Enum::Material;
    using enum SurfacePattern;
    switch (material) {
    case Plastic:
        return {}; // matte
    case SmoothPlastic:
        return {.specular = 0.15f, .shininess = 32.0f};
    case Neon:
        return {.emissive = 1.0f};
    case Glass:
        return {.specular = 0.8f, .shininess = 96.0f}; // opaque until Transparency says otherwise
    case ForceField:
        return {.emissive = 0.6f, .minTransparency = 0.6f};
    case Ice:
    case Glacier:
        return {
            .specular = 0.5f, .shininess = 64.0f, .pattern = Speckle, .patternScale = 2.0f, .patternStrength = 0.1f};
    case Metal:
    case DiamondPlate:
        return {.tint = 0.9f,
                .specular = 0.5f,
                .shininess = 48.0f,
                .pattern = Speckle,
                .patternScale = 6.0f,
                .patternStrength = 0.1f};
    case Foil:
        return {.specular = 0.7f, .shininess = 64.0f, .pattern = Grain, .patternScale = 3.0f, .patternStrength = 0.15f};
    case CorrodedMetal:
        return {.tint = 0.85f,
                .specular = 0.2f,
                .shininess = 16.0f,
                .pattern = Cells,
                .patternScale = 1.5f,
                .patternStrength = 0.3f};
    case Wood:
        return {.tint = 0.95f, .pattern = Grain, .patternScale = 1.0f, .patternStrength = 0.3f};
    case WoodPlanks:
        return {.tint = 0.95f, .pattern = Planks, .patternScale = 1.0f, .patternStrength = 0.35f};
    case Brick:
    case ClayRoofTiles:
    case RoofShingles:
    case CeramicTiles:
        return {.pattern = Bricks, .patternScale = 1.0f, .patternStrength = 0.35f};
    case Cobblestone:
    case Pebble:
        return {.pattern = Cells, .patternScale = 1.2f, .patternStrength = 0.45f};
    case Grass:
    case LeafyGrass:
        return {.pattern = Blades, .patternScale = 6.0f, .patternStrength = 0.4f};
    case Sand:
    case Salt:
        return {.pattern = Speckle, .patternScale = 12.0f, .patternStrength = 0.15f};
    case Snow:
        return {.specular = 0.1f, .pattern = Sparkle, .patternScale = 10.0f, .patternStrength = 0.2f};
    case Concrete:
    case Pavement:
    case Asphalt:
    case Plaster:
    case Limestone:
    case Sandstone:
        return {.pattern = Speckle, .patternScale = 8.0f, .patternStrength = 0.2f};
    case Granite:
    case Rock:
    case Basalt:
    case Slate:
    case Marble:
    case CrackedLava:
        return {.specular = material == Marble ? 0.3f : 0.05f,
                .shininess = 32.0f,
                .pattern = Speckle,
                .patternScale = 3.0f,
                .patternStrength = 0.3f};
    case Fabric:
    case Carpet:
    case Leather:
    case Cardboard:
        return {.pattern = Speckle, .patternScale = 10.0f, .patternStrength = 0.15f};
    case Mud:
    case Ground:
        return {.tint = 0.95f, .pattern = Speckle, .patternScale = 4.0f, .patternStrength = 0.3f};
    case Rubber:
        return {.specular = 0.1f, .shininess = 8.0f};
    case Water:
        return {.specular = 0.6f, .shininess = 64.0f, .minTransparency = 0.3f};
    case Air:
        return {.minTransparency = 1.0f};
    }
    return {};
}

} // namespace lunar::render
