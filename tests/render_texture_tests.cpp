#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Primitives.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using lunar::CFrame;
using lunar::Color3;
using lunar::Vector3;
using namespace lunar::render;

namespace {

constexpr std::uint32_t width = 128;
constexpr std::uint32_t height = 96;

std::unique_ptr<OffscreenRenderer> makeOffscreen(Context& context)
{
    auto offscreen = OffscreenRenderer::create(context, width, height);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    return std::move(*offscreen);
}

Camera cameraAt(Vector3 eye, Vector3 target = {})
{
    Camera camera;
    camera.cframe = Camera::lookAt(eye, target);
    return camera;
}

Lighting flatLight()
{
    Lighting lighting;
    lighting.shadows = false;
    return lighting;
}

// Where a world point lands in the image.
Rgba pixelAt(const PixelImage& image, const Camera& camera, Vector3 world)
{
    const Mat4 vp = camera.viewProjection(static_cast<float>(width) / static_cast<float>(height));
    const Vec4 clip = vp * Vec4{world.x, world.y, world.z, 1.0f};
    const float x = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width);
    const float y = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(height);
    return image.at(static_cast<std::uint32_t>(std::clamp(x, 0.0f, width - 1.0f)),
                    static_cast<std::uint32_t>(std::clamp(y, 0.0f, height - 1.0f)));
}

struct Texel {
    std::uint8_t r, g, b, a;
};

ImagePtr makeImage(std::uint32_t w, std::uint32_t h, auto&& texel)
{
    auto image = std::make_shared<lunar::Image>();
    image->width = w;
    image->height = h;
    image->rgba.resize(image->byteSize());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const Texel t = texel(x, y);
            const std::size_t i = (std::size_t{y} * w + x) * 4;
            image->rgba[i] = t.r;
            image->rgba[i + 1] = t.g;
            image->rgba[i + 2] = t.b;
            image->rgba[i + 3] = t.a;
        }
    }
    return image;
}

// Red, green / blue, white: the quadrants say which way u and v run.
ImagePtr quadrants()
{
    return makeImage(2, 2, [](std::uint32_t x, std::uint32_t y) {
        static constexpr Texel texels[2][2]
            = {{{255, 0, 0, 255}, {0, 255, 0, 255}}, {{0, 0, 255, 255}, {255, 255, 255, 255}}};
        return texels[y][x];
    });
}

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

bool white(Rgba p)
{
    return p.r > 150 && p.g > 150 && p.b > 150;
}

int brightness(Rgba p)
{
    return p.r + p.g + p.b;
}

DrawItem texturedBox(Vector3 position, Vector3 size, ImagePtr texture)
{
    DrawItem item{.model = partMatrix(CFrame(position), size), .color = {0.3f, 0.3f, 0.3f}};
    item.material = lunar::Enum::Material::Neon; // unlit, so texels come out as they are
    item.texture = std::move(texture);
    return item;
}

} // namespace

GPU_TEST(texture_shows_upright_on_every_box_face)
{
    auto offscreen = makeOffscreen(context);
    const std::vector items{texturedBox({}, {8.0f, 8.0f, 8.0f}, quadrants())};
    // Looking at each face from outside, the image should read upright:
    // red top left, green top right, blue bottom left, white bottom right.
    struct View {
        Vector3 eye;
        Vector3 right;
        Vector3 up;
    };
    const View views[] = {
        {{0.0f, 0.0f, 20.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},   // back (+Z)
        {{0.0f, 0.0f, -20.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // front (-Z)
        {{20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},  // right (+X)
        {{-20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},  // left (-X)
    };
    for (const View& view : views) {
        const Camera camera = cameraAt(view.eye);
        const PixelImage image = offscreen->render(camera, items, flatLight());
        const Vector3 face = view.eye.unit() * 4.0f;
        auto sample
            = [&](float right, float up) { return pixelAt(image, camera, face + view.right * right + view.up * up); };
        CHECK(pure(sample(-2.0f, 2.0f), Channel::Red));
        CHECK(pure(sample(2.0f, 2.0f), Channel::Green));
        CHECK(pure(sample(-2.0f, -2.0f), Channel::Blue));
        CHECK(white(sample(2.0f, -2.0f)));
    }
}

GPU_TEST(texture_overlay_alpha_shows_the_colour_beneath)
{
    auto offscreen = makeOffscreen(context);
    // Left half clear, right half opaque red: the part's blue shows on the left.
    ImagePtr half = makeImage(
        8, 8, [](std::uint32_t x, std::uint32_t) { return x < 4 ? Texel{0, 0, 0, 0} : Texel{255, 0, 0, 255}; });
    DrawItem item = texturedBox({}, {8.0f, 8.0f, 1.0f}, half);
    item.color = {0.0f, 0.0f, 1.0f};
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    const PixelImage image = offscreen->render(camera, std::vector{item}, flatLight());
    CHECK(pure(pixelAt(image, camera, {-2.5f, 0.0f, 0.5f}), Channel::Blue));
    CHECK(pure(pixelAt(image, camera, {2.5f, 0.0f, 0.5f}), Channel::Red));
}

GPU_TEST(texture_alpha_cuts_holes_or_blends)
{
    auto offscreen = makeOffscreen(context);
    ImagePtr half = makeImage(
        8, 8, [](std::uint32_t x, std::uint32_t) { return x < 4 ? Texel{255, 0, 0, 0} : Texel{255, 0, 0, 255}; });
    DrawItem behind = texturedBox({0.0f, 0.0f, -4.0f}, {20.0f, 20.0f, 1.0f}, nullptr);
    behind.color = {0.0f, 0.0f, 1.0f};
    DrawItem front = texturedBox({}, {8.0f, 8.0f, 1.0f}, half);
    front.alphaMode = TextureAlpha::Transparency;
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});

    // Fully opaque item: a cut-out. The clear half is a hole, the rest solid red.
    PixelImage image = offscreen->render(camera, std::vector{behind, front}, flatLight());
    CHECK_EQ(offscreen->renderer().stats().drawCalls, 2u);
    CHECK(pure(pixelAt(image, camera, {-2.5f, 0.0f, 0.5f}), Channel::Blue));
    CHECK(pure(pixelAt(image, camera, {2.5f, 0.0f, 0.5f}), Channel::Red));

    // Half transparent: blended, so the red half is purple and the hole still blue.
    front.transparency = 0.5f;
    image = offscreen->render(camera, std::vector{behind, front}, flatLight());
    CHECK(pure(pixelAt(image, camera, {-2.5f, 0.0f, 0.5f}), Channel::Blue));
    const Rgba mixed = pixelAt(image, camera, {2.5f, 0.0f, 0.5f});
    CHECK(mixed.r > 80);
    CHECK(mixed.b > 80);
    CHECK(mixed.g < 20);
}

GPU_TEST(texture_uploads_are_budgeted_and_see_through_items_wait)
{
    auto offscreen = makeOffscreen(context);
    // Two textures bigger than one frame's budget together: the second waits
    // a frame. Its see-through item sits that frame out rather than flash
    // solid; an opaque item draws untextured meanwhile.
    constexpr std::uint32_t side = 3700;
    static_assert(2ull * side * side * 4 > MeshRenderer::textureUploadBudget);
    auto big = [&](std::uint8_t shade) {
        return makeImage(side, side, [shade](std::uint32_t, std::uint32_t) { return Texel{shade, shade, shade, 255}; });
    };
    DrawItem opaque = texturedBox({0.0f, 5.0f, 0.0f}, {4.0f, 4.0f, 1.0f}, big(200));
    DrawItem holes = texturedBox({0.0f, -5.0f, 0.0f}, {4.0f, 4.0f, 1.0f}, big(100));
    holes.alphaMode = TextureAlpha::Transparency;
    const Camera camera = cameraAt({0.0f, 0.0f, 20.0f});
    offscreen->render(camera, std::vector{opaque, holes}, flatLight());
    CHECK_EQ(offscreen->renderer().stats().texturesUploaded, 1u);
    CHECK_EQ(offscreen->renderer().stats().texturesPending, 1u);
    CHECK_EQ(offscreen->renderer().stats().visible, 1u);
    offscreen->render(camera, std::vector{opaque, holes}, flatLight());
    CHECK_EQ(offscreen->renderer().stats().texturesUploaded, 1u);
    CHECK_EQ(offscreen->renderer().stats().texturesPending, 0u);
    CHECK_EQ(offscreen->renderer().stats().visible, 2u);
}

GPU_TEST(texture_tiles_by_uv_scale)
{
    auto offscreen = makeOffscreen(context);
    // Dark left half, bright right half: each tile adds one dark-to-bright edge.
    ImagePtr stripe = makeImage(
        16, 1, [](std::uint32_t x, std::uint32_t) { return x < 8 ? Texel{0, 0, 0, 255} : Texel{255, 255, 255, 255}; });
    DrawItem item = texturedBox({}, {12.0f, 12.0f, 1.0f}, stripe);
    item.uvScale[0] = 3.0f;
    const Camera camera = cameraAt({0.0f, 0.0f, 12.0f});
    const PixelImage image = offscreen->render(camera, std::vector{item}, flatLight());

    int rising = 0;
    bool bright = true; // the face starts on a dark half
    for (int i = 0; i <= 120; ++i) {
        const float x = -5.95f + 11.9f * static_cast<float>(i) / 120.0f;
        const int level = brightness(pixelAt(image, camera, {x, 0.0f, 0.5f}));
        if (!bright && level > 500)
            ++rising;
        if (level > 500)
            bright = true;
        else if (level < 200)
            bright = false;
    }
    CHECK_EQ(rising, 3);
}

GPU_TEST(texture_mipmaps_average_fine_detail_far_away)
{
    auto offscreen = makeOffscreen(context);
    // A one-texel checkerboard repeated 16 times: far away and tilted it
    // should blur to grey rather than shimmer black and white.
    ImagePtr checker = makeImage(256, 256, [](std::uint32_t x, std::uint32_t y) {
        return (x + y) % 2 == 0 ? Texel{0, 0, 0, 255} : Texel{255, 255, 255, 255};
    });
    DrawItem ground = texturedBox({}, {400.0f, 1.0f, 400.0f}, checker);
    ground.uvScale[0] = 16.0f;
    ground.uvScale[1] = 16.0f;
    const Camera camera = cameraAt({0.0f, 8.0f, 60.0f}, {0.0f, 0.0f, -100.0f});
    const PixelImage image = offscreen->render(camera, std::vector{ground}, flatLight());
    int lowest = 1000;
    int highest = 0;
    for (std::uint32_t y = height / 2 + 4; y < height / 2 + 12; ++y) {
        for (std::uint32_t x = width / 4; x < 3 * width / 4; ++x) {
            const int level = brightness(image.at(x, y));
            lowest = std::min(lowest, level);
            highest = std::max(highest, level);
        }
    }
    // Unlit (Neon) black and white average to a mid grey, evenly.
    CHECK(lowest > 300);
    CHECK(highest < 720);
    CHECK(highest - lowest < 90);
}

GPU_TEST(texture_normal_map_changes_shading)
{
    auto offscreen = makeOffscreen(context);
    auto flat = makeImage(4, 4, [](std::uint32_t, std::uint32_t) { return Texel{128, 128, 255, 255}; });
    // Tilted towards +u, which on the +Z face is +X: away from a sun at -X.
    auto tilted = makeImage(4, 4, [](std::uint32_t, std::uint32_t) { return Texel{230, 128, 180, 255}; });
    Lighting lighting = flatLight();
    lighting.sunDirection = Vector3{-1.0f, 0.0f, 1.0f}.unit();
    lighting.ambient = {0.1f, 0.1f, 0.1f};
    lighting.sunColor = {1.0f, 1.0f, 1.0f};

    DrawItem item{.model = partMatrix(CFrame(), {8.0f, 8.0f, 1.0f}), .color = {0.8f, 0.8f, 0.8f}};
    item.physical = true;
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    const Vector3 centre{0.0f, 0.0f, 0.5f};

    const int plain = brightness(pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, centre));
    item.normalMap = flat;
    const int flatLevel = brightness(pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, centre));
    item.normalMap = tilted;
    const int tiltedLevel = brightness(pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, centre));

    CHECK(std::abs(plain - flatLevel) <= 6);
    CHECK(tiltedLevel + 40 < flatLevel);
}

GPU_TEST(texture_cache_uploads_each_image_once_and_frees_unused_ones)
{
    auto offscreen = makeOffscreen(context);
    ImagePtr shared = quadrants();
    ImagePtr other = quadrants();
    std::vector<DrawItem> items;
    items.reserve(20);
    for (int i = 0; i < 20; ++i) {
        const auto column = static_cast<float>(i % 5);
        const auto row = static_cast<float>(i / 5); // NOLINT(bugprone-integer-division): whole rows
        items.push_back(
            texturedBox({column * 3.0f - 6.0f, row * 3.0f - 4.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, i == 7 ? other : shared));
    }
    // The same image as data (a normal map) is a texture of its own.
    items[3].normalMap = shared;
    const Camera camera = cameraAt({0.0f, 0.0f, 30.0f});
    offscreen->render(camera, items, flatLight());
    CHECK_EQ(offscreen->renderer().stats().texturesUploaded, 3u);
    CHECK_EQ(offscreen->renderer().textureCount(), 3u);
    offscreen->render(camera, items, flatLight());
    CHECK_EQ(offscreen->renderer().stats().texturesUploaded, 0u);
    CHECK_EQ(offscreen->renderer().textureCount(), 3u);
    // Textured instances still share one draw per mesh.
    CHECK_EQ(offscreen->renderer().stats().drawCalls, 1u);

    for (std::uint64_t i = 0; i <= MeshRenderer::keepUnusedFrames + 4; ++i)
        offscreen->render(camera, {}, flatLight());
    CHECK_EQ(offscreen->renderer().textureCount(), 0u);
    // And they come back when used again.
    offscreen->render(camera, items, flatLight());
    CHECK_EQ(offscreen->renderer().stats().texturesUploaded, 3u);
}

GPU_TEST(texture_streams_finer_levels_as_the_camera_comes_closer)
{
    auto offscreen = makeOffscreen(context);
    // Red, green / blue, white quadrants, 512 texels across.
    ImagePtr big = makeImage(512, 512, [](std::uint32_t x, std::uint32_t y) {
        static constexpr Texel texels[2][2]
            = {{{255, 0, 0, 255}, {0, 255, 0, 255}}, {{0, 0, 255, 255}, {255, 255, 255, 255}}};
        return texels[y / 256][x / 256];
    });
    const DrawItem item = texturedBox({}, {4.0f, 4.0f, 4.0f}, big);
    const FrameStats& stats = offscreen->renderer().stats();
    constexpr std::uint64_t fullBytes = 512ull * 512 * 4 * 4 / 3;

    // A few pixels big: a small level is all it gets.
    offscreen->render(cameraAt({0.0f, 0.0f, 200.0f}), std::vector{item}, flatLight());
    CHECK_EQ(stats.texturesResident, 1u);
    CHECK(stats.textureBytes > 0);
    CHECK(stats.textureBytes < fullBytes / 4);

    // Filling the view it wants every level; they come over the next frames.
    const Camera near = cameraAt({0.0f, 0.0f, 6.0f});
    PixelImage image;
    std::uint32_t resized = 0;
    for (int i = 0; i < 4; ++i) {
        image = offscreen->render(near, std::vector{item}, flatLight());
        resized += stats.texturesResized;
    }
    CHECK_EQ(resized, 1u);
    CHECK(stats.textureBytes >= fullBytes);
    CHECK(pure(pixelAt(image, near, {-1.0f, 1.0f, 2.0f}), Channel::Red));
    CHECK(pure(pixelAt(image, near, {1.0f, 1.0f, 2.0f}), Channel::Green));
    CHECK(pure(pixelAt(image, near, {-1.0f, -1.0f, 2.0f}), Channel::Blue));
    CHECK(white(pixelAt(image, near, {1.0f, -1.0f, 2.0f})));
}

GPU_TEST(texture_shrinks_to_fit_the_memory_budget_and_grows_back)
{
    // A budget too small for anything leaves the textures their minimum,
    // 16 MiB: far less than sixteen 1024x1024 textures at full size.
    setenv("LUNAR_GPU_BUDGET_MB", "1", 1); // NOLINT(concurrency-mt-unsafe): the tests run on one thread
    auto offscreen = makeOffscreen(context);
    // A 4 x 4 grid of boxes, 5 studs apart.
    auto spot = [](int i) {
        return Vector3{static_cast<float>(i % 4) * 5.0f - 7.5f, static_cast<float>(i >> 2) * 5.0f - 7.5f, 0.0f};
    };
    std::vector<DrawItem> items;
    for (int i = 0; i < 16; ++i) {
        const auto shade = static_cast<std::uint8_t>(40 + i * 10);
        DrawItem item = texturedBox(
            spot(i), {4.0f, 4.0f, 1.0f},
            makeImage(1024, 1024, [shade](std::uint32_t, std::uint32_t) { return Texel{0, shade, 0, 255}; }));
        // Magnified sixteen times, so every one wants its finest level.
        item.uvScale[0] = item.uvScale[1] = 1.0f / 16.0f;
        items.push_back(std::move(item));
    }
    const Camera camera = cameraAt({0.0f, 0.0f, 24.0f});
    const FrameStats& stats = offscreen->renderer().stats();
    PixelImage image = offscreen->render(camera, items, flatLight());
    for (int i = 0; i < 40 && stats.texturesPending > 0; ++i)
        image = offscreen->render(camera, items, flatLight());
    CHECK_EQ(stats.texturesPending, 0u);
    CHECK_EQ(stats.texturesResident, 16u);
    CHECK(stats.textureBytes <= stats.textureBudget);
    CHECK(stats.textureQuality < 3);
    CHECK_EQ(stats.outOfMemory, 0u);
    // Each still shows its own shade of green.
    int previous = 0;
    for (int i = 0; i < 16; ++i) {
        const Rgba p = pixelAt(image, camera, spot(i) + Vector3{0.0f, 0.0f, 0.5f});
        const int green = p.g;
        CHECK(p.r < 10);
        CHECK(p.b < 10);
        CHECK(green > previous);
        previous = green;
    }

    // Room again: the quality level comes back and the textures grow.
    unsetenv("LUNAR_GPU_BUDGET_MB"); // NOLINT(concurrency-mt-unsafe)
    constexpr std::uint64_t fullBytes = 16ull * 1024 * 1024 * 4 * 4 / 3;
    for (int i = 0; i < 400 && (stats.textureQuality < 3 || stats.textureBytes < fullBytes); ++i)
        offscreen->render(camera, items, flatLight());
    CHECK_EQ(stats.textureQuality, 3);
    CHECK(stats.textureBytes >= fullBytes);
}

GPU_TEST(texture_block_compressed_sources_stay_compressed)
{
    if (!context.textureCompressionBC())
        SKIP("the device can't sample BC textures");
    auto offscreen = makeOffscreen(context);
    // Red as RGBA, blue as BC1 blocks (colour 0 pure blue, every index 0):
    // what shows says which was uploaded.
    auto image = std::make_shared<lunar::Image>(
        *makeImage(256, 256, [](std::uint32_t, std::uint32_t) { return Texel{255, 0, 0, 255}; }));
    auto blocks = std::make_shared<lunar::BlockCompressed>();
    blocks->format = lunar::BlockCompressed::Format::Bc1;
    for (std::uint32_t side = 256; side >= 1; side /= 2) {
        const std::size_t count = std::size_t{(side + 3) / 4} * ((side + 3) / 4);
        auto& level = blocks->levels.emplace_back();
        for (std::size_t i = 0; i < count; ++i)
            level.insert(level.end(), {0x1f, 0x00, 0x00, 0x00, 0, 0, 0, 0});
    }
    image->compressed = blocks;
    const DrawItem item = texturedBox({}, {4.0f, 4.0f, 4.0f}, image);
    const Camera camera = cameraAt({0.0f, 0.0f, 6.0f});
    const PixelImage frame = offscreen->render(camera, std::vector{item}, flatLight());
    CHECK(pure(pixelAt(frame, camera, {0.0f, 0.0f, 2.0f}), Channel::Blue));
    // Half a byte a texel instead of four.
    CHECK(offscreen->renderer().stats().textureBytes < 256ull * 256 * 4 / 4);

    // Far away, its finer levels are left out like any texture's.
    offscreen->render(cameraAt({0.0f, 0.0f, 300.0f}), std::vector{item}, flatLight());
    for (std::uint64_t i = 0; i <= MeshRenderer::keepUnusedFrames + 4; ++i)
        offscreen->render(camera, {}, flatLight());
    offscreen->render(cameraAt({0.0f, 0.0f, 300.0f}), std::vector{item}, flatLight());
    CHECK(offscreen->renderer().stats().textureBytes < 256ull * 256 / 2 / 4);
}

GPU_TEST(texture_ignores_malformed_images)
{
    auto offscreen = makeOffscreen(context);
    auto broken = std::make_shared<lunar::Image>();
    broken->width = 4;
    broken->height = 4;
    broken->rgba.resize(7);
    DrawItem item = texturedBox({}, {4.0f, 4.0f, 4.0f}, broken);
    item.color = {0.0f, 0.0f, 1.0f};
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    const PixelImage image = offscreen->render(camera, std::vector{item}, flatLight());
    CHECK_EQ(offscreen->renderer().textureCount(), 0u);
    CHECK_EQ(offscreen->renderer().stats().visible, 1u);
    // Drawn untextured, in the part's colour.
    CHECK(pure(pixelAt(image, camera, {0.0f, 0.0f, 2.0f}), Channel::Blue));
}

namespace {

std::shared_ptr<lunar::Part> addPart(lunar::Instance* parent, Vector3 position, Vector3 size)
{
    auto part = std::make_shared<lunar::Part>();
    part->setPosition(position);
    part->setSize(size);
    part->setColor({0.5f, 0.5f, 0.5f});
    part->setParent(parent);
    return part;
}

std::shared_ptr<lunar::Decal> addDecal(lunar::Instance* part, std::string id, lunar::Enum::NormalId face)
{
    auto decal = std::make_shared<lunar::Decal>();
    decal->setTexture(std::move(id));
    decal->setFace(face);
    decal->setParent(part);
    return decal;
}

// Serves fixed images by id.
TextureProvider serve(std::map<std::string, ImagePtr, std::less<>> images)
{
    return [images = std::move(images)](std::string_view id) -> ImagePtr {
        auto it = images.find(id);
        return it == images.end() ? nullptr : it->second;
    };
}

// Sun from behind the camera, so faces towards it are lit evenly.
Lighting frontLight(Vector3 towardsCamera)
{
    Lighting lighting = flatLight();
    lighting.sunDirection = towardsCamera.unit();
    return lighting;
}

} // namespace

GPU_TEST(texture_decal_shows_upright_on_the_front_face)
{
    auto offscreen = makeOffscreen(context);
    auto game = lunar::DataModel::create();
    auto part = addPart(game->workspace(), {}, {8.0f, 8.0f, 2.0f});
    addDecal(part.get(), "quadrants", lunar::Enum::NormalId::Front);
    const auto items = extractScene(*game->workspace(), {}, serve({{"quadrants", quadrants()}}));
    CHECK_EQ(items.size(), 2u);

    // Front is -Z; seen from there, the viewer's right is -X.
    const Camera camera = cameraAt({0.0f, 0.0f, -15.0f});
    const PixelImage image = offscreen->render(camera, items, frontLight({0.0f, 0.0f, -1.0f}));
    auto sample = [&](float right, float up) { return pixelAt(image, camera, {-right, up, -1.0f}); };
    CHECK(pure(sample(-3.0f, 3.0f), Channel::Red));
    CHECK(pure(sample(3.0f, 3.0f), Channel::Green));
    CHECK(pure(sample(-3.0f, -3.0f), Channel::Blue));
    CHECK(white(sample(3.0f, -3.0f)));
    // The back face has no decal: plain grey.
    const PixelImage back = offscreen->render(cameraAt({0.0f, 0.0f, 15.0f}), items, frontLight({0.0f, 0.0f, 1.0f}));
    const Rgba grey = pixelAt(back, cameraAt({0.0f, 0.0f, 15.0f}), {2.0f, 2.0f, 1.0f});
    CHECK(std::abs(grey.r - grey.b) < 4);
}

GPU_TEST(texture_decals_stack_by_z_index)
{
    auto offscreen = makeOffscreen(context);
    const TextureProvider provider = serve({
        {"red", makeImage(2, 2, [](std::uint32_t, std::uint32_t) { return Texel{255, 0, 0, 255}; })},
        {"blue", makeImage(2, 2, [](std::uint32_t, std::uint32_t) { return Texel{0, 0, 255, 255}; })},
    });
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    for (int redOnTop = 0; redOnTop < 2; ++redOnTop) {
        auto game = lunar::DataModel::create();
        auto part = addPart(game->workspace(), {}, {8.0f, 8.0f, 2.0f});
        addDecal(part.get(), "red", lunar::Enum::NormalId::Back)->setZIndex(redOnTop != 0 ? 2 : 1);
        addDecal(part.get(), "blue", lunar::Enum::NormalId::Back)->setZIndex(redOnTop != 0 ? 1 : 2);
        const PixelImage image
            = offscreen->render(camera, extractScene(*game->workspace(), {}, provider), frontLight({0.0f, 0.0f, 1.0f}));
        CHECK(pure(pixelAt(image, camera, {0.0f, 0.0f, 1.0f}), redOnTop != 0 ? Channel::Red : Channel::Blue));
    }
}

GPU_TEST(texture_decal_transparency_and_alpha_blend_over_the_part)
{
    auto offscreen = makeOffscreen(context);
    // Left half clear, right half red; the decal half transparent.
    const TextureProvider provider = serve({{"half", makeImage(8, 8, [](std::uint32_t x, std::uint32_t) {
                                                 return x < 4 ? Texel{255, 0, 0, 0} : Texel{255, 0, 0, 255};
                                             })}});
    auto game = lunar::DataModel::create();
    auto part = addPart(game->workspace(), {}, {8.0f, 8.0f, 2.0f});
    part->setColor({0.0f, 0.0f, 1.0f});
    addDecal(part.get(), "half", lunar::Enum::NormalId::Back)->setTransparency(0.5f);
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    const PixelImage image
        = offscreen->render(camera, extractScene(*game->workspace(), {}, provider), frontLight({0.0f, 0.0f, 1.0f}));
    CHECK(pure(pixelAt(image, camera, {-2.5f, 0.0f, 1.0f}), Channel::Blue));
    const Rgba mixed = pixelAt(image, camera, {2.5f, 0.0f, 1.0f});
    CHECK(mixed.r > 60);
    CHECK(mixed.b > 60);
}

GPU_TEST(texture_instances_tile_across_the_face)
{
    auto offscreen = makeOffscreen(context);
    const TextureProvider provider = serve({{"stripe", makeImage(16, 1, [](std::uint32_t x, std::uint32_t) {
                                                 return x < 8 ? Texel{0, 0, 0, 255} : Texel{255, 255, 255, 255};
                                             })}});
    auto game = lunar::DataModel::create();
    auto part = addPart(game->workspace(), {}, {12.0f, 12.0f, 1.0f});
    auto texture = std::make_shared<lunar::Texture>();
    texture->setTexture("stripe");
    texture->setFace(lunar::Enum::NormalId::Back);
    texture->setStudsPerTileU(4.0f); // three tiles across
    texture->setParent(part);
    const Camera camera = cameraAt({0.0f, 0.0f, 12.0f});
    const PixelImage image
        = offscreen->render(camera, extractScene(*game->workspace(), {}, provider), frontLight({0.0f, 0.0f, 1.0f}));
    int rising = 0;
    bool bright = true;
    for (int i = 0; i <= 120; ++i) {
        const float x = -5.95f + 11.9f * static_cast<float>(i) / 120.0f;
        const int level = brightness(pixelAt(image, camera, {x, 0.0f, 0.5f}));
        if (!bright && level > 400)
            ++rising;
        if (level > 400)
            bright = true;
        else if (level < 150)
            bright = false;
    }
    CHECK_EQ(rising, 3);
}

GPU_TEST(texture_stud_surfaces_shade_the_top)
{
    auto offscreen = makeOffscreen(context);
    auto spread = [&](lunar::Enum::SurfaceType surface, const SurfaceTextureProvider& provider) {
        auto game = lunar::DataModel::create();
        addPart(game->workspace(), {}, {8.0f, 1.0f, 8.0f})->setTopSurface(surface);
        offscreen->renderer().setSurfaceTextures(provider);
        const Camera camera = cameraAt({0.0f, 12.0f, 0.01f});
        const PixelImage image = offscreen->render(camera, extractScene(*game->workspace()), flatLight());
        int lowest = 1000;
        int highest = 0;
        for (int i = 0; i < 40; ++i) {
            for (int j = 0; j < 40; ++j) {
                const Vector3 p{-3.0f + 0.05f * static_cast<float>(i), 0.5f, -1.0f + 0.05f * static_cast<float>(j)};
                const int level = brightness(pixelAt(image, camera, p));
                lowest = std::min(lowest, level);
                highest = std::max(highest, level);
            }
        }
        return highest - lowest;
    };
    CHECK(spread(lunar::Enum::SurfaceType::Smooth, {}) < 4);
    CHECK(spread(lunar::Enum::SurfaceType::Studs, {}) > 20); // procedural studs
    // A surface texture replaces them: a flat mid grey tile leaves the top plain.
    ImagePtr grey = makeImage(4, 4, [](std::uint32_t, std::uint32_t) { return Texel{188, 188, 188, 255}; });
    CHECK(spread(lunar::Enum::SurfaceType::Studs, [&](lunar::Enum::SurfaceType) { return grey; }) < 4);
}

namespace {

// Brightness spread along a line of world points.
int spreadAlong(const PixelImage& image, const Camera& camera, Vector3 from, Vector3 to)
{
    int lowest = 1000;
    int highest = 0;
    for (int i = 0; i <= 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        const int level = brightness(pixelAt(image, camera, from + (to - from) * t));
        lowest = std::min(lowest, level);
        highest = std::max(highest, level);
    }
    return highest - lowest;
}

} // namespace

GPU_TEST(texture_material_textures_tile_parts_and_terrain)
{
    auto offscreen = makeOffscreen(context);
    // Plastic (no procedural pattern) gets a stripe texture two studs per tile.
    ImagePtr stripe = makeImage(16, 16, [](std::uint32_t x, std::uint32_t) {
        return x < 8 ? Texel{60, 60, 60, 255} : Texel{255, 255, 255, 255};
    });
    const Camera camera = cameraAt({0.0f, 0.0f, 15.0f});
    DrawItem part{.model = partMatrix(CFrame(), {8.0f, 8.0f, 1.0f}), .color = {0.8f, 0.8f, 0.8f}};

    PixelImage image = offscreen->render(camera, std::vector{part}, frontLight({0.0f, 0.0f, 1.0f}));
    CHECK(spreadAlong(image, camera, {-3.0f, 0.0f, 0.5f}, {3.0f, 0.0f, 0.5f}) < 4);

    offscreen->renderer().setMaterialTextures([&](lunar::Enum::Material material, std::string_view) {
        return material == lunar::Enum::Material::Plastic ? MaterialTextures{stripe, nullptr, 2.0f}
                                                          : MaterialTextures{};
    });
    image = offscreen->render(camera, std::vector{part}, frontLight({0.0f, 0.0f, 1.0f}));
    CHECK(spreadAlong(image, camera, {-3.0f, 0.0f, 0.5f}, {3.0f, 0.0f, 0.5f}) > 100);
    // Stripes run along v: no change going up the face.
    CHECK(spreadAlong(image, camera, {0.5f, -3.0f, 0.5f}, {0.5f, 3.0f, 0.5f}) < 10);

    // A world mesh (terrain) is projected from above, keeping its vertex colours.
    auto ground = std::make_shared<lunar::MeshData>();
    const Vector3 up{0.0f, 1.0f, 0.0f};
    const Color3 green{0.2f, 0.8f, 0.2f};
    ground->vertices = {{{-20.0f, 0.0f, -20.0f}, up, 0.0f, 0.0f, green},
                        {{-20.0f, 0.0f, 20.0f}, up, 0.0f, 0.0f, green},
                        {{20.0f, 0.0f, 20.0f}, up, 0.0f, 0.0f, green},
                        {{20.0f, 0.0f, -20.0f}, up, 0.0f, 0.0f, green}};
    ground->indices = {0, 1, 2, 0, 2, 3};
    const Camera above = cameraAt({0.0f, 15.0f, 0.01f});
    offscreen->renderer().setMaterialTextures([&](lunar::Enum::Material material, std::string_view) {
        return material == lunar::Enum::Material::Grass ? MaterialTextures{stripe, nullptr, 2.0f} : MaterialTextures{};
    });
    image = offscreen->render(above, std::vector{worldMesh(ground, lunar::Enum::Material::Grass)},
                              frontLight({0.0f, 1.0f, 0.0f}));
    CHECK(spreadAlong(image, above, {-3.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}) > 100);
    const Rgba sample = pixelAt(image, above, {1.5f, 0.0f, 0.0f}); // a bright stripe
    CHECK(sample.g > sample.r + 40);
}

GPU_TEST(texture_physical_roughness_and_metalness_change_highlights)
{
    auto offscreen = makeOffscreen(context);
    auto level = [](std::uint8_t v) {
        return makeImage(2, 2, [v](std::uint32_t, std::uint32_t) { return Texel{v, v, v, 255}; });
    };
    // Sun and camera mirrored about the face's normal: a highlight in the middle.
    Lighting lighting = flatLight();
    lighting.sunDirection = Vector3{0.0f, 0.5f, 1.0f}.unit();
    const Camera camera = cameraAt({0.0f, -7.5f, 15.0f}, {0.0f, 0.0f, 0.0f});
    DrawItem item{.model = partMatrix(CFrame(), {12.0f, 12.0f, 1.0f}), .color = {0.3f, 0.3f, 0.6f}};
    item.physical = true;

    auto centre = [&] {
        return brightness(pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, {0.0f, 0.0f, 0.5f}));
    };
    item.roughnessMap = level(255);
    const int rough = centre();
    item.roughnessMap = level(40);
    const int smooth = centre();
    CHECK(smooth > rough + 60);
    // Metal tints the highlight with its colour: blue goes up more than red.
    item.roughnessMap = level(150);
    const Rgba dielectric = pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, {0.0f, 0.0f, 0.5f});
    item.metalnessMap = level(255);
    const Rgba metal = pixelAt(offscreen->render(camera, std::vector{item}, lighting), camera, {0.0f, 0.0f, 0.5f});
    CHECK(metal.b - metal.r > dielectric.b - dielectric.r + 20);
}
