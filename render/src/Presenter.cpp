#include <lunar/render/Presenter.hpp>

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ranges>
#include <utility>

namespace lunar::render {

std::expected<std::unique_ptr<Presenter>, std::string> Presenter::create(Context& context, VkExtent2D windowSize,
                                                                         const PresentOptions& options)
{
    if (context.surface() == VK_NULL_HANDLE)
        return std::unexpected("the context has no surface to present to");

    std::unique_ptr<Presenter> presenter(new Presenter(context, options));
    presenter->windowSize_ = windowSize;
    try {
        presenter->createFrames();
        presenter->chooseFormat();
        auto renderer = MeshRenderer::create(context, presenter->format_, framesInFlight);
        if (!renderer)
            return std::unexpected(renderer.error());
        presenter->renderer_ = std::move(*renderer);
        presenter->rebuildSwapchain();
    } catch (const VulkanError& e) {
        return std::unexpected(e.what());
    }
    return presenter;
}

Presenter::~Presenter()
{
    vkDeviceWaitIdle(context_->device());
}

void Presenter::createFrames()
{
    VkDevice device = context_->device();
    for (Frame& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_->queueFamily(),
        };
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCheck(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        frame.pool = UniqueCommandPool(device, pool);

        VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkCheck(vkAllocateCommandBuffers(device, &allocInfo, &frame.cmd), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        vkCheck(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        frame.imageAcquired = UniqueSemaphore(device, semaphore);

        VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                    .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = VK_NULL_HANDLE;
        vkCheck(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        frame.done = UniqueFence(device, fence);
    }
}

void Presenter::chooseFormat()
{
    VkPhysicalDevice gpu = context_->physicalDevice();
    VkSurfaceKHR surface = context_->surface();

    std::uint32_t count = 0;
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr),
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data()),
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formats.empty())
        throw VulkanError("the surface reports no formats");

    // An sRGB format lets the hardware encode the renderer's linear output.
    auto srgb = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& f) {
        return (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB)
               && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    const VkSurfaceFormatKHR chosen = srgb != formats.end() ? *srgb : formats.front();
    format_ = chosen.format;
    colorSpace_ = chosen.colorSpace;

    vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, nullptr),
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(count);
    vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes.data()),
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR; // always supported
    if (!options_.vsync) {
        for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::ranges::contains(modes, preferred)) {
                presentMode_ = preferred;
                break;
            }
        }
    }
}

void Presenter::resize(VkExtent2D windowSize)
{
    if (windowSize.width != windowSize_.width || windowSize.height != windowSize_.height) {
        windowSize_ = windowSize;
        outdated_ = true;
    }
}

void Presenter::rebuildSwapchain()
{
    VkDevice device = context_->device();
    vkCheck(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");

    VkSurfaceCapabilitiesKHR caps;
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_->physicalDevice(), context_->surface(), &caps),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    // 0xFFFFFFFF means the surface takes whatever size the swapchain has (Wayland).
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(windowSize_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(windowSize_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    extent_ = extent;
    if (extent.width == 0 || extent.height == 0)
        return; // minimised; stay outdated until a real size arrives

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    // With a mutable format the images can also be viewed as UNORM, which
    // overlays (the GUI) use to blend in sRGB space.
    unormFormat_ = VK_FORMAT_UNDEFINED;
    if (context_->swapchainMutableFormat()) {
        if (format_ == VK_FORMAT_B8G8R8A8_SRGB)
            unormFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
        else if (format_ == VK_FORMAT_R8G8B8A8_SRGB)
            unormFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    }
    const VkFormat viewFormats[2] = {format_, unormFormat_};
    const VkImageFormatListCreateInfo formatList{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2,
        .pViewFormats = viewFormats,
    };
    blitTarget_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    // Screenshots copy 8-bit RGBA or BGRA pixels as they are.
    const bool eightBit = format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_R8G8B8A8_SRGB
                          || format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_R8G8B8A8_UNORM;
    readable_ = options_.readable && eightBit && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = unormFormat_ != VK_FORMAT_UNDEFINED ? &formatList : nullptr,
        .flags = unormFormat_ != VK_FORMAT_UNDEFINED
                     ? VkSwapchainCreateFlagsKHR{VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR}
                     : VkSwapchainCreateFlagsKHR{0},
        .surface = context_->surface(),
        .minImageCount = imageCount,
        .imageFormat = format_,
        .imageColorSpace = colorSpace_,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        // Blitted to when the scene draws at a render scale, if the surface
        // allows it; copied from for screenshots when asked.
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                      | (readable_ ? VkImageUsageFlags{VK_IMAGE_USAGE_TRANSFER_SRC_BIT} : VkImageUsageFlags{0}),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = presentMode_,
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain_.get(),
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    vkCheck(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
    images_.clear();
    swapchain_ = UniqueSwapchain(device, swapchain);

    std::uint32_t count = 0;
    vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(count);
    vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "vkGetSwapchainImagesKHR");

    for (VkImage image : images) {
        SwapchainImage& entry = images_.emplace_back();
        entry.image = image;

        VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        entry.view = UniqueImageView(device, view);
        if (unormFormat_ != VK_FORMAT_UNDEFINED) {
            viewInfo.format = unormFormat_;
            vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
            entry.unormView = UniqueImageView(device, view);
        }

        VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        vkCheck(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        entry.renderDone = UniqueSemaphore(device, semaphore);
    }
    outdated_ = false;
}

Presenter::FrameResult Presenter::drawFrame(const Camera& camera, std::span<const DrawItem> items,
                                            const Lighting& lighting)
{
    if (outdated_)
        rebuildSwapchain();
    if (outdated_)
        return FrameResult::Skipped;

    using Clock = std::chrono::steady_clock;
    Clock::time_point mark = Clock::now();
    const auto lap = [&mark](double& into) {
        const Clock::time_point now = Clock::now();
        into += std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
    };

    VkDevice device = context_->device();
    Frame& frame = frames_[frameIndex_];
    VkFence done = frame.done.get();
    vkCheck(vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    lap(timings_.fenceWait);

    std::uint32_t imageIndex = 0;
    VkResult acquired = vkAcquireNextImageKHR(device, swapchain_.get(), UINT64_MAX, frame.imageAcquired.get(),
                                              VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        outdated_ = true;
        return FrameResult::Skipped;
    }
    if (acquired != VK_SUBOPTIMAL_KHR)
        vkCheck(acquired, "vkAcquireNextImageKHR");
    vkCheck(vkResetFences(device, 1, &done), "vkResetFences");
    lap(timings_.acquire);

    const SwapchainImage& image = images_[imageIndex];
    vkCheck(vkResetCommandPool(device, frame.pool.get(), 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkCheck(vkBeginCommandBuffer(frame.cmd, &begin), "vkBeginCommandBuffer");
    renderer_->record(frame.cmd, frameIndex_,
                      {image.image, image.view.get(), extent_, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, image.unormView.get(),
                       unormFormat_, blitTarget_},
                      camera, items, lighting);
    const bool capturing = std::exchange(captureRequested_, false) && readable_;
    if (capturing)
        recordCapture(frame.cmd, image.image);
    vkCheck(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");
    lap(timings_.record);

    // The renderer's first barrier waits at colour output, which is where
    // the acquire semaphore is waited on.
    VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.imageAcquired.get(),
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = image.renderDone.get(),
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cmdInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                      .commandBuffer = frame.cmd};
    VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmdInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    vkCheck(vkQueueSubmit2(context_->queue(), 1, &submit, done), "vkQueueSubmit2");
    lap(timings_.submit);
    if (capturing) {
        vkCheck(vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        readCapture();
    }

    VkSemaphore renderDone = image.renderDone.get();
    VkSwapchainKHR swapchain = swapchain_.get();
    VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderDone,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex,
    };
    VkResult presented = vkQueuePresentKHR(context_->queue(), &present);
    lap(timings_.present);
    ++timings_.frames;
    frameIndex_ = (frameIndex_ + 1) % framesInFlight;
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        outdated_ = true;
        return FrameResult::Presented;
    }
    vkCheck(presented, "vkQueuePresentKHR");
    return FrameResult::Presented;
}

void Presenter::recordCapture(VkCommandBuffer cmd, VkImage image)
{
    const VkDeviceSize size = VkDeviceSize{extent_.width} * extent_.height * 4;
    if (capture_.size() < size)
        capture_ = Buffer(context_->allocator(), size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback);
    captureExtent_ = extent_;
    auto layout = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStage,
            .dstAccessMask = dstAccess,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    // Whatever wrote the image last (the scene, its blit, the GUI) is done before the copy.
    layout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
           VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {extent_.width, extent_.height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_.get(), 1, &region);
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT,
           VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
    // The copy is visible to the host once the frame's fence signals.
    const VkMemoryBarrier2 host{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void Presenter::readCapture()
{
    PixelImage image{captureExtent_.width, captureExtent_.height,
                     std::vector<Rgba>(std::size_t{captureExtent_.width} * captureExtent_.height)};
    std::memcpy(image.pixels.data(), capture_.mapped().data(), image.pixels.size() * sizeof(Rgba));
    // The swapchain is BGRA on most surfaces.
    if (format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM)
        for (Rgba& p : image.pixels)
            std::swap(p.r, p.b);
    for (Rgba& p : image.pixels)
        p.a = 255; // the compositor ignores it (opaque surface)
    captured_ = std::move(image);
}

std::optional<PixelImage> Presenter::takeCapture()
{
    return std::exchange(captured_, std::nullopt);
}

} // namespace lunar::render
