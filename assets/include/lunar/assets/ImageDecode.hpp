#pragma once

#include <lunar/Image.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::assets {

enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg, Dds, Ktx2, Bmp, Tga };

std::string_view imageFormatName(ImageFormat format);

// By magic bytes. TGA has none, so it's only reported when the header is
// plausible; Roblox's .tex sky files are DDS and come back as such.
ImageFormat detectImageFormat(std::string_view bytes);

// Everything in a texture file: one image for PNG and friends, every face,
// array slice and mip level for DDS and KTX2.
struct TextureFile {
    ImageFormat format = ImageFormat::Unknown;
    std::string pixelFormat;  // as stored: "BC1", "RGBA 8-bit", "baseline YCbCr 4:2:0", ...
    std::uint32_t layers = 1; // cubemap faces (+X -X +Y -Y +Z -Z) times array slices, or volume depth slices
    std::uint32_t mips = 1;   // levels kept in images, may be fewer than the file has
    std::uint32_t fileMips = 1;
    bool cubemap = false;
    bool volume = false;
    std::vector<Image> images; // images[layer * mips + mip], all mips of a layer together

    const Image& image(std::uint32_t layer = 0, std::uint32_t mip = 0) const { return images[layer * mips + mip]; }
};

struct DecodeOptions {
    bool allMips = false;  // else only level 0 of each layer
    bool allLayers = true; // else only the first face/slice
};

// Every decoder checks every size and offset against the data: garbage gives
// an error, never a crash or an absurd allocation.
std::expected<TextureFile, std::string> decodeTexture(std::string_view bytes, DecodeOptions options = {});

// The first image of any supported file, the one to draw.
std::expected<Image, std::string> decodeImage(std::string_view bytes);

// The image halved (each output pixel the mean of a 2x2 block, in the
// stored colour space) until neither side is over maxDimension; images that
// fit come back as they are.
Image limitImageSize(Image image, std::uint32_t maxDimension);

// PNG: every colour type and bit depth, palettes with tRNS, Adam7. 16-bit
// samples are cut to their high byte; gamma and colour profiles are ignored.
std::expected<Image, std::string> decodePng(std::string_view bytes);
// JPEG: baseline and progressive Huffman, grayscale, YCbCr, Adobe RGB/CMYK/YCCK,
// any sampling factors, restart markers. Matches libjpeg's default decode
// (islow IDCT, fancy upsampling) closely. Arithmetic coding and lossless
// JPEG are refused.
std::expected<Image, std::string> decodeJpeg(std::string_view bytes);
// BMP: 1/4/8-bit palettes, RLE4/RLE8, 16/24/32-bit with or without bit fields.
std::expected<Image, std::string> decodeBmp(std::string_view bytes);
// TGA: colour-mapped, true-colour and gray, raw or RLE, either origin.
std::expected<Image, std::string> decodeTga(std::string_view bytes);
// DDS: BC1-BC5, BC7, and uncompressed formats; cubemaps, arrays, volumes.
std::expected<TextureFile, std::string> decodeDds(std::string_view bytes, DecodeOptions options = {});

} // namespace lunar::assets
