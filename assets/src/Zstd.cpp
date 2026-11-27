#include "Zstd.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <numeric>

namespace lunar::assets::detail {

namespace {

template <class T>
using Result = std::expected<T, std::string>;

using Bytes = std::span<const std::uint8_t>;

std::uint64_t loadLe(Bytes data, std::size_t at, std::size_t count)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i)
        value |= std::uint64_t{data[at + i]} << (8 * i);
    return value;
}

// Forward, least significant bit first: FSE table descriptions.
class ForwardBits {
public:
    explicit ForwardBits(Bytes data) : data_(data) {}
    unsigned read(int n)
    {
        unsigned value = 0;
        for (int i = 0; i < n; ++i, ++bit_) {
            const std::size_t byte = bit_ / 8;
            if (byte < data_.size())
                value |= static_cast<unsigned>((data_[byte] >> (bit_ % 8)) & 1) << i;
            else
                overran_ = true;
        }
        return value;
    }
    void rewind(int n) { bit_ -= static_cast<std::size_t>(n); }
    std::size_t bytesUsed() const { return (bit_ + 7) / 8; }
    bool overran() const { return overran_; }

private:
    Bytes data_;
    std::size_t bit_ = 0;
    bool overran_ = false;
};

// Backward, from the padding marker in the last byte down to bit 0: Huffman
// and FSE streams. Reading past the start gives zeros and a negative offset.
class BackwardBits {
public:
    static Result<BackwardBits> open(Bytes data)
    {
        if (data.empty() || data.back() == 0)
            return std::unexpected("zstd bitstream has no end marker");
        BackwardBits bits;
        bits.data_ = data;
        bits.offset_ = static_cast<std::int64_t>(data.size() * 8) - std::countl_zero(data.back()) - 1;
        return bits;
    }
    std::uint64_t read(int n)
    {
        const std::uint64_t value = peek(n);
        offset_ -= n;
        return value;
    }
    std::uint64_t peek(int n) const
    {
        if (n == 0)
            return 0;
        const std::int64_t start = offset_ - n;
        if (start >= 0)
            return bitsAt(static_cast<std::size_t>(start), n);
        if (offset_ <= 0)
            return 0;
        // Some bits are real, the rest below bit 0 are zeros.
        return bitsAt(0, static_cast<int>(offset_)) << (-start);
    }
    void skip(int n) { offset_ -= n; }
    std::int64_t offset() const { return offset_; }

private:
    std::uint64_t bitsAt(std::size_t start, int n) const
    {
        const std::size_t byte = start / 8;
        const std::size_t count = std::min<std::size_t>(8, data_.size() - byte);
        const std::uint64_t window = loadLe(data_, byte, count) >> (start % 8);
        return n >= 64 ? window : window & ((std::uint64_t{1} << n) - 1);
    }

    Bytes data_;
    std::int64_t offset_ = 0;
};

struct FseTable {
    int accuracyLog = 0;
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> bits;
    std::vector<std::uint16_t> base;

    Result<void> build(std::span<const std::int16_t> counts, int log)
    {
        const std::size_t size = std::size_t{1} << log;
        accuracyLog = log;
        symbols.assign(size, 0);
        bits.assign(size, 0);
        base.assign(size, 0);
        std::vector<std::uint32_t> next(counts.size(), 0);
        std::size_t high = size;
        for (std::size_t s = 0; s < counts.size(); ++s)
            if (counts[s] == -1) {
                if (high == 0)
                    return std::unexpected("invalid zstd FSE table");
                symbols[--high] = static_cast<std::uint8_t>(s);
                next[s] = 1;
            }
        const std::size_t step = (size >> 1) + (size >> 3) + 3;
        std::size_t pos = 0;
        for (std::size_t s = 0; s < counts.size(); ++s) {
            if (counts[s] <= 0)
                continue;
            next[s] = static_cast<std::uint32_t>(counts[s]);
            for (int i = 0; i < counts[s]; ++i) {
                symbols[pos] = static_cast<std::uint8_t>(s);
                do
                    pos = (pos + step) & (size - 1);
                while (pos >= high);
            }
        }
        if (pos != 0)
            return std::unexpected("invalid zstd FSE table");
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint32_t state = next[symbols[i]]++;
            if (state == 0)
                return std::unexpected("invalid zstd FSE table");
            const int n = log - (std::bit_width(state) - 1);
            if (n < 0)
                return std::unexpected("invalid zstd FSE table");
            bits[i] = static_cast<std::uint8_t>(n);
            base[i] = static_cast<std::uint16_t>((state << n) - size);
        }
        return {};
    }

    void buildRle(std::uint8_t symbol)
    {
        accuracyLog = 0;
        symbols.assign(1, symbol);
        bits.assign(1, 0);
        base.assign(1, 0);
    }
};

// Reads a table description; returns the bytes it took.
Result<std::size_t> readFseTable(Bytes data, int maxLog, std::size_t maxSymbols, FseTable& table)
{
    ForwardBits in(data);
    const int log = 5 + static_cast<int>(in.read(4));
    if (log > maxLog)
        return std::unexpected("zstd FSE accuracy is too high");
    std::vector<std::int16_t> counts;
    int remaining = 1 << log;
    while (remaining > 0 && counts.size() < maxSymbols) {
        const int bits = std::bit_width(static_cast<unsigned>(remaining + 1));
        unsigned value = in.read(bits);
        const unsigned lowerMask = (1u << (bits - 1)) - 1;
        const unsigned threshold = (1u << bits) - 1 - static_cast<unsigned>(remaining + 1);
        if ((value & lowerMask) < threshold) {
            in.rewind(1);
            value &= lowerMask;
        } else if (value > lowerMask) {
            value -= threshold;
        }
        const int probability = static_cast<int>(value) - 1;
        remaining -= std::abs(probability);
        counts.push_back(static_cast<std::int16_t>(probability));
        if (probability == 0) {
            unsigned repeat = in.read(2);
            while (true) {
                for (unsigned i = 0; i < repeat && counts.size() < maxSymbols; ++i)
                    counts.push_back(0);
                if (repeat != 3)
                    break;
                repeat = in.read(2);
            }
        }
        if (in.overran())
            return std::unexpected("zstd FSE table is truncated");
    }
    if (remaining != 0)
        return std::unexpected("invalid zstd FSE table");
    if (auto built = table.build(counts, log); !built)
        return std::unexpected(built.error());
    return in.bytesUsed();
}

class FseState {
public:
    void init(const FseTable& table, BackwardBits& in)
    {
        table_ = &table;
        state_ = static_cast<std::size_t>(in.read(table.accuracyLog));
    }
    std::uint8_t symbol() const { return table_->symbols[state_]; }
    void update(BackwardBits& in)
    {
        state_ = table_->base[state_] + static_cast<std::size_t>(in.read(table_->bits[state_]));
    }

private:
    const FseTable* table_ = nullptr;
    std::size_t state_ = 0;
};

struct HuffmanTable {
    int maxBits = 0;
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> lengths;
};

Result<std::size_t> readHuffmanTable(Bytes data, HuffmanTable& table)
{
    if (data.empty())
        return std::unexpected("zstd Huffman table is missing");
    const unsigned header = data[0];
    std::vector<std::uint8_t> weights;
    std::size_t used = 0;
    if (header < 128) {
        if (header + 1u > data.size())
            return std::unexpected("zstd Huffman table is truncated");
        const Bytes compressed = data.subspan(1, header);
        FseTable fse;
        auto tableBytes = readFseTable(compressed, 6, 256, fse);
        if (!tableBytes)
            return std::unexpected(tableBytes.error());
        auto in = BackwardBits::open(compressed.subspan(*tableBytes));
        if (!in)
            return std::unexpected(in.error());
        FseState one;
        FseState two;
        one.init(fse, *in);
        two.init(fse, *in);
        while (weights.size() < 255) {
            weights.push_back(one.symbol());
            one.update(*in);
            if (in->offset() < 0) {
                weights.push_back(two.symbol());
                break;
            }
            weights.push_back(two.symbol());
            two.update(*in);
            if (in->offset() < 0) {
                weights.push_back(one.symbol());
                break;
            }
        }
        used = 1 + header;
    } else {
        const std::size_t count = header - 127;
        if (1 + (count + 1) / 2 > data.size())
            return std::unexpected("zstd Huffman table is truncated");
        for (std::size_t i = 0; i < count; ++i)
            weights.push_back(static_cast<std::uint8_t>(i % 2 == 0 ? data[1 + i / 2] >> 4 : data[1 + i / 2] & 15));
        used = 1 + (count + 1) / 2;
    }
    if (weights.size() > 255)
        return std::unexpected("invalid zstd Huffman table");

    std::uint32_t total = 0;
    for (std::uint8_t w : weights) {
        if (w > 11)
            return std::unexpected("invalid zstd Huffman weight");
        if (w)
            total += 1u << (w - 1);
    }
    if (total == 0)
        return std::unexpected("invalid zstd Huffman table");
    const int maxBits = std::bit_width(total);
    if (maxBits > 11)
        return std::unexpected("invalid zstd Huffman table");
    const std::uint32_t left = (1u << maxBits) - total;
    if (!std::has_single_bit(left))
        return std::unexpected("invalid zstd Huffman table");
    weights.push_back(static_cast<std::uint8_t>(std::bit_width(left)));

    table.maxBits = maxBits;
    const std::size_t size = std::size_t{1} << maxBits;
    table.symbols.assign(size, 0);
    table.lengths.assign(size, 0);
    std::array<std::uint32_t, 13> rankCount{};
    for (std::uint8_t w : weights)
        if (w)
            ++rankCount[static_cast<std::size_t>(maxBits + 1 - w)];
    std::array<std::uint32_t, 13> rankStart{};
    for (int bits = maxBits; bits >= 1; --bits)
        rankStart[static_cast<std::size_t>(bits - 1)]
            = rankStart[static_cast<std::size_t>(bits)]
              + rankCount[static_cast<std::size_t>(bits)] * (1u << (maxBits - bits));
    for (std::size_t s = 0; s < weights.size(); ++s) {
        if (!weights[s])
            continue;
        const auto bits = static_cast<std::size_t>(maxBits + 1 - weights[s]);
        const std::uint32_t span = 1u << (static_cast<std::size_t>(maxBits) - bits);
        const std::uint32_t start = rankStart[bits];
        if (start + span > size)
            return std::unexpected("invalid zstd Huffman table");
        for (std::uint32_t i = 0; i < span; ++i) {
            table.symbols[start + i] = static_cast<std::uint8_t>(s);
            table.lengths[start + i] = static_cast<std::uint8_t>(bits);
        }
        rankStart[bits] += span;
    }
    return used;
}

Result<void> decodeHuffmanStream(Bytes stream, const HuffmanTable& table, std::size_t count,
                                 std::vector<std::uint8_t>& out)
{
    auto in = BackwardBits::open(stream);
    if (!in)
        return std::unexpected(in.error());
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(in->peek(table.maxBits));
        out.push_back(table.symbols[index]);
        in->skip(table.lengths[index]);
    }
    if (in->offset() != 0)
        return std::unexpected("zstd literal stream size mismatch");
    return {};
}

constexpr std::array<std::uint32_t, 36> literalBase{0,  1,  2,   3,   4,   5,    6,    7,    8,    9,     10,    11,
                                                    12, 13, 14,  15,  16,  18,   20,   22,   24,   28,    32,    40,
                                                    48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr std::array<std::uint8_t, 36> literalBits{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
                                                   1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr std::array<std::uint32_t, 53> matchBase{
    3,  4,  5,  6,  7,  8,  9,  10,  11,  12,  13,   14,   15,   16,   17,    18,    19,   20,
    21, 22, 23, 24, 25, 26, 27, 28,  29,  30,  31,   32,   33,   34,   35,    37,    39,   41,
    43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
constexpr std::array<std::uint8_t, 53> matchBits{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
                                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
                                                 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

constexpr std::array<std::int16_t, 36> literalDefault{4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
                                                      2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr std::array<std::int16_t, 53> matchDefault{1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1,  1, 1,
                                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
                                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
constexpr std::array<std::int16_t, 29> offsetDefault{1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
                                                     1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

constexpr int probabilitySum(std::span<const std::int16_t> counts)
{
    int sum = 0;
    for (std::int16_t c : counts)
        sum += c < 0 ? 1 : c;
    return sum;
}
static_assert(probabilitySum(literalDefault) == 64 && probabilitySum(matchDefault) == 64
              && probabilitySum(offsetDefault) == 32);

struct FrameState {
    std::array<std::uint64_t, 3> repeats{1, 4, 8};
    HuffmanTable huffman;
    bool haveHuffman = false;
    FseTable literals;
    FseTable offsets;
    FseTable matches;
    std::array<bool, 3> haveTables{};
};

Result<std::size_t> readSequenceTable(Bytes data, unsigned mode, std::span<const std::int16_t> defaults, int defaultLog,
                                      int maxLog, std::size_t maxSymbols, FseTable& table, bool& have)
{
    switch (mode) {
    case 0:
        have = true;
        if (auto built = table.build(defaults, defaultLog); !built)
            return std::unexpected(built.error());
        return 0;
    case 1:
        if (data.empty())
            return std::unexpected("zstd sequence table is truncated");
        if (data[0] >= maxSymbols)
            return std::unexpected("invalid zstd RLE symbol");
        table.buildRle(data[0]);
        have = true;
        return 1;
    case 2: {
        auto used = readFseTable(data, maxLog, maxSymbols, table);
        if (!used)
            return used;
        have = true;
        return used;
    }
    default:
        if (!have)
            return std::unexpected("zstd repeats a sequence table it never had");
        return 0;
    }
}

// Appends to out; matches reach back only as far as frameStart.
Result<void> decodeBlock(Bytes block, FrameState& frame, std::vector<std::uint8_t>& out, std::size_t frameStart,
                         std::size_t maxSize)
{
    // Literals.
    if (block.empty())
        return std::unexpected("zstd block is truncated");
    const unsigned type = block[0] & 3;
    const unsigned sizeFormat = (block[0] >> 2) & 3;
    std::vector<std::uint8_t> literals;
    std::size_t at = 0;
    if (type <= 1) {
        std::size_t size = 0;
        if (sizeFormat == 0 || sizeFormat == 2) {
            size = block[0] >> 3;
            at = 1;
        } else if (sizeFormat == 1) {
            if (block.size() < 2)
                return std::unexpected("zstd literals header is truncated");
            size = (block[0] >> 4) + (std::size_t{block[1]} << 4);
            at = 2;
        } else {
            if (block.size() < 3)
                return std::unexpected("zstd literals header is truncated");
            size = (block[0] >> 4) + (std::size_t{block[1]} << 4) + (std::size_t{block[2]} << 12);
            at = 3;
        }
        if (type == 0) {
            if (size > block.size() - at)
                return std::unexpected("zstd literals are truncated");
            literals.assign(block.begin() + static_cast<std::ptrdiff_t>(at),
                            block.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
        } else {
            if (at >= block.size())
                return std::unexpected("zstd literals are truncated");
            literals.assign(size, block[at]);
            at += 1;
        }
    } else {
        const std::size_t headerBytes = sizeFormat <= 1 ? 3 : sizeFormat == 2 ? 4 : 5;
        const int sizeBits = sizeFormat <= 1 ? 10 : sizeFormat == 2 ? 14 : 18;
        if (block.size() < headerBytes)
            return std::unexpected("zstd literals header is truncated");
        const std::uint64_t header = loadLe(block, 0, headerBytes) >> 4;
        const std::size_t regenerated = header & ((1u << sizeBits) - 1);
        const std::size_t compressed = (header >> sizeBits) & ((1u << sizeBits) - 1);
        const bool fourStreams = sizeFormat != 0;
        at = headerBytes;
        if (compressed > block.size() - at || regenerated > (128u << 10))
            return std::unexpected("zstd literals are truncated");
        Bytes streams = block.subspan(at, compressed);
        at += compressed;
        if (type == 2) {
            auto used = readHuffmanTable(streams, frame.huffman);
            if (!used)
                return std::unexpected(used.error());
            frame.haveHuffman = true;
            streams = streams.subspan(*used);
        } else if (!frame.haveHuffman) {
            return std::unexpected("zstd reuses a Huffman table it never had");
        }
        literals.reserve(regenerated);
        if (!fourStreams) {
            if (auto ok = decodeHuffmanStream(streams, frame.huffman, regenerated, literals); !ok)
                return ok;
        } else {
            if (streams.size() < 6)
                return std::unexpected("zstd literal jump table is truncated");
            const std::array<std::size_t, 3> sizes{loadLe(streams, 0, 2), loadLe(streams, 2, 2), loadLe(streams, 4, 2)};
            std::size_t offset = 6;
            const std::size_t each = (regenerated + 3) / 4;
            if (each * 3 > regenerated)
                return std::unexpected("zstd literal streams are too short");
            for (std::size_t i = 0; i < 4; ++i) {
                const std::size_t size = i < 3 ? sizes[i] : streams.size() - std::min(offset, streams.size());
                if (offset > streams.size() || size > streams.size() - offset)
                    return std::unexpected("zstd literal streams are truncated");
                const std::size_t count = i < 3 ? each : regenerated - each * 3;
                if (auto ok = decodeHuffmanStream(streams.subspan(offset, size), frame.huffman, count, literals); !ok)
                    return ok;
                offset += size;
            }
        }
    }

    // Sequences.
    if (at >= block.size())
        return std::unexpected("zstd sequences header is missing");
    std::size_t sequences = block[at++];
    if (sequences >= 128) {
        if (at >= block.size())
            return std::unexpected("zstd sequences header is truncated");
        if (sequences < 255) {
            sequences = ((sequences - 128) << 8) + block[at++];
        } else {
            if (block.size() - at < 2)
                return std::unexpected("zstd sequences header is truncated");
            sequences = block[at] + (std::size_t{block[at + 1]} << 8) + 0x7f00;
            at += 2;
        }
    }
    if (sequences == 0) {
        if (literals.size() > maxSize - out.size())
            return std::unexpected("zstd output is larger than expected");
        out.insert(out.end(), literals.begin(), literals.end());
        return {};
    }
    if (at >= block.size())
        return std::unexpected("zstd sequences header is truncated");
    const unsigned modes = block[at++];
    if (modes & 3)
        return std::unexpected("zstd reserved bits are set");
    auto literalTable = readSequenceTable(block.subspan(at), modes >> 6, literalDefault, 6, 9, 36, frame.literals,
                                          frame.haveTables[0]);
    if (!literalTable)
        return std::unexpected(literalTable.error());
    at += *literalTable;
    auto offsetTable = readSequenceTable(block.subspan(at), (modes >> 4) & 3, offsetDefault, 5, 8, 32, frame.offsets,
                                         frame.haveTables[1]);
    if (!offsetTable)
        return std::unexpected(offsetTable.error());
    at += *offsetTable;
    auto matchTable = readSequenceTable(block.subspan(at), (modes >> 2) & 3, matchDefault, 6, 9, 53, frame.matches,
                                        frame.haveTables[2]);
    if (!matchTable)
        return std::unexpected(matchTable.error());
    at += *matchTable;

    auto in = BackwardBits::open(block.subspan(at));
    if (!in)
        return std::unexpected(in.error());
    FseState literalState;
    FseState offsetState;
    FseState matchState;
    literalState.init(frame.literals, *in);
    offsetState.init(frame.offsets, *in);
    matchState.init(frame.matches, *in);
    std::size_t literalAt = 0;
    for (std::size_t i = 0; i < sequences; ++i) {
        const std::uint8_t offsetCode = offsetState.symbol();
        const std::uint8_t matchCode = matchState.symbol();
        const std::uint8_t literalCode = literalState.symbol();
        if (offsetCode > 31 || matchCode >= matchBase.size() || literalCode >= literalBase.size())
            return std::unexpected("invalid zstd sequence code");
        std::uint64_t offsetValue = (std::uint64_t{1} << offsetCode) + in->read(offsetCode);
        const std::size_t matchLength = matchBase[matchCode] + in->read(matchBits[matchCode]);
        const std::size_t literalLength = literalBase[literalCode] + in->read(literalBits[literalCode]);

        auto& reps = frame.repeats;
        std::uint64_t offset = 0;
        if (offsetValue > 3) {
            offset = offsetValue - 3;
            reps = {offset, reps[0], reps[1]};
        } else {
            if (literalLength == 0)
                ++offsetValue;
            if (offsetValue == 1) {
                offset = reps[0];
            } else if (offsetValue == 2) {
                offset = reps[1];
                reps = {offset, reps[0], reps[2]};
            } else if (offsetValue == 3) {
                offset = reps[2];
                reps = {offset, reps[0], reps[1]};
            } else {
                offset = reps[0] - 1;
                reps = {offset, reps[0], reps[1]};
            }
        }

        if (literalLength > literals.size() - literalAt)
            return std::unexpected("zstd sequence uses more literals than there are");
        if (literalLength + matchLength > maxSize - out.size())
            return std::unexpected("zstd output is larger than expected");
        out.insert(out.end(), literals.begin() + static_cast<std::ptrdiff_t>(literalAt),
                   literals.begin() + static_cast<std::ptrdiff_t>(literalAt + literalLength));
        literalAt += literalLength;
        if (offset == 0 || offset > out.size() - frameStart)
            return std::unexpected("zstd match points before the start of the data");
        const std::size_t from = out.size() - static_cast<std::size_t>(offset);
        const std::size_t to = out.size();
        out.resize(to + matchLength);
        for (std::size_t k = 0; k < matchLength; ++k)
            out[to + k] = out[from + k];

        if (i + 1 < sequences) {
            literalState.update(*in);
            matchState.update(*in);
            offsetState.update(*in);
        }
    }
    if (in->offset() != 0)
        return std::unexpected("zstd sequence stream size mismatch");
    const std::size_t rest = literals.size() - literalAt;
    if (rest > maxSize - out.size())
        return std::unexpected("zstd output is larger than expected");
    out.insert(out.end(), literals.begin() + static_cast<std::ptrdiff_t>(literalAt), literals.end());
    return {};
}

} // namespace

std::expected<std::vector<std::uint8_t>, std::string> zstdDecompress(std::span<const std::uint8_t> data,
                                                                     std::size_t maxSize)
{
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < 4)
            return std::unexpected("zstd frame is truncated");
        const auto magic = static_cast<std::uint32_t>(loadLe(data, at, 4));
        at += 4;
        if ((magic & 0xfffffff0u) == 0x184d2a50u) {
            if (data.size() - at < 4)
                return std::unexpected("zstd skippable frame is truncated");
            const std::uint64_t size = loadLe(data, at, 4);
            at += 4;
            if (size > data.size() - at)
                return std::unexpected("zstd skippable frame is truncated");
            at += static_cast<std::size_t>(size);
            continue;
        }
        if (magic != 0xfd2fb528u)
            return std::unexpected("not a zstd frame");
        if (at >= data.size())
            return std::unexpected("zstd frame header is truncated");
        const unsigned descriptor = data[at++];
        const unsigned sizeFlag = descriptor >> 6;
        const bool singleSegment = (descriptor >> 5) & 1;
        const bool checksum = (descriptor >> 2) & 1;
        const unsigned dictionaryFlag = descriptor & 3;
        if (descriptor & 8)
            return std::unexpected("zstd reserved bit is set");
        const std::size_t dictionaryBytes = dictionaryFlag == 3 ? 4 : dictionaryFlag;
        const std::size_t sizeBytes = sizeFlag == 0 ? (singleSegment ? 1 : 0) : std::size_t{1} << sizeFlag;
        const std::size_t headerRest = (singleSegment ? 0 : 1) + dictionaryBytes + sizeBytes;
        if (data.size() - at < headerRest)
            return std::unexpected("zstd frame header is truncated");
        at += singleSegment ? 0 : 1;
        if (dictionaryBytes && loadLe(data, at, dictionaryBytes) != 0)
            return std::unexpected("zstd dictionaries aren't supported");
        at += dictionaryBytes + sizeBytes;

        FrameState frame;
        const std::size_t frameStart = out.size();
        bool last = false;
        while (!last) {
            if (data.size() - at < 3)
                return std::unexpected("zstd block header is truncated");
            const auto header = static_cast<std::uint32_t>(loadLe(data, at, 3));
            at += 3;
            last = header & 1;
            const unsigned type = (header >> 1) & 3;
            const std::size_t size = header >> 3;
            if (type == 1) {
                if (at >= data.size())
                    return std::unexpected("zstd block is truncated");
                if (size > maxSize - out.size())
                    return std::unexpected("zstd output is larger than expected");
                out.insert(out.end(), size, data[at]);
                at += 1;
                continue;
            }
            if (size > data.size() - at || size > (128u << 10))
                return std::unexpected("zstd block is truncated");
            const Bytes block = data.subspan(at, size);
            at += size;
            if (type == 0) {
                if (size > maxSize - out.size())
                    return std::unexpected("zstd output is larger than expected");
                out.insert(out.end(), block.begin(), block.end());
            } else if (type == 2) {
                if (auto ok = decodeBlock(block, frame, out, frameStart, maxSize); !ok)
                    return std::unexpected(ok.error());
            } else {
                return std::unexpected("invalid zstd block type");
            }
        }
        if (checksum) {
            if (data.size() - at < 4)
                return std::unexpected("zstd checksum is truncated");
            at += 4;
        }
    }
    return out;
}

} // namespace lunar::assets::detail
