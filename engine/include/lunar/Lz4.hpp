#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace lunar::lz4 {

// One raw LZ4 block (no frame header), compressed: sequences of a token byte
// (the literal and match lengths, 15 meaning "more follows") followed by the
// literals and a two byte match offset. The last sequence is literals only,
// and a block holds no decompressed size: the caller knows it, as
// decompressBlock needs.
std::string compressBlock(std::string_view raw);

// Decodes one raw LZ4 block (no frame header). It has to expand to exactly
// decodedSize bytes, which the container always knows up front.
std::expected<std::string, std::string> decompressBlock(std::string_view block, std::size_t decodedSize);

} // namespace lunar::lz4
