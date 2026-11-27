#pragma once

#include <lunar/render/Image.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar::render {

class Context;

using UniqueSwapchain = DeviceHandle<VkSwapchainKHR, vkDestroySwapchainKHR>;

struct PresentOptions {
    bool vsync = true; // FIFO; otherwise MAILBOX (or IMMEDIATE) when the surface has it
    // Swapchain images that can be copied from, for screenshots
    // (captureNextFrame). Off unless asked: some drivers keep such images
    // uncompressed.
    bool readable = false;
};

// Draws frames into a swapchain on the context's surface. The window owns the
// size: on Wayland the surface has none until the first buffer arrives, so
// pass the window's size in pixels at creation and to resize().
class Presenter {
public:
    static constexpr std::uint32_t framesInFlight = 2;

    // context must have been created with a surface (ContextOptions::createSurface).
    static std::expected<std::unique_ptr<Presenter>, std::string> create(Context& context, VkExtent2D windowSize,
                                                                         const PresentOptions& options = {});

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    ~Presenter();

    // The window changed size; the swapchain is rebuilt before the next frame.
    void resize(VkExtent2D windowSize);

    enum class FrameResult : std::uint8_t {
        Presented,
        Skipped, // swapchain out of date or zero sized; rebuilt, try again next frame
    };
    // Waits for a free frame slot, renders and queues the image for presentation.
    FrameResult drawFrame(const Camera& camera, std::span<const DrawItem> items, const Lighting& lighting = {});

    // Screenshots: the next frame drawFrame presents is copied back as it
    // goes to the screen (sRGB encoded), and takeCapture hands it over once.
    // That frame waits for the GPU. Nothing comes back without
    // PresentOptions::readable or when the surface can't be copied from.
    void captureNextFrame() { captureRequested_ = true; }
    std::optional<PixelImage> takeCapture();

    // Where drawFrame's time went, in milliseconds summed over `frames`: the wait for
    // a free frame slot (the GPU still on the frame before last), the swapchain's
    // next image (vsync), recording the commands, queueing them and presenting.
    struct Timings {
        double fenceWait = 0, acquire = 0, record = 0, submit = 0, present = 0;
        int frames = 0;
    };
    Timings takeTimings() { return std::exchange(timings_, {}); }

    MeshRenderer& renderer() { return *renderer_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat format() const { return format_; }
    VkPresentModeKHR presentMode() const { return presentMode_; }

private:
    struct Frame {
        UniqueCommandPool pool;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        UniqueSemaphore imageAcquired;
        UniqueFence done;
    };
    struct SwapchainImage {
        VkImage image = VK_NULL_HANDLE;
        UniqueImageView view;
        UniqueImageView unormView;  // with a mutable-format swapchain, for overlays
        UniqueSemaphore renderDone; // per image: reused only once the image comes back
    };

    Presenter(Context& context, const PresentOptions& options) : context_(&context), options_(options) {}
    void createFrames();
    void chooseFormat();
    void rebuildSwapchain();
    // Records the copy of a presented image into capture_ (it ends up back
    // in PRESENT_SRC).
    void recordCapture(VkCommandBuffer cmd, VkImage image);
    void readCapture();

    Context* context_;
    PresentOptions options_;
    VkExtent2D windowSize_{};
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkFormat unormFormat_ = VK_FORMAT_UNDEFINED; // format_'s UNORM twin when the swapchain is mutable
    bool blitTarget_ = false;                    // the swapchain's images take blits
    bool readable_ = false;                      // they can be copied from (screenshots)
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool outdated_ = true;

    UniqueSwapchain swapchain_;
    std::vector<SwapchainImage> images_;
    Frame frames_[framesInFlight];
    Timings timings_;
    std::uint32_t frameIndex_ = 0;
    std::unique_ptr<MeshRenderer> renderer_;
    bool captureRequested_ = false;
    Buffer capture_; // the copied frame, host visible
    VkExtent2D captureExtent_{};
    std::optional<PixelImage> captured_;
};

} // namespace lunar::render
