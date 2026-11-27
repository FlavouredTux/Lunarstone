#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lunar::render {

class Context;

// Highlights for MeshRenderer, which draws the highlighted items into each
// slot's mask (which highlight each pixel is under, 0 for none) with its
// mesh pipelines; this then blends each highlight's fill and outline over
// the scene from that mask.
class HighlightPass {
public:
    static constexpr VkFormat maskFormat = VK_FORMAT_R8_UINT;
    // The highest id a style can have: highlight.frag keeps the ids near a
    // pixel as the bits of a word.
    static constexpr std::uint32_t maxId = 31;
    // The tiles of the mask whose ids highlight.frag looks at before
    // searching a pixel's outline: pixels on a side (as wide as the widest
    // outline, so a search never leaves a tile's neighbours), a bit per id.
    static constexpr std::uint32_t tileSize = 8;
    static constexpr VkFormat tileFormat = VK_FORMAT_R32_UINT;

    HighlightPass(Context& context, std::size_t slots, VkFormat colorFormat);
    HighlightPass(const HighlightPass&) = delete;
    HighlightPass& operator=(const HighlightPass&) = delete;
    ~HighlightPass();

    // The slot's mask, made (again) at this size when it isn't.
    const Image& mask(std::uint32_t slot, VkExtent2D extent);

    struct Style {
        float fill[4]{};    // linear RGB, alpha 1 - FillTransparency
        float outline[4]{}; // linear RGB, alpha 1 - OutlineTransparency
        std::uint32_t id = 0; // its value in the mask
    };
    // Every style over the scene (in COLOR_ATTACHMENT_OPTIMAL, left so) in
    // one draw, from the slot's mask, which must be in
    // SHADER_READ_ONLY_OPTIMAL. Ids are 1 to maxId.
    void record(VkCommandBuffer cmd, std::uint32_t slot, VkImageView scene, VkExtent2D extent,
                std::span<const Style> styles);

    // The outline's width in pixels at a picture this many lines tall: 1 to
    // tileSize.
    static int outlineRadius(std::uint32_t height);

private:
    struct Push {
        std::int32_t radius;
    };
    // highlight.frag's Styles, by id.
    struct Styles {
        float fill[maxId + 1][4];
        float outline[maxId + 1][4];
    };
    struct Slot {
        Image mask;
        Image tiles;   // tileFormat, a texel per tileSize x tileSize of the mask
        Buffer styles; // Styles, written by the frame that uses the slot
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageView bound = VK_NULL_HANDLE; // the mask view the set names
    };

    Context* context_;
    UniqueDescriptorSetLayout setLayout_;
    UniquePipelineLayout layout_;
    UniqueDescriptorPool pool_;
    UniqueSampler sampler_;
    UniqueShaderModule vert_;
    UniqueShaderModule frag_;
    UniquePipeline pipeline_;
    UniqueShaderModule tiles_;
    UniquePipeline tilesPipeline_;
    std::vector<Slot> slots_;
};

} // namespace lunar::render
