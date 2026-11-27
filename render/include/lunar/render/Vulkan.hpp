#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace lunar::render {

// A Vulkan call failed after setup (device lost, out of memory, ...).
class VulkanError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The device or the host ran out of memory (VK_ERROR_OUT_OF_DEVICE_MEMORY,
// VK_ERROR_OUT_OF_HOST_MEMORY). Callers that can do without what they were
// making (a texture, a mesh) catch it, free something and try again later.
class OutOfMemoryError : public VulkanError {
public:
    using VulkanError::VulkanError;
};

std::string_view toString(VkResult result);

// Throws VulkanError naming what failed unless result is VK_SUCCESS;
// OutOfMemoryError when it ran out of memory.
void vkCheck(VkResult result, std::string_view what);

// Owns a handle that belongs to a VkDevice and is freed with Destroy.
template <class T, auto Destroy>
class DeviceHandle {
public:
    DeviceHandle() = default;
    DeviceHandle(VkDevice device, T handle) : device_(device), handle_(handle) {}
    DeviceHandle(DeviceHandle&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }
    DeviceHandle& operator=(DeviceHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }
    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;
    ~DeviceHandle() { reset(); }

    T get() const { return handle_; }
    explicit operator bool() const { return handle_ != VK_NULL_HANDLE; }

    void reset()
    {
        if (handle_ != VK_NULL_HANDLE)
            Destroy(device_, std::exchange(handle_, VK_NULL_HANDLE), nullptr);
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    T handle_ = VK_NULL_HANDLE;
};

using UniqueBuffer = DeviceHandle<VkBuffer, vkDestroyBuffer>;
using UniqueImage = DeviceHandle<VkImage, vkDestroyImage>;
using UniqueImageView = DeviceHandle<VkImageView, vkDestroyImageView>;
using UniqueShaderModule = DeviceHandle<VkShaderModule, vkDestroyShaderModule>;
using UniquePipelineLayout = DeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout>;
using UniquePipeline = DeviceHandle<VkPipeline, vkDestroyPipeline>;
using UniqueCommandPool = DeviceHandle<VkCommandPool, vkDestroyCommandPool>;
using UniqueFence = DeviceHandle<VkFence, vkDestroyFence>;
using UniqueSemaphore = DeviceHandle<VkSemaphore, vkDestroySemaphore>;
using UniqueDescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
using UniqueDescriptorPool = DeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool>;
using UniqueSampler = DeviceHandle<VkSampler, vkDestroySampler>;
using UniqueQueryPool = DeviceHandle<VkQueryPool, vkDestroyQueryPool>;

} // namespace lunar::render
