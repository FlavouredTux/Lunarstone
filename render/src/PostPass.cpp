#include "PostPass.hpp"

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <span>

namespace lunar::render {

namespace {

// post.vert and post.frag's SPIR-V, assembled into words as Shaders.cpp does.
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

constexpr unsigned char postVertBytes[] = {
#embed "post.vert.spv"
};
constexpr unsigned char postFragBytes[] = {
#embed "post.frag.spv"
};
constexpr unsigned char shaftsFragBytes[] = {
#embed "shafts.frag.spv"
};
constexpr auto postVertWords = toWords(postVertBytes);
constexpr auto postFragWords = toWords(postFragBytes);
constexpr auto shaftsFragWords = toWords(shaftsFragBytes);
static_assert(postVertWords[0] == 0x07230203 && postFragWords[0] == 0x07230203 && shaftsFragWords[0] == 0x07230203,
              "not SPIR-V");

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

UniqueSampler makeSampler(VkDevice device, VkFilter filter)
{
    const VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler");
    return {device, sampler};
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
             VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkExtent2D levelExtent(VkExtent2D scene, int index)
{
    return {std::max(1u, scene.width >> (index + 1)), std::max(1u, scene.height >> (index + 1))};
}

void texelOf(float (&out)[4], VkExtent2D extent)
{
    out[0] = 1.0f / static_cast<float>(extent.width);
    out[1] = 1.0f / static_cast<float>(extent.height);
}

constexpr std::int32_t modeBright = 0;
constexpr std::int32_t modeDown = 1;
constexpr std::int32_t modeUp = 2;
constexpr std::int32_t modeRays = 3;
constexpr std::int32_t modeRadial = 4;
constexpr std::int32_t modeComposite = 5;

} // namespace

PostPass::PostPass(Context& context, std::size_t slots, bool rayQueries) : context_(&context), slots_(slots)
{
    VkDevice device = context.device();
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
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

    const std::uint32_t sets = setsPerSlot * static_cast<std::uint32_t>(slots);
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        sets * static_cast<std::uint32_t>(bindings.size())};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = sets,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    pool_ = UniqueDescriptorPool(device, pool);
    const std::vector<VkDescriptorSetLayout> layouts(setsPerSlot, setLayout);
    for (Slot& slot : slots_) {
        slot.sets.resize(setsPerSlot);
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = setsPerSlot,
            .pSetLayouts = layouts.data(),
        };
        vkCheck(vkAllocateDescriptorSets(device, &allocate, slot.sets.data()), "vkAllocateDescriptorSets");
    }

    if (rayQueries) {
        const VkDescriptorSetLayoutBinding shaftBindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        const VkDescriptorSetLayoutCreateInfo shaftSetInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = shaftBindings,
        };
        VkDescriptorSetLayout shaftSetLayout = VK_NULL_HANDLE;
        vkCheck(vkCreateDescriptorSetLayout(device, &shaftSetInfo, nullptr, &shaftSetLayout),
                "vkCreateDescriptorSetLayout");
        shaftsSetLayout_ = UniqueDescriptorSetLayout(device, shaftSetLayout);
        const VkPipelineLayoutCreateInfo shaftLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &shaftSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        };
        VkPipelineLayout shaftLayout = VK_NULL_HANDLE;
        vkCheck(vkCreatePipelineLayout(device, &shaftLayoutInfo, nullptr, &shaftLayout), "vkCreatePipelineLayout");
        shaftsLayout_ = UniquePipelineLayout(device, shaftLayout);
        const auto count = static_cast<std::uint32_t>(slots);
        const VkDescriptorPoolSize shaftSizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count},
                                                    {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, count}};
        const VkDescriptorPoolCreateInfo shaftPoolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = count,
            .poolSizeCount = 2,
            .pPoolSizes = shaftSizes,
        };
        VkDescriptorPool shaftPool = VK_NULL_HANDLE;
        vkCheck(vkCreateDescriptorPool(device, &shaftPoolInfo, nullptr, &shaftPool), "vkCreateDescriptorPool");
        shaftsPool_ = UniqueDescriptorPool(device, shaftPool);
        for (Slot& slot : slots_) {
            const VkDescriptorSetAllocateInfo allocate{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = shaftPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &shaftSetLayout,
            };
            vkCheck(vkAllocateDescriptorSets(device, &allocate, &slot.shafts), "vkAllocateDescriptorSets");
        }
        shaftsFrag_ = makeShader(device, shaftsFragWords);
    }

    linear_ = makeSampler(device, VK_FILTER_LINEAR);
    nearest_ = makeSampler(device, VK_FILTER_NEAREST);
    vert_ = makeShader(device, postVertWords);
    frag_ = makeShader(device, postFragWords);
    dummy_ = Image(context.allocator(), {1, 1}, levelFormat,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

PostPass::~PostPass() = default;

int PostPass::levelsFor(float pixels, std::uint32_t height)
{
    const float scaled = std::max(pixels * static_cast<float>(height) / 1080.0f, 2.0f);
    return std::clamp(static_cast<int>(std::lround(std::log2(scaled))) - 1, 1, maxLevels);
}

VkPipeline PostPass::pipeline(VkFormat format, bool add, bool shafts)
{
    const std::uint64_t key
        = (std::uint64_t{static_cast<std::uint32_t>(format)} << 2) | (add ? 1u : 0u) | (shafts ? 2u : 0u);
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second.get();
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert_.get(),
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = shafts ? shaftsFrag_.get() : frag_.get(),
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
    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = add ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
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
        .pColorAttachmentFormats = &format,
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
        .layout = shafts ? shaftsLayout_.get() : layout_.get(),
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(context_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    return pipelines_.emplace(key, UniquePipeline(context_->device(), pipeline)).first->second.get();
}

Image& PostPass::level(std::array<Image, maxLevels>& images, int index, VkExtent2D sceneExtent)
{
    Image& image = images.at(static_cast<std::size_t>(index));
    const VkExtent2D extent = levelExtent(sceneExtent, index);
    if (image.get() == VK_NULL_HANDLE || image.extent().width != extent.width
        || image.extent().height != extent.height)
        image = Image(context_->allocator(), extent, levelFormat,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    return image;
}

void PostPass::pass(VkCommandBuffer cmd, Slot& slot, const Output& out, const Inputs& in, const Push& push)
{
    VkDescriptorSet set = slot.sets.at(slot.used++);
    const VkImageView dummy = dummy_.view();
    auto info = [&](VkImageView view, bool depth) {
        if (view == VK_NULL_HANDLE)
            return VkDescriptorImageInfo{linear_.get(), dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return VkDescriptorImageInfo{depth ? nearest_.get() : linear_.get(), view,
                                     depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    const std::array<VkDescriptorImageInfo, 5> images{info(in.source, false), info(in.bloom, false),
                                                      info(in.blur, false), info(in.rays, false),
                                                      info(in.depth, true)};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i)
        writes[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &images[i],
        };
    vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    if (out.image != VK_NULL_HANDLE)
        barrier(cmd, out.image,
                out.add ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = out.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = out.add ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, out.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(out.extent.width), static_cast<float>(out.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, out.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(out.format, out.add));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (out.image != VK_NULL_HANDLE)
        barrier(cmd, out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void PostPass::traceShafts(VkCommandBuffer cmd, Slot& slot, const Frame& frame, Image& out, const Push& push)
{
    const VkDescriptorImageInfo depthInfo{nearest_.get(), frame.depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSetAccelerationStructureKHR topInfo{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &frame.rayScene,
    };
    const VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.shafts,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &depthInfo,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = &topInfo,
            .dstSet = slot.shafts,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        },
    };
    vkUpdateDescriptorSets(context_->device(), 2, writes, 0, nullptr);

    barrier(cmd, out.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = out.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, out.extent()},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(out.extent().width),
                              static_cast<float>(out.extent().height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, out.extent()};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(levelFormat, false, true));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shaftsLayout_.get(), 0, 1, &slot.shafts, 0,
                            nullptr);
    vkCmdPushConstants(cmd, shaftsLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    barrier(cmd, out.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void PostPass::record(VkCommandBuffer cmd, std::uint32_t slotIndex, const Frame& frame)
{
    Slot& slot = slots_.at(slotIndex);
    slot.used = 0;
    slot.extent = frame.sceneExtent;
    if (!dummyReady_) {
        barrier(cmd, dummy_.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dummy_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(cmd, dummy_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        dummyReady_ = true;
    }
    const Lighting& lighting = *frame.lighting;
    const PostLook& look = lighting.post;
    const VkExtent2D scene = frame.sceneExtent;
    auto output = [&](Image& image, bool add = false) {
        return Output{image.get(), image.view(), image.extent(), levelFormat, add};
    };

    // Bloom: the light above Threshold at half size, down a level at a
    // time to about Size, then each level's tent-filtered copy added onto
    // the one above it.
    VkImageView bloom = VK_NULL_HANDLE;
    float bloomWeight = 0.0f;
    if (look.bloom && look.bloomIntensity > 0.0f && look.bloomSize > 0.0f) {
        const int levels = levelsFor(look.bloomSize, scene.height);
        Push push{.mode = modeBright};
        texelOf(push.texel, scene);
        push.params[0] = look.bloomIntensity;
        push.params[1] = look.bloomThreshold;
        push.params3[3] = lighting.retroTone ? 1.0f : 0.0f;
        pass(cmd, slot, output(level(slot.bloom, 0, scene)), {.source = frame.scene}, push);
        for (int i = 1; i < levels; ++i) {
            Push down{.mode = modeDown};
            texelOf(down.texel, slot.bloom[static_cast<std::size_t>(i - 1)].extent());
            pass(cmd, slot, output(level(slot.bloom, i, scene)),
                 {.source = slot.bloom[static_cast<std::size_t>(i - 1)].view()}, down);
        }
        for (int i = levels - 2; i >= 0; --i) {
            const Image& from = slot.bloom[static_cast<std::size_t>(i + 1)];
            Push up{.mode = modeUp};
            texelOf(up.texel, from.extent());
            up.params[0] = 1.0f;
            up.params[1] = 1.0f;
            pass(cmd, slot, output(slot.bloom[static_cast<std::size_t>(i)], true), {.source = from.view()}, up);
        }
        bloom = slot.bloom[0].view();
        bloomWeight = 1.0f / static_cast<float>(levels);
    }

    // Blur, and depth of field's out-of-focus image: the scene down a
    // level at a time and tent-filtered back up to half size.
    VkImageView blurred = VK_NULL_HANDLE;
    float blurAmount = 0.0f;
    if (look.blur > 0.0f || look.depthOfField) {
        // GUESS: depth of field's blur reaches about 16 pixels.
        const float pixels = look.blur > 0.0f ? look.blur : 16.0f;
        const int levels = levelsFor(pixels, scene.height);
        if (look.blur > 0.0f)
            blurAmount = levels > 1 ? 1.0f : std::clamp(look.blur * static_cast<float>(scene.height) / 1080.0f / 4.0f,
                                                         0.0f, 1.0f);
        for (int i = 0; i < levels; ++i) {
            Push down{.mode = modeDown};
            texelOf(down.texel, i == 0 ? scene : slot.blur[static_cast<std::size_t>(i - 1)].extent());
            pass(cmd, slot, output(level(slot.blur, i, scene)),
                 {.source = i == 0 ? frame.scene : slot.blur[static_cast<std::size_t>(i - 1)].view()}, down);
        }
        blurred = slot.blur[static_cast<std::size_t>(levels - 1)].view();
        VkExtent2D blurredExtent = slot.blur[static_cast<std::size_t>(levels - 1)].extent();
        for (int i = levels - 2; i >= 0; --i) {
            Push up{.mode = modeUp};
            texelOf(up.texel, blurredExtent);
            up.params[0] = 1.0f;
            up.params[1] = 1.0f;
            Image& to = level(slot.blurUp, i, scene);
            pass(cmd, slot, output(to), {.source = blurred}, up);
            blurred = to.view();
            blurredExtent = to.extent();
        }
    }

    // Sun rays, at half size: traced through the scene when there are ray
    // queries, else the client's way, how much of the way to the sun is
    // open sky, blurred along the way to it (SunRays6FS, RadialBlur*FS).
    VkImageView rays = VK_NULL_HANDLE;
    const Camera& camera = *frame.camera;
    const float aspect = static_cast<float>(scene.width) / static_cast<float>(scene.height);
    const Vector3 toSun = lighting.sunDirection;
    const Vector3 forward = camera.cframe.lookVector();
    const float facing = forward.dot(toSun);
    auto raysImage = [&](std::size_t i) -> Image& {
        Image& image = slot.rays[i];
        const VkExtent2D extent = levelExtent(scene, 0);
        if (image.get() == VK_NULL_HANDLE || image.extent().width != extent.width
            || image.extent().height != extent.height)
            image = Image(context_->allocator(), extent, levelFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
        return image;
    };
    if (look.sunRays && look.sunRaysIntensity > 0.0f && frame.depth != VK_NULL_HANDLE
        && frame.rayScene != VK_NULL_HANDLE && shaftsFrag_) {
        // Traced: the light the air scatters wherever the sun reaches it,
        // softened by a tent (the steps' jitter is noisy).
        const float tanY = std::tan(camera.fieldOfView * 0.5f * std::numbers::pi_v<float> / 180.0f);
        const Vector3 right = camera.cframe.rightVector() * (tanY * aspect);
        const Vector3 up = camera.cframe.upVector() * tanY;
        const Vector3 eye = camera.cframe.position;
        // GUESS: Spread widens the lobe; Intensity 0.25 scatters 0.6% of
        // the sunlight a stud (more hazes over everything near).
        const float g = 0.6f - 0.3f * look.sunRaysSpread;
        Push shafts{.mode = modeRays};
        texelOf(shafts.texel, scene);
        shafts.params[0] = eye.x;
        shafts.params[1] = eye.y;
        shafts.params[2] = g;
        shafts.params[3] = g * g;
        shafts.params2[0] = lighting.sunColor.r;
        shafts.params2[1] = lighting.sunColor.g;
        shafts.params2[2] = lighting.sunColor.b;
        shafts.params2[3] = look.sunRaysIntensity * 0.025f;
        shafts.params3[0] = camera.nearPlane;
        shafts.params3[1] = camera.farPlane;
        shafts.params3[2] = shaftDistance;
        shafts.params3[3] = eye.z;
        const Vector3 basis[3] = {forward, right, up};
        const float sun[3] = {toSun.x, toSun.y, toSun.z};
        for (std::size_t i = 0; i < 3; ++i) {
            shafts.extra[i][0] = basis[i].x;
            shafts.extra[i][1] = basis[i].y;
            shafts.extra[i][2] = basis[i].z;
            shafts.extra[i][3] = sun[i];
        }
        traceShafts(cmd, slot, frame, raysImage(0), shafts);
        for (std::size_t i = 0; i < 2; ++i) {
            Push spread{.mode = modeUp};
            texelOf(spread.texel, levelExtent(scene, 0));
            spread.params[0] = 1.0f;
            spread.params[1] = 1.0f;
            pass(cmd, slot, output(raysImage((i + 1) % 2)), {.source = slot.rays[i % 2].view()}, spread);
        }
        rays = slot.rays[0].view();
    } else if (look.sunRays && look.sunRaysIntensity > 0.0f && frame.depth != VK_NULL_HANDLE && facing > 0.0f) {
        const Mat4 viewProjection = camera.viewProjection(aspect);
        const auto clip = viewProjection * Vec4{toSun.x, toSun.y, toSun.z, 0.0f};
        if (clip.w > 1e-4f) {
            const float sunU = clip.x / clip.w * 0.5f + 0.5f;
            const float sunV = clip.y / clip.w * 0.5f + 0.5f;
            const float tanY = std::tan(camera.fieldOfView * 0.5f * std::numbers::pi_v<float> / 180.0f);
            const Vector3 right = camera.cframe.rightVector() * (tanY * aspect);
            const Vector3 up = camera.cframe.upVector() * tanY;
            // GUESS: Spread widens the glow around the sun (a lower g), and
            // Intensity 0.25 brightens the sun's middle by about 2.
            const float g = 0.95f - 0.2f * look.sunRaysSpread;
            constexpr float raysScale = 8.0f;
            // Fades out as the sun leaves the side of the view.
            const float fade = std::clamp(facing * 4.0f, 0.0f, 1.0f);
            Push mask{.mode = modeRays};
            texelOf(mask.texel, scene);
            mask.params[0] = sunU;
            mask.params[1] = sunV;
            mask.params[2] = g;
            mask.params[3] = g * g;
            mask.params2[0] = lighting.sunColor.r;
            mask.params2[1] = lighting.sunColor.g;
            mask.params2[2] = lighting.sunColor.b;
            mask.params2[3] = look.sunRaysIntensity * raysScale * fade;
            mask.params3[0] = camera.nearPlane;
            mask.params3[1] = camera.farPlane;
            const Vector3 basis[3] = {forward, right, up};
            const float sun[3] = {toSun.x, toSun.y, toSun.z};
            for (std::size_t i = 0; i < 3; ++i) {
                mask.extra[i][0] = basis[i].x;
                mask.extra[i][1] = basis[i].y;
                mask.extra[i][2] = basis[i].z;
                mask.extra[i][3] = sun[i];
            }
            pass(cmd, slot, output(raysImage(0)), {.depth = frame.depth}, mask);
            // GUESS: two blurs, the second filling in between the first's taps.
            const std::array<std::array<float, 2>, 2> blurs{{{12.0f, 0.35f}, {6.0f, 0.06f}}};
            for (std::size_t i = 0; i < blurs.size(); ++i) {
                Push radial{.mode = modeRadial};
                texelOf(radial.texel, levelExtent(scene, 0));
                radial.params[0] = sunU;
                radial.params[1] = sunV;
                radial.params[2] = blurs[i][0];
                radial.params[3] = blurs[i][1];
                pass(cmd, slot, output(raysImage((i + 1) % 2)), {.source = slot.rays[i % 2].view()}, radial);
            }
            rays = slot.rays[blurs.size() % 2].view();
        }
    }

    // All of it onto the target, and ColorCorrection.
    barrier(cmd, frame.target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    Push composite{.mode = modeComposite};
    texelOf(composite.texel, scene);
    composite.params[0] = bloomWeight;
    composite.params[1] = blurAmount;
    composite.params[2] = rays != VK_NULL_HANDLE ? 1.0f : 0.0f;
    composite.params[3] = bloom != VK_NULL_HANDLE || rays != VK_NULL_HANDLE ? 1.0f : 0.0f;
    if (look.depthOfField && frame.depth != VK_NULL_HANDLE) {
        composite.params2[0] = look.focusDistance - look.inFocusRadius;
        composite.params2[1] = look.focusDistance + look.inFocusRadius;
        composite.params2[2] = look.nearIntensity;
        composite.params2[3] = look.farIntensity;
        composite.params3[2] = 1.0f;
    }
    composite.params3[0] = camera.nearPlane;
    composite.params3[1] = camera.farPlane;
    composite.params3[3] = lighting.retroTone ? 1.0f : 0.0f;
    for (std::size_t row = 0; row < 3; ++row)
        std::ranges::copy(lighting.colorCorrection[row], composite.extra[row]);
    pass(cmd, slot,
         {.view = frame.targetView, .extent = frame.targetExtent, .format = frame.targetFormat},
         {.source = frame.scene, .bloom = bloom, .blur = blurred, .rays = rays,
          .depth = look.depthOfField ? frame.depth : VK_NULL_HANDLE},
         composite);
}

} // namespace lunar::render
