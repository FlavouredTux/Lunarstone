#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/ClassicChat.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/Gui.hpp>
#include <lunar/ui/TextEditor.hpp>

#include <cmath>
#include <memory>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::ui;

namespace {

void requireClientContent()
{
    if (assets::AssetConfig::fromEnvironment().contentDir.empty())
        SKIP("set LUNAR_CONTENT_DIR to run font-dependent tests");
}

bool near(float a, float b, float tolerance = 0.01f)
{
    return std::abs(a - b) <= tolerance;
}

template <class T>
std::shared_ptr<T> make(Instance* parent, std::string name = {})
{
    auto object = std::make_shared<T>();
    if (!name.empty())
        object->setName(std::move(name));
    object->setParent(parent);
    return object;
}

// A StarterGui with one ScreenGui, laid out on an 800x600 screen with no inset.
struct Screen {
    std::shared_ptr<DataModel> game = DataModel::create();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    GuiEngine gui{fonts, atlas};
    std::shared_ptr<ScreenGui> screen;

    Screen()
    {
        auto* starter = game->getService<StarterGui>();
        screen = make<ScreenGui>(starter);
        gui.setRoots({starter});
        gui.settings().topInset = 0.0f;
    }
    void layout() { gui.update({800, 600}, 0.0); }
    RectF rect(const GuiObject& object) const
    {
        auto r = gui.rectOf(&object);
        if (!r)
            throw lt::Failure{"object wasn't laid out"};
        return *r;
    }
};

} // namespace

// A text stroke is the glyph grown by the radius: each pixel the strongest
// coverage within the round reach, as a plain scan of that reach finds it.
TEST(ui_glyph_stroke_is_the_strongest_coverage_within_the_reach)
{
    requireClientContent();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    const FontFace* face = fonts.face(Enum::Font::SourceSans);
    CHECK(face != nullptr);
    if (!face)
        return;
    GlyphAtlas atlas;
    atlas.beginFrame();
    int compared = 0;
    for (const char32_t ch : {U'W', U'g', U'8', U'@', U'i'}) {
        const std::uint16_t glyph = face->glyphIndex(ch);
        for (const int radius : {1, 2, 5, 8}) {
            const float ppu = 70.0f / static_cast<float>(face->unitsPerEm());
            const GlyphAtlas::Entry* plain = atlas.get(face, glyph, ppu, 0, 0);
            const GlyphAtlas::Entry* grown = atlas.get(face, glyph, ppu, radius, 0);
            CHECK(plain != nullptr && grown != nullptr);
            if (!plain || !grown)
                continue;
            const GlyphAtlas::Entry a = *plain, g = *grown;
            CHECK_EQ(g.width, a.width + 2 * radius);
            CHECK_EQ(g.height, a.height + 2 * radius);
            const auto pixels = atlas.pixels();
            auto at = [&](const GlyphAtlas::Entry& e, int x, int y) -> int {
                if (x < 0 || y < 0 || x >= e.width || y >= e.height)
                    return 0;
                return pixels[static_cast<std::size_t>(e.y + y) * GlyphAtlas::size + static_cast<std::size_t>(e.x + x)];
            };
            int wrong = 0;
            for (int y = 0; y < g.height; ++y)
                for (int x = 0; x < g.width; ++x) {
                    int best = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx)
                            if (dx * dx + dy * dy <= radius * radius + radius)
                                best = std::max(best, at(a, x - radius + dx, y - radius + dy));
                    wrong += best != at(g, x, y);
                }
            CHECK_EQ(wrong, 0);
            ++compared;
        }
    }
    CHECK_EQ(compared, 20);
}

// A frame that asks for more glyphs than fit leaves the rest out instead of
// starting over under the ones it already drew; the next frame packs the
// atlas again with only what the last one drew, which makes room.
TEST(ui_glyph_atlas_never_starts_over_mid_frame)
{
    requireClientContent();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    const FontFace* face = fonts.face(Enum::Font::SourceSans);
    CHECK(face != nullptr);
    if (!face)
        return;
    const std::uint16_t glyph = face->glyphIndex(U'W');
    auto ppu = [&](int em) { return static_cast<float>(em) / static_cast<float>(face->unitsPerEm()); };
    GlyphAtlas atlas;
    atlas.beginFrame();
    const GlyphAtlas::Entry* first = atlas.get(face, glyph, ppu(300));
    CHECK(first != nullptr);
    const GlyphAtlas::Entry kept = *first;
    // A billboard crowd: the same W at a new size every time, until one doesn't fit.
    int em = 300;
    while (atlas.get(face, glyph, ppu(++em)) && em < 2000) {
    }
    CHECK(em < 2000);
    CHECK_EQ(atlas.generation(), 0u);
    CHECK(first->x == kept.x && first->y == kept.y); // what this frame drew stays put
    // The next frame draws two of them: the atlas is still full of the last frame's.
    atlas.beginFrame();
    CHECK(atlas.get(face, glyph, ppu(300)) != nullptr);
    CHECK(atlas.get(face, glyph, ppu(em)) == nullptr);
    // The one after keeps only those and the left out one fits.
    atlas.beginFrame();
    CHECK(atlas.generation() > 0u);
    CHECK(atlas.get(face, glyph, ppu(em)) != nullptr);
    const GlyphAtlas::Entry* again = atlas.get(face, glyph, ppu(300));
    CHECK(again != nullptr);
    CHECK(again->width == kept.width && again->height == kept.height);
}

TEST(layout_udim2_math)
{
    // Size: scale of the parent plus offset; position likewise minus AnchorPoint * size.
    const RectF parent{100, 50, 400, 200};
    const Placement p = place({{0.5f, 10}, {0.0f, 20}}, {{0.25f, 0}, {0.5f, -10}}, {0.5f, 1.0f}, parent);
    CHECK(near(p.rect.w, 100));
    CHECK(near(p.rect.h, 90));
    CHECK(near(p.rect.x, 100 + 200 + 10 - 50));
    CHECK(near(p.rect.y, 50 + 20 - 90));

    // SizeConstraint: RelativeXX takes both scales from the parent's width.
    const Vector2 xx = resolveSize({{0.5f, 0}, {0.5f, 0}}, {400, 200}, Enum::SizeConstraint::RelativeXX);
    CHECK(near(xx.x, 200));
    CHECK(near(xx.y, 200));
    const Vector2 yy = resolveSize({{0.5f, 0}, {0.5f, 0}}, {400, 200}, Enum::SizeConstraint::RelativeYY);
    CHECK(near(yy.x, 100));

    CHECK(
        near(applyAspectRatio({400, 100}, 2.0f, Enum::AspectType::FitWithinMaxSize, Enum::DominantAxis::Width).x, 200));
    CHECK(near(applyAspectRatio({400, 100}, 2.0f, Enum::AspectType::ScaleWithParentSize, Enum::DominantAxis::Width).y,
               200));
}

TEST(layout_nested_frames_and_padding)
{
    Screen s;
    auto outer = make<Frame>(s.screen.get());
    outer->setPosition({{0.5f, 0}, {0.5f, 0}});
    outer->setAnchorPoint({0.5f, 0.5f});
    outer->setSize(UDim2::fromOffset(400, 300));
    auto padding = make<UIPadding>(outer.get());
    padding->setPaddingLeft({0.0f, 10});
    padding->setPaddingTop({0.1f, 0}); // 10% of the height
    auto inner = make<Frame>(outer.get());
    inner->setSize(UDim2::fromScale(0.5f, 0.5f));
    s.layout();

    CHECK(s.rect(*outer) == (RectF{200, 150, 400, 300}));
    // Padding shrinks the content area the scales resolve against.
    const RectF r = s.rect(*inner);
    CHECK(near(r.x, 210));
    CHECK(near(r.y, 180));
    CHECK(near(r.w, 195));
    CHECK(near(r.h, 135));
    // AbsolutePosition/AbsoluteSize are written back.
    CHECK(near(inner->absolutePosition().x, 210));
    CHECK(near(inner->absoluteSize().y, 135));
}

// A negative Size reaches back from where it starts: drawn flipped, its
// AbsoluteSize negative (what Studio reports). The game's cooldown overlays.
TEST(layout_negative_size_reaches_back)
{
    Screen s;
    auto slot = make<Frame>(s.screen.get());
    slot->setSize(UDim2::fromOffset(60, 60));
    slot->setPosition(UDim2::fromOffset(100, 100));
    auto cooldown = make<Frame>(slot.get());
    cooldown->setAnchorPoint({0.5f, 0.0f});
    cooldown->setPosition(UDim2::fromScale(0.5f, 1.0f));
    cooldown->setSize(UDim2::fromScale(1.0f, -0.5f));
    s.layout();
    CHECK(s.rect(*cooldown) == (RectF{100, 130, 60, 30}));
    CHECK(near(cooldown->absolutePosition().y, 160));
    CHECK(near(cooldown->absoluteSize().y, -30));
}

TEST(layout_ignores_invisible_and_respects_inset)
{
    Screen s;
    s.gui.settings().topInset = 36.0f;
    auto frame = make<Frame>(s.screen.get());
    frame->setSize(UDim2::fromOffset(10, 10));
    auto hidden = make<Frame>(s.screen.get());
    hidden->setVisible(false);
    s.layout();
    // Drawn under the top bar, but AbsolutePosition counts from below it.
    CHECK(near(s.rect(*frame).y, 36));
    CHECK(near(frame->absolutePosition().y, 0));
    CHECK(!s.gui.rectOf(hidden.get()));
    s.screen->setIgnoreGuiInset(true);
    s.layout();
    CHECK(near(s.rect(*frame).y, 0));
    CHECK(near(frame->absolutePosition().y, -36));
}

TEST(layout_list_layout)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(200, 400));
    auto list = make<UIListLayout>(holder.get());
    CHECK(list->fillDirection() == Enum::FillDirection::Vertical); // Roblox's default for lists
    list->setPadding({0.0f, 5});
    list->setHorizontalAlignment(Enum::HorizontalAlignment::Center);
    std::vector<std::shared_ptr<Frame>> items;
    for (int i = 0; i < 3; ++i) {
        auto item = make<Frame>(holder.get());
        item->setSize(UDim2::fromOffset(100, 20 + i * 10));
        item->setLayoutOrder(2 - i);                    // reverse order
        item->setPosition(UDim2::fromOffset(999, 999)); // ignored under a layout
        items.push_back(item);
    }
    s.layout();
    // Sorted by LayoutOrder: items[2] first.
    CHECK(near(s.rect(*items[2]).y, 0));
    CHECK(near(s.rect(*items[1]).y, 40 + 5));
    CHECK(near(s.rect(*items[0]).y, 40 + 5 + 30 + 5));
    CHECK(near(s.rect(*items[0]).x, 50)); // centred in 200

    list->setFillDirection(Enum::FillDirection::Horizontal);
    list->setVerticalAlignment(Enum::VerticalAlignment::Bottom);
    list->setHorizontalAlignment(Enum::HorizontalAlignment::Left);
    s.layout();
    CHECK(near(s.rect(*items[2]).x, 0));
    CHECK(near(s.rect(*items[1]).x, 105));
    CHECK(near(s.rect(*items[2]).bottom(), 400));
}

TEST(layout_grid_layout)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(230, 400));
    auto grid = make<UIGridLayout>(holder.get());
    grid->setCellSize(UDim2::fromOffset(50, 40));
    grid->setCellPadding(UDim2::fromOffset(10, 5));
    std::vector<std::shared_ptr<Frame>> cells;
    for (int i = 0; i < 5; ++i) {
        auto cell = make<Frame>(holder.get());
        cell->setLayoutOrder(i);
        cells.push_back(cell);
    }
    s.layout();
    // (230 + 10) / (50 + 10) = 4 per row.
    CHECK(s.rect(*cells[0]) == (RectF{0, 0, 50, 40}));
    CHECK(near(s.rect(*cells[3]).x, 180));
    CHECK(near(s.rect(*cells[4]).x, 0));
    CHECK(near(s.rect(*cells[4]).y, 45));

    grid->setFillDirectionMaxCells(2);
    grid->setStartCorner(Enum::StartCorner::TopRight);
    s.layout();
    CHECK(near(s.rect(*cells[0]).x, 60));
    CHECK(near(s.rect(*cells[1]).x, 0));
}

TEST(layout_grid_reports_cells_and_reserves_constrained_spans)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(230, 200));
    auto grid = make<UIGridLayout>(holder.get());
    grid->setCellSize(UDim2::fromOffset(50, 40));
    grid->setCellPadding(UDim2::fromOffset(10, 5));
    auto wide = make<Frame>(holder.get());
    auto constraint = make<UISizeConstraint>(wide.get());
    constraint->setMinSize({110, 85}); // Two columns and two rows.
    std::vector<std::shared_ptr<Frame>> cells;
    for (int i = 0; i < 5; ++i) {
        auto cell = make<Frame>(holder.get());
        cell->setLayoutOrder(i + 1);
        cells.push_back(cell);
    }
    auto scale = make<UIScale>(cells[0].get());
    scale->setScale(0.5f); // Individual scale does not change the occupied cells.
    s.layout();
    CHECK_EQ(grid->absoluteCellSize(), (Vector2{50, 40}));
    CHECK_EQ(grid->absoluteCellCount(), (Vector2{4, 3}));
    CHECK(s.rect(*wide) == (RectF{0, 0, 110, 85}));
    CHECK(s.rect(*cells[0]) == (RectF{120, 0, 25, 20}));
    CHECK(s.rect(*cells[2]) == (RectF{120, 45, 50, 40}));
    CHECK(s.rect(*cells[4]) == (RectF{0, 90, 50, 40}));
    s.gui.settings().pixelScale = 2;
    s.gui.update({1600, 1200}, 0);
    CHECK_EQ(grid->absoluteCellSize(), (Vector2{50, 40}));
    CHECK_EQ(grid->absoluteCellCount(), (Vector2{4, 3}));
    wide->destroy();
    for (const auto& cell : cells)
        cell->destroy();
    s.layout();
    CHECK_EQ(grid->absoluteCellCount(), Vector2{});
    CHECK_EQ(grid->absoluteContentSize(), Vector2{});
}

TEST(gui_inactive_button_keeps_mouse_events_without_activated)
{
    Screen s;
    auto button = make<TextButton>(s.screen.get());
    button->setSize(UDim2::fromOffset(100, 50));
    CHECK(button->active());
    button->setActive(false);
    int clicks = 0, activated = 0, inputs = 0;
    button->mouseButton1Click.connect([&] { ++clicks; });
    button->eventFired.connect([&](std::string_view name, std::span<const Value>) {
        activated += name == "Activated";
        inputs += name == "InputBegan" || name == "InputEnded";
    });
    s.layout();
    s.gui.handle(platform::MouseButtonEvent{.pressed = true, .x = 10, .y = 10});
    s.gui.handle(platform::MouseButtonEvent{.pressed = false, .x = 10, .y = 10});
    CHECK_EQ(clicks, 1);
    CHECK_EQ(activated, 0);
    CHECK_EQ(inputs, 2);
}

TEST(gui_page_wheel_obeys_configuration_and_nested_scrolling_frames)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(200, 100));
    holder->setClipsDescendants(true);
    auto pages = make<UIPageLayout>(holder.get());
    pages->setAnimated(false);
    auto first = make<Frame>(holder.get());
    first->setSize(UDim2::fromScale(1, 1));
    auto second = make<Frame>(holder.get());
    second->setLayoutOrder(1);
    second->setSize(UDim2::fromScale(1, 1));
    s.layout();
    CHECK(pages->currentPage() == first.get());
    s.gui.handle(platform::MouseMoveEvent{20, 20});
    pages->setScrollWheelInputEnabled(false);
    CHECK(!s.gui.handle(platform::ScrollEvent{.dy = -1}));
    CHECK(pages->currentPage() == first.get());
    pages->setScrollWheelInputEnabled(true);
    CHECK(s.gui.handle(platform::ScrollEvent{.dy = -1}));
    CHECK(pages->currentPage() == second.get());
    s.layout();
    auto scrolling = make<ScrollingFrame>(second.get());
    scrolling->setSize(UDim2::fromScale(1, 1));
    scrolling->setCanvasSize(UDim2::fromOffset(200, 300));
    s.layout();
    CHECK(s.gui.handle(platform::ScrollEvent{.dy = -1}));
    CHECK_EQ(scrolling->canvasPosition(), (Vector2{0, 40}));
    CHECK(pages->currentPage() == second.get());
    scrolling->setScrollingEnabled(false);
    pages->setCircular(true);
    CHECK(s.gui.handle(platform::ScrollEvent{.dy = -1}));
    CHECK(pages->currentPage() == first.get());
}

// A UIPageLayout's pages in a row, the current one where the alignments put
// it (centred by default), the others beside it; part of the way back from
// the last one while they slide.
TEST(layout_page_layout)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(200, 100));
    auto layout = make<UIPageLayout>(holder.get());
    layout->setPadding({0.0f, 10});
    std::vector<std::shared_ptr<Frame>> pages;
    for (int i = 0; i < 3; ++i) {
        auto page = make<Frame>(holder.get());
        page->setSize(UDim2::fromOffset(100, 50));
        page->setLayoutOrder(i);
        pages.push_back(page);
    }
    s.layout();
    CHECK(layout->currentPage() == pages[0].get());
    CHECK(s.rect(*pages[0]) == (RectF{50, 25, 100, 50}));
    CHECK(near(s.rect(*pages[1]).x, 160));
    CHECK(near(layout->absoluteContentSize().x, 320));
    CHECK(near(layout->absoluteContentSize().y, 50));

    layout->assignCurrentPage(pages[2].get());
    s.layout();
    CHECK(near(s.rect(*pages[2]).x, 50));
    CHECK(near(s.rect(*pages[0]).x, 50 - 220));
    // Halfway from the first.
    layout->assignSlide(pages[0].get(), 0.5f);
    s.layout();
    CHECK(near(s.rect(*pages[0]).x, 50 - 110));
}

// GuiService.TopbarInset: the top bar but the health bar's corner, as wide
// as the screen and as tall as the bar.
TEST(layout_topbar_inset)
{
    Screen s;
    s.gui.settings().topInset = 36.0f;
    const auto* service = dynamic_cast<GuiService*>(s.game->getService("GuiService"));
    s.layout();
    CHECK(service != nullptr);
    if (service)
        CHECK(service->topbarInset() == (Rect{{0.0f, 0.0f}, {640.0f, 36.0f}}));
}

// GuiService:SetGlobalGuiInset (the Lua app's navigation rail) narrows the
// ScreenGuis that keep the GUI inset, under the top bar; IgnoreGuiInset ones
// keep the whole screen.
TEST(layout_global_gui_inset)
{
    Screen s;
    s.gui.settings().topInset = 36.0f;
    auto* service = dynamic_cast<GuiService*>(s.game->getService("GuiService"));
    CHECK(service != nullptr);
    if (!service)
        return;
    service->assignGlobalGuiInset({64.0f, 10.0f, 6.0f, 20.0f});
    auto frame = make<Frame>(s.screen.get());
    frame->setSize(UDim2::fromScale(1.0f, 1.0f));
    auto rail = make<Frame>(s.screen.get());
    rail->setPosition(UDim2::fromOffset(-64, 0));
    rail->setSize(UDim2{{0.0f, 64}, {1.0f, 0}});
    s.layout();
    CHECK(s.rect(*frame) == (RectF{64, 46, 730, 534}));
    CHECK(s.rect(*rail) == (RectF{0, 46, 64, 534}));

    s.screen->setIgnoreGuiInset(true);
    s.layout();
    CHECK(s.rect(*frame) == (RectF{0, 0, 800, 600}));
}

// A row that grows to what its UIListLayout stacks, also when the layout
// aligns them to its bottom: the children sit inside it, not above.
TEST(layout_automatic_size_from_a_bottom_aligned_row)
{
    Screen s;
    auto row = make<Frame>(s.screen.get());
    row->setPosition(UDim2::fromOffset(0, 100));
    row->setSize(UDim2{{1.0f, 0}, {0.0f, 0}});
    row->setAutomaticSize(Enum::AutomaticSize::Y);
    auto list = make<UIListLayout>(row.get());
    list->setFillDirection(Enum::FillDirection::Horizontal);
    list->setVerticalAlignment(Enum::VerticalAlignment::Bottom);
    list->setPadding({0.0f, 10});
    auto icon = make<Frame>(row.get());
    icon->setSize(UDim2::fromOffset(200, 200));
    icon->setLayoutOrder(1);
    auto title = make<Frame>(row.get());
    title->setSize(UDim2::fromOffset(300, 50));
    title->setLayoutOrder(2);
    s.layout();
    CHECK(s.rect(*row) == (RectF{0, 100, 800, 200}));
    CHECK(s.rect(*icon) == (RectF{0, 100, 200, 200}));
    CHECK(s.rect(*title) == (RectF{210, 250, 300, 50}));
}

TEST(layout_constraints_and_scale)
{
    Screen s;
    auto frame = make<Frame>(s.screen.get());
    frame->setSize(UDim2::fromScale(1.0f, 0.5f)); // 800 x 300
    auto aspect = make<UIAspectRatioConstraint>(frame.get());
    aspect->setAspectRatio(2.0f);
    s.layout();
    CHECK(near(s.rect(*frame).w, 600));
    CHECK(near(s.rect(*frame).h, 300));
    aspect->destroy();

    auto limits = make<UISizeConstraint>(frame.get());
    limits->setMaxSize({500, 1e9f});
    s.layout();
    CHECK(near(s.rect(*frame).w, 500));
    limits->destroy();

    // UIScale scales the object and its children's offsets.
    frame->setSize(UDim2::fromOffset(100, 100));
    auto child = make<Frame>(frame.get());
    child->setSize(UDim2::fromOffset(10, 10));
    child->setPosition(UDim2::fromOffset(10, 0));
    auto scale = make<UIScale>(frame.get());
    scale->setScale(2.0f);
    s.layout();
    CHECK(near(s.rect(*frame).w, 200));
    CHECK(near(s.rect(*child).w, 20));
    CHECK(near(s.rect(*child).x, 20));
}

TEST(layout_scrolling_frame_canvas)
{
    Screen s;
    auto scroll = make<ScrollingFrame>(s.screen.get());
    scroll->setSize(UDim2::fromOffset(100, 100));
    scroll->setCanvasSize(UDim2::fromOffset(0, 500));
    scroll->setCanvasPosition({0, 50});
    auto child = make<Frame>(scroll.get());
    child->setSize(UDim2::fromOffset(10, 10));
    child->setPosition(UDim2::fromOffset(0, 60));
    s.layout();
    CHECK(near(s.rect(*child).y, 10)); // 60 down the canvas, scrolled by 50
    CHECK(near(scroll->absoluteCanvasSize().y, 500));
    // CanvasPosition is clamped to the canvas.
    scroll->setCanvasPosition({0, 1000});
    s.layout();
    CHECK(near(scroll->canvasPosition().y, 400));
    // Clicks and the wheel: scrolling moves the canvas.
    s.gui.handle(platform::MouseMoveEvent{50, 50});
    CHECK(s.gui.handle(platform::ScrollEvent{0, 1}));
    CHECK(scroll->canvasPosition().y < 400);
}

// The lobby lays transparent frames over the whole screen (a fade, the
// settings), above the crate shop: the wheel still scrolls the shop, as the
// frames take no input. A button or an active frame over it does take the wheel.
TEST(layout_wheel_goes_through_transparent_frames_laid_over_a_scrolling_frame)
{
    Screen s;
    auto scroll = make<ScrollingFrame>(s.screen.get());
    scroll->setSize(UDim2::fromOffset(100, 100));
    scroll->setCanvasSize(UDim2::fromOffset(0, 500));
    auto item = make<Frame>(scroll.get());
    item->setSize(UDim2::fromOffset(80, 80));
    auto fade = make<Frame>(s.screen.get());
    fade->setSize(UDim2::fromScale(1, 1));
    fade->setBackgroundTransparency(1.0f);
    fade->setZIndex(100);
    s.layout();
    s.gui.handle(platform::MouseMoveEvent{50, 50});
    CHECK(s.gui.handle(platform::ScrollEvent{0, -1}));
    CHECK(scroll->canvasPosition().y > 0);

    // The same wheel over an active frame that's on top goes nowhere.
    const float scrolled = scroll->canvasPosition().y;
    fade->setActive(true);
    s.layout();
    CHECK(!s.gui.handle(platform::ScrollEvent{0, -1}));
    CHECK(near(scroll->canvasPosition().y, scrolled));
}

TEST(layout_zindex_and_hit_testing)
{
    Screen s;
    auto low = make<TextButton>(s.screen.get());
    low->setSize(UDim2::fromOffset(100, 100));
    low->setZIndex(5);
    auto high = make<TextButton>(s.screen.get());
    high->setSize(UDim2::fromOffset(100, 100));
    high->setZIndex(1);
    s.layout();
    // Higher ZIndex is on top even though it comes first.
    CHECK(s.gui.objectAt(50, 50) == low.get());

    int clicks = 0;
    low->mouseButton1Click.connect([&] { ++clicks; });
    CHECK(s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 50, 50, {}}));
    CHECK(s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, false, 50, 50, {}}));
    CHECK_EQ(clicks, 1);
    // Releasing elsewhere isn't a click.
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 50, 50, {}});
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, false, 500, 500, {}});
    CHECK_EQ(clicks, 1);
    CHECK(!s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 500, 500, {}}));
}

TEST(layout_order_does_not_reorder_equal_zindex_siblings_or_hit_targets)
{
    for (const auto mode : {Enum::ZIndexBehavior::Global, Enum::ZIndexBehavior::Sibling}) {
        for (int kind = 0; kind < 3; ++kind) {
            Screen s;
            s.screen->setZIndexBehavior(mode);
            auto holder = make<Frame>(s.screen.get());
            holder->setSize(UDim2::fromOffset(200, 100));
            auto first = make<Frame>(holder.get());
            first->setSize(UDim2::fromOffset(100, 100));
            first->setLayoutOrder(1);
            auto overhang = make<Frame>(first.get());
            overhang->setPosition(UDim2::fromOffset(-10, 0));
            overhang->setSize(UDim2::fromOffset(20, 50));
            overhang->setActive(true);
            auto second = make<Frame>(holder.get());
            second->setSize(UDim2::fromOffset(100, 100));
            second->setLayoutOrder(0);
            second->setActive(true);
            if (kind == 0) {
                auto layout = make<UIListLayout>(holder.get());
                layout->setFillDirection(Enum::FillDirection::Horizontal);
            } else if (kind == 1) {
                auto layout = make<UIGridLayout>(holder.get());
                layout->setCellSize(UDim2::fromOffset(100, 100));
                layout->setCellPadding({});
            } else {
                auto layout = make<UIPageLayout>(holder.get());
                layout->setHorizontalAlignment(Enum::HorizontalAlignment::Left);
                layout->setVerticalAlignment(Enum::VerticalAlignment::Top);
            }
            s.layout();
            CHECK_EQ(first->absolutePosition(), (Vector2{100, 0}));
            CHECK_EQ(second->absolutePosition(), Vector2{});
            // The first sibling's overhang is painted before the second
            // sibling, even though the layout positions it on the right.
            CHECK(s.gui.objectAt(95, 10) == second.get());
            const auto stack = s.gui.objectsAt(95, 10);
            CHECK(stack.size() >= 2);
            CHECK(stack[0] == second);
            CHECK(stack[1] == overhang);
        }
    }
}

TEST(layout_legacy_styles_inset_children_at_small_sizes_and_high_dpi)
{
    Screen s;
    auto button = make<TextButton>(s.screen.get());
    button->setStyle(Enum::ButtonStyle::RobloxRoundDefaultButton);
    button->setSize(UDim2::fromOffset(100, 40));
    auto child = make<Frame>(button.get());
    child->setSize(UDim2::fromScale(1, 1));
    s.layout();
    CHECK_EQ(child->absolutePosition(), (Vector2{12, 12}));
    CHECK_EQ(child->absoluteSize(), (Vector2{76, 16}));
    button->setSize(UDim2::fromOffset(100, 13));
    s.layout();
    CHECK_EQ(child->absolutePosition(), (Vector2{6, 6}));
    CHECK_EQ(child->absoluteSize(), (Vector2{88, 1}));
    auto padding = make<UIPadding>(button.get());
    padding->setPaddingLeft({0, 3});
    s.gui.settings().pixelScale = 2;
    s.gui.update({1600, 1200}, 0);
    CHECK_EQ(child->absolutePosition(), (Vector2{9, 6}));
    CHECK_EQ(child->absoluteSize(), (Vector2{85, 1}));
    button->setStyle(Enum::ButtonStyle::Custom);
    s.layout();
    CHECK_EQ(child->absolutePosition(), (Vector2{3, 0}));
    CHECK_EQ(child->absoluteSize(), (Vector2{97, 13}));

    auto frame = make<Frame>(s.screen.get());
    frame->setSize(UDim2::fromOffset(100, 100));
    frame->setStyle(Enum::FrameStyle::ChatBlue);
    child->setParent(frame);
    s.layout();
    CHECK_EQ(child->absolutePosition(), (Vector2{17, 17}));
    CHECK_EQ(child->absoluteSize(), (Vector2{66, 66}));
    frame->setStyle(Enum::FrameStyle::RobloxRound);
    s.layout();
    CHECK_EQ(child->absolutePosition(), (Vector2{8, 8}));
    CHECK_EQ(child->absoluteSize(), (Vector2{84, 84}));
    auto scale = make<UIScale>(frame.get());
    scale->setScale(0);
    s.layout();
    CHECK_EQ(child->absolutePosition(), Vector2{});
    CHECK_EQ(child->absoluteSize(), Vector2{});
}

// The game colours its enemy names and health bars with a
// UIGradient over white, and its HUD has dozens of them: each object takes
// one ramp however many glyphs its text has, objects with the same gradient
// share it, and every quad keeps its gradient (past the renderer's rows a
// quad would draw white).
TEST(gui_gradients_share_one_ramp_per_object_and_per_colour)
{
    requireClientContent();
    Screen s;
    const ColorSequence red({{0.0f, {0.83f, 0.24f, 0.24f}}, {1.0f, {0.88f, 0.47f, 0.47f}}});
    const ColorSequence green({{0.0f, {0.36f, 0.72f, 0.33f}}, {1.0f, {0.26f, 0.93f, 0.25f}}});
    for (int i = 0; i < 40; ++i) {
        auto name = make<TextLabel>(s.screen.get());
        name->setSize(UDim2::fromOffset(200, 30));
        name->setPosition(UDim2::fromOffset(0, i * 10));
        name->setText("Sample Enemy");
        name->setBackgroundTransparency(1);
        make<UIGradient>(name.get())->setColor(red);
        auto bar = make<Frame>(s.screen.get());
        bar->setSize(UDim2::fromOffset(200, 10));
        bar->setBorderSizePixel(0);
        make<UIGradient>(bar.get())->setColor(green);
    }
    s.layout();
    DrawList list;
    s.gui.draw(list);
    CHECK_EQ(list.gradients.size(), 2u);
    std::size_t glyphs = 0;
    for (const Quad& q : list.quads) {
        CHECK(q.gradient == 1 || q.gradient == 2);
        glyphs += q.kind == QuadKind::Glyph;
    }
    CHECK(glyphs > 64);
}

TEST(gui_legacy_button_styles_use_builtin_art_and_selection_states)
{
    Screen s;
    auto image = std::make_shared<Image>();
    image->width = image->height = 20;
    image->rgba.resize(20 * 20 * 4);
    std::vector<std::string> requested;
    s.gui.setImageProvider([&](std::string_view id) {
        requested.emplace_back(id);
        return image;
    });
    auto button = make<TextButton>(s.screen.get());
    button->setSize(UDim2::fromOffset(120, 40));
    button->setStyle(Enum::ButtonStyle::RobloxRoundDefaultButton);
    button->setBackgroundColor3({1, 0, 0});
    button->setBackgroundTransparency(1);
    const auto draw = [&] {
        s.layout();
        requested.clear();
        DrawList list;
        s.gui.draw(list);
        CHECK_EQ(list.quads.size(), 9u);
        CHECK(std::ranges::all_of(
            list.quads, [](const Quad& q) { return q.kind == QuadKind::Image && q.color == Color4{1, 1, 1, 1}; }));
        CHECK_EQ(requested.size(), 1u);
        return requested.front();
    };
    CHECK_EQ(draw(), "rbxasset://textures/ui/btn_newBlue.png");
    s.gui.handle(platform::MouseMoveEvent{20, 20});
    CHECK_EQ(draw(), "rbxasset://textures/ui/btn_newBlueGlow.png");
    button->setAutoButtonColor(false);
    CHECK_EQ(draw(), "rbxasset://textures/ui/btn_newBlue.png");
    button->setAutoButtonColor(true);
    s.gui.handle(platform::MouseMoveEvent{200, 200});
    button->setSelected(true);
    CHECK_EQ(draw(), "rbxasset://textures/ui/btn_newBlueGlow.png");
    button->setActive(false);
    CHECK_EQ(draw(), "rbxasset://textures/ui/btn_newGrey.png");
    button->setActive(true);
    button->setSelected(false);
    for (const auto& [style, file] : std::vector<std::pair<Enum::ButtonStyle, std::string>>{
             {Enum::ButtonStyle::RobloxButtonDefault, "btn_red.png"},
             {Enum::ButtonStyle::RobloxButton, "btn_grey.png"},
             {Enum::ButtonStyle::RobloxRoundButton, "btn_newGrey.png"},
             {Enum::ButtonStyle::RobloxRoundDropdownButton, "btn_newWhite.png"}}) {
        button->setStyle(style);
        CHECK_EQ(draw(), "rbxasset://textures/ui/" + file);
    }
}

TEST(gui_custom_button_colors_match_native_hover_and_press_blends)
{
    Screen s;
    auto button = make<TextButton>(s.screen.get());
    button->setSize(UDim2::fromOffset(100, 50));
    button->setBorderSizePixel(0);
    button->setBackgroundColor3({0.2f, 0.4f, 0.6f});
    const auto color = [&] {
        s.layout();
        DrawList list;
        s.gui.draw(list);
        CHECK_EQ(list.quads.size(), 1u);
        return list.quads[0].color;
    };
    s.gui.handle(platform::MouseMoveEvent{10, 10});
    // Geometry is established by the first draw.
    color();
    s.gui.handle(platform::MouseMoveEvent{10, 10});
    auto tint = color();
    CHECK(near(tint.r, 0.14f) && near(tint.g, 0.28f) && near(tint.b, 0.42f));
    s.gui.handle(platform::MouseButtonEvent{.pressed = true, .x = 10, .y = 10});
    tint = color();
    CHECK(near(tint.r, 0.44f) && near(tint.g, 0.58f) && near(tint.b, 0.72f));
    button->setAutoButtonColor(false);
    tint = color();
    CHECK(near(tint.r, 0.2f) && near(tint.g, 0.4f) && near(tint.b, 0.6f));
}

TEST(gui_core_modal_release_cancels_the_held_button_without_activating_it)
{
    Screen s;
    auto button = make<TextButton>(s.screen.get());
    button->setSize(UDim2::fromOffset(100, 50));
    int downs = 0, ups = 0, clicks = 0;
    button->mouseButton1Down.connect([&] { ++downs; });
    button->mouseButton1Up.connect([&] { ++ups; });
    button->activated.connect([&] { ++clicks; });
    s.layout();
    s.gui.handle(platform::MouseButtonEvent{.pressed = true, .x = 10, .y = 10});
    // The native prompt consumes this release before GuiEngine::handle.
    s.gui.finishInputEvent(platform::MouseButtonEvent{.pressed = false, .x = 10, .y = 10});
    CHECK_EQ(downs, 1);
    CHECK_EQ(ups, 1);
    CHECK_EQ(clicks, 0);
}

TEST(layout_nested_automatic_size_does_not_change_script_properties)
{
    Screen s;
    Instance* parent = s.screen.get();
    std::vector<std::shared_ptr<Frame>> frames;
    int changes = 0;
    for (int i = 0; i < 12; ++i) {
        auto frame = make<Frame>(parent);
        frame->setSize(UDim2::fromScale(1, 0));
        frame->setAutomaticSize(Enum::AutomaticSize::Y);
        frame->changed.connect([&](std::string_view property) {
            if (property == "AutomaticSize")
                ++changes;
        });
        parent = frame.get();
        frames.push_back(std::move(frame));
    }
    auto leaf = make<Frame>(parent);
    leaf->setSize(UDim2::fromOffset(100, 24));
    s.layout();
    for (const auto& frame : frames) {
        CHECK_EQ(frame->absoluteSize().y, 24.0f);
        CHECK_EQ(frame->automaticSize(), Enum::AutomaticSize::Y);
    }
    CHECK_EQ(changes, 0);
}

// The game's sidebar: a frame with AutomaticSize XY around one
// that holds a UIGridLayout of buttons, anchored at its bottom right. The
// frame grows once it knows its content, which lays the subtree out again;
// scripts watching AbsolutePosition (the game's Fusion state follows a
// button's) hear where things end up, once a layout, and nothing at all
// when the next layout puts them in the same place.
TEST(layout_announces_where_things_end_up_once)
{
    Screen s;
    auto side = make<Frame>(s.screen.get(), "Sidebar");
    side->setSize(UDim2::fromScale(0.1f, 0));
    side->setPosition(UDim2::fromScale(0.969f, 0.7f));
    side->setAnchorPoint({1, 1});
    side->setAutomaticSize(Enum::AutomaticSize::XY);
    auto holder = make<Frame>(side.get(), "Holder");
    holder->setSize(UDim2::fromScale(1, 0));
    holder->setAutomaticSize(Enum::AutomaticSize::XY);
    auto grid = make<UIGridLayout>(holder.get());
    grid->setCellSize(UDim2::fromOffset(68, 68));
    grid->setCellPadding(UDim2::fromOffset(8, 8));
    grid->setFillDirectionMaxCells(2);
    std::vector<std::shared_ptr<Frame>> buttons;
    for (int i = 0; i < 9; ++i)
        buttons.push_back(make<Frame>(holder.get()));
    std::map<std::string, int> changes;
    for (GuiObject* object : {static_cast<GuiObject*>(side.get()), static_cast<GuiObject*>(holder.get()),
                              static_cast<GuiObject*>(buttons[4].get())})
        object->changed.connect([&changes, object](std::string_view property) {
            if (property.starts_with("Absolute"))
                ++changes[object->name() + "." + std::string(property)];
        });
    s.layout();
    for (const auto& [what, count] : changes)
        if (count > 1)
            throw lt::Failure{std::format("{} changed {} times in one layout", what, count)};
    CHECK_EQ(holder->absoluteSize().x, 144.0f);
    CHECK_EQ(side->absoluteSize().y, 372.0f);
    changes.clear();
    s.layout();
    CHECK(changes.empty());
}

TEST(layout_nested_grids_reuse_measured_automatic_sizes)
{
    Screen s;
    Instance* parent = s.screen.get();
    std::vector<std::shared_ptr<Frame>> frames;
    int changes = 0;
    for (int i = 0; i < 8; ++i) {
        auto frame = make<Frame>(parent);
        frame->setSize(UDim2::fromScale(1, 0));
        frame->setAutomaticSize(Enum::AutomaticSize::Y);
        frame->changed.connect([&](std::string_view property) {
            if (property == "AbsoluteSize")
                ++changes;
        });
        auto grid = make<UIGridLayout>(frame.get());
        grid->setCellSize(UDim2{{1, 0}, {0, 1}});
        grid->setCellPadding(UDim2{});
        parent = frame.get();
        frames.push_back(std::move(frame));
    }
    auto leaf = make<Frame>(parent);
    leaf->setAutomaticSize(Enum::AutomaticSize::Y);
    auto content = make<Frame>(leaf.get());
    content->setSize(UDim2::fromOffset(100, 24));
    s.layout();
    for (const auto& frame : frames)
        CHECK_EQ(frame->absoluteSize().y, 24.0f);
    CHECK(changes <= 24);
}

TEST(layout_list_repositions_after_a_grid_child_grows)
{
    Screen s;
    auto container = make<Frame>(s.screen.get());
    container->setSize(UDim2::fromOffset(200, 200));
    auto list = make<UIListLayout>(container.get());
    list->setSortOrder(Enum::SortOrder::LayoutOrder);
    auto row = make<Frame>(container.get());
    row->setSize(UDim2::fromScale(1, 0));
    row->setAutomaticSize(Enum::AutomaticSize::Y);
    row->setLayoutOrder(1);
    auto grid = make<UIGridLayout>(row.get());
    grid->setCellSize(UDim2::fromOffset(100, 20));
    grid->setCellPadding(UDim2{});
    grid->setFillDirectionMaxCells(1);
    make<Frame>(row.get());
    make<Frame>(row.get());
    auto footer = make<TextButton>(container.get());
    footer->setSize(UDim2::fromOffset(100, 20));
    footer->setLayoutOrder(2);
    s.layout();
    CHECK_EQ(row->absoluteSize().y, 40.0f);
    CHECK_EQ(footer->absolutePosition().y, 40.0f);
}

TEST(layout_folders_preserve_container_geometry_and_input)
{
    Screen s;
    auto parent = make<Frame>(s.screen.get());
    parent->setPosition(UDim2::fromOffset(20, 30));
    parent->setSize(UDim2::fromOffset(200, 100));
    auto provider = make<Folder>(parent.get(), "ProviderContainer");
    auto page = make<Folder>(provider.get(), "PageWrapper");
    auto button = make<TextButton>(page.get());
    button->setPosition(UDim2::fromScale(0.25f, 0.25f));
    button->setSize(UDim2::fromScale(0.5f, 0.5f));
    s.layout();
    CHECK_EQ(button->absolutePosition(), (Vector2{70, 55}));
    CHECK_EQ(button->absoluteSize(), (Vector2{100, 50}));
    int clicks = 0;
    button->mouseButton1Click.connect([&] { ++clicks; });
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 80, 65, {}});
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, false, 80, 65, {}});
    CHECK_EQ(clicks, 1);
    parent->setVisible(false);
    s.layout();
    CHECK(s.gui.objectAt(80, 65) == nullptr);
}

// A layout arranges its siblings only: what a Folder holds keeps its
// Position and takes no room in the row (Myriad's Cascade window: eight
// resize handles in a Folder pushed the content pane 3476 px to the right).
TEST(layout_list_leaves_folder_children_where_they_sit)
{
    Screen s;
    auto body = make<Frame>(s.screen.get());
    body->setSize(UDim2::fromOffset(300, 100));
    auto list = make<UIListLayout>(body.get());
    list->setFillDirection(Enum::FillDirection::Horizontal);
    auto sidebar = make<Frame>(body.get());
    sidebar->setSize(UDim2::fromOffset(100, 100));
    sidebar->setLayoutOrder(0);
    auto handles = make<Folder>(body.get(), "Handles");
    auto handle = make<Frame>(handles.get());
    handle->setSize(UDim2::fromOffset(500, 10));
    handle->setPosition(UDim2::fromOffset(5, 80));
    auto pane = make<Frame>(body.get());
    pane->setSize(UDim2::fromOffset(200, 100));
    pane->setLayoutOrder(1);
    s.layout();
    CHECK(near(s.rect(*pane).x, 100));
    CHECK(near(s.rect(*handle).x, 5));
    CHECK(near(s.rect(*handle).y, 80));
    CHECK(near(list->absoluteContentSize().x, 300));

    // Nor does it add to what an automatic size stacks.
    auto column = make<Frame>(s.screen.get());
    column->setSize(UDim2::fromOffset(100, 0));
    column->setAutomaticSize(Enum::AutomaticSize::Y);
    auto rows = make<UIListLayout>(column.get());
    for (int i = 0; i < 2; ++i) {
        auto row = make<Frame>(column.get());
        row->setSize(UDim2::fromOffset(100, 20));
    }
    auto kept = make<Folder>(column.get(), "LayoutIgnore");
    auto shadow = make<Frame>(kept.get());
    shadow->setSize(UDim2::fromOffset(5, 30));
    s.layout();
    CHECK(near(column->absoluteSize().y, 40));
}

// A text object's AutomaticSize grows it to its children as well as its
// text (Myriad's Cascade buttons: an empty TextButton around their label).
TEST(layout_automatic_size_text_object_grows_to_its_children)
{
    requireClientContent();
    Screen s;
    auto button = make<TextButton>(s.screen.get());
    button->setSize(UDim2::fromOffset(0, 0));
    button->setText("");
    button->setAutomaticSize(Enum::AutomaticSize::XY);
    auto label = make<TextLabel>(button.get());
    label->setSize(UDim2::fromScale(1, 1));
    label->setAutomaticSize(Enum::AutomaticSize::XY);
    label->setText("Configure");
    s.layout();
    CHECK(label->absoluteSize().x > 20.0f);
    CHECK(near(button->absoluteSize().x, label->absoluteSize().x));
    CHECK(near(button->absoluteSize().y, label->absoluteSize().y));
}

TEST(layout_textbox_focus_and_typing)
{
    Screen s;
    auto box = make<TextBox>(s.screen.get());
    box->setSize(UDim2::fromOffset(200, 30));
    box->setText("old");
    s.layout();
    bool lost = false;
    bool enter = false;
    box->focusLost.connect([&](bool e) {
        lost = true;
        enter = e;
    });
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 10, 10, {}});
    CHECK(s.gui.textBoxFocused());
    CHECK_EQ(box->text(), std::string()); // ClearTextOnFocus
    s.gui.handle(platform::TextEvent{"hi there"});
    s.gui.handle(platform::KeyEvent{.key = platform::Key::Backspace, .pressed = true});
    CHECK_EQ(box->text(), std::string("hi ther"));
    s.gui.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK(!s.gui.textBoxFocused());
    CHECK(lost);
    CHECK(enter);
}

// The GUI's layout is kept from one update to the next while nothing in it
// changes, but focusing an empty box takes its placeholder away, though no
// property changes.
TEST(layout_focusing_an_empty_textbox_hides_its_placeholder)
{
    requireClientContent();
    Screen s;
    auto box = make<TextBox>(s.screen.get());
    box->setSize(UDim2::fromOffset(200, 30));
    box->setPlaceholderText("Search");
    s.layout();
    auto glyphs = [&] {
        DrawList list;
        s.gui.draw(list);
        std::size_t n = 0;
        for (const Quad& q : list.quads)
            n += q.kind == QuadKind::Glyph;
        return n;
    };
    CHECK(glyphs() >= 6u);
    s.gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, 10, 10, {}});
    CHECK(s.gui.textBoxFocused());
    s.layout();
    CHECK_EQ(glyphs(), 0u);
}

// Each ScreenGui's layout is kept apart: a change in one lays out that one
// again, and a frame moved from one into another is laid out where it went.
TEST(layout_screen_guis_are_kept_apart)
{
    Screen s;
    auto other = make<ScreenGui>(s.game->getService<StarterGui>());
    other->setIgnoreGuiInset(true);
    auto still = make<Frame>(s.screen.get());
    still->setSize(UDim2::fromOffset(50, 50));
    still->setPosition(UDim2::fromOffset(10, 10));
    auto moving = make<Frame>(other.get());
    moving->setSize(UDim2::fromOffset(20, 20));
    s.layout();
    CHECK(near(s.rect(*still).x, 10.0f));
    moving->setPosition(UDim2::fromOffset(100, 0));
    s.layout();
    CHECK(near(s.rect(*moving).x, 100.0f));
    CHECK(near(s.rect(*still).x, 10.0f));
    // Into the first, inside the frame that stays.
    moving->setParent(still.get());
    s.layout();
    CHECK(near(s.rect(*moving).x, 110.0f));
    CHECK(near(moving->absolutePosition().x, 110.0f));
    // And a property of the moved frame counts for the one it's in now.
    moving->setPosition(UDim2::fromOffset(5, 0));
    s.layout();
    CHECK(near(s.rect(*moving).x, 15.0f));
    other->setEnabled(false);
    still->setVisible(false);
    s.layout();
    CHECK(!s.gui.rectOf(still.get()));
}

// Auto-sized wrapped text grows only as wide as its container and wraps
// there (the game's item descriptions, in a wrapping list).
TEST(layout_auto_sized_wrapped_text_wraps_at_its_container)
{
    requireClientContent();
    Screen s;
    auto panel = make<Frame>(s.screen.get());
    panel->setSize(UDim2::fromOffset(200, 300));
    auto list = make<UIListLayout>(panel.get());
    list->setFillDirection(Enum::FillDirection::Horizontal);
    list->setWraps(true);
    auto label = make<TextLabel>(panel.get());
    label->setAutomaticSize(Enum::AutomaticSize::XY);
    label->setSize(UDim2::fromOffset(0, 0));
    label->setTextWrapped(true);
    label->setTextSize(20);
    label->setText("Wrapped text should fit within the width of the container and continue on another line");
    auto plain = make<TextLabel>(s.screen.get());
    plain->setAutomaticSize(Enum::AutomaticSize::X);
    plain->setSize(UDim2::fromOffset(0, 20));
    plain->setPosition(UDim2::fromOffset(0, 400));
    plain->setTextSize(20);
    plain->setText(label->text());
    s.layout();
    CHECK(s.rect(*label).w <= 200.5f);
    CHECK(s.rect(*label).h > 40.0f); // more than one line
    CHECK(s.rect(*plain).w > 300.0f); // unwrapped text still grows to it all
}

// The wheel over a list that scrolls is the list's: the camera doesn't zoom.
TEST(layout_wheel_over_a_scrolling_frame_is_taken)
{
    Screen s;
    s.screen->setIgnoreGuiInset(true);
    auto scroll = make<ScrollingFrame>(s.screen.get());
    scroll->setSize(UDim2::fromOffset(200, 200));
    scroll->setCanvasSize(UDim2::fromOffset(200, 600));
    auto empty = make<ScrollingFrame>(s.screen.get());
    empty->setPosition(UDim2::fromOffset(300, 0));
    empty->setSize(UDim2::fromOffset(200, 200));
    empty->setCanvasSize(UDim2::fromOffset(200, 100));
    s.layout();
    s.gui.handle(platform::MouseMoveEvent{50, 50});
    CHECK(s.gui.wheelTaken());
    // At its end it still is.
    scroll->setCanvasPosition({0, 400});
    s.layout();
    CHECK(s.gui.wheelTaken());
    s.gui.handle(platform::MouseMoveEvent{350, 50}); // nothing to scroll there
    CHECK(!s.gui.wheelTaken());
    s.gui.handle(platform::MouseMoveEvent{700, 500});
    CHECK(!s.gui.wheelTaken());
}

TEST(layout_text_editor)
{
    TextEditor e;
    e.insert("héllo");
    CHECK_EQ(e.length(), std::size_t{5});
    e.key({.key = platform::Key::Left, .pressed = true});
    e.key({.key = platform::Key::Left, .pressed = true});
    e.key({.key = platform::Key::Left, .pressed = true});
    e.key({.key = platform::Key::Backspace, .pressed = true}); // removes the two-byte e-acute
    CHECK_EQ(e.text, std::string("hllo"));
    e.maxLength = 5;
    e.insert("abc\n");
    CHECK_EQ(e.text, std::string("hallo"));
    CHECK(e.key({.key = platform::Key::Enter, .pressed = true}) == TextEditor::Result::Submitted);
}

TEST(layout_text_editor_selection_and_clipboard)
{
    using platform::Key;
    std::string board;
    setClipboard(
        {.read = [&board] { return board; }, .write = [&board](std::string text) { board = std::move(text); }});
    const platform::Modifiers ctrl{.ctrl = true};
    const platform::Modifiers shift{.shift = true};

    TextEditor e;
    e.insert("hello world");
    // Shift+Home selects back to the start; typing replaces the selection.
    e.key({.key = Key::Left, .pressed = true});
    e.key({.key = Key::Left, .pressed = true});
    e.key({.key = Key::Left, .pressed = true});
    e.key({.key = Key::Left, .pressed = true});
    e.key({.key = Key::Left, .pressed = true});
    e.key({.key = Key::Home, .pressed = true, .modifiers = shift});
    CHECK(e.hasSelection());
    e.key({.key = Key::C, .pressed = true, .modifiers = ctrl});
    CHECK_EQ(board, std::string("hello "));
    e.insert("bye ");
    CHECK_EQ(e.text, std::string("bye world"));
    CHECK(!e.hasSelection());

    // A plain arrow collapses a selection to its edge.
    e.key({.key = Key::Right, .pressed = true, .modifiers = shift});
    e.key({.key = Key::Right, .pressed = true, .modifiers = shift});
    e.key({.key = Key::Left, .pressed = true});
    CHECK_EQ(e.caret, std::size_t{4});
    CHECK(!e.hasSelection());

    // Ctrl+A then Ctrl+X cuts everything; Ctrl+V pastes it back twice.
    e.key({.key = Key::A, .pressed = true, .modifiers = ctrl});
    e.key({.key = Key::X, .pressed = true, .modifiers = ctrl});
    CHECK_EQ(board, std::string("bye world"));
    CHECK(e.text.empty());
    e.key({.key = Key::V, .pressed = true, .modifiers = ctrl});
    e.key({.key = Key::V, .pressed = true, .modifiers = ctrl});
    CHECK_EQ(e.text, std::string("bye worldbye world"));

    // Ctrl+C without a selection copies the whole line; Backspace deletes a selection.
    board.clear();
    e.key({.key = Key::C, .pressed = true, .modifiers = ctrl});
    CHECK_EQ(board, std::string("bye worldbye world"));
    e.key({.key = Key::End, .pressed = true});
    e.key({.key = Key::Left, .pressed = true, .modifiers = shift});
    e.key({.key = Key::Left, .pressed = true, .modifiers = shift});
    e.key({.key = Key::Backspace, .pressed = true});
    CHECK_EQ(e.text, std::string("bye worldbye wor"));

    // Pasted newlines are dropped on a single line, and the length cap counts what the selection frees.
    e.setText("abc");
    e.maxLength = 4;
    e.key({.key = Key::A, .pressed = true, .modifiers = ctrl});
    board = "x\ny\nz";
    e.key({.key = Key::V, .pressed = true, .modifiers = ctrl});
    CHECK_EQ(e.text, std::string("xyz"));
    setClipboard({});
}

TEST(layout_text_editor_lines)
{
    using platform::Key;
    TextEditor e;
    e.multiLine = true;
    e.insert("one\ntwo\nthree");
    const std::vector<std::string_view> lines = e.lines();
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(std::string(lines[1]), std::string("two"));
    CHECK_EQ(e.lineCount(), std::size_t{3});
    CHECK_EQ(e.caretLine(), std::size_t{2});
    CHECK_EQ(e.lineEnd(), e.text.size());
    CHECK_EQ(e.lineStart(), e.text.rfind('\n') + 1);

    // Up and Down walk the lines, Home and End stay on the caret's.
    e.key({.key = Key::Home, .pressed = true});
    CHECK_EQ(e.caret, e.lineStart());
    e.key({.key = Key::Up, .pressed = true});
    CHECK_EQ(e.caretLine(), std::size_t{1});
    CHECK_EQ(e.caret, e.lineStart());
    e.key({.key = Key::Down, .pressed = true});
    CHECK_EQ(e.caretLine(), std::size_t{2});
    e.key({.key = Key::Up, .pressed = true});
    e.key({.key = Key::Up, .pressed = true});
    CHECK_EQ(e.caret, std::size_t{0}); // nowhere above the first line
    e.key({.key = Key::End, .pressed = true});
    CHECK_EQ(e.caret, e.lineEnd());
    CHECK_EQ(e.caret, std::size_t{3}); // the end of "one", not of the text

    // Enter is a new line; with Ctrl it submits instead.
    e.key({.key = Key::Enter, .pressed = true});
    CHECK_EQ(e.text, std::string("one\n\ntwo\nthree"));
    CHECK_EQ(e.caretLine(), std::size_t{1});
    CHECK(e.key({.key = Key::Enter, .pressed = true, .modifiers = {.ctrl = true}}) == TextEditor::Result::Submitted);

    // A line below a longer one: the caret goes as far as it goes.
    TextEditor shortLine;
    shortLine.multiLine = true;
    shortLine.insert("abcdef\nxy");
    shortLine.key({.key = Key::Home, .pressed = true});
    shortLine.key({.key = Key::End, .pressed = true});
    shortLine.key({.key = Key::Up, .pressed = true});
    CHECK_EQ(shortLine.caret, std::size_t{2});
    shortLine.key({.key = Key::Down, .pressed = true});
    CHECK_EQ(shortLine.caretLine(), std::size_t{1});

    // A single-line editor has no lines to move between, and its arrows are
    // nobody's business.
    TextEditor single;
    single.insert("x\ny");
    CHECK_EQ(single.text, std::string("xy"));
    CHECK(single.key({.key = Key::Up, .pressed = true}) == TextEditor::Result::None);
    CHECK(single.key({.key = Key::Home, .pressed = true}) == TextEditor::Result::Edited);
    CHECK_EQ(single.caret, std::size_t{0});
}

// Pinned against an independent transcription of the Lua chat's
// GetNameValue/ComputeNameColor.
TEST(chat_legacy_name_colors)
{
    CHECK(legacyNameColor("Shedletsky") == BrickColor::fromName("Light reddish violet").color()); // 30 -> 7
    CHECK(legacyNameColor("builderman") == BrickColor::fromName("Bright violet").color());        // 35 -> 4
    CHECK(legacyNameColor("Telamon") == BrickColor::fromName("Bright orange").color());           // 140 -> 5
    CHECK(legacyNameColor("Roblox") == BrickColor::fromName("Bright orange").color());            // 28 -> 5
    CHECK(legacyNameColor("Player1") == Color3::fromRGB(2, 184, 87));                             // 66 -> 3
    CHECK(legacyNameColor("a") == Color3::fromRGB(1, 162, 255));                                  // 97 -> 2
    CHECK(legacyNameColor("") == Color3::fromRGB(253, 41, 67));                                   // 0 -> 1
    // Negative sums wrap the way Lua's floored % does.
    CHECK(legacyNameColor("\xff\xff\x01") == legacyNameColor("\xff\xff\x01"));
}

TEST(chat_echo_and_input)
{
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    ClassicChat chat(fonts, atlas);
    LocalEchoChatSource echo("builderman", "Builder");
    chat.setSource(&echo);
    chat.update(0.0, {1280, 720}, 36.0f, 1.0f);

    // Window geometry from ChatSettings: 30% wide, 25% (of the area under the top bar) + 24 tall.
    CHECK(near(chat.windowRect().y, 36));
    CHECK(near(chat.windowRect().w, 384));
    CHECK(near(chat.windowRect().h, std::round(684 * 0.25f + 24)));
    CHECK(near(chat.barRect().h, 18 + 24));

    CHECK(!chat.focused());
    // "/" opens the bar by the character it types, whatever key that is
    // (Shift+7 on Swiss keyboards), and isn't typed itself.
    CHECK(!chat.handle(platform::TextEvent{"7"}));
    CHECK(!chat.focused());
    CHECK(chat.handle(platform::TextEvent{"/"}));
    CHECK(chat.focused());
    chat.handle(platform::TextEvent{"hello"});
    chat.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK(!chat.focused());
    CHECK_EQ(chat.messages().size(), std::size_t{1});
    CHECK_EQ(chat.messages().back().displayName, std::string("Builder"));
    CHECK_EQ(chat.messages().back().fromName, std::string("builderman"));
    CHECK_EQ(chat.messages().back().text, std::string("hello"));

    // Blank messages aren't sent; history keeps the last 50.
    chat.focus();
    chat.handle(platform::TextEvent{"   "});
    chat.handle(platform::KeyEvent{.key = platform::Key::Enter, .pressed = true});
    CHECK_EQ(chat.messages().size(), std::size_t{1});
    for (int i = 0; i < 60; ++i)
        chat.postSystem("line");
    CHECK_EQ(chat.messages().size(), ClassicChat::historyLength);
}

TEST(chat_health_color)
{
    CHECK(healthColor(1.0f) == ui::CoreGui::healthGreen);
    CHECK(healthColor(0.05f) == ui::CoreGui::healthRed);
    CHECK(healthColor(0.5f) == ui::CoreGui::healthYellow);
    // Between samples: a blend leaning towards the nearest.
    const Color3 c = healthColor(0.3f);
    CHECK(c.r > 0.9f);
    CHECK(c.g > 0.3f);
    CHECK(c.g < 0.95f);
}

// --- BillboardGuis -----------------------------------------------------------------

namespace {

// A part 20 studs straight ahead of a camera at the origin (looking down -Z,
// 70 degrees of vertical view) with a BillboardGui on it, for an 800x600 screen.
struct Billboarded {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Part> part = make<Part>(game->workspace(), "Head");
    std::shared_ptr<BillboardGui> gui = make<BillboardGui>(part.get(), "Overhead");
    Camera& camera = *game->workspace()->ensureCurrentCamera();
    static constexpr Vector2 screen{800.0f, 600.0f};

    Billboarded()
    {
        part->setCFrame(CFrame(0.0f, 0.0f, -20.0f));
        camera.setCFrame(CFrame());
        camera.setFieldOfView(70.0f);
        gui->setSize(UDim2{{0.0f, 100}, {0.0f, 50}});
    }
    std::optional<GuiEngine::BillboardPlacement> place(const Player* local = nullptr) const
    {
        return GuiEngine::placeBillboard(*gui, camera, screen, 1.0f, local);
    }
};

} // namespace

// A ScreenGui kept in a Folder of the roots shows, as on Roblox (gethui()
// is one); one inside another GUI doesn't.
TEST(ui_screen_guis_in_folders_show)
{
    Screen s;
    auto* starter = s.game->getService<StarterGui>();
    auto folder = make<Folder>(starter, "Kept");
    auto inFolder = make<ScreenGui>(folder.get());
    auto shown = make<Frame>(inFolder.get());
    shown->setSize(UDim2{{0, 100}, {0, 50}});
    auto nested = make<ScreenGui>(s.screen.get());
    auto hidden = make<Frame>(nested.get());
    hidden->setSize(UDim2{{0, 100}, {0, 50}});
    s.layout();
    CHECK(s.gui.rectOf(shown.get()).has_value());
    CHECK(near(s.rect(*shown).w, 100));
    CHECK(!s.gui.rectOf(hidden.get()).has_value());
}

TEST(ui_billboard_placement)
{
    Billboarded b;
    auto at = b.place();
    CHECK(at.has_value());
    CHECK(near(at->rect.x, 350.0f) && near(at->rect.y, 275.0f) && near(at->rect.w, 100.0f) && near(at->rect.h, 50.0f));
    CHECK(near(at->depth, 20.0f));

    // Scale is in studs: two studs wide at 20 studs away.
    const float pixelsPerStud = 600.0f / (2.0f * 20.0f * std::tan(35.0f * 3.14159265f / 180.0f));
    b.gui->setSize(UDim2{{2.0f, 0}, {1.0f, 10}});
    at = b.place();
    CHECK(near(at->rect.w, 2.0f * pixelsPerStud, 0.05f) && near(at->rect.h, pixelsPerStud + 10.0f, 0.05f));

    // StudsOffset is along the camera: two studs up lifts it.
    b.gui->setSize(UDim2{{0.0f, 100}, {0.0f, 50}});
    b.gui->setStudsOffset(Vector3{0.0f, 2.0f, 0.0f});
    at = b.place();
    CHECK(near(at->rect.y + 25.0f, 300.0f - 2.0f * pixelsPerStud, 0.05f));
    b.gui->setStudsOffset(Vector3{});

    // Gone: past MaxDistance, behind the camera, disabled, hidden from us.
    b.gui->setMaxDistance(10.0f);
    CHECK(!b.place());
    b.gui->setMaxDistance(100.0f);
    b.part->setCFrame(CFrame(0.0f, 0.0f, 20.0f));
    CHECK(!b.place());
    b.part->setCFrame(CFrame(0.0f, 0.0f, -20.0f));
    b.gui->setEnabled(false);
    CHECK(!b.place());
    b.gui->setEnabled(true);
    auto player = make<Player>(b.game->getService("Players"), "Me");
    b.gui->setPlayerToHideFrom(player.get());
    CHECK(!b.place(player.get()));
    CHECK(b.place().has_value());
}

TEST(ui_billboard_lays_out_its_children)
{
    Billboarded b;
    auto label = make<TextLabel>(b.gui.get(), "FPS");
    label->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    GuiEngine gui{fonts, atlas};
    gui.settings().topInset = 0.0f;
    gui.setWorld(b.game->workspace());
    gui.update(Billboarded::screen, 0.0);
    auto rect = gui.rectOf(label.get());
    CHECK(rect.has_value() && near(rect->x, 350.0f) && near(rect->y, 275.0f) && near(rect->w, 100.0f));
    CHECK(near(b.gui->absoluteSize().x, 100.0f));
    // One added later shows too.
    auto other = make<Part>(b.game->workspace(), "Other");
    other->setCFrame(CFrame(5.0f, 0.0f, -20.0f));
    auto second = make<BillboardGui>(other.get(), "Second");
    second->setSize(UDim2{{0.0f, 40}, {0.0f, 40}});
    auto frame = make<Frame>(second.get());
    frame->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    gui.update(Billboarded::screen, 0.0);
    CHECK(gui.rectOf(frame.get()).has_value());
}

TEST(ui_surface_gui_placement)
{
    auto game = DataModel::create();
    auto part = make<Part>(game->workspace(), "Sign");
    part->setSize(Vector3{8.0f, 4.0f, 1.0f});
    part->setCFrame(CFrame(0.0f, 2.0f, 0.0f));
    auto gui = make<SurfaceGui>(part.get(), "Board");
    gui->setCanvasSize(Vector2{400.0f, 200.0f});
    // Front: the -Z face, read from in front, canvas (0, 0) at its top left.
    auto at = GuiEngine::placeSurface(*gui);
    CHECK(at.has_value());
    CHECK(near(at->origin.x, 4.0f) && near(at->origin.y, 4.0f) && near(at->origin.z, -0.51f));
    CHECK(near(at->axisX.x, -8.0f / 400.0f) && near(at->axisY.y, -4.0f / 200.0f));
    CHECK(near(at->normal.z, -1.0f));
    // The canvas's far corner is the face's bottom right.
    const Vector3 corner = at->origin + at->axisX * 400.0f + at->axisY * 200.0f;
    CHECK(near(corner.x, -4.0f) && near(corner.y, 0.0f));
    // PixelsPerStud sizes the canvas by the face.
    gui->setSizingMode(Enum::SurfaceGuiSizingMode::PixelsPerStud);
    gui->setPixelsPerStud(25.0f);
    gui->setFace(Enum::NormalId::Top);
    at = GuiEngine::placeSurface(*gui);
    CHECK(near(at->canvas.x, 200.0f) && near(at->canvas.y, 25.0f) && near(at->normal.y, 1.0f));
    gui->setEnabled(false);
    CHECK(!GuiEngine::placeSurface(*gui));
}

TEST(ui_world_guis_draw_into_layers)
{
    Billboarded b;
    make<TextLabel>(b.gui.get(), "Name")->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    b.gui->setAlwaysOnTop(true);
    b.gui->setLightInfluence(0.0f);
    // A sign facing the camera and one facing away (not drawn).
    auto sign = make<Part>(b.game->workspace(), "Sign");
    sign->setSize(Vector3{4.0f, 2.0f, 1.0f});
    sign->setCFrame(CFrame(0.0f, 0.0f, -10.0f));
    auto front = make<SurfaceGui>(sign.get(), "Toward");
    front->setFace(Enum::NormalId::Back); // +Z, towards the camera at the origin
    make<Frame>(front.get())->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    auto away = make<SurfaceGui>(sign.get(), "Away");
    away->setFace(Enum::NormalId::Front);
    make<Frame>(away.get())->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});

    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    GuiEngine gui{fonts, atlas};
    gui.setWorld(b.game->workspace());
    gui.update(Billboarded::screen, 0.0);
    DrawList list;
    gui.draw(list);
    CHECK_EQ(list.layers.size(), 2u);
    CHECK(list.layers[0].kind == WorldLayer::Kind::Surface && list.layers[0].depthTested);
    CHECK(list.layers[1].kind == WorldLayer::Kind::Billboard && !list.layers[1].depthTested);
    CHECK(near(list.layers[1].tint.r, 1.0f) && near(list.layers[1].origin.z, -20.0f));
    std::size_t inLayers = 0;
    for (const Quad& q : list.quads)
        inLayers += q.layer != 0;
    CHECK(inLayers >= 2);
}

// Midnight Racing's map card: a SurfaceGui in the player's GUI on a board
// ahead of the camera, its Travel and Back buttons clicked through the
// world. The mouse reaches it along the camera's ray; something in front
// of the board (the world pick) keeps it off, the board itself doesn't.
TEST(ui_surface_gui_buttons_take_clicks_through_the_camera)
{
    Billboarded b;
    auto board = make<Part>(b.game->workspace(), "UIBoard");
    board->setSize(Vector3{10.0f, 6.0f, 1.0f});
    board->setCFrame(CFrame(0.0f, 0.0f, -20.0f));
    auto* starter = b.game->getService<StarterGui>();
    auto card = make<SurfaceGui>(starter, "Card");
    card->setAdornee(board.get());
    card->setFace(Enum::NormalId::Back); // +Z, towards the camera
    card->setSizingMode(Enum::SurfaceGuiSizingMode::PixelsPerStud);
    card->setPixelsPerStud(20.0f); // a 200x120 canvas
    auto back = make<TextButton>(card.get(), "Back");
    back->setSize(UDim2{{0.5f, 0}, {1.0f, 0}});
    auto travel = make<TextButton>(card.get(), "GoTo");
    travel->setPosition(UDim2{{0.5f, 0}, {0.0f, 0}});
    travel->setSize(UDim2{{0.5f, 0}, {1.0f, 0}});
    int travelled = 0;
    int backed = 0;
    travel->activated.connect([&] { ++travelled; });
    back->mouseButton1Click.connect([&] { ++backed; });

    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    GuiEngine gui{fonts, atlas};
    gui.settings().topInset = 0.0f;
    gui.setRoots({starter});
    gui.setWorld(b.game->workspace());
    gui.update(Billboarded::screen, 0.0);
    auto click = [&](double x, double y) {
        const bool used = gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, true, x, y, {}});
        gui.handle(platform::MouseButtonEvent{platform::MouseButton::Left, false, x, y, {}});
        return used;
    };
    // About 22 pixels a stud at 19.5 studs: the board spans x 290..510.
    CHECK(gui.objectAt(455, 300) == travel.get());
    CHECK(gui.objectAt(345, 300) == back.get());
    CHECK(gui.objectAt(600, 300) == nullptr);
    CHECK(click(455, 300));
    CHECK(click(345, 300));
    CHECK(!click(600, 300));
    CHECK_EQ(travelled, 1);
    CHECK_EQ(backed, 1);

    // A wall in front of the right half; the board itself is no obstacle.
    auto wall = make<Part>(b.game->workspace(), "Wall");
    gui.setWorldPick([&](Vector3, Vector3 direction) -> std::optional<GuiEngine::WorldHit> {
        if (direction.x > 0.0f)
            return GuiEngine::WorldHit{wall.get(), 10.0f};
        return GuiEngine::WorldHit{board.get(), 19.5f};
    });
    CHECK(!click(455, 300));
    CHECK(click(345, 300));
    CHECK_EQ(travelled, 1);
    CHECK_EQ(backed, 2);

    // An inactive SurfaceGui takes no input; nor one seen from behind.
    gui.setWorldPick(nullptr);
    card->setActive(false);
    gui.update(Billboarded::screen, 0.0);
    CHECK(gui.objectAt(455, 300) == nullptr);
    card->setActive(true);
    card->setFace(Enum::NormalId::Front);
    gui.update(Billboarded::screen, 0.0);
    CHECK(gui.objectAt(455, 300) == nullptr);
}

// A BillboardGui in the player's GUI shows on its Adornee, also one put
// there after the roots were given, and stops once it's taken out; one
// without an Adornee doesn't show. The roots are given every frame.
TEST(ui_root_billboards_show_on_their_adornee)
{
    Billboarded b;
    auto* starter = b.game->getService<StarterGui>();
    auto held = make<BillboardGui>(starter, "Held");
    held->setAdornee(b.part.get());
    held->setSize(UDim2{{0.0f, 100}, {0.0f, 50}});
    auto heldFrame = make<Frame>(held.get());
    heldFrame->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    auto loose = make<BillboardGui>(starter, "Loose");
    loose->setSize(UDim2{{0.0f, 100}, {0.0f, 50}});
    auto looseFrame = make<Frame>(loose.get());

    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    GuiEngine gui{fonts, atlas};
    gui.settings().topInset = 0.0f;
    gui.setWorld(b.game->workspace());
    auto frame = [&] {
        gui.setRoots({starter});
        gui.update(Billboarded::screen, 0.0);
    };
    frame();
    CHECK(gui.rectOf(heldFrame.get()).has_value());
    CHECK(!gui.rectOf(looseFrame.get()).has_value());

    auto folder = make<Folder>(starter, "Later");
    auto later = make<BillboardGui>(folder.get(), "Later");
    later->setAdornee(b.part.get());
    later->setSize(UDim2{{0.0f, 100}, {0.0f, 50}});
    auto laterFrame = make<Frame>(later.get());
    laterFrame->setSize(UDim2{{1.0f, 0}, {1.0f, 0}});
    frame();
    CHECK(gui.rectOf(laterFrame.get()).has_value());

    held->setParent(nullptr);
    frame();
    CHECK(!gui.rectOf(heldFrame.get()).has_value());
    CHECK(gui.rectOf(laterFrame.get()).has_value());
}

namespace {

// A holder with a UIListLayout and frames of the given sizes in it, in order.
struct ListFixture {
    Screen s;
    std::shared_ptr<Frame> holder = make<Frame>(s.screen.get());
    std::shared_ptr<UIListLayout> list = make<UIListLayout>(holder.get());
    std::vector<std::shared_ptr<Frame>> items;

    ListFixture(Vector2 holderSize, std::vector<Vector2> sizes, Enum::FillDirection direction)
    {
        holder->setSize(UDim2::fromOffset(static_cast<int>(holderSize.x), static_cast<int>(holderSize.y)));
        list->setFillDirection(direction);
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto item = make<Frame>(holder.get());
            item->setSize(UDim2::fromOffset(static_cast<int>(sizes[i].x), static_cast<int>(sizes[i].y)));
            item->setLayoutOrder(static_cast<int>(i));
            items.push_back(item);
        }
    }
    RectF rect(std::size_t i)
    {
        s.layout();
        return s.rect(*items[i]);
    }
};

} // namespace

TEST(layout_list_wraps_onto_lines)
{
    // 100 wide, items 40 wide with 10 between: two per line.
    ListFixture f({100, 200}, {{40, 20}, {40, 30}, {40, 20}, {40, 20}, {40, 20}}, Enum::FillDirection::Horizontal);
    f.list->setPadding({0.0f, 10});
    f.list->setWraps(true);
    CHECK(near(f.rect(1).x, 50));
    CHECK(near(f.rect(2).x, 0));
    CHECK(near(f.rect(2).y, 30 + 10)); // under the first line, as tall as its tallest
    CHECK(near(f.rect(4).y, 30 + 10 + 20 + 10));
    // Lines spread across the cross axis.
    f.list->setVerticalFlex(Enum::UIFlexAlignment::SpaceBetween);
    CHECK(near(f.rect(0).y, 0));
    CHECK(near(f.rect(4).bottom(), 200));
    // Without Wraps it's one line, running past the end.
    f.list->setWraps(false);
    f.list->setVerticalFlex(Enum::UIFlexAlignment::None);
    CHECK(near(f.rect(4).x, 200));
    CHECK(near(f.rect(4).y, 0));
}

TEST(layout_list_flex_spreads_the_free_space)
{
    // Three 20-wide items in 200: 140 free.
    ListFixture f({200, 50}, {{20, 10}, {20, 10}, {20, 10}}, Enum::FillDirection::Horizontal);
    f.list->setHorizontalFlex(Enum::UIFlexAlignment::SpaceBetween);
    CHECK(near(f.rect(0).x, 0));
    CHECK(near(f.rect(1).x, 90));
    CHECK(near(f.rect(2).right(), 200));
    f.list->setHorizontalFlex(Enum::UIFlexAlignment::SpaceEvenly);
    CHECK(near(f.rect(0).x, 35));
    CHECK(near(f.rect(1).x, 90));
    f.list->setHorizontalFlex(Enum::UIFlexAlignment::SpaceAround);
    CHECK(near(f.rect(0).x, 140.0f / 6.0f));
    f.list->setHorizontalFlex(Enum::UIFlexAlignment::Fill);
    CHECK(near(f.rect(0).w, 200.0f / 3.0f));
    CHECK(near(f.rect(2).right(), 200));
}

TEST(layout_list_flex_items_grow_shrink_and_align)
{
    ListFixture f({200, 50}, {{20, 10}, {40, 10}, {20, 10}}, Enum::FillDirection::Horizontal);
    auto grow = make<UIFlexItem>(f.items[1].get());
    grow->setFlexMode(Enum::UIFlexMode::Grow);
    CHECK(near(f.rect(1).w, 160));
    CHECK(near(f.rect(2).right(), 200));
    // Custom ratios share it out.
    auto other = make<UIFlexItem>(f.items[0].get());
    other->setFlexMode(Enum::UIFlexMode::Custom);
    other->setGrowRatio(3.0f);
    CHECK(near(f.rect(0).w, 20 + 90));
    CHECK(near(f.rect(1).w, 40 + 30));

    // Too little room: Shrink gives it back, by size.
    ListFixture g({50, 50}, {{20, 10}, {40, 10}}, Enum::FillDirection::Horizontal);
    make<UIFlexItem>(g.items[1].get())->setFlexMode(Enum::UIFlexMode::Shrink);
    CHECK(near(g.rect(1).w, 30));

    // Across the line: Stretch fills it, the item's own alignment wins.
    f.list->setItemLineAlignment(Enum::ItemLineAlignment::Stretch);
    CHECK(near(f.rect(2).h, 50));
    grow->setItemLineAlignment(Enum::ItemLineAlignment::End);
    CHECK(near(f.rect(1).bottom(), 50));
    CHECK(near(f.rect(1).h, 10));
}

TEST(layout_screen_insets)
{
    Screen s;
    s.gui.settings().topInset = 36.0f;
    auto frame = make<Frame>(s.screen.get());
    frame->setSize(UDim2::fromScale(1, 1));
    s.layout();
    CHECK(near(s.rect(*frame).y, 36)); // CoreUISafeInsets: under the top bar
    s.screen->setScreenInsets(Enum::ScreenInsets::DeviceSafeInsets);
    s.layout();
    CHECK(near(s.rect(*frame).y, 0));
    CHECK(near(s.rect(*frame).h, 600));
    s.screen->setScreenInsets(Enum::ScreenInsets::TopbarSafeInsets);
    s.layout();
    CHECK(near(s.rect(*frame).h, 36)); // in the top bar, between its buttons
    CHECK(s.rect(*frame).x > 0);
    CHECK(s.rect(*frame).right() < 800);
}

TEST(layout_canvas_group_fades_and_tints_what_is_in_it)
{
    Screen s;
    auto group = make<CanvasGroup>(s.screen.get());
    group->setSize(UDim2::fromOffset(100, 100));
    group->setBackgroundTransparency(1.0f);
    group->setGroupTransparency(0.5f);
    group->setGroupColor3({1.0f, 0.0f, 0.0f});
    auto inside = make<Frame>(group.get());
    inside->setSize(UDim2::fromOffset(10, 10));
    inside->setBackgroundColor3({1.0f, 1.0f, 1.0f});
    inside->setBorderSizePixel(0);
    s.layout();
    DrawList list;
    s.gui.draw(list);
    CHECK_EQ(list.quads.size(), 1u);
    CHECK(near(list.quads[0].color.a, 0.5f));
    CHECK(near(list.quads[0].color.g, 0.0f));
    CHECK(near(list.quads[0].color.r, 1.0f));
}

TEST(layout_hidden_measurements_do_not_add_cells_to_visible_layout_extents)
{
    Screen s;
    auto holder = make<Frame>(s.screen.get());
    holder->setSize(UDim2::fromOffset(300, 200));
    auto list = make<UIListLayout>(holder.get());
    auto visible = make<Frame>(holder.get());
    visible->setSize(UDim2::fromOffset(80, 20));
    auto hidden = make<Frame>(holder.get());
    hidden->setVisible(false);
    hidden->setSize(UDim2::fromOffset(1000, 1000));
    s.gui.ensureGeometry({800, 600}, 0);
    CHECK(list->absoluteContentSize() == (Vector2{80, 20}));
    CHECK(hidden->absoluteSize() == (Vector2{1000, 1000}));
    CHECK(!s.gui.rectOf(hidden.get()).has_value());
    visible->setVisible(false);
    s.gui.ensureGeometry({800, 600}, 0);
    CHECK(list->absoluteContentSize() == Vector2{});
}
