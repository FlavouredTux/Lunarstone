#include "ImageCodecs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lunar::assets {

std::string_view imageFormatName(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return "PNG";
    case ImageFormat::Jpeg:
        return "JPEG";
    case ImageFormat::Dds:
        return "DDS";
    case ImageFormat::Ktx2:
        return "KTX2";
    case ImageFormat::Bmp:
        return "BMP";
    case ImageFormat::Tga:
        return "TGA";
    case ImageFormat::Unknown:
        break;
    }
    return "unknown";
}

ImageFormat detectImageFormat(std::string_view bytes)
{
    if (bytes.starts_with("\x89PNG\r\n\x1a\n"))
        return ImageFormat::Png;
    if (bytes.starts_with("\xff\xd8\xff"))
        return ImageFormat::Jpeg;
    if (bytes.starts_with("DDS "))
        return ImageFormat::Dds;
    if (bytes.starts_with("\xabKTX 20\xbb\r\n\x1a\n"))
        return ImageFormat::Ktx2;
    if (bytes.starts_with("BM") && bytes.size() >= 26)
        return ImageFormat::Bmp;
    if (detail::plausibleTga(bytes))
        return ImageFormat::Tga;
    return ImageFormat::Unknown;
}

std::expected<TextureFile, std::string> decodeTexture(std::string_view bytes, DecodeOptions options)
{
    switch (detectImageFormat(bytes)) {
    case ImageFormat::Png:
        return detail::decodePngFile(bytes);
    case ImageFormat::Jpeg:
        return detail::decodeJpegFile(bytes);
    case ImageFormat::Dds:
        return detail::decodeDdsFile(bytes, options);
    case ImageFormat::Ktx2:
        return detail::decodeKtx2File(bytes, options);
    case ImageFormat::Bmp:
        return detail::decodeBmpFile(bytes);
    case ImageFormat::Tga:
        return detail::decodeTgaFile(bytes);
    case ImageFormat::Unknown:
        break;
    }
    if (bytes.starts_with("<roblox"))
        return std::unexpected("a Roblox model, not an image (a Decal asset id where an image id belongs?)");
    if (bytes.starts_with("{"))
        return std::unexpected("JSON, not an image");
    return std::unexpected("not an image format this client understands");
}

namespace {

std::expected<Image, std::string> first(std::expected<TextureFile, std::string> file)
{
    if (!file)
        return std::unexpected(std::move(file.error()));
    if (file->images.empty())
        return std::unexpected("texture has no images");
    return std::move(file->images.front());
}

} // namespace

std::expected<Image, std::string> decodeImage(std::string_view bytes)
{
    return first(decodeTexture(bytes, {.allMips = false, .allLayers = false}));
}

Image limitImageSize(Image image, std::uint32_t maxDimension)
{
    maxDimension = std::max(maxDimension, 1u);
    while (!image.empty() && image.rgba.size() == image.byteSize()
           && std::max(image.width, image.height) > maxDimension) {
        Image half;
        half.width = std::max(image.width / 2, 1u);
        half.height = std::max(image.height / 2, 1u);
        half.rgba.resize(half.byteSize());
        const std::size_t stride = std::size_t{image.width} * 4;
        for (std::uint32_t y = 0; y < half.height; ++y) {
            const std::uint8_t* top = image.rgba.data() + std::min(2 * y, image.height - 1) * stride;
            const std::uint8_t* bottom = image.rgba.data() + std::min(2 * y + 1, image.height - 1) * stride;
            std::uint8_t* out = half.rgba.data() + std::size_t{y} * half.width * 4;
            for (std::uint32_t x = 0; x < half.width; ++x) {
                const std::size_t left = std::size_t{std::min(2 * x, image.width - 1)} * 4;
                const std::size_t right = std::size_t{std::min(2 * x + 1, image.width - 1)} * 4;
                for (std::size_t c = 0; c < 4; ++c)
                    out[std::size_t{x} * 4 + c] = static_cast<std::uint8_t>(
                        (top[left + c] + top[right + c] + bottom[left + c] + bottom[right + c] + 2) / 4);
            }
        }
        // The compressed levels shrink alike, or go if too few are left.
        if (image.compressed) {
            if (image.compressed->levels.size() > 1) {
                auto blocks = std::make_shared<BlockCompressed>(*image.compressed);
                blocks->levels.erase(blocks->levels.begin());
                half.compressed = std::move(blocks);
            }
        }
        image = std::move(half);
    }
    return image;
}

std::expected<Image, std::string> decodePng(std::string_view bytes)
{
    return first(detail::decodePngFile(bytes));
}

std::expected<Image, std::string> decodeJpeg(std::string_view bytes)
{
    return first(detail::decodeJpegFile(bytes));
}

std::expected<Image, std::string> decodeBmp(std::string_view bytes)
{
    return first(detail::decodeBmpFile(bytes));
}

std::expected<Image, std::string> decodeTga(std::string_view bytes)
{
    return first(detail::decodeTgaFile(bytes));
}

std::expected<TextureFile, std::string> decodeDds(std::string_view bytes, DecodeOptions options)
{
    return detail::decodeDdsFile(bytes, options);
}

} // namespace lunar::assets
