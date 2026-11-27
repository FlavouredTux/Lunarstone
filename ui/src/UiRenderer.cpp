#include <lunar/ui/UiRenderer.hpp>

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace lunar::ui {

namespace {

// glslc output, #embedded at build time like render/src/Shaders.cpp does.
constexpr unsigned char vertBytes[] = {
#embed "ui.vert.spv"
};
constexpr unsigned char fragBytes[] = {
#embed "ui.frag.spv"
};

template <std::size_t N>
std::vector<std::uint32_t> words(const unsigned char (&bytes)[N])
{
    static_assert(N % 4 == 0, "SPIR-V is a whole number of words");
    std::vector<std::uint32_t> out(N / 4);
    std::memcpy(out.data(), bytes, N);
    return out;
}

render::UniqueShaderModule makeShader(VkDevice device, const std::vector<std::uint32_t>& code)
{
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * 4,
        .pCode = code.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    render::vkCheck(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return {device, module};
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
             VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    const VkImageMemoryBarrier2 b{
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
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

bool isUnorm(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM
           || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

constexpr VkImageLayout readLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// Matches ui.glsl's Quad (std430).
struct GpuQuad {
    float rect[4];
    float uv[4];
    float color[4];
    float radii[4];
    float clip[4];
    float rotation[4];
    float gradient[4];
    std::uint32_t info[4];
};
static_assert(sizeof(GpuQuad) == 128);

// A WorldLayer, stored after the quads in their buffer (ui.glsl's Layer).
struct GpuLayer {
    float origin[4]; // w: kind
    float axisX[4];  // w: depth tested
    float axisY[4];
    float tint[4];
    float unused[16];
};
static_assert(sizeof(GpuLayer) == sizeof(GpuQuad));

// ui.glsl's push constants.
struct Push {
    float params[4]; // viewport width, height, gradient rows, has view
    std::uint32_t layerBase[4];
    float viewProjection[16];
};

} // namespace

std::expected<std::unique_ptr<UiRenderer>, std::string> UiRenderer::create(render::Context& context,
                                                                           VkFormat colorFormat, std::uint32_t slots)
{
    std::unique_ptr<UiRenderer> ui(new UiRenderer(context));
    try {
        ui->createResources(colorFormat, std::max(slots, 1u));
    } catch (const render::VulkanError& e) {
        return std::unexpected(e.what());
    }
    return ui;
}

UiRenderer::~UiRenderer()
{
    context_->waitIdle();
}

void UiRenderer::createResources(VkFormat colorFormat, std::uint32_t slotCount)
{
    VkDevice device = context_->device();

    auto makeSampler = [&](VkFilter filter) {
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
        render::vkCheck(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler");
        return render::UniqueSampler(device, sampler);
    };
    linear_ = makeSampler(VK_FILTER_LINEAR);
    nearest_ = makeSampler(VK_FILTER_NEAREST);
    const VkSampler samplers[2] = {linear_.get(), nearest_.get()};

    const VkDescriptorSetLayoutBinding bindings[3] = {
        {.binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = maxTextures,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .descriptorCount = 2,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .pImmutableSamplers = samplers},
        {.binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    const VkDescriptorBindingFlags flags[3] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 3,
        .pBindingFlags = flags,
    };
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flagsInfo,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    render::vkCheck(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout),
                    "vkCreateDescriptorSetLayout");
    setLayout_ = render::UniqueDescriptorSetLayout(device, setLayout);

    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxTextures * slotCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * slotCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slotCount},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = slotCount,
        .poolSizeCount = 3,
        .pPoolSizes = sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    render::vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    pool_ = render::UniqueDescriptorPool(device, pool);

    slots_.resize(slotCount);
    for (Slot& slot : slots_) {
        const VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        render::vkCheck(vkAllocateDescriptorSets(device, &allocInfo, &slot.set), "vkAllocateDescriptorSets");
    }

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    render::vkCheck(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout),
                    "vkCreatePipelineLayout");
    layout_ = render::UniquePipelineLayout(device, pipelineLayout);

    colorFormat_ = colorFormat;
    pipeline(colorFormat);

    atlas_ = render::Image(context_->allocator(), {GlyphAtlas::size, GlyphAtlas::size}, VK_FORMAT_R8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    gradients_ = render::Image(context_->allocator(), {GradientRamp::width, maxGradients}, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    views_.assign(maxTextures, VK_NULL_HANDLE);
    views_[0] = atlas_.view();
    views_[1] = gradients_.view();
    for (std::uint32_t i = maxTextures; i-- > 2;)
        freeIndices_.push_back(i);
}

VkPipeline UiRenderer::pipeline(VkFormat format, VkFormat depthFormat)
{
    const std::uint64_t key = std::uint64_t{static_cast<std::uint32_t>(format)} << 32
                              | static_cast<std::uint32_t>(depthFormat);
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second.get();
    VkDevice device = context_->device();
    struct Specialization {
        std::int32_t textureCount = static_cast<std::int32_t>(maxTextures);
        VkBool32 srgbTarget = VK_FALSE;
    } constants;
    constants.srgbTarget = isUnorm(format) ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entries[2] = {
        {0, offsetof(Specialization, textureCount), sizeof(std::int32_t)},
        {1, offsetof(Specialization, srgbTarget), sizeof(VkBool32)},
    };
    const VkSpecializationInfo specialization{2, entries, sizeof(Specialization), &constants};

    const render::UniqueShaderModule vert = makeShader(device, words(vertBytes));
    const render::UniqueShaderModule frag = makeShader(device, words(fragBytes));
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert.get(),
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{.sType
                                                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask
        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blendState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
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
        .depthAttachmentFormat = depthFormat,
    };
    // With the scene's depth: world layers behind what it drew don't show
    // (reversed Z); nothing writes depth.
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depthFormat != VK_FORMAT_UNDEFINED ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blendState,
        .pDynamicState = &dynamic,
        .layout = layout_.get(),
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    render::vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                    "vkCreateGraphicsPipelines");
    return pipelines_.emplace(key, render::UniquePipeline(device, pipeline)).first->second.get();
}

void UiRenderer::setPrepass(const void* owner, std::function<void(VkCommandBuffer, std::uint32_t)> prepass)
{
    std::erase_if(prepasses_, [owner](const auto& entry) { return entry.first == owner; });
    if (prepass)
        prepasses_.emplace_back(owner, std::move(prepass));
}

bool UiRenderer::setExternalImage(const Image* key, std::function<VkImageView()> view)
{
    auto it = externals_.find(key);
    if (!view) {
        if (it != externals_.end()) {
            views_[it->second.index] = VK_NULL_HANDLE;
            freeIndices_.push_back(it->second.index);
            ++viewsVersion_;
            externals_.erase(it);
        }
        return true;
    }
    if (it == externals_.end()) {
        if (freeIndices_.empty())
            return false;
        it = externals_.emplace(key, External{.index = freeIndices_.back()}).first;
        freeIndices_.pop_back();
    }
    it->second.view = std::move(view);
    return true;
}

void UiRenderer::submit(const DrawList& list, GlyphAtlas& atlas)
{
    list_ = list;
    atlas_source_ = &atlas;
}

void UiRenderer::uploadImage(VkCommandBuffer cmd, Slot& slot, GpuImage& image)
{
    const Image& src = *image.source;
    render::Buffer staging(context_->allocator(), src.byteSize(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           render::MemoryUsage::Upload);
    std::memcpy(staging.mapped().data(), src.rgba.data(), src.byteSize());
    src.rgba.evict(); // pixels mapped from a file needn't stay in memory once copied
    barrier(cmd, image.image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {src.width, src.height, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging.get(), image.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, image.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    slot.staging.push_back(std::move(staging));
    image.uploaded = true;
}

void UiRenderer::record(VkCommandBuffer cmd, std::uint32_t slotIndex, const render::RenderTarget& target)
{
    ++frame_;
    Slot& slot = slots_[slotIndex % slots_.size()];
    slot.staging.clear(); // the GPU finished this slot's previous frame
    for (const auto& [owner, prepass] : prepasses_)
        prepass(cmd, slotIndex);
    for (auto& [key, external] : externals_)
        if (VkImageView view = external.view(); view != views_[external.index]) {
            views_[external.index] = view;
            ++viewsVersion_;
        }

    // Glyph atlas: whole on first use or after a reset, else the changed rows.
    if (atlas_source_) {
        auto [begin, end] = atlas_source_->takeDirty();
        if (!atlasInitialized_ || atlas_source_->generation() != atlasGeneration_) {
            begin = 0;
            end = GlyphAtlas::size;
        }
        if (end > begin) {
            const auto rows = static_cast<std::uint32_t>(end - begin);
            const VkDeviceSize bytes = VkDeviceSize{rows} * GlyphAtlas::size;
            render::Buffer staging(context_->allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   render::MemoryUsage::Upload);
            if (atlas_source_->pixels().empty()) // no glyph yet
                std::memset(staging.mapped().data(), 0, bytes);
            else
                std::memcpy(staging.mapped().data(),
                            atlas_source_->pixels().data() + static_cast<std::size_t>(begin) * GlyphAtlas::size, bytes);
            barrier(cmd, atlas_.get(), atlasInitialized_ ? readLayout : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkBufferImageCopy region{
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageOffset = {0, begin, 0},
                .imageExtent = {GlyphAtlas::size, rows, 1},
            };
            vkCmdCopyBufferToImage(cmd, staging.get(), atlas_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, atlas_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            slot.staging.push_back(std::move(staging));
            atlasInitialized_ = true;
            atlasGeneration_ = atlas_source_->generation();
        }
    }
    if (!atlasInitialized_) {
        // Nothing to upload yet, but the shader may still sample it.
        barrier(cmd, atlas_.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, atlas_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(cmd, atlas_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        atlasInitialized_ = true;
    }

    // Gradient ramps, rewritten whenever the frame has any.
    const std::size_t gradientCount = std::min<std::size_t>(list_.gradients.size(), maxGradients);
    if (gradientCount > 0 || !gradientsInitialized_) {
        const std::size_t rows = std::max<std::size_t>(gradientCount, 1);
        const VkDeviceSize bytes = rows * GradientRamp::width * 4;
        render::Buffer staging(context_->allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               render::MemoryUsage::Upload);
        std::memset(staging.mapped().data(), 255, bytes);
        for (std::size_t i = 0; i < gradientCount; ++i)
            std::memcpy(staging.mapped().data() + (i * GradientRamp::width * 4), list_.gradients[i].rgba.data(),
                        list_.gradients[i].rgba.size());
        barrier(cmd, gradients_.get(), gradientsInitialized_ ? readLayout : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkBufferImageCopy region{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {GradientRamp::width, static_cast<std::uint32_t>(rows), 1},
        };
        vkCmdCopyBufferToImage(cmd, staging.get(), gradients_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, gradients_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readLayout, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        slot.staging.push_back(std::move(staging));
        gradientsInitialized_ = true;
    }

    // Images: upload new ones, map DrawList slots to array indices.
    std::vector<std::uint32_t> remap(list_.textures.size() + 1, 0);
    for (std::size_t i = 0; i < list_.textures.size(); ++i) {
        const auto& source = list_.textures[i];
        if (auto external = externals_.find(source.get()); external != externals_.end()) {
            remap[i + 1] = views_[external->second.index] ? external->second.index : 0;
            continue;
        }
        auto it = images_.find(source.get());
        if (it == images_.end()) {
            if (freeIndices_.empty() || source->width > 8192 || source->height > 8192)
                continue;
            GpuImage entry;
            entry.source = source;
            entry.image = render::Image(
                context_->allocator(), {source->width, source->height}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            entry.index = freeIndices_.back();
            freeIndices_.pop_back();
            views_[entry.index] = entry.image.view();
            ++viewsVersion_;
            it = images_.emplace(source.get(), std::move(entry)).first;
        }
        if (!it->second.uploaded)
            uploadImage(cmd, slot, it->second);
        it->second.lastUsed = frame_;
        remap[i + 1] = it->second.index;
    }
    // Images no frame used for a while go; every frame that could read them is done.
    const std::uint64_t keep = slots_.size() + 120;
    std::erase_if(images_, [&](auto& entry) {
        if (entry.second.lastUsed + keep >= frame_)
            return false;
        views_[entry.second.index] = VK_NULL_HANDLE;
        freeIndices_.push_back(entry.second.index);
        ++viewsVersion_;
        return true;
    });

    // Quads into this slot's buffer, then their world layers.
    const auto count = static_cast<std::uint32_t>(list_.quads.size());
    const auto entries = count + static_cast<std::uint32_t>(list_.layers.size());
    if (entries > slot.capacity) {
        slot.capacity = std::max<std::uint32_t>(entries + entries / 2, 256);
        slot.quads = render::Buffer(context_->allocator(), VkDeviceSize{slot.capacity} * sizeof(GpuQuad),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, render::MemoryUsage::Stream);
        slot.viewsVersion = ~std::uint64_t{0};
    }
    if (count > 0) {
        std::byte* out = slot.quads.mapped().data();
        for (std::uint32_t i = 0; i < count; ++i) {
            const Quad& q = list_.quads[i];
            GpuQuad g{};
            g.rect[0] = q.rect.x;
            g.rect[1] = q.rect.y;
            g.rect[2] = q.rect.w;
            g.rect[3] = q.rect.h;
            std::ranges::copy(q.uv, g.uv);
            g.color[0] = q.color.r;
            g.color[1] = q.color.g;
            g.color[2] = q.color.b;
            g.color[3] = q.color.a;
            std::ranges::copy(q.radii, g.radii);
            g.clip[0] = q.clip.x;
            g.clip[1] = q.clip.y;
            g.clip[2] = q.clip.right();
            g.clip[3] = q.clip.bottom();
            g.rotation[0] = std::cos(q.rotation);
            g.rotation[1] = std::sin(q.rotation);
            g.rotation[2] = q.pivotX;
            g.rotation[3] = q.pivotY;
            g.gradient[0] = std::cos(q.gradientRotation);
            g.gradient[1] = std::sin(q.gradientRotation);
            g.gradient[2] = q.gradientOffsetX;
            g.gradient[3] = q.gradientOffsetY;
            std::uint32_t texture = 0;
            QuadKind kind = q.kind;
            if (q.kind == QuadKind::Image) {
                texture = q.texture < remap.size() ? remap[q.texture] : 0;
                if (texture == 0)
                    kind = QuadKind::Solid; // not resident: drawn as its colour
            }
            g.info[0] = texture;
            const std::uint32_t layer = q.layer <= list_.layers.size() ? q.layer : 0;
            g.info[1] = static_cast<std::uint32_t>(kind) | (q.pixelated ? 1u << 8 : 0u) | (q.premultiplied ? 1u << 9 : 0u) | layer << 16;
            g.info[2] = q.gradient <= maxGradients ? q.gradient : 0;
            g.info[3] = std::bit_cast<std::uint32_t>(q.stroke);
            std::memcpy(out + std::size_t{i} * sizeof(GpuQuad), &g, sizeof(GpuQuad));
        }
        for (std::size_t i = 0; i < list_.layers.size(); ++i) {
            const WorldLayer& l = list_.layers[i];
            GpuLayer g{};
            g.origin[0] = l.origin.x;
            g.origin[1] = l.origin.y;
            g.origin[2] = l.origin.z;
            g.origin[3] = l.kind == WorldLayer::Kind::Surface ? 1.0f : 0.0f;
            g.axisX[0] = l.axisX.x;
            g.axisX[1] = l.axisX.y;
            g.axisX[2] = l.axisX.z;
            g.axisX[3] = l.depthTested ? 1.0f : 0.0f;
            g.axisY[0] = l.axisY.x;
            g.axisY[1] = l.axisY.y;
            g.axisY[2] = l.axisY.z;
            g.tint[0] = l.tint.r;
            g.tint[1] = l.tint.g;
            g.tint[2] = l.tint.b;
            g.tint[3] = 1.0f;
            std::memcpy(out + (std::size_t{count} + i) * sizeof(GpuQuad), &g, sizeof(GpuLayer));
        }
    }
    list_.quads.clear();
    list_.layers.clear();
    if (count == 0)
        return;

    if (slot.viewsVersion != viewsVersion_) {
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;
        imageInfos.reserve(maxTextures);
        for (std::uint32_t i = 0; i < maxTextures; ++i) {
            if (!views_[i])
                continue;
            imageInfos.push_back({VK_NULL_HANDLE, views_[i], readLayout});
            writes.push_back({
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot.set,
                .dstBinding = 0,
                .dstArrayElement = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &imageInfos.back(),
            });
        }
        const VkDescriptorBufferInfo bufferInfo{slot.quads.get(), 0, VK_WHOLE_SIZE};
        writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &bufferInfo,
        });
        vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        slot.viewsVersion = viewsVersion_;
    }

    // Through the UNORM view when there is one: blending then happens on
    // sRGB-encoded values, like Roblox's GUI (a 50% black box halves the
    // encoded value). Otherwise the hardware blends in linear space.
    const bool unorm = target.unormView != VK_NULL_HANDLE;
    // World layers need the scene's view; its depth, when there, hides them.
    const bool world = target.viewProjection.has_value();
    const bool depthTest = world && target.depthView != VK_NULL_HANDLE;
    VkPipeline pipe = pipeline(unorm ? target.unormFormat : colorFormat_,
                               depthTest ? target.depthFormat : VK_FORMAT_UNDEFINED);
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = unorm ? target.unormView : target.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, target.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = depthTest ? &depth : nullptr,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &slot.set, 0, nullptr);
    Push push{};
    push.params[0] = static_cast<float>(target.extent.width);
    push.params[1] = static_cast<float>(target.extent.height);
    push.params[2] = static_cast<float>(maxGradients);
    push.params[3] = world ? 1.0f : 0.0f;
    push.layerBase[0] = count;
    if (world)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                push.viewProjection[c * 4 + r] = target.viewProjection->m[static_cast<std::size_t>(c)][static_cast<std::size_t>(r)];
    vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(cmd, 4, count, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace lunar::ui
