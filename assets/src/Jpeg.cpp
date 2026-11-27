#include "ImageCodecs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace lunar::assets::detail {

namespace {

// Natural (row-major) position of the k-th coefficient in zigzag order.
constexpr std::array<std::uint8_t, 64> zigzag{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                              12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                              35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                              58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr int lookupBits = 9;

struct HuffmanTable {
    bool defined = false;
    std::array<std::uint8_t, 256> values{};
    std::array<std::int32_t, 18> maxCode{}; // largest code of each length, -1 if none
    std::array<std::int32_t, 17> valueOffset{};
    std::array<std::uint16_t, 1 << lookupBits> lookup{}; // length << 8 | value, 0 for longer codes

    Result<void> build(const std::array<std::uint8_t, 17>& counts, std::span<const std::uint8_t> symbols)
    {
        std::ranges::copy(symbols, values.begin());
        lookup.fill(0);
        std::int32_t code = 0;
        std::int32_t k = 0;
        for (int length = 1; length <= 16; ++length) {
            const auto l = static_cast<std::size_t>(length);
            valueOffset[l] = k - code;
            for (int i = 0; i < int{counts[l]}; ++i, ++code, ++k) {
                if (code >= (1 << length))
                    return std::unexpected("invalid JPEG Huffman table");
                if (length <= lookupBits) {
                    const int spread = lookupBits - length;
                    for (int j = 0; j < (1 << spread); ++j)
                        lookup[static_cast<std::size_t>((code << spread) | j)]
                            = static_cast<std::uint16_t>((length << 8) | values[static_cast<std::size_t>(k)]);
                }
            }
            if (code > (1 << length))
                return std::unexpected("invalid JPEG Huffman table");
            maxCode[l] = counts[l] ? code - 1 : -1;
            code <<= 1;
        }
        maxCode[17] = 0x7fffffff;
        defined = true;
        return {};
    }
};

// Entropy-coded data, most significant bit first. A 0xFF byte is followed
// by a stuffed zero; any other marker ends the data, after which (and past
// the end of the file) zeros come back.
class BitReader {
public:
    BitReader(std::string_view data, std::size_t pos) : data_(data), pos_(pos) {}

    void fill()
    {
        while (count_ <= 56) {
            std::uint64_t byte = 0;
            if (!atMarker_ && pos_ < data_.size()) {
                byte = byteAt(data_, pos_);
                if (byte == 0xff) {
                    std::size_t next = pos_ + 1;
                    while (next < data_.size() && byteAt(data_, next) == 0xff)
                        ++next;
                    if (next < data_.size() && byteAt(data_, next) == 0) {
                        pos_ = next + 1;
                    } else {
                        atMarker_ = true; // pos_ stays on the 0xFF
                        exhausted_ = next >= data_.size();
                        byte = 0;
                    }
                } else {
                    ++pos_;
                }
            } else if (!atMarker_) {
                exhausted_ = true;
            }
            bits_ |= byte << (56 - count_);
            count_ += 8;
        }
    }
    unsigned bits(int n) // 1 <= n <= 16
    {
        if (count_ < n)
            fill();
        const auto value = static_cast<unsigned>(bits_ >> (64 - n));
        bits_ <<= n;
        count_ -= n;
        return value;
    }
    unsigned bit() { return bits(1); }
    int extend(int size) // the signed value of a size-bit magnitude category
    {
        if (size == 0)
            return 0;
        const int value = static_cast<int>(bits(size));
        return value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
    }
    int decode(const HuffmanTable& table)
    {
        if (count_ < 16)
            fill();
        const std::uint16_t entry = table.lookup[bits_ >> (64 - lookupBits)];
        if (entry != 0) {
            bits_ <<= entry >> 8;
            count_ -= entry >> 8;
            return entry & 0xff;
        }
        for (int length = lookupBits + 1; length <= 16; ++length) {
            const auto code = static_cast<std::int32_t>(bits_ >> (64 - length));
            if (code <= table.maxCode[static_cast<std::size_t>(length)]) {
                bits_ <<= length;
                count_ -= length;
                const std::int32_t index = code + table.valueOffset[static_cast<std::size_t>(length)];
                if (index < 0 || index > 255)
                    return -1;
                return table.values[static_cast<std::size_t>(index)];
            }
        }
        return -1;
    }

    // Drops buffered bits and steps over the restart marker that should be next.
    void restart()
    {
        bits_ = 0;
        count_ = 0;
        if (!atMarker_) {
            // Garbage before the marker: find it.
            while (pos_ + 1 < data_.size()
                   && (byteAt(data_, pos_) != 0xff || byteAt(data_, pos_ + 1) == 0 || byteAt(data_, pos_ + 1) == 0xff))
                ++pos_;
        }
        while (pos_ + 1 < data_.size() && byteAt(data_, pos_) == 0xff && byteAt(data_, pos_ + 1) == 0xff)
            ++pos_;
        if (pos_ + 1 < data_.size() && byteAt(data_, pos_) == 0xff && (byteAt(data_, pos_ + 1) & 0xf8) == 0xd0)
            pos_ += 2;
        atMarker_ = false;
    }

    bool exhausted() const { return exhausted_; }
    // Where the marker after the data starts.
    std::size_t markerPosition() const
    {
        std::size_t at = pos_;
        while (at + 1 < data_.size() && (byteAt(data_, at) != 0xff || byteAt(data_, at + 1) == 0))
            ++at;
        return at;
    }

private:
    std::string_view data_;
    std::size_t pos_;
    std::uint64_t bits_ = 0;
    int count_ = 0;
    bool atMarker_ = false;
    bool exhausted_ = false;
};

struct Component {
    int id = 0;
    int h = 1;
    int v = 1;
    int quantTable = 0;
    std::array<std::uint16_t, 64> quant{}; // natural order, fixed at the component's first scan
    bool quantLatched = false;
    std::uint32_t width = 0; // samples, before upsampling
    std::uint32_t height = 0;
    std::size_t blocksPerLine = 0; // padded to whole MCUs
    std::size_t blocksPerColumn = 0;
    std::vector<std::int16_t> coefficients; // 64 per block, natural order
    int dcTable = 0;
    int acTable = 0;
    int dcPrediction = 0;

    std::int16_t* block(std::size_t row, std::size_t column)
    {
        return coefficients.data() + (row * blocksPerLine + column) * 64;
    }
};

class Decoder {
public:
    explicit Decoder(std::string_view bytes) : bytes_(bytes) {}

    Result<TextureFile> run();

private:
    Result<std::string_view> segment(std::size_t& at);
    Result<void> readFrame(std::string_view data, bool isProgressive);
    Result<void> readHuffman(std::string_view data);
    Result<void> readQuantization(std::string_view data);
    Result<std::size_t> readScan(std::string_view data, std::size_t dataStart);
    Result<void> decodeBlock(BitReader& in, Component& c, std::int16_t* block);
    TextureFile output();

    std::string_view bytes_;
    std::array<std::array<std::uint16_t, 64>, 4> quant_{};
    std::array<bool, 4> quantDefined_{};
    std::array<HuffmanTable, 4> dcTables_{};
    std::array<HuffmanTable, 4> acTables_{};
    std::vector<Component> components_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    int maxH_ = 1;
    int maxV_ = 1;
    std::size_t mcusPerLine_ = 0;
    std::size_t mcusPerColumn_ = 0;
    bool progressive_ = false;
    bool frameSeen_ = false;
    unsigned restartInterval_ = 0;
    int adobeTransform_ = -1;
    bool jfif_ = false;
    // The scan being decoded.
    int spectralStart_ = 0;
    int spectralEnd_ = 63;
    int approxHigh_ = 0;
    int approxLow_ = 0;
    unsigned eobRun_ = 0;
};

Result<std::string_view> Decoder::segment(std::size_t& at)
{
    if (bytes_.size() - at < 2)
        return std::unexpected("JPEG segment is truncated");
    const std::size_t length = be16(bytes_, at);
    if (length < 2 || length > bytes_.size() - at)
        return std::unexpected("JPEG segment runs past the end of the file");
    std::string_view data = bytes_.substr(at + 2, length - 2);
    at += length;
    return data;
}

Result<void> Decoder::readFrame(std::string_view data, bool isProgressive)
{
    if (frameSeen_)
        return std::unexpected("JPEG has two frames");
    frameSeen_ = true;
    progressive_ = isProgressive;
    if (data.size() < 6)
        return std::unexpected("JPEG frame header is truncated");
    if (byteAt(data, 0) != 8)
        return std::unexpected(std::format("{}-bit JPEG isn't supported", byteAt(data, 0)));
    height_ = be16(data, 1);
    width_ = be16(data, 3);
    if (height_ == 0)
        return std::unexpected("JPEG with the height in a DNL marker isn't supported");
    if (auto size = checkSize(width_, height_); !size)
        return std::unexpected(size.error());
    const std::size_t count = byteAt(data, 5);
    if (count != 1 && count != 3 && count != 4)
        return std::unexpected(std::format("JPEG with {} components isn't supported", count));
    if (data.size() < 6 + count * 3)
        return std::unexpected("JPEG frame header is truncated");
    for (std::size_t i = 0; i < count; ++i) {
        Component c;
        c.id = byteAt(data, 6 + i * 3);
        c.h = byteAt(data, 7 + i * 3) >> 4;
        c.v = byteAt(data, 7 + i * 3) & 15;
        c.quantTable = byteAt(data, 8 + i * 3);
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.quantTable > 3)
            return std::unexpected("invalid JPEG component");
        for (const Component& other : components_)
            if (other.id == c.id)
                return std::unexpected("JPEG has two components with the same id");
        maxH_ = std::max(maxH_, c.h);
        maxV_ = std::max(maxV_, c.v);
        components_.push_back(c);
    }
    mcusPerLine_ = (width_ + 8u * static_cast<unsigned>(maxH_) - 1) / (8u * static_cast<unsigned>(maxH_));
    mcusPerColumn_ = (height_ + 8u * static_cast<unsigned>(maxV_) - 1) / (8u * static_cast<unsigned>(maxV_));
    std::size_t totalBlocks = 0;
    for (Component& c : components_) {
        c.width = static_cast<std::uint32_t>(
            (std::uint64_t{width_} * static_cast<unsigned>(c.h) + static_cast<unsigned>(maxH_) - 1)
            / static_cast<unsigned>(maxH_));
        c.height = static_cast<std::uint32_t>(
            (std::uint64_t{height_} * static_cast<unsigned>(c.v) + static_cast<unsigned>(maxV_) - 1)
            / static_cast<unsigned>(maxV_));
        c.blocksPerLine = mcusPerLine_ * static_cast<std::size_t>(c.h);
        c.blocksPerColumn = mcusPerColumn_ * static_cast<std::size_t>(c.v);
        totalBlocks += c.blocksPerLine * c.blocksPerColumn;
    }
    // Every block costs at least a bit, so a tiny file can't claim a huge
    // image and make us allocate for it.
    if (totalBlocks > bytes_.size() * 8 + 4096)
        return std::unexpected("JPEG is too small for its size");
    for (Component& c : components_)
        c.coefficients.assign(c.blocksPerLine * c.blocksPerColumn * 64, 0);
    return {};
}

Result<void> Decoder::readHuffman(std::string_view data)
{
    std::size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < 17)
            return std::unexpected("JPEG Huffman table is truncated");
        const unsigned kind = byteAt(data, at) >> 4;
        const unsigned index = byteAt(data, at) & 15;
        if (kind > 1 || index > 3)
            return std::unexpected("invalid JPEG Huffman table id");
        std::array<std::uint8_t, 17> counts{};
        std::size_t total = 0;
        for (std::size_t i = 1; i <= 16; ++i) {
            counts[i] = byteAt(data, at + i);
            total += counts[i];
        }
        at += 17;
        if (total > 256 || total > data.size() - at)
            return std::unexpected("JPEG Huffman table is truncated");
        std::array<std::uint8_t, 256> symbols{};
        for (std::size_t i = 0; i < total; ++i)
            symbols[i] = byteAt(data, at + i);
        at += total;
        HuffmanTable& table = kind == 0 ? dcTables_[index] : acTables_[index];
        if (auto built = table.build(counts, std::span(symbols).first(total)); !built)
            return built;
    }
    return {};
}

Result<void> Decoder::readQuantization(std::string_view data)
{
    std::size_t at = 0;
    while (at < data.size()) {
        const unsigned precision = byteAt(data, at) >> 4;
        const unsigned index = byteAt(data, at) & 15;
        if (precision > 1 || index > 3)
            return std::unexpected("invalid JPEG quantization table");
        const std::size_t size = precision ? 128 : 64;
        if (data.size() - at - 1 < size)
            return std::unexpected("JPEG quantization table is truncated");
        for (std::size_t k = 0; k < 64; ++k)
            quant_[index][zigzag[k]] = precision ? be16(data, at + 1 + k * 2) : byteAt(data, at + 1 + k);
        quantDefined_[index] = true;
        at += 1 + size;
    }
    return {};
}

Result<void> Decoder::decodeBlock(BitReader& in, Component& c, std::int16_t* block)
{
    const int high = approxHigh_;
    const int low = approxLow_;
    if (!progressive_ || spectralStart_ == 0) {
        // DC: sequential, or the first or a refining progressive scan.
        if (progressive_ && high != 0) {
            if (in.bit())
                block[0] = static_cast<std::int16_t>(block[0] | (1 << low));
        } else {
            const int size = in.decode(dcTables_[static_cast<std::size_t>(c.dcTable)]);
            if (size < 0 || size > 16)
                return std::unexpected("invalid JPEG DC code");
            c.dcPrediction = static_cast<std::int16_t>(c.dcPrediction + in.extend(size));
            block[0] = static_cast<std::int16_t>(c.dcPrediction * (1 << low));
        }
        if (progressive_)
            return {};
    }
    const HuffmanTable& ac = acTables_[static_cast<std::size_t>(c.acTable)];
    if (!progressive_) {
        for (int k = 1; k < 64;) {
            const int rs = in.decode(ac);
            if (rs < 0)
                return std::unexpected("invalid JPEG AC code");
            const int run = rs >> 4;
            const int size = rs & 15;
            if (size == 0) {
                if (run != 15)
                    break;
                k += 16;
                continue;
            }
            k += run;
            if (k > 63)
                return std::unexpected("JPEG AC coefficients run past the block");
            block[zigzag[static_cast<std::size_t>(k++)]] = static_cast<std::int16_t>(in.extend(size));
        }
        return {};
    }

    if (high == 0) {
        // First AC scan of a band.
        if (eobRun_ > 0) {
            --eobRun_;
            return {};
        }
        for (int k = spectralStart_; k <= spectralEnd_;) {
            const int rs = in.decode(ac);
            if (rs < 0)
                return std::unexpected("invalid JPEG AC code");
            const int run = rs >> 4;
            const int size = rs & 15;
            if (size == 0) {
                if (run < 15) {
                    eobRun_ = (1u << run) - 1;
                    if (run)
                        eobRun_ += in.bits(run);
                    break;
                }
                k += 16;
                continue;
            }
            k += run;
            if (k > spectralEnd_)
                return std::unexpected("JPEG AC coefficients run past the band");
            block[zigzag[static_cast<std::size_t>(k++)]] = static_cast<std::int16_t>(in.extend(size) * (1 << low));
        }
        return {};
    }

    // Refinement: one more bit for coefficients already nonzero, new ones
    // of magnitude 1 placed among the zeros.
    const int plus = 1 << low;
    const int minus = -plus;
    auto refine = [&](std::int16_t& coefficient) {
        if (in.bit() && (coefficient & plus) == 0)
            coefficient = static_cast<std::int16_t>(coefficient + (coefficient >= 0 ? plus : minus));
    };
    int k = spectralStart_;
    if (eobRun_ == 0) {
        for (; k <= spectralEnd_; ++k) {
            const int rs = in.decode(ac);
            if (rs < 0)
                return std::unexpected("invalid JPEG AC code");
            int run = rs >> 4;
            const int size = rs & 15;
            int value = 0;
            if (size != 0) {
                if (size != 1)
                    return std::unexpected("invalid JPEG refinement code");
                value = in.bit() ? plus : minus;
            } else if (run != 15) {
                eobRun_ = 1u << run;
                if (run)
                    eobRun_ += in.bits(run);
                break;
            }
            while (k <= spectralEnd_) {
                std::int16_t& coefficient = block[zigzag[static_cast<std::size_t>(k)]];
                if (coefficient != 0) {
                    refine(coefficient);
                } else {
                    if (run == 0)
                        break;
                    --run;
                }
                ++k;
            }
            if (value != 0 && k <= spectralEnd_)
                block[zigzag[static_cast<std::size_t>(k)]] = static_cast<std::int16_t>(value);
        }
    }
    if (eobRun_ > 0) {
        for (; k <= spectralEnd_; ++k) {
            std::int16_t& coefficient = block[zigzag[static_cast<std::size_t>(k)]];
            if (coefficient != 0)
                refine(coefficient);
        }
        --eobRun_;
    }
    return {};
}

Result<std::size_t> Decoder::readScan(std::string_view data, std::size_t dataStart)
{
    if (!frameSeen_)
        return std::unexpected("JPEG scan before the frame header");
    if (data.empty())
        return std::unexpected("JPEG scan header is truncated");
    const std::size_t count = byteAt(data, 0);
    if (count < 1 || count > 4 || data.size() < 1 + count * 2 + 3)
        return std::unexpected("invalid JPEG scan header");
    std::vector<Component*> scan;
    for (std::size_t i = 0; i < count; ++i) {
        const int id = byteAt(data, 1 + i * 2);
        auto found = std::ranges::find(components_, id, &Component::id);
        if (found == components_.end())
            return std::unexpected("JPEG scan names an unknown component");
        if (std::ranges::find(scan, &*found) != scan.end())
            return std::unexpected("JPEG scan names a component twice");
        found->dcTable = byteAt(data, 2 + i * 2) >> 4;
        found->acTable = byteAt(data, 2 + i * 2) & 15;
        if (found->dcTable > 3 || found->acTable > 3)
            return std::unexpected("invalid JPEG Huffman table id");
        scan.push_back(&*found);
    }
    spectralStart_ = byteAt(data, 1 + count * 2);
    spectralEnd_ = byteAt(data, 2 + count * 2);
    approxHigh_ = byteAt(data, 3 + count * 2) >> 4;
    approxLow_ = byteAt(data, 3 + count * 2) & 15;
    if (progressive_) {
        if (spectralEnd_ > 63 || spectralStart_ > spectralEnd_ || (spectralStart_ == 0 && spectralEnd_ != 0)
            || (spectralStart_ != 0 && count != 1) || approxHigh_ > 13 || approxLow_ > 13)
            return std::unexpected("invalid progressive JPEG scan");
    } else {
        spectralStart_ = 0;
        spectralEnd_ = 63;
        approxHigh_ = approxLow_ = 0;
    }
    const bool needDc = !progressive_ || (spectralStart_ == 0 && approxHigh_ == 0);
    const bool needAc = !progressive_ || spectralStart_ != 0;
    for (Component* c : scan) {
        if ((needDc && !dcTables_[static_cast<std::size_t>(c->dcTable)].defined)
            || (needAc && !acTables_[static_cast<std::size_t>(c->acTable)].defined))
            return std::unexpected("JPEG scan uses an undefined Huffman table");
        if (!c->quantLatched) {
            if (!quantDefined_[static_cast<std::size_t>(c->quantTable)])
                return std::unexpected("JPEG component uses an undefined quantization table");
            c->quant = quant_[static_cast<std::size_t>(c->quantTable)];
            c->quantLatched = true;
        }
        c->dcPrediction = 0;
    }
    eobRun_ = 0;

    BitReader in(bytes_, dataStart);
    unsigned untilRestart = restartInterval_;
    auto restart = [&] {
        if (restartInterval_ == 0)
            return;
        if (untilRestart == 0) {
            in.restart();
            for (Component* c : scan)
                c->dcPrediction = 0;
            eobRun_ = 0;
            untilRestart = restartInterval_;
        }
        --untilRestart;
    };
    if (scan.size() == 1) {
        // One component: blocks in raster order, only those covering its samples.
        Component& c = *scan[0];
        const std::size_t columns = (c.width + 7) / 8;
        const std::size_t rows = (c.height + 7) / 8;
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t column = 0; column < columns; ++column) {
                restart();
                if (auto ok = decodeBlock(in, c, c.block(row, column)); !ok)
                    return std::unexpected(ok.error());
            }
    } else {
        for (std::size_t my = 0; my < mcusPerColumn_; ++my)
            for (std::size_t mx = 0; mx < mcusPerLine_; ++mx) {
                restart();
                for (Component* c : scan)
                    for (int by = 0; by < c->v; ++by)
                        for (int bx = 0; bx < c->h; ++bx) {
                            std::int16_t* block
                                = c->block(my * static_cast<std::size_t>(c->v) + static_cast<std::size_t>(by),
                                           mx * static_cast<std::size_t>(c->h) + static_cast<std::size_t>(bx));
                            if (auto ok = decodeBlock(in, *c, block); !ok)
                                return std::unexpected(ok.error());
                        }
            }
    }
    if (in.exhausted())
        return std::unexpected("JPEG data is truncated");
    return in.markerPosition();
}

// libjpeg's accurate integer IDCT (jidctint.c), so pixels match what most
// tools produce. 64-bit, so corrupt coefficients can't overflow.
void inverseDct(const std::int16_t* in, const std::array<std::uint16_t, 64>& quant, std::uint8_t* out,
                std::size_t stride)
{
    using I = std::int64_t;
    constexpr int constBits = 13;
    constexpr int pass1Bits = 2;
    constexpr I f0298 = 2446, f0390 = 3196, f0541 = 4433, f0765 = 6270, f0899 = 7373, f1175 = 9633, f1501 = 12299,
                f1847 = 15137, f1961 = 16069, f2053 = 16819, f2562 = 20995, f3072 = 25172;
    auto descale = [](I x, int n) { return (x + (I{1} << (n - 1))) >> n; };
    std::array<I, 64> work{};
    for (std::size_t col = 0; col < 8; ++col) {
        auto dq = [&](std::size_t row) { return I{in[row * 8 + col]} * quant[row * 8 + col]; };
        bool acZero = true;
        for (std::size_t row = 1; row < 8; ++row)
            acZero = acZero && in[row * 8 + col] == 0;
        if (acZero) {
            const I dc = dq(0) * (1 << pass1Bits);
            for (std::size_t row = 0; row < 8; ++row)
                work[row * 8 + col] = dc;
            continue;
        }
        I z2 = dq(2);
        I z3 = dq(6);
        I z1 = (z2 + z3) * f0541;
        I tmp2 = z1 + z3 * -f1847;
        I tmp3 = z1 + z2 * f0765;
        z2 = dq(0);
        z3 = dq(4);
        I tmp0 = (z2 + z3) * (1 << constBits);
        I tmp1 = (z2 - z3) * (1 << constBits);
        const I tmp10 = tmp0 + tmp3;
        const I tmp13 = tmp0 - tmp3;
        const I tmp11 = tmp1 + tmp2;
        const I tmp12 = tmp1 - tmp2;
        tmp0 = dq(7);
        tmp1 = dq(5);
        tmp2 = dq(3);
        tmp3 = dq(1);
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        I z4 = tmp1 + tmp3;
        const I z5 = (z3 + z4) * f1175;
        tmp0 *= f0298;
        tmp1 *= f2053;
        tmp2 *= f3072;
        tmp3 *= f1501;
        z1 *= -f0899;
        z2 *= -f2562;
        z3 *= -f1961;
        z4 *= -f0390;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        constexpr int shift = constBits - pass1Bits;
        work[0 + col] = descale(tmp10 + tmp3, shift);
        work[56 + col] = descale(tmp10 - tmp3, shift);
        work[8 + col] = descale(tmp11 + tmp2, shift);
        work[48 + col] = descale(tmp11 - tmp2, shift);
        work[16 + col] = descale(tmp12 + tmp1, shift);
        work[40 + col] = descale(tmp12 - tmp1, shift);
        work[24 + col] = descale(tmp13 + tmp0, shift);
        work[32 + col] = descale(tmp13 - tmp0, shift);
    }
    auto clamp = [](I x) { return static_cast<std::uint8_t>(std::clamp<I>(x + 128, 0, 255)); };
    for (std::size_t row = 0; row < 8; ++row) {
        const I* w = work.data() + row * 8;
        std::uint8_t* o = out + row * stride;
        if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 && w[5] == 0 && w[6] == 0 && w[7] == 0) {
            const std::uint8_t value = clamp(descale(w[0], pass1Bits + 3));
            std::fill(o, o + 8, value);
            continue;
        }
        I z2 = w[2];
        I z3 = w[6];
        I z1 = (z2 + z3) * f0541;
        I tmp2 = z1 + z3 * -f1847;
        I tmp3 = z1 + z2 * f0765;
        I tmp0 = (w[0] + w[4]) * (1 << constBits);
        I tmp1 = (w[0] - w[4]) * (1 << constBits);
        const I tmp10 = tmp0 + tmp3;
        const I tmp13 = tmp0 - tmp3;
        const I tmp11 = tmp1 + tmp2;
        const I tmp12 = tmp1 - tmp2;
        tmp0 = w[7];
        tmp1 = w[5];
        tmp2 = w[3];
        tmp3 = w[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        I z4 = tmp1 + tmp3;
        const I z5 = (z3 + z4) * f1175;
        tmp0 *= f0298;
        tmp1 *= f2053;
        tmp2 *= f3072;
        tmp3 *= f1501;
        z1 *= -f0899;
        z2 *= -f2562;
        z3 *= -f1961;
        z4 *= -f0390;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        constexpr int shift = constBits + pass1Bits + 3;
        o[0] = clamp(descale(tmp10 + tmp3, shift));
        o[7] = clamp(descale(tmp10 - tmp3, shift));
        o[1] = clamp(descale(tmp11 + tmp2, shift));
        o[6] = clamp(descale(tmp11 - tmp2, shift));
        o[2] = clamp(descale(tmp12 + tmp1, shift));
        o[5] = clamp(descale(tmp12 - tmp1, shift));
        o[3] = clamp(descale(tmp13 + tmp0, shift));
        o[4] = clamp(descale(tmp13 - tmp0, shift));
    }
}

// A component's samples at full resolution, upsampled the way libjpeg does
// by default: triangle filters for 2x horizontal and/or vertical, plain
// replication for anything else.
std::vector<std::uint8_t> upsample(const std::vector<std::uint8_t>& plane, std::size_t stride, const Component& c,
                                   int maxH, int maxV, std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> out(std::size_t{width} * height);
    const std::size_t w = c.width;
    const std::size_t h = c.height;
    auto at = [&](std::size_t x, std::size_t y) -> int { return plane[y * stride + x]; };
    const bool h2 = maxH == 2 * c.h;
    const bool v2 = maxV == 2 * c.v;
    const bool h1 = maxH == c.h;
    const bool v1 = maxV == c.v;
    std::vector<int> sums(w);
    for (std::size_t y = 0; y < height; ++y) {
        std::uint8_t* row = out.data() + y * width;
        if (h1 && v1) {
            for (std::size_t x = 0; x < width; ++x)
                row[x] = static_cast<std::uint8_t>(at(x, y));
        } else if (h2 && v1 && w > 1) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::size_t i = x / 2;
                const int here = at(i, y) * 3;
                if (x % 2 == 0)
                    row[x] = static_cast<std::uint8_t>(i == 0 ? at(0, y) : (here + at(i - 1, y) + 1) >> 2);
                else
                    row[x] = static_cast<std::uint8_t>(i + 1 >= w ? at(i, y) : (here + at(i + 1, y) + 2) >> 2);
            }
        } else if ((h2 || h1) && v2 && w > 1) {
            const std::size_t inRow = y / 2;
            const std::size_t other = y % 2 == 0 ? (inRow == 0 ? 0 : inRow - 1) : std::min(inRow + 1, h - 1);
            if (h1) {
                const int bias = y % 2 == 0 ? 1 : 2;
                for (std::size_t x = 0; x < width; ++x)
                    row[x] = static_cast<std::uint8_t>((at(x, inRow) * 3 + at(x, other) + bias) >> 2);
                continue;
            }
            for (std::size_t i = 0; i < w; ++i)
                sums[i] = at(i, inRow) * 3 + at(i, other);
            for (std::size_t x = 0; x < width; ++x) {
                const std::size_t i = x / 2;
                if (x % 2 == 0)
                    row[x] = static_cast<std::uint8_t>(i == 0 ? (sums[0] * 4 + 8) >> 4
                                                              : (sums[i] * 3 + sums[i - 1] + 8) >> 4);
                else
                    row[x] = static_cast<std::uint8_t>(i + 1 >= w ? (sums[i] * 4 + 7) >> 4
                                                                  : (sums[i] * 3 + sums[i + 1] + 7) >> 4);
            }
        } else {
            const std::size_t sy = std::min(y * static_cast<std::size_t>(c.v) / static_cast<std::size_t>(maxV), h - 1);
            for (std::size_t x = 0; x < width; ++x)
                row[x] = static_cast<std::uint8_t>(
                    at(std::min(x * static_cast<std::size_t>(c.h) / static_cast<std::size_t>(maxH), w - 1), sy));
        }
    }
    return out;
}

TextureFile Decoder::output()
{
    std::vector<std::vector<std::uint8_t>> planes;
    for (Component& c : components_) {
        const std::size_t stride = c.blocksPerLine * 8;
        std::vector<std::uint8_t> plane(stride * c.blocksPerColumn * 8);
        for (std::size_t row = 0; row < c.blocksPerColumn; ++row)
            for (std::size_t column = 0; column < c.blocksPerLine; ++column)
                inverseDct(c.block(row, column), c.quant, plane.data() + row * 8 * stride + column * 8, stride);
        planes.push_back(upsample(plane, stride, c, maxH_, maxV_, width_, height_));
    }

    Image image = blankImage(width_, height_);
    const std::size_t pixels = std::size_t{width_} * height_;
    // jdcolor.c's fixed-point YCbCr to RGB.
    constexpr int scale = 16;
    constexpr int half = 1 << (scale - 1);
    auto fix = [](double x) { return static_cast<int>(std::lround(x * (1 << scale))); };
    const int crR = fix(1.40200);
    const int cbB = fix(1.77200);
    const int crG = fix(0.71414);
    const int cbG = fix(0.34414);
    auto clampByte = [](int x) { return static_cast<std::uint8_t>(std::clamp(x, 0, 255)); };
    auto ycc = [&](int y, int cb, int cr, std::uint8_t* out) {
        cb -= 128;
        cr -= 128;
        out[0] = clampByte(y + ((crR * cr + half) >> scale));
        out[1] = clampByte(y + ((-cbG * cb - crG * cr + half) >> scale));
        out[2] = clampByte(y + ((cbB * cb + half) >> scale));
    };
    const std::size_t count = components_.size();
    // libjpeg's rule: 3 components are YCbCr unless an Adobe marker says
    // otherwise, or (without JFIF) they're named R, G and B.
    const bool rgb = count == 3
                     && (adobeTransform_ == 0
                         || (adobeTransform_ < 0 && !jfif_ && components_[0].id == 'R' && components_[1].id == 'G'
                             && components_[2].id == 'B'));
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t* out = image.rgba.data() + i * 4;
        out[3] = 255;
        if (count == 1) {
            out[0] = out[1] = out[2] = planes[0][i];
        } else if (count == 3) {
            if (rgb) {
                out[0] = planes[0][i];
                out[1] = planes[1][i];
                out[2] = planes[2][i];
            } else {
                ycc(planes[0][i], planes[1][i], planes[2][i], out);
            }
        } else {
            // Adobe CMYK is stored inverted; YCCK is YCbCr for CMY plus K.
            std::array<std::uint8_t, 4> cmy{planes[0][i], planes[1][i], planes[2][i], 0};
            if (adobeTransform_ == 2) {
                ycc(planes[0][i], planes[1][i], planes[2][i], cmy.data());
                for (std::size_t c = 0; c < 3; ++c)
                    cmy[c] = static_cast<std::uint8_t>(255 - cmy[c]);
            }
            const int k = planes[3][i];
            for (std::size_t c = 0; c < 3; ++c)
                out[c] = static_cast<std::uint8_t>(cmy[c] * k / 255);
        }
    }

    std::string description;
    if (count == 1) {
        description = "gray";
    } else if (count == 4) {
        description = adobeTransform_ == 2 ? "YCCK" : "CMYK";
    } else {
        description = rgb ? "RGB" : "YCbCr";
        const Component& chroma = components_[1];
        if (components_[0].h == 2 * chroma.h && components_[0].v == 2 * chroma.v)
            description += " 4:2:0";
        else if (components_[0].h == 2 * chroma.h && components_[0].v == chroma.v)
            description += " 4:2:2";
        else if (components_[0].h == chroma.h && components_[0].v == chroma.v)
            description += " 4:4:4";
        else
            description += std::format(" {}x{}/{}x{}", components_[0].h, components_[0].v, chroma.h, chroma.v);
    }
    return singleImage(ImageFormat::Jpeg, std::format("{} {}", progressive_ ? "progressive" : "baseline", description),
                       std::move(image));
}

Result<TextureFile> Decoder::run()
{
    if (bytes_.size() < 4 || byteAt(bytes_, 0) != 0xff || byteAt(bytes_, 1) != 0xd8)
        return std::unexpected("not a JPEG file");
    std::size_t at = 2;
    bool sawScan = false;
    while (true) {
        // Fill bytes may come before a marker.
        if (at >= bytes_.size() || byteAt(bytes_, at) != 0xff)
            return std::unexpected(at >= bytes_.size() ? "JPEG ends before EOI" : "JPEG marker expected");
        while (at < bytes_.size() && byteAt(bytes_, at) == 0xff)
            ++at;
        if (at >= bytes_.size())
            return std::unexpected("JPEG ends before EOI");
        const unsigned marker = byteAt(bytes_, at++);
        if (marker == 0xd9)
            break;
        if ((marker & 0xf8) == 0xd0 || marker == 0x01)
            continue; // stray restart or TEM marker, no length
        auto data = segment(at);
        if (!data)
            return std::unexpected(data.error());
        Result<void> ok;
        switch (marker) {
        case 0xc0:
        case 0xc1:
        case 0xc2:
            ok = readFrame(*data, marker == 0xc2);
            break;
        case 0xc3:
        case 0xc5:
        case 0xc6:
        case 0xc7:
        case 0xcb:
        case 0xcd:
        case 0xce:
        case 0xcf:
            return std::unexpected("lossless and hierarchical JPEG aren't supported");
        case 0xc9:
        case 0xca:
            return std::unexpected("arithmetic-coded JPEG isn't supported");
        case 0xc4:
            ok = readHuffman(*data);
            break;
        case 0xdb:
            ok = readQuantization(*data);
            break;
        case 0xdd:
            if (data->size() < 2)
                return std::unexpected("JPEG restart interval is truncated");
            restartInterval_ = be16(*data, 0);
            break;
        case 0xda: {
            auto end = readScan(*data, at);
            if (!end)
                return std::unexpected(end.error());
            at = *end;
            sawScan = true;
            break;
        }
        case 0xe0:
            jfif_ = jfif_ || data->starts_with(std::string_view("JFIF\0", 5));
            break;
        case 0xee:
            if (data->size() >= 12 && data->starts_with("Adobe"))
                adobeTransform_ = byteAt(*data, 11);
            break;
        default:
            break; // APPn, COM, DNL and the like
        }
        if (!ok)
            return std::unexpected(ok.error());
    }
    if (!sawScan)
        return std::unexpected("JPEG has no image data");
    return output();
}

} // namespace

Result<TextureFile> decodeJpegFile(std::string_view bytes)
{
    Decoder decoder(bytes);
    return decoder.run();
}

} // namespace lunar::assets::detail
