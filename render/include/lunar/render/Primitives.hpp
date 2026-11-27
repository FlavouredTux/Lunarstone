#pragma once

#include <lunar/Mesh.hpp>

#include <cstdint>
#include <memory>

namespace lunar::render {

// The built-in shapes, each filling the unit cube [-0.5, 0.5]^3 so a part's
// Size scales it. Shapes follow Roblox's conventions:
//   Wedge        slope faces the front (-Z) and rises to the back (+Z)
//   CornerWedge  apex at the right-top-front corner (+X, +Y, -Z)
//   Cylinder     along X, like Part.Shape = Cylinder
//   CylinderY    along Y, like CylinderMesh and SpecialMesh cylinders
//   Ball         a sphere
enum class Primitive : std::uint8_t { Block, Wedge, CornerWedge, Cylinder, CylinderY, Ball };

// Builds the shape. Curved shapes use the given number of segments around.
MeshData makePrimitive(Primitive primitive, std::uint32_t segments = 24);

// One shared copy per shape, so every part of that shape uses one GPU mesh.
const std::shared_ptr<const MeshData>& primitiveMesh(Primitive primitive);

} // namespace lunar::render
