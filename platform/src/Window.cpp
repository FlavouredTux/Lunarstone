#include <lunar/platform/Window.hpp>

#include <lunar/platform/KeyRepeat.hpp>
#include <lunar/platform/Keyboard.hpp>

#include <cursor-shape-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <wayland-client.h>
#include <xdg-decoration-unstable-v1-client-protocol.h>
#include <xdg-shell-client-protocol.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::platform {

namespace {

template <class T, auto Destroy>
struct WlDeleter {
    void operator()(T* p) const { Destroy(p); }
};

template <class T, auto Destroy>
using WlPtr = std::unique_ptr<T, WlDeleter<T, Destroy>>;

// Objects with a release request from some version on must use it then.
void releaseSeat(wl_seat* seat)
{
    wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION ? wl_seat_release(seat) : wl_seat_destroy(seat);
}

void releaseKeyboard(wl_keyboard* keyboard)
{
    wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION ? wl_keyboard_release(keyboard)
                                                                           : wl_keyboard_destroy(keyboard);
}

void releaseDataDevice(wl_data_device* device)
{
    wl_data_device_get_version(device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION ? wl_data_device_release(device)
                                                                               : wl_data_device_destroy(device);
}

void releasePointer(wl_pointer* pointer)
{
    wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION ? wl_pointer_release(pointer)
                                                                        : wl_pointer_destroy(pointer);
}

void releaseOutput(wl_output* output)
{
    wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION ? wl_output_release(output)
                                                                     : wl_output_destroy(output);
}

// Owns a file descriptor.
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd()
    {
        if (fd_ >= 0)
            close(fd_);
    }
    int get() const { return fd_; }

private:
    int fd_;
};

// Read-only or read-write view of an mmap'd region, unmapped on destruction.
class Mapping {
public:
    Mapping(int fd, std::size_t size, int prot, int flags) : size_(size), data_(mmap(nullptr, size, prot, flags, fd, 0))
    {
        if (data_ == MAP_FAILED)
            data_ = nullptr;
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping()
    {
        if (data_)
            munmap(data_, size_);
    }
    void* data() const { return data_; }

private:
    std::size_t size_;
    void* data_;
};

constexpr std::uint32_t compositorVersion = 6; // preferred_buffer_scale
constexpr std::uint32_t wmBaseVersion = 6;
constexpr std::uint32_t seatVersion = 8; // axis_value120
constexpr std::uint32_t outputVersion = 4;
constexpr std::uint32_t dataDeviceManagerVersion = 3;

// Text types a clipboard offer may come in, best first, and what ours offers.
constexpr std::array<std::string_view, 5> textMimeTypes{"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                                        "STRING", "TEXT"};

int pollTimeout(std::optional<KeyRepeat::Clock::duration> timeout)
{
    if (!timeout)
        return -1;
    auto ms = std::chrono::ceil<std::chrono::milliseconds>(std::max(*timeout, KeyRepeat::Clock::duration::zero()));
    return static_cast<int>(std::min<std::chrono::milliseconds::rep>(ms.count(), 1'000'000));
}

struct Output {
    WlPtr<wl_output, releaseOutput> output;
    std::uint32_t name = 0;
    std::int32_t scale = 1;
    Window::Impl* window = nullptr;
};

struct ShmBuffer {
    WlPtr<wl_buffer, wl_buffer_destroy> buffer;
    bool busy = true;
};

// Wheel input collected over one wl_pointer.frame. Index 0 is vertical, 1 horizontal.
struct PendingScroll {
    std::array<double, 2> value{};
    std::array<std::int32_t, 2> discrete{};
    std::array<std::int32_t, 2> value120{};
    std::array<bool, 2> hasDiscrete{};
    std::array<bool, 2> hasValue120{};
};

} // namespace

// Declared in the order of creation; members are destroyed in reverse, which
// is the order the protocol wants (decoration before toplevel before
// xdg_surface before wl_surface, everything before the display).
struct Window::Impl {
    WlPtr<wl_display, wl_display_disconnect> display;
    WlPtr<wl_registry, wl_registry_destroy> registry;
    WlPtr<wl_compositor, wl_compositor_destroy> compositor;
    WlPtr<wl_shm, wl_shm_destroy> shm;
    WlPtr<xdg_wm_base, xdg_wm_base_destroy> wmBase;
    WlPtr<zxdg_decoration_manager_v1, zxdg_decoration_manager_v1_destroy> decorationManager;
    WlPtr<zwp_relative_pointer_manager_v1, zwp_relative_pointer_manager_v1_destroy> relativePointerManager;
    WlPtr<zwp_pointer_constraints_v1, zwp_pointer_constraints_v1_destroy> pointerConstraints;
    WlPtr<wp_cursor_shape_manager_v1, wp_cursor_shape_manager_v1_destroy> cursorShapeManager;
    WlPtr<wp_viewporter, wp_viewporter_destroy> viewporter;
    WlPtr<wp_fractional_scale_manager_v1, wp_fractional_scale_manager_v1_destroy> fractionalScaleManager;
    std::vector<std::unique_ptr<Output>> outputs;

    WlPtr<wl_seat, releaseSeat> seat;
    std::uint32_t seatName = 0;
    WlPtr<wl_keyboard, releaseKeyboard> keyboard;
    WlPtr<wl_pointer, releasePointer> pointer;
    WlPtr<zwp_relative_pointer_v1, zwp_relative_pointer_v1_destroy> relativePointer;
    WlPtr<wp_cursor_shape_device_v1, wp_cursor_shape_device_v1_destroy> cursorShapeDevice;

    // The clipboard (the seat's selection). Offers announce their types
    // before the selection event names one; ours is clipboardSource.
    WlPtr<wl_data_device_manager, wl_data_device_manager_destroy> dataDeviceManager;
    WlPtr<wl_data_device, releaseDataDevice> dataDevice;
    std::unordered_map<wl_data_offer*, std::vector<std::string>> offerTypes;
    wl_data_offer* selectionOffer = nullptr;
    wl_data_offer* dragOffer = nullptr;
    WlPtr<wl_data_source, wl_data_source_destroy> clipboardSource;
    std::string clipboardText;
    std::uint32_t inputSerial = 0; // of the latest key press, for set_selection

    WlPtr<wl_surface, wl_surface_destroy> surface;
    WlPtr<wp_viewport, wp_viewport_destroy> viewport;
    WlPtr<wp_fractional_scale_v1, wp_fractional_scale_v1_destroy> fractionalScale;
    WlPtr<xdg_surface, xdg_surface_destroy> xdgSurface;
    WlPtr<xdg_toplevel, xdg_toplevel_destroy> toplevel;
    // The frame callback asked for with the latest frame, while it's
    // outstanding: one that doesn't come means the window isn't being shown.
    WlPtr<wl_callback, wl_callback_destroy> frameCallback;
    std::chrono::steady_clock::time_point frameRequested;
    bool unseen = false; // a frame callback overdue: not being shown, until one comes
    WlPtr<zxdg_toplevel_decoration_v1, zxdg_toplevel_decoration_v1_destroy> decoration;
    WlPtr<zwp_locked_pointer_v1, zwp_locked_pointer_v1_destroy> lockedPointer;
    std::vector<std::unique_ptr<ShmBuffer>> shmBuffers;

    std::optional<Keyboard> xkb;
    KeyRepeat repeat;
    EventQueue events;
    InputState input;
    WindowGeometry geometry;
    std::vector<wl_output*> enteredOutputs;

    Extent pendingSize;
    bool pendingFullscreen = false;
    bool pendingConstrained = false;
    bool pendingSuspended = false;
    bool configured = false;
    bool fullscreen = false;
    bool suspended = false; // the compositor isn't showing it (xdg-shell 6)
    bool serverDecorations = false;
    bool focused = false;
    bool closeRequested = false;
    std::optional<std::string> error;

    std::uint32_t pointerSerial = 0;
    bool pointerInside = false;
    std::optional<Vec2d> pointerPosition;
    PendingScroll scroll;
    bool preferredScaleSeen = false;
    bool lockRequested = false;
    bool locked = false;
    std::optional<std::uint32_t> solidColor;

    explicit Impl(Extent size) : geometry(size) {}

    void push(Event event)
    {
        input.apply(event);
        events.push(std::move(event));
    }

    std::expected<void, std::string> connect(const WindowConfig& config);
    void dispatch(std::optional<KeyRepeat::Clock::duration> timeout);
    void checkConnection();
    void processRepeats();

    void onGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version);
    void onGlobalRemove(std::uint32_t name);
    void onSeatCapabilities(std::uint32_t caps);
    void createDataDevice();
    void destroyOffer(wl_data_offer*& offer);
    void onToplevelConfigure(std::int32_t width, std::int32_t height, const wl_array* states);
    void onSurfaceConfigure(std::uint32_t serial);
    void onKeymap(std::uint32_t format, int fd, std::uint32_t size);
    void onKey(std::uint32_t scancode, bool pressed);
    void onPointerMotion(double x, double y);
    void onPointerFrame();
    void onRelativeMotion(double dx, double dy, double rawDx, double rawDy);
    void onOutputsChanged();
    void applyScale120(std::int32_t scale120);
    void updateSurfaceScale() const;

    std::int32_t outputScale() const;
    void updateCursor() const;
    void updatePointerLock();
    std::expected<void, std::string> drawSolid(std::uint32_t argb);
    void redrawSolid();
};

namespace {

Window::Impl& self(void* data)
{
    return *static_cast<Window::Impl*>(data);
}

constexpr wl_registry_listener registryListener{
    .global = [](void* data, wl_registry*, std::uint32_t name, const char* interface,
                 std::uint32_t version) { self(data).onGlobal(name, interface, version); },
    .global_remove = [](void* data, wl_registry*, std::uint32_t name) { self(data).onGlobalRemove(name); },
};

constexpr xdg_wm_base_listener wmBaseListener{
    .ping = [](void*, xdg_wm_base* base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); },
};

constexpr xdg_surface_listener xdgSurfaceListener{
    .configure = [](void* data, xdg_surface*, std::uint32_t serial) { self(data).onSurfaceConfigure(serial); },
};

constexpr xdg_toplevel_listener toplevelListener{
    .configure = [](void* data, xdg_toplevel*, std::int32_t width, std::int32_t height,
                    wl_array* states) { self(data).onToplevelConfigure(width, height, states); },
    .close =
        [](void* data, xdg_toplevel*) {
            self(data).closeRequested = true;
            self(data).push(CloseEvent{});
        },
    .configure_bounds = [](void*, xdg_toplevel*, std::int32_t, std::int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

constexpr zxdg_toplevel_decoration_v1_listener decorationListener{
    .configure
    = [](void* data, zxdg_toplevel_decoration_v1*,
         std::uint32_t mode) { self(data).serverDecorations = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE; },
};

constexpr wl_callback_listener frameDoneListener{
    .done =
        [](void* data, wl_callback*, std::uint32_t) {
            self(data).frameCallback.reset();
            self(data).unseen = false;
        },
};

constexpr wl_surface_listener surfaceListener{
    .enter =
        [](void* data, wl_surface*, wl_output* output) {
            Window::Impl& impl = self(data);
            impl.enteredOutputs.push_back(output);
            impl.onOutputsChanged();
        },
    .leave =
        [](void* data, wl_surface*, wl_output* output) {
            Window::Impl& impl = self(data);
            std::erase(impl.enteredOutputs, output);
            impl.onOutputsChanged();
        },
    .preferred_buffer_scale =
        [](void* data, wl_surface*, std::int32_t factor) {
            Window::Impl& impl = self(data);
            impl.preferredScaleSeen = true;
            if (!impl.fractionalScale)
                impl.applyScale120(factor * 120);
        },
    .preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {},
};

constexpr wp_fractional_scale_v1_listener fractionalScaleListener{
    .preferred_scale = [](void* data, wp_fractional_scale_v1*,
                          std::uint32_t scale) { self(data).applyScale120(static_cast<std::int32_t>(scale)); },
};

constexpr wl_output_listener outputListener{
    .geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char*,
                   const char*, std::int32_t) {},
    .mode = [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {},
    .done = [](void* data, wl_output*) { static_cast<Output*>(data)->window->onOutputsChanged(); },
    .scale = [](void* data, wl_output*, std::int32_t factor) { static_cast<Output*>(data)->scale = factor; },
    .name = [](void*, wl_output*, const char*) {},
    .description = [](void*, wl_output*, const char*) {},
};

constexpr wl_seat_listener seatListener{
    .capabilities = [](void* data, wl_seat*, std::uint32_t caps) { self(data).onSeatCapabilities(caps); },
    .name = [](void*, wl_seat*, const char*) {},
};

constexpr wl_keyboard_listener keyboardListener{
    .keymap = [](void* data, wl_keyboard*, std::uint32_t format, std::int32_t fd,
                 std::uint32_t size) { self(data).onKeymap(format, fd, size); },
    .enter =
        [](void* data, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) {
            self(data).focused = true;
            self(data).push(FocusEvent{.focused = true});
        },
    .leave =
        [](void* data, wl_keyboard*, std::uint32_t, wl_surface*) {
            Window::Impl& impl = self(data);
            impl.focused = false;
            impl.repeat.stop();
            impl.push(FocusEvent{.focused = false});
        },
    .key =
        [](void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
            if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                self(data).inputSerial = serial;
            self(data).onKey(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
        },
    .modifiers =
        [](void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t latched,
           std::uint32_t locked, std::uint32_t group) {
            Window::Impl& impl = self(data);
            if (impl.xkb && impl.xkb->updateModifiers(depressed, latched, locked, group))
                impl.push(ModifiersEvent{.modifiers = impl.xkb->modifiers()});
        },
    .repeat_info
    = [](void* data, wl_keyboard*, std::int32_t rate, std::int32_t delay) { self(data).repeat.setInfo(rate, delay); },
};

constexpr wl_data_offer_listener dataOfferListener{
    .offer
    = [](void* data, wl_data_offer* offer, const char* type) { self(data).offerTypes[offer].emplace_back(type); },
    .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
    .action = [](void*, wl_data_offer*, std::uint32_t) {},
};

// Drag and drop isn't supported: drag offers are only cleaned up.
constexpr wl_data_device_listener dataDeviceListener{
    .data_offer =
        [](void* data, wl_data_device*, wl_data_offer* offer) {
            self(data).offerTypes[offer];
            wl_data_offer_add_listener(offer, &dataOfferListener, data);
        },
    .enter =
        [](void* data, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer) {
            Window::Impl& impl = self(data);
            impl.destroyOffer(impl.dragOffer);
            impl.dragOffer = offer;
        },
    .leave = [](void* data, wl_data_device*) { self(data).destroyOffer(self(data).dragOffer); },
    .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
    .drop = [](void* data, wl_data_device*) { self(data).destroyOffer(self(data).dragOffer); },
    .selection =
        [](void* data, wl_data_device*, wl_data_offer* offer) {
            Window::Impl& impl = self(data);
            impl.destroyOffer(impl.selectionOffer);
            impl.selectionOffer = offer;
        },
};

constexpr wl_data_source_listener dataSourceListener{
    .target = [](void*, wl_data_source*, const char*) {},
    .send =
        [](void* data, wl_data_source*, const char*, std::int32_t fd) {
            const std::string& text = self(data).clipboardText;
            std::size_t written = 0;
            while (written < text.size()) {
                const ssize_t n = write(fd, text.data() + written, text.size() - written);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                written += static_cast<std::size_t>(n);
            }
            close(fd);
        },
    .cancelled =
        [](void* data, wl_data_source*) {
            // Someone else owns the clipboard now.
            self(data).clipboardSource.reset();
            self(data).clipboardText.clear();
        },
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished = [](void*, wl_data_source*) {},
    .action = [](void*, wl_data_source*, std::uint32_t) {},
};

constexpr wl_pointer_listener pointerListener{
    .enter =
        [](void* data, wl_pointer*, std::uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
            Window::Impl& impl = self(data);
            impl.pointerSerial = serial;
            impl.pointerInside = true;
            impl.pointerPosition.reset(); // no relative motion across the jump
            impl.updateCursor();
            impl.push(MouseEnterEvent{.entered = true});
            impl.onPointerMotion(wl_fixed_to_double(x), wl_fixed_to_double(y));
        },
    .leave =
        [](void* data, wl_pointer*, std::uint32_t, wl_surface*) {
            Window::Impl& impl = self(data);
            impl.pointerInside = false;
            impl.push(MouseEnterEvent{.entered = false});
        },
    .motion = [](void* data, wl_pointer*, std::uint32_t, wl_fixed_t x,
                 wl_fixed_t y) { self(data).onPointerMotion(wl_fixed_to_double(x), wl_fixed_to_double(y)); },
    .button =
        [](void* data, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t button, std::uint32_t state) {
            Window::Impl& impl = self(data);
            MouseButton mapped = mouseButtonFromEvdev(button);
            if (mapped == MouseButton::Count)
                return;
            Vec2d at = impl.pointerPosition.value_or(Vec2d{});
            impl.push(MouseButtonEvent{
                .button = mapped,
                .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
                .x = at.x,
                .y = at.y,
                .modifiers = impl.xkb ? impl.xkb->modifiers() : Modifiers{},
            });
        },
    .axis =
        [](void* data, wl_pointer* pointer, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
            Window::Impl& impl = self(data);
            if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL)
                return;
            impl.scroll.value.at(axis) += wl_fixed_to_double(value);
            // Before version 5 there are no frame events to wait for.
            if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
                impl.onPointerFrame();
        },
    .frame = [](void* data, wl_pointer*) { self(data).onPointerFrame(); },
    .axis_source = [](void*, wl_pointer*, std::uint32_t) {},
    .axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
    .axis_discrete =
        [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t discrete) {
            if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL)
                return;
            self(data).scroll.discrete.at(axis) += discrete;
            self(data).scroll.hasDiscrete.at(axis) = true;
        },
    .axis_value120 =
        [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t value120) {
            if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL)
                return;
            self(data).scroll.value120.at(axis) += value120;
            self(data).scroll.hasValue120.at(axis) = true;
        },
    .axis_relative_direction = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
};

constexpr zwp_relative_pointer_v1_listener relativePointerListener{
    .relative_motion =
        [](void* data, zwp_relative_pointer_v1*, std::uint32_t, std::uint32_t, wl_fixed_t dx, wl_fixed_t dy,
           wl_fixed_t rawDx, wl_fixed_t rawDy) {
            self(data).onRelativeMotion(wl_fixed_to_double(dx), wl_fixed_to_double(dy), wl_fixed_to_double(rawDx),
                                        wl_fixed_to_double(rawDy));
        },
};

constexpr zwp_locked_pointer_v1_listener lockedPointerListener{
    .locked =
        [](void* data, zwp_locked_pointer_v1*) {
            self(data).locked = true;
            self(data).push(PointerLockEvent{.locked = true});
        },
    .unlocked =
        [](void* data, zwp_locked_pointer_v1*) {
            self(data).locked = false;
            self(data).push(PointerLockEvent{.locked = false});
        },
};

constexpr wl_buffer_listener shmBufferListener{
    .release = [](void* data, wl_buffer*) { static_cast<ShmBuffer*>(data)->busy = false; },
};

} // namespace

std::expected<void, std::string> Window::Impl::connect(const WindowConfig& config)
{
    display.reset(wl_display_connect(nullptr));
    if (!display)
        return std::unexpected("can't connect to a Wayland compositor (is WAYLAND_DISPLAY set?)");

    registry.reset(wl_display_get_registry(display.get()));
    wl_registry_add_listener(registry.get(), &registryListener, this);
    // The first roundtrip binds the globals, the second collects what they
    // send on bind (seat capabilities, keymap, output scales).
    for (int i = 0; i < 2; ++i)
        if (wl_display_roundtrip(display.get()) < 0)
            return std::unexpected("Wayland roundtrip failed while binding globals");

    if (!compositor)
        return std::unexpected("compositor doesn't offer wl_compositor");
    if (!wmBase)
        return std::unexpected("compositor doesn't support xdg-shell");

    surface.reset(wl_compositor_create_surface(compositor.get()));
    wl_surface_add_listener(surface.get(), &surfaceListener, this);
    // Fractional scaling renders at exactly the output's resolution (e.g. 1.5x)
    // and lets the viewport map the buffer onto the logical size. Without it
    // we fall back to integer buffer scales.
    if (viewporter && fractionalScaleManager) {
        viewport.reset(wp_viewporter_get_viewport(viewporter.get(), surface.get()));
        fractionalScale.reset(
            wp_fractional_scale_manager_v1_get_fractional_scale(fractionalScaleManager.get(), surface.get()));
        wp_fractional_scale_v1_add_listener(fractionalScale.get(), &fractionalScaleListener, this);
    }
    xdgSurface.reset(xdg_wm_base_get_xdg_surface(wmBase.get(), surface.get()));
    xdg_surface_add_listener(xdgSurface.get(), &xdgSurfaceListener, this);
    toplevel.reset(xdg_surface_get_toplevel(xdgSurface.get()));
    xdg_toplevel_add_listener(toplevel.get(), &toplevelListener, this);
    xdg_toplevel_set_title(toplevel.get(), config.title.c_str());
    xdg_toplevel_set_app_id(toplevel.get(), config.appId.c_str());
    if (config.fullscreen)
        xdg_toplevel_set_fullscreen(toplevel.get(), nullptr);

    // Without xdg-decoration (GNOME) the window simply has no decorations.
    if (decorationManager) {
        decoration.reset(zxdg_decoration_manager_v1_get_toplevel_decoration(decorationManager.get(), toplevel.get()));
        zxdg_toplevel_decoration_v1_add_listener(decoration.get(), &decorationListener, this);
        zxdg_toplevel_decoration_v1_set_mode(decoration.get(), ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    // An empty commit asks for the first configure; nothing may be attached
    // before it's acked.
    wl_surface_commit(surface.get());
    while (!configured)
        if (wl_display_roundtrip(display.get()) < 0)
            return std::unexpected("Wayland connection failed before the window was configured");

    updatePointerLock();
    return {};
}

void Window::Impl::onGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version)
{
    auto bindTo
        = [&]<class T, class D>(std::unique_ptr<T, D>& ptr, const wl_interface& iface, std::uint32_t maxVersion) {
              ptr.reset(static_cast<T*>(wl_registry_bind(registry.get(), name, &iface, std::min(version, maxVersion))));
          };

    if (interface == wl_compositor_interface.name) {
        bindTo(compositor, wl_compositor_interface, compositorVersion);
    } else if (interface == wl_shm_interface.name) {
        bindTo(shm, wl_shm_interface, 1);
    } else if (interface == xdg_wm_base_interface.name) {
        bindTo(wmBase, xdg_wm_base_interface, wmBaseVersion);
        xdg_wm_base_add_listener(wmBase.get(), &wmBaseListener, this);
    } else if (interface == zxdg_decoration_manager_v1_interface.name) {
        bindTo(decorationManager, zxdg_decoration_manager_v1_interface, 1);
    } else if (interface == zwp_relative_pointer_manager_v1_interface.name) {
        bindTo(relativePointerManager, zwp_relative_pointer_manager_v1_interface, 1);
    } else if (interface == zwp_pointer_constraints_v1_interface.name) {
        bindTo(pointerConstraints, zwp_pointer_constraints_v1_interface, 1);
    } else if (interface == wp_cursor_shape_manager_v1_interface.name) {
        bindTo(cursorShapeManager, wp_cursor_shape_manager_v1_interface, 1);
    } else if (interface == wp_viewporter_interface.name) {
        bindTo(viewporter, wp_viewporter_interface, 1);
    } else if (interface == wp_fractional_scale_manager_v1_interface.name) {
        bindTo(fractionalScaleManager, wp_fractional_scale_manager_v1_interface, 1);
    } else if (interface == wl_data_device_manager_interface.name) {
        bindTo(dataDeviceManager, wl_data_device_manager_interface, dataDeviceManagerVersion);
        createDataDevice();
    } else if (interface == wl_seat_interface.name && !seat) {
        // One seat is plenty for a game client.
        bindTo(seat, wl_seat_interface, seatVersion);
        seatName = name;
        wl_seat_add_listener(seat.get(), &seatListener, this);
        createDataDevice();
    } else if (interface == wl_output_interface.name) {
        auto& output = outputs.emplace_back(std::make_unique<Output>());
        bindTo(output->output, wl_output_interface, outputVersion);
        output->name = name;
        output->window = this;
        wl_output_add_listener(output->output.get(), &outputListener, output.get());
    }
}

void Window::Impl::onGlobalRemove(std::uint32_t name)
{
    if (seat && name == seatName) {
        onSeatCapabilities(0);
        destroyOffer(selectionOffer);
        destroyOffer(dragOffer);
        clipboardSource.reset();
        dataDevice.reset();
        seat.reset();
        return;
    }
    auto gone = std::ranges::find(outputs, name, &Output::name);
    if (gone == outputs.end())
        return;
    std::erase(enteredOutputs, (*gone)->output.get());
    outputs.erase(gone);
    onOutputsChanged();
}

void Window::Impl::createDataDevice()
{
    if (!dataDeviceManager || !seat || dataDevice)
        return;
    dataDevice.reset(wl_data_device_manager_get_data_device(dataDeviceManager.get(), seat.get()));
    wl_data_device_add_listener(dataDevice.get(), &dataDeviceListener, this);
}

void Window::Impl::destroyOffer(wl_data_offer*& offer)
{
    if (!offer)
        return;
    offerTypes.erase(offer);
    wl_data_offer_destroy(offer);
    offer = nullptr;
}

void Window::Impl::onSeatCapabilities(std::uint32_t caps)
{
    bool hasKeyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    bool hasPointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;

    if (hasKeyboard && !keyboard) {
        keyboard.reset(wl_seat_get_keyboard(seat.get()));
        wl_keyboard_add_listener(keyboard.get(), &keyboardListener, this);
    } else if (!hasKeyboard && keyboard) {
        repeat.stop();
        keyboard.reset();
        xkb.reset();
    }

    if (hasPointer && !pointer) {
        pointer.reset(wl_seat_get_pointer(seat.get()));
        wl_pointer_add_listener(pointer.get(), &pointerListener, this);
        if (relativePointerManager) {
            relativePointer.reset(
                zwp_relative_pointer_manager_v1_get_relative_pointer(relativePointerManager.get(), pointer.get()));
            zwp_relative_pointer_v1_add_listener(relativePointer.get(), &relativePointerListener, this);
        }
        if (cursorShapeManager)
            cursorShapeDevice.reset(wp_cursor_shape_manager_v1_get_pointer(cursorShapeManager.get(), pointer.get()));
        updatePointerLock();
    } else if (!hasPointer && pointer) {
        lockedPointer.reset();
        cursorShapeDevice.reset();
        relativePointer.reset();
        pointer.reset();
    }
}

void Window::Impl::onToplevelConfigure(std::int32_t width, std::int32_t height, const wl_array* states)
{
    pendingSize = {width, height};
    pendingFullscreen = false;
    pendingConstrained = false;
    pendingSuspended = false;
    std::span<const std::uint32_t> list(static_cast<const std::uint32_t*>(states->data),
                                        states->size / sizeof(std::uint32_t));
    for (std::uint32_t state : list) {
        switch (state) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            pendingFullscreen = true;
            pendingConstrained = true;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            pendingConstrained = true;
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            pendingSuspended = true;
            break;
        default:
            break;
        }
    }
}

void Window::Impl::onSurfaceConfigure(std::uint32_t serial)
{
    geometry.configure(pendingSize, pendingConstrained);
    bool resized = geometry.commit();
    fullscreen = pendingFullscreen;
    suspended = pendingSuspended;
    xdg_surface_ack_configure(xdgSurface.get(), serial);
    if (resized || !configured)
        updateSurfaceScale();

    if (!configured) {
        configured = true;
        return; // create() reports the initial size
    }
    if (resized) {
        push(ResizeEvent{geometry.logicalSize(), geometry.framebufferSize(), geometry.scale()});
        if (solidColor)
            redrawSolid();
    }
}

void Window::Impl::onKeymap(std::uint32_t format, int fd, std::uint32_t size)
{
    Fd owned(fd);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0)
        return;
    Mapping map(owned.get(), size, PROT_READ, MAP_PRIVATE);
    if (!map.data())
        return;
    const auto* text = static_cast<const char*>(map.data());
    // The keymap is NUL-terminated and the size includes the terminator.
    auto keymap = Keyboard::fromString(std::string_view(text, strnlen(text, size)));
    if (keymap)
        xkb = std::move(*keymap);
}

void Window::Impl::onKey(std::uint32_t scancode, bool pressed)
{
    if (!xkb)
        return;
    if (!pressed) {
        repeat.release(scancode);
        push(KeyEvent{
            .key = xkb->key(scancode),
            .scancode = scancode,
            .keysym = 0,
            .pressed = false,
            .repeat = false,
            .modifiers = xkb->modifiers(),
        });
        return;
    }

    KeyPress key = xkb->press(scancode);
    push(KeyEvent{
        .key = key.key,
        .scancode = scancode,
        .keysym = key.keysym,
        .pressed = true,
        .repeat = false,
        .modifiers = xkb->modifiers(),
    });
    if (!key.text.empty())
        push(TextEvent{std::move(key.text)});
    if (key.repeats)
        repeat.press(scancode, KeyRepeat::Clock::now());
}

void Window::Impl::processRepeats()
{
    auto scancode = repeat.key();
    if (!xkb || !scancode)
        return;
    Keyboard& keys = *xkb;
    for (std::int32_t n = repeat.due(KeyRepeat::Clock::now()); n > 0; --n) {
        KeyPress key = keys.press(*scancode);
        push(KeyEvent{
            .key = key.key,
            .scancode = *scancode,
            .keysym = key.keysym,
            .pressed = true,
            .repeat = true,
            .modifiers = keys.modifiers(),
        });
        if (!key.text.empty())
            push(TextEvent{std::move(key.text)});
    }
}

void Window::Impl::onPointerMotion(double x, double y)
{
    // Without zwp_relative_pointer_v1, derive relative motion from the cursor.
    if (!relativePointer && pointerPosition) {
        double dx = x - pointerPosition->x;
        double dy = y - pointerPosition->y;
        push(RelativeMotionEvent{.dx = dx, .dy = dy, .rawDx = dx, .rawDy = dy});
    }
    pointerPosition = Vec2d{x, y};
    push(MouseMoveEvent{.x = x, .y = y});
}

void Window::Impl::onPointerFrame()
{
    auto steps = [&](std::size_t axis) {
        if (scroll.hasValue120.at(axis))
            return scroll.value120.at(axis) / 120.0;
        if (scroll.hasDiscrete.at(axis))
            return static_cast<double>(scroll.discrete.at(axis));
        return scroll.value.at(axis) / 10.0; // libinput's ~10 px per notch
    };
    // Wayland's vertical axis grows downwards; ours is positive for "up".
    double dy = -steps(WL_POINTER_AXIS_VERTICAL_SCROLL);
    double dx = steps(WL_POINTER_AXIS_HORIZONTAL_SCROLL);
    scroll = {};
    if (dx != 0 || dy != 0)
        push(ScrollEvent{.dx = dx, .dy = dy});
}

void Window::Impl::onRelativeMotion(double dx, double dy, double rawDx, double rawDy)
{
    push(RelativeMotionEvent{.dx = dx, .dy = dy, .rawDx = rawDx, .rawDy = rawDy});
}

std::int32_t Window::Impl::outputScale() const
{
    std::int32_t scale = 1;
    for (const auto& output : outputs)
        if (std::ranges::contains(enteredOutputs, output->output.get()))
            scale = std::max(scale, output->scale);
    return scale;
}

void Window::Impl::onOutputsChanged()
{
    // The scale of the outputs we're on is only a fallback for compositors
    // that don't send wl_surface.preferred_buffer_scale.
    if (!preferredScaleSeen && !fractionalScale)
        applyScale120(outputScale() * 120);
}

void Window::Impl::updateSurfaceScale() const
{
    // Double-buffered state, latched on the next commit, which for Vulkan is
    // the next present: the renderer must recreate its swapchain on the
    // ResizeEvent before presenting again.
    if (viewport)
        wp_viewport_set_destination(viewport.get(), geometry.logicalSize().width, geometry.logicalSize().height);
    else
        wl_surface_set_buffer_scale(surface.get(), geometry.scale120() / 120);
}

void Window::Impl::applyScale120(std::int32_t scale120)
{
    if (!surface || !geometry.setScale120(scale120))
        return;
    updateSurfaceScale();
    if (configured)
        push(ResizeEvent{geometry.logicalSize(), geometry.framebufferSize(), geometry.scale()});
    if (solidColor)
        redrawSolid();
}

void Window::Impl::updateCursor() const
{
    if (!pointer || !pointerInside)
        return;
    if (lockRequested)
        wl_pointer_set_cursor(pointer.get(), pointerSerial, nullptr, 0, 0);
    else if (cursorShapeDevice)
        wp_cursor_shape_device_v1_set_shape(cursorShapeDevice.get(), pointerSerial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

void Window::Impl::updatePointerLock()
{
    if (lockRequested && !lockedPointer && pointer && pointerConstraints && surface) {
        // Persistent: the compositor re-locks whenever the pointer comes back.
        lockedPointer.reset(zwp_pointer_constraints_v1_lock_pointer(pointerConstraints.get(), surface.get(),
                                                                    pointer.get(), nullptr,
                                                                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT));
        zwp_locked_pointer_v1_add_listener(lockedPointer.get(), &lockedPointerListener, this);
    } else if (!lockRequested && lockedPointer) {
        lockedPointer.reset();
        if (std::exchange(locked, false))
            push(PointerLockEvent{.locked = false});
    }
    updateCursor();
}

void Window::Impl::redrawSolid()
{
    // On failure keep showing the old buffer and stop trying.
    if (solidColor && !drawSolid(*solidColor))
        solidColor.reset();
}

std::expected<void, std::string> Window::Impl::drawSolid(std::uint32_t argb)
{
    if (!shm)
        return std::unexpected("compositor doesn't offer wl_shm");
    std::erase_if(shmBuffers, [](const auto& b) { return !b->busy; });

    Extent size = geometry.framebufferSize();
    std::int32_t stride = size.width * 4;
    auto bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(size.height);

    Fd fd(memfd_create("lunarstone-shm", MFD_CLOEXEC));
    if (fd.get() < 0 || ftruncate(fd.get(), static_cast<off_t>(bytes)) < 0)
        return std::unexpected(std::format("can't allocate a shm buffer: {}", std::strerror(errno)));
    {
        Mapping map(fd.get(), bytes, PROT_READ | PROT_WRITE, MAP_SHARED);
        if (!map.data())
            return std::unexpected(std::format("can't map a shm buffer: {}", std::strerror(errno)));
        std::ranges::fill(std::span(static_cast<std::uint32_t*>(map.data()), bytes / 4), argb);
    }

    wl_shm_pool* pool = wl_shm_create_pool(shm.get(), fd.get(), static_cast<std::int32_t>(bytes));
    auto& buffer = shmBuffers.emplace_back(std::make_unique<ShmBuffer>());
    buffer->buffer.reset(wl_shm_pool_create_buffer(pool, 0, size.width, size.height, stride, WL_SHM_FORMAT_ARGB8888));
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(buffer->buffer.get(), &shmBufferListener, buffer.get());

    wl_surface_attach(surface.get(), buffer->buffer.get(), 0, 0);
    updateSurfaceScale();
    wl_surface_damage_buffer(surface.get(), 0, 0, size.width, size.height);
    wl_surface_commit(surface.get());
    wl_display_flush(display.get());
    return {};
}

void Window::Impl::dispatch(std::optional<KeyRepeat::Clock::duration> timeout)
{
    if (error)
        return;
    wl_display* d = display.get();

    // The standard dance for reading without racing other readers.
    while (wl_display_prepare_read(d) != 0)
        wl_display_dispatch_pending(d);
    if (wl_display_flush(d) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(d);
        checkConnection();
        return;
    }

    if (auto deadline = repeat.nextDeadline()) {
        auto untilRepeat = *deadline - KeyRepeat::Clock::now();
        timeout = timeout ? std::min(*timeout, untilRepeat) : untilRepeat;
    }

    pollfd fd{.fd = wl_display_get_fd(d), .events = POLLIN, .revents = 0};
    if (poll(&fd, 1, pollTimeout(timeout)) > 0 && (fd.revents & POLLIN) != 0)
        wl_display_read_events(d);
    else
        wl_display_cancel_read(d);

    wl_display_dispatch_pending(d);
    processRepeats();
    checkConnection();
}

void Window::Impl::checkConnection()
{
    int code = wl_display_get_error(display.get());
    if (code == 0 || error)
        return;
    if (code == EPROTO) {
        const wl_interface* iface = nullptr;
        std::uint32_t id = 0;
        std::uint32_t protocolCode = wl_display_get_protocol_error(display.get(), &iface, &id);
        error = std::format("Wayland protocol error {} on {}@{}", protocolCode, iface ? iface->name : "?", id);
    } else {
        error = std::format("Wayland connection lost: {}", std::strerror(code));
    }
    closeRequested = true;
    push(CloseEvent{});
}

Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;
Window::~Window()
{
    if (impl_) {
        impl_->destroyOffer(impl_->selectionOffer);
        impl_->destroyOffer(impl_->dragOffer);
    }
}

std::expected<Window, std::string> Window::create(const WindowConfig& config)
{
    auto impl = std::make_unique<Impl>(config.size);
    if (auto connected = impl->connect(config); !connected)
        return std::unexpected(connected.error());
    return Window(std::move(impl));
}

void Window::inject(Event event)
{
    impl_->push(std::move(event));
}

void Window::pollEvents()
{
    impl_->input.beginFrame();
    impl_->dispatch(KeyRepeat::Clock::duration::zero());
}

void Window::waitEvents(std::optional<std::chrono::milliseconds> timeout)
{
    impl_->input.beginFrame();
    auto start = KeyRepeat::Clock::now();
    // Some wakeups (pings, configure for an unchanged size) produce no events.
    while (impl_->events.empty() && !impl_->error) {
        std::optional<KeyRepeat::Clock::duration> remaining;
        if (timeout) {
            remaining = *timeout - (KeyRepeat::Clock::now() - start);
            if (*remaining <= KeyRepeat::Clock::duration::zero())
                break;
        }
        impl_->dispatch(remaining);
    }
}

EventQueue& Window::events()
{
    return impl_->events;
}

const InputState& Window::input() const
{
    return impl_->input;
}

bool Window::shouldClose() const
{
    return impl_->closeRequested;
}

void Window::setShouldClose(bool value)
{
    impl_->closeRequested = value;
}

const std::optional<std::string>& Window::error() const
{
    return impl_->error;
}

Extent Window::logicalSize() const
{
    return impl_->geometry.logicalSize();
}

Extent Window::framebufferSize() const
{
    return impl_->geometry.framebufferSize();
}

double Window::scale() const
{
    return impl_->geometry.scale();
}

std::string Window::clipboardText()
{
    Impl& impl = *impl_;
    // Our own selection: reading it back through a pipe would wait on our
    // own send handler.
    if (impl.clipboardSource)
        return impl.clipboardText;
    if (!impl.selectionOffer)
        return {};
    const auto& types = impl.offerTypes[impl.selectionOffer];
    auto type = std::ranges::find_first_of(textMimeTypes, types);
    if (type == textMimeTypes.end())
        return {};
    std::array<int, 2> fds{};
    if (pipe2(fds.data(), O_CLOEXEC) != 0)
        return {};
    wl_data_offer_receive(impl.selectionOffer, std::string(*type).c_str(), fds[1]);
    close(fds[1]);
    wl_display_flush(impl.display.get());
    // The owner writes it and closes; give a stuck one half a second.
    std::string text;
    std::array<char, 4096> chunk{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
        const auto left
            = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd waiting{.fd = fds[0], .events = POLLIN, .revents = 0};
        if (left.count() <= 0 || poll(&waiting, 1, static_cast<int>(left.count())) <= 0)
            break;
        const ssize_t n = read(fds[0], chunk.data(), chunk.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text.append(chunk.data(), static_cast<std::size_t>(n));
    }
    close(fds[0]);
    return text;
}

bool Window::setClipboardText(std::string text)
{
    Impl& impl = *impl_;
    if (!impl.dataDeviceManager || !impl.dataDevice)
        return false;
    impl.clipboardSource.reset(wl_data_device_manager_create_data_source(impl.dataDeviceManager.get()));
    wl_data_source_add_listener(impl.clipboardSource.get(), &dataSourceListener, impl_.get());
    for (std::string_view type : textMimeTypes)
        wl_data_source_offer(impl.clipboardSource.get(), std::string(type).c_str());
    impl.clipboardText = std::move(text);
    wl_data_device_set_selection(impl.dataDevice.get(), impl.clipboardSource.get(), impl.inputSerial);
    wl_display_flush(impl.display.get());
    return true;
}

void Window::setTitle(const std::string& title)
{
    xdg_toplevel_set_title(impl_->toplevel.get(), title.c_str());
}

void Window::setFullscreen(bool fullscreen)
{
    // Takes effect when the compositor's configure arrives.
    if (fullscreen)
        xdg_toplevel_set_fullscreen(impl_->toplevel.get(), nullptr);
    else
        xdg_toplevel_unset_fullscreen(impl_->toplevel.get());
    wl_display_flush(impl_->display.get());
}

bool Window::fullscreen() const
{
    return impl_->fullscreen;
}

bool Window::focused() const
{
    return impl_->focused;
}

bool Window::suspended() const
{
    return impl_->suspended || impl_->unseen;
}

void Window::watchFrames()
{
    // Compositors that don't say so (or not for every way of hiding) still
    // hold back the frame callback while the window isn't shown.
    if (!impl_->frameCallback)
        return;
    const auto waited = std::chrono::steady_clock::now() - impl_->frameRequested;
    if (waited > std::chrono::milliseconds(250))
        impl_->unseen = true;
    // One asked for while hidden may never come (Hyprland drops it), so
    // every second an empty commit asks again; it comes once it's shown.
    if (impl_->unseen && waited > std::chrono::seconds(1)) {
        impl_->frameCallback.reset(wl_surface_frame(impl_->surface.get()));
        wl_callback_add_listener(impl_->frameCallback.get(), &frameDoneListener, impl_.get());
        wl_surface_commit(impl_->surface.get());
        wl_display_flush(impl_->display.get());
        impl_->frameRequested = std::chrono::steady_clock::now();
    }
}

void Window::requestFrameCallback()
{
    if (impl_->frameCallback)
        return;
    impl_->frameCallback.reset(wl_surface_frame(impl_->surface.get()));
    wl_callback_add_listener(impl_->frameCallback.get(), &frameDoneListener, impl_.get());
    impl_->frameRequested = std::chrono::steady_clock::now();
}

bool Window::hasServerDecorations() const
{
    return impl_->serverDecorations;
}

bool Window::setPointerLocked(bool locked)
{
    if (locked && !impl_->pointerConstraints)
        return false;
    impl_->lockRequested = locked;
    impl_->updatePointerLock();
    wl_display_flush(impl_->display.get());
    return true;
}

bool Window::pointerLockRequested() const
{
    return impl_->lockRequested;
}

bool Window::pointerLocked() const
{
    return impl_->locked;
}

NativeHandles Window::nativeHandles() const
{
    return {.display = impl_->display.get(), .surface = impl_->surface.get()};
}

std::expected<void, std::string> Window::fillSolid(std::uint32_t argb)
{
    impl_->solidColor = argb;
    return impl_->drawSolid(argb);
}

} // namespace lunar::platform
