#include "framework.hpp"

#include <lunar/assets/ImageDecode.hpp>
#include <lunar/assets/Inflate.hpp>
#include <lunar/render/Image.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::assets;

namespace {

// A 16x12 swirl, encoded by libjpeg-turbo's cjpeg: 4:2:0 baseline, and
// progressive with a restart marker after every MCU.
constexpr unsigned char s420Jpeg[] = {
    255, 216, 255, 224, 0,   16,  74,  70,  73,  70,  0,   1,   1,   0,   0,   1,   0,   1,   0,   0,   255, 219, 0,
    67,  0,   3,   2,   2,   3,   2,   2,   3,   3,   3,   3,   4,   3,   3,   4,   5,   8,   5,   5,   4,   4,   5,
    10,  7,   7,   6,   8,   12,  10,  12,  12,  11,  10,  11,  11,  13,  14,  18,  16,  13,  14,  17,  14,  11,  11,
    16,  22,  16,  17,  19,  20,  21,  21,  21,  12,  15,  23,  24,  22,  20,  24,  18,  20,  21,  20,  255, 219, 0,
    67,  1,   3,   4,   4,   5,   4,   5,   9,   5,   5,   9,   20,  13,  11,  13,  20,  20,  20,  20,  20,  20,  20,
    20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,
    20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  255, 192, 0,
    17,  8,   0,   12,  0,   16,  3,   1,   34,  0,   2,   17,  1,   3,   17,  1,   255, 196, 0,   31,  0,   0,   1,
    5,   1,   1,   1,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   1,   2,   3,   4,   5,   6,   7,   8,
    9,   10,  11,  255, 196, 0,   181, 16,  0,   2,   1,   3,   3,   2,   4,   3,   5,   5,   4,   4,   0,   0,   1,
    125, 1,   2,   3,   0,   4,   17,  5,   18,  33,  49,  65,  6,   19,  81,  97,  7,   34,  113, 20,  50,  129, 145,
    161, 8,   35,  66,  177, 193, 21,  82,  209, 240, 36,  51,  98,  114, 130, 9,   10,  22,  23,  24,  25,  26,  37,
    38,  39,  40,  41,  42,  52,  53,  54,  55,  56,  57,  58,  67,  68,  69,  70,  71,  72,  73,  74,  83,  84,  85,
    86,  87,  88,  89,  90,  99,  100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132,
    133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169,
    170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213,
    214, 215, 216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248,
    249, 250, 255, 196, 0,   31,  1,   0,   3,   1,   1,   1,   1,   1,   1,   1,   1,   1,   0,   0,   0,   0,   0,
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  255, 196, 0,   181, 17,  0,   2,   1,   2,   4,   4,
    3,   4,   7,   5,   4,   4,   0,   1,   2,   119, 0,   1,   2,   3,   17,  4,   5,   33,  49,  6,   18,  65,  81,
    7,   97,  113, 19,  34,  50,  129, 8,   20,  66,  145, 161, 177, 193, 9,   35,  51,  82,  240, 21,  98,  114, 209,
    10,  22,  36,  52,  225, 37,  241, 23,  24,  25,  26,  38,  39,  40,  41,  42,  53,  54,  55,  56,  57,  58,  67,
    68,  69,  70,  71,  72,  73,  74,  83,  84,  85,  86,  87,  88,  89,  90,  99,  100, 101, 102, 103, 104, 105, 106,
    115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151,
    152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195,
    196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228, 229, 230, 231, 232,
    233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250, 255, 218, 0,   12,  3,   1,   0,   2,   17,  3,   17,  0,
    63,  0,   249, 69,  60,  68,  93,  73,  15,  250, 213, 113, 226, 179, 188, 169, 56,  175, 55,  26,  149, 194, 244,
    122, 111, 219, 230, 206, 119, 115, 95,  212, 148, 124, 17,  203, 34,  231, 237, 57,  90,  123, 104, 244, 63,  98,
    171, 226, 254, 97,  37,  30,  78,  100, 214, 254, 103, 255, 217};
constexpr unsigned char sprogJpeg[] = {
    255, 216, 255, 224, 0,   16,  74,  70,  73,  70,  0,   1,   1,   0,   0,   1,   0,   1,   0,   0,   255, 219, 0,
    67,  0,   3,   2,   2,   3,   2,   2,   3,   3,   3,   3,   4,   3,   3,   4,   5,   8,   5,   5,   4,   4,   5,
    10,  7,   7,   6,   8,   12,  10,  12,  12,  11,  10,  11,  11,  13,  14,  18,  16,  13,  14,  17,  14,  11,  11,
    16,  22,  16,  17,  19,  20,  21,  21,  21,  12,  15,  23,  24,  22,  20,  24,  18,  20,  21,  20,  255, 219, 0,
    67,  1,   3,   4,   4,   5,   4,   5,   9,   5,   5,   9,   20,  13,  11,  13,  20,  20,  20,  20,  20,  20,  20,
    20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,
    20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  20,  255, 194, 0,
    17,  8,   0,   12,  0,   16,  3,   1,   34,  0,   2,   17,  1,   3,   17,  1,   255, 196, 0,   23,  0,   0,   3,
    1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   3,   6,   7,   255, 196, 0,   21,  1,
    1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   6,   7,   255, 221, 0,   4,   0,
    1,   255, 218, 0,   12,  3,   1,   0,   2,   16,  3,   16,  0,   0,   1,   202, 23,  54,  42,  76,  127, 255, 196,
    0,   25,  16,  1,   0,   2,   3,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   20,  2,
    18,  19,  255, 221, 0,   4,   0,   2,   255, 218, 0,   8,   1,   1,   0,   1,   5,   2,   176, 180, 255, 208, 233,
    147, 121, 127, 255, 196, 0,   25,  17,  0,   1,   5,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   17,  0,   1,   2,   4,   6,   255, 221, 0,   4,   0,   1,   255, 218, 0,   8,   1,   3,   1,   1,   63,  1,
    150, 190, 195, 128, 87,  255, 196, 0,   26,  17,  0,   1,   5,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   17,  0,   1,   2,   4,   6,   18,  255, 218, 0,   8,   1,   2,   1,   1,   63,  1,   134, 34,  179,
    30,  130, 255, 196, 0,   20,  16,  1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    16,  255, 221, 0,   4,   0,   2,   255, 218, 0,   8,   1,   1,   0,   6,   63,  2,   63,  255, 208, 63,  255, 196,
    0,   25,  16,  0,   2,   3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   17,  1,   49,
    65,  97,  255, 218, 0,   8,   1,   1,   0,   1,   63,  33,  135, 139, 46,  143, 255, 208, 86,  157, 207, 255, 221,
    0,   4,   0,   1,   255, 218, 0,   12,  3,   1,   0,   2,   0,   3,   0,   0,   0,   16,  3,   255, 196, 0,   24,
    17,  0,   2,   3,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   33,  49,  97,  255,
    218, 0,   8,   1,   3,   1,   1,   63,  16,  89,  9,   171, 211, 255, 196, 0,   23,  17,  1,   1,   1,   1,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   33,  0,   49,  255, 218, 0,   8,   1,   2,   1,   1,
    63,  16,  74,  193, 228, 102, 255, 196, 0,   25,  16,  1,   0,   3,   1,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   1,   0,   17,  49,  33,  161, 255, 221, 0,   4,   0,   2,   255, 218, 0,   8,   1,   1,   0,
    1,   63,  16,  102, 79,  112, 119, 77,  79,  255, 208, 48,  79,  107, 212, 255, 217};

// FNV-1a of the RGB bytes djpeg produces for both files.
constexpr std::uint64_t swirlDjpegHash = 0xca61ed2f62cbb4a4ull;

std::string_view bytesOf(std::span<const unsigned char> data)
{
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::uint64_t rgbHash(const Image& image)
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < image.rgba.size(); ++i)
        if (i % 4 != 3)
            hash = (hash ^ image.rgba[i]) * 0x100000001b3ull;
    return hash;
}

render::PixelImage testPattern(std::uint32_t width, std::uint32_t height)
{
    render::PixelImage image;
    image.width = width;
    image.height = height;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            image.pixels.push_back({static_cast<std::uint8_t>(x * 17), static_cast<std::uint8_t>(y * 29),
                                    static_cast<std::uint8_t>((x ^ y) * 7), static_cast<std::uint8_t>(255 - x * y)});
    return image;
}

bool samePixels(const Image& image, const render::PixelImage& expected)
{
    if (image.width != expected.width || image.height != expected.height)
        return false;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        const render::Rgba p = expected.pixels[i];
        if (image.rgba[i * 4] != p.r || image.rgba[i * 4 + 1] != p.g || image.rgba[i * 4 + 2] != p.b
            || image.rgba[i * 4 + 3] != p.a)
            return false;
    }
    return true;
}

std::string le32(std::uint32_t v)
{
    return {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
}

// A legacy DDS header for a FourCC format.
std::string ddsHeader(std::uint32_t width, std::uint32_t height, std::string_view fourCc, std::uint32_t mips = 1)
{
    std::string h = "DDS " + le32(124) + le32(0x1007 | (mips > 1 ? 0x20000u : 0u)) + le32(height) + le32(width)
                    + le32(0) + le32(0) + le32(mips);
    h.append(44, '\0');
    h += le32(32) + le32(4) + std::string(fourCc) + std::string(20, '\0');
    h += le32(0x1000) + std::string(16, '\0');
    return h;
}

// Flips bytes at random and checks nothing worse than an error happens.
template <class Decode>
void fuzz(std::string_view bytes, Decode decode, int rounds = 2000)
{
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (std::size_t length = 0; length < bytes.size(); length += 1 + bytes.size() / 64)
        (void)decode(bytes.substr(0, length));
    for (int round = 0; round < rounds; ++round) {
        std::string corrupted(bytes);
        const int flips = 1 + static_cast<int>(next() % 4);
        for (int i = 0; i < flips; ++i)
            corrupted[next() % corrupted.size()] = static_cast<char>(next());
        auto image = decode(corrupted);
        if (image)
            CHECK_EQ(image->rgba.size(), image->byteSize());
    }
}

} // namespace

TEST(inflate_reads_stored_fixed_and_dynamic_blocks)
{
    // zlib.compress(b"hello hello hello hello", level) for levels 0 and 9.
    const std::array<std::uint8_t, 34> stored{0x78, 0x01, 0x01, 0x17, 0x00, 0xe8, 0xff, 'h',  'e',  'l', 'l', 'o',
                                              ' ',  'h',  'e',  'l',  'l',  'o',  ' ',  'h',  'e',  'l', 'l', 'o',
                                              ' ',  'h',  'e',  'l',  'l',  'o',  0x68, 0x03, 0x08, 0xb1};
    auto out = zlibDecompress(stored);
    CHECK(out.has_value());
    CHECK_EQ(std::string(out->begin(), out->end()), "hello hello hello hello");
    const std::array<std::uint8_t, 18> fixed{0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8,
                                             0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1, 0,    0};
    out = zlibDecompress(std::span(fixed).first(16));
    CHECK(out.has_value());
    CHECK_EQ(std::string(out->begin(), out->end()), "hello hello hello hello");

    // A wrong checksum, a bad header and truncation are errors.
    auto bad = fixed;
    bad[15] ^= 1;
    CHECK_EQ(zlibDecompress(std::span(bad).first(16)).error(), "zlib checksum mismatch");
    CHECK(!zlibDecompress(std::span(fixed).first(10)).has_value());
    bad = fixed;
    bad[1] = 0;
    CHECK(!zlibDecompress(std::span(bad).first(16)).has_value());
    // More output than allowed.
    CHECK(!zlibDecompress(std::span(fixed).first(16), {.sizeHint = 0, .maxSize = 10}).has_value());
    CHECK_EQ(adler32(std::span(reinterpret_cast<const std::uint8_t*>("Wikipedia"), 9)), 0x11E60398u);
}

TEST(inflate_reads_stored_blocks_between_compressed_ones)
{
    // Raw deflate from Python's zlib: "abc" * 12 at level 9 and a full
    // flush, 40 stored bytes "A".."h" and a flush, then "xyz" * 8. The
    // reader loads eight bytes at a time and copies stored blocks straight
    // from the input; the block after one must read on from its end.
    const std::array<std::uint8_t, 69> raw{
        0x4a, 0x4c, 0x4a, 0x4e, 0x24, 0x84, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x28, 0x00, 0xd7, 0xff, 0x41,
        0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53,
        0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65,
        0x66, 0x67, 0x68, 0x00, 0x00, 0x00, 0xff, 0xff, 0xab, 0xa8, 0xac, 0xaa, 0xc0, 0x86, 0x00};
    std::string expected;
    for (int i = 0; i < 12; ++i)
        expected += "abc";
    for (char c = 'A'; c <= 'h'; ++c)
        expected += c;
    for (int i = 0; i < 8; ++i)
        expected += "xyz";
    for (const std::size_t hint : {std::size_t{0}, expected.size()}) {
        auto out = inflate(raw, {.sizeHint = hint, .maxSize = expected.size()});
        CHECK(out.has_value());
        if (out)
            CHECK_EQ(std::string(out->begin(), out->end()), expected);
    }
}

TEST(gunzip_reads_gzip_streams)
{
    // printf 'hello gzip world, hello gzip world\n' | gzip -9 -n, then the
    // same with the file name stored (FNAME).
    const std::vector<std::uint8_t> plain{0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xcb,
                                          0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x48, 0xaf, 0xca, 0x2c, 0x50, 0x28,
                                          0xcf, 0x2f, 0xca, 0x49, 0xd1, 0x51, 0xc8, 0x40, 0x13, 0xe1, 0x02,
                                          0x00, 0x06, 0x69, 0x31, 0xe4, 0x23, 0x00, 0x00, 0x00};
    const std::vector<std::uint8_t> named{0x1f, 0x8b, 0x08, 0x08, 0xf7, 0x45, 0xcb, 0x6a, 0x00, 0x03, 0x67, 0x2e,
                                          0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x48, 0xaf,
                                          0xca, 0x2c, 0x50, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xd1, 0x51, 0xc8, 0x40,
                                          0x13, 0xe1, 0x02, 0x00, 0x06, 0x69, 0x31, 0xe4, 0x23, 0x00, 0x00, 0x00};
    const std::string_view text = "hello gzip world, hello gzip world\n";
    for (const auto* stream : {&plain, &named}) {
        auto out = gunzip(*stream);
        CHECK(out.has_value());
        CHECK_EQ(std::string_view(reinterpret_cast<const char*>(out->data()), out->size()), text);
    }
    auto bad = plain;
    bad[bad.size() - 6] ^= 1; // the CRC
    CHECK_EQ(gunzip(bad).error(), "gzip checksum mismatch");
    CHECK(!gunzip(std::span(plain).first(20)).has_value());
    CHECK(!gunzip(std::span(plain).subspan(1)).has_value());
    CHECK_EQ(crc32(std::span(reinterpret_cast<const std::uint8_t*>("123456789"), 9)), 0xCBF43926u);
}

TEST(png_round_trips_the_renderer_encoder)
{
    for (auto [w, h] : {std::pair{1u, 1u}, {7u, 5u}, {64u, 3u}}) {
        const render::PixelImage expected = testPattern(w, h);
        const std::string png = render::encodePng(expected);
        CHECK_EQ(detectImageFormat(png), ImageFormat::Png);
        auto image = decodePng(png);
        CHECK(image.has_value());
        CHECK(samePixels(*image, expected));
        auto file = decodeTexture(png);
        CHECK_EQ(file->pixelFormat, "RGBA 8-bit");
    }
}

// Every filter on 8-bit rows of every colour type, against the PNG
// specification's own unfiltering, a byte at a time: the 3- and 4-byte pixels
// take the decoder's SIMD paths, the first row the ones with nothing above.
TEST(png_unfilters_every_row_filter)
{
    std::uint64_t state = 0x2545F4914F6CDD1Dull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint8_t>(state >> 32);
    };
    auto chunk = [](std::string_view type, std::string_view data) {
        std::string body = std::string(type) + std::string(data);
        const std::uint32_t crc = crc32(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
        auto be = [](std::uint32_t v) {
            return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                               static_cast<char>(v)};
        };
        return be(static_cast<std::uint32_t>(data.size())) + body + be(crc);
    };
    for (const unsigned colorType : {0u, 2u, 3u, 4u, 6u}) {
        const std::size_t bpp = colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : 1;
        const std::uint32_t width = 13, height = 11;
        const std::size_t stride = width * bpp;
        // Filtered rows of random bytes, each filter in turn (the first row too).
        std::vector<std::uint8_t> filtered, plain(stride * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            const unsigned filter = (y + colorType) % 5;
            filtered.push_back(static_cast<std::uint8_t>(filter));
            for (std::size_t i = 0; i < stride; ++i) {
                const std::uint8_t raw = next();
                filtered.push_back(raw);
                const int a = i >= bpp ? plain[y * stride + i - bpp] : 0;
                const int b = y > 0 ? plain[(y - 1) * stride + i] : 0;
                const int c = y > 0 && i >= bpp ? plain[(y - 1) * stride + i - bpp] : 0;
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                const int predictor = filter == 1   ? a
                                      : filter == 2 ? b
                                      : filter == 3 ? (a + b) / 2
                                      : filter == 4 ? (pa <= pb && pa <= pc ? a
                                                       : pb <= pc           ? b
                                                                            : c)
                                                    : 0;
                plain[y * stride + i] = static_cast<std::uint8_t>(raw + predictor);
            }
        }
        // A zlib stream of one stored block.
        std::string zlib = "\x78\x01\x01";
        const auto length = static_cast<std::uint16_t>(filtered.size());
        zlib += {static_cast<char>(length), static_cast<char>(length >> 8), static_cast<char>(~length),
                 static_cast<char>(~length >> 8)};
        zlib.append(filtered.begin(), filtered.end());
        const std::uint32_t adler = adler32(filtered);
        zlib += {static_cast<char>(adler >> 24), static_cast<char>(adler >> 16), static_cast<char>(adler >> 8),
                 static_cast<char>(adler)};
        std::string header
            = {0, 0, 0, static_cast<char>(width), 0, 0, 0, static_cast<char>(height), 8, static_cast<char>(colorType),
               0, 0, 0};
        std::string png = "\x89PNG\r\n\x1a\n" + chunk("IHDR", header);
        std::string palette;
        for (int i = 0; i < 256; ++i)
            palette += {static_cast<char>(i), static_cast<char>(255 - i), static_cast<char>(i * 3)};
        if (colorType == 3)
            png += chunk("PLTE", palette);
        png += chunk("IDAT", zlib) + chunk("IEND", "");

        auto image = decodePng(png);
        CHECK(image.has_value());
        if (!image)
            continue;
        bool same = image->width == width && image->height == height;
        for (std::size_t px = 0; same && px < std::size_t{width} * height; ++px) {
            const std::uint8_t* s = plain.data() + px * bpp;
            std::array<std::uint8_t, 4> expected{};
            switch (colorType) {
            case 0:
                expected = {s[0], s[0], s[0], 255};
                break;
            case 2:
                expected = {s[0], s[1], s[2], 255};
                break;
            case 3:
                expected = {static_cast<std::uint8_t>(s[0]), static_cast<std::uint8_t>(255 - s[0]),
                            static_cast<std::uint8_t>(s[0] * 3), 255};
                break;
            case 4:
                expected = {s[0], s[0], s[0], s[1]};
                break;
            default:
                expected = {s[0], s[1], s[2], s[3]};
                break;
            }
            same = std::memcmp(image->rgba.data() + px * 4, expected.data(), 4) == 0;
        }
        CHECK(same);
    }
}

TEST(png_rejects_broken_files)
{
    const std::string png = render::encodePng(testPattern(9, 9));
    CHECK_EQ(decodePng("").error(), "not a PNG file");
    CHECK(!decodePng(png.substr(0, png.size() - 20)).has_value());
    std::string wrongDepth = png;
    wrongDepth[24] = 3; // bit depth 3
    CHECK(!decodePng(wrongDepth).has_value());
    std::string huge = png;
    huge[16] = 0x7f; // width ~2 billion
    CHECK(!decodePng(huge).has_value());
    fuzz(png, [](std::string_view b) { return decodePng(b); });
}

TEST(jpeg_matches_libjpeg)
{
    for (std::string_view jpeg : {bytesOf(s420Jpeg), bytesOf(sprogJpeg)}) {
        CHECK_EQ(detectImageFormat(jpeg), ImageFormat::Jpeg);
        auto image = decodeJpeg(jpeg);
        CHECK(image.has_value());
        CHECK_EQ(image->width, 16u);
        CHECK_EQ(image->height, 12u);
        CHECK_EQ(rgbHash(*image), swirlDjpegHash);
        CHECK_EQ(image->rgba[3], 255);
    }
    CHECK_EQ(decodeTexture(bytesOf(sprogJpeg))->pixelFormat, "progressive YCbCr 4:2:0");
}

TEST(jpeg_rejects_broken_files)
{
    const std::string_view jpeg = bytesOf(s420Jpeg);
    CHECK(!decodeJpeg(jpeg.substr(0, jpeg.size() / 2)).has_value());
    CHECK(!decodeJpeg("\xff\xd8\xff\xd9").has_value());
    fuzz(jpeg, [](std::string_view b) { return decodeJpeg(b); });
    fuzz(bytesOf(sprogJpeg), [](std::string_view b) { return decodeJpeg(b); });
}

namespace {

std::string redPngForBlocks()
{
    lunar::render::PixelImage image;
    image.width = 2;
    image.height = 2;
    image.pixels.assign(4, {255, 0, 0, 255});
    return lunar::render::encodePng(image);
}

} // namespace

TEST(dds_bc1_bc3_and_mips)
{
    // One BC1 block: colour 0 pure red, colour 1 pure blue, every pixel index 0
    // except the last, index 1.
    std::string bc1 = ddsHeader(4, 4, "DXT1");
    bc1 += std::string("\x00\xf8\x1f\x00", 4) + std::string("\x00\x00\x00\x40", 4);
    auto file = decodeDds(bc1);
    CHECK(file.has_value());
    const Image& image = file->image();
    CHECK_EQ(image.rgba[0], 255);
    CHECK_EQ(image.rgba[2], 0);
    CHECK_EQ(image.rgba[62], 255);
    CHECK_EQ(image.rgba[60], 0);

    // c0 <= c1 is the three-colour mode, index 3 transparent.
    std::string punch = ddsHeader(4, 4, "DXT1");
    punch += std::string("\x1f\x00\x00\xf8", 4) + std::string("\xff\xff\xff\xff", 4);
    CHECK_EQ(decodeDds(punch)->image().rgba[3], 0);

    // BC3: alpha endpoints 255 and 0, all indices 0; partial edge blocks for 5x5.
    std::string bc3 = ddsHeader(5, 5, "DXT5", 3);
    for (int block = 0; block < 4 + 1 + 1; ++block)
        bc3 += std::string("\xff\x00", 2) + std::string(6, '\0') + std::string("\x00\xf8\x00\xf8", 4)
               + std::string(4, '\0');
    file = decodeDds(bc3, {.allMips = true, .allLayers = true});
    CHECK(file.has_value());
    CHECK_EQ(file->mips, 3u);
    CHECK_EQ(file->image(0, 0).width, 5u);
    CHECK_EQ(file->image(0, 1).width, 2u);
    CHECK_EQ(file->image(0, 2).width, 1u);
    CHECK_EQ(file->image().rgba[3], 255);
    CHECK_EQ(file->image().rgba[0], 255);

    // The blocks stay too, every level of them, for a GPU to sample as is.
    const auto blocks = decodeImage(bc3)->compressed;
    CHECK(blocks != nullptr);
    CHECK(blocks->format == BlockCompressed::Format::Bc3);
    CHECK_EQ(blocks->levels.size(), 3u);
    CHECK_EQ(blocks->levels[0].size(), 64u);
    CHECK_EQ(blocks->levels[2].size(), 16u);
    CHECK_EQ(decodeImage(bc1)->compressed->levels.size(), 1u);
    CHECK(decodeImage(redPngForBlocks())->compressed == nullptr);

    CHECK(!decodeDds(bc3.substr(0, bc3.size() - 1), {.allMips = true, .allLayers = true}).has_value());
    CHECK(!decodeDds(ddsHeader(4, 4, "ABCD") + std::string(8, '\0')).has_value());
    fuzz(bc3, [](std::string_view b) {
        return decodeDds(b).transform([](TextureFile f) { return std::move(f.images.front()); });
    });
}

TEST(bmp_and_tga)
{
    // 2x2 24-bit bottom-up BMP: rows of 6 bytes padded to 8.
    std::string bmp = "BM" + le32(70) + le32(0) + le32(54) + le32(40) + le32(2) + le32(2);
    bmp += std::string("\x01\x00\x18\x00", 4) + le32(0) + le32(16) + std::string(16, '\0');
    bmp += std::string("\x00\x00\xff\x00\xff\x00\x00\x00", 8); // bottom row: red, green
    bmp += std::string("\xff\x00\x00\xff\xff\xff\x00\x00", 8); // top row: blue, white
    auto image = decodeImage(bmp);
    CHECK(image.has_value());
    CHECK_EQ(image->rgba[2], 255);      // top left blue
    CHECK_EQ(image->rgba[4], 255);      // top right white
    CHECK_EQ(image->rgba[12 + 1], 255); // bottom right green
    CHECK_EQ(image->rgba[8], 255);      // bottom left red
    CHECK_EQ(image->rgba[8 + 1], 0);
    CHECK_EQ(image->rgba[3], 255);
    fuzz(bmp, [](std::string_view b) { return decodeBmp(b); });

    // 2x1 RLE true-colour TGA, top-down: one run of two red pixels with alpha.
    std::string tga("\x00\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x01\x00\x20\x28", 18);
    tga += std::string("\x81\x00\x00\xff\x80", 5);
    image = decodeTga(tga);
    CHECK(image.has_value());
    CHECK_EQ(image->width, 2u);
    CHECK_EQ(image->rgba[4], 255);
    CHECK_EQ(image->rgba[7], 128);
    CHECK_EQ(detectImageFormat(tga), ImageFormat::Tga);
    CHECK(!decodeTga(tga.substr(0, 20)).has_value());
    fuzz(tga, [](std::string_view b) { return decodeTga(b); });
}

TEST(decode_texture_explains_non_images)
{
    CHECK(decodeImage("<roblox!\x89\xff").error().contains("Roblox model"));
    CHECK(!decodeImage("").has_value());
    CHECK(!decodeImage("hello").has_value());
}
