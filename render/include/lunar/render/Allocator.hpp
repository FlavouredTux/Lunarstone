#pragma once

#include <lunar/render/Vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace lunar::render {

// First-fit sub-allocator over [0, capacity). Pure bookkeeping, no Vulkan.
class RangeAllocator {
public:
    explicit RangeAllocator(VkDeviceSize capacity);

    // Offset of a free range of size bytes aligned to alignment (a power of two).
    std::optional<VkDeviceSize> allocate(VkDeviceSize size, VkDeviceSize alignment);
    // Gives back exactly what allocate handed out; neighbours merge.
    void free(VkDeviceSize offset, VkDeviceSize size);

    VkDeviceSize capacity() const { return capacity_; }
    VkDeviceSize used() const { return used_; }
    std::size_t freeRangeCount() const { return free_.size(); }

private:
    VkDeviceSize capacity_;
    VkDeviceSize used_ = 0;
    std::map<VkDeviceSize, VkDeviceSize> free_; // offset -> size
};

enum class MemoryUsage : std::uint8_t {
    GpuOnly,  // device local, never mapped
    Upload,   // host visible and coherent, written by the CPU
    Stream,   // like Upload, but preferably device local too: written each frame, read a lot by the GPU
    Readback, // host visible and coherent, preferably cached, read by the CPU
};

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    std::byte* mapped = nullptr; // null unless host visible
    std::uint32_t block = 0;     // internal, which block or dedicated slot
    bool dedicated = false;
};

// How much device memory the renderer should use. The driver's numbers come
// from VK_EXT_memory_budget when the device has it.
struct MemoryBudget {
    VkDeviceSize heapSize = 0;  // the largest device-local heap
    VkDeviceSize available = 0; // of it, what the driver lets this process use (other programs' use left out)
    VkDeviceSize used = 0;      // what this process uses of it, by the driver's count
    VkDeviceSize allocated = 0; // what this allocator holds of it
    VkDeviceSize target = 0;    // what the allocator's device-local memory should stay under
    bool fromDriver = false;    // available and used are the driver's, not guesses
    bool overridden = false;    // LUNAR_GPU_BUDGET_MB set target
};

// The budget for a heap the driver reports on: what this process has plus
// what's still free, less some headroom for others and the driver; without
// the driver's numbers half the heap. overrideMb (LUNAR_GPU_BUDGET_MB) wins.
VkDeviceSize budgetTarget(const MemoryBudget& budget, std::optional<std::uint64_t> overrideMb);
// LUNAR_GPU_BUDGET_MB as a positive number of MiB, if set.
std::optional<std::uint64_t> budgetOverride();

// Device memory allocator: carves buffers and images out of large blocks per
// memory type, and gives oversized requests a VkDeviceMemory of their own.
// Linear (buffer) and optimal (image) resources live in separate blocks so
// bufferImageGranularity never matters. Host-visible blocks stay mapped.
//
// When the device is full a request gets memory of its exact size instead of
// a new block, then, for device-local requests, memory of another type the
// resource allows (system memory: slower to read, but drawing goes on).
// Only when nothing is left does it throw OutOfMemoryError.
class Allocator {
public:
    static constexpr VkDeviceSize blockSize = VkDeviceSize{64} << 20;

    // deviceAddresses: every block can back buffers that shaders and
    // acceleration structure builds address (bufferDeviceAddress is on).
    Allocator(VkPhysicalDevice physicalDevice, VkDevice device, bool memoryBudget = false,
              bool deviceAddresses = false);
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    ~Allocator();

    // Throws OutOfMemoryError when nothing fits, VulkanError if no memory type does.
    Allocation allocate(const VkMemoryRequirements& requirements, MemoryUsage usage, bool linear);
    void free(const Allocation& allocation);

    VkDevice device() const { return device_; }
    std::size_t blockCount() const;
    // Device memory held, blocks and dedicated allocations, of every type.
    VkDeviceSize allocatedBytes() const;
    // The part of it in the largest device-local heap.
    VkDeviceSize deviceLocalBytes() const { return heapBytes_[deviceHeap_]; }
    // Device-local requests that had to go to other memory since the start.
    std::uint64_t fallbackCount() const { return fallbacks_; }
    // Asks the driver (a cheap call) and works out the target.
    MemoryBudget budget() const;

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint32_t memoryType = 0;
        VkDeviceSize size = 0;
        bool linear = false;
        std::byte* mapped = nullptr;
        RangeAllocator ranges{0}; // empty for dedicated memory
    };

    // Memory types allowed and fitting usage, best first.
    std::vector<std::uint32_t> memoryTypes(std::uint32_t allowedTypes, MemoryUsage usage) const;
    // Empty memory when the device or host is out of it; throws on other errors.
    Block allocateMemory(std::uint32_t memoryType, VkDeviceSize size);
    std::optional<Allocation> allocateFrom(std::uint32_t memoryType, const VkMemoryRequirements& requirements,
                                           bool linear, bool exactOnly);
    Allocation dedicate(Block block, VkDeviceSize size);
    void releaseEmptyBlocks();
    void count(const Block& block, bool add);

    VkPhysicalDevice physicalDevice_;
    VkDevice device_;
    bool memoryBudget_ = false;
    bool deviceAddresses_ = false;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::uint32_t deviceHeap_ = 0;
    std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heapBytes_{};
    std::uint64_t fallbacks_ = 0;
    std::vector<Block> blocks_; // empty memory marks a freed slot
    std::vector<Block> dedicated_;
};

// A VkBuffer and its memory.
class Buffer {
public:
    Buffer() = default;
    Buffer(Allocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory);
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    VkBuffer get() const { return buffer_.get(); }
    VkDeviceSize size() const { return size_; }
    // Device memory it holds.
    VkDeviceSize memorySize() const { return allocation_.size; }
    // Host-visible buffers only.
    std::span<std::byte> mapped() const;

private:
    Allocator* allocator_ = nullptr;
    UniqueBuffer buffer_;
    Allocation allocation_;
    VkDeviceSize size_ = 0;
};

// A 2D VkImage with one layer and mipLevels mip levels, its memory and a
// view of every level; or a 3D one with a single level.
class Image {
public:
    Image() = default;
    // flags: VkImageCreateFlags, e.g. VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT for
    // extra views in other formats.
    Image(Allocator& allocator, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
          std::uint32_t mipLevels = 1, VkImageCreateFlags flags = 0);
    // A 3D colour image with one level, viewed as such.
    static Image volume(Allocator& allocator, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    VkImage get() const { return image_.get(); }
    VkImageView view() const { return view_.get(); }
    VkExtent2D extent() const { return extent_; }
    std::uint32_t depth() const { return depth_; } // 1 for 2D images
    VkFormat format() const { return format_; }
    std::uint32_t mipLevels() const { return mipLevels_; }
    // Device memory it holds.
    VkDeviceSize memorySize() const { return allocation_.size; }

private:
    void release();
    void createVolume(VkImageUsageFlags usage);

    Allocator* allocator_ = nullptr;
    UniqueImage image_;
    UniqueImageView view_;
    Allocation allocation_;
    VkExtent2D extent_{};
    std::uint32_t depth_ = 1;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t mipLevels_ = 1;
};

} // namespace lunar::render
