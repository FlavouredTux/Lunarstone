#include "EffectPass.hpp"

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <utility>

namespace lunar::render {

namespace {

// effect.vert and effect.frag's SPIR-V, assembled into words as Shaders.cpp does.
template <std::size_t N>
constexpr auto toWords(const unsigned char (&bytes)[N])
{
    static_assert(N % 4 == 0, "SPIR-V is a whole number of words");
    std::array<std::uint32_t, N / 4> words{};
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = std::uint32_t{bytes[4 * i]} | std::uint32_t{bytes[4 * i + 1]} << 8
                   | std::uint32_t{bytes[4 * i + 2]} << 16 | std::uint32_t{bytes[4 * i + 3]} << 24;
    return words;
}

constexpr unsigned char effectVertBytes[] = {
#embed "effect.vert.spv"
};
constexpr unsigned char effectFragBytes[] = {
#embed "effect.frag.spv"
};
constexpr auto effectVertWords = toWords(effectVertBytes);
constexpr auto effectFragWords = toWords(effectFragBytes);
static_assert(effectVertWords[0] == 0x07230203 && effectFragWords[0] == 0x07230203, "not SPIR-V");

UniqueShaderModule makeShader(VkDevice device, std::span<const std::uint32_t> code)
{
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return {device, module};
}

// Between two rendering scopes on the scene: the colour written through one
// view is blended through another, and the depth tested again.
void attachmentBarrier(VkCommandBuffer cmd)
{
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                         | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

EffectPass::EffectPass(Context& context, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat,
                       const VkSpecializationInfo& specialization, std::size_t slots)
    : context_(&context), layout_(layout), colorFormat_(colorFormat), depthFormat_(depthFormat), slots_(slots)
{
    VkDevice device = context.device();
    vert_ = makeShader(device, effectVertWords);
    frag_ = makeShader(device, effectFragWords);
    // The mesh pipelines' constants, kept (they live on the caller's
    // stack), and gammaTarget after them.
    const auto* data = static_cast<const std::byte*>(specialization.pData);
    specializationData_.assign(data, data + specialization.dataSize);
    specializationEntries_.assign(specialization.pMapEntries,
                                  specialization.pMapEntries + specialization.mapEntryCount);
    gammaOffset_ = static_cast<std::uint32_t>((specializationData_.size() + 3) / 4 * 4);
    specializationData_.resize(gammaOffset_ + sizeof(VkBool32));
    specializationEntries_.push_back({gammaConstant, gammaOffset_, sizeof(VkBool32)});
    pipeline(colorFormat, false);
}

VkPipeline EffectPass::pipeline(VkFormat colorFormat, bool gamma)
{
    const std::uint64_t key = (std::uint64_t{static_cast<std::uint32_t>(colorFormat)} << 1) | (gamma ? 1u : 0u);
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second.get();
    VkDevice device = context_->device();
    const VkBool32 gammaValue = gamma ? VK_TRUE : VK_FALSE;
    std::memcpy(specializationData_.data() + gammaOffset_, &gammaValue, sizeof(gammaValue));
    const VkSpecializationInfo specialization{static_cast<std::uint32_t>(specializationEntries_.size()),
                                              specializationEntries_.data(), specializationData_.size(),
                                              specializationData_.data()};
    const VkPipelineLayout layout = layout_;
    const VkFormat depthFormat = depthFormat_;
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert_.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag_.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, uvq)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, look)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, texture)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, extra)},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 6,
        .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    // Ribbons are seen from both sides.
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Reversed Z, tested against the scene but not written.
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    // Premultiplied: effect.frag scales alpha by one less LightEmission.
    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask
        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = depthFormat,
    };
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
    };
    VkPipeline made = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &made), "vkCreateGraphicsPipelines");
    return pipelines_.emplace(key, UniquePipeline(device, made)).first->second.get();
}

EffectPass::~EffectPass() = default;

void EffectPass::write(std::uint32_t slot, const EffectGeometry& geometry, std::span<const std::uint32_t> order,
                       std::span<const std::uint32_t> textures)
{
    Slot& s = slots_.at(slot);
    s.starts.clear();
    std::size_t indexCount = 0;
    for (std::uint32_t b : order)
        indexCount += geometry.batches[b].indexCount;
    if (indexCount == 0 || geometry.vertices.empty())
        return;
    // Grown to the next power of two; the slot's last frame is done with them.
    auto reserve = [&](Buffer& buffer, std::uint32_t& capacity, std::size_t count, std::size_t stride,
                       VkBufferUsageFlags usage) {
        if (count <= capacity)
            return;
        capacity = std::bit_ceil(std::max<std::uint32_t>(static_cast<std::uint32_t>(count), 1024));
        buffer = Buffer(context_->allocator(), VkDeviceSize{capacity} * stride, usage, MemoryUsage::Stream);
    };
    reserve(s.vertices, s.vertexCapacity, geometry.vertices.size(), sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    reserve(s.indices, s.indexCapacity, indexCount, sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // Every vertex takes the look of the batch whose triangles use it,
    // written once into host memory and then copied over whole: scattered
    // small writes into the mapped buffers cost several times more.
    vertices_.resize(geometry.vertices.size());
    indices_.resize(indexCount);
    written_.assign(geometry.vertices.size(), 0);
    std::uint32_t next = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const EffectBatch& batch = geometry.batches[order[i]];
        s.starts.push_back(next);
        const float look[3] = {1.0f - std::clamp(batch.lightEmission, 0.0f, 1.0f),
                               std::clamp(batch.lightInfluence, 0.0f, 1.0f), std::max(batch.brightness, 0.0f)};
        // effect.frag's flags.
        constexpr std::uint32_t clampBit = 0x80000000u;
        constexpr std::uint32_t untonedBit = 0x40000000u;
        const std::uint32_t texture
            = textures[i] | (batch.clampUv ? clampBit : 0u) | (batch.toneMapped ? 0u : untonedBit);
        // On top: tested where it'd be moved all the way to the near plane
        // (effect.vert stops it short of it), so nothing in the scene hides it.
        const float depthOffset = batch.onTop ? 1e9f : batch.depthOffset;
        for (std::uint32_t k = 0; k < batch.indexCount; ++k) {
            const std::uint32_t index = geometry.indices[batch.firstIndex + k];
            indices_[next++] = index;
            if (std::exchange(written_[index], 1) != 0)
                continue;
            const EffectVertex& from = geometry.vertices[index];
            vertices_[index] = {
                {from.position.x, from.position.y, from.position.z},
                {from.uvq[0], from.uvq[1], from.uvq[2]},
                {from.color[0], from.color[1], from.color[2], from.color[3]},
                {look[0], look[1], look[2]},
                texture,
                {from.uv2[0], from.uv2[1], from.frameBlend, depthOffset},
            };
        }
    }
    std::memcpy(s.vertices.mapped().data(), vertices_.data(), vertices_.size() * sizeof(Vertex));
    std::memcpy(s.indices.mapped().data(), indices_.data(), indices_.size() * sizeof(std::uint32_t));
    s.starts.push_back(next);
}

void EffectPass::draw(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t first, std::uint32_t count,
                      const GammaTarget* gamma)
{
    const Slot& s = slots_.at(slot);
    if (count == 0 || first + count >= s.starts.size())
        return;
    const std::uint32_t begin = s.starts[first];
    const std::uint32_t end = s.starts[first + count];
    if (end == begin)
        return;
    const bool switching = gamma && gamma->view != VK_NULL_HANDLE && gamma->resume;
    if (switching) {
        // The scene's image again through its UNORM view: blended on its
        // sRGB-encoded values, the way the client blends everything in its
        // gamma-space frame buffer (and the GUI here). Bound state carries
        // over from one rendering scope to the next.
        vkCmdEndRendering(cmd);
        attachmentBarrier(cmd);
        const VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = gamma->view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = gamma->resume->renderArea,
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
            .pDepthAttachment = gamma->resume->pDepthAttachment,
        };
        vkCmdBeginRendering(cmd, &rendering);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      switching ? pipeline(gamma->format, true) : pipeline(colorFormat_, false));
    const VkBuffer vertices = s.vertices.get();
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &zero);
    vkCmdBindIndexBuffer(cmd, s.indices.get(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, end - begin, 1, begin, 0, 0);
    if (switching) {
        vkCmdEndRendering(cmd);
        attachmentBarrier(cmd);
        vkCmdBeginRendering(cmd, gamma->resume);
    }
}

std::uint32_t EffectPass::triangles(std::uint32_t slot) const
{
    const Slot& s = slots_.at(slot);
    return s.starts.empty() ? 0 : s.starts.back() / 3;
}

} // namespace lunar::render
