#include "BlockCompression.hpp"
#include "ImageCodecs.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace lunar::assets::detail {

namespace {

// Uncompressed layouts beyond plain bit masks.
enum class Pixels : std::uint8_t {
    Masked,  // up to 32 bits a pixel with R, G, B, A (or luminance) masks
    Rgba16,  // 4 x 16-bit unorm
    Rg16,    // 2 x 16-bit unorm
    Rg16F,   // 2 x half
    R16,     // 16-bit unorm
    Rgba16F, // 4 x half
    R16F,
    Rgba32F, // 4 x float
    R32F,
    Rgb10A2,
};

struct Format {
    std::string name;
    std::optional<BlockFormat> block; // else uncompressed
    Pixels pixels = Pixels::Masked;
    unsigned bits = 0; // per pixel, uncompressed
    std::array<std::uint32_t, 4> masks{};
    bool luminance = false; // masks[0] is gray
};

Format blockFormat(std::string name, BlockFormat block)
{
    return Format{
        .name = std::move(name), .block = block, .pixels = Pixels::Masked, .bits = 0, .masks = {}, .luminance = false};
}

Format masked(std::string name, unsigned bits, std::array<std::uint32_t, 4> masks, bool luminance = false)
{
    return Format{.name = std::move(name),
                  .block = std::nullopt,
                  .pixels = Pixels::Masked,
                  .bits = bits,
                  .masks = masks,
                  .luminance = luminance};
}

Format wide(std::string name, Pixels pixels, unsigned bits)
{
    return Format{.name = std::move(name),
                  .block = std::nullopt,
                  .pixels = pixels,
                  .bits = bits,
                  .masks = {},
                  .luminance = false};
}

Result<Format> dxgiFormat(std::uint32_t dxgi)
{
    switch (dxgi) {
    case 70:
    case 71:
    case 72:
        return blockFormat("BC1", BlockFormat::Bc1);
    case 73:
    case 74:
    case 75:
        return blockFormat("BC2", BlockFormat::Bc2);
    case 76:
    case 77:
    case 78:
        return blockFormat("BC3", BlockFormat::Bc3);
    case 79:
    case 80:
        return blockFormat("BC4", BlockFormat::Bc4);
    case 81:
        return blockFormat("BC4 signed", BlockFormat::Bc4Snorm);
    case 82:
    case 83:
        return blockFormat("BC5", BlockFormat::Bc5);
    case 84:
        return blockFormat("BC5 signed", BlockFormat::Bc5Snorm);
    case 97:
    case 98:
    case 99:
        return blockFormat("BC7", BlockFormat::Bc7);
    case 27:
    case 28:
    case 29:
    case 30:
        return masked("RGBA8", 32, {0xff, 0xff00, 0xff0000, 0xff000000});
    case 87:
    case 90:
    case 91:
        return masked("BGRA8", 32, {0xff0000, 0xff00, 0xff, 0xff000000});
    case 88:
    case 92:
    case 93:
        return masked("BGRX8", 32, {0xff0000, 0xff00, 0xff, 0});
    case 60:
    case 61:
    case 62:
        return masked("R8", 8, {0xff, 0, 0, 0}, true);
    case 65:
        return masked("A8", 8, {0, 0, 0, 0xff});
    case 48:
    case 49:
    case 50:
        return masked("RG8", 16, {0xff, 0xff00, 0, 0});
    case 85:
        return masked("B5G6R5", 16, {0xf800, 0x07e0, 0x001f, 0});
    case 86:
        return masked("B5G5R5A1", 16, {0x7c00, 0x03e0, 0x001f, 0x8000});
    case 115:
        return masked("B4G4R4A4", 16, {0x0f00, 0x00f0, 0x000f, 0xf000});
    case 9:
    case 11:
    case 12:
        return wide("RGBA16", Pixels::Rgba16, 64);
    case 10:
        return wide("RGBA16F", Pixels::Rgba16F, 64);
    case 33:
    case 35:
    case 36:
        return wide("RG16", Pixels::Rg16, 32);
    case 34:
        return wide("RG16F", Pixels::Rg16F, 32);
    case 53:
    case 56:
    case 57:
        return wide("R16", Pixels::R16, 16);
    case 54:
        return wide("R16F", Pixels::R16F, 16);
    case 1:
    case 2:
        return wide("RGBA32F", Pixels::Rgba32F, 128);
    case 41:
        return wide("R32F", Pixels::R32F, 32);
    case 23:
    case 24:
    case 25:
        return wide("RGB10A2", Pixels::Rgb10A2, 32);
    case 94:
    case 95:
    case 96:
        return std::unexpected("BC6H (HDR) DDS files aren't supported");
    default:
        return std::unexpected(std::format("DXGI format {} isn't supported", dxgi));
    }
}

Result<Format> legacyFormat(std::string_view header)
{
    const std::uint32_t flags = le32(header, 80);
    const std::string_view fourCc = header.substr(84, 4);
    const std::uint32_t bitCount = le32(header, 88);
    const std::array<std::uint32_t, 4> masks{le32(header, 92), le32(header, 96), le32(header, 100), le32(header, 104)};
    if (flags & 0x4) { // DDPF_FOURCC
        if (fourCc == "DXT1")
            return blockFormat("BC1", BlockFormat::Bc1);
        if (fourCc == "DXT2" || fourCc == "DXT3")
            return blockFormat("BC2", BlockFormat::Bc2);
        if (fourCc == "DXT4" || fourCc == "DXT5")
            return blockFormat("BC3", BlockFormat::Bc3);
        if (fourCc == "ATI1" || fourCc == "BC4U")
            return blockFormat("BC4", BlockFormat::Bc4);
        if (fourCc == "BC4S")
            return blockFormat("BC4 signed", BlockFormat::Bc4Snorm);
        if (fourCc == "ATI2" || fourCc == "BC5U")
            return blockFormat("BC5", BlockFormat::Bc5);
        if (fourCc == "BC5S")
            return blockFormat("BC5 signed", BlockFormat::Bc5Snorm);
        // Old D3DFORMAT numbers in the FourCC field.
        switch (le32(header, 84)) {
        case 36:
            return wide("RGBA16", Pixels::Rgba16, 64);
        case 111:
            return wide("R16F", Pixels::R16F, 16);
        case 113:
            return wide("RGBA16F", Pixels::Rgba16F, 64);
        case 114:
            return wide("R32F", Pixels::R32F, 32);
        case 116:
            return wide("RGBA32F", Pixels::Rgba32F, 128);
        default:
            break;
        }
        std::string printable;
        for (char c : fourCc)
            printable += c >= 32 && c < 127 ? c : '?';
        return std::unexpected(std::format("DDS FourCC \"{}\" isn't supported", printable));
    }
    if (bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32)
        return std::unexpected(std::format("DDS with {} bits a pixel isn't supported", bitCount));
    if (flags & 0x20000) // DDPF_LUMINANCE
        return masked(std::format("L{}{}", bitCount, masks[3] ? "A" : ""), bitCount,
                      {masks[0], 0, 0, (flags & 0x1) ? masks[3] : 0}, true);
    if (flags & 0x40) // DDPF_RGB
        return masked(std::format("RGB{} masked", bitCount), bitCount,
                      {masks[0], masks[1], masks[2], (flags & 0x1) ? masks[3] : 0});
    if (flags & 0x2) // DDPF_ALPHA
        return masked("A8", bitCount, {0, 0, 0, masks[3]});
    return std::unexpected("DDS pixel format isn't supported");
}

std::size_t surfaceBytes(const Format& format, std::uint32_t width, std::uint32_t height)
{
    if (format.block)
        return blockImageBytes(*format.block, width, height);
    return (std::size_t{width} * format.bits + 7) / 8 * height;
}

float halfToFloat(std::uint16_t half)
{
    const int exponent = (half >> 10) & 31;
    const int mantissa = half & 1023;
    float value = 0.0f;
    if (exponent == 0)
        value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        value = mantissa ? 0.0f : 65504.0f; // NaN to 0, infinity to the largest finite
    else
        value = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
    return (half & 0x8000) ? -value : value;
}

std::uint8_t unitToByte(float value)
{
    if (!(value > 0.0f)) // NaN too
        return 0;
    if (value >= 1.0f)
        return 255;
    return static_cast<std::uint8_t>(std::lround(value * 255.0f));
}

std::uint8_t maskedChannel(std::uint32_t value, std::uint32_t mask)
{
    if (mask == 0)
        return 0;
    const int shift = std::countr_zero(mask);
    const std::uint32_t max = mask >> shift;
    const std::uint64_t channel = (value & mask) >> shift;
    return static_cast<std::uint8_t>((channel * 255 + max / 2) / max);
}

Image decodeUncompressed(const Format& format, std::string_view data, std::uint32_t width, std::uint32_t height)
{
    Image image = blankImage(width, height);
    const std::size_t pitch = (std::size_t{width} * format.bits + 7) / 8;
    const std::size_t pixelBytes = format.bits / 8;
    for (std::size_t y = 0; y < height; ++y) {
        const std::string_view row = data.substr(y * pitch, pitch);
        for (std::size_t x = 0; x < width; ++x) {
            std::uint8_t* out = image.rgba.data() + (y * width + x) * 4;
            const std::size_t at = x * pixelBytes;
            switch (format.pixels) {
            case Pixels::Masked: {
                std::uint32_t value = 0;
                for (std::size_t i = 0; i < pixelBytes; ++i)
                    value |= std::uint32_t{byteAt(row, at + i)} << (8 * i);
                if (format.luminance) {
                    out[0] = out[1] = out[2] = maskedChannel(value, format.masks[0]);
                } else {
                    for (std::size_t c = 0; c < 3; ++c)
                        out[c] = maskedChannel(value, format.masks[c]);
                }
                out[3] = format.masks[3] ? maskedChannel(value, format.masks[3]) : 255;
                break;
            }
            case Pixels::Rgba16:
                for (std::size_t c = 0; c < 4; ++c)
                    out[c] = byteAt(row, at + c * 2 + 1);
                break;
            case Pixels::Rg16:
                out[0] = byteAt(row, at + 1);
                out[1] = byteAt(row, at + 3);
                out[2] = 0;
                out[3] = 255;
                break;
            case Pixels::Rg16F:
                out[0] = unitToByte(halfToFloat(le16(row, at)));
                out[1] = unitToByte(halfToFloat(le16(row, at + 2)));
                out[2] = 0;
                out[3] = 255;
                break;
            case Pixels::R16:
                out[0] = out[1] = out[2] = byteAt(row, at + 1);
                out[3] = 255;
                break;
            case Pixels::Rgba16F:
                for (std::size_t c = 0; c < 4; ++c)
                    out[c] = unitToByte(halfToFloat(le16(row, at + c * 2)));
                break;
            case Pixels::R16F:
                out[0] = out[1] = out[2] = unitToByte(halfToFloat(le16(row, at)));
                out[3] = 255;
                break;
            case Pixels::Rgba32F:
                for (std::size_t c = 0; c < 4; ++c)
                    out[c] = unitToByte(std::bit_cast<float>(le32(row, at + c * 4)));
                break;
            case Pixels::R32F:
                out[0] = out[1] = out[2] = unitToByte(std::bit_cast<float>(le32(row, at)));
                out[3] = 255;
                break;
            case Pixels::Rgb10A2: {
                const std::uint32_t value = le32(row, at);
                out[0] = maskedChannel(value, 0x3ff);
                out[1] = maskedChannel(value, 0xffc00);
                out[2] = maskedChannel(value, 0x3ff00000);
                out[3] = maskedChannel(value, 0xc0000000);
                break;
            }
            }
        }
    }
    return image;
}

Image decodeSurface(const Format& format, std::string_view data, std::uint32_t width, std::uint32_t height)
{
    if (format.block)
        return decodeBlocks(*format.block, std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()),
                            width, height);
    return decodeUncompressed(format, data, width, height);
}

} // namespace

Result<TextureFile> decodeDdsFile(std::string_view bytes, DecodeOptions options)
{
    if (!bytes.starts_with("DDS "))
        return std::unexpected("not a DDS file");
    if (bytes.size() < 128)
        return std::unexpected("DDS header is truncated");
    const std::string_view header = bytes.substr(0, 128);
    if (le32(header, 4) != 124 || le32(header, 76) != 32)
        return std::unexpected("DDS header has the wrong size");
    const std::uint32_t height = le32(header, 12);
    const std::uint32_t width = le32(header, 16);
    const std::uint32_t flags = le32(header, 8);
    std::uint32_t depth = (flags & 0x800000) ? std::max<std::uint32_t>(1, le32(header, 24)) : 1;
    std::uint32_t mips = (flags & 0x20000) ? std::max<std::uint32_t>(1, le32(header, 28)) : 1;
    const std::uint32_t caps2 = le32(header, 112);
    if (auto size = checkSize(width, height); !size)
        return std::unexpected(size.error());

    std::size_t at = 128;
    Format format;
    std::uint32_t layers = 1;
    bool cubemap = false;
    bool volume = (caps2 & 0x200000) != 0 && depth > 1;
    if (header.substr(84, 4) == "DX10" && (le32(header, 80) & 0x4)) {
        if (bytes.size() < 148)
            return std::unexpected("DDS DX10 header is truncated");
        auto parsed = dxgiFormat(le32(bytes, 128));
        if (!parsed)
            return std::unexpected(parsed.error());
        format = std::move(*parsed);
        const std::uint32_t dimension = le32(bytes, 132);
        cubemap = (le32(bytes, 136) & 0x4) != 0;
        layers = std::max<std::uint32_t>(1, le32(bytes, 140));
        volume = dimension == 4 && depth > 1;
        if (cubemap) {
            if (layers > 0xffffffffu / 6)
                return std::unexpected("DDS has too many array slices");
            layers *= 6;
        }
        at = 148;
    } else {
        auto parsed = legacyFormat(header);
        if (!parsed)
            return std::unexpected(parsed.error());
        format = std::move(*parsed);
        if (caps2 & 0x200) {
            cubemap = true;
            layers = static_cast<std::uint32_t>(std::popcount(caps2 & 0xfc00u));
            if (layers == 0)
                layers = 6;
        }
    }
    if (!volume)
        depth = 1;
    else if (layers != 1 || cubemap)
        return std::unexpected("DDS volume arrays aren't supported");
    // More levels than halvings is a broken header; keep the ones that exist.
    const auto fullChain = static_cast<std::uint32_t>(std::bit_width(std::max({width, height, depth})));
    mips = std::min(mips, fullChain);

    // Per layer: every mip, each with its depth slices.
    std::vector<std::size_t> mipOffsets;
    std::size_t layerBytes = 0;
    for (std::uint32_t m = 0; m < mips; ++m) {
        mipOffsets.push_back(layerBytes);
        const std::uint32_t w = std::max(1u, width >> m);
        const std::uint32_t h = std::max(1u, height >> m);
        const std::uint32_t d = std::max(1u, depth >> m);
        layerBytes += surfaceBytes(format, w, h) * d;
    }
    const std::size_t available = bytes.size() - at;
    if (layerBytes == 0 || layers > available / layerBytes + 1)
        return std::unexpected("DDS data is truncated");

    TextureFile file;
    file.format = ImageFormat::Dds;
    file.pixelFormat = format.name;
    file.cubemap = cubemap;
    file.volume = volume;
    file.fileMips = mips;
    // A volume's slices become layers of level 0; its smaller levels aren't kept.
    file.mips = options.allMips && !volume ? mips : 1;
    file.layers = volume ? depth : layers;
    if (!options.allLayers)
        file.layers = 1;
    const std::size_t sliceBytes = surfaceBytes(format, width, height);
    for (std::uint32_t layer = 0; layer < file.layers; ++layer) {
        for (std::uint32_t m = 0; m < file.mips; ++m) {
            const std::uint32_t w = std::max(1u, width >> m);
            const std::uint32_t h = std::max(1u, height >> m);
            const std::size_t offset = volume ? at + sliceBytes * layer : at + layerBytes * layer + mipOffsets[m];
            const std::size_t size = surfaceBytes(format, w, h);
            if (offset > bytes.size() || size > bytes.size() - offset)
                return std::unexpected("DDS data is truncated");
            file.images.push_back(decodeSurface(format, bytes.substr(offset, size), w, h));
        }
    }
    // A plain 2D texture the GPU can sample compressed keeps its blocks too,
    // every level the file has.
    const std::optional<BlockCompressed::Format> gpu = format.block ? gpuFormat(*format.block) : std::nullopt;
    if (gpu && !volume && !cubemap && layers == 1 && !file.images.empty()) {
        auto blocks = std::make_shared<BlockCompressed>();
        blocks->format = *gpu;
        for (std::uint32_t m = 0; m < mips; ++m) {
            const std::size_t offset = at + mipOffsets[m];
            const std::size_t size = surfaceBytes(format, std::max(1u, width >> m), std::max(1u, height >> m));
            if (offset > bytes.size() || size > bytes.size() - offset)
                break;
            blocks->levels.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }
        if (!blocks->levels.empty())
            file.images.front().compressed = std::move(blocks);
    }
    return file;
}

} // namespace lunar::assets::detail
