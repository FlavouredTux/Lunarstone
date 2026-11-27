#include "ImageCodecs.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace lunar::assets::detail {

namespace {

std::uint8_t maskedValue(std::uint32_t value, std::uint32_t mask)
{
    if (mask == 0)
        return 0;
    const int shift = std::countr_zero(mask);
    const std::uint64_t max = mask >> shift;
    const std::uint64_t channel = (value & mask) >> shift;
    return static_cast<std::uint8_t>((channel * 255 + max / 2) / max);
}

using Color = std::array<std::uint8_t, 4>;

// RLE8 and RLE4 into palette indices; pixels the stream skips stay transparent.
Result<void> decodeBmpRle(std::string_view data, unsigned bits, const std::vector<Color>& palette, Image& image,
                          bool bottomUp)
{
    std::size_t at = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    auto put = [&](unsigned index) {
        if (x < image.width && y < image.height) {
            const std::uint32_t row = bottomUp ? image.height - 1 - y : y;
            const Color color = index < palette.size() ? palette[index] : Color{0, 0, 0, 255};
            std::ranges::copy(color, image.rgba.data() + (std::size_t{row} * image.width + x) * 4);
        }
        ++x;
    };
    while (true) {
        if (data.size() - at < 2)
            return std::unexpected("BMP RLE data is truncated");
        const unsigned count = byteAt(data, at);
        const unsigned value = byteAt(data, at + 1);
        at += 2;
        if (count > 0) {
            for (unsigned i = 0; i < count; ++i)
                put(bits == 8 ? value : (i % 2 == 0 ? value >> 4 : value & 15));
            continue;
        }
        if (value == 0) {
            x = 0;
            ++y;
        } else if (value == 1) {
            return {};
        } else if (value == 2) {
            if (data.size() - at < 2)
                return std::unexpected("BMP RLE data is truncated");
            x += byteAt(data, at);
            y += byteAt(data, at + 1);
            at += 2;
        } else {
            const std::size_t bytes = bits == 8 ? value : (value + 1) / 2;
            const std::size_t padded = (bytes + 1) & ~std::size_t{1};
            if (data.size() - at < padded)
                return std::unexpected("BMP RLE data is truncated");
            for (unsigned i = 0; i < value; ++i) {
                const unsigned byte = byteAt(data, at + (bits == 8 ? i : i / 2));
                put(bits == 8 ? byte : (i % 2 == 0 ? byte >> 4 : byte & 15));
            }
            at += padded;
        }
        if (y > image.height)
            return {}; // past the last row: nothing more can land
    }
}

} // namespace

Result<TextureFile> decodeBmpFile(std::string_view bytes)
{
    if (!bytes.starts_with("BM") || bytes.size() < 26)
        return std::unexpected("not a BMP file");
    const std::uint32_t dataOffset = le32(bytes, 10);
    const std::uint32_t headerSize = le32(bytes, 14);
    if (headerSize < 12 || headerSize > bytes.size() - 14)
        return std::unexpected("BMP header is truncated");
    std::int64_t width = 0;
    std::int64_t height = 0;
    unsigned bits = 0;
    std::uint32_t compression = 0;
    std::uint32_t colorsUsed = 0;
    std::size_t paletteEntry = 4;
    std::array<std::uint32_t, 4> masks{};
    bool hasMasks = false;
    if (headerSize == 12) {
        width = le16(bytes, 18);
        height = le16(bytes, 20);
        bits = le16(bytes, 24);
        paletteEntry = 3;
    } else {
        // OS/2 2.x headers may stop after any field; the rest count as zero.
        if (headerSize < 16)
            return std::unexpected("BMP header is truncated");
        auto field = [&](std::size_t offset) { return offset + 4 <= headerSize ? le32(bytes, 14 + offset) : 0u; };
        width = static_cast<std::int32_t>(le32(bytes, 18));
        height = static_cast<std::int32_t>(le32(bytes, 22));
        bits = le16(bytes, 28);
        compression = field(16);
        colorsUsed = field(32);
        if ((headerSize == 16 || headerSize == 64) && compression >= 3)
            return std::unexpected("OS/2 Huffman and RLE24 BMPs aren't supported");
        if (compression == 3 || compression == 6) {
            // Masks follow a 40-byte header, or sit inside a bigger one.
            const std::size_t count = compression == 6 || headerSize >= 56 ? 4 : 3;
            const std::size_t maskAt = 14 + 40;
            if (bytes.size() < maskAt + count * 4)
                return std::unexpected("BMP bit fields are truncated");
            for (std::size_t i = 0; i < count; ++i)
                masks[i] = le32(bytes, maskAt + i * 4);
            hasMasks = true;
        }
    }
    const bool bottomUp = height > 0;
    height = height < 0 ? -height : height;
    if (width <= 0)
        return std::unexpected("BMP has no pixels");
    if (auto size = checkSize(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)); !size)
        return std::unexpected(size.error());
    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);
    if (compression == 4 || compression == 5)
        return std::unexpected("BMP with embedded JPEG or PNG isn't supported");
    const bool rleMismatch = (compression == 1 && bits != 8) || (compression == 2 && bits != 4);
    if (compression > 6 || rleMismatch)
        return std::unexpected(std::format("BMP compression {} with {} bits isn't supported", compression, bits));

    std::vector<Color> palette;
    if (bits <= 8) {
        if (bits != 1 && bits != 2 && bits != 4 && bits != 8)
            return std::unexpected(std::format("BMP with {} bits a pixel isn't supported", bits));
        std::size_t count = colorsUsed != 0 && colorsUsed <= (1u << bits) ? colorsUsed : (1u << bits);
        std::size_t paletteAt = 14 + std::size_t{headerSize} + (hasMasks && headerSize == 40 ? 12 : 0);
        if (paletteAt > bytes.size())
            return std::unexpected("BMP palette is truncated");
        count = std::min(count, (bytes.size() - paletteAt) / paletteEntry);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t at = paletteAt + i * paletteEntry;
            palette.push_back({byteAt(bytes, at + 2), byteAt(bytes, at + 1), byteAt(bytes, at), 255});
        }
        if (palette.empty())
            return std::unexpected("BMP palette is missing");
    } else if (!hasMasks) {
        if (bits == 16)
            masks = {0x7c00, 0x03e0, 0x001f, 0};
        else if (bits == 24 || bits == 32)
            masks = {0xff0000, 0xff00, 0xff, 0}; // BI_RGB's fourth byte isn't alpha
        else
            return std::unexpected(std::format("BMP with {} bits a pixel isn't supported", bits));
    } else if (bits != 16 && bits != 32) {
        return std::unexpected("BMP bit fields need 16 or 32 bits a pixel");
    }

    if (dataOffset > bytes.size())
        return std::unexpected("BMP pixel data is missing");
    const std::string_view data = bytes.substr(dataOffset);
    Image image = blankImage(w, h);
    if (compression == 1 || compression == 2) {
        if (auto rle = decodeBmpRle(data, bits, palette, image, bottomUp); !rle)
            return std::unexpected(rle.error());
        return singleImage(ImageFormat::Bmp, std::format("{}-bit RLE", bits), std::move(image));
    }

    const std::size_t stride = (std::size_t{w} * bits + 31) / 32 * 4;
    if (stride * h > data.size())
        return std::unexpected("BMP pixel data is truncated");
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::string_view row = data.substr(std::size_t{bottomUp ? h - 1 - y : y} * stride, stride);
        std::uint8_t* out = image.rgba.data() + std::size_t{y} * w * 4;
        for (std::uint32_t x = 0; x < w; ++x, out += 4) {
            if (bits <= 8) {
                const std::size_t bit = std::size_t{x} * bits;
                const auto shift = static_cast<unsigned>(8 - bits - bit % 8);
                const unsigned index = (unsigned{byteAt(row, bit / 8)} >> shift) & ((1u << bits) - 1);
                const Color color = index < palette.size() ? palette[index] : Color{0, 0, 0, 255};
                std::ranges::copy(color, out);
                continue;
            }
            const std::size_t at = std::size_t{x} * (bits / 8);
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < bits / 8; ++i)
                value |= std::uint32_t{byteAt(row, at + i)} << (8 * i);
            for (std::size_t c = 0; c < 3; ++c)
                out[c] = maskedValue(value, masks[c]);
            out[3] = masks[3] ? maskedValue(value, masks[3]) : 255;
        }
    }
    return singleImage(ImageFormat::Bmp, std::format("{}-bit{}", bits, hasMasks ? " bit fields" : ""),
                       std::move(image));
}

bool plausibleTga(std::string_view bytes)
{
    if (bytes.size() < 18)
        return false;
    const unsigned mapType = byteAt(bytes, 1);
    const unsigned type = byteAt(bytes, 2);
    const unsigned mapEntry = byteAt(bytes, 7);
    const unsigned bits = byteAt(bytes, 16);
    if (mapType > 1 || le16(bytes, 12) == 0 || le16(bytes, 14) == 0 || (byteAt(bytes, 17) & 0xc0) != 0)
        return false;
    switch (type & ~8u) {
    case 1:
        return mapType == 1 && (bits == 8 || bits == 16)
               && (mapEntry == 15 || mapEntry == 16 || mapEntry == 24 || mapEntry == 32) && type <= 11;
    case 2:
        return bits == 15 || bits == 16 || bits == 24 || bits == 32;
    case 3:
        return bits == 8 || bits == 16;
    default:
        return false;
    }
}

Result<TextureFile> decodeTgaFile(std::string_view bytes)
{
    if (!plausibleTga(bytes))
        return std::unexpected("not a TGA file");
    const std::size_t idLength = byteAt(bytes, 0);
    const unsigned mapType = byteAt(bytes, 1);
    const unsigned type = byteAt(bytes, 2);
    const std::size_t mapFirst = le16(bytes, 3);
    const std::size_t mapLength = le16(bytes, 5);
    const unsigned mapEntry = byteAt(bytes, 7);
    const std::uint32_t width = le16(bytes, 12);
    const std::uint32_t height = le16(bytes, 14);
    const unsigned bits = byteAt(bytes, 16);
    const unsigned descriptor = byteAt(bytes, 17);
    const bool alphaBits = (descriptor & 15) != 0;
    const bool rle = type >= 9;
    const unsigned kind = type & ~8u;

    std::size_t at = 18 + idLength;
    auto decodeColor = [&](std::uint32_t value, unsigned colorBits, bool gray) -> Color {
        if (gray)
            return colorBits == 16 ? Color{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value),
                                           static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)}
                                   : Color{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value),
                                           static_cast<std::uint8_t>(value), 255};
        if (colorBits == 15 || colorBits == 16)
            return {maskedValue(value, 0x7c00), maskedValue(value, 0x03e0), maskedValue(value, 0x001f),
                    colorBits == 16 && alphaBits ? maskedValue(value, 0x8000) : std::uint8_t{255}};
        return {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value),
                colorBits == 32 ? static_cast<std::uint8_t>(value >> 24) : std::uint8_t{255}};
    };
    auto readValue = [&](std::size_t from, unsigned byteCount) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < byteCount; ++i)
            value |= std::uint32_t{byteAt(bytes, from + i)} << (8 * i);
        return value;
    };

    std::vector<Color> palette;
    if (mapType == 1) {
        const unsigned entryBytes = (mapEntry + 7) / 8;
        if (mapLength * entryBytes > bytes.size() - std::min(bytes.size(), at))
            return std::unexpected("TGA colour map is truncated");
        for (std::size_t i = 0; i < mapLength; ++i)
            palette.push_back(decodeColor(readValue(at + i * entryBytes, entryBytes), mapEntry, false));
        at += mapLength * entryBytes;
    }
    if (auto size = checkSize(width, height); !size)
        return std::unexpected(size.error());

    const unsigned pixelBytes = (bits + 7) / 8;
    auto pixelColor = [&](std::size_t from) -> Color {
        const std::uint32_t value = readValue(from, pixelBytes);
        if (kind == 1) {
            const std::size_t index = value - std::min<std::size_t>(value, mapFirst);
            return index < palette.size() && value >= mapFirst ? palette[index] : Color{0, 0, 0, 255};
        }
        return decodeColor(value, bits, kind == 3);
    };

    const std::size_t pixels = std::size_t{width} * height;
    std::vector<Color> decoded;
    decoded.reserve(std::min(pixels, bytes.size()));
    if (!rle) {
        if (at > bytes.size() || pixels * pixelBytes > bytes.size() - at)
            return std::unexpected("TGA pixel data is truncated");
        for (std::size_t i = 0; i < pixels; ++i)
            decoded.push_back(pixelColor(at + i * pixelBytes));
    } else {
        while (decoded.size() < pixels) {
            if (at >= bytes.size())
                return std::unexpected("TGA RLE data is truncated");
            const unsigned packet = byteAt(bytes, at++);
            const std::size_t count = std::min<std::size_t>((packet & 0x7f) + 1, pixels - decoded.size());
            if (packet & 0x80) {
                if (bytes.size() - at < pixelBytes)
                    return std::unexpected("TGA RLE data is truncated");
                const Color color = pixelColor(at);
                at += pixelBytes;
                decoded.insert(decoded.end(), count, color);
            } else {
                if ((bytes.size() - at) / pixelBytes < count)
                    return std::unexpected("TGA RLE data is truncated");
                for (std::size_t i = 0; i < count; ++i)
                    decoded.push_back(pixelColor(at + i * pixelBytes));
                at += count * pixelBytes;
            }
        }
    }

    // Files that don't declare alpha bits but leave the byte zero would vanish.
    const bool usesAlpha = alphaBits || std::ranges::any_of(decoded, [](const Color& c) { return c[3] != 0; });
    Image image = blankImage(width, height);
    const bool topDown = (descriptor & 0x20) != 0;
    const bool rightToLeft = (descriptor & 0x10) != 0;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            Color color = decoded[std::size_t{y} * width + x];
            if (!usesAlpha)
                color[3] = 255;
            const std::uint32_t outY = topDown ? y : height - 1 - y;
            const std::uint32_t outX = rightToLeft ? width - 1 - x : x;
            std::ranges::copy(color, image.rgba.data() + (std::size_t{outY} * width + outX) * 4);
        }
    static constexpr std::array<std::string_view, 4> kinds{"", "colour-mapped", "true-colour", "gray"};
    return singleImage(ImageFormat::Tga, std::format("{} {}-bit{}", kinds[kind], bits, rle ? " RLE" : ""),
                       std::move(image));
}

} // namespace lunar::assets::detail
