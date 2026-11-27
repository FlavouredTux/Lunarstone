#include "BlockCompression.hpp"
#include "ImageCodecs.hpp"
#include "Zstd.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <memory>
#include <optional>

namespace lunar::assets::detail {

namespace {

struct VkFormat {
    std::string_view name;
    std::optional<BlockFormat> block;
    unsigned channels = 0; // uncompressed 8-bit: 1 R, 2 RG, 4 RGBA
    bool bgra = false;
};

std::optional<VkFormat> vkFormat(std::uint32_t format)
{
    switch (format) {
    case 9:
    case 10:
    case 13:
    case 14:
    case 15:
        return VkFormat{"R8", std::nullopt, 1, false};
    case 16:
    case 17:
    case 20:
    case 21:
    case 22:
        return VkFormat{"RG8", std::nullopt, 2, false};
    case 37:
    case 38:
    case 41:
    case 42:
    case 43:
        return VkFormat{"RGBA8", std::nullopt, 4, false};
    case 44:
    case 45:
    case 48:
    case 49:
    case 50:
        return VkFormat{"BGRA8", std::nullopt, 4, true};
    case 131:
    case 132:
    case 133:
    case 134:
        return VkFormat{"BC1", BlockFormat::Bc1, 0, false};
    case 135:
    case 136:
        return VkFormat{"BC2", BlockFormat::Bc2, 0, false};
    case 137:
    case 138:
        return VkFormat{"BC3", BlockFormat::Bc3, 0, false};
    case 139:
        return VkFormat{"BC4", BlockFormat::Bc4, 0, false};
    case 140:
        return VkFormat{"BC4 signed", BlockFormat::Bc4Snorm, 0, false};
    case 141:
        return VkFormat{"BC5", BlockFormat::Bc5, 0, false};
    case 142:
        return VkFormat{"BC5 signed", BlockFormat::Bc5Snorm, 0, false};
    case 145:
    case 146:
        return VkFormat{"BC7", BlockFormat::Bc7, 0, false};
    case 147:
    case 148:
        return VkFormat{"ETC2 RGB", BlockFormat::Etc2Rgb, 0, false};
    case 151:
    case 152:
        return VkFormat{"ETC2 RGBA", BlockFormat::Etc2Rgba, 0, false};
    default:
        return std::nullopt;
    }
}

std::uint64_t le64(std::string_view bytes, std::size_t at)
{
    return std::uint64_t{le32(bytes, at)} | (std::uint64_t{le32(bytes, at + 4)} << 32);
}

std::size_t imageBytes(const VkFormat& format, std::uint32_t width, std::uint32_t height)
{
    if (format.block)
        return blockImageBytes(*format.block, width, height);
    return std::size_t{width} * height * format.channels;
}

Image decodeImageData(const VkFormat& format, std::span<const std::uint8_t> data, std::uint32_t width,
                      std::uint32_t height)
{
    if (format.block)
        return decodeBlocks(*format.block, data, width, height);
    Image image = blankImage(width, height);
    const std::size_t pixels = std::size_t{width} * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* in = data.data() + i * format.channels;
        std::uint8_t* out = image.rgba.data() + i * 4;
        switch (format.channels) {
        case 1:
            out[0] = out[1] = out[2] = in[0];
            out[3] = 255;
            break;
        case 2:
            out[0] = in[0];
            out[1] = in[1];
            out[2] = 0;
            out[3] = 255;
            break;
        default:
            out[0] = in[format.bgra ? 2 : 0];
            out[1] = in[1];
            out[2] = in[format.bgra ? 0 : 2];
            out[3] = in[3];
            break;
        }
    }
    return image;
}

} // namespace

Result<TextureFile> decodeKtx2File(std::string_view bytes, DecodeOptions options)
{
    if (bytes.size() < 80)
        return std::unexpected("KTX2 header is truncated");
    const std::uint32_t formatId = le32(bytes, 12);
    const std::uint32_t width = le32(bytes, 20);
    const std::uint32_t height = std::max(1u, le32(bytes, 24));
    const std::uint32_t depth = le32(bytes, 28);
    const std::uint32_t arrayLayers = std::max(1u, le32(bytes, 32));
    const std::uint32_t faces = le32(bytes, 36);
    const std::uint32_t levels = std::max(1u, le32(bytes, 40));
    const std::uint32_t supercompression = le32(bytes, 44);
    if (formatId == 0)
        return std::unexpected("Basis Universal KTX2 textures aren't supported");
    const auto format = vkFormat(formatId);
    if (!format)
        return std::unexpected(std::format("KTX2 Vulkan format {} isn't supported", formatId));
    if (depth > 1)
        return std::unexpected("3D KTX2 textures aren't supported");
    if (faces != 1 && faces != 6)
        return std::unexpected("KTX2 face count must be 1 or 6");
    if (supercompression != 0 && supercompression != 2)
        return std::unexpected(std::format("KTX2 supercompression {} isn't supported", supercompression));
    if (auto size = checkSize(width, height); !size)
        return std::unexpected(size.error());
    if (levels > 32 || arrayLayers > 2048)
        return std::unexpected("KTX2 header is invalid");
    const std::size_t indexEnd = 80 + std::size_t{levels} * 24;
    if (bytes.size() < indexEnd)
        return std::unexpected("KTX2 level index is truncated");

    TextureFile file;
    file.format = ImageFormat::Ktx2;
    file.pixelFormat = std::format("{}{}", format->name, supercompression == 2 ? ", zstd" : "");
    file.cubemap = faces == 6;
    file.fileMips = levels;
    file.mips = options.allMips ? levels : 1;
    file.layers = options.allLayers ? arrayLayers * faces : 1;
    // images[layer * mips + mip], but the file stores level by level.
    file.images.resize(std::size_t{file.layers} * file.mips);
    // A plain 2D texture the GPU can sample compressed keeps its blocks too,
    // every level the file has (as far as they're intact).
    const std::optional<BlockCompressed::Format> gpu = format->block ? gpuFormat(*format->block) : std::nullopt;
    std::shared_ptr<BlockCompressed> blocks;
    if (gpu && faces == 1 && arrayLayers == 1) {
        blocks = std::make_shared<BlockCompressed>();
        blocks->format = *gpu;
    }
    const std::uint32_t readLevels = blocks ? levels : file.mips;
    for (std::uint32_t level = 0; level < readLevels; ++level) {
        // Past the levels decoded, a broken level only ends the kept blocks.
        const bool decoded = level < file.mips;
        auto fail = [&](std::string error) -> std::optional<Result<TextureFile>> {
            if (decoded)
                return std::unexpected(std::move(error));
            return std::nullopt;
        };
        const std::uint64_t offset = le64(bytes, 80 + std::size_t{level} * 24);
        const std::uint64_t length = le64(bytes, 88 + std::size_t{level} * 24);
        const std::uint64_t unpacked = le64(bytes, 96 + std::size_t{level} * 24);
        if (offset > bytes.size() || length > bytes.size() - offset) {
            if (auto error = fail("KTX2 level data is truncated"))
                return std::move(*error);
            break;
        }
        const auto stored
            = std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()) + offset, static_cast<std::size_t>(length));
        const std::uint32_t w = std::max(1u, width >> level);
        const std::uint32_t h = std::max(1u, height >> level);
        const std::size_t faceBytes = imageBytes(*format, w, h);
        const std::size_t needed = faceBytes * arrayLayers * faces;
        std::vector<std::uint8_t> inflated;
        std::span<const std::uint8_t> data = stored;
        if (supercompression == 2) {
            if (unpacked < needed || unpacked > (std::size_t{1} << 30)) {
                if (auto error = fail("KTX2 level has the wrong size"))
                    return std::move(*error);
                break;
            }
            auto out = zstdDecompress(stored, static_cast<std::size_t>(unpacked));
            if (!out) {
                if (auto error = fail(std::format("KTX2 level {}: {}", level, out.error())))
                    return std::move(*error);
                break;
            }
            inflated = std::move(*out);
            data = inflated;
        }
        if (data.size() < needed) {
            if (auto error = fail("KTX2 level data is truncated"))
                return std::move(*error);
            break;
        }
        if (blocks)
            blocks->levels.emplace_back(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(faceBytes));
        for (std::uint32_t layer = 0; decoded && layer < file.layers; ++layer)
            file.images[std::size_t{layer} * file.mips + level]
                = decodeImageData(*format, data.subspan(faceBytes * layer, faceBytes), w, h);
    }
    if (blocks && !blocks->levels.empty() && !file.images.empty())
        file.images.front().compressed = std::move(blocks);
    return file;
}

} // namespace lunar::assets::detail
