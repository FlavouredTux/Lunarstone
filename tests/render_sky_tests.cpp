#include "render_gpu.hpp"

// The sky pass: Sky faces where the client puts them, the right way round.

#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>

using lunar::Vector3;
using namespace lunar::render;

namespace {

constexpr std::uint32_t width = 64;
constexpr std::uint32_t height = 48;

struct Colour {
    std::uint8_t r, g, b;
};

// A 16x16 face, left half one colour and right half another.
ImagePtr face(Colour left, Colour right)
{
    auto image = std::make_shared<lunar::Image>();
    image->width = 16;
    image->height = 16;
    image->rgba.resize(image->byteSize());
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const Colour c = x < 8 ? left : right;
            const std::size_t at = (std::size_t{y} * 16 + x) * 4;
            image->rgba[at] = c.r;
            image->rgba[at + 1] = c.g;
            image->rgba[at + 2] = c.b;
            image->rgba[at + 3] = 255;
        }
    return image;
}

bool near(Rgba p, Colour c)
{
    return std::abs(p.r - c.r) <= 3 && std::abs(p.g - c.g) <= 3 && std::abs(p.b - c.b) <= 3;
}

constexpr Colour red{220, 30, 30}, green{30, 200, 40}, blue{30, 40, 210}, yellow{230, 220, 30}, cyan{30, 210, 220},
    magenta{210, 30, 200}, white{240, 240, 240};

// Rt, Lf, Up, Dn, Bk, Ft.
Lighting skyLighting()
{
    Lighting lighting;
    lighting.skybox.faces = {face(red, red),       face(green, green), face(blue, blue),
                             face(yellow, yellow), face(cyan, cyan),   face(magenta, white)};
    lighting.skybox.celestialBodiesShown = false;
    return lighting;
}

PixelImage look(Context& context, Vector3 direction)
{
    auto offscreen = OffscreenRenderer::create(context, width, height);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    Camera camera;
    camera.cframe = Camera::lookAt({}, direction);
    return (*offscreen)->render(camera, {}, skyLighting());
}

} // namespace

GPU_TEST(sky_faces_sit_where_the_client_puts_them)
{
    // Roblox's -Z is the default camera's forward: SkyboxFt. +X is Rt, +Y Up.
    CHECK(near(look(context, {1.0f, 0.0f, 0.0f}).at(width / 2, height / 2), red));
    CHECK(near(look(context, {-1.0f, 0.0f, 0.0f}).at(width / 2, height / 2), green));
    CHECK(near(look(context, {0.0f, 1.0f, 0.01f}).at(width / 2, height / 2), blue));
    CHECK(near(look(context, {0.0f, -1.0f, 0.01f}).at(width / 2, height / 2), yellow));
    CHECK(near(look(context, {0.0f, 0.0f, 1.0f}).at(width / 2, height / 2), cyan));

    // Faces are laid out as an OpenGL cube map is, so looking at Ft the
    // image's left half is on the right of the screen.
    const PixelImage front = look(context, {0.0f, 0.0f, -1.0f});
    CHECK(near(front.at(width - 12, height / 2), magenta));
    CHECK(near(front.at(12, height / 2), white));
}

GPU_TEST(sky_without_faces_is_the_flat_colour)
{
    auto offscreen = OffscreenRenderer::create(context, width, height);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    Lighting lighting;
    lighting.sky = lunar::Color3::fromRGB(10, 128, 250);
    lighting.skyTint = {0.5f, 0.5f, 0.5f};
    Camera camera;
    const PixelImage image = (*offscreen)->render(camera, {}, lighting);
    // The tint is linear: half of 128's linear value is about 93 in sRGB.
    const Rgba p = image.at(width / 2, height / 2);
    CHECK(std::abs(p.g - 93) <= 3);
}
