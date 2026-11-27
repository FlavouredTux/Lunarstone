#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/render/Vulkan.hpp>
#include <lunar/ui/DrawList.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::render {
class Context;
}

namespace lunar::ui {

// Draws DrawLists on top of the 3D scene as a render::FrameOverlay: set it
// with MeshRenderer::setOverlay (Presenter::renderer(), OffscreenRenderer::
// renderer()) and hand it each frame's list with submit() before the frame
// is drawn. All quads go out in one instanced draw, read from a storage
// buffer; textures (glyph atlas, gradient ramps, images) sit in one array.
class UiRenderer final : public render::FrameOverlay {
public:
    static constexpr std::uint32_t maxTextures = 512;
    static constexpr std::uint32_t maxGradients = 512;

    // colorFormat must match the target views the scene renderer draws into.
    static std::expected<std::unique_ptr<UiRenderer>, std::string>
    create(render::Context& context, VkFormat colorFormat, std::uint32_t slots = 2);
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    ~UiRenderer() override;

    // This frame's drawing. The list is copied; the atlas must outlive the
    // frame's recording (its new glyphs upload then).
    void submit(const DrawList& list, GlyphAtlas& atlas);
    void record(VkCommandBuffer cmd, std::uint32_t slot, const render::RenderTarget& target) override;

    std::size_t residentImages() const { return images_.size(); }

    // Runs at the start of every record(), before anything else is recorded:
    // other offscreen passes whose pictures the UI shows (the map, the
    // ViewportFrames). One per owner; a null function forgets the owner's.
    void setPrepass(const void* owner, std::function<void(VkCommandBuffer cmd, std::uint32_t slot)> prepass);
    // Quads whose texture is `key` show a GPU image instead: whatever view
    // `view` returns while recording (after the prepass), which must be in
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and hold sRGB-encoded values
    // in a UNORM format. Null views draw the quads as their colour. A null
    // function forgets the key. False when the texture array is full: the
    // key isn't known then, and would draw as the image it is.
    bool setExternalImage(const Image* key, std::function<VkImageView()> view);

private:
    struct Slot {
        render::Buffer quads;
        std::uint32_t capacity = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::vector<render::Buffer> staging;
        std::uint64_t viewsVersion = ~std::uint64_t{0}; // which descriptor contents the set holds
    };
    struct GpuImage {
        std::shared_ptr<const Image> source;
        render::Image image;
        std::uint32_t index = 0;
        std::uint64_t lastUsed = 0;
        bool uploaded = false;
    };

    explicit UiRenderer(render::Context& context) : context_(&context) {}
    void createResources(VkFormat colorFormat, std::uint32_t slotCount);
    void uploadImage(VkCommandBuffer cmd, Slot& slot, GpuImage& image);
    // Made on first use per target format and depth format (UNDEFINED: none).
    VkPipeline pipeline(VkFormat format, VkFormat depthFormat = VK_FORMAT_UNDEFINED);

    render::Context* context_;
    render::UniqueDescriptorSetLayout setLayout_;
    render::UniqueDescriptorPool pool_;
    render::UniqueSampler linear_;
    render::UniqueSampler nearest_;
    render::UniquePipelineLayout layout_;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    std::unordered_map<std::uint64_t, render::UniquePipeline> pipelines_; // by colour << 32 | depth format
    std::vector<Slot> slots_;

    render::Image atlas_;
    bool atlasInitialized_ = false;
    std::uint64_t atlasGeneration_ = 0;
    render::Image gradients_;
    bool gradientsInitialized_ = false;

    std::unordered_map<const Image*, GpuImage> images_;
    struct External {
        std::function<VkImageView()> view;
        std::uint32_t index = 0;
    };
    std::unordered_map<const Image*, External> externals_;
    std::vector<std::pair<const void*, std::function<void(VkCommandBuffer, std::uint32_t)>>> prepasses_;
    std::vector<std::uint32_t> freeIndices_;
    std::vector<VkImageView> views_; // per texture index
    std::uint64_t viewsVersion_ = 0;
    std::uint64_t frame_ = 0;

    // The submitted frame.
    DrawList list_;
    GlyphAtlas* atlas_source_ = nullptr;
};

} // namespace lunar::ui
