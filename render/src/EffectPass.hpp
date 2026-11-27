#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar::render {

class Context;

// Draws a frame's EffectGeometry for MeshRenderer: one pipeline (effect.vert
// and effect.frag) on the renderer's own layout and descriptors, and each
// frame slot's vertex and index buffers.
class EffectPass {
public:
    // specialization: the mesh pipelines', which size frame.glsl's texture array.
    EffectPass(Context& context, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat,
               const VkSpecializationInfo& specialization, std::size_t slots);
    EffectPass(const EffectPass&) = delete;
    EffectPass& operator=(const EffectPass&) = delete;
    ~EffectPass();

    // Writes the geometry into the slot's buffers with its batches in
    // `order` (indices into geometry.batches), batch i textured by
    // textures[i] (a renderer texture index, 0 for white). draw() then
    // takes ranges of that order.
    void write(std::uint32_t slot, const EffectGeometry& geometry, std::span<const std::uint32_t> order,
               std::span<const std::uint32_t> textures);
    // The scene's image as UNORM, for blending the way the client does: the
    // scene's rendering scope (resume, loading what's there) is ended, the
    // batches drawn through the view, and the scope begun again.
    struct GammaTarget {
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        const VkRenderingInfo* resume = nullptr;
    };
    // Draws the batches order[first, first + count) of the last write, inside
    // the scene's rendering with the renderer's descriptors bound. Binds its
    // own pipeline, and vertex and index buffers. With a gamma target they're
    // blended in sRGB space; without, in linear space through the scene's
    // own view.
    void draw(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t first, std::uint32_t count,
              const GammaTarget* gamma = nullptr);

    // What the last write put in the slot's buffers.
    std::uint32_t triangles(std::uint32_t slot) const;

private:
    // What the vertex shader reads: an EffectVertex with its batch's look.
    struct Vertex {
        float position[3];
        float uvq[3];
        float color[4];
        float look[3];         // one less LightEmission, LightInfluence, Brightness
        std::uint32_t texture; // flags in the top bits (effect.frag)
        float extra[4];        // the second flipbook frame's uv, how far towards it, depth offset
    };
    struct Slot {
        Buffer vertices;
        Buffer indices;
        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;
        // Where each batch of the written order starts in the index buffer.
        std::vector<std::uint32_t> starts;
    };

    // effect.frag's constant_id for writing sRGB-encoded colour.
    static constexpr std::uint32_t gammaConstant = 10;
    // The pipeline drawing into colorFormat, made the first time it's asked for.
    VkPipeline pipeline(VkFormat colorFormat, bool gamma);

    Context* context_;
    VkPipelineLayout layout_;
    VkFormat colorFormat_;
    VkFormat depthFormat_;
    UniqueShaderModule vert_;
    UniqueShaderModule frag_;
    std::vector<std::byte> specializationData_;
    std::vector<VkSpecializationMapEntry> specializationEntries_;
    std::uint32_t gammaOffset_ = 0;
    std::unordered_map<std::uint64_t, UniquePipeline> pipelines_; // by colour format and gamma
    std::vector<Slot> slots_;
    // A frame's vertices and indices before they're copied into a slot's
    // buffers, kept for their capacity.
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::uint8_t> written_;
};

} // namespace lunar::render
