#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lunar::render {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;

    friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
};

// 8-bit RGBA pixels, rows top to bottom.
struct PixelImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba> pixels;

    Rgba at(std::uint32_t x, std::uint32_t y) const { return pixels[std::size_t{y} * width + x]; }
};

// Binary PPM (P6), alpha dropped.
std::string encodePpm(const PixelImage& image);
// PNG, RGBA, with uncompressed deflate blocks: big but dependency free.
std::string encodePng(const PixelImage& image);

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);
std::uint32_t adler32(std::span<const std::uint8_t> bytes, std::uint32_t adler = 1);

} // namespace lunar::render
