#include <lunar/render/MapRenderer.hpp>

#include <lunar/render/Context.hpp>
#include <lunar/render/Frustum.hpp>

#include "Shaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace lunar::render {

namespace {

// Matches map.vert's push constants.
struct Push {
    float viewProjection[16];
    float eye[4];
    float light[4];
    float params[4]; // lowest point, 1 / height, mode (map.frag)
    float lines[4];  // pixels per stud (at one stud in perspective), terrain cell studs
};
static_assert(sizeof(Push) <= 128, "push constants past the guaranteed minimum");

// The sun the map is lit by, from the south-west and fairly high (north is
// -Z). GUESS: picked by eye.
constexpr Vector3 sunDirection{-0.45f, 0.8f, 0.4f};

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

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess)
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
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

Mat4 orthographic(float width, float height, float nearZ, float farZ)
{
    // depth = (farZ + z) / (farZ - nearZ) for view-space z = -distance.
    Mat4 result;
    result.m[0] = {2.0f / width, 0.0f, 0.0f, 0.0f};
    result.m[1] = {0.0f, -2.0f / height, 0.0f, 0.0f};
    result.m[2] = {0.0f, 0.0f, 1.0f / (farZ - nearZ), 0.0f};
    result.m[3] = {0.0f, 0.0f, farZ / (farZ - nearZ), 1.0f};
    return result;
}

std::expected<std::unique_ptr<MapRenderer>, std::string> MapRenderer::create(Context& context, std::uint32_t slots)
{
    std::unique_ptr<MapRenderer> renderer(new MapRenderer(context));
    try {
        renderer->slots_.resize(std::max(slots, 1u));
        renderer->createPipeline();
    } catch (const VulkanError& e) {
        return std::unexpected(e.what());
    }
    return renderer;
}

MapRenderer::~MapRenderer()
{
    context_->waitIdle();
}

void MapRenderer::createPipeline()
{
    VkDevice device = context_->device();
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    layout_ = UniquePipelineLayout(device, layout);

    const UniqueShaderModule vert = makeShader(device, shaders::mapVert());
    const UniqueShaderModule frag = makeShader(device, shaders::mapFrag());
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert.get(),
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag.get(),
         .pName = "main"},
    };
    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(MapVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(MapInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription attributes[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MapVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(MapVertex, color)},
        {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {5, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(MapInstance, color)},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2,
        .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = 6,
        .pVertexAttributeDescriptions = attributes,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    // Both sides: plenty of meshes are single sheets (leaves, signs), and
    // the shader turns each face's normal towards the camera anyway.
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
    VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL, // reversed Z
    };
    VkPipelineColorBlendAttachmentState blendAttachment{
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
    const VkFormat color = colorFormat;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color,
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
    auto make = [&] {
        VkPipeline pipeline = VK_NULL_HANDLE;
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        return UniquePipeline(device, pipeline);
    };
    faces_ = make();
    // Outlines: lines tested against the faces but not written, blended
    // (they fade with distance). The highlight: the same over everything.
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    depth.depthWriteEnable = VK_FALSE;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    outlines_ = make();
    depth.depthTestEnable = VK_FALSE;
    highlightOutlines_ = make();
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    highlightFaces_ = make();
}

void MapRenderer::setMesh(std::shared_ptr<const MapMesh> mesh)
{
    if (mesh == mesh_)
        return;
    mesh_ = std::move(mesh);
    meshDirty_ = true;
}

void MapRenderer::setDynamic(DynamicKind kind, std::shared_ptr<const MapMesh> mesh)
{
    Dynamic& dynamic = dynamic_[kind];
    if (mesh == dynamic.mesh)
        return;
    dynamic.mesh = std::move(mesh);
    ++dynamic.version;
}

void MapRenderer::request(std::size_t index, const View& view)
{
    if (index >= maxViews || view.extent.width == 0 || view.extent.height == 0)
        return;
    views_[index].view = view;
    views_[index].requested = true;
}

VkImageView MapRenderer::imageView(std::size_t index) const
{
    return index < maxViews && views_[index].drawn ? views_[index].color.view() : VK_NULL_HANDLE;
}

void MapRenderer::retire(Buffer buffer)
{
    if (buffer.get() != VK_NULL_HANDLE)
        retired_.push_back({.frame = frame_, .buffer = std::move(buffer)});
}

void MapRenderer::retire(Image image)
{
    if (image.get() != VK_NULL_HANDLE)
        retired_.push_back({.frame = frame_, .image = std::move(image)});
}

void MapRenderer::upload(VkCommandBuffer cmd, Slot& slot)
{
    Allocator& allocator = context_->allocator();
    if (meshDirty_) {
        meshDirty_ = false;
        retire(std::move(vertices_));
        retire(std::move(indices_));
        retire(std::move(instances_));
        vertices_ = {};
        indices_ = {};
        instances_ = {};
        uploaded_ = nullptr;
        stats_.meshBytes = 0;
        if (mesh_ && !mesh_->empty()) {
            const VkDeviceSize vertexBytes = mesh_->vertices.size() * sizeof(MapVertex);
            const VkDeviceSize indexBytes = mesh_->indices.size() * sizeof(std::uint32_t);
            const VkDeviceSize instanceBytes = mesh_->instances.size() * sizeof(MapInstance);
            try {
                Buffer& staging = slot.staging.emplace_back(allocator, vertexBytes + indexBytes + instanceBytes,
                                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
                std::byte* out = staging.mapped().data();
                std::memcpy(out, mesh_->vertices.data(), vertexBytes);
                std::memcpy(out + vertexBytes, mesh_->indices.data(), indexBytes);
                std::memcpy(out + vertexBytes + indexBytes, mesh_->instances.data(), instanceBytes);
                auto make = [&](VkDeviceSize bytes, VkBufferUsageFlags usage) {
                    return Buffer(allocator, bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly);
                };
                vertices_ = make(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                indices_ = make(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                instances_ = make(instanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                const VkBufferCopy vertexCopy{0, 0, vertexBytes};
                vkCmdCopyBuffer(cmd, staging.get(), vertices_.get(), 1, &vertexCopy);
                const VkBufferCopy indexCopy{vertexBytes, 0, indexBytes};
                vkCmdCopyBuffer(cmd, staging.get(), indices_.get(), 1, &indexCopy);
                const VkBufferCopy instanceCopy{vertexBytes + indexBytes, 0, instanceBytes};
                vkCmdCopyBuffer(cmd, staging.get(), instances_.get(), 1, &instanceCopy);
                const VkMemoryBarrier2 barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
                };
                const VkDependencyInfo dependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &barrier,
                };
                vkCmdPipelineBarrier2(cmd, &dependency);
                uploaded_ = mesh_;
                stats_.meshBytes = vertexBytes + indexBytes + instanceBytes;
            } catch (const OutOfMemoryError&) {
                // No map rather than no game; tried again with the next mesh.
                vertices_ = {};
                indices_ = {};
                instances_ = {};
            }
        }
    }
}

void MapRenderer::uploadDynamic(Slot& slot, DynamicKind kind)
{
    const Dynamic& dynamic = dynamic_[kind];
    SlotDynamic& mine = slot.dynamic[kind];
    if (mine.version == dynamic.version)
        return;
    mine.version = dynamic.version;
    mine.ready = false;
    if (!dynamic.mesh || dynamic.mesh->empty())
        return;
    const MapMesh& mesh = *dynamic.mesh;
    const VkDeviceSize vertexBytes = mesh.vertices.size() * sizeof(MapVertex);
    const VkDeviceSize indexBytes = mesh.indices.size() * sizeof(std::uint32_t);
    const VkDeviceSize instanceBytes = mesh.instances.size() * sizeof(MapInstance);
    const VkDeviceSize total = vertexBytes + indexBytes + instanceBytes;
    if (mine.buffer.size() < total) {
        try {
            mine.buffer
                = Buffer(context_->allocator(), total * 3 / 2,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, MemoryUsage::Stream);
        } catch (const OutOfMemoryError&) {
            mine.buffer = {};
            return;
        }
    }
    std::byte* out = mine.buffer.mapped().data();
    std::memcpy(out, mesh.vertices.data(), vertexBytes);
    std::memcpy(out + vertexBytes, mesh.indices.data(), indexBytes);
    std::memcpy(out + vertexBytes + indexBytes, mesh.instances.data(), instanceBytes);
    VkBuffer buffer = mine.buffer.get();
    mine.buffers = {buffer, buffer, buffer, 0, vertexBytes, vertexBytes + indexBytes};
    mine.ready = true;
}

void MapRenderer::record(VkCommandBuffer cmd, std::uint32_t slotIndex)
{
    ++frame_;
    Slot& slot = slots_[slotIndex % slots_.size()];
    slot.staging.clear(); // the GPU finished this slot's previous frame
    std::erase_if(retired_, [&](const Retired& r) { return r.frame + slots_.size() < frame_; });
    stats_.views = 0;
    stats_.draws = 0;
    stats_.triangles = 0;
    if (std::ranges::none_of(views_, &Target::requested))
        return;
    upload(cmd, slot);
    uploadDynamic(slot, Moving);
    uploadDynamic(slot, Highlight);
    stats_.movingBytes = slot.dynamic[Moving].buffer.size();
    for (Target& target : views_)
        if (target.requested) {
            draw(cmd, target, slot);
            target.requested = false;
        }
    stats_.imageBytes = 0;
    for (const Target& target : views_)
        stats_.imageBytes += target.color.memorySize() + target.depth.memorySize();
}

void MapRenderer::drawMesh(VkCommandBuffer cmd, const MapMesh& mesh, const Buffers& buffers, const Mat4& viewProjection,
                           bool outlines)
{
    const VkBuffer vertexBuffers[2] = {buffers.vertices, buffers.instances};
    const VkDeviceSize offsets[2] = {buffers.vertexOffset, buffers.instanceOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, buffers.indices, buffers.indexOffset, VK_INDEX_TYPE_UINT32);
    const Frustum frustum(viewProjection);

    // World tiles in view, neighbours as one draw, with the identity instance.
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    auto flushTiles = [&] {
        if (count == 0)
            return;
        vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
        ++stats_.draws;
        if (!outlines)
            stats_.triangles += count / 3;
        count = 0;
    };
    // Faces and outlines alternate in the index array, so only one tile at
    // a time when outlining.
    for (const MapTile& tile : mesh.tiles) {
        if (!frustum.intersectsBox(tile.bounds.center(), tile.bounds.extent() * 0.5f)) {
            flushTiles();
            continue;
        }
        if (outlines) {
            flushTiles();
            first = tile.firstEdge;
            count = tile.edgeCount;
            continue;
        }
        if (count > 0 && first + count != tile.firstIndex)
            flushTiles();
        if (count == 0)
            first = tile.firstIndex;
        count += tile.indexCount;
    }
    flushTiles();

    // Shape batches in view; neighbouring ones of the same shape at once.
    std::uint32_t shape = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t instances = 0;
    auto flushBatches = [&] {
        if (instances == 0)
            return;
        const MapShape& s = mesh.shapes[shape];
        if (outlines) {
            if (s.edgeCount > 0)
                vkCmdDrawIndexed(cmd, s.edgeCount, instances, s.firstEdge, s.vertexOffset, firstInstance);
        } else {
            vkCmdDrawIndexed(cmd, s.indexCount, instances, s.firstIndex, s.vertexOffset, firstInstance);
            stats_.triangles += std::uint64_t{s.indexCount / 3} * instances;
        }
        ++stats_.draws;
        instances = 0;
    };
    for (const MapBatch& batch : mesh.batches) {
        if (!frustum.intersectsBox(batch.bounds.center(), batch.bounds.extent() * 0.5f)) {
            flushBatches();
            continue;
        }
        if (instances > 0 && (batch.shape != shape || batch.firstInstance != firstInstance + instances))
            flushBatches();
        if (instances == 0) {
            shape = batch.shape;
            firstInstance = batch.firstInstance;
        }
        instances += batch.instanceCount;
    }
    flushBatches();
}

void MapRenderer::draw(VkCommandBuffer cmd, Target& target, Slot& slot)
{
    const View& view = target.view;
    Allocator& allocator = context_->allocator();
    VkImageLayout colorFrom = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (target.extent.width != view.extent.width || target.extent.height != view.extent.height) {
        retire(std::move(target.color));
        retire(std::move(target.depth));
        target.color
            = Image(allocator, view.extent, colorFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        target.depth = Image(allocator, view.extent, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT);
        target.extent = view.extent;
        target.drawn = false;
    }
    if (!target.drawn)
        colorFrom = VK_IMAGE_LAYOUT_UNDEFINED;
    // Earlier frames may still be sampling the picture: wait for that.
    imageBarrier(cmd, target.color.get(), VK_IMAGE_ASPECT_COLOR_BIT, colorFrom,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cmd, target.depth.get(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.color.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    color.clearValue.color = {{view.background[0], view.background[1], view.background[2], view.background[3]}};
    VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.depth.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    };
    depth.clearValue.depthStencil = {0.0f, 0};
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, target.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    Push push{};
    std::memcpy(push.viewProjection, view.viewProjection.m.data(), sizeof(push.viewProjection));
    push.eye[0] = view.eye.x;
    push.eye[1] = view.eye.y;
    push.eye[2] = view.eye.z;
    push.eye[3] = view.orthographic ? 0.0f : 1.0f;
    const Vector3 sun = sunDirection.unit();
    push.light[0] = sun.x;
    push.light[1] = sun.y;
    push.light[2] = sun.z;
    if (uploaded_) {
        push.params[0] = uploaded_->bounds.min.y;
        push.params[1] = 1.0f / std::max(uploaded_->bounds.extent().y, 1.0f);
        push.lines[1] = uploaded_->terrainCell;
    }
    // Pixels a stud covers: the projection's vertical scale (m11 is -2 /
    // height for orthographic views, -1 / tan(fov / 2) for perspective
    // ones, times the view's own rows) over two, times the image's height.
    {
        const Mat4& vp = view.viewProjection;
        const float scaleY
            = std::sqrt((vp.m[0][1] * vp.m[0][1]) + (vp.m[1][1] * vp.m[1][1]) + (vp.m[2][1] * vp.m[2][1]));
        push.lines[0] = scaleY * 0.5f * static_cast<float>(target.extent.height);
    }
    auto mode = [&](VkPipeline pipeline, float which) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        push.params[2] = which;
        vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
    };
    const bool haveStatic = uploaded_ && vertices_.get() != VK_NULL_HANDLE;
    const Buffers statics{vertices_.get(), indices_.get(), instances_.get(), 0, 0, 0};
    const SlotDynamic& moving = slot.dynamic[Moving];
    const bool haveMoving = view.moving && moving.ready && dynamic_[Moving].mesh;
    const SlotDynamic& highlight = slot.dynamic[Highlight];
    const bool haveHighlight = view.highlight && highlight.ready && dynamic_[Highlight].mesh;
    for (const bool outlines : {false, true}) {
        mode(outlines ? outlines_.get() : faces_.get(), outlines ? 1.0f : 0.0f);
        if (haveStatic)
            drawMesh(cmd, *uploaded_, statics, view.viewProjection, outlines);
        if (haveMoving)
            drawMesh(cmd, *dynamic_[Moving].mesh, moving.buffers, view.viewProjection, outlines);
    }
    if (haveHighlight) {
        if (view.highlightFaces) {
            mode(highlightFaces_.get(), 2.0f);
            drawMesh(cmd, *dynamic_[Highlight].mesh, highlight.buffers, view.viewProjection, false);
        }
        mode(highlightOutlines_.get(), 3.0f);
        drawMesh(cmd, *dynamic_[Highlight].mesh, highlight.buffers, view.viewProjection, true);
    }

    vkCmdEndRendering(cmd);

    imageBarrier(cmd, target.color.get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.drawn = true;
    ++stats_.views;
}

} // namespace lunar::render
