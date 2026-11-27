#pragma once

// Roblox's precomputed collision geometry: the "CSGPHS" blobs MeshParts and
// unions carry in PhysicalConfigData (and the older PhysicsData).
//
// Read out of RobloxPlayer 0.742 (macOS x86_64), namespace
// RBX::ConvexDecomp::Serialization:
//   header check            sub_102fac390 (PhysicsMeshDecoder: ... strings)
//   decoder factory         sub_102fac28e: Null / Old (versions 0-7) / V8
//   PhysicsMeshDecoderImplOld::parse   sub_102facb6e
//   PhysicsMeshDecoderImplV8::parse    sub_102fadf34, multimesh sub_102fbe1e4,
//                                      Edgebreaker walk sub_102fbd9c2/sub_102fbd6ac
//   writers                 sub_102faec39 (v7), sub_102fa97e2 (v8)
//
// Layout, all little-endian:
//   "CSGPHS" u32 version
//   v0:  "BLOCK" follows: the part collides as its box. "CSGPHS" + u32 0 alone
//        means the same (sub_101153352 compares against that blob).
//   v7, v8: u8 CollisionGeometryType
//   v6, v7: 40 bytes of mass properties: f32 volume, vec3 center of mass,
//        6 f32 inertia (xx, xy, xz, yy, yz, zz), all unscaled
//   v1-v7: convex pieces until the end, each
//        u32 16, vec4 translation; u32 16, quaternion (x, y, z, w)
//        u32 floatCount, u32 4, floatCount f32 (x y z per vertex)
//        u32 indexCount, indexCount u32 (triangles; each < vertex count)
//      The decoder insists on the strides and that indices are in range. It
//      never applies the transform (it only checks it's identity, behind a
//      flag), so neither do we.
//   v8:  u8 reserved, then one zstd frame holding
//        u32 extraHulls, u32 extraVertices, u32 extraTriangles (in all),
//        u32 the most vertices, u32 the most triangles of one extra hull,
//        u32 sizeA, u32 bitCount, u32 sizeB, u32 sizeC,
//        vec3 boundsMin, vec3 boundsMax (0x3c bytes so far), then
//        A: two ragged arrays (u32 n, n u32 offsets starting at 0 and never
//           decreasing, offsets[n-1] u32 of data): triangle indices first,
//           then vertex floats; n - 1 pieces
//        B: bitCount bits of Edgebreaker symbols for the extra pieces
//        C: the extra pieces' vertex floats, back to back
//
// The client turns pieces into convex hulls of their vertices (the physics
// loader sub_1033b7040 only takes vertices), centers them on the bounds of
// all vertices and scales that box onto the part's Size (sub_1033997f2).

#include <lunar/Math.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::physics {

// CollisionGeometryType, in the order of the client's name table
// (0x1069edd9a: ConvexHull, VoxelConvexDecomp, ...). Files before v7 don't
// store it; the old decoder then reports 0 for no pieces, 1 for one and 2
// for more.
enum class CollisionGeometryType : std::uint8_t {
    ConvexHull,
    VoxelConvexDecomp,
    HACDConvexDecomp,
    HACDConvexDecompDynamic, // DEPRECATED_HACDConvexDecomp_Dynamic
    ClosedVoxelDynamic,
    ClosedVoxel,
    VoroCD,
};

struct CollisionMassProperties {
    float volume = 0.0f;
    Vector3 centerOfMass;
    std::array<float, 6> inertia{}; // xx, xy, xz, yy, yz, zz
};

// One convex piece, as stored (unscaled, not centered).
struct CollisionPiece {
    std::vector<Vector3> vertices;
    std::vector<std::uint32_t> indices; // triangles; empty for v8 Edgebreaker pieces
};

struct CollisionData {
    std::uint32_t version = 0;
    bool block = false; // collides as the part's box
    std::uint8_t geometryType = 0;
    std::optional<CollisionMassProperties> mass;
    std::vector<CollisionPiece> pieces;
    // Box around every vertex (v8 stores its own). Pieces are centered on it.
    Vector3 boundsMin;
    Vector3 boundsMax;
};

// The header alone, as sub_102fac390 checks it: magic and a version below 9.
bool isCollisionData(std::string_view bytes);

// Decodes a blob. Errors carry the reason; callers fall back to the box the
// way the client does ("Invalid mesh collision data - reverting to box
// collisions").
std::expected<CollisionData, std::string> decodeCollisionData(std::string_view bytes);

} // namespace lunar::physics
