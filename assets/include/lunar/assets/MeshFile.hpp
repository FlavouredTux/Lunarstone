#pragma once

#include <lunar/Math.hpp>
#include <lunar/Mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::assets {

// One part a union was made from (its PartOperationAsset's ChildData), in
// the union's own studs, as the mesh is.
struct UnionOperand {
    enum class Kind : std::uint8_t { Block, Wedge, CornerWedge, Cylinder, Ball, Other };
    Kind kind = Kind::Other; // Other: a mesh, a nested union, anything without a primitive shape
    CFrame cframe;
    Vector3 size;
    bool negate = false; // a NegateOperation: cut out of the rest
};

// A Roblox .mesh file, reduced to what drawing needs: the highest level of
// detail as a MeshData (and with MeshParseOptions::keepLods the coarser ones,
// in MeshData::lodIndices), plus the box around the highest. MeshPart
// stretches that box to its Size (Roblox stores it as MeshSize/InitialSize).
struct MeshFile {
    std::string version; // "2.00", "7.00", ...
    MeshData mesh;
    Vector3 boundsMin;
    Vector3 boundsMax;
    std::size_t lodCount = 1;  // levels of detail in the file; mesh.levels() of them are kept
    std::size_t boneCount = 0; // skinning that was skipped
    bool hasFacs = false;      // facial animation data that was skipped
    // A union's asset only: the parts it was made from (empty when its
    // ChildData didn't load).
    std::vector<UnionOperand> operands;

    Vector3 size() const { return boundsMax - boundsMin; }
    Vector3 center() const { return (boundsMin + boundsMax) * 0.5f; }
};

struct MeshParseOptions {
    // Keep the file's coarser levels of detail, as the file has them. The
    // client drops them (its FFlag SupportMeshLOD is off: the mesh loader
    // cuts the faces to LOD 0's) and streams levels Roblox's servers
    // transcode instead. With good reason: the ones in real 3.00 files wind
    // a fifth to a third of their faces the wrong way round, and the
    // vertices only they use carry unnormalised normals and black colours.
    bool keepLods = false;
};

// Whether the bytes start like a mesh file ("version N.NN").
bool looksLikeMesh(std::string_view bytes);

// Parses every Roblox mesh version: 1.00/1.01 (text), 2.00, 3.00/3.01,
// 4.00/4.01, 5.00 and the chunked 6.00/7.00. Version 7 stores geometry with
// Google Draco, which needs a build with LUNAR_WITH_DRACO. Never trusts a
// count or offset in the file; garbage gives an error, not a crash.
std::expected<MeshFile, std::string> parseMesh(std::string_view bytes, const MeshParseOptions& options = {});

// A union's geometry: the MeshData of its PartOperationAsset, which starts
// "CSGMDL" and a u32 version XORed with a 31-byte key. Version 2 is XORed
// throughout and holds count x 84-byte vertices (position, normal, RGBA
// colour, ...) and a u32-counted index list. Version 5 is plain after the
// header: one array per attribute, delta-coded indices and levels of detail
// (the first is drawn). Both are in the union's own studs. parseMesh takes
// the whole asset (a model file holding one PartOperationAsset) too, and
// then also reads the union's operands from its ChildData.
std::expected<MeshFile, std::string> parseCsgMesh(std::string_view meshData);

// Whether this build can decode Draco-compressed (version 7) geometry.
bool meshDracoSupported();

} // namespace lunar::assets
