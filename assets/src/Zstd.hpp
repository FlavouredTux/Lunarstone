#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace lunar::assets::detail {

// Zstandard frames (RFC 8878) without dictionaries, as KTX2 supercompression
// uses them. Content checksums aren't verified.
std::expected<std::vector<std::uint8_t>, std::string> zstdDecompress(std::span<const std::uint8_t> data,
                                                                     std::size_t maxSize);

} // namespace lunar::assets::detail
