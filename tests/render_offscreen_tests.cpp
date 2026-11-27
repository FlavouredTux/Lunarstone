#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Primitives.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

// Not all of lunar: the engine's Roblox Camera and Lighting classes would
// clash with the renderer's.
using lunar::CFrame;
using lunar::Color3;
using lunar::DataModel;
using lunar::Part;
using lunar::Vector3;
using namespace lunar::render;

template <>
struct std::formatter<Rgba> : std::formatter<std::string_view> {
    auto format(const Rgba& p, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(std::format("rgba({}, {}, {}, {})", p.r, p.g, p.b, p.a), ctx);
    }
};

namespace {

constexpr std::uint32_t width = 64;
constexpr std::uint32_t height = 48;

std::unique_ptr<OffscreenRenderer> makeOffscreen(Context& context)
{
    auto offscreen = OffscreenRenderer::create(context, width, height);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    return std::move(*offscreen);
}

// A camera 10 studs back from the origin looking down -Z.
Camera frontCamera()
{
    Camera camera;
    camera.cframe = CFrame(0.0f, 0.0f, 10.0f);
    return camera;
}

DrawItem box(Vector3 position, Vector3 size, Color3 color, float transparency = 0.0f)
{
    return {partMatrix(CFrame(position), size), color, transparency};
}

bool near(Rgba a, Rgba b, int tolerance = 1)
{
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance
           && std::abs(a.a - b.a) <= tolerance;
}

Rgba skyPixel()
{
    const Color3 sky = Lighting{}.sky;
    auto byte = [](float c) { return static_cast<std::uint8_t>(std::lround(c * 255.0f)); };
    return {byte(sky.r), byte(sky.g), byte(sky.b), 255};
}

// Lit pure red, green or blue: that channel bright, the others dark (but
// for the sun's highlight, which whitens even matte plastic a little).
enum class Channel : std::uint8_t { Red, Green, Blue };
bool pure(Rgba p, Channel channel)
{
    const int values[3] = {p.r, p.g, p.b};
    const auto index = static_cast<std::size_t>(channel);
    for (std::size_t i = 0; i < 3; ++i)
        if (i == index ? values[i] <= 100 : values[i] >= 60)
            return false;
    return true;
}

Rgba centre(const PixelImage& image)
{
    return image.at(image.width / 2, image.height / 2);
}

} // namespace

GPU_TEST(offscreen_empty_scene_is_sky)
{
    auto offscreen = makeOffscreen(context);
    PixelImage image = offscreen->render(frontCamera(), {});
    CHECK_EQ(image.width, width);
    CHECK_EQ(image.height, height);
    CHECK_EQ(image.pixels.size(), std::size_t{width} * height);
    for (const Rgba& pixel : image.pixels)
        if (!near(pixel, skyPixel()))
            lt::fail(std::format("expected sky {}, got {}", skyPixel(), pixel), std::source_location::current());
}

GPU_TEST(offscreen_clear_colour_follows_lighting)
{
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.sky = Color3::fromRGB(10, 128, 250);
    PixelImage image = offscreen->render(frontCamera(), {}, lighting);
    CHECK(near(image.at(0, 0), Rgba{10, 128, 250, 255}));
    CHECK(near(image.at(width - 1, height - 1), Rgba{10, 128, 250, 255}));
}

GPU_TEST(offscreen_red_cube_in_front)
{
    auto offscreen = makeOffscreen(context);
    std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    PixelImage image = offscreen->render(frontCamera(), items);

    const Rgba c = centre(image);
    CHECK(pure(c, Channel::Red));
    CHECK_EQ(c.a, 255);
    // A 4 stud cube 8 studs away covers the middle, not the corners.
    for (auto [x, y] : {std::pair{0u, 0u}, {width - 1, 0u}, {0u, height - 1}, {width - 1, height - 1}})
        CHECK(near(image.at(x, y), skyPixel()));
}

GPU_TEST(offscreen_render_scale_draws_smaller_and_fills_the_target)
{
    // Half the size, scaled up onto the whole image: the cube's still in
    // the middle and the sky in the corners.
    auto offscreen = makeOffscreen(context);
    GraphicsSettings graphics;
    graphics.renderScale = 0.5f;
    offscreen->renderer().setGraphics(graphics);
    std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    PixelImage image = offscreen->render(frontCamera(), items);
    CHECK_EQ(image.width, width);
    CHECK_EQ(image.height, height);
    CHECK(pure(centre(image), Channel::Red));
    for (auto [x, y] : {std::pair{0u, 0u}, {width - 1, 0u}, {0u, height - 1}, {width - 1, height - 1}})
        CHECK(near(image.at(x, y), skyPixel()));
}

GPU_TEST(offscreen_minimal_graphics_casts_no_shadows)
{
    auto offscreen = makeOffscreen(context);
    std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f}),
                                box({0.0f, -3.0f, 0.0f}, {20.0f, 1.0f, 20.0f}, {0.5f, 0.5f, 0.5f})};
    offscreen->render(frontCamera(), items);
    CHECK(offscreen->renderer().stats().shadowCasters > 0u);
    offscreen->renderer().setGraphics(graphicsSettings(GraphicsPreset::Minimal));
    PixelImage image = offscreen->render(frontCamera(), items);
    CHECK_EQ(offscreen->renderer().stats().shadowCasters, 0u);
    CHECK(pure(centre(image), Channel::Red));
}

GPU_TEST(offscreen_image_is_upright)
{
    auto offscreen = makeOffscreen(context);
    // Green above the centre, blue to the right.
    std::vector<DrawItem> items{
        box({0.0f, 3.0f, 0.0f}, {1.5f, 1.5f, 1.5f}, {0.0f, 1.0f, 0.0f}),
        box({4.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}, {0.0f, 0.0f, 1.0f}),
    };
    PixelImage image = offscreen->render(frontCamera(), items);

    // Projected: 3 studs up at distance 10 with a 70 degree FOV is about 30% of the way to the top.
    const Rgba up = image.at(width / 2, height / 2 - 11);
    CHECK(pure(up, Channel::Green));
    const Rgba down = image.at(width / 2, height / 2 + 7);
    CHECK(near(down, skyPixel()));

    const Rgba right = image.at(width / 2 + 15, height / 2);
    CHECK(pure(right, Channel::Blue));
    CHECK(near(image.at(width / 2 - 15, height / 2), skyPixel()));
}

GPU_TEST(offscreen_depth_test_keeps_the_nearest)
{
    auto offscreen = makeOffscreen(context);
    const DrawItem nearGreen = box({0.0f, 0.0f, 2.0f}, {2.0f, 2.0f, 2.0f}, {0.0f, 1.0f, 0.0f});
    const DrawItem farRed = box({0.0f, 0.0f, -3.0f}, {6.0f, 6.0f, 2.0f}, {1.0f, 0.0f, 0.0f});

    for (const auto& items : {std::vector{nearGreen, farRed}, std::vector{farRed, nearGreen}}) {
        PixelImage image = offscreen->render(frontCamera(), items);
        const Rgba c = centre(image);
        CHECK(pure(c, Channel::Green));
        // Beside the small cube the big red one shows.
        const Rgba side = image.at(width / 2 + 7, height / 2);
        CHECK(pure(side, Channel::Red));
    }
}

GPU_TEST(offscreen_lights_faces_differently)
{
    auto offscreen = makeOffscreen(context);
    Camera camera;
    camera.cframe = Camera::lookAt({6.0f, 6.0f, 6.0f}, {});
    std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 1.0f, 1.0f})};
    PixelImage image = offscreen->render(camera, items);

    // Seen from a corner: top face above the centre, the +X and +Z faces below to the right and left.
    const Rgba top = image.at(width / 2, height / 2 - 6);
    const Rgba left = image.at(width / 2 - 5, height / 2 + 3);
    const Rgba right = image.at(width / 2 + 5, height / 2 + 3);
    for (Rgba face : {top, left, right})
        CHECK(!near(face, skyPixel(), 4));
    CHECK(top.r != left.r);
    CHECK(left.r != right.r);
    CHECK(top.r > 150); // the sun is high
}

GPU_TEST(offscreen_light_goes_through_the_client_tone_curve)
{
    // A white part lit by Ambient alone shows the client's curve of the
    // light: Default, x (0.3 x + 0.4) / (x (0.3 x + 0.38) + 0.12), about
    // the forward shaders' sqrt in the middle with a soft shoulder above 1;
    // Retro (Technology Compatibility's), about sqrt throughout. Past the
    // scene's headroom of 4 is all the same.
    auto offscreen = makeOffscreen(context);
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 1.0f, 1.0f})};
    auto shown = [&](float light, bool retro) {
        Lighting lighting;
        lighting.sunColor = {};
        lighting.backColor = {};
        lighting.ambient = {light, light, light};
        lighting.environmentDiffuse = 0.0f;
        lighting.environmentSpecular = 0.0f;
        lighting.shadows = false;
        lighting.retroTone = retro;
        return centre(offscreen->render(frontCamera(), items, lighting));
    };
    auto expected = [](float x, bool retro) {
        const float y = retro ? std::min(x * (0.88f * x + 0.16f) / (x * (0.88f * x + 1.34f) + 0.028f) * 2.18f, 1.0f)
                              : x * (0.3f * x + 0.4f) / (x * (0.3f * x + 0.38f) + 0.12f);
        const auto byte = static_cast<std::uint8_t>(std::lround(y * 255.0f));
        return Rgba{byte, byte, byte, 255};
    };
    for (bool retro : {false, true}) {
        for (float light : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f}) {
            const Rgba pixel = shown(light, retro);
            if (!near(pixel, expected(light, retro), 2))
                lt::fail(std::format("light {} (retro {}): expected {}, got {}", light, retro, expected(light, retro),
                                     pixel),
                         std::source_location::current());
        }
        CHECK(near(shown(8.0f, retro), expected(4.0f, retro), 1));
    }
    // Retro is the brighter in the dark.
    CHECK(shown(0.05f, true).r > shown(0.05f, false).r + 10);
}

GPU_TEST(offscreen_blends_transparent_parts)
{
    auto offscreen = makeOffscreen(context);
    std::vector<DrawItem> items{
        box({0.0f, 0.0f, 3.0f}, {2.0f, 2.0f, 0.5f}, {0.0f, 0.0f, 1.0f}, 0.5f),
        box({0.0f, 0.0f, -2.0f}, {6.0f, 6.0f, 1.0f}, {1.0f, 0.0f, 0.0f}),
    };
    PixelImage image = offscreen->render(frontCamera(), items);
    const Rgba c = centre(image);
    CHECK(c.r > 50);
    CHECK(c.b > 50);
    CHECK(c.g < 60);

    // Fully opaque again: only blue.
    items[0].transparency = 0.0f;
    const Rgba solid = centre(offscreen->render(frontCamera(), items));
    CHECK(pure(solid, Channel::Blue));
}

GPU_TEST(offscreen_renders_many_parts)
{
    auto offscreen = makeOffscreen(context);
    auto game = DataModel::create();
    for (int x = -20; x < 20; ++x) {
        for (int z = -20; z < 20; ++z) {
            auto part = std::make_shared<Part>();
            part->setSize({0.8f, 0.8f, 0.8f});
            part->setPosition({static_cast<float>(x), 0.0f, static_cast<float>(z)});
            part->setColor({0.2f, 0.8f, 0.3f});
            part->setTransparency((x + z) % 7 == 0 ? 0.4f : 0.0f);
            part->setParent(game->workspace());
        }
    }
    auto items = extractScene(*game->workspace());
    CHECK_EQ(items.size(), 1600u);

    Camera camera;
    camera.cframe = Camera::lookAt({0.0f, 25.0f, 25.0f}, {});
    PixelImage image = offscreen->render(camera, items);
    CHECK(centre(image).g > 80);
    // Smaller and larger frames reuse and grow the instance buffer.
    offscreen->render(camera, std::span(items).first(10));
    offscreen->render(camera, items);
}

TEST(png_checksums)
{
    const std::string_view digits = "123456789";
    const std::span bytes{reinterpret_cast<const std::uint8_t*>(digits.data()), digits.size()};
    CHECK_EQ(crc32(bytes), 0xCBF43926u);
    const std::string_view wiki = "Wikipedia";
    CHECK_EQ(adler32({reinterpret_cast<const std::uint8_t*>(wiki.data()), wiki.size()}), 0x11E60398u);
}

TEST(png_encodes_stored_blocks)
{
    PixelImage image{2, 1, {{255, 0, 0, 255}, {0, 0, 255, 128}}};
    const std::string png = encodePng(image);
    CHECK(png.starts_with("\x89PNG\r\n\x1a\n"));
    CHECK(png.substr(12, 4) == "IHDR");
    // The empty IEND chunk always ends with the same CRC.
    CHECK(png.ends_with(std::string_view("IEND\xae\x42\x60\x82", 8)));
    // signature + IHDR + IDAT(2 zlib + 5 block header + 9 raw + 4 adler) + IEND
    CHECK_EQ(png.size(), 8u + 25u + (12u + 20u) + 12u);

    // Big enough to need several stored blocks.
    PixelImage large{200, 200, std::vector<Rgba>(40000, Rgba{1, 2, 3, 4})};
    const std::string bigPng = encodePng(large);
    const std::size_t raw = std::size_t{200} * (1 + 200 * 4);
    CHECK_EQ(bigPng.size(), 8u + 25u + 12u + 2u + 5u * ((raw + 65534) / 65535) + raw + 4u + 12u);
}

TEST(ppm_has_header_and_rgb)
{
    PixelImage image{2, 1, {{255, 0, 0, 255}, {0, 0, 255, 128}}};
    CHECK_EQ(encodePpm(image), std::string("P6\n2 1\n255\n\xff\x00\x00\x00\x00\xff", 17));
}

namespace {

DrawItem shape(Primitive primitive, Vector3 position, Vector3 size, Color3 color)
{
    DrawItem item = box(position, size, color);
    item.mesh = primitiveMesh(primitive);
    return item;
}

Camera cameraAt(Vector3 eye, Vector3 target = {})
{
    Camera camera;
    camera.cframe = Camera::lookAt(eye, target);
    return camera;
}

bool isSky(Rgba p)
{
    return near(p, skyPixel(), 2);
}

// Pixel offset from the centre of a point `studs` off the view axis at
// `distance` (70 degree vertical FOV, square pixels).
int pixels(float studs, float distance)
{
    const float halfHeight = distance * std::tan(35.0f * std::numbers::pi_v<float> / 180.0f);
    return static_cast<int>(std::lround(studs / halfHeight * (height / 2.0f)));
}

Rgba at(const PixelImage& image, int dx, int dy)
{
    return image.at(static_cast<std::uint32_t>(static_cast<int>(width / 2) + dx),
                    static_cast<std::uint32_t>(static_cast<int>(height / 2) + dy));
}

} // namespace

GPU_TEST(offscreen_cylinder_is_round_end_on_and_square_from_the_side)
{
    auto offscreen = makeOffscreen(context);
    const std::vector items{shape(Primitive::Cylinder, {}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};

    // From the side (+Z) the cylinder along X fills its square, corners too.
    // Its top and bottom edges are 10 studs away, not 8.
    PixelImage side = offscreen->render(cameraAt({0.0f, 0.0f, 10.0f}), items);
    const int inside = pixels(1.7f, 10.0f);
    for (auto [x, y] : {std::pair{inside, inside}, {-inside, inside}, {inside, -inside}, {-inside, -inside}})
        CHECK(!isSky(at(side, x, y)));
    CHECK(pure(at(side, 0, 0), Channel::Red));
    CHECK(isSky(at(side, 0, pixels(2.2f, 10.0f))));

    const int corner = pixels(1.7f, 8.0f);
    const int edge = pixels(1.6f, 8.0f);

    // End on (+X) it is a disc: the square's corners show sky, its sides don't.
    PixelImage end = offscreen->render(cameraAt({10.0f, 0.0f, 0.0f}), items);
    CHECK(isSky(at(end, corner, corner)));
    CHECK(isSky(at(end, -corner, -corner)));
    CHECK(!isSky(at(end, edge, 0)));
    CHECK(!isSky(at(end, 0, -edge)));
}

GPU_TEST(offscreen_wedge_slopes_down_to_the_front)
{
    auto offscreen = makeOffscreen(context);
    const std::vector items{shape(Primitive::Wedge, {}, {4.0f, 4.0f, 4.0f}, {0.0f, 1.0f, 0.0f})};
    // Seen from +X, screen right is -Z (the front), where the wedge is low.
    PixelImage image = offscreen->render(cameraAt({10.0f, 0.0f, 0.0f}), items);
    // The slope runs corner to corner, from top left to bottom right.
    const int d = pixels(1.5f, 8.0f);
    CHECK(pure(at(image, -d, -d / 2), Channel::Green)); // high at the back
    CHECK(isSky(at(image, d, -d)));                     // nothing above the slope's foot
    CHECK(isSky(at(image, d / 2, -d / 2 - 3)));
    CHECK(pure(at(image, d / 2, d), Channel::Green)); // bottom front
    CHECK(pure(at(image, -d / 2, d / 2), Channel::Green));
}

GPU_TEST(offscreen_ball_is_round)
{
    auto offscreen = makeOffscreen(context);
    const std::vector items{shape(Primitive::Ball, {}, {4.0f, 4.0f, 4.0f}, {0.0f, 0.0f, 1.0f})};
    PixelImage image = offscreen->render(frontCamera(), items);
    const int corner = pixels(1.7f, 8.0f);
    const int edge = pixels(1.5f, 8.0f);
    CHECK(pure(at(image, 0, 0), Channel::Blue));
    CHECK(!isSky(at(image, edge, 0)));
    CHECK(!isSky(at(image, 0, edge)));
    for (auto [x, y] : {std::pair{corner, corner}, {-corner, corner}, {corner, -corner}, {-corner, -corner}})
        CHECK(isSky(at(image, x, y)));
}

GPU_TEST(offscreen_uploads_a_shared_mesh_once)
{
    auto offscreen = makeOffscreen(context);
    auto mesh = std::make_shared<const lunar::MeshData>(makePrimitive(Primitive::Ball, 12));
    std::vector<DrawItem> items;
    for (int i = 0; i < 100; ++i) {
        const int row = i / 10;
        DrawItem item = box({static_cast<float>(i % 10) - 5.0f, static_cast<float>(row) - 5.0f, 0.0f},
                            {0.8f, 0.8f, 0.8f}, {0.5f, 0.5f, 0.5f});
        if (i % 2 == 0)
            item.mesh = mesh;
        items.push_back(item);
    }
    Lighting unshadowed;
    unshadowed.shadows = false;
    offscreen->render(cameraAt({0.0f, 0.0f, 20.0f}), items, unshadowed);
    const FrameStats& stats = offscreen->renderer().stats();
    CHECK_EQ(stats.meshesUploaded, 2u);
    CHECK_EQ(offscreen->renderer().meshCount(), 2u);
    CHECK_EQ(stats.drawCalls, 2u);
    CHECK_EQ(stats.visible, 100u);
    CHECK_EQ(stats.shadowCasters, 0u);

    // Next frame: nothing new to upload. Shadows draw each mesh once more.
    offscreen->render(cameraAt({0.0f, 0.0f, 20.0f}), items);
    CHECK_EQ(offscreen->renderer().stats().meshesUploaded, 0u);
    CHECK_EQ(offscreen->renderer().meshCount(), 2u);
    CHECK_EQ(offscreen->renderer().stats().drawCalls, 4u);
    CHECK_EQ(offscreen->renderer().stats().shadowCasters, 100u);
}

GPU_TEST(offscreen_culls_what_the_camera_cannot_see)
{
    auto offscreen = makeOffscreen(context);
    const std::vector items{
        box({}, {2.0f, 2.0f, 2.0f}, {1.0f, 0.0f, 0.0f}),
        box({0.0f, 0.0f, 30.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 0.0f, 0.0f}),  // behind the camera
        box({500.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, {1.0f, 0.0f, 0.0f}), // far off to the side
    };
    offscreen->render(frontCamera(), items);
    CHECK_EQ(offscreen->renderer().stats().items, 3u);
    CHECK_EQ(offscreen->renderer().stats().visible, 1u);
}

GPU_TEST(offscreen_timestamps_measure_the_gpu_when_asked)
{
    auto offscreen = makeOffscreen(context);
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    // Nothing is measured until timestamps are asked for.
    CHECK_EQ(offscreen->gpuMilliseconds(), 0.0);
    offscreen->render(frontCamera(), items);
    CHECK_EQ(offscreen->gpuMilliseconds(), 0.0);

    if (!offscreen->setTimestamps(true))
        SKIP("the device cannot write timestamps");
    offscreen->render(frontCamera(), items);
    CHECK(offscreen->gpuMilliseconds() > 0.0);
    CHECK(offscreen->readbackMilliseconds() >= 0.0);

    // And off again: the frame still draws, but says nothing about the GPU.
    CHECK(!offscreen->setTimestamps(false));
    offscreen->render(frontCamera(), items);
    CHECK_EQ(offscreen->gpuMilliseconds(), 0.0);
    CHECK(pure(centre(offscreen->render(frontCamera(), items)), Channel::Red));
}

GPU_TEST(offscreen_pass_timestamps_measure_each_pass)
{
    // Eight times the usual target: a device's timestamps tick tens of
    // nanoseconds at a time, and at 64x48 even the sky's fullscreen draw
    // fits inside one tick, so the passes would read as nothing at all.
    auto made = OffscreenRenderer::create(context, width * 8, height * 8);
    if (!made)
        throw lt::Failure{made.error()};
    std::unique_ptr<OffscreenRenderer> offscreen = std::move(*made);
    const std::vector<DrawItem> items{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    // Nothing is measured until pass timestamps are asked for, and a device
    // that cannot write them has none at all.
    offscreen->render(frontCamera(), items);
    for (double ms : offscreen->renderer().passMilliseconds())
        CHECK_EQ(ms, 0.0);
    if (!offscreen->setPassTimestamps(true)) {
        CHECK(!context.timestampQueries());
        SKIP("the device cannot write timestamps");
    }
    CHECK(context.timestampQueries());

    offscreen->render(frontCamera(), items);
    const PassTimes& times = offscreen->renderer().passMilliseconds();
    double total = 0.0;
    for (std::size_t p = 0; p < gpuPassCount; ++p) {
        CHECK(times[p] >= 0.0);
        total += times[p];
    }
    CHECK(total > 0.0);
    CHECK(times[static_cast<std::size_t>(GpuPass::Sky)] > 0.0);       // a fullscreen draw every frame
    CHECK(times[static_cast<std::size_t>(GpuPass::SunShadow)] > 0.0); // the map is cleared every frame
    // What this scene does not draw costs next to nothing: it has no water
    // and no decals. Two stamps with nothing between them can still come a
    // tick or so apart (a microsecond on the 3070, when it's busy).
    CHECK(times[static_cast<std::size_t>(GpuPass::Water)] < 0.05);
    CHECK(times[static_cast<std::size_t>(GpuPass::Decals)] < 0.05);
    CHECK(times[static_cast<std::size_t>(GpuPass::DecalLayer)] < 0.05);
    // The passes between the frame's first and last stamps are the whole
    // frame: they cannot add up to more than the timestamps around them say.
    if (offscreen->setTimestamps(true)) {
        offscreen->render(frontCamera(), items);
        CHECK(offscreen->gpuMilliseconds() > 0.0);
        double wholeTotal = 0.0;
        for (double ms : offscreen->renderer().passMilliseconds())
            wholeTotal += ms;
        CHECK(wholeTotal > 0.0);
        CHECK(wholeTotal <= offscreen->gpuMilliseconds() + 0.5);
    }

    // Off again: the frame still draws, but says nothing about the GPU.
    CHECK(!offscreen->setPassTimestamps(false));
    offscreen->render(frontCamera(), items);
    for (double ms : offscreen->renderer().passMilliseconds())
        CHECK_EQ(ms, 0.0);
    CHECK(pure(centre(offscreen->render(frontCamera(), items)), Channel::Red));
}

GPU_TEST(offscreen_world_meshes_use_vertex_colours)
{
    auto offscreen = makeOffscreen(context);
    // A big two-coloured ground quad in world space: red on the left (-X), blue on the right.
    auto ground = std::make_shared<lunar::MeshData>();
    const Vector3 up{0.0f, 1.0f, 0.0f};
    for (float x : {-50.0f, 0.0f, 0.0f, 50.0f}) {
        const Color3 c = x < 0.0f || (x == 0.0f && ground->vertices.size() < 4) ? Color3{1.0f, 0.0f, 0.0f}
                                                                                : Color3{0.0f, 0.0f, 1.0f};
        ground->vertices.push_back({{x, 0.0f, -50.0f}, up, 0.0f, 0.0f, c});
        ground->vertices.push_back({{x, 0.0f, 50.0f}, up, 0.0f, 0.0f, c});
    }
    // Quads (0,1,3,2) and (4,5,7,6), counter-clockwise from above.
    ground->indices = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};
    const std::vector items{worldMesh(ground)};

    PixelImage image = offscreen->render(cameraAt({0.0f, 20.0f, 10.0f}, {0.0f, 0.0f, 0.0f}), items);
    CHECK(pure(at(image, -10, 0), Channel::Red));
    CHECK(pure(at(image, 10, 0), Channel::Blue));
}

GPU_TEST(offscreen_fog_hides_distant_parts)
{
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.fogColor = Color3::fromRGB(0, 255, 0);
    lighting.fogStart = 5.0f;
    lighting.fogEnd = 20.0f;
    const std::vector nearRed{box({}, {4.0f, 4.0f, 4.0f}, {1.0f, 0.0f, 0.0f})};
    const std::vector farRed{box({0.0f, 0.0f, -30.0f}, {20.0f, 20.0f, 1.0f}, {1.0f, 0.0f, 0.0f})};

    // 8 studs away: a little fog. Beyond FogEnd: nothing but fog.
    const Rgba close = centre(offscreen->render(frontCamera(), nearRed, lighting));
    CHECK(close.r > 150);
    CHECK(close.g > 20);
    const Rgba hidden = centre(offscreen->render(frontCamera(), farRed, lighting));
    CHECK(near(hidden, Rgba{0, 255, 0, 255}, 2));
}

GPU_TEST(offscreen_neon_ignores_the_light)
{
    auto offscreen = makeOffscreen(context);
    DrawItem neon = box({}, {4.0f, 4.0f, 4.0f}, {0.2f, 0.3f, 0.6f});
    neon.material = lunar::Enum::Material::Neon;
    DrawItem plastic = neon;
    plastic.material = lunar::Enum::Material::Plastic;

    // From a corner every Neon face is the same colour; Plastic faces differ.
    const Camera camera = cameraAt({6.0f, 6.0f, 6.0f});
    PixelImage glowing = offscreen->render(camera, std::vector{neon});
    PixelImage lit = offscreen->render(camera, std::vector{plastic});
    const std::pair<int, int> faces[3] = {{0, -6}, {-5, 3}, {5, 3}};
    for (auto [x, y] : faces)
        CHECK(near(at(glowing, x, y), at(glowing, faces[0].first, faces[0].second), 1));
    CHECK(!near(at(lit, -5, 3), at(lit, 5, 3), 2));
    CHECK(at(glowing, -5, 3).b > at(lit, -5, 3).b);
}

GPU_TEST(offscreen_force_field_is_see_through)
{
    auto offscreen = makeOffscreen(context);
    DrawItem shield = box({0.0f, 0.0f, 3.0f}, {2.0f, 2.0f, 0.5f}, {0.0f, 0.0f, 1.0f});
    shield.material = lunar::Enum::Material::ForceField;
    const std::vector items{shield, box({0.0f, 0.0f, -2.0f}, {6.0f, 6.0f, 1.0f}, {1.0f, 0.0f, 0.0f})};
    const Rgba c = centre(offscreen->render(frontCamera(), items));
    CHECK(c.r > 50); // the red wall shows through
    CHECK(c.b > 50);
}

GPU_TEST(offscreen_parts_cast_sun_shadows)
{
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.sunDirection = {0.0f, 1.0f, 0.0f}; // straight down
    DrawItem roof = box({0.0f, 4.0f, 0.0f}, {4.0f, 0.5f, 4.0f}, {1.0f, 1.0f, 1.0f});
    std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f}), roof};

    // Seen from a slant, the ground under the roof shows just below the
    // centre of the image, and open ground further down.
    const Camera camera = cameraAt({0.0f, 12.0f, 12.0f}, {0.0f, 0.0f, 0.0f});
    const PixelImage image = offscreen->render(camera, items, lighting);
    const Rgba shadowed = at(image, 0, 1);
    const Rgba lit = at(image, 0, 12);
    CHECK(shadowed.r + 25 < lit.r);

    // Without shadows, or with CastShadow off, the ground there is lit.
    lighting.shadows = false;
    CHECK(near(at(offscreen->render(camera, items, lighting), 0, 1), lit, 3));
    lighting.shadows = true;
    items[1].castShadow = false;
    CHECK(near(at(offscreen->render(camera, items, lighting), 0, 1), lit, 3));
}

// Ray queries take a context of their own; without them (or under
// AddressSanitizer, where the driver can't place buffer device addresses)
// there's nothing to test.

// The scene of offscreen_parts_cast_sun_shadows.
TEST(offscreen_ray_traced_shadows_follow_casters_and_their_alpha)
{
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.sunDirection = {0.0f, 1.0f, 0.0f}; // straight down
    auto texture = [](std::uint8_t alpha) {
        auto image = std::make_shared<lunar::Image>();
        image->width = image->height = 4;
        for (int i = 0; i < 16; ++i)
            image->rgba.insert(image->rgba.end(), {255, 255, 255, alpha});
        return ImagePtr(std::move(image));
    };
    // The roof is a cut-out: a solid texture, then a clear one.
    DrawItem roof = box({0.0f, 4.0f, 0.0f}, {4.0f, 0.5f, 4.0f}, {1.0f, 1.0f, 1.0f});
    roof.texture = texture(255);
    roof.alphaMode = TextureAlpha::Transparency;
    std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f}), roof};
    const Camera camera = cameraAt({0.0f, 12.0f, 12.0f}, {0.0f, 0.0f, 0.0f});

    const PixelImage image = offscreen->render(camera, items, lighting);
    const Rgba lit = at(image, 0, 12);
    CHECK(at(image, 0, 1).r + 25 < lit.r);
    items[1].texture = texture(0);
    CHECK(near(at(offscreen->render(camera, items, lighting), 0, 1), lit, 3));
    items[1].texture = texture(255);
    items[1].castShadow = false;
    CHECK(near(at(offscreen->render(camera, items, lighting), 0, 1), lit, 3));
    lt::checkNoValidationErrors(context);
}

// The top level is refitted, not built again, while things move a little
// (RayScene::buildTop): the shadows come out where a top level built
// afresh for the same frame puts them, to the bit.
TEST(offscreen_ray_traced_top_level_refits_to_where_things_moved)
{
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    Lighting lighting;
    lighting.sunDirection = {0.0f, 1.0f, 0.0f}; // straight down
    std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f}),
                      box({0.0f, 4.0f, 0.0f}, {4.0f, 0.5f, 4.0f}, {1.0f, 1.0f, 1.0f})};
    const Camera camera = cameraAt({0.0f, 12.0f, 12.0f}, {0.0f, 0.0f, 0.0f});

    // The roof's shadow slides across the floor half a stud a frame.
    auto moving = makeOffscreen(context);
    PixelImage first, last;
    std::uint32_t refits = 0;
    for (int frame = 0; frame <= 8; ++frame) {
        items[1].model = partMatrix(CFrame(Vector3{0.5f * static_cast<float>(frame), 4.0f, 0.0f}), {4.0f, 0.5f, 4.0f});
        last = moving->render(camera, items, lighting);
        if (frame == 0)
            first = last;
        refits += moving->renderer().stats().tlasRefits;
    }
    CHECK(refits > 0);
    auto fresh = makeOffscreen(context);
    const PixelImage built = fresh->render(camera, items, lighting);
    CHECK_EQ(fresh->renderer().stats().tlasRefits, 0u);
    bool same = last.width == built.width && last.height == built.height;
    for (std::size_t i = 0; same && i < last.pixels.size(); ++i)
        same = near(last.pixels[i], built.pixels[i], 0);
    CHECK(same);
    // And it did move: under the roof's first place the floor is lit now.
    CHECK(at(first, 0, 1).r + 25 < at(last, 0, 1).r);
    lt::checkNoValidationErrors(context);
}

// A mirror for a floor shows what's behind the camera, which only a ray
// can find; without rays it shows the sky there.
TEST(offscreen_ray_traced_reflections_show_the_scene)
{
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.sunDirection = Vector3{0.0f, 1.0f, 1.0f}.unit(); // onto the red block's face
    DrawItem floor = box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f});
    floor.reflectance = 1.0f;
    // Out of view, where the floor's centre reflects the camera's view.
    const DrawItem block = box({0.0f, 12.0f, -12.0f}, {8.0f, 8.0f, 8.0f}, {1.0f, 0.0f, 0.0f});
    const Camera camera = cameraAt({0.0f, 12.0f, 12.0f}, {0.0f, 0.0f, 0.0f});

    const std::vector both{floor, block};
    const Rgba red = at(offscreen->render(camera, both, lighting), 0, 0);
    const std::vector alone{floor};
    const Rgba sky = at(offscreen->render(camera, alone, lighting), 0, 0);
    CHECK(red.r > red.g + 30);
    CHECK(red.r > red.b + 30);
    CHECK(sky.b > sky.r);
    lt::checkNoValidationErrors(context);
}

// --reflect-everything: a plain plastic floor mirrors the block too.
TEST(offscreen_everything_reflects_when_asked)
{
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    auto offscreen = makeOffscreen(context);
    Lighting lighting;
    lighting.sunDirection = Vector3{0.0f, 1.0f, 1.0f}.unit();
    const std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {0.5f, 0.5f, 0.5f}),
                            box({0.0f, 12.0f, -12.0f}, {8.0f, 8.0f, 8.0f}, {1.0f, 0.0f, 0.0f})};
    const Camera camera = cameraAt({0.0f, 12.0f, 12.0f}, {0.0f, 0.0f, 0.0f});

    const Rgba plain = at(offscreen->render(camera, items, lighting), 0, 0);
    CHECK(near(plain, {plain.r, plain.r, plain.r, plain.a}, 3)); // grey, no red in it
    GraphicsSettings graphics;
    graphics.reflectEverything = true;
    offscreen->renderer().setGraphics(graphics);
    const Rgba polished = at(offscreen->render(camera, items, lighting), 0, 0);
    CHECK(polished.r > polished.g + 30);
    lt::checkNoValidationErrors(context);
}

// Past each pixel's three strongest lights the rest share one shadow ray,
// which says 0 or 1 for all of them: a floor lit by a ring of lights round a
// pillar would be speckled where some of them are behind it. The denoiser
// spreads what those rays found over the floor around each pixel, so it
// comes out smooth. (LUNAR_RT_SKIP=denoise shows the speckle: this fails.)
TEST(offscreen_ray_traced_lights_shared_ray_is_denoised)
{
    Context* rays = lt::rayTracingGpu();
    if (!rays)
        SKIP("no ray queries");
    Context& context = *rays;
    context.takeValidationErrors();
    constexpr std::uint32_t wide = 256, high = 192;
    auto made = OffscreenRenderer::create(context, wide, high);
    if (!made)
        throw lt::Failure{made.error()};
    auto& offscreen = *made;
    struct Ring : LightSource {
        std::vector<LocalLight> lights;
        void collect(std::vector<LocalLight>& out) override { out.insert(out.end(), lights.begin(), lights.end()); }
    } ring;
    for (int i = 0; i < 12; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / 12.0f;
        ring.lights.push_back({.position = {8.0f * std::cos(angle), 2.0f, 8.0f * std::sin(angle)},
                               .color = {1.0f, 1.0f, 1.0f},
                               .brightness = 0.6f,
                               .range = 30.0f,
                               .shadows = true});
    }
    offscreen->renderer().setLights(&ring);
    Lighting lighting;
    lighting.technology = lunar::Enum::Technology::Future; // per pixel
    lighting.sunColor = {};
    lighting.backColor = {};
    lighting.ambient = {0.02f, 0.02f, 0.02f};
    lighting.shadows = false;
    const std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f}),
                            box({0.0f, 3.0f, 0.0f}, {2.0f, 6.0f, 2.0f}, {1.0f, 1.0f, 1.0f})};
    const PixelImage image = offscreen->render(cameraAt({0.0f, 18.0f, 14.0f}), items, lighting);

    // How far each pixel is from the mean of its four neighbours, on average:
    // a speckle of 0s and 1s is several levels a pixel; smooth light is about 1.
    auto level = [&](std::uint32_t x, std::uint32_t y) {
        const Rgba c = image.at(x, y);
        return (static_cast<double>(c.r) + c.g + c.b) / 3.0;
    };
    double speckle = 0.0;
    double lit = 0.0;
    int pixels = 0;
    for (std::uint32_t y = 1; y + 1 < high; ++y) {
        for (std::uint32_t x = 1; x + 1 < wide; ++x) {
            const double around = (level(x - 1, y) + level(x + 1, y) + level(x, y - 1) + level(x, y + 1)) / 4.0;
            speckle += std::abs(level(x, y) - around);
            lit += level(x, y);
            ++pixels;
        }
    }
    CHECK(lit / pixels > 20.0); // the lights do light the floor
    CHECK(speckle / pixels < 1.5);
    lt::checkNoValidationErrors(context);
}

// A shadowed light's cube map is drawn again only when it or what's around
// it changed: the same frame twice draws nothing into the atlas, and a
// caster that moved does, its shadow moving with it.
GPU_TEST(offscreen_local_shadows_redraw_only_when_casters_move)
{
    auto offscreen = makeOffscreen(context);
    struct One : LightSource {
        std::vector<LocalLight> lights;
        void collect(std::vector<LocalLight>& out) override { out.insert(out.end(), lights.begin(), lights.end()); }
    } lamp;
    lamp.lights.push_back({.position = {0.0f, 6.0f, 0.0f},
                           .color = {1.0f, 1.0f, 1.0f},
                           .brightness = 2.0f,
                           .range = 30.0f,
                           .shadows = true});
    offscreen->renderer().setLights(&lamp);
    Lighting lighting;
    lighting.technology = lunar::Enum::Technology::Future;
    lighting.sunColor = {};
    lighting.backColor = {};
    lighting.ambient = {0.02f, 0.02f, 0.02f};
    lighting.shadows = false;
    const Camera camera = cameraAt({0.0f, 20.0f, 0.1f});
    std::vector items{box({0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {1.0f, 1.0f, 1.0f}),
                      box({3.0f, 2.0f, 0.0f}, {1.0f, 1.0f, 6.0f}, {1.0f, 1.0f, 1.0f})};
    const PixelImage first = offscreen->render(camera, items, lighting);
    CHECK_EQ(offscreen->renderer().stats().shadowedLights, 1u);
    CHECK(offscreen->renderer().stats().localShadowCasters > 0u);
    const PixelImage again = offscreen->render(camera, items, lighting);
    CHECK_EQ(offscreen->renderer().stats().localShadowCasters, 0u);
    CHECK(std::ranges::equal(again.pixels, first.pixels, [](Rgba a, Rgba b) { return near(a, b, 0); }));
    // The bar crosses to the other side of the light: its shadow follows.
    items[1] = box({-3.0f, 2.0f, 0.0f}, {1.0f, 1.0f, 6.0f}, {1.0f, 1.0f, 1.0f});
    const PixelImage moved = offscreen->render(camera, items, lighting);
    CHECK(offscreen->renderer().stats().localShadowCasters > 0u);
    // The shadow is the darkest floor across the middle row, on the bar's
    // side (the light falls off alike on both).
    auto darkest = [](const PixelImage& image, std::uint32_t from, std::uint32_t to) {
        int least = 1000;
        for (std::uint32_t x = from; x < to; ++x) {
            const Rgba c = image.at(x, height / 2);
            least = std::min(least, c.r + c.g + c.b);
        }
        return least;
    };
    CHECK(darkest(first, width / 2, width) + 30 < darkest(first, 0, width / 2));
    CHECK(darkest(moved, 0, width / 2) + 30 < darkest(moved, width / 2, width));
    lt::checkNoValidationErrors(context);
}

GPU_TEST(offscreen_dithered_terrain_fades_in_complementary_halves)
{
    auto offscreen = makeOffscreen(context);
    // A terrain wall filling the view, facing the camera.
    auto wall = std::make_shared<lunar::MeshData>();
    const Vector3 back{0.0f, 0.0f, 1.0f};
    lunar::MeshVertex v{{}, back, 0.0f, 0.0f, Color3{0.0f, 1.0f, 0.0f}};
    v.materialWeights[0] = 255;
    for (Vector3 p : {Vector3{-50.0f, -50.0f, 0.0f}, Vector3{50.0f, -50.0f, 0.0f}, Vector3{50.0f, 50.0f, 0.0f},
                      Vector3{-50.0f, 50.0f, 0.0f}}) {
        v.position = p;
        wall->vertices.push_back(v);
    }
    wall->indices = {0, 1, 2, 0, 2, 3};
    DrawItem item = worldMesh(wall);
    item.terrain = true;
    auto skyCount = [&](std::vector<DrawItem> items) {
        const PixelImage image = offscreen->render(frontCamera(), items);
        int sky = 0;
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x)
                sky += near(image.at(x, y), skyPixel(), 2) ? 1 : 0;
        return sky;
    };
    const int pixels = static_cast<int>(width * height);
    CHECK_EQ(skyCount({item}), 0);
    DrawItem half = item;
    half.dither[1] = 0.5f;
    CHECK_EQ(skyCount({half}), pixels / 2);
    DrawItem other = item;
    other.dither[0] = 0.5f;
    CHECK_EQ(skyCount({half, other}), 0);
}

GPU_TEST(offscreen_water_hides_what_is_deep_below_it)
{
    // A red floor under terrain water, seen from straight above: through a
    // stud of water it still shows, through fifty it's gone.
    auto offscreen = makeOffscreen(context);
    auto redUnder = [&](float depth) {
        DrawItem water = box({0.0f, -depth / 2.0f, 0.0f}, {20.0f, depth, 20.0f}, {0.04f, 0.27f, 0.29f}, 0.5f);
        water.terrainWater = true;
        water.castShadow = false;
        std::vector<DrawItem> items{box({0.0f, -depth - 0.5f, 0.0f}, {20.0f, 1.0f, 20.0f}, {1.0f, 0.0f, 0.0f}), water};
        Camera camera;
        camera.cframe = Camera::lookAt({0.0f, 10.0f, 0.01f}, {0.0f, 0.0f, 0.0f});
        return centre(offscreen->render(camera, items)).r;
    };
    const int shallow = redUnder(1.0f);
    const int deep = redUnder(50.0f);
    CHECK(shallow > 100);
    CHECK(deep < 60);
}

namespace {

std::shared_ptr<const HighlightStyle> solidHighlight(bool alwaysOnTop)
{
    return std::make_shared<const HighlightStyle>(HighlightStyle{
        .fillColor = {0.0f, 0.0f, 1.0f},
        .fillTransparency = 0.0f,
        .outlineColor = {1.0f, 0.0f, 0.0f},
        .outlineTransparency = 0.0f,
        .alwaysOnTop = alwaysOnTop,
    });
}

int countPixels(const PixelImage& image, Rgba colour)
{
    return static_cast<int>(std::ranges::count_if(image.pixels, [&](Rgba p) { return near(p, colour); }));
}

} // namespace

GPU_TEST(offscreen_highlight_fills_and_outlines_its_items)
{
    auto offscreen = makeOffscreen(context);
    DrawItem item = box({}, {4.0f, 4.0f, 4.0f}, {0.0f, 1.0f, 0.0f});
    item.highlight = solidHighlight(true);
    std::vector<DrawItem> items{item};
    const PixelImage image = offscreen->render(frontCamera(), items);
    CHECK(near(centre(image), Rgba{0, 0, 255, 255}));
    // A ring of outline just outside the cube; the corners stay sky.
    const int outline = countPixels(image, Rgba{255, 0, 0, 255});
    CHECK(outline > 40);
    CHECK(outline < 200);
    CHECK(near(image.at(0, 0), skyPixel()));

    // All see-through: as if there were none.
    item.highlight = std::make_shared<const HighlightStyle>(
        HighlightStyle{.fillTransparency = 1.0f, .outlineTransparency = 1.0f});
    items = {item};
    const PixelImage clear = offscreen->render(frontCamera(), items);
    items[0].highlight.reset();
    const PixelImage none = offscreen->render(frontCamera(), items);
    CHECK(clear.pixels == none.pixels);
}

GPU_TEST(offscreen_highlight_depth_modes)
{
    // A grey wall in front of the highlighted cube's middle: AlwaysOnTop
    // shows through it, Occluded doesn't.
    auto offscreen = makeOffscreen(context);
    auto middle = [&](bool alwaysOnTop) {
        DrawItem cube = box({}, {4.0f, 4.0f, 4.0f}, {0.0f, 1.0f, 0.0f});
        cube.highlight = solidHighlight(alwaysOnTop);
        std::vector<DrawItem> items{cube, box({0.0f, 0.0f, 4.0f}, {1.0f, 1.0f, 0.2f}, {0.5f, 0.5f, 0.5f})};
        return centre(offscreen->render(frontCamera(), items));
    };
    CHECK(near(middle(true), Rgba{0, 0, 255, 255}));
    const Rgba occluded = middle(false);
    CHECK(occluded.b < 200);
    CHECK(std::abs(occluded.r - occluded.b) < 30);
}
