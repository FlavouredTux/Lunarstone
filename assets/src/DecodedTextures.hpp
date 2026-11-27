#pragma once

// TextureLoader's decoded textures on disk (a DerivedCache): the RGBA it
// decoded, raw, mapped back into memory instead of decoding again.

#include <lunar/Image.hpp>
#include <lunar/assets/DerivedCache.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lunar::assets {

class DecodedTextures {
public:
    // maxDimension is the loader's size limit, which the images were made under.
    DecodedTextures(const DerivedCache& cache, std::uint32_t maxDimension);

    // The image decoded from `source` before, mapped (its pixels external,
    // read from the file as they're touched), or nothing.
    std::optional<Image> find(std::string_view source) const;
    // Keeps the image decoded from source, and gives it back mapped from
    // the file kept; as it was when it can't be kept (too small to be worth
    // a file, block-compressed, or the file can't be written).
    Image keep(std::string_view source, Image image) const;

    // Images under this many bytes stay in memory: decoding them is cheap.
    static constexpr std::size_t minimumBytes = 64 * 1024;
    // What a file holds before its pixels.
    static constexpr std::size_t headerBytes = 64;
    // Goes up whenever the decoders' output for the same bytes would change:
    // files of other versions are never read.
    static constexpr std::uint32_t version = 1;

    // The file the image decoded from source is kept in (tests).
    std::string nameFor(std::string_view source) const;

private:
    const DerivedCache* cache_;
    std::uint32_t maxDimension_ = 0;
};

} // namespace lunar::assets
