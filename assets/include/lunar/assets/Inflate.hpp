#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace lunar::assets {

struct InflateLimits {
    std::size_t sizeHint = 0;                   // what to reserve, if the caller knows
    std::size_t maxSize = std::size_t{1} << 30; // more output than this is an error
};

// Raw DEFLATE (RFC 1951): stored, fixed and dynamic Huffman blocks. Fails on
// anything malformed or truncated instead of guessing.
std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> data,
                                                              InflateLimits limits = {});

// A zlib stream (RFC 1950): the header, DEFLATE data and an Adler-32 that
// has to match.
std::expected<std::vector<std::uint8_t>, std::string> zlibDecompress(std::span<const std::uint8_t> data,
                                                                     InflateLimits limits = {});

std::uint32_t adler32(std::span<const std::uint8_t> bytes, std::uint32_t adler = 1);

// A gzip stream (RFC 1952): the header with its optional fields, DEFLATE
// data, then a CRC-32 and the size, which have to match.
std::expected<std::vector<std::uint8_t>, std::string> gunzip(std::span<const std::uint8_t> data,
                                                             InflateLimits limits = {});

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);

} // namespace lunar::assets
