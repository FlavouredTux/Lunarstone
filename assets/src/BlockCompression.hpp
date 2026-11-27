#pragma once

// GPU block-compressed formats, decoded to RGBA8 on the CPU.

#include <lunar/Image.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lunar::assets::detail {

enum class BlockFormat : std::uint8_t {
    Bc1, // DXT1, with 1-bit alpha
    Bc2, // DXT3
    Bc3, // DXT5
    Bc4, // one channel, to gray
    Bc4Snorm,
    Bc5, // two channels, to red and green
    Bc5Snorm,
    Bc7,
    Etc2Rgb,  // ETC1 is a subset
    Etc2Rgba, // with EAC alpha
};

std::size_t blockBytes(BlockFormat format);

// The format as a GPU samples it as is, for the ones Lunarstone keeps
// compressed (unsigned BCn); nullopt for the rest.
std::optional<BlockCompressed::Format> gpuFormat(BlockFormat format);

// Bytes of a width x height image: whole 4x4 blocks, partial ones at the edges.
std::size_t blockImageBytes(BlockFormat format, std::uint32_t width, std::uint32_t height);

// data must hold blockImageBytes(format, width, height).
Image decodeBlocks(BlockFormat format, std::span<const std::uint8_t> data, std::uint32_t width, std::uint32_t height);

} // namespace lunar::assets::detail
