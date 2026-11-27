#include "BlockCompression.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace lunar::assets::detail {

namespace {

using Block = std::array<std::uint8_t, 64>; // 4x4 RGBA, rows top to bottom

std::uint8_t clampByte(int value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

std::uint16_t le16At(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint64_t le64At(const std::uint8_t* p)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | p[i];
    return value;
}

std::uint64_t be64At(const std::uint8_t* p)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | p[i];
    return value;
}

// --- BC1-BC5 ---------------------------------------------------------------

// threeColor allows BC1's punch-through mode; BC2 and BC3 always use four colours.
void decodeColor(const std::uint8_t* p, Block& out, bool threeColor)
{
    const std::uint16_t c0 = le16At(p);
    const std::uint16_t c1 = le16At(p + 2);
    std::array<std::array<int, 4>, 4> palette{};
    auto expand = [](std::uint16_t c) {
        const int r = (c >> 11) & 31;
        const int g = (c >> 5) & 63;
        const int b = c & 31;
        return std::array<int, 4>{(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
    };
    palette[0] = expand(c0);
    palette[1] = expand(c1);
    if (c0 > c1 || !threeColor) {
        for (int c = 0; c < 3; ++c) {
            const auto i = static_cast<std::size_t>(c);
            palette[2][i] = (2 * palette[0][i] + palette[1][i]) / 3;
            palette[3][i] = (palette[0][i] + 2 * palette[1][i]) / 3;
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int c = 0; c < 3; ++c) {
            const auto i = static_cast<std::size_t>(c);
            palette[2][i] = (palette[0][i] + palette[1][i]) / 2;
        }
        palette[2][3] = 255;
        palette[3] = {0, 0, 0, 0};
    }
    std::uint32_t indices
        = static_cast<std::uint32_t>(le16At(p + 4)) | (static_cast<std::uint32_t>(le16At(p + 6)) << 16);
    for (std::size_t i = 0; i < 16; ++i, indices >>= 2) {
        const auto& color = palette[indices & 3];
        for (std::size_t c = 0; c < 4; ++c)
            out[i * 4 + c] = static_cast<std::uint8_t>(color[c]);
    }
}

// The 8-value interpolated channel of BC3 alpha, BC4 and BC5.
template <bool Signed>
void decodeChannel(const std::uint8_t* p, Block& out, std::size_t channel)
{
    std::array<int, 8> values{};
    const int a0 = Signed ? static_cast<std::int8_t>(p[0]) : p[0];
    const int a1 = Signed ? static_cast<std::int8_t>(p[1]) : p[1];
    values[0] = a0;
    values[1] = a1;
    const int low = Signed ? -127 : 0;
    const int high = Signed ? 127 : 255;
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            values[static_cast<std::size_t>(i) + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; ++i)
            values[static_cast<std::size_t>(i) + 1] = ((5 - i) * a0 + i * a1) / 5;
        values[6] = low;
        values[7] = high;
    }
    std::uint64_t indices = le64At(p) >> 16;
    for (std::size_t i = 0; i < 16; ++i, indices >>= 3) {
        int value = values[indices & 7];
        if (Signed)
            value = (std::max(value, -127) + 127) * 255 / 254;
        out[i * 4 + channel] = clampByte(value);
    }
}

void decodeBc2Alpha(const std::uint8_t* p, Block& out)
{
    for (std::size_t i = 0; i < 16; ++i) {
        const int nibble = (p[i / 2] >> ((i % 2) * 4)) & 15;
        out[i * 4 + 3] = static_cast<std::uint8_t>(nibble * 17);
    }
}

// --- BC7 -------------------------------------------------------------------

struct Bc7Mode {
    int subsets, partitionBits, rotationBits, indexSelectionBits, colorBits, alphaBits, endpointPBits, sharedPBits,
        indexBits, index2Bits;
};

constexpr std::array<Bc7Mode, 8> bc7Modes{{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

// Bit i set: pixel i is in subset 1.
constexpr std::array<std::uint16_t, 64> partitions2{
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8,
    0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110,
    0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696,
    0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720,
    0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};

// Two bits per pixel, pixel 0 in the low bits.
constexpr std::array<std::uint32_t, 64> partitions3{
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254};

constexpr std::array<std::uint8_t, 64> anchor2{15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                               15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                               15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                               6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::array<std::uint8_t, 64> anchor3a{
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8, 6,  6, 6,  5,  3,  3,  3, 3,  8, 15, 3, 3, 6, 10, 5, 8,  8, 6,  8,  5,  15, 15,
    8, 15, 3,  5,  6, 10, 8,  15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5,  5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3,  3};
constexpr std::array<std::uint8_t, 64> anchor3b{15, 8, 8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                                15, 8, 15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                                15, 3, 15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                                15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

constexpr std::array<int, 4> weights2{0, 21, 43, 64};
constexpr std::array<int, 8> weights3{0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<int, 16> weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

int bc7Weight(int bits, unsigned index)
{
    return bits == 2 ? weights2[index & 3] : bits == 3 ? weights3[index & 7] : weights4[index & 15];
}

class Bits128 {
public:
    explicit Bits128(const std::uint8_t* p) : low_(le64At(p)), high_(le64At(p + 8)) {}
    unsigned read(int n)
    {
        if (n == 0)
            return 0;
        const auto value = static_cast<unsigned>(low_ & ((std::uint64_t{1} << n) - 1));
        low_ = (low_ >> n) | (high_ << (64 - n));
        high_ >>= n;
        return value;
    }

private:
    std::uint64_t low_;
    std::uint64_t high_;
};

unsigned subsetOf(int subsets, unsigned partition, std::size_t pixel)
{
    if (subsets == 2)
        return (partitions2[partition] >> pixel) & 1;
    if (subsets == 3)
        return (partitions3[partition] >> (pixel * 2)) & 3;
    return 0;
}

bool isAnchor(int subsets, unsigned partition, std::size_t pixel)
{
    if (pixel == 0)
        return true;
    if (subsets == 2)
        return pixel == anchor2[partition];
    if (subsets == 3)
        return pixel == anchor3a[partition] || pixel == anchor3b[partition];
    return false;
}

void decodeBc7(const std::uint8_t* p, Block& out)
{
    int mode = 0;
    while (mode < 8 && (p[0] & (1 << mode)) == 0)
        ++mode;
    if (mode == 8) {
        out.fill(0); // reserved: transparent black, as the spec says
        return;
    }
    const Bc7Mode& m = bc7Modes[static_cast<std::size_t>(mode)];
    Bits128 bits(p);
    bits.read(mode + 1);
    const unsigned partition = bits.read(m.partitionBits);
    const unsigned rotation = bits.read(m.rotationBits);
    const unsigned indexSelection = bits.read(m.indexSelectionBits);

    // endpoints[subset * 2 + end][channel]
    std::array<std::array<int, 4>, 6> endpoints{};
    const int ends = m.subsets * 2;
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ends; ++e)
            endpoints[static_cast<std::size_t>(e)][static_cast<std::size_t>(c)]
                = static_cast<int>(bits.read(m.colorBits));
    for (int e = 0; e < ends; ++e)
        endpoints[static_cast<std::size_t>(e)][3] = m.alphaBits ? static_cast<int>(bits.read(m.alphaBits)) : 255;

    int colorBits = m.colorBits;
    int alphaBits = m.alphaBits;
    if (m.endpointPBits || m.sharedPBits) {
        std::array<unsigned, 6> pbits{};
        if (m.endpointPBits) {
            for (int e = 0; e < ends; ++e)
                pbits[static_cast<std::size_t>(e)] = bits.read(1);
        } else {
            for (int s = 0; s < m.subsets; ++s)
                pbits[static_cast<std::size_t>(s) * 2] = pbits[static_cast<std::size_t>(s) * 2 + 1] = bits.read(1);
        }
        for (int e = 0; e < ends; ++e) {
            auto& endpoint = endpoints[static_cast<std::size_t>(e)];
            for (int c = 0; c < (m.alphaBits ? 4 : 3); ++c)
                endpoint[static_cast<std::size_t>(c)] = (endpoint[static_cast<std::size_t>(c)] << 1)
                                                        | static_cast<int>(pbits[static_cast<std::size_t>(e)]);
        }
        ++colorBits;
        if (alphaBits)
            ++alphaBits;
    }
    auto expand = [](int value, int bitCount) { return (value << (8 - bitCount)) | (value >> (2 * bitCount - 8)); };
    for (int e = 0; e < ends; ++e) {
        auto& endpoint = endpoints[static_cast<std::size_t>(e)];
        for (std::size_t c = 0; c < 3; ++c)
            endpoint[c] = expand(endpoint[c], colorBits);
        if (alphaBits)
            endpoint[3] = expand(endpoint[3], alphaBits);
    }

    std::array<unsigned, 16> indices{};
    std::array<unsigned, 16> indices2{};
    for (std::size_t i = 0; i < 16; ++i)
        indices[i] = bits.read(m.indexBits - (isAnchor(m.subsets, partition, i) ? 1 : 0));
    if (m.index2Bits)
        for (std::size_t i = 0; i < 16; ++i)
            indices2[i] = bits.read(m.index2Bits - (i == 0 ? 1 : 0));

    for (std::size_t i = 0; i < 16; ++i) {
        const unsigned subset = subsetOf(m.subsets, partition, i);
        const auto& e0 = endpoints[std::size_t{subset} * 2];
        const auto& e1 = endpoints[std::size_t{subset} * 2 + 1];
        int colorWeight = bc7Weight(m.indexBits, indices[i]);
        int alphaWeight = colorWeight;
        if (m.index2Bits) {
            const int other = bc7Weight(m.index2Bits, indices2[i]);
            if (indexSelection) {
                alphaWeight = colorWeight;
                colorWeight = other;
            } else {
                alphaWeight = other;
            }
        }
        std::array<int, 4> pixel{};
        for (std::size_t c = 0; c < 3; ++c)
            pixel[c] = ((64 - colorWeight) * e0[c] + colorWeight * e1[c] + 32) >> 6;
        pixel[3] = ((64 - alphaWeight) * e0[3] + alphaWeight * e1[3] + 32) >> 6;
        if (rotation != 0)
            std::swap(pixel[3], pixel[rotation - 1]);
        for (std::size_t c = 0; c < 4; ++c)
            out[i * 4 + c] = static_cast<std::uint8_t>(pixel[c]);
    }
}

// --- ETC2 ------------------------------------------------------------------

constexpr std::array<std::array<int, 2>, 8> etcModifiers{
    {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}}};
constexpr std::array<int, 8> etcDistances{3, 6, 11, 16, 23, 32, 41, 64};

// Pixel indices run down columns: pixel (x, y) is entry x * 4 + y.
unsigned etcIndex(std::uint64_t block, std::size_t x, std::size_t y)
{
    const std::size_t i = x * 4 + y;
    const auto msb = static_cast<unsigned>((block >> (16 + i)) & 1);
    const auto lsb = static_cast<unsigned>((block >> i) & 1);
    return (msb << 1) | lsb;
}

void setPixel(Block& out, std::size_t x, std::size_t y, int r, int g, int b)
{
    std::uint8_t* px = out.data() + (y * 4 + x) * 4;
    px[0] = clampByte(r);
    px[1] = clampByte(g);
    px[2] = clampByte(b);
    px[3] = 255;
}

void decodeEtc2Color(std::uint64_t block, Block& out)
{
    auto field
        = [&](int high, int count) { return static_cast<int>((block >> (high - count + 1)) & ((1u << count) - 1)); };
    auto extend4 = [](int v) { return v * 17; };
    auto extend5 = [](int v) { return (v << 3) | (v >> 2); };
    const bool differential = (block >> 33) & 1;
    const bool flip = (block >> 32) & 1;

    std::array<std::array<int, 3>, 2> base{};
    if (!differential) {
        base[0] = {extend4(field(63, 4)), extend4(field(55, 4)), extend4(field(47, 4))};
        base[1] = {extend4(field(59, 4)), extend4(field(51, 4)), extend4(field(43, 4))};
    } else {
        auto signed3 = [](int v) { return v >= 4 ? v - 8 : v; };
        const int r = field(63, 5);
        const int g = field(55, 5);
        const int b = field(47, 5);
        const int r2 = r + signed3(field(58, 3));
        const int g2 = g + signed3(field(50, 3));
        const int b2 = b + signed3(field(42, 3));
        if (r2 < 0 || r2 > 31) {
            // T mode
            const std::array<int, 3> c1{extend4((field(60, 2) << 2) | field(57, 2)), extend4(field(55, 4)),
                                        extend4(field(51, 4))};
            const std::array<int, 3> c2{extend4(field(47, 4)), extend4(field(43, 4)), extend4(field(39, 4))};
            const int d = etcDistances[static_cast<std::size_t>((field(35, 2) << 1) | field(32, 1))];
            const std::array<std::array<int, 3>, 4> paint{c1, std::array<int, 3>{c2[0] + d, c2[1] + d, c2[2] + d}, c2,
                                                          std::array<int, 3>{c2[0] - d, c2[1] - d, c2[2] - d}};
            for (std::size_t y = 0; y < 4; ++y)
                for (std::size_t x = 0; x < 4; ++x) {
                    const auto& c = paint[etcIndex(block, x, y)];
                    setPixel(out, x, y, c[0], c[1], c[2]);
                }
            return;
        }
        if (g2 < 0 || g2 > 31) {
            // H mode
            const int r1 = field(62, 4);
            const int g1 = (field(58, 3) << 1) | field(52, 1);
            const int b1 = (field(51, 1) << 3) | field(49, 3);
            const int r2h = field(46, 4);
            const int g2h = field(42, 4);
            const int b2h = field(38, 4);
            const int order = ((r1 << 8) | (g1 << 4) | b1) >= ((r2h << 8) | (g2h << 4) | b2h) ? 1 : 0;
            const int d = etcDistances[static_cast<std::size_t>((field(34, 1) << 2) | (field(32, 1) << 1) | order)];
            const std::array<int, 3> c1{extend4(r1), extend4(g1), extend4(b1)};
            const std::array<int, 3> c2{extend4(r2h), extend4(g2h), extend4(b2h)};
            const std::array<std::array<int, 3>, 4> paint{std::array<int, 3>{c1[0] + d, c1[1] + d, c1[2] + d},
                                                          std::array<int, 3>{c1[0] - d, c1[1] - d, c1[2] - d},
                                                          std::array<int, 3>{c2[0] + d, c2[1] + d, c2[2] + d},
                                                          std::array<int, 3>{c2[0] - d, c2[1] - d, c2[2] - d}};
            for (std::size_t y = 0; y < 4; ++y)
                for (std::size_t x = 0; x < 4; ++x) {
                    const auto& c = paint[etcIndex(block, x, y)];
                    setPixel(out, x, y, c[0], c[1], c[2]);
                }
            return;
        }
        if (b2 < 0 || b2 > 31) {
            // Planar mode
            auto extend6 = [](int v) { return (v << 2) | (v >> 4); };
            auto extend7 = [](int v) { return (v << 1) | (v >> 6); };
            const std::array<int, 3> o{extend6(field(62, 6)), extend7((field(56, 1) << 6) | field(54, 6)),
                                       extend6((field(48, 1) << 5) | (field(44, 2) << 3) | field(41, 3))};
            const std::array<int, 3> h{extend6((field(38, 5) << 1) | field(32, 1)), extend7(field(31, 7)),
                                       extend6(field(24, 6))};
            const std::array<int, 3> v{extend6(field(18, 6)), extend7(field(12, 7)), extend6(field(5, 6))};
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    std::array<int, 3> c{};
                    for (std::size_t k = 0; k < 3; ++k)
                        c[k] = (x * (h[k] - o[k]) + y * (v[k] - o[k]) + 4 * o[k] + 2) >> 2;
                    setPixel(out, static_cast<std::size_t>(x), static_cast<std::size_t>(y), c[0], c[1], c[2]);
                }
            return;
        }
        base[0] = {extend5(r), extend5(g), extend5(b)};
        base[1] = {extend5(r2), extend5(g2), extend5(b2)};
    }
    const std::array<int, 2> tables{field(39, 3), field(36, 3)};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
            const unsigned index = etcIndex(block, x, y);
            const auto& modifiers = etcModifiers[static_cast<std::size_t>(tables[sub])];
            const int magnitude = modifiers[index & 1];
            const int delta = (index & 2) ? -magnitude : magnitude;
            setPixel(out, x, y, base[sub][0] + delta, base[sub][1] + delta, base[sub][2] + delta);
        }
}

constexpr std::array<std::array<int, 8>, 16> eacModifiers{{
    {-3, -6, -9, -15, 2, 5, 8, 14},
    {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},
    {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},
    {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},
    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},
    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
}};

void decodeEacAlpha(std::uint64_t block, Block& out)
{
    const auto base = static_cast<int>(block >> 56);
    const auto multiplier = static_cast<int>((block >> 52) & 15);
    const auto& modifiers = eacModifiers[(block >> 48) & 15];
    for (std::size_t x = 0; x < 4; ++x)
        for (std::size_t y = 0; y < 4; ++y) {
            const std::size_t i = x * 4 + y;
            const auto index = static_cast<std::size_t>((block >> (45 - 3 * i)) & 7);
            out[(y * 4 + x) * 4 + 3] = clampByte(base + modifiers[index] * multiplier);
        }
}

void decodeBlock(BlockFormat format, const std::uint8_t* p, Block& out)
{
    switch (format) {
    case BlockFormat::Bc1:
        decodeColor(p, out, true);
        break;
    case BlockFormat::Bc2:
        decodeColor(p + 8, out, false);
        decodeBc2Alpha(p, out);
        break;
    case BlockFormat::Bc3:
        decodeColor(p + 8, out, false);
        decodeChannel<false>(p, out, 3);
        break;
    case BlockFormat::Bc4:
    case BlockFormat::Bc4Snorm:
        if (format == BlockFormat::Bc4)
            decodeChannel<false>(p, out, 0);
        else
            decodeChannel<true>(p, out, 0);
        for (std::size_t i = 0; i < 16; ++i) {
            out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4];
            out[i * 4 + 3] = 255;
        }
        break;
    case BlockFormat::Bc5:
    case BlockFormat::Bc5Snorm:
        if (format == BlockFormat::Bc5) {
            decodeChannel<false>(p, out, 0);
            decodeChannel<false>(p + 8, out, 1);
        } else {
            decodeChannel<true>(p, out, 0);
            decodeChannel<true>(p + 8, out, 1);
        }
        for (std::size_t i = 0; i < 16; ++i) {
            out[i * 4 + 2] = 0;
            out[i * 4 + 3] = 255;
        }
        break;
    case BlockFormat::Bc7:
        decodeBc7(p, out);
        break;
    case BlockFormat::Etc2Rgb:
        decodeEtc2Color(be64At(p), out);
        break;
    case BlockFormat::Etc2Rgba:
        decodeEtc2Color(be64At(p + 8), out);
        decodeEacAlpha(be64At(p), out);
        break;
    }
}

} // namespace

std::size_t blockBytes(BlockFormat format)
{
    switch (format) {
    case BlockFormat::Bc1:
    case BlockFormat::Bc4:
    case BlockFormat::Bc4Snorm:
    case BlockFormat::Etc2Rgb:
        return 8;
    default:
        return 16;
    }
}

std::optional<BlockCompressed::Format> gpuFormat(BlockFormat format)
{
    switch (format) {
    case BlockFormat::Bc1:
        return BlockCompressed::Format::Bc1;
    case BlockFormat::Bc2:
        return BlockCompressed::Format::Bc2;
    case BlockFormat::Bc3:
        return BlockCompressed::Format::Bc3;
    case BlockFormat::Bc4:
        return BlockCompressed::Format::Bc4;
    case BlockFormat::Bc5:
        return BlockCompressed::Format::Bc5;
    case BlockFormat::Bc7:
        return BlockCompressed::Format::Bc7;
    default:
        return std::nullopt;
    }
}

std::size_t blockImageBytes(BlockFormat format, std::uint32_t width, std::uint32_t height)
{
    return ((std::size_t{width} + 3) / 4) * ((std::size_t{height} + 3) / 4) * blockBytes(format);
}

Image decodeBlocks(BlockFormat format, std::span<const std::uint8_t> data, std::uint32_t width, std::uint32_t height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(image.byteSize());
    const std::size_t blocksX = (std::size_t{width} + 3) / 4;
    const std::size_t blocksY = (std::size_t{height} + 3) / 4;
    const std::size_t size = blockBytes(format);
    Block block{};
    for (std::size_t by = 0; by < blocksY; ++by)
        for (std::size_t bx = 0; bx < blocksX; ++bx) {
            decodeBlock(format, data.data() + (by * blocksX + bx) * size, block);
            const std::size_t rows = std::min<std::size_t>(4, height - by * 4);
            const std::size_t columns = std::min<std::size_t>(4, width - bx * 4);
            for (std::size_t y = 0; y < rows; ++y)
                std::memcpy(image.rgba.data() + ((by * 4 + y) * width + bx * 4) * 4, block.data() + y * 16,
                            columns * 4);
        }
    return image;
}

} // namespace lunar::assets::detail
