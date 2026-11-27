#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/LoadingScreen.hpp>

#include <string>
#include <string_view>

using namespace lunar;
using namespace lunar::ui;

namespace {

// The loading screen on a 1280x720 screen, a frame at a time.
struct Screen {
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    LoadingScreen loading{fonts, atlas};
    DrawList list;
    double time = 1.0;
    int leaves = 0;
    std::string asked; // the last image the screen asked for

    Screen()
    {
        loading.onLeave = [this] { ++leaves; };
        loading.setImageProvider([this](std::string_view id) {
            asked = id;
            return nullptr; // still loading
        });
        frame(0.0);
    }
    void frame(double dt = 1.0 / 60.0)
    {
        time += dt;
        loading.update(time, {1280, 720}, 1.0f);
        list.clear();
        loading.draw(list);
    }
    bool click(double x, double y)
    {
        loading.handle(platform::MouseMoveEvent{x, y});
        const bool down = loading.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y});
        const bool up = loading.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y});
        return down && up;
    }
};

} // namespace

TEST(loading_screen_starts_hidden_and_draws_nothing)
{
    Screen s;
    CHECK(!s.loading.visible());
    CHECK(!s.loading.blocking());
    CHECK(s.list.quads.empty());
    CHECK(!s.loading.handle(platform::KeyEvent{.key = platform::Key::W, .pressed = true}));
}

TEST(loading_screen_covers_the_screen_and_takes_the_input)
{
    Screen s;
    s.loading.show();
    s.loading.setStatus("Joining server");
    s.frame();
    CHECK(s.loading.visible());
    CHECK(s.loading.blocking());
    CHECK(s.loading.opacity() == 1.0f);
    CHECK(!s.list.quads.empty());
    const Quad& backdrop = s.list.quads.front();
    CHECK(backdrop.rect == (RectF{0, 0, 1280, 720}));
    CHECK(backdrop.color.a == 1.0f);
    // Keys and clicks away from the leave button go nowhere.
    CHECK(s.loading.handle(platform::KeyEvent{.key = platform::Key::W, .pressed = true}));
    CHECK(s.click(640, 360));
    CHECK(s.leaves == 0);
    // Pointer moves are everyone's.
    CHECK(!s.loading.handle(platform::MouseMoveEvent{10, 10}));
}

TEST(loading_screen_asks_for_the_game_icon)
{
    Screen s;
    s.loading.show();
    s.loading.setGame("Sample Experience", "Sample Creator", "rbxthumb://type=GameIcon&id=123456002&w=256&h=256");
    s.frame();
    CHECK(s.asked == "rbxthumb://type=GameIcon&id=123456002&w=256&h=256");
    // A new game forgets the last one's.
    s.asked.clear();
    s.loading.show();
    s.frame();
    CHECK(s.asked.empty());
}

TEST(loading_screen_leave_button_leaves)
{
    Screen s;
    s.loading.show();
    s.frame();
    const RectF leave = s.loading.leaveRect();
    CHECK(leave.right() <= 1280.0f);
    CHECK(leave.x > 1280.0f - 80.0f);
    const double x = static_cast<double>(leave.x + leave.w / 2), y = static_cast<double>(leave.y + leave.h / 2);
    CHECK(s.click(x, y));
    CHECK(s.leaves == 1);
    // Pressed on it and let go elsewhere: no.
    s.loading.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y});
    s.loading.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = 640, .y = 360});
    CHECK(s.leaves == 1);
}

TEST(loading_screen_fades_out_once_dismissed)
{
    Screen s;
    s.loading.show();
    s.frame();
    s.loading.dismiss();
    CHECK(!s.loading.blocking()); // the game has the input back while it fades
    CHECK(!s.loading.handle(platform::KeyEvent{.key = platform::Key::W, .pressed = true}));
    s.frame(static_cast<double>(LoadingScreen::fadeSeconds) / 2);
    CHECK(s.loading.visible());
    CHECK(s.loading.opacity() > 0.4f);
    CHECK(s.loading.opacity() < 0.6f);
    CHECK(s.list.quads.front().color.a < 1.0f);
    s.frame(static_cast<double>(LoadingScreen::fadeSeconds));
    CHECK(!s.loading.visible());
    CHECK(s.list.quads.empty());
}

// ReplicatedFirst:RemoveDefaultLoadingScreen(): gone in the same frame.
TEST(loading_screen_removed_at_once)
{
    Screen s;
    s.loading.show();
    s.frame();
    s.loading.remove();
    s.frame();
    CHECK(!s.loading.visible());
    CHECK(s.list.quads.empty());
}

TEST(loading_screen_error_stops_the_spinner)
{
    Screen s;
    s.loading.show();
    s.loading.setStatus("Joining server");
    s.frame();
    const std::size_t spinning = s.list.quads.size();
    s.loading.setStatus("Disconnected: kicked", true);
    s.frame();
    CHECK(s.list.quads.size() < spinning); // the dots are gone, the text stays
    CHECK(s.loading.blocking());
}
