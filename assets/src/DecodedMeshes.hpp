#pragma once

// MeshLoader's parsed meshes on disk (a DerivedCache): a MeshFile as parsing
// and generateLods left it, read back instead of made again.

#include <lunar/assets/DerivedCache.hpp>
#include <lunar/assets/MeshFile.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lunar::assets {

class DecodedMeshes {
public:
    // levelsOfDetail: whether the loader makes levels (generateLods), which
    // meshes made without them don't share files with.
    DecodedMeshes(const DerivedCache& cache, bool levelsOfDetail);

    // The mesh made from `source` before, or nothing.
    std::optional<MeshFile> find(std::string_view source) const;
    // Keeps the mesh made from source, unless it's too small to be worth a file.
    void keep(std::string_view source, const MeshFile& mesh) const;

    // Meshes under this many triangles aren't kept: they parse in no time
    // and get no levels.
    static constexpr std::size_t minimumTriangles = 64;
    // Goes up whenever the parsers' or generateLods' output for the same
    // bytes would change: files of other versions are never read.
    static constexpr std::uint32_t version = 1;

    std::string nameFor(std::string_view source) const;

private:
    const DerivedCache* cache_;
    bool levelsOfDetail_ = true;
};

} // namespace lunar::assets
