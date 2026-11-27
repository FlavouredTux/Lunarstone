#include "RayScene.hpp"

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace lunar::render {

namespace {

template <class F>
F load(VkDevice device, const char* name)
{
    auto function = reinterpret_cast<F>(vkGetDeviceProcAddr(device, name));
    if (!function)
        throw VulkanError(std::string("no ") + name);
    return function;
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

RayScene::Bottom::Bottom(Bottom&& other) noexcept
    : scene_(other.scene_), storage_(std::move(other.storage_)), handle_(std::exchange(other.handle_, VK_NULL_HANDLE)),
      address_(std::exchange(other.address_, 0))
{
}

RayScene::Bottom& RayScene::Bottom::operator=(Bottom&& other) noexcept
{
    if (this != &other) {
        if (handle_ != VK_NULL_HANDLE)
            scene_->destroy(handle_);
        scene_ = other.scene_;
        storage_ = std::move(other.storage_);
        handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        address_ = std::exchange(other.address_, 0);
    }
    return *this;
}

RayScene::Bottom::~Bottom()
{
    if (handle_ != VK_NULL_HANDLE)
        scene_->destroy(handle_);
}

RayScene::RayScene(Context& context, std::size_t slots) : context_(&context), slots_(slots)
{
    VkDevice device = context.device();
    create_ = load<PFN_vkCreateAccelerationStructureKHR>(device, "vkCreateAccelerationStructureKHR");
    destroy_ = load<PFN_vkDestroyAccelerationStructureKHR>(device, "vkDestroyAccelerationStructureKHR");
    sizes_ = load<PFN_vkGetAccelerationStructureBuildSizesKHR>(device, "vkGetAccelerationStructureBuildSizesKHR");
    build_ = load<PFN_vkCmdBuildAccelerationStructuresKHR>(device, "vkCmdBuildAccelerationStructuresKHR");
    address_
        = load<PFN_vkGetAccelerationStructureDeviceAddressKHR>(device, "vkGetAccelerationStructureDeviceAddressKHR");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 all{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties};
    vkGetPhysicalDeviceProperties2(context.physicalDevice(), &all);
    scratchAlignment_ = std::max<VkDeviceSize>(properties.minAccelerationStructureScratchOffsetAlignment, 1);
}

RayScene::~RayScene()
{
    for (Slot& slot : slots_)
        if (slot.handle != VK_NULL_HANDLE)
            destroy(slot.handle);
}

void RayScene::destroy(VkAccelerationStructureKHR handle) const
{
    destroy_(context_->device(), handle, nullptr);
}

VkDeviceAddress RayScene::bufferAddress(const Buffer& buffer) const
{
    const VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.get()};
    return vkGetBufferDeviceAddress(context_->device(), &info);
}

// Scratch space for one build, aligned as builds need it; the caller keeps
// it as long as the frame's transient buffers.
Buffer RayScene::scratch(VkDeviceSize size, VkDeviceAddress& address) const
{
    Buffer buffer(context_->allocator(), size + scratchAlignment_,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::GpuOnly);
    address = (bufferAddress(buffer) + scratchAlignment_ - 1) / scratchAlignment_ * scratchAlignment_;
    return buffer;
}

VkDeviceAddress RayScene::bottom(const void* key) const
{
    const auto it = bottoms_.find(key);
    return it != bottoms_.end() ? it->second.address() : 0;
}

void RayScene::buildBottom(VkCommandBuffer cmd, const void* key, const Buffer& vertices, std::uint32_t vertexCount,
                           VkDeviceSize vertexStride, const Buffer& indices, std::uint32_t indexCount,
                           std::vector<Buffer>& transient)
{
    VkDevice device = context_->device();
    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = {.triangles = {
                         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                         .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                         .vertexData = {.deviceAddress = bufferAddress(vertices)},
                         .vertexStride = vertexStride,
                         .maxVertex = std::max(vertexCount, 1u) - 1,
                         .indexType = VK_INDEX_TYPE_UINT32,
                         .indexData = {.deviceAddress = bufferAddress(indices)},
                     }},
    };
    VkAccelerationStructureBuildGeometryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const std::uint32_t triangles = indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType
                                                   = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    sizes_(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &triangles, &sizes);

    Bottom bottom;
    bottom.scene_ = this;
    bottom.storage_
        = Buffer(context_->allocator(), sizes.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 MemoryUsage::GpuOnly);
    const VkAccelerationStructureCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = bottom.storage_.get(),
        .size = sizes.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };
    vkCheck(create_(device, &createInfo, nullptr, &bottom.handle_), "vkCreateAccelerationStructureKHR");
    VkDeviceAddress scratchAddress = 0;
    transient.push_back(scratch(sizes.buildScratchSize, scratchAddress));
    info.dstAccelerationStructure = bottom.handle_;
    info.scratchData.deviceAddress = scratchAddress;
    const VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = triangles};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    build_(cmd, 1, &info, &ranges);
    ++stats_.bottomBuilds;
    const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = bottom.handle_,
    };
    bottom.address_ = address_(device, &addressInfo);
    bottoms_.insert_or_assign(key, std::move(bottom));
}

void RayScene::barrierAfterBottoms(VkCommandBuffer cmd) const
{
    memoryBarrier(
        cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

void RayScene::buildTop(VkCommandBuffer cmd, std::size_t index, std::span<const Instance> instances,
                        std::vector<Buffer>& transient)
{
    VkDevice device = context_->device();
    Slot& slot = slots_.at(index);
    slot.replaced = false;
    const auto count = static_cast<std::uint32_t>(instances.size());

    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = {.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR}},
    };
    VkAccelerationStructureBuildGeometryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                 | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };

    // Room for this many instances, in whole doublings so it's rarely remade.
    if (slot.handle == VK_NULL_HANDLE || count > slot.capacity) {
        std::uint32_t capacity = std::max(slot.capacity, 1024u);
        while (capacity < count)
            capacity *= 2;
        if (slot.handle != VK_NULL_HANDLE)
            destroy(slot.handle);
        slot.handle = VK_NULL_HANDLE;
        slot.capacity = capacity;
        slot.instances = Buffer(context_->allocator(), capacity * sizeof(VkAccelerationStructureInstanceKHR),
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                MemoryUsage::Upload);
        VkAccelerationStructureBuildSizesInfoKHR sizes{.sType
                                                       = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        sizes_(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
        slot.storage
            = Buffer(context_->allocator(), sizes.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     MemoryUsage::GpuOnly);
        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = slot.storage.get(),
            .size = sizes.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        vkCheck(create_(device, &createInfo, nullptr, &slot.handle), "vkCreateAccelerationStructureKHR");
        slot.replaced = true;
    }

    // A refit needs the tree the slot built last, of as many instances, and
    // is only as good as the instances are where they were then.
    bool refit = !slot.replaced && slot.refits < maxRefits && slot.placed.size() == count;
    slot.placed.resize(count);

    // Row-major 3x4 transforms, from the column-major models.
    std::byte* out = slot.instances.mapped().data();
    for (std::uint32_t i = 0; i < count; ++i) {
        const Instance& in = instances[i];
        VkAccelerationStructureInstanceKHR instance{};
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 4; ++c)
                instance.transform.matrix[r][c] = in.model.m[c][r];
        instance.instanceCustomIndex = in.custom & 0xffffffu;
        instance.mask = in.mask;
        // Facing culling stays on where it can: only rays that ask for it
        // cull (local lights' shadow rays), the rest see both sides.
        instance.flags
            = (in.opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR)
              | (in.cullable ? VkGeometryInstanceFlagsKHR{0}
                             : VkGeometryInstanceFlagsKHR{VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR});
        instance.accelerationStructureReference = in.bottom;
        std::memcpy(out + std::size_t{i} * sizeof(instance), &instance, sizeof(instance));

        Slot::Placed& placed = slot.placed[i];
        const std::uint32_t seen = std::uint32_t{instance.flags} << 8 | std::uint32_t{instance.mask};
        const float at[3] = {in.model.m[3][0], in.model.m[3][1], in.model.m[3][2]};
        if (refit) {
            const float dx = at[0] - placed.at[0], dy = at[1] - placed.at[1], dz = at[2] - placed.at[2];
            refit = placed.bottom == in.bottom && placed.seen == seen
                    && dx * dx + dy * dy + dz * dz <= refitReach * refitReach;
        }
        placed = {in.bottom, seen, {at[0], at[1], at[2]}};
    }
    if (refit) {
        // In place, from the slot's own last build.
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        info.srcAccelerationStructure = slot.handle;
        ++slot.refits;
        ++stats_.topRefits;
    } else {
        slot.refits = 0;
    }

    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType
                                                   = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    sizes_(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    VkDeviceAddress scratchAddress = 0;
    transient.push_back(scratch(refit ? sizes.updateScratchSize : sizes.buildScratchSize, scratchAddress));
    geometry.geometry.instances.data.deviceAddress = bufferAddress(slot.instances);
    info.dstAccelerationStructure = slot.handle;
    info.scratchData.deviceAddress = scratchAddress;
    const VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = count};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    build_(cmd, 1, &info, &ranges);
    ++stats_.topBuilds;
    stats_.topInstances = count;
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

RayScene::Stats RayScene::takeStats()
{
    const Stats taken = stats_;
    stats_ = {};
    return taken;
}

} // namespace lunar::render
