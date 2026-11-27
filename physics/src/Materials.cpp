#include <lunar/physics/Materials.hpp>

namespace lunar::physics {

PhysicalProperties defaultPhysicalProperties(Enum::Material material)
{
    // Binary: the table PhysicalProperties are built from in 0x101215c2a
    // (density, friction, elasticity, frictionWeight, elasticityWeight,
    // acousticAbsorption), indexed through the material map at 0x101216734.
    using M = Enum::Material;
    switch (material) {
    case M::SmoothPlastic:
        return {0.7f, 0.2f, 0.5f, 1.0f, 1.0f, 0.2f};
    case M::Neon:
        return {0.7f, 0.3f, 0.2f, 1.0f, 1.0f, 0.3f};
    case M::Wood:
    case M::WoodPlanks:
        return {0.35f, 0.48f, 0.2f, 1.0f, 1.0f, 0.25f};
    case M::Marble:
        return {2.563f, 0.2f, 0.17f, 1.0f, 1.0f, 0.01f};
    case M::Slate:
        return {2.691f, 0.4f, 0.2f, 1.0f, 1.0f, 0.03f};
    case M::Concrete:
        return {2.403f, 0.7f, 0.2f, 0.3f, 1.0f, 0.05f};
    case M::Granite:
        return {2.691f, 0.4f, 0.2f, 1.0f, 1.0f, 0.2f};
    case M::Brick:
        return {1.922f, 0.8f, 0.15f, 0.3f, 1.0f, 0.35f};
    case M::Pebble:
        return {2.403f, 0.4f, 0.17f, 1.0f, 1.5f, 0.4f};
    case M::Cobblestone:
        return {2.691f, 0.5f, 0.17f, 1.0f, 1.0f, 0.45f};
    case M::Rock:
        return {2.691f, 0.5f, 0.17f, 1.0f, 1.0f, 0.15f};
    case M::Sandstone:
        return {2.691f, 0.5f, 0.15f, 5.0f, 1.0f, 0.2f};
    case M::Basalt:
        return {2.691f, 0.7f, 0.15f, 0.3f, 1.0f, 0.2f};
    case M::CrackedLava:
        return {2.691f, 0.65f, 0.15f, 1.0f, 1.0f, 0.35f};
    case M::Limestone:
        return {2.691f, 0.5f, 0.15f, 1.0f, 1.0f, 0.15f};
    case M::Pavement:
        return {2.691f, 0.5f, 0.17f, 0.3f, 1.0f, 0.6f};
    case M::CorrodedMetal:
        return {7.85f, 0.7f, 0.2f, 1.0f, 1.0f, 0.15f};
    case M::DiamondPlate:
        return {7.85f, 0.35f, 0.25f, 1.0f, 1.0f, 0.05f};
    case M::Foil:
        return {2.7f, 0.4f, 0.25f, 1.0f, 1.0f, 0.1f};
    case M::Metal:
        return {7.85f, 0.4f, 0.25f, 1.0f, 1.0f, 0.1f};
    case M::Grass:
        return {0.9f, 0.4f, 0.1f, 1.0f, 1.5f, 0.7f};
    case M::LeafyGrass:
        return {0.9f, 0.4f, 0.1f, 2.0f, 2.0f, 0.75f};
    case M::Sand:
        return {1.602f, 0.5f, 0.05f, 5.0f, 2.5f, 0.55f};
    case M::Fabric:
        return {0.7f, 0.35f, 0.05f, 1.0f, 1.0f, 0.65f};
    case M::Snow:
        return {0.9f, 0.3f, 0.03f, 3.0f, 4.0f, 0.8f};
    case M::Mud:
        return {0.9f, 0.3f, 0.07f, 3.0f, 4.0f, 0.65f};
    case M::Ground:
        return {0.9f, 0.45f, 0.1f, 1.0f, 1.0f, 0.6f};
    case M::Asphalt:
        return {2.36f, 0.8f, 0.2f, 0.3f, 1.0f, 0.4f};
    case M::Salt:
        return {2.165f, 0.5f, 0.05f, 1.0f, 1.0f, 0.15f};
    case M::Ice:
        return {0.919f, 0.02f, 0.15f, 3.0f, 1.0f, 0.2f};
    case M::Glacier:
        return {0.919f, 0.05f, 0.15f, 2.0f, 1.0f, 0.7f};
    case M::Glass:
        return {2.403f, 0.25f, 0.2f, 1.0f, 1.0f, 0.1f};
    case M::ForceField:
        return {2.403f, 0.25f, 0.2f, 1.0f, 1.0f, 0.0f};
    case M::Air:
        return {0.01f, 0.01f, 0.01f, 1.0f, 1.0f, 0.01f};
    case M::Water:
        return {1.0f, 0.005f, 0.01f, 1.0f, 1.0f, 0.01f};
    case M::Cardboard:
        return {0.7f, 0.5f, 0.05f, 1.0f, 2.0f, 0.55f};
    case M::Carpet:
        return {1.1f, 0.4f, 0.25f, 1.0f, 2.0f, 0.65f};
    case M::CeramicTiles:
        return {2.4f, 0.51f, 0.2f, 1.0f, 1.0f, 0.04f};
    case M::ClayRoofTiles:
        return {2.0f, 0.51f, 0.2f, 1.0f, 1.0f, 0.3f};
    case M::RoofShingles:
        return {2.36f, 0.8f, 0.2f, 0.3f, 1.0f, 0.3f};
    case M::Leather:
        return {0.86f, 0.35f, 0.25f, 1.0f, 1.0f, 0.65f};
    case M::Plaster:
        return {0.75f, 0.6f, 0.2f, 0.3f, 1.0f, 0.3f};
    case M::Rubber:
        return {1.3f, 1.5f, 0.95f, 3.0f, 2.0f, 0.5f};
    default:
        // Plastic, and anything the map doesn't know.
        return {0.7f, 0.3f, 0.5f, 1.0f, 1.0f, 0.3f};
    }
}

} // namespace lunar::physics
