#include "render_gpu.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/GameUi.hpp>
#include <lunar/ui/UiRenderer.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>

using namespace lunar::ui;
namespace render = lunar::render;

namespace {

constexpr std::uint32_t width = 160;
constexpr std::uint32_t height = 120;

struct Fixture {
    std::unique_ptr<render::OffscreenRenderer> offscreen;
    std::unique_ptr<UiRenderer> ui;

    explicit Fixture(render::Context& context)
    {
        auto o = render::OffscreenRenderer::create(context, width, height);
        if (!o)
            throw lt::Failure{o.error()};
        offscreen = std::move(*o);
        auto u = UiRenderer::create(context, render::OffscreenRenderer::colorFormat, 1);
        if (!u)
            throw lt::Failure{u.error()};
        ui = std::move(*u);
        offscreen->renderer().setOverlay(ui.get());
    }
    ~Fixture() { offscreen->renderer().setOverlay(nullptr); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    render::PixelImage draw(const DrawList& list, GlyphAtlas& atlas) const
    {
        ui->submit(list, atlas);
        render::Camera camera;
        render::Lighting lighting;
        lighting.sky = {0.0f, 0.0f, 0.0f}; // black behind the UI
        auto image = offscreen->render(camera, {}, lighting);
        if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP")) {
            std::ofstream(std::filesystem::path(dir) / "ui_render.png", std::ios::binary) << render::encodePng(image);
        }
        return image;
    }
};

int channelDiff(render::Rgba p, int r, int g, int b)
{
    return std::max({std::abs(p.r - r), std::abs(p.g - g), std::abs(p.b - b)});
}

} // namespace

GPU_TEST(ui_draws_solid_and_rounded_rects)
{
    Fixture f(context);
    GlyphAtlas atlas;
    DrawList list;
    const RectF all{0, 0, width, height};
    list.rect({10, 10, 40, 30}, {1, 0, 0, 1}, all);
    // Half-transparent white over black: sRGB 50% grey when blended in sRGB
    // space, about 186 when blended in linear space.
    list.rect({60, 10, 40, 30}, {1, 1, 1, 0.5f}, all);
    Quad rounded;
    rounded.rect = {110, 10, 40, 40};
    rounded.color = {0, 1, 0, 1};
    rounded.radii = {20, 20, 20, 20};
    rounded.clip = all;
    list.push(rounded);
    // Clipped away entirely.
    list.rect({10, 60, 20, 20}, {1, 1, 0, 1}, {0, 0, 5, 5});
    // A stroke ring.
    Quad ring;
    ring.rect = {60, 60, 40, 40};
    ring.color = {0, 0, 1, 1};
    ring.stroke = 3;
    ring.clip = all;
    list.push(ring);

    const render::PixelImage image = f.draw(list, atlas);
    CHECK(channelDiff(image.at(20, 20), 255, 0, 0) <= 1);
    CHECK(channelDiff(image.at(5, 5), 0, 0, 0) <= 1);
    // Through the offscreen target's UNORM view the blend happens on encoded values.
    CHECK(channelDiff(image.at(80, 20), 128, 128, 128) <= 1);
    CHECK(channelDiff(image.at(130, 30), 0, 255, 0) <= 1); // centre of the circle
    CHECK(channelDiff(image.at(111, 11), 0, 0, 0) <= 1);   // corner cut off by the radius
    CHECK(channelDiff(image.at(20, 70), 0, 0, 0) <= 1);    // clipped
    CHECK(channelDiff(image.at(61, 80), 0, 0, 255) <= 1);  // on the ring
    CHECK(channelDiff(image.at(80, 80), 0, 0, 0) <= 1);    // inside the ring
}

GPU_TEST(ui_layout_order_keeps_hierarchy_paint_order)
{
    using namespace lunar;
    Fixture f(context);
    FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    GlyphAtlas atlas;
    for (const auto mode : {Enum::ZIndexBehavior::Global, Enum::ZIndexBehavior::Sibling}) {
        auto game = DataModel::create();
        auto screen = std::make_shared<ScreenGui>();
        screen->setIgnoreGuiInset(true);
        screen->setZIndexBehavior(mode);
        screen->setParent(game->getService<StarterGui>());
        auto first = std::make_shared<Frame>();
        first->setSize(UDim2::fromOffset(80, 100));
        first->setLayoutOrder(1);
        first->setBorderSizePixel(0);
        first->setBackgroundColor3({1, 0, 0});
        first->setParent(screen);
        auto label = std::make_shared<Frame>();
        label->setPosition(UDim2::fromOffset(-10, 0));
        label->setSize(UDim2::fromOffset(20, 50));
        label->setBorderSizePixel(0);
        label->setBackgroundColor3({0, 1, 0});
        label->setParent(first);
        auto second = std::make_shared<Frame>();
        second->setSize(UDim2::fromOffset(80, 100));
        second->setBorderSizePixel(0);
        second->setBackgroundColor3({0, 0, 1});
        second->setParent(screen);
        auto grid = std::make_shared<UIGridLayout>();
        grid->setCellSize(UDim2::fromOffset(80, 100));
        grid->setCellPadding({});
        grid->setParent(screen);
        GuiEngine gui(fonts, atlas);
        gui.setRoots({game->getService<StarterGui>()});
        gui.settings().topInset = 0;
        gui.update({width, height}, 0);
        DrawList list;
        gui.draw(list);
        const auto image = f.draw(list, atlas);
        CHECK(channelDiff(image.at(75, 10), 0, 0, 255) <= 1);
        CHECK(channelDiff(image.at(85, 10), 0, 255, 0) <= 1);
        CHECK(channelDiff(image.at(100, 10), 255, 0, 0) <= 1);
    }
}

GPU_TEST(ui_draws_emoji_in_their_own_colors)
{
    FontLibrary fonts(lunar::assets::AssetConfig::fromEnvironment().contentDir);
    if (!std::filesystem::exists(fonts.contentDir() / "fonts/TwemojiMozilla.ttf"))
        SKIP("no Roblox content folder with fonts");
    Fixture f(context);
    GlyphAtlas atlas;
    DrawList list;
    TextParams params;
    params.text = "\U0001F600";
    params.style.font = fontFromEnum(lunar::Enum::Font::SourceSans);
    params.style.size = 48;
    params.style.color = {0, 0, 1}; // blue text: the face stays yellow
    params.box = {width, height};
    list.text(atlas, layoutText(fonts, params), 0, 0, {0, 0, width, height});
    const render::PixelImage image = f.draw(list, atlas);

    int yellow = 0;
    int blue = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const render::Rgba p = image.at(x, y);
            yellow += p.r > 200 && p.g > 150 && p.b < 120;
            blue += p.b > 200 && p.r < 80;
        }
    }
    CHECK(yellow > 300);
    CHECK_EQ(blue, 0);
}

GPU_TEST(ui_draws_text)
{
    FontLibrary fonts(lunar::assets::AssetConfig::fromEnvironment().contentDir);
    if (!std::filesystem::exists(fonts.contentDir() / "fonts/SourceSansPro-Regular.ttf"))
        SKIP("no Roblox content folder with fonts");
    Fixture f(context);
    GlyphAtlas atlas;
    DrawList list;
    TextParams params;
    params.text = "Hello";
    params.style.font = fontFromEnum(lunar::Enum::Font::SourceSansBold);
    params.style.size = 32;
    params.style.color = {1, 1, 1};
    params.box = {width, height};
    const TextLayout layout = layoutText(fonts, params);
    list.text(atlas, layout, 0, 0, {0, 0, width, height});
    const render::PixelImage image = f.draw(list, atlas);

    // White text pixels inside the centred bounds, nothing outside.
    int lit = 0;
    int outside = 0;
    const float x0 = (width - layout.bounds.x) / 2;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (image.at(x, y).r < 128)
                continue;
            const bool inBox = static_cast<float>(x) >= x0 - 2 && static_cast<float>(x) <= x0 + layout.bounds.x + 2
                               && y >= 44 && y <= 76;
            (inBox ? lit : outside)++;
        }
    }
    CHECK(lit > 150);
    CHECK_EQ(outside, 0);
}

GPU_TEST(ui_renders_a_place_gui)
{
    const auto contentDir = lunar::assets::AssetConfig::fromEnvironment().contentDir;
    if (!std::filesystem::exists(contentDir / "fonts/SourceSansPro-Regular.ttf"))
        SKIP("no Roblox content folder with fonts");
    auto bytes = lunar::readFile(std::filesystem::path(LUNAR_TEST_DATA) / "ui_showcase.rbxlx");
    CHECK(bytes.has_value());
    auto place = lunar::loadPlace(*bytes);
    CHECK(place.has_value());

    constexpr std::uint32_t w = 1280;
    constexpr std::uint32_t h = 720;
    auto offscreen = render::OffscreenRenderer::create(context, w, h);
    CHECK(offscreen.has_value());
    GameUi::Options options;
    options.contentDir = contentDir;
    auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *place->game, options);
    CHECK(ui.has_value());
    (*ui)->attach((*offscreen)->renderer());
    (*ui)->update(0.0, {w, h});
    render::Lighting lighting;
    lighting.sky = {0.0f, 0.0f, 0.0f};
    const render::PixelImage image = (*offscreen)->render({}, {}, lighting);
    if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP"))
        std::ofstream(std::filesystem::path(dir) / "ui_place.png", std::ios::binary) << render::encodePng(image);

    // The 520x360 panel is centred under the modern 58 px inset.
    constexpr std::uint32_t panelX = (w - 520) / 2;
    constexpr std::uint32_t panelY = (h + static_cast<std::uint32_t>(Topbar::height) - 360) / 2;
    // The Play button
    // (0, 162, 255) sits 16 px in and 64 px down, 180x40 with rounded corners.
    CHECK(channelDiff(image.at(panelX + 16 + 20, panelY + 64 + 5), 0, 162, 255) <= 2);
    CHECK(channelDiff(image.at(panelX + 16, panelY + 64), 40, 44, 52) <= 2); // corner cut off: panel colour
    // Panel body.
    CHECK(channelDiff(image.at(panelX + 100, panelY + 200), 40, 44, 52) <= 2);
    // The modern chat log: (25, 27, 29) at 0.3 transparency over black.
    CHECK(channelDiff(image.at(300, 150), 18, 19, 20) <= 2);
    // The grid's first cell is a red circle, 40 px wide.
    CHECK(channelDiff(image.at(panelX + 212 + 6 + 20, panelY + 64 + 6 + 20), 255, 90, 90) <= 2);
    (*ui)->detach();
}

// A joined game's picture (--net-join/--net-replay --render): the local
// player's PlayerGui over the 3D view. StarterGui only fills it; it isn't
// drawn itself once there is a local player.
GPU_TEST(ui_draws_the_local_players_gui)
{
    auto game = lunar::DataModel::create();
    auto* players = game->getService<lunar::Players>();
    auto player = std::make_shared<lunar::Player>();
    player->setName("Player1");
    player->setParent(players);
    players->assignLocalPlayer(player.get());
    auto playerGui = std::make_shared<lunar::PlayerGui>();
    playerGui->setName("PlayerGui");
    playerGui->setParent(player);
    auto frameIn = [](lunar::Instance& container, lunar::Color3 color, int x) {
        auto screen = std::make_shared<lunar::ScreenGui>();
        screen->setIgnoreGuiInset(true);
        screen->setParent(&container);
        auto frame = std::make_shared<lunar::Frame>();
        frame->setBackgroundColor3(color);
        frame->setBorderSizePixel(0);
        frame->setPosition(lunar::UDim2::fromOffset(x, 10));
        frame->setSize(lunar::UDim2::fromOffset(40, 40));
        frame->setParent(screen);
    };
    frameIn(*playerGui, {1, 0, 0}, 10);
    frameIn(*game->getService("StarterGui"), {0, 0, 1}, 80);

    auto offscreen = render::OffscreenRenderer::create(context, width, height);
    CHECK(offscreen.has_value());
    GameUi::Options options;
    options.showChat = false;
    options.showCoreGui = false;
    options.map.minimap = false;
    auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
    CHECK(ui.has_value());
    (*ui)->attach((*offscreen)->renderer());
    (*ui)->update(0.0, {width, height});
    render::Lighting lighting;
    lighting.sky = {0.0f, 0.0f, 0.0f};
    const render::PixelImage image = (*offscreen)->render({}, {}, lighting);
    CHECK(channelDiff(image.at(30, 30), 255, 0, 0) <= 1); // the PlayerGui's frame
    CHECK(channelDiff(image.at(100, 30), 0, 0, 0) <= 1);  // nothing of StarterGui's
    (*ui)->detach();
}

// The Escape menu through GameUi, the way a game window draws it: slid in
// from the right over a dimmed screen, at 1280x720 and at 4K with the GUI
// scaled twice. LUNAR_UI_TEST_DUMP saves the pictures. Its Reset (R, then
// Enter on the question) kills the local character.
GPU_TEST(ui_draws_the_escape_menu)
{
    const auto contentDir = lunar::assets::AssetConfig::fromEnvironment().contentDir;
    if (!std::filesystem::exists(contentDir / "fonts/BuilderSans-Regular.otf"))
        SKIP("no Roblox content folder with fonts");
    auto game = lunar::DataModel::create();
    auto* players = game->getService<lunar::Players>();
    auto player = std::make_shared<lunar::Player>();
    player->setName("builderman");
    player->setParent(players);
    players->assignLocalPlayer(player.get());
    auto character = std::make_shared<lunar::Model>();
    auto humanoid = std::make_shared<lunar::Humanoid>();
    humanoid->setParent(character);
    character->setParent(game->workspace());
    player->setCharacter(character.get());

    for (const auto& [w, h, scale, name] :
         {std::tuple{1280u, 720u, 1.0f, "ui_menu.png"}, std::tuple{3840u, 2160u, 2.0f, "ui_menu_4k.png"}}) {
        auto offscreen = render::OffscreenRenderer::create(context, w, h);
        CHECK(offscreen.has_value());
        GameUi::Options options;
        options.contentDir = contentDir;
        options.showChat = false;
        options.map.minimap = false;
        auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
        CHECK(ui.has_value());
        (*ui)->attach((*offscreen)->renderer());
        const lunar::Vector2 size{static_cast<float>(w), static_cast<float>(h)};
        (*ui)->update(0.0, size, scale);
        CHECK((*ui)->handle(lunar::platform::KeyEvent{.key = lunar::platform::Key::Escape, .pressed = true}));
        CHECK((*ui)->menuOpen());
        for (int i = 0; i < 30; ++i)
            (*ui)->update(1.0 / 60.0, size, scale);
        render::Lighting lighting;
        lighting.sky = {0.8f, 0.8f, 0.8f};
        const render::PixelImage image = (*offscreen)->render({}, {}, lighting);
        if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP"))
            std::ofstream(std::filesystem::path(dir) / name, std::ios::binary) << render::encodePng(image);
        // The game dimmed on the left, the dark panel on the right, a quarter
        // of the way down (under the buttons on Home: the panel itself).
        const render::Rgba sky = image.at(10, h / 2);
        const render::Rgba panel = image.at(w - static_cast<std::uint32_t>(40 * scale), h - h / 8);
        CHECK(sky.r > 80);
        CHECK(sky.r < 200);
        CHECK(panel.r < 40);
        CHECK(panel.g < 40);
        (*ui)->detach();
    }

    // The default Reset Character: the Humanoid's Health to 0.
    auto offscreen = render::OffscreenRenderer::create(context, width, height);
    CHECK(offscreen.has_value());
    GameUi::Options options;
    options.contentDir = contentDir;
    auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
    CHECK(ui.has_value());
    (*ui)->core().setMenuOpen(true);
    (*ui)->update(0.0, {640, 480});
    (*ui)->handle(lunar::platform::KeyEvent{.key = lunar::platform::Key::R, .pressed = true});
    (*ui)->update(0.0, {640, 480});
    CHECK(humanoid->health() > 0.0f);
    (*ui)->handle(lunar::platform::KeyEvent{.key = lunar::platform::Key::Enter, .pressed = true});
    CHECK_EQ(humanoid->health(), 0.0f);
    CHECK(!(*ui)->menuOpen());
}

GPU_TEST(ui_draws_blurred_shadows)
{
    // A white shadow quad: its box (40 wide, inside a rect 20 bigger) solid
    // in the middle, fading out across the 20 pixel blur at its edge.
    Fixture f(context);
    GlyphAtlas atlas;
    DrawList list;
    Quad shadow;
    shadow.rect = {50, 30, 60, 60};
    shadow.kind = QuadKind::Shadow;
    shadow.uv = {20, 0, 0, 0};
    shadow.color = {1, 1, 1, 1};
    shadow.clip = {0, 0, width, height};
    list.push(shadow);
    const render::PixelImage image = f.draw(list, atlas);
    CHECK(channelDiff(image.at(80, 60), 255, 255, 255) <= 1); // the middle
    const int edge = image.at(60, 60).r;                      // on the box's edge: half
    CHECK(edge > 100 && edge < 220);
    CHECK(image.at(52, 60).r < edge); // fading out
    CHECK(channelDiff(image.at(45, 60), 0, 0, 0) <= 1);
}

GPU_TEST(ui_draws_the_loading_screen_over_everything)
{
    const auto contentDir = lunar::assets::AssetConfig::fromEnvironment().contentDir;
    if (!std::filesystem::exists(contentDir / "fonts/BuilderSans-Regular.otf"))
        SKIP("no Roblox content folder with fonts");
    auto game = lunar::DataModel::create();
    for (const auto& [w, h, scale, name] :
         {std::tuple{1280u, 720u, 1.0f, "ui_loading.png"}, std::tuple{3840u, 2160u, 2.0f, "ui_loading_4k.png"}}) {
        auto offscreen = render::OffscreenRenderer::create(context, w, h);
        CHECK(offscreen.has_value());
        GameUi::Options options;
        options.contentDir = contentDir;
        options.map.minimap = false;
        auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
        CHECK(ui.has_value());
        (*ui)->attach((*offscreen)->renderer());
        LoadingScreen& loading = (*ui)->loading();
        loading.show();
        loading.setGame("A Very Good Game", "builderman", "");
        loading.setStatus("Loading the game");
        const lunar::Vector2 size{static_cast<float>(w), static_cast<float>(h)};
        (*ui)->update(0.0, size, scale);
        CHECK((*ui)->loadingScreenUp());
        // The menu stays shut under it.
        CHECK((*ui)->handle(lunar::platform::KeyEvent{.key = lunar::platform::Key::Escape, .pressed = true}));
        CHECK(!(*ui)->menuOpen());
        render::Lighting lighting;
        lighting.sky = {0.8f, 0.8f, 0.8f}; // the game's bright sky, which mustn't show
        const render::PixelImage image = (*offscreen)->render({}, {}, lighting);
        if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP"))
            std::ofstream(std::filesystem::path(dir) / name, std::ios::binary) << render::encodePng(image);
        const auto bg = LoadingScreen::background;
        const auto channel = [](float c) { return static_cast<int>(std::lround(c * 255.0f)); };
        CHECK(channelDiff(image.at(4, h - 4), channel(bg.r), channel(bg.g), channel(bg.b)) <= 2);
        CHECK(channelDiff(image.at(w / 4, h / 2), channel(bg.r), channel(bg.g), channel(bg.b)) <= 2);
        // The icon's placeholder in the middle, a little lighter.
        const render::Rgba icon = image.at(w / 2, static_cast<std::uint32_t>(static_cast<float>(h) * 0.45f) - 60);
        CHECK(icon.r > channel(bg.r) + 10);
        (*ui)->detach();
    }
}

// A ViewportFrame's parts through its camera, on its rect in the GUI: what's
// behind the parts shows through, what's over the frame covers it, and the
// picture is drawn again only for what changes it. LUNAR_UI_TEST_DUMP saves it.
GPU_TEST(ui_draws_a_viewport_frame)
{
    auto game = lunar::DataModel::create();
    auto* starter = game->getService("StarterGui");
    auto screen = std::make_shared<lunar::ScreenGui>();
    screen->setIgnoreGuiInset(true);
    screen->setParent(starter);
    auto backdrop = std::make_shared<lunar::Frame>();
    backdrop->setBackgroundColor3({0, 0, 1});
    backdrop->setBorderSizePixel(0);
    backdrop->setSize(lunar::UDim2::fromScale(1, 1));
    backdrop->setParent(screen);
    auto viewport = std::make_shared<lunar::ViewportFrame>();
    viewport->setBackgroundTransparency(1);
    viewport->setBorderSizePixel(0);
    viewport->setPosition(lunar::UDim2::fromOffset(20, 10));
    viewport->setSize(lunar::UDim2::fromOffset(100, 100));
    viewport->setZIndex(2);
    viewport->setParent(screen);
    auto cover = std::make_shared<lunar::Frame>();
    cover->setBackgroundColor3({1, 1, 0});
    cover->setBorderSizePixel(0);
    cover->setPosition(lunar::UDim2::fromOffset(60, 50));
    cover->setSize(lunar::UDim2::fromOffset(10, 10));
    cover->setZIndex(3);
    cover->setParent(screen);

    // The camera a child, the part in a WorldModel, far from the origin: the
    // picture has its own space, as a cloned model in an inventory does.
    auto camera = std::make_shared<lunar::Camera>();
    camera->setCFrame(render::Camera::lookAt({500, 0, 10}, {500, 0, 0}));
    camera->setFieldOfView(40);
    camera->setParent(viewport);
    viewport->setCurrentCamera(camera.get());
    auto world = std::make_shared<lunar::WorldModel>();
    world->setParent(viewport);
    auto part = std::make_shared<lunar::Part>();
    part->setSize({3, 3, 1});
    part->setCFrame(lunar::CFrame(500, 0, 0));
    part->setColor({1, 0, 0});
    part->setParent(world);
    viewport->setAmbient({1, 1, 1});
    viewport->setLightColor({0, 0, 0});

    auto offscreen = render::OffscreenRenderer::create(context, width, height);
    CHECK(offscreen.has_value());
    GameUi::Options options;
    options.showChat = false;
    options.showCoreGui = false;
    options.map.minimap = false;
    auto ui = GameUi::create(context, render::OffscreenRenderer::colorFormat, 1, *game, options);
    CHECK(ui.has_value());
    (*ui)->attach((*offscreen)->renderer());
    ViewportFrames& pictures = *(*ui)->viewports();
    const lunar::Vector2 size{width, height};
    auto frame = [&](const char* name = nullptr) {
        (*ui)->update(0.0, size);
        render::PixelImage image = (*offscreen)->render({}, {}, {});
        if (const char* dir = std::getenv("LUNAR_UI_TEST_DUMP"); dir && name)
            std::ofstream(std::filesystem::path(dir) / name, std::ios::binary) << render::encodePng(image);
        return image;
    };

    render::PixelImage image = frame("ui_viewport.png");
    CHECK(pictures.drawn() == 1);
    const render::Rgba middle = image.at(50, 60);
    CHECK(middle.r > 150 && middle.g < 40 && middle.b < 40);
    CHECK(channelDiff(image.at(24, 14), 0, 0, 255) <= 2);    // past the part: what's under the frame
    CHECK(channelDiff(image.at(140, 60), 0, 0, 255) <= 2);   // past the frame
    CHECK(channelDiff(image.at(65, 55), 255, 255, 0) <= 2);  // what's in front of it

    // Nothing changed, or nothing the picture shows: it stays as it is.
    frame();
    viewport->setImageColor3({0, 1, 1});
    image = frame();
    CHECK(pictures.drawn() == 1);
    CHECK(image.at(50, 60).r < 10); // tinted as an image, not drawn again
    viewport->setImageColor3({1, 1, 1});
    viewport->setPosition(lunar::UDim2::fromOffset(30, 10));
    image = frame();
    CHECK(pictures.drawn() == 1);
    CHECK(image.at(60, 60).r > 150);
    viewport->setPosition(lunar::UDim2::fromOffset(20, 10));

    // A part or the camera moved, a part added, and the light: drawn again.
    part->setColor({0, 1, 0});
    image = frame();
    CHECK(pictures.drawn() == 2);
    CHECK(image.at(50, 60).g > 150);
    camera->setCFrame(render::Camera::lookAt({503.5f, 0, 10}, {503.5f, 0, 0}));
    image = frame();
    CHECK(pictures.drawn() == 3);
    CHECK(channelDiff(image.at(50, 60), 0, 0, 255) <= 2);
    camera->setCFrame(render::Camera::lookAt({500, 0, 10}, {500, 0, 0}));
    frame();
    auto second = std::make_shared<lunar::Part>();
    second->setSize({1, 1, 1});
    second->setCFrame(lunar::CFrame(500, 3, 0));
    second->setColor({1, 1, 1});
    second->setParent(world);
    image = frame();
    CHECK(pictures.drawn() == 5);
    CHECK(image.at(70, 22).r > 150);
    second->setParent(nullptr);
    viewport->setAmbient({0, 0, 0});
    image = frame();
    CHECK(pictures.drawn() == 6);
    CHECK(image.at(50, 60).g < 30); // no light, and LightColor is black

    // Lit from the camera's side by LightDirection, which says where light goes.
    viewport->setLightColor({1, 1, 1});
    viewport->setLightDirection({0, 0, -1});
    image = frame();
    CHECK(image.at(50, 60).g > 150);
    viewport->setLightDirection({0, 0, 1});
    image = frame();
    CHECK(image.at(50, 60).g < 30);
    viewport->setAmbient({1, 1, 1});
    viewport->setLightColor({0, 0, 0});

    // See-through parts blend with what's under the frame, not with black.
    part->setTransparency(0.5f);
    image = frame("ui_viewport_transparent.png");
    const render::Rgba blended = image.at(50, 60);
    CHECK(blended.g > 100 && blended.b > 100);
    part->setTransparency(0.0f);
    frame();

    // Off the screen, hidden or fully faded: nothing is drawn, whatever changes.
    const std::uint64_t before = pictures.drawn();
    viewport->setPosition(lunar::UDim2::fromOffset(1000, 10));
    part->setColor({1, 0, 0});
    frame();
    CHECK(pictures.drawn() == before);
    viewport->setPosition(lunar::UDim2::fromOffset(20, 10));
    viewport->setVisible(false);
    frame();
    CHECK(pictures.drawn() == before);
    viewport->setVisible(true);
    viewport->setImageTransparency(1);
    image = frame();
    CHECK(pictures.drawn() == before);
    CHECK(channelDiff(image.at(50, 60), 0, 0, 255) <= 2);
    viewport->setImageTransparency(0);
    image = frame();
    CHECK(pictures.drawn() == before + 1);
    CHECK(image.at(50, 60).r > 150);

    // Without a camera it shows only its background.
    viewport->setCurrentCamera(nullptr);
    image = frame();
    CHECK(channelDiff(image.at(50, 60), 0, 0, 255) <= 2);
    (*ui)->detach();
}
