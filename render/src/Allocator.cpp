#include <lunar/render/Allocator.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <format>
#include <iterator>
#include <ranges>
#include <string_view>
#include <system_error>

namespace lunar::render {

namespace {

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

// --- RangeAllocator ------------------------------------------------------------

RangeAllocator::RangeAllocator(VkDeviceSize capacity) : capacity_(capacity)
{
    if (capacity > 0)
        free_.emplace(0, capacity);
}

std::optional<VkDeviceSize> RangeAllocator::allocate(VkDeviceSize size, VkDeviceSize alignment)
{
    if (size == 0 || !std::has_single_bit(alignment))
        return std::nullopt;

    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const auto [start, length] = *it;
        const VkDeviceSize offset = alignUp(start, alignment);
        const VkDeviceSize padding = offset - start;
        if (padding + size > length)
            continue;

        free_.erase(it);
        if (padding > 0)
            free_.emplace(start, padding);
        if (padding + size < length)
            free_.emplace(offset + size, length - padding - size);
        used_ += size;
        return offset;
    }
    return std::nullopt;
}

void RangeAllocator::free(VkDeviceSize offset, VkDeviceSize size)
{
    auto [it, inserted] = free_.emplace(offset, size);
    if (!inserted)
        return;
    used_ -= size;

    if (auto next = std::next(it); next != free_.end() && offset + size == next->first) {
        it->second += next->second;
        free_.erase(next);
    }
    if (it != free_.begin()) {
        if (auto prev = std::prev(it); prev->first + prev->second == offset) {
            prev->second += it->second;
            free_.erase(it);
        }
    }
}

// --- Budget --------------------------------------------------------------------

std::optional<std::uint64_t> budgetOverride()
{
    const char* value = std::getenv("LUNAR_GPU_BUDGET_MB");
    if (!value || *value == '\0')
        return std::nullopt;
    std::uint64_t mb = 0;
    const std::string_view text(value);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), mb);
    if (error != std::errc() || end != text.data() + text.size() || mb == 0)
        return std::nullopt;
    return mb;
}

VkDeviceSize budgetTarget(const MemoryBudget& budget, std::optional<std::uint64_t> overrideMb)
{
    if (overrideMb)
        return VkDeviceSize{*overrideMb} << 20;
    if (!budget.fromDriver)
        return budget.heapSize / 2; // GUESS: without the driver's numbers, share the device evenly
    // What others (and the driver for us: swapchain, pipelines) hold stays theirs.
    const VkDeviceSize others = budget.used > budget.allocated ? budget.used - budget.allocated : 0;
    const VkDeviceSize ours = budget.available > others ? budget.available - others : 0;
    // GUESS: a sixteenth of the heap (at least 96 MiB) kept free, so other
    // programs growing a little doesn't push anyone out of memory at once.
    const VkDeviceSize headroom = std::max(budget.heapSize / 16, VkDeviceSize{96} << 20);
    return ours > headroom ? ours - headroom : 0;
}

// --- Allocator -----------------------------------------------------------------

Allocator::Allocator(VkPhysicalDevice physicalDevice, VkDevice device, bool memoryBudget, bool deviceAddresses)
    : physicalDevice_(physicalDevice), device_(device), memoryBudget_(memoryBudget), deviceAddresses_(deviceAddresses)
{
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties_);
    VkDeviceSize largest = 0;
    for (std::uint32_t i = 0; i < memoryProperties_.memoryHeapCount; ++i) {
        const VkMemoryHeap& heap = memoryProperties_.memoryHeaps[i];
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap.size > largest) {
            largest = heap.size;
            deviceHeap_ = i;
        }
    }
}

Allocator::~Allocator()
{
    for (const Block& block : blocks_)
        if (block.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, block.memory, nullptr);
    for (const Block& block : dedicated_)
        if (block.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, block.memory, nullptr);
}

std::size_t Allocator::blockCount() const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(blocks_, [](const Block& block) { return block.memory != VK_NULL_HANDLE; }));
}

VkDeviceSize Allocator::allocatedBytes() const
{
    VkDeviceSize total = 0;
    for (const std::vector<Block>* list : {&blocks_, &dedicated_})
        for (const Block& block : *list)
            if (block.memory != VK_NULL_HANDLE)
                total += block.size;
    return total;
}

MemoryBudget Allocator::budget() const
{
    MemoryBudget budget;
    budget.heapSize = memoryProperties_.memoryHeaps[deviceHeap_].size;
    budget.allocated = deviceLocalBytes();
    budget.available = budget.heapSize;
    budget.used = budget.allocated;
    if (memoryBudget_) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT driver{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                     .pNext = &driver};
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice_, &properties);
        if (driver.heapBudget[deviceHeap_] > 0) {
            budget.available = std::min(driver.heapBudget[deviceHeap_], budget.heapSize);
            budget.used = driver.heapUsage[deviceHeap_];
            budget.fromDriver = true;
        }
    }
    const std::optional<std::uint64_t> forced = budgetOverride();
    budget.overridden = forced.has_value();
    budget.target = budgetTarget(budget, forced);
    return budget;
}

std::vector<std::uint32_t> Allocator::memoryTypes(std::uint32_t allowedTypes, MemoryUsage usage) const
{
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    switch (usage) {
    case MemoryUsage::GpuOnly:
        preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryUsage::Upload:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case MemoryUsage::Stream:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryUsage::Readback:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    }

    // Preferred types first, then the rest, each in the driver's order.
    std::vector<std::uint32_t> types;
    for (const bool wantPreferred : {true, false}) {
        for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[i].propertyFlags;
            if ((allowedTypes & (1u << i)) == 0 || (flags & required) != required)
                continue;
            if (((flags & preferred) == preferred) == wantPreferred)
                types.push_back(i);
        }
    }
    if (types.empty())
        throw VulkanError(std::format("no memory type fits (allowed types {:#x})", allowedTypes));
    return types;
}

void Allocator::count(const Block& block, bool add)
{
    VkDeviceSize& bytes = heapBytes_[memoryProperties_.memoryTypes[block.memoryType].heapIndex];
    bytes = add ? bytes + block.size : bytes - block.size;
}

Allocator::Block Allocator::allocateMemory(std::uint32_t memoryType, VkDeviceSize size)
{
    const VkMemoryAllocateFlagsInfo addressed{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = deviceAddresses_ ? &addressed : nullptr,
        .allocationSize = size,
        .memoryTypeIndex = memoryType,
    };
    Block block;
    block.memoryType = memoryType;
    block.size = size;
    const VkResult result = vkAllocateMemory(device_, &info, nullptr, &block.memory);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
        return {};
    vkCheck(result, "vkAllocateMemory");

    if (memoryProperties_.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* data = nullptr;
        if (VkResult mapped = vkMapMemory(device_, block.memory, 0, VK_WHOLE_SIZE, 0, &data); mapped != VK_SUCCESS) {
            vkFreeMemory(device_, block.memory, nullptr);
            if (mapped == VK_ERROR_OUT_OF_DEVICE_MEMORY || mapped == VK_ERROR_OUT_OF_HOST_MEMORY
                || mapped == VK_ERROR_MEMORY_MAP_FAILED)
                return {};
            vkCheck(mapped, "vkMapMemory");
        }
        block.mapped = static_cast<std::byte*>(data);
    }
    count(block, true);
    return block;
}

Allocation Allocator::dedicate(Block block, VkDeviceSize size)
{
    Allocation allocation{block.memory, 0, size, block.mapped, 0, true};
    auto slot = std::ranges::find(dedicated_, VkDeviceMemory{VK_NULL_HANDLE}, &Block::memory);
    allocation.block = static_cast<std::uint32_t>(slot - dedicated_.begin());
    if (slot == dedicated_.end())
        dedicated_.push_back(std::move(block));
    else
        *slot = std::move(block);
    return allocation;
}

std::optional<Allocation> Allocator::allocateFrom(std::uint32_t memoryType, const VkMemoryRequirements& requirements,
                                                  bool linear, bool exactOnly)
{
    if (requirements.size > blockSize / 2 || exactOnly) {
        Block block = allocateMemory(memoryType, requirements.size);
        if (block.memory == VK_NULL_HANDLE)
            return std::nullopt;
        return dedicate(std::move(block), requirements.size);
    }

    auto tryBlock = [&](std::uint32_t index) -> std::optional<Allocation> {
        Block& block = blocks_[index];
        if (block.memory == VK_NULL_HANDLE || block.memoryType != memoryType || block.linear != linear)
            return std::nullopt;
        auto offset = block.ranges.allocate(requirements.size, requirements.alignment);
        if (!offset)
            return std::nullopt;
        return Allocation{block.memory, *offset, requirements.size, block.mapped ? block.mapped + *offset : nullptr,
                          index,        false};
    };

    for (std::uint32_t i = 0; i < blocks_.size(); ++i)
        if (auto allocation = tryBlock(i))
            return allocation;

    Block block = allocateMemory(memoryType, blockSize);
    if (block.memory == VK_NULL_HANDLE) {
        // No room for a whole block: memory of just this size may still fit.
        Block exact = allocateMemory(memoryType, requirements.size);
        if (exact.memory == VK_NULL_HANDLE)
            return std::nullopt;
        return dedicate(std::move(exact), requirements.size);
    }
    block.linear = linear;
    block.ranges = RangeAllocator(blockSize);
    auto slot = std::ranges::find(blocks_, VkDeviceMemory{VK_NULL_HANDLE}, &Block::memory);
    const auto index = static_cast<std::uint32_t>(slot - blocks_.begin());
    if (slot == blocks_.end())
        blocks_.push_back(std::move(block));
    else
        *slot = std::move(block);
    auto allocation = tryBlock(index);
    if (!allocation)
        throw VulkanError(std::format("{} bytes don't fit a fresh memory block", requirements.size));
    return allocation;
}

void Allocator::releaseEmptyBlocks()
{
    for (Block& block : blocks_) {
        if (block.memory != VK_NULL_HANDLE && block.ranges.used() == 0) {
            count(block, false);
            vkFreeMemory(device_, block.memory, nullptr);
            block = {};
        }
    }
}

Allocation Allocator::allocate(const VkMemoryRequirements& requirements, MemoryUsage usage, bool linear)
{
    const std::vector<std::uint32_t> types = memoryTypes(requirements.memoryTypeBits, usage);
    const auto preferred = [&](std::uint32_t type) {
        const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[type].propertyFlags;
        return usage == MemoryUsage::GpuOnly ? (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 : type == types[0];
    };
    if (auto allocation = allocateFrom(types[0], requirements, linear, false))
        return *allocation;
    // Full: the empty blocks kept around for later go first.
    releaseEmptyBlocks();
    if (auto allocation = allocateFrom(types[0], requirements, linear, true))
        return *allocation;
    for (std::uint32_t type : types | std::views::drop(1)) {
        if (auto allocation = allocateFrom(type, requirements, linear, false)) {
            if (!preferred(type))
                ++fallbacks_;
            return *allocation;
        }
    }
    throw OutOfMemoryError(std::format("vkAllocateMemory failed: out of memory for {} KiB", requirements.size >> 10));
}

void Allocator::free(const Allocation& allocation)
{
    if (allocation.memory == VK_NULL_HANDLE)
        return;

    if (allocation.dedicated) {
        Block& block = dedicated_[allocation.block];
        count(block, false);
        vkFreeMemory(device_, block.memory, nullptr);
        block = {};
        return;
    }

    Block& block = blocks_[allocation.block];
    block.ranges.free(allocation.offset, allocation.size);
    // Keep one empty block per kind around; release any extra.
    if (block.ranges.used() == 0) {
        const bool another = std::ranges::any_of(blocks_, [&](const Block& other) {
            return &other != &block && other.memory != VK_NULL_HANDLE && other.memoryType == block.memoryType
                   && other.linear == block.linear;
        });
        if (another) {
            count(block, false);
            vkFreeMemory(device_, block.memory, nullptr);
            block = {};
        }
    }
}

// --- Buffer --------------------------------------------------------------------

Buffer::Buffer(Allocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory)
    : allocator_(&allocator), size_(size)
{
    VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCheck(vkCreateBuffer(allocator.device(), &info, nullptr, &buffer), "vkCreateBuffer");
    buffer_ = UniqueBuffer(allocator.device(), buffer);

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(allocator.device(), buffer, &requirements);
    allocation_ = allocator.allocate(requirements, memory, true);
    if (VkResult result = vkBindBufferMemory(allocator.device(), buffer, allocation_.memory, allocation_.offset);
        result != VK_SUCCESS) {
        allocator.free(std::exchange(allocation_, {}));
        vkCheck(result, "vkBindBufferMemory");
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(other.allocator_), buffer_(std::move(other.buffer_)),
      allocation_(std::exchange(other.allocation_, {})), size_(std::exchange(other.size_, 0))
{
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this != &other) {
        buffer_.reset();
        if (allocator_)
            allocator_->free(allocation_);
        allocator_ = other.allocator_;
        buffer_ = std::move(other.buffer_);
        allocation_ = std::exchange(other.allocation_, {});
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

Buffer::~Buffer()
{
    buffer_.reset();
    if (allocator_)
        allocator_->free(allocation_);
}

std::span<std::byte> Buffer::mapped() const
{
    if (!allocation_.mapped)
        return {};
    return {allocation_.mapped, static_cast<std::size_t>(size_)};
}

// --- Image ---------------------------------------------------------------------

Image::Image(Allocator& allocator, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
             VkImageAspectFlags aspect, std::uint32_t mipLevels, VkImageCreateFlags flags)
    : allocator_(&allocator), extent_(extent), format_(format), mipLevels_(mipLevels)
{
    VkDevice device = allocator.device();
    VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = mipLevels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    vkCheck(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage");
    image_ = UniqueImage(device, image);

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    allocation_ = allocator.allocate(requirements, MemoryUsage::GpuOnly, false);
    try {
        vkCheck(vkBindImageMemory(device, image, allocation_.memory, allocation_.offset), "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {aspect, 0, mipLevels, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        view_ = UniqueImageView(device, view);
    } catch (...) {
        release();
        throw;
    }
}

Image Image::volume(Allocator& allocator, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage)
{
    Image out;
    out.allocator_ = &allocator;
    out.extent_ = {extent.width, extent.height};
    out.depth_ = extent.depth;
    out.format_ = format;
    out.createVolume(usage);
    return out;
}

void Image::createVolume(VkImageUsageFlags usage)
{
    Allocator& allocator = *allocator_;
    const VkExtent3D extent{extent_.width, extent_.height, depth_};
    const VkFormat format = format_;
    VkDevice device = allocator.device();
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_3D,
        .format = format,
        .extent = extent,
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    vkCheck(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage");
    image_ = UniqueImage(device, image);

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    allocation_ = allocator.allocate(requirements, MemoryUsage::GpuOnly, false);
    try {
        vkCheck(vkBindImageMemory(device, image, allocation_.memory, allocation_.offset), "vkBindImageMemory");
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_3D,
            .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        view_ = UniqueImageView(device, view);
    } catch (...) {
        release();
        throw;
    }
}

Image::Image(Image&& other) noexcept
    : allocator_(other.allocator_), image_(std::move(other.image_)), view_(std::move(other.view_)),
      allocation_(std::exchange(other.allocation_, {})), extent_(other.extent_), depth_(other.depth_),
      format_(other.format_), mipLevels_(other.mipLevels_)
{
}

Image& Image::operator=(Image&& other) noexcept
{
    if (this != &other) {
        release();
        allocator_ = other.allocator_;
        image_ = std::move(other.image_);
        view_ = std::move(other.view_);
        allocation_ = std::exchange(other.allocation_, {});
        extent_ = other.extent_;
        depth_ = other.depth_;
        format_ = other.format_;
        mipLevels_ = other.mipLevels_;
    }
    return *this;
}

Image::~Image()
{
    release();
}

void Image::release()
{
    view_.reset();
    image_.reset();
    if (allocator_)
        allocator_->free(std::exchange(allocation_, {}));
}

} // namespace lunar::render
