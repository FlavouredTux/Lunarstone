#include <lunar/render/Offscreen.hpp>

#include <lunar/render/Context.hpp>

#include <chrono>
#include <cstring>

namespace lunar::render {

std::expected<std::unique_ptr<OffscreenRenderer>, std::string>
OffscreenRenderer::create(Context& context, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
        return std::unexpected("offscreen target needs a non-zero size");

    std::unique_ptr<OffscreenRenderer> offscreen(new OffscreenRenderer(context, width, height));
    auto renderer = MeshRenderer::create(context, colorFormat, 1);
    if (!renderer)
        return std::unexpected(renderer.error());
    offscreen->renderer_ = std::move(*renderer);

    try {
        offscreen->color_ = Image(context.allocator(), {width, height}, colorFormat,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = offscreen->color_.get(),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = unormFormat,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCheck(vkCreateImageView(context.device(), &viewInfo, nullptr, &view), "vkCreateImageView");
        offscreen->unormView_ = UniqueImageView(context.device(), view);
        offscreen->readback_ = Buffer(context.allocator(), VkDeviceSize{width} * height * sizeof(Rgba),
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback);
    } catch (const VulkanError& e) {
        return std::unexpected(e.what());
    }
    return offscreen;
}

OffscreenRenderer::~OffscreenRenderer() = default;

bool OffscreenRenderer::setTimestamps(bool on)
{
    if (!on) {
        timestampsOn_ = false;
        gpuMilliseconds_ = 0.0;
        return false;
    }
    if (!context_->timestampQueries())
        return false;
    if (!timestamps_) {
        const VkQueryPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = timestampCount,
        };
        VkQueryPool pool = VK_NULL_HANDLE;
        vkCheck(vkCreateQueryPool(context_->device(), &info, nullptr, &pool), "vkCreateQueryPool");
        timestamps_ = UniqueQueryPool(context_->device(), pool);
    }
    timestampsOn_ = true;
    return true;
}

bool OffscreenRenderer::setPassTimestamps(bool on)
{
    // The renderer owns the pool; this only remembers to write the frame's
    // last stamp (after the readback copy) and to read the passes back.
    passTimestampsOn_ = renderer_->setPassTimestamps(on);
    return passTimestampsOn_;
}

// The GPU is done with the frame: the fence signalled, so the queries are in
// without waiting on them again. Ticks wrap after timestampValidBits of them,
// which is far longer than a frame takes.
void OffscreenRenderer::readTimestamps()
{
    std::uint64_t ticks[timestampCount]{};
    const VkResult ready = vkGetQueryPoolResults(context_->device(), timestamps_.get(), 0, timestampCount,
                                                 sizeof(ticks), ticks, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    gpuMilliseconds_
        = ready == VK_SUCCESS ? static_cast<double>(ticks[1] - ticks[0]) * context_->timestampNanoseconds() / 1e6 : 0.0;
}

PixelImage OffscreenRenderer::render(const Camera& camera, std::span<const DrawItem> items, const Lighting& lighting)
{
    const RenderTarget target{
        color_.get(), color_.view(), {width_, height_}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unormView_.get(),
        unormFormat,  true};

    const bool measure = timestampsOn_;
    const bool measurePasses = passTimestampsOn_;
    context_->submitAndWait([&](VkCommandBuffer cmd) {
        if (measure) {
            // No VK_EXT_host_query_reset, so the frame clears them as it starts.
            vkCmdResetQueryPool(cmd, timestamps_.get(), 0, timestampCount);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_.get(), 0);
        }
        renderer_->record(cmd, 0, target, camera, items, lighting);

        VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {width_, height_, 1},
        };
        vkCmdCopyImageToBuffer(cmd, color_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.get(), 1, &region);

        // The frame's last stamp: what is left between here and the overlay's
        // is the copy back, which is the Readback pass.
        if (measurePasses)
            renderer_->endPassTimestamps(cmd);

        // Make the copy visible to the host once the fence signals.
        VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);

        if (measure)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_.get(), 1);
    });

    if (measure)
        readTimestamps();
    // The fence the frame was waited on has signalled, so the pass stamps are
    // in; readTimestamps() above read the whole frame's from the pool beside.
    if (measurePasses)
        renderer_->readPassTimestamps();

    PixelImage image{width_, height_, std::vector<Rgba>(std::size_t{width_} * height_)};
    const auto copying = std::chrono::steady_clock::now();
    std::memcpy(image.pixels.data(), readback_.mapped().data(), image.pixels.size() * sizeof(Rgba));
    readbackMilliseconds_
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copying).count();
    return image;
}

} // namespace lunar::render
