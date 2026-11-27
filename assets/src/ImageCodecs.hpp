#pragma once

// Shared pieces of the image decoders.

#include <lunar/assets/ImageDecode.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace lunar::assets::detail {

template <class T>
using Result = std::expected<T, std::string>;

// Far beyond any Roblox texture (1024 x 1024, 4096 at most), small enough
// that a lying header can't make us allocate gigabytes.
inline constexpr std::uint32_t maxImageSide = 1u << 15;
inline constexpr std::uint64_t maxImagePixels = std::uint64_t{1} << 26;

inline Result<void> checkSize(std::uint64_t width, std::uint64_t height)
{
    if (width == 0 || height == 0)
        return std::unexpected("image has no pixels");
    if (width > maxImageSide || height > maxImageSide || width * height > maxImagePixels)
        return std::unexpected(std::format("image is too large ({} x {})", width, height));
    return {};
}

inline Image blankImage(std::uint32_t width, std::uint32_t height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.assign(image.byteSize(), 0);
    return image;
}

inline std::uint8_t byteAt(std::string_view bytes, std::size_t at)
{
    return static_cast<std::uint8_t>(bytes[at]);
}
inline std::uint16_t le16(std::string_view bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(byteAt(bytes, at) | (byteAt(bytes, at + 1) << 8));
}
inline std::uint32_t le32(std::string_view bytes, std::size_t at)
{
    return std::uint32_t{le16(bytes, at)} | (std::uint32_t{le16(bytes, at + 2)} << 16);
}
inline std::uint16_t be16(std::string_view bytes, std::size_t at)
{
    return static_cast<std::uint16_t>((byteAt(bytes, at) << 8) | byteAt(bytes, at + 1));
}
inline std::uint32_t be32(std::string_view bytes, std::size_t at)
{
    return (std::uint32_t{be16(bytes, at)} << 16) | be16(bytes, at + 2);
}

// One image as a TextureFile, for the formats that only hold one.
inline TextureFile singleImage(ImageFormat format, std::string pixelFormat, Image image)
{
    TextureFile file;
    file.format = format;
    file.pixelFormat = std::move(pixelFormat);
    file.images.push_back(std::move(image));
    return file;
}

Result<TextureFile> decodePngFile(std::string_view bytes);
Result<TextureFile> decodeJpegFile(std::string_view bytes);
Result<TextureFile> decodeBmpFile(std::string_view bytes);
Result<TextureFile> decodeTgaFile(std::string_view bytes);
Result<TextureFile> decodeDdsFile(std::string_view bytes, DecodeOptions options);
Result<TextureFile> decodeKtx2File(std::string_view bytes, DecodeOptions options);
bool plausibleTga(std::string_view bytes);

} // namespace lunar::assets::detail
