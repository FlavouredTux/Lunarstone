#include "DecodedTextures.hpp"

#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <vector>

namespace lunar::assets {

namespace {

// A file's first bytes: what it is, which version wrote it, the size limit
// the image was decoded under, the content key it was decoded from, and the
// image's size. Little-endian, as the machines this runs on.
constexpr std::array<char, 8> magic{'L', 'U', 'N', 'A', 'R', 'R', 'G', 'B'};

struct Header {
    std::array<char, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t maxDimension = 0;
    std::array<std::uint8_t, 16> key{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixelBytes = 0;
    std::array<std::uint8_t, 16> reserved{};
};
static_assert(sizeof(Header) == DecodedTextures::headerBytes);

// The image in the file, if it is one of key's under this version and size
// limit, and whole.
std::optional<Image> imageIn(const std::shared_ptr<const MappedFile>& file, const std::array<std::uint8_t, 16>& key,
                             std::uint32_t maxDimension)
{
    if (!file || file->bytes().size() < sizeof(Header))
        return std::nullopt;
    Header header;
    std::memcpy(&header, file->bytes().data(), sizeof header);
    const std::uint64_t pixels = std::uint64_t{header.width} * header.height * 4;
    if (header.magic != magic || header.version != DecodedTextures::version || header.maxDimension != maxDimension
        || header.key != key || header.width == 0 || header.height == 0 || header.width > maxDimension
        || header.height > maxDimension || header.pixelBytes != pixels
        || file->bytes().size() != sizeof header + pixels)
        return std::nullopt;
    Image image;
    image.width = header.width;
    image.height = header.height;
    image.rgba = PixelBuffer::external(file->bytes().data() + sizeof header, pixels, file);
    return image;
}

} // namespace

DecodedTextures::DecodedTextures(const DerivedCache& cache, std::uint32_t maxDimension)
    : cache_(&cache), maxDimension_(maxDimension)
{
}

std::string DecodedTextures::nameFor(std::string_view source) const
{
    return DerivedCache::name(source, std::format("{}.v{}.rgba", maxDimension_, version));
}

std::optional<Image> DecodedTextures::find(std::string_view source) const
{
    if (!cache_->enabled())
        return std::nullopt;
    return imageIn(cache_->open(nameFor(source)), DerivedCache::key(source), maxDimension_);
}

Image DecodedTextures::keep(std::string_view source, Image image) const
{
    if (!cache_->enabled() || image.compressed || image.empty() || image.rgba.size() != image.byteSize()
        || image.byteSize() < minimumBytes || image.width > maxDimension_ || image.height > maxDimension_)
        return image;
    Header header;
    header.magic = magic;
    header.version = version;
    header.maxDimension = maxDimension_;
    header.key = DerivedCache::key(source);
    header.width = image.width;
    header.height = image.height;
    header.pixelBytes = image.byteSize();
    const std::string name = nameFor(source);
    const std::array<std::span<const std::uint8_t>, 2> parts{
        std::span(reinterpret_cast<const std::uint8_t*>(&header), sizeof header),
        std::span(image.rgba.data(), image.rgba.size())};
    if (!cache_->write(name, parts))
        return image;
    if (std::optional<Image> mapped = imageIn(MappedFile::open(cache_->path(name)), header.key, maxDimension_))
        return std::move(*mapped);
    return image;
}

} // namespace lunar::assets
