#pragma once

// Acceleration structures for ray queries (Context::rayTracing): a bottom
// level per mesh, built in the frame that uploads it, and a top level of
// the frame's instances, built every frame.

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Mat4.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar::render {

class Context;

class RayScene {
public:
    // A mesh's bottom level; empty until built.
    class Bottom {
    public:
        Bottom() = default;
        Bottom(Bottom&& other) noexcept;
        Bottom& operator=(Bottom&& other) noexcept;
        Bottom(const Bottom&) = delete;
        Bottom& operator=(const Bottom&) = delete;
        ~Bottom();

        bool built() const { return handle_ != VK_NULL_HANDLE; }
        VkDeviceAddress address() const { return address_; }

    private:
        friend class RayScene;
        const RayScene* scene_ = nullptr;
        Buffer storage_;
        VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
        VkDeviceAddress address_ = 0;
    };

    // One instance of the frame's top level.
    struct Instance {
        Mat4 model;
        VkDeviceAddress bottom = 0;
        std::uint32_t custom = 0; // what shaders read as the instance custom index (24 bits)
        bool opaque = true;       // false: hits go to the shader's alpha test
        std::uint8_t mask = 0xff; // which rays see it (the shader's ray masks)
        // Whether rays that cull a facing (local lights' shadow rays) may
        // skip its faces; false for surfaces with no inside, like terrain.
        bool cullable = true;
    };

    RayScene(Context& context, std::size_t slots);
    RayScene(const RayScene&) = delete;
    RayScene& operator=(const RayScene&) = delete;
    ~RayScene();

    // Builds the bottom level of a mesh (by key) from its first indexCount
    // indices (its full level), after barriers that make the copies that
    // filled the buffers visible. Scratch goes to transient, which must
    // outlive the frame. The buffers need
    // ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY and SHADER_DEVICE_ADDRESS
    // usage. Throws OutOfMemoryError.
    void buildBottom(VkCommandBuffer cmd, const void* key, const Buffer& vertices, std::uint32_t vertexCount,
                     VkDeviceSize vertexStride, const Buffer& indices, std::uint32_t indexCount,
                     std::vector<Buffer>& transient);
    // The mesh's bottom level's address, 0 if it has none.
    VkDeviceAddress bottom(const void* key) const;
    // Drops a mesh's bottom level; no frame in flight may use it any more.
    void forget(const void* key) { bottoms_.erase(key); }
    // Makes this frame's bottom-level builds visible to the top-level build.
    void barrierAfterBottoms(VkCommandBuffer cmd) const;

    // Builds the slot's top level from instances (which must name built
    // bottoms), visible to fragment shaders afterwards. When the slot's last
    // build had the same instances in the same order, of the same bottoms,
    // none moved more than refitReach studs and fewer than maxRefits refits
    // were made since its last full build, its tree is refitted instead
    // (VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR): the same hits, a
    // fraction of the build's time.
    void buildTop(VkCommandBuffer cmd, std::size_t index, std::span<const Instance> instances,
                  std::vector<Buffer>& transient);
    // The slot's top level (null before its first build), and whether the
    // last build had to make a new one (its descriptor needs writing).
    VkAccelerationStructureKHR top(std::size_t slot) const { return slots_.at(slot).handle; }
    bool topReplaced(std::size_t slot) const { return slots_.at(slot).replaced; }

    VkDeviceAddress bufferAddress(const Buffer& buffer) const;

    // How far an instance may have moved since the slot's last build for
    // it still to be refitted: the tree's boxes grow with what moved, so
    // rays look at more of them. GUESS: a few studs barely shows.
    static constexpr float refitReach = 4.0f;
    // Refits in a row before a slot's top level is built anew anyway.
    static constexpr std::uint32_t maxRefits = 59;

    struct Stats {
        std::uint32_t bottomBuilds = 0;
        std::uint32_t topBuilds = 0;
        std::uint32_t topRefits = 0; // of topBuilds, refitted rather than built
        std::uint32_t topInstances = 0;
    };
    // What was built since the last take; record() takes them once a frame.
    Stats takeStats();

private:
    struct Slot {
        Buffer instances; // VkAccelerationStructureInstanceKHR, host written
        std::uint32_t capacity = 0;
        Buffer storage;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        bool replaced = false;
        // What the last build put at each place: its bottom, how it's
        // seen (the instance's flags and mask) and where it was.
        struct Placed {
            VkDeviceAddress bottom = 0;
            std::uint32_t seen = 0;
            float at[3]{};
        };
        std::vector<Placed> placed;
        std::uint32_t refits = 0; // since the last full build
    };

    Buffer scratch(VkDeviceSize size, VkDeviceAddress& address) const;
    void destroy(VkAccelerationStructureKHR handle) const;

    Context* context_;
    VkDeviceSize scratchAlignment_ = 256;
    PFN_vkCreateAccelerationStructureKHR create_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR address_ = nullptr;
    std::vector<Slot> slots_;
    std::unordered_map<const void*, Bottom> bottoms_;
    Stats stats_;
};

} // namespace lunar::render
