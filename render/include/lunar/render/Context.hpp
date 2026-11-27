#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lunar::render {

struct ContextOptions {
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true;
#endif
    // Extra instance extensions, e.g. VK_KHR_surface and VK_KHR_wayland_surface.
    std::vector<const char*> instanceExtensions;
    // Presentation hook: called once the instance exists. The context owns the
    // returned surface, picks a queue that can present to it and enables
    // VK_KHR_swapchain. Leave empty to render offscreen only.
    std::function<std::expected<VkSurfaceKHR, std::string>(VkInstance)> createSurface;
    // Turn on ray queries (acceleration structures, buffer device addresses)
    // when the device has them; rayTracing() says whether it did.
    bool rayTracing = false;
};

// Instance, device and graphics queue, plus the shared memory allocator and a
// way to run one-off command buffers. Needs Vulkan 1.3 (dynamic rendering and
// synchronization2) and non-uniform indexing into partially bound texture
// arrays. Nothing here needs a window.
class Context {
public:
    // Fails when there is no usable Vulkan device; callers that can live
    // without rendering (tests) treat that as "skip".
    static std::expected<std::unique_ptr<Context>, std::string> create(const ContextOptions& options = {});

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queueFamily() const { return queueFamily_; }
    VkSurfaceKHR surface() const { return surface_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    // Whether samplers may filter anisotropically (samplerAnisotropy is on).
    bool anisotropy() const { return anisotropy_; }
    // Whether BC1-BC7 textures can be sampled (textureCompressionBC is on).
    bool textureCompressionBC() const { return textureCompressionBC_; }
    // Whether swapchain images can be viewed in another format
    // (VK_KHR_swapchain_mutable_format), so overlays can blend in sRGB space.
    bool swapchainMutableFormat() const { return swapchainMutableFormat_; }
    // Whether ray queries are on (asked for, and the device has them).
    bool rayTracing() const { return rayTracing_; }
    // Whether the device could ray trace (ray queries), asked for or not.
    bool rayTracingSupported() const { return rayTracingSupported_; }
    // Whether the graphics queue can write timestamps, which is what a
    // frame's own GPU time is measured with.
    bool timestampQueries() const
    {
        return queueTimestampValidBits_ != 0 && properties_.limits.timestampComputeAndGraphics == VK_TRUE;
    }
    // What one timestamp tick is worth in nanoseconds, for turning the gap
    // between two of them into milliseconds.
    double timestampNanoseconds() const { return properties_.limits.timestampPeriod; }
    Allocator& allocator() { return *allocator_; }

    bool validationEnabled() const { return messenger_ != VK_NULL_HANDLE; }
    // Validation-layer errors seen since the last call, oldest first. All
    // warnings and errors are printed to stderr as they happen as well.
    std::vector<std::string> takeValidationErrors();

    // Records a command buffer with record, submits it and waits for the GPU.
    void submitAndWait(const std::function<void(VkCommandBuffer)>& record);
    void waitIdle() const;

private:
    Context() = default;
    void createInstance(const ContextOptions& options);
    void pickPhysicalDevice();
    static bool supportsRayQueries(VkPhysicalDevice device);
    void createDevice();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    bool anisotropy_ = false;
    bool textureCompressionBC_ = false;
    bool swapchainMutableFormat_ = false;
    bool rayTracingWanted_ = false;
    bool rayTracingSupported_ = false; // of the picked device
    bool rayTracing_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    std::uint32_t queueTimestampValidBits_ = 0; // of that family: no timestamps without it
    std::unique_ptr<Allocator> allocator_;
    UniqueCommandPool commandPool_;
    UniqueFence fence_;

    std::mutex validationMutex_;
    std::vector<std::string> validationErrors_;
};

} // namespace lunar::render
