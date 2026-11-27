#include "ImageCodecs.hpp"

#include <lunar/assets/Inflate.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace lunar::assets::detail {

namespace {

constexpr std::string_view signature = "\x89PNG\r\n\x1a\n";

struct Header {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    unsigned depth = 0;
    unsigned colorType = 0;
    bool interlaced = false;

    unsigned channels() const
    {
        switch (colorType) {
        case 0:
        case 3:
            return 1;
        case 2:
            return 3;
        case 4:
            return 2;
        default:
            return 4;
        }
    }
    unsigned bitsPerPixel() const { return channels() * depth; }
    std::size_t rowBytes(std::uint32_t pixels) const { return (std::size_t{pixels} * bitsPerPixel() + 7) / 8; }
};

Result<Header> parseHeader(std::string_view data)
{
    if (data.size() != 13)
        return std::unexpected("IHDR has the wrong size");
    Header h;
    h.width = be32(data, 0);
    h.height = be32(data, 4);
    h.depth = byteAt(data, 8);
    h.colorType = byteAt(data, 9);
    if (byteAt(data, 10) != 0 || byteAt(data, 11) != 0)
        return std::unexpected("unknown PNG compression or filter method");
    if (byteAt(data, 12) > 1)
        return std::unexpected("unknown PNG interlace method");
    h.interlaced = byteAt(data, 12) == 1;
    const unsigned d = h.depth;
    bool valid = false;
    switch (h.colorType) {
    case 0:
        valid = d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
        break;
    case 3:
        valid = d == 1 || d == 2 || d == 4 || d == 8;
        break;
    case 2:
    case 4:
    case 6:
        valid = d == 8 || d == 16;
        break;
    default:
        break;
    }
    if (!valid)
        return std::unexpected(std::format("invalid PNG colour type {} with bit depth {}", h.colorType, d));
    if (auto size = checkSize(h.width, h.height); !size)
        return std::unexpected(size.error());
    return h;
}

struct Pass {
    std::uint32_t x0, y0, dx, dy;
};
constexpr std::array<Pass, 7> adam7{
    {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};
constexpr std::array<Pass, 1> progressive{{{0, 0, 1, 1}}};

std::uint32_t passSize(std::uint32_t size, std::uint32_t start, std::uint32_t step)
{
    return size > start ? (size - start + step - 1) / step : 0;
}

std::span<const Pass> passesOf(const Header& h)
{
    return h.interlaced ? std::span<const Pass>(adam7) : std::span<const Pass>(progressive);
}

// Unfiltered scanline bytes needed, filter bytes included.
std::size_t expectedBytes(const Header& h)
{
    std::size_t total = 0;
    for (const Pass& pass : passesOf(h)) {
        const std::uint32_t w = passSize(h.width, pass.x0, pass.dx);
        const std::uint32_t rows = passSize(h.height, pass.y0, pass.dy);
        if (w != 0 && rows != 0)
            total += std::size_t{rows} * (1 + h.rowBytes(w));
    }
    return total;
}

#if defined(__SSE2__)
// Sub, Average and Paeth a pixel at a time for 8-bit RGB and RGBA rows, as
// libpng's SSE2 filters do: the byte loops below carry each byte's result to
// the one bpp later, and these do the pixel's three or four at once, with the
// same arithmetic. Rows of other pixel sizes take the byte loops.
template <std::size_t Bpp>
__m128i loadPixel(const std::uint8_t* p)
{
    std::uint32_t v = 0;
    std::memcpy(&v, p, Bpp);
    return _mm_cvtsi32_si128(static_cast<int>(v));
}
template <std::size_t Bpp>
void storePixel(std::uint8_t* p, __m128i v)
{
    const auto bits = static_cast<std::uint32_t>(_mm_cvtsi128_si32(v));
    std::memcpy(p, &bits, Bpp);
}

template <std::size_t Bpp>
bool unfilterPixels(std::uint8_t* row, const std::uint8_t* previous, std::size_t length, unsigned filter)
{
    if (length % Bpp != 0 || !previous)
        return false;
    const __m128i zero = _mm_setzero_si128();
    switch (filter) {
    case 1: {
        __m128i a = zero;
        for (std::size_t i = 0; i < length; i += Bpp) {
            a = _mm_add_epi8(loadPixel<Bpp>(row + i), a);
            storePixel<Bpp>(row + i, a);
        }
        return true;
    }
    case 3: {
        // (a + b) >> 1: avg_epu8 rounds up, so take the lost bit back off.
        const __m128i one = _mm_set1_epi8(1);
        __m128i a = zero;
        for (std::size_t i = 0; i < length; i += Bpp) {
            const __m128i b = loadPixel<Bpp>(previous + i);
            __m128i average = _mm_avg_epu8(a, b);
            average = _mm_sub_epi8(average, _mm_and_si128(_mm_xor_si128(a, b), one));
            a = _mm_add_epi8(loadPixel<Bpp>(row + i), average);
            storePixel<Bpp>(row + i, a);
        }
        return true;
    }
    case 4: {
        // In 16-bit lanes: a left, b above, c above-left; the first pixel has
        // a = c = 0, which makes it pick b (Up), as the byte loop does.
        auto abs16 = [zero](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
        auto select = [](__m128i mask, __m128i yes, __m128i no) {
            return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
        };
        __m128i a = zero;
        __m128i c = zero;
        for (std::size_t i = 0; i < length; i += Bpp) {
            const __m128i b = _mm_unpacklo_epi8(loadPixel<Bpp>(previous + i), zero);
            const __m128i d = _mm_unpacklo_epi8(loadPixel<Bpp>(row + i), zero);
            // |p - a| = |b - c|, |p - b| = |a - c|, |p - c| = |a + b - 2c| for p = a + b - c.
            const __m128i bc = _mm_sub_epi16(b, c);
            const __m128i ac = _mm_sub_epi16(a, c);
            const __m128i pa = abs16(bc);
            const __m128i pb = abs16(ac);
            const __m128i pc = abs16(_mm_add_epi16(bc, ac));
            const __m128i smallest = _mm_min_epi16(pc, _mm_min_epi16(pa, pb));
            // Ties go to a, then b, then c.
            const __m128i predictor
                = select(_mm_cmpeq_epi16(smallest, pa), a, select(_mm_cmpeq_epi16(smallest, pb), b, c));
            // Both under 256 in every lane: the high bytes stay zero.
            a = _mm_add_epi8(d, predictor);
            storePixel<Bpp>(row + i, _mm_packus_epi16(a, a));
            c = b;
        }
        return true;
    }
    default:
        return false;
    }
}
#endif

Result<void> unfilter(std::uint8_t* row, const std::uint8_t* previous, std::size_t length, std::size_t bpp,
                      unsigned filter)
{
#if defined(__SSE2__)
    if (bpp == 4 && unfilterPixels<4>(row, previous, length, filter))
        return {};
    if (bpp == 3 && unfilterPixels<3>(row, previous, length, filter))
        return {};
#endif
    switch (filter) {
    case 0:
        break;
    case 1:
        for (std::size_t i = bpp; i < length; ++i)
            row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
        break;
    case 2:
        if (previous)
            for (std::size_t i = 0; i < length; ++i)
                row[i] = static_cast<std::uint8_t>(row[i] + previous[i]);
        break;
    // Average and Paeth: the first pixel (nothing to its left) and the first
    // row (nothing above) apart, so the loops over the rest don't ask.
    case 3:
        if (!previous) {
            for (std::size_t i = bpp; i < length; ++i)
                row[i] = static_cast<std::uint8_t>(row[i] + (row[i - bpp] >> 1));
            break;
        }
        for (std::size_t i = 0; i < std::min(bpp, length); ++i)
            row[i] = static_cast<std::uint8_t>(row[i] + (previous[i] >> 1));
        for (std::size_t i = bpp; i < length; ++i)
            row[i] = static_cast<std::uint8_t>(row[i] + ((unsigned{row[i - bpp]} + previous[i]) >> 1));
        break;
    case 4:
        // With nothing above, Paeth picks the left byte: Sub.
        if (!previous) {
            for (std::size_t i = bpp; i < length; ++i)
                row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
            break;
        }
        // With nothing to the left, it picks the byte above: Up.
        for (std::size_t i = 0; i < std::min(bpp, length); ++i)
            row[i] = static_cast<std::uint8_t>(row[i] + previous[i]);
        for (std::size_t i = bpp; i < length; ++i) {
            const int a = row[i - bpp];
            const int b = previous[i];
            const int c = previous[i - bpp];
            // |p - a|, |p - b|, |p - c| for p = a + b - c.
            const int pa = std::abs(b - c);
            const int pb = std::abs(a - c);
            const int pc = std::abs(a + b - c - c);
            const int predictor = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            row[i] = static_cast<std::uint8_t>(row[i] + predictor);
        }
        break;
    default:
        return std::unexpected(std::format("invalid PNG filter type {}", filter));
    }
    return {};
}

struct Palette {
    std::array<std::array<std::uint8_t, 4>, 256> colors{};
    std::size_t size = 0;
};

struct Transparency {
    bool present = false;
    std::array<std::uint16_t, 3> key{}; // gray or RGB at the image's bit depth
};

// One unfiltered scanline to RGBA, every step-th pixel of the output row.
void expandRow(const Header& h, const std::uint8_t* row, std::uint32_t count, std::uint8_t* out, std::size_t step,
               const Palette& palette, const Transparency& trns)
{
    const unsigned depth = h.depth;
    auto sample = [&](std::uint32_t index) -> unsigned {
        // Sample index within the row, at the image's bit depth.
        if (depth == 8)
            return row[index];
        if (depth == 16)
            return (unsigned{row[std::size_t{index} * 2]} << 8) | row[std::size_t{index} * 2 + 1];
        const std::size_t bit = std::size_t{index} * depth;
        const unsigned shift = 8 - depth - static_cast<unsigned>(bit % 8);
        return (row[bit / 8] >> shift) & ((1u << depth) - 1);
    };
    auto to8 = [&](unsigned value) -> std::uint8_t {
        switch (depth) {
        case 1:
            return static_cast<std::uint8_t>(value * 255);
        case 2:
            return static_cast<std::uint8_t>(value * 85);
        case 4:
            return static_cast<std::uint8_t>(value * 17);
        case 16:
            return static_cast<std::uint8_t>(value >> 8);
        default:
            return static_cast<std::uint8_t>(value);
        }
    };
    // 8-bit samples, by far the most common: no unpacking or scaling.
    if (depth == 8) {
        switch (h.colorType) {
        case 6:
            if (step == 4) {
                std::memcpy(out, row, std::size_t{count} * 4);
                return;
            }
            for (std::uint32_t x = 0; x < count; ++x, out += step, row += 4)
                std::memcpy(out, row, 4);
            return;
        case 2:
            for (std::uint32_t x = 0; x < count; ++x, out += step, row += 3) {
                out[0] = row[0];
                out[1] = row[1];
                out[2] = row[2];
                out[3]
                    = trns.present && row[0] == trns.key[0] && row[1] == trns.key[1] && row[2] == trns.key[2] ? 0 : 255;
            }
            return;
        case 3:
            for (std::uint32_t x = 0; x < count; ++x, out += step)
                std::memcpy(out, palette.colors[row[x]].data(), 4);
            return;
        case 0:
            for (std::uint32_t x = 0; x < count; ++x, out += step) {
                out[0] = out[1] = out[2] = row[x];
                out[3] = trns.present && row[x] == trns.key[0] ? 0 : 255;
            }
            return;
        case 4:
            for (std::uint32_t x = 0; x < count; ++x, out += step, row += 2) {
                out[0] = out[1] = out[2] = row[0];
                out[3] = row[1];
            }
            return;
        default:
            break;
        }
    }
    for (std::uint32_t x = 0; x < count; ++x, out += step) {
        switch (h.colorType) {
        case 0: {
            const unsigned g = sample(x);
            out[0] = out[1] = out[2] = to8(g);
            out[3] = trns.present && g == trns.key[0] ? 0 : 255;
            break;
        }
        case 2: {
            const unsigned r = sample(x * 3);
            const unsigned g = sample(x * 3 + 1);
            const unsigned b = sample(x * 3 + 2);
            out[0] = to8(r);
            out[1] = to8(g);
            out[2] = to8(b);
            out[3] = trns.present && r == trns.key[0] && g == trns.key[1] && b == trns.key[2] ? 0 : 255;
            break;
        }
        case 3: {
            // Indices past the palette are an error in libpng; black is kinder.
            const unsigned index = sample(x);
            const auto& color = palette.colors[index];
            out[0] = color[0];
            out[1] = color[1];
            out[2] = color[2];
            out[3] = color[3];
            break;
        }
        case 4:
            out[0] = out[1] = out[2] = to8(sample(x * 2));
            out[3] = to8(sample(x * 2 + 1));
            break;
        default:
            for (unsigned c = 0; c < 4; ++c)
                out[c] = to8(sample(x * 4 + c));
            break;
        }
    }
}

std::string describe(const Header& h)
{
    static constexpr std::array<std::string_view, 7> names{"gray", "", "RGB", "palette", "gray+alpha", "", "RGBA"};
    return std::format("{} {}-bit{}", names[h.colorType], h.depth, h.interlaced ? ", Adam7" : "");
}

} // namespace

Result<TextureFile> decodePngFile(std::string_view bytes)
{
    if (!bytes.starts_with(signature))
        return std::unexpected("not a PNG file");
    std::size_t at = signature.size();
    Header header;
    bool haveHeader = false;
    Palette palette;
    for (auto& color : palette.colors)
        color = {0, 0, 0, 255};
    Transparency trns;
    std::vector<std::uint8_t> compressed;
    bool sawData = false;
    bool dataEnded = false;
    bool ended = false;

    while (!ended) {
        if (bytes.size() - at < 12)
            return std::unexpected("PNG ends before IEND");
        const std::uint32_t length = be32(bytes, at);
        const std::string_view type = bytes.substr(at + 4, 4);
        if (length > bytes.size() - at - 12)
            return std::unexpected(std::format("PNG chunk {} runs past the end of the file", type));
        const std::string_view data = bytes.substr(at + 8, length);
        at += 12 + std::size_t{length}; // CRCs aren't checked; zlib's checksum catches damaged pixels

        if (!haveHeader && type != "IHDR")
            return std::unexpected("PNG doesn't start with IHDR");
        if (type == "IHDR") {
            if (haveHeader)
                return std::unexpected("PNG has two IHDR chunks");
            auto parsed = parseHeader(data);
            if (!parsed)
                return std::unexpected(parsed.error());
            header = *parsed;
            haveHeader = true;
        } else if (type == "PLTE") {
            if (length % 3 != 0 || length / 3 > 256 || length == 0)
                return std::unexpected("PLTE has an invalid size");
            palette.size = length / 3;
            for (std::size_t i = 0; i < palette.size; ++i)
                palette.colors[i] = {byteAt(data, i * 3), byteAt(data, i * 3 + 1), byteAt(data, i * 3 + 2), 255};
        } else if (type == "tRNS") {
            if (header.colorType == 3) {
                for (std::size_t i = 0; i < std::min<std::size_t>(length, 256); ++i)
                    palette.colors[i][3] = byteAt(data, i);
            } else if (header.colorType == 0 && length >= 2) {
                trns.present = true;
                trns.key[0] = be16(data, 0);
            } else if (header.colorType == 2 && length >= 6) {
                trns.present = true;
                trns.key = {be16(data, 0), be16(data, 2), be16(data, 4)};
            }
        } else if (type == "IDAT") {
            if (dataEnded)
                return std::unexpected("PNG image data isn't contiguous");
            sawData = true;
            compressed.insert(compressed.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            ended = true;
        } else if ((byteAt(type, 0) & 0x20) == 0) {
            return std::unexpected(std::format("unknown critical PNG chunk {}", type));
        }
        if (sawData && type != "IDAT")
            dataEnded = true;
    }
    if (!sawData)
        return std::unexpected("PNG has no image data");
    const Header& h = header;
    if (h.colorType == 3 && palette.size == 0)
        return std::unexpected("palette PNG has no PLTE");

    const std::size_t expected = expectedBytes(h);
    auto raw = zlibDecompress(std::span(compressed), {.sizeHint = expected, .maxSize = expected});
    if (!raw)
        return std::unexpected(std::format("PNG image data: {}", raw.error()));
    if (raw->size() != expected)
        return std::unexpected(std::format("PNG image data has {} bytes, expected {}", raw->size(), expected));

    Image image = blankImage(h.width, h.height);
    const std::size_t bpp = std::max<std::size_t>(1, h.bitsPerPixel() / 8);
    std::size_t offset = 0;
    for (const Pass& pass : passesOf(h)) {
        const std::uint32_t w = passSize(h.width, pass.x0, pass.dx);
        const std::uint32_t rows = passSize(h.height, pass.y0, pass.dy);
        if (w == 0 || rows == 0)
            continue;
        const std::size_t stride = h.rowBytes(w);
        const std::uint8_t* previous = nullptr;
        for (std::uint32_t y = 0; y < rows; ++y) {
            std::uint8_t* line = raw->data() + offset;
            if (auto ok = unfilter(line + 1, previous, stride, bpp, line[0]); !ok)
                return std::unexpected(ok.error());
            const std::size_t outY = pass.y0 + std::size_t{y} * pass.dy;
            std::uint8_t* out = image.rgba.data() + (outY * h.width + pass.x0) * 4;
            expandRow(h, line + 1, w, out, std::size_t{pass.dx} * 4, palette, trns);
            previous = line + 1;
            offset += 1 + stride;
        }
    }
    return singleImage(ImageFormat::Png, describe(h), std::move(image));
}

} // namespace lunar::assets::detail
