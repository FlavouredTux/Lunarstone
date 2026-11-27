#include "framework.hpp"

#include <lunar/Lz4.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace lunar;
using namespace std::string_literals;

TEST(lz4_literals_only)
{
    auto out = lz4::decompressBlock("\x50hello"s, 5);
    CHECK(out.has_value());
    CHECK_EQ(*out, "hello");
}

TEST(lz4_overlapping_match_repeats)
{
    // "a", then copy 9 bytes from 1 back (overlapping), then "b".
    auto out = lz4::decompressBlock("\x15" "a" "\x01\x00" "\x10" "b"s, 11);
    CHECK(out.has_value());
    CHECK_EQ(*out, "aaaaaaaaaab");
}

TEST(lz4_long_lengths)
{
    // 15 + 5 = 20 literals.
    auto literals = lz4::decompressBlock("\xF0\x05" "abcdefghijklmnopqrst"s, 20);
    CHECK(literals.has_value());
    CHECK_EQ(*literals, "abcdefghijklmnopqrst");

    // Match length 4 + 15 + 255 + 1 = 275.
    auto match = lz4::decompressBlock("\x1F" "x" "\x01\x00" "\xFF\x01" "\x10" "y"s, 277);
    CHECK(match.has_value());
    CHECK_EQ(*match, std::string(276, 'x') + "y");
}

TEST(lz4_rejects_bad_blocks)
{
    CHECK_EQ(lz4::decompressBlock("\x50hello"s, 4).error(), "block decoded to 5 bytes, expected 4");
    CHECK_EQ(lz4::decompressBlock("\x50hel"s, 5).error(), "literals run past the end of the block");
    CHECK_EQ(lz4::decompressBlock("\x10" "a" "\x00\x00"s, 10).error(), "match offset 0 points before the start");
    CHECK_EQ(lz4::decompressBlock("\x10" "a" "\x05\x00"s, 10).error(), "match offset 5 points before the start");
    CHECK_EQ(lz4::decompressBlock("\x15" "a" "\x01\x00"s, 5).error(), "block decodes to more bytes than expected");
    CHECK_EQ(lz4::decompressBlock("\x10" "a" "\x01"s, 10).error(), "truncated match offset");
    CHECK_EQ(lz4::decompressBlock("\xF0"s, 10).error(), "length runs past the end of the block");
}

// Matches at every short offset and of every length around the eight-byte
// steps, each built by hand: the decoder copies a match closer than eight
// bytes as a repeat of its first bytes, and a farther one eight at a time.
TEST(lz4_matches_at_every_offset_and_length)
{
    for (std::size_t offset = 1; offset <= 20; ++offset) {
        for (std::size_t length = 4; length <= 40; ++length) {
            std::string literals;
            for (std::size_t i = 0; i < offset; ++i)
                literals += static_cast<char>('a' + i);
            std::string expected = literals;
            for (std::size_t i = 0; i < length; ++i)
                expected += expected[expected.size() - offset];
            expected += "end!z";
            // Token: the literals, the match (length - 4, extended past 15), then the last literals.
            std::string block;
            const std::size_t extra = length - 4;
            block += static_cast<char>((literals.size() < 15 ? literals.size() << 4 : 0xf0)
                                       | (extra < 15 ? extra : 0x0f));
            if (literals.size() >= 15)
                block += static_cast<char>(literals.size() - 15);
            block += literals;
            block += static_cast<char>(offset);
            block += '\0';
            if (extra >= 15)
                block += static_cast<char>(extra - 15);
            block += "\x50"s + "end!z";
            auto out = lz4::decompressBlock(block, expected.size());
            CHECK(out.has_value());
            if (out)
                CHECK_EQ(*out, expected);
        }
    }
}

// What the compressor makes of repeating and random stretches comes back.
TEST(lz4_round_trips_the_compressor)
{
    std::string raw;
    std::uint32_t state = 12345;
    for (int stretch = 0; stretch < 200; ++stretch) {
        state = state * 1664525u + 1013904223u;
        const std::size_t period = 1 + (state >> 24) % 12;
        const std::size_t length = 8 + (state >> 8) % 300;
        if (stretch % 3 == 0) {
            for (std::size_t i = 0; i < length; ++i) {
                state = state * 1664525u + 1013904223u;
                raw += static_cast<char>(state >> 24);
            }
        } else {
            for (std::size_t i = 0; i < length; ++i)
                raw += static_cast<char>('A' + i % period + static_cast<std::size_t>(stretch % 7));
        }
    }
    const std::string block = lz4::compressBlock(raw);
    CHECK(block.size() < raw.size());
    auto out = lz4::decompressBlock(block, raw.size());
    CHECK(out.has_value());
    if (out)
        CHECK(*out == raw);
}
