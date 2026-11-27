#include <lunar/Lz4.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>

namespace lunar::lz4 {

namespace {

// The format's limits: a match can't reach the last bytes (its lengths and
// offset have to fit after it), and its offset is two bytes wide.
constexpr std::size_t lastLiterals = 5;
constexpr std::size_t minMatch = 4;
constexpr std::size_t maxOffset = 65535;

constexpr unsigned hashBits = 16;
constexpr std::size_t hashSize = std::size_t{1} << hashBits;

// A four byte sequence read little-end, which is what the format's reference
// compressor hashes.
std::uint32_t sequenceAt(const std::uint8_t* data, std::size_t at)
{
    std::uint32_t sequence = 0;
    for (std::size_t i = 0; i < minMatch; ++i)
        sequence |= static_cast<std::uint32_t>(data[at + i]) << (8 * i);
    return sequence;
}

// The lengths a token nibble can't hold are 255 at a time after the 15.
void appendLength(std::string& out, std::size_t length)
{
    while (length >= 255) {
        out.push_back(static_cast<char>(255));
        length -= 255;
    }
    out.push_back(static_cast<char>(length));
}

// One token byte, its literals and the match that follows them. The match
// length is what is left after the minimum four.
void appendSequence(std::string& out, std::string_view literals, std::size_t matchLength, std::size_t offset)
{
    const std::size_t match = matchLength - minMatch;
    out.push_back(static_cast<char>((literals.size() < 15 ? literals.size() << 4 : 0xf0) | (match < 15 ? match : 0x0f)));
    if (literals.size() >= 15)
        appendLength(out, literals.size() - 15);
    out.append(literals);
    out.push_back(static_cast<char>(offset & 0xff));
    out.push_back(static_cast<char>((offset >> 8) & 0xff));
    if (match >= 15)
        appendLength(out, match - 15);
}

// The decoder as it was, a byte at a time into a growing string: only for a
// block whose literals run past the size it should decode to, where it tells
// how many bytes the whole block does come to.
std::expected<std::string, std::string> decompressSlowly(std::string_view block, std::size_t decodedSize)
{
    std::string out;
    out.reserve(decodedSize);
    std::size_t pos = 0;

    auto byte = [&] { return static_cast<std::uint8_t>(block[pos++]); };
    // 15 in a token nibble means "keep adding bytes until one isn't 255".
    auto extendedLength = [&](std::size_t length) -> std::expected<std::size_t, std::string> {
        if (length != 15)
            return length;
        std::uint8_t more = 0;
        do {
            if (pos >= block.size())
                return std::unexpected("length runs past the end of the block");
            more = byte();
            length += more;
        } while (more == 255);
        return length;
    };

    while (pos < block.size()) {
        std::uint8_t token = byte();

        auto literals = extendedLength(token >> 4);
        if (!literals)
            return std::unexpected(literals.error());
        if (*literals > block.size() - pos)
            return std::unexpected("literals run past the end of the block");
        out.append(block.substr(pos, *literals));
        pos += *literals;

        if (pos == block.size())
            break; // the last sequence is literals only

        if (block.size() - pos < 2)
            return std::unexpected("truncated match offset");
        std::size_t offset = byte();
        offset |= static_cast<std::size_t>(byte()) << 8;
        if (offset == 0 || offset > out.size())
            return std::unexpected(std::format("match offset {} points before the start", offset));

        auto length = extendedLength(token & 0x0F);
        if (!length)
            return std::unexpected(length.error());
        std::size_t matchLength = *length + 4;
        if (matchLength > decodedSize - out.size())
            return std::unexpected("block decodes to more bytes than expected");

        // Byte by byte: matches may overlap the bytes they produce.
        std::size_t from = out.size() - offset;
        for (std::size_t i = 0; i < matchLength; ++i)
            out.push_back(out[from + i]);
    }

    if (out.size() != decodedSize)
        return std::unexpected(std::format("block decoded to {} bytes, expected {}", out.size(), decodedSize));
    return out;
}

} // namespace

std::string compressBlock(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size() + raw.size() / 255 + 16);
    if (raw.empty())
        return out;

    const auto* data = reinterpret_cast<const std::uint8_t*>(raw.data());
    // No match starts in the last stretch: the lengths and the offset after it
    // would run past the end of the block.
    const std::size_t limit = raw.size() > lastLiterals + minMatch ? raw.size() - lastLiterals - minMatch : 0;

    // Where each four byte sequence was last seen, so a repeat is one hash
    // lookup away. limit + 1 is past every position, and means "nothing".
    std::array<std::size_t, hashSize> table;
    table.fill(limit + 1);

    std::size_t anchor = 0; // where this sequence's literals start
    std::size_t pos = 0;
    while (pos < limit) {
        const std::uint32_t sequence = sequenceAt(data, pos);
        const std::size_t hash = (sequence * 2654435761u) >> (32 - hashBits);
        const std::size_t candidate = table[hash];
        table[hash] = pos;
        if (candidate > pos || pos - candidate > maxOffset || sequenceAt(data, candidate) != sequence) {
            ++pos;
            continue;
        }
        // Matches may overlap what they produce, so the comparison walks on
        // into bytes this very match is about to copy.
        std::size_t length = minMatch;
        while (pos + length < limit + lastLiterals && data[candidate + length] == data[pos + length])
            ++length;
        appendSequence(out, raw.substr(anchor, pos - anchor), length, pos - candidate);
        anchor = pos + length;
        pos = anchor;
    }

    // The last sequence: literals to the end and no match after them.
    const std::size_t literals = raw.size() - anchor;
    out.push_back(static_cast<char>(literals < 15 ? literals << 4 : 0xf0));
    if (literals >= 15)
        appendLength(out, literals - 15);
    out.append(raw.substr(anchor));
    return out;
}

std::expected<std::string, std::string> decompressBlock(std::string_view block, std::size_t decodedSize)
{
    // Straight into the string's buffer, with eight bytes to spare: a match
    // at least eight back copies eight bytes at a time and may write up to
    // seven past its end, still inside the buffer.
    std::string out;
    std::string error;
    bool overflowed = false;
    out.resize_and_overwrite(decodedSize + 8, [&](char* buffer, std::size_t) -> std::size_t {
        auto* const to = reinterpret_cast<std::uint8_t*>(buffer);
        const auto* const in = reinterpret_cast<const std::uint8_t*>(block.data());
        std::size_t size = 0;
        std::size_t pos = 0;
        // 15 in a token nibble means "keep adding bytes until one isn't 255".
        auto extendedLength = [&](std::size_t& length) {
            if (length != 15)
                return true;
            std::uint8_t more = 0;
            do {
                if (pos >= block.size()) {
                    error = "length runs past the end of the block";
                    return false;
                }
                more = in[pos++];
                length += more;
            } while (more == 255);
            return true;
        };

        while (pos < block.size()) {
            const std::uint8_t token = in[pos++];

            std::size_t literals = token >> 4;
            if (!extendedLength(literals))
                return size;
            if (literals > block.size() - pos) {
                error = "literals run past the end of the block";
                return size;
            }
            // Literals past the size said: the byte-at-a-time decoder below
            // runs the whole block again, for the size it does come to.
            if (literals > decodedSize - size) {
                overflowed = true;
                return 0;
            }
            std::memcpy(to + size, in + pos, literals);
            size += literals;
            pos += literals;

            if (pos == block.size())
                break; // the last sequence is literals only

            if (block.size() - pos < 2) {
                error = "truncated match offset";
                return size;
            }
            std::size_t offset = in[pos];
            offset |= static_cast<std::size_t>(in[pos + 1]) << 8;
            pos += 2;
            if (offset == 0 || offset > size) {
                error = std::format("match offset {} points before the start", offset);
                return size;
            }

            std::size_t length = token & 0x0F;
            if (!extendedLength(length))
                return size;
            const std::size_t matchLength = length + 4;
            if (matchLength > decodedSize - size) {
                error = "block decodes to more bytes than expected";
                return size;
            }

            std::uint8_t* const at = to + size;
            const std::uint8_t* const from = at - offset;
            if (offset >= 8) {
                // Each eight bytes come from at least eight back: already written.
                for (std::size_t i = 0; i < matchLength; i += 8)
                    std::memcpy(at + i, from + i, 8);
            } else {
                // The match overlaps the bytes it produces: it repeats the
                // offset's bytes, and so any multiple of the offset. Byte by
                // byte up to the first multiple of eight or more, then eight
                // bytes at a time from that far back. Most matches in the
                // binary formats' interleaved columns are these.
                const std::size_t period = offset * ((8 + offset - 1) / offset);
                const std::size_t head = std::min(period, matchLength);
                for (std::size_t i = 0; i < head; ++i)
                    at[i] = from[i];
                for (std::size_t i = head; i < matchLength; i += 8)
                    std::memcpy(at + i, at + i - period, 8);
            }
            size += matchLength;
        }
        return size;
    });
    if (overflowed)
        return decompressSlowly(block, decodedSize);
    if (!error.empty())
        return std::unexpected(error);
    if (out.size() != decodedSize)
        return std::unexpected(std::format("block decoded to {} bytes, expected {}", out.size(), decodedSize));
    return out;
}

} // namespace lunar::lz4
