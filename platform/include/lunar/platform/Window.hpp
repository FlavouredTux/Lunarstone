#pragma once

#include <lunar/platform/Events.hpp>
#include <lunar/platform/Geometry.hpp>
#include <lunar/platform/InputState.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

struct wl_display;
struct wl_surface;

namespace lunar::platform {

struct WindowConfig {
    std::string title = "Lunarstone";
    std::string appId = "lunarstone"; // matches the .desktop file name, if any
    Extent size{1280, 720};           // logical pixels
    bool fullscreen = false;
};

// What a renderer needs for vkCreateWaylandSurfaceKHR. Both stay valid for the
// window's lifetime.
struct NativeHandles {
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;
};

// A Wayland xdg_toplevel with keyboard and pointer input. Move-only; the
// connection to the compositor is per window.
//
// The surface has no content of its own: it appears once something presents
// to it (a Vulkan swapchain, or fillSolid). Call pollEvents() or waitEvents()
// once per frame, then read events() and input(). Not thread-safe.
class Window {
public:
    static std::expected<Window, std::string> create(const WindowConfig& config = {});

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // Starts a new input frame and dispatches whatever the compositor sent,
    // without blocking.
    void pollEvents();
    // Like pollEvents, but blocks until at least one event arrives or the
    // timeout passes (forever without one).
    void waitEvents(std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    EventQueue& events();
    const InputState& input() const;
    // An event as if the compositor had sent it (input() sees it too), for
    // input from elsewhere: a remote control, a test. Call after pollEvents.
    void inject(Event event);

    // Set by a close request from the compositor (or a lost connection).
    bool shouldClose() const;
    void setShouldClose(bool value);
    // Why the connection broke, if it did.
    const std::optional<std::string>& error() const;

    Extent logicalSize() const;
    Extent framebufferSize() const; // swapchain extent
    double scale() const;           // fractional where the compositor supports it

    void setTitle(const std::string& title);
    void setFullscreen(bool fullscreen);
    void toggleFullscreen() { setFullscreen(!fullscreen()); }
    bool fullscreen() const;
    bool focused() const;
    // The compositor says nothing of the window is on screen (another
    // workspace, say), so frames presented now would only queue up.
    bool suspended() const;
    // Asks for a frame callback with the next commit (the next frame
    // presented), unless one is outstanding; suspended() watches for it.
    void requestFrameCallback();
    // Once a frame: notices an overdue frame callback, and asks again now
    // and then while hidden, so being shown again is noticed.
    void watchFrames();
    bool hasServerDecorations() const;

    // Locks the pointer in place and hides it while the window has pointer
    // focus; relative motion keeps coming as RelativeMotionEvent. Needs
    // zwp_pointer_constraints_v1 (returns false without it).
    bool setPointerLocked(bool locked);
    bool pointerLockRequested() const;
    bool pointerLocked() const; // actually locked right now

    // The clipboard as UTF-8 text: empty when it holds no text. Reading
    // waits (up to half a second) while its owner sends it. Setting it takes
    // effect from the latest key press, as Wayland wants a recent input
    // serial; false without clipboard support.
    std::string clipboardText();
    bool setClipboardText(std::string text);

    NativeHandles nativeHandles() const;

    // Presents a solid colour (0xAARRGGBB) through wl_shm so the window shows
    // up without a renderer. Don't mix with a Vulkan swapchain on the same
    // window. Redrawn automatically on resize once called.
    std::expected<void, std::string> fillSolid(std::uint32_t argb);

    struct Impl; // public only so the Wayland callbacks in Window.cpp can name it

private:
    explicit Window(std::unique_ptr<Impl> impl);

    // Heap-allocated so the listener user-data pointers survive moves.
    std::unique_ptr<Impl> impl_;
};

} // namespace lunar::platform
