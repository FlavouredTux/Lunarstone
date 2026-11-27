#include "Shaders.hpp"

#include <array>
#include <cstddef>

namespace lunar::render::shaders {

namespace {

// glslc writes SPIR-V as little-endian 32-bit words; #embed pulls in the
// bytes and they're reassembled into words at compile time.
template <std::size_t N>
constexpr auto toWords(const unsigned char (&bytes)[N])
{
    static_assert(N % 4 == 0, "SPIR-V is a whole number of words");
    std::array<std::uint32_t, N / 4> words{};
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = std::uint32_t{bytes[4 * i]} | std::uint32_t{bytes[4 * i + 1]} << 8
                   | std::uint32_t{bytes[4 * i + 2]} << 16 | std::uint32_t{bytes[4 * i + 3]} << 24;
    return words;
}

constexpr unsigned char meshVertBytes[] = {
#embed "mesh.vert.spv"
};
constexpr unsigned char meshFragBytes[] = {
#embed "mesh.frag.spv"
};
constexpr unsigned char meshRayFragBytes[] = {
#embed "mesh.rt.frag.spv"
};
constexpr unsigned char meshRayDenoiseFragBytes[] = {
#embed "mesh.rt-denoise.frag.spv"
};
constexpr unsigned char denoiseCompBytes[] = {
#embed "denoise.comp.spv"
};
constexpr unsigned char denoiseCompositeFragBytes[] = {
#embed "denoise_composite.frag.spv"
};
constexpr unsigned char fullscreenVertBytes[] = {
#embed "post.vert.spv"
};

constexpr unsigned char shadowVertBytes[] = {
#embed "shadow.vert.spv"
};
constexpr unsigned char shadowFragBytes[] = {
#embed "shadow.frag.spv"
};

constexpr unsigned char skyVertBytes[] = {
#embed "sky.vert.spv"
};
constexpr unsigned char skyFragBytes[] = {
#embed "sky.frag.spv"
};
constexpr unsigned char mapVertBytes[] = {
#embed "map.vert.spv"
};
constexpr unsigned char mapFragBytes[] = {
#embed "map.frag.spv"
};
constexpr unsigned char highlightMaskFragBytes[] = {
#embed "highlight_mask.frag.spv"
};
constexpr unsigned char decalLayerFragBytes[] = {
#embed "decal_layer.frag.spv"
};

constexpr auto shadowVertWords = toWords(shadowVertBytes);
constexpr auto skyVertWords = toWords(skyVertBytes);
constexpr auto skyFragWords = toWords(skyFragBytes);
constexpr auto mapVertWords = toWords(mapVertBytes);
constexpr auto mapFragWords = toWords(mapFragBytes);
constexpr auto meshVertWords = toWords(meshVertBytes);
constexpr auto meshFragWords = toWords(meshFragBytes);
constexpr auto meshRayFragWords = toWords(meshRayFragBytes);
static_assert(meshRayFragWords[0] == 0x07230203, "not SPIR-V");
constexpr auto meshRayDenoiseFragWords = toWords(meshRayDenoiseFragBytes);
constexpr auto denoiseCompWords = toWords(denoiseCompBytes);
constexpr auto denoiseCompositeFragWords = toWords(denoiseCompositeFragBytes);
constexpr auto fullscreenVertWords = toWords(fullscreenVertBytes);
static_assert(meshRayDenoiseFragWords[0] == 0x07230203 && denoiseCompWords[0] == 0x07230203
                  && denoiseCompositeFragWords[0] == 0x07230203 && fullscreenVertWords[0] == 0x07230203,
              "not SPIR-V");
constexpr auto shadowFragWords = toWords(shadowFragBytes);
static_assert(meshVertWords[0] == 0x07230203 && meshFragWords[0] == 0x07230203 && shadowVertWords[0] == 0x07230203
                  && shadowFragWords[0] == 0x07230203,
              "not SPIR-V");
static_assert(skyVertWords[0] == 0x07230203 && skyFragWords[0] == 0x07230203, "not SPIR-V");
static_assert(mapVertWords[0] == 0x07230203 && mapFragWords[0] == 0x07230203, "not SPIR-V");
constexpr auto highlightMaskFragWords = toWords(highlightMaskFragBytes);
static_assert(highlightMaskFragWords[0] == 0x07230203, "not SPIR-V");
constexpr auto decalLayerFragWords = toWords(decalLayerFragBytes);
static_assert(decalLayerFragWords[0] == 0x07230203, "not SPIR-V");

} // namespace

std::span<const std::uint32_t> meshVert()
{
    return meshVertWords;
}

std::span<const std::uint32_t> meshFrag()
{
    return meshFragWords;
}

std::span<const std::uint32_t> meshRayFrag()
{
    return meshRayFragWords;
}

std::span<const std::uint32_t> meshRayDenoiseFrag()
{
    return meshRayDenoiseFragWords;
}

std::span<const std::uint32_t> denoiseComp()
{
    return denoiseCompWords;
}

std::span<const std::uint32_t> denoiseCompositeFrag()
{
    return denoiseCompositeFragWords;
}

std::span<const std::uint32_t> fullscreenVert()
{
    return fullscreenVertWords;
}

std::span<const std::uint32_t> shadowVert()
{
    return shadowVertWords;
}

std::span<const std::uint32_t> shadowFrag()
{
    return shadowFragWords;
}

std::span<const std::uint32_t> skyVert()
{
    return skyVertWords;
}

std::span<const std::uint32_t> skyFrag()
{
    return skyFragWords;
}

std::span<const std::uint32_t> mapVert()
{
    return mapVertWords;
}

std::span<const std::uint32_t> mapFrag()
{
    return mapFragWords;
}

std::span<const std::uint32_t> highlightMaskFrag()
{
    return highlightMaskFragWords;
}

std::span<const std::uint32_t> decalLayerFrag()
{
    return decalLayerFragWords;
}

} // namespace lunar::render::shaders
