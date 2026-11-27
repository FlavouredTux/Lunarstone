#include <lunar/render/Image.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace lunar::render {

namespace {

constexpr std::array<std::uint32_t, 256> crcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}();

void putU32(std::string& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::span<const std::uint8_t> asBytes(std::string_view s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

void putChunk(std::string& out, std::string_view type, std::string_view data)
{
    putU32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out += type;
    out += data;
    putU32(out, crc32(asBytes(std::string_view(out).substr(start))));
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc)
{
    crc = ~crc;
    for (std::uint8_t byte : bytes)
        crc = crcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32(std::span<const std::uint8_t> bytes, std::uint32_t adler)
{
    std::uint32_t a = adler & 0xFFFF;
    std::uint32_t b = adler >> 16;
    for (std::uint8_t byte : bytes) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::string encodePpm(const PixelImage& image)
{
    std::string out = std::format("P6\n{} {}\n255\n", image.width, image.height);
    out.reserve(out.size() + image.pixels.size() * 3);
    for (const Rgba& p : image.pixels) {
        out.push_back(static_cast<char>(p.r));
        out.push_back(static_cast<char>(p.g));
        out.push_back(static_cast<char>(p.b));
    }
    return out;
}

std::string encodePng(const PixelImage& image)
{
    // Raw scanlines, each prefixed with filter type 0 (none).
    std::string raw;
    raw.reserve(image.pixels.size() * 4 + image.height);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        raw.push_back('\0');
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const Rgba p = image.at(x, y);
            raw += {static_cast<char>(p.r), static_cast<char>(p.g), static_cast<char>(p.b), static_cast<char>(p.a)};
        }
    }

    // zlib stream of stored deflate blocks (at most 65535 bytes each).
    std::string zlib = "\x78\x01";
    constexpr std::size_t maxBlock = 65535;
    std::size_t offset = 0;
    do {
        const std::size_t length = std::min(maxBlock, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib.push_back(last ? '\x01' : '\x00');
        const auto len = static_cast<std::uint16_t>(length);
        const auto nlen = static_cast<std::uint16_t>(~len);
        zlib += {static_cast<char>(len & 0xFF), static_cast<char>(len >> 8), static_cast<char>(nlen & 0xFF),
                 static_cast<char>(nlen >> 8)};
        zlib.append(raw, offset, length);
        offset += length;
    } while (offset < raw.size());
    putU32(zlib, adler32(asBytes(raw)));

    std::string header;
    putU32(header, image.width);
    putU32(header, image.height);
    header += {'\x08', '\x06', '\x00', '\x00', '\x00'}; // 8-bit RGBA, deflate, no filter set, no interlace

    std::string out = "\x89PNG\r\n\x1a\n";
    putChunk(out, "IHDR", header);
    putChunk(out, "IDAT", zlib);
    putChunk(out, "IEND", "");
    return out;
}

} // namespace lunar::render
