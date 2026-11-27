#include "framework.hpp"

#include <lunar/platform/Window.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

using namespace lunar::platform;
using namespace std::chrono_literals;

namespace {

bool haveCompositor()
{
    const char* display = std::getenv("WAYLAND_DISPLAY");
    return display && *display;
}

// Points WAYLAND_DISPLAY elsewhere for one scope.
class DisplayOverride {
public:
    explicit DisplayOverride(const char* value)
    {
        if (const char* old = std::getenv("WAYLAND_DISPLAY"))
            old_ = old;
        setenv("WAYLAND_DISPLAY", value, 1);
    }
    DisplayOverride(const DisplayOverride&) = delete;
    DisplayOverride& operator=(const DisplayOverride&) = delete;
    ~DisplayOverride()
    {
        if (old_)
            setenv("WAYLAND_DISPLAY", old_->c_str(), 1);
        else
            unsetenv("WAYLAND_DISPLAY");
    }

private:
    std::optional<std::string> old_;
};

} // namespace

TEST(window_create_fails_cleanly_without_compositor)
{
    DisplayOverride display("lunarstone-test-no-such-display");
    auto window = Window::create();
    CHECK(!window);
    CHECK(window.error().contains("Wayland"));
}

// Needs a running compositor; passes trivially without one.
TEST(window_smoke_test)
{
    if (!haveCompositor())
        return;

    auto created = Window::create({.title = "Lunarstone test", .appId = "lunarstone-test", .size = {320, 240}});
    CHECK(created.has_value());
    Window window = std::move(*created);

    NativeHandles handles = window.nativeHandles();
    CHECK(handles.display != nullptr);
    CHECK(handles.surface != nullptr);
    CHECK(window.logicalSize().width > 0);
    CHECK(window.framebufferSize().width >= window.logicalSize().width);
    CHECK(window.scale() >= 1);

    CHECK(window.fillSolid(0xff202838).has_value());
    window.setPointerLocked(true);
    auto until = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < until && !window.shouldClose()) {
        window.waitEvents(20ms);
        window.events().clear();
    }
    window.setPointerLocked(false);
    window.pollEvents();
    CHECK(!window.error());
}
