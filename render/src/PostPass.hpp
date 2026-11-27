#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Camera.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lunar::render {

class Context;

// Lighting's post effects for MeshRenderer: bloom, blur, sun rays and depth
// of field over the 3D image, then ColorCorrection, drawn onto the target in
// one last pass (which also scales a smaller scene up to it). The scene is
// tone mapped already, so the light the client keeps above white for its
// bloom is taken back through the inverse of the tone curve (post.frag).
//
// Each frame slot owns its smaller images: the bloom's levels, the blur's
// (down and back up) and the sun rays', made the first frame that needs
// them at the scene's size. With ray queries (and a frame's top level),
// sun rays are traced through the scene instead (shafts.frag).
class PostPass {
public:
    // rayQueries: the context has them (Context::rayTracing), for traced sun rays.
    PostPass(Context& context, std::size_t slots, bool rayQueries = false);
    PostPass(const PostPass&) = delete;
    PostPass& operator=(const PostPass&) = delete;
    ~PostPass();

    struct Frame {
        // The 3D image, in SHADER_READ_ONLY_OPTIMAL, its format sRGB (read
        // as linear), and its depth (reversed Z, DEPTH_READ_ONLY_OPTIMAL,
        // the same size) when needsDepth says so.
        VkImageView scene = VK_NULL_HANDLE;
        VkExtent2D sceneExtent{};
        VkImageView depth = VK_NULL_HANDLE;
        // Drawn over completely (whatever it held is dropped) and left in
        // COLOR_ATTACHMENT_OPTIMAL.
        VkImage target = VK_NULL_HANDLE;
        VkImageView targetView = VK_NULL_HANDLE;
        VkFormat targetFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D targetExtent{};
        const Camera* camera = nullptr;
        const Lighting* lighting = nullptr;
        // The frame's top level, built and visible to fragment shaders:
        // sun rays are traced through it. Null for the screen's.
        VkAccelerationStructureKHR rayScene = VK_NULL_HANDLE;
    };
    void record(VkCommandBuffer cmd, std::uint32_t slot, const Frame& frame);

    // Whether a frame with these effects reads the scene's depth.
    static bool needsDepth(const PostLook& look) { return look.sunRays || look.depthOfField; }
    // The levels (each half the last, the first half the scene) a blur of
    // `pixels` (at 1080 lines; GUESS: the client's sizes scale with the
    // screen) goes down to.
    static int levelsFor(float pixels, std::uint32_t height);

    static constexpr int maxLevels = 6;
    // Traced sun rays: studs along each view ray they look at most. GUESS.
    static constexpr float shaftDistance = 400.0f;
    static constexpr VkFormat levelFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

private:
    struct Push {
        float texel[4]{};
        float params[4]{};
        float params2[4]{};
        float extra[3][4]{};
        float params3[4]{};
        std::int32_t mode = 0;
    };
    struct Slot {
        VkExtent2D extent{}; // the scene's, which the images are made for
        std::array<Image, maxLevels> bloom;
        std::array<Image, maxLevels> blur;
        std::array<Image, maxLevels> blurUp;
        std::array<Image, 2> rays;
        std::vector<VkDescriptorSet> sets;
        std::uint32_t used = 0; // sets this frame
        VkDescriptorSet shafts = VK_NULL_HANDLE; // the traced sun rays' (depth, top level)
    };
    // What a pass reads at each binding (post.frag); null for nothing.
    struct Inputs {
        VkImageView source = VK_NULL_HANDLE;
        VkImageView bloom = VK_NULL_HANDLE;
        VkImageView blur = VK_NULL_HANDLE;
        VkImageView rays = VK_NULL_HANDLE;
        VkImageView depth = VK_NULL_HANDLE;
    };
    struct Output {
        VkImage image = VK_NULL_HANDLE; // null for the target, whose layout the caller sees to
        VkImageView view = VK_NULL_HANDLE;
        VkExtent2D extent{};
        VkFormat format = levelFormat;
        bool add = false; // blended onto what's there
    };
    static constexpr std::uint32_t setsPerSlot = 32;

    VkPipeline pipeline(VkFormat format, bool add, bool shafts = false);
    // The sun rays traced into the slot's first rays image.
    void traceShafts(VkCommandBuffer cmd, Slot& slot, const Frame& frame, Image& out, const Push& push);
    Image& level(std::array<Image, maxLevels>& images, int index, VkExtent2D sceneExtent);
    void pass(VkCommandBuffer cmd, Slot& slot, const Output& out, const Inputs& in, const Push& push);

    Context* context_;
    UniqueDescriptorSetLayout setLayout_;
    UniquePipelineLayout layout_;
    UniqueDescriptorPool pool_;
    UniqueSampler linear_;
    UniqueSampler nearest_;
    UniqueShaderModule vert_;
    UniqueShaderModule frag_;
    // Traced sun rays: their own set (depth, top level) and shader.
    UniqueDescriptorSetLayout shaftsSetLayout_;
    UniquePipelineLayout shaftsLayout_;
    UniqueDescriptorPool shaftsPool_;
    UniqueShaderModule shaftsFrag_;
    std::unordered_map<std::uint64_t, UniquePipeline> pipelines_; // by format and blending
    Image dummy_; // 1 x 1 black, for what a pass doesn't read
    bool dummyReady_ = false;
    std::vector<Slot> slots_;
};

} // namespace lunar::render
