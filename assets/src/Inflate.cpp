#include <lunar/assets/Inflate.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace lunar::assets {

namespace {

template <class T>
using Result = std::expected<T, std::string>;

// Least significant bit first, as DEFLATE packs everything but Huffman codes.
// Past the end it reads zeros, and overran() tells.
class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    void refill()
    {
        // Eight bytes at once while they're there: the whole bytes that fit
        // count, and the bits loaded above them are the stream's next ones
        // anyway, so loading them again later changes nothing.
        if (std::endian::native == std::endian::little && data_.size() - std::min(pos_, data_.size()) >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, data_.data() + pos_, 8);
            bits_ |= word << count_;
            pos_ += static_cast<std::size_t>((63 - count_) >> 3);
            count_ |= 56;
            return;
        }
        while (count_ <= 56) {
            const std::uint64_t byte = pos_ < data_.size() ? data_[pos_] : 0;
            ++pos_;
            bits_ |= byte << count_;
            count_ += 8;
        }
    }
    // Callers refill first; 56 bits are always available after that.
    std::uint32_t peek(int n) const { return static_cast<std::uint32_t>(bits_ & ((std::uint64_t{1} << n) - 1)); }
    void consume(int n)
    {
        bits_ >>= n;
        count_ -= n;
    }
    std::uint32_t read(int n)
    {
        if (count_ < n)
            refill();
        const std::uint32_t value = peek(n);
        consume(n);
        return value;
    }
    void alignToByte() { consume(count_ % 8); }

    bool overran() const { return pos_ * 8 - static_cast<std::size_t>(count_) > data_.size() * 8; }
    // Hands out whole bytes for stored blocks and trailers, buffered ones first.
    bool takeBytes(std::size_t n, std::vector<std::uint8_t>& out)
    {
        while (n > 0 && count_ >= 8) {
            out.push_back(static_cast<std::uint8_t>(read(8)));
            --n;
        }
        if (n == 0)
            return true;
        // The buffer is empty here, so pos_ is exactly the next unread byte.
        // Bits a refill loaded above the count are bytes from pos_ on, which
        // this skips: they have to go too.
        bits_ = 0;
        if (pos_ > data_.size() || n > data_.size() - pos_)
            return false;
        out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return true;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::uint64_t bits_ = 0;
    int count_ = 0;
};

constexpr int fastBits = 10;

// Canonical Huffman decoding: one table lookup for codes up to fastBits long,
// a search by length for the rest.
class Huffman {
public:
    Result<void> build(std::span<const std::uint8_t> lengths)
    {
        std::array<int, 16> counts{};
        for (std::uint8_t length : lengths)
            ++counts[length];
        counts[0] = 0;
        int left = 1;
        for (int length = 1; length < 16; ++length) {
            left = (left << 1) - counts[static_cast<std::size_t>(length)];
            if (left < 0)
                return std::unexpected("over-subscribed Huffman code");
        }
        // Incomplete codes are allowed (a lone distance code is common);
        // decoding fails if a missing code turns up.
        std::array<int, 16> nextCode{};
        std::array<int, 16> nextIndex{};
        int code = 0;
        int index = 0;
        for (std::size_t length = 1; length < 16; ++length) {
            code = (code + counts[length - 1]) << 1;
            nextCode[length] = code;
            firstCode_[length] = code;
            firstIndex_[length] = index;
            index += counts[length];
            // Codes of this length are below this, left-justified to 16 bits.
            limit_[length] = (code + counts[length]) << (16 - length);
        }
        limit_[16] = 1 << 16;
        fast_.fill(0);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const int length = lengths[symbol];
            if (length == 0)
                continue;
            const auto at = static_cast<std::size_t>(length);
            symbols_[static_cast<std::size_t>(nextIndex[at]) + static_cast<std::size_t>(firstIndex_[at])]
                = static_cast<std::uint16_t>(symbol);
            ++nextIndex[at];
            const int value = nextCode[at]++;
            if (length <= fastBits) {
                const std::uint32_t reversed = reverse(static_cast<std::uint32_t>(value), length);
                for (std::uint32_t j = reversed; j < (1u << fastBits); j += 1u << length)
                    fast_[j] = static_cast<std::uint16_t>((length << 9) | static_cast<int>(symbol));
            }
        }
        return {};
    }

    // -1 if the bits match no code.
    int decode(BitReader& in) const
    {
        in.refill();
        const std::uint16_t entry = fast_[in.peek(fastBits)];
        if (entry != 0) {
            in.consume(entry >> 9);
            return entry & 511;
        }
        const auto reversed = static_cast<int>(reverse(in.peek(16), 16));
        for (int length = fastBits + 1; length < 16; ++length) {
            if (reversed < limit_[static_cast<std::size_t>(length)]) {
                const auto at = static_cast<std::size_t>(length);
                const int index = firstIndex_[at] + (reversed >> (16 - length)) - firstCode_[at];
                if (index < 0 || static_cast<std::size_t>(index) >= symbols_.size())
                    return -1;
                in.consume(length);
                return symbols_[static_cast<std::size_t>(index)];
            }
        }
        return -1;
    }

private:
    static std::uint32_t reverse(std::uint32_t value, int bits)
    {
        std::uint32_t out = 0;
        for (int i = 0; i < bits; ++i) {
            out = (out << 1) | (value & 1);
            value >>= 1;
        }
        return out;
    }

    std::array<std::uint16_t, 1 << fastBits> fast_{}; // length << 9 | symbol, 0 when longer
    std::array<int, 17> limit_{};
    std::array<int, 16> firstCode_{};
    std::array<int, 16> firstIndex_{};
    std::array<std::uint16_t, 288> symbols_{};
};

constexpr std::array<std::uint16_t, 29> lengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> lengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> distanceBase{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                     33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distanceExtra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// out holds `size` bytes and room to spare beyond them: matches copy eight
// bytes at a time and may write up to seven past their end.
//
// The bit reader and the output's pointer and size are locals here, written
// back at the end: the output is bytes, and a byte store may alias anything,
// so with them behind references every store made the compiler load them
// all again for the next symbol.
Result<void> inflateBlock(BitReader& reader, const Huffman& literals, const Huffman& distances,
                          std::vector<std::uint8_t>& out, std::size_t& outSize, std::size_t maxSize)
{
    BitReader in = reader;
    std::size_t size = outSize;
    std::uint8_t* data = out.data();
    std::size_t available = out.size();
    auto room = [&](std::size_t need) {
        if (need > available) {
            out.resize(std::max({need, out.size() * 2, out.capacity()}));
            data = out.data();
            available = out.size();
        }
    };
    auto done = [&](Result<void> result) {
        reader = in;
        outSize = size;
        return result;
    };
    while (true) {
        const int symbol = literals.decode(in);
        if (symbol < 0)
            return done(std::unexpected("invalid literal/length code"));
        if (in.overran())
            return done(std::unexpected("deflate data is truncated"));
        if (symbol < 256) {
            if (size >= maxSize)
                return done(std::unexpected("inflated data is larger than expected"));
            room(size + 1);
            data[size++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        if (symbol == 256)
            return done({});
        const auto lengthCode = static_cast<std::size_t>(symbol - 257);
        if (lengthCode >= lengthBase.size())
            return done(std::unexpected("invalid length code"));
        const std::size_t length = lengthBase[lengthCode] + in.read(lengthExtra[lengthCode]);
        const int distanceCode = distances.decode(in);
        if (distanceCode < 0 || static_cast<std::size_t>(distanceCode) >= distanceBase.size())
            return done(std::unexpected("invalid distance code"));
        const auto dc = static_cast<std::size_t>(distanceCode);
        const std::size_t distance = distanceBase[dc] + in.read(distanceExtra[dc]);
        if (distance > size)
            return done(std::unexpected("distance points before the start of the data"));
        if (length > maxSize - size)
            return done(std::unexpected("inflated data is larger than expected"));
        room(size + length + 8);
        std::uint8_t* to = data + size;
        const std::uint8_t* from = to - distance;
        if (distance >= 8) {
            // Each eight bytes come from at least eight back: already written.
            for (std::size_t i = 0; i < length; i += 8)
                std::memcpy(to + i, from + i, 8);
        } else if (distance == 1) {
            std::memset(to, *from, length);
        } else {
            // Byte by byte: the copy overlaps what it's writing.
            for (std::size_t i = 0; i < length; ++i)
                to[i] = from[i];
        }
        size += length;
    }
}

Result<void> readDynamicTables(BitReader& in, Huffman& literals, Huffman& distances)
{
    const std::uint32_t literalCount = in.read(5) + 257;
    const std::uint32_t distanceCount = in.read(5) + 1;
    const std::uint32_t lengthCodeCount = in.read(4) + 4;
    if (literalCount > 286 || distanceCount > 30)
        return std::unexpected("too many Huffman codes");

    static constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                        11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::array<std::uint8_t, 19> lengthLengths{};
    for (std::uint32_t i = 0; i < lengthCodeCount; ++i)
        lengthLengths[order[i]] = static_cast<std::uint8_t>(in.read(3));
    Huffman lengthCode;
    if (auto built = lengthCode.build(lengthLengths); !built)
        return built;

    std::array<std::uint8_t, 286 + 30> lengths{};
    std::uint32_t count = 0;
    while (count < literalCount + distanceCount) {
        const int symbol = lengthCode.decode(in);
        if (symbol < 0)
            return std::unexpected("invalid code length code");
        if (symbol < 16) {
            lengths[count++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t value = 0;
        std::uint32_t repeat = 0;
        if (symbol == 16) {
            if (count == 0)
                return std::unexpected("code length repeat with nothing before it");
            value = lengths[count - 1];
            repeat = 3 + in.read(2);
        } else if (symbol == 17) {
            repeat = 3 + in.read(3);
        } else {
            repeat = 11 + in.read(7);
        }
        if (count + repeat > literalCount + distanceCount)
            return std::unexpected("code lengths run past the table");
        for (std::uint32_t i = 0; i < repeat; ++i)
            lengths[count++] = value;
    }
    if (in.overran())
        return std::unexpected("deflate data is truncated");
    if (lengths[256] == 0)
        return std::unexpected("no end-of-block code");
    if (auto built = literals.build(std::span(lengths).first(literalCount)); !built)
        return built;
    return distances.build(std::span(lengths).subspan(literalCount, distanceCount));
}

const Huffman& fixedLiterals()
{
    static const Huffman table = [] {
        std::array<std::uint8_t, 288> lengths{};
        for (std::size_t i = 0; i < 288; ++i)
            lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        Huffman h;
        if (!h.build(lengths))
            std::abort(); // the fixed tables are valid
        return h;
    }();
    return table;
}

const Huffman& fixedDistances()
{
    static const Huffman table = [] {
        std::array<std::uint8_t, 30> lengths{};
        lengths.fill(5);
        Huffman h;
        if (!h.build(lengths))
            std::abort(); // the fixed tables are valid
        return h;
    }();
    return table;
}

Result<void> inflateStream(BitReader& in, std::vector<std::uint8_t>& out, std::size_t maxSize)
{
    // Blocks write through size (see inflateBlock); out gets it back at the end.
    std::size_t size = out.size();
    bool last = false;
    while (!last) {
        last = in.read(1) != 0;
        const std::uint32_t type = in.read(2);
        if (type == 0) {
            in.alignToByte();
            const std::uint32_t length = in.read(16);
            const std::uint32_t inverse = in.read(16);
            if ((length ^ 0xffff) != inverse)
                return std::unexpected("stored block length doesn't match its complement");
            if (length > maxSize - size)
                return std::unexpected("inflated data is larger than expected");
            out.resize(size);
            if (!in.takeBytes(length, out))
                return std::unexpected("deflate data is truncated");
            size = out.size();
        } else if (type == 1) {
            if (auto block = inflateBlock(in, fixedLiterals(), fixedDistances(), out, size, maxSize); !block)
                return block;
        } else if (type == 2) {
            Huffman literals;
            Huffman distances;
            if (auto tables = readDynamicTables(in, literals, distances); !tables)
                return tables;
            if (auto block = inflateBlock(in, literals, distances, out, size, maxSize); !block)
                return block;
        } else {
            return std::unexpected("invalid deflate block type");
        }
        if (in.overran())
            return std::unexpected("deflate data is truncated");
    }
    out.resize(size);
    return {};
}

} // namespace

std::uint32_t adler32(std::span<const std::uint8_t> bytes, std::uint32_t adler)
{
    std::uint32_t a = adler & 0xffff;
    std::uint32_t b = adler >> 16;
    while (!bytes.empty()) {
        // The largest run that can't overflow 32 bits before the modulo.
        const std::size_t run = std::min<std::size_t>(bytes.size(), 5552);
        std::span<const std::uint8_t> left = bytes.first(run);
#if defined(__SSE2__)
        // Sixteen bytes x0..x15 at a time: a byte at a time, b would gain
        // 16a + 16x0 + 15x1 + ... + x15 and a their sum, which these are,
        // and the same values the byte loop has at each sixteenth byte.
        const __m128i zero = _mm_setzero_si128();
        const __m128i high = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10, 9);
        const __m128i low = _mm_setr_epi16(8, 7, 6, 5, 4, 3, 2, 1);
        while (left.size() >= 16) {
            __m128i v;
            std::memcpy(&v, left.data(), 16);
            const __m128i sums = _mm_sad_epu8(v, zero);
            const __m128i weighted = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(v, zero), high),
                                                   _mm_madd_epi16(_mm_unpackhi_epi8(v, zero), low));
            const __m128i pairs = _mm_add_epi32(weighted, _mm_shuffle_epi32(weighted, 0x4e));
            const auto weightedSum
                = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_add_epi32(pairs, _mm_shuffle_epi32(pairs, 0xb1))));
            const auto sum
                = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_add_epi32(sums, _mm_shuffle_epi32(sums, 0x4e))));
            b += 16 * a + weightedSum;
            a += sum;
            left = left.subspan(16);
        }
#endif
        for (std::uint8_t byte : left) {
            a += byte;
            b += a;
        }
        a %= 65521;
        b %= 65521;
        bytes = bytes.subspan(run);
    }
    return (b << 16) | a;
}

std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> data, InflateLimits limits)
{
    std::vector<std::uint8_t> out;
    // Eight more: inflateBlock copies matches eight bytes at a time.
    out.reserve(std::min({limits.sizeHint, limits.maxSize, data.size() * 1032 + 64}) + 8);
    BitReader in(data);
    if (auto done = inflateStream(in, out, limits.maxSize); !done)
        return std::unexpected(done.error());
    return out;
}

std::expected<std::vector<std::uint8_t>, std::string> zlibDecompress(std::span<const std::uint8_t> data,
                                                                     InflateLimits limits)
{
    if (data.size() < 6)
        return std::unexpected("zlib stream is truncated");
    const unsigned method = data[0];
    const unsigned flags = data[1];
    if ((method & 15) != 8 || (method >> 4) > 7)
        return std::unexpected("zlib stream isn't deflate");
    if ((method * 256 + flags) % 31 != 0)
        return std::unexpected("zlib header check failed");
    if (flags & 0x20)
        return std::unexpected("zlib preset dictionaries aren't supported");

    std::vector<std::uint8_t> out;
    // Eight more: inflateBlock copies matches eight bytes at a time.
    out.reserve(std::min({limits.sizeHint, limits.maxSize, data.size() * 1032 + 64}) + 8);
    BitReader in(data.subspan(2));
    if (auto done = inflateStream(in, out, limits.maxSize); !done)
        return std::unexpected(done.error());
    in.alignToByte();
    std::vector<std::uint8_t> trailer;
    if (!in.takeBytes(4, trailer) || in.overran())
        return std::unexpected("zlib stream has no checksum");
    const std::uint32_t expected = (std::uint32_t{trailer[0]} << 24) | (std::uint32_t{trailer[1]} << 16)
                                   | (std::uint32_t{trailer[2]} << 8) | trailer[3];
    if (adler32(out) != expected)
        return std::unexpected("zlib checksum mismatch");
    return out;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> entries{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            entries[i] = c;
        }
        return entries;
    }();
    crc = ~crc;
    for (std::uint8_t byte : bytes)
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::expected<std::vector<std::uint8_t>, std::string> gunzip(std::span<const std::uint8_t> data, InflateLimits limits)
{
    if (data.size() < 18)
        return std::unexpected("gzip stream is truncated");
    if (data[0] != 0x1f || data[1] != 0x8b || data[2] != 8)
        return std::unexpected("not a gzip deflate stream");
    const unsigned flags = data[3];
    std::size_t at = 10;
    if (flags & 4) { // FEXTRA: a length, then that many bytes
        if (at + 2 > data.size())
            return std::unexpected("gzip stream is truncated");
        at += 2 + (std::size_t{data[at]} | std::size_t{data[at + 1]} << 8);
    }
    for (unsigned field : {8u, 16u}) { // FNAME, FCOMMENT: zero-terminated
        if (!(flags & field))
            continue;
        while (at < data.size() && data[at] != 0)
            ++at;
        ++at;
    }
    if (flags & 2) // FHCRC
        at += 2;
    if (at + 8 > data.size())
        return std::unexpected("gzip stream is truncated");

    std::vector<std::uint8_t> out;
    // Eight more: inflateBlock copies matches eight bytes at a time.
    out.reserve(std::min({limits.sizeHint, limits.maxSize, data.size() * 1032 + 64}) + 8);
    BitReader in(data.subspan(at));
    if (auto done = inflateStream(in, out, limits.maxSize); !done)
        return std::unexpected(done.error());
    in.alignToByte();
    std::vector<std::uint8_t> trailer;
    if (!in.takeBytes(8, trailer) || in.overran())
        return std::unexpected("gzip stream has no trailer");
    auto word = [&](std::size_t i) {
        return std::uint32_t{trailer[i]} | std::uint32_t{trailer[i + 1]} << 8 | std::uint32_t{trailer[i + 2]} << 16
               | std::uint32_t{trailer[i + 3]} << 24;
    };
    if (crc32(out) != word(0) || static_cast<std::uint32_t>(out.size()) != word(4))
        return std::unexpected("gzip checksum mismatch");
    return out;
}

} // namespace lunar::assets
