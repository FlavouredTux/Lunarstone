#include "HighlightPass.hpp"

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace lunar::render {

namespace {

// post.vert (one triangle over the target), highlight.frag's and
// highlight_tiles.comp's SPIR-V.
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

constexpr unsigned char vertBytes[] = {
#embed "post.vert.spv"
};
constexpr unsigned char fragBytes[] = {
#embed "highlight.frag.spv"
};
constexpr unsigned char tilesBytes[] = {
#embed "highlight_tiles.comp.spv"
};
constexpr auto vertWords = toWords(vertBytes);
constexpr auto fragWords = toWords(fragBytes);
constexpr auto tilesWords = toWords(tilesBytes);
static_assert(vertWords[0] == 0x07230203 && fragWords[0] == 0x07230203 && tilesWords[0] == 0x07230203, "not SPIR-V");

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

} // namespace

HighlightPass::HighlightPass(Context& context, std::size_t slots, VkFormat colorFormat)
    : context_(&context), slots_(slots)
{
    VkDevice device = context.device();
    // The mask, the styles, and the tiles' ids as the draw reads them and
    // as the tile pass writes them.
    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    setLayout_ = UniqueDescriptorSetLayout(device, setLayout);

    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    layout_ = UniquePipelineLayout(device, layout);

    const auto count = static_cast<std::uint32_t>(slots);
    const VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * count},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = count,
        .poolSizeCount = 3,
        .pPoolSizes = poolSizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    pool_ = UniqueDescriptorPool(device, pool);
    for (Slot& slot : slots_) {
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        vkCheck(vkAllocateDescriptorSets(device, &allocate, &slot.set), "vkAllocateDescriptorSets");
        slot.styles = Buffer(context.allocator(), sizeof(Styles), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             MemoryUsage::Upload);
        const VkDescriptorBufferInfo buffer{slot.styles.get(), 0, sizeof(Styles)};
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &buffer,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // The mask holds integers: read with texelFetch, never filtered.
    const VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
    sampler_ = UniqueSampler(device, sampler);

    vert_ = makeShader(device, vertWords);
    frag_ = makeShader(device, fragWords);
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert_.get(),
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag_.get(),
         .pName = "main"},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
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
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // Blended over the scene, premultiplied; its alpha stays as it was.
    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
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
        .layout = layout_.get(),
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    pipeline_ = UniquePipeline(device, pipeline);

    tiles_ = makeShader(device, tilesWords);
    const VkComputePipelineCreateInfo tilesInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = tiles_.get(),
                  .pName = "main"},
        .layout = layout_.get(),
    };
    vkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &tilesInfo, nullptr, &pipeline),
            "vkCreateComputePipelines");
    tilesPipeline_ = UniquePipeline(device, pipeline);
}

HighlightPass::~HighlightPass() = default;

int HighlightPass::outlineRadius(std::uint32_t height)
{
    // GUESS: about two pixels at 1080 lines, like the client's looks, wider
    // on taller pictures.
    return std::clamp(static_cast<int>(std::lround(2.0 * static_cast<double>(height) / 1080.0)), 1,
                      static_cast<int>(tileSize));
}

const Image& HighlightPass::mask(std::uint32_t slot, VkExtent2D extent)
{
    Slot& s = slots_.at(slot);
    if (s.mask.get() == VK_NULL_HANDLE || s.mask.extent().width != extent.width
        || s.mask.extent().height != extent.height) {
        s.mask = Image(context_->allocator(), extent, maskFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        s.tiles = Image(context_->allocator(),
                        {(extent.width + tileSize - 1) / tileSize, (extent.height + tileSize - 1) / tileSize},
                        tileFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (s.bound != s.mask.view()) {
        const VkDescriptorImageInfo images[3] = {
            {sampler_.get(), s.mask.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler_.get(), s.tiles.view(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, s.tiles.view(), VK_IMAGE_LAYOUT_GENERAL},
        };
        const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        const std::uint32_t at[3] = {0, 2, 3};
        VkWriteDescriptorSet writes[3]{};
        for (std::size_t i = 0; i < 3; ++i)
            writes[i] = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = s.set,
                .dstBinding = at[i],
                .descriptorCount = 1,
                .descriptorType = types[i],
                .pImageInfo = &images[i],
            };
        vkUpdateDescriptorSets(context_->device(), 3, writes, 0, nullptr);
        s.bound = s.mask.view();
    }
    return s.mask;
}

void HighlightPass::record(VkCommandBuffer cmd, std::uint32_t slot, VkImageView scene, VkExtent2D extent,
                           std::span<const Style> styles)
{
    if (styles.empty())
        return;
    const Slot& s = slots_.at(slot);
    // The styles by id, for the frame's one draw: the GPU finished the slot's
    // last frame, which read them, before this one is recorded.
    Styles looks{};
    for (const Style& style : styles) {
        if (style.id == 0 || style.id > maxId)
            continue;
        std::memcpy(looks.fill[style.id], style.fill, sizeof(style.fill));
        std::memcpy(looks.outline[style.id], style.outline, sizeof(style.outline));
    }
    std::memcpy(s.styles.mapped().data(), &looks, sizeof(looks));

    // Which ids each tile has, for the draw to skip the pixels with none
    // near them.
    const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto tilesBarrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                            VkAccessFlags2 dstAccess, VkImageLayout oldLayout) {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStage,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = s.tiles.get(),
            .subresourceRange = colorRange,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    // Whatever the slot's last frame read of them.
    tilesBarrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tilesPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_.get(), 0, 1, &s.set, 0, nullptr);
    vkCmdDispatch(cmd, s.tiles.extent().width, s.tiles.extent().height, 1);
    tilesBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);

    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = scene,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &s.set, 0, nullptr);
    const Push push{.radius = outlineRadius(extent.height)};
    vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace lunar::render
