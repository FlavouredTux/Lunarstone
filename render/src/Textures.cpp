#include "Textures.hpp"

#include <lunar/render/Context.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>

namespace lunar::render {

namespace {

constexpr VkImageUsageFlags textureUsage
    = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

UniqueSampler makeSampler(const Context& context, VkSamplerAddressMode mode)
{
    const VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = mode,
        .addressModeV = mode,
        .addressModeW = mode,
        .anisotropyEnable = context.anisotropy() ? VK_TRUE : VK_FALSE,
        .maxAnisotropy
        = context.anisotropy() ? std::min(16.0f, context.properties().limits.maxSamplerAnisotropy) : 1.0f,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(context.device(), &info, nullptr, &sampler), "vkCreateSampler");
    return {context.device(), sampler};
}

bool canBlit(VkPhysicalDevice device, VkFormat format)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(device, format, &properties);
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                                            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (properties.optimalTilingFeatures & needed) == needed;
}

VkImageMemoryBarrier2 levelBarrier(VkImage image, std::uint32_t level, std::uint32_t levels, VkImageLayout from,
                                   VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1},
    };
    if (from == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    } else if (from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // Earlier frames may still be sampling it.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    }
    if (to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    return barrier;
}

void barriers(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier2>& list)
{
    if (list.empty())
        return;
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(list.size()),
        .pImageMemoryBarriers = list.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

std::size_t blockBytes(BlockCompressed::Format format)
{
    return format == BlockCompressed::Format::Bc1 || format == BlockCompressed::Format::Bc4 ? 8 : 16;
}

std::size_t blockLevelBytes(BlockCompressed::Format format, std::uint32_t width, std::uint32_t height)
{
    return ((std::size_t{width} + 3) / 4) * ((std::size_t{height} + 3) / 4) * blockBytes(format);
}

// BC4 and BC5 hold data (one or two channels), never colour: no sRGB form.
VkFormat vulkanFormat(const BlockCompressed* blocks, bool srgb)
{
    if (!blocks)
        return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    switch (blocks->format) {
    case BlockCompressed::Format::Bc1:
        return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case BlockCompressed::Format::Bc2:
        return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case BlockCompressed::Format::Bc3:
        return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case BlockCompressed::Format::Bc4:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case BlockCompressed::Format::Bc5:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case BlockCompressed::Format::Bc7:
        return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

std::int32_t mipSize(std::uint32_t size, std::uint32_t level)
{
    return static_cast<std::int32_t>(std::max(size >> level, 1u));
}

} // namespace

std::uint32_t mipCount(std::uint32_t width, std::uint32_t height)
{
    return static_cast<std::uint32_t>(std::bit_width(std::max({width, height, 1u})));
}

VkDeviceSize mipChainBytes(std::uint32_t width, std::uint32_t height, std::uint32_t skip)
{
    VkDeviceSize bytes = 0;
    const std::uint32_t levels = mipCount(width, height);
    for (std::uint32_t level = std::min(skip, levels - 1); level < levels; ++level)
        bytes += VkDeviceSize{std::max(width >> level, 1u)} * std::max(height >> level, 1u) * 4;
    return bytes;
}

TextureCache::TextureCache(Context& context, std::uint32_t slots) : context_(&context), dirty_(std::max(slots, 1u))
{
    const VkPhysicalDeviceLimits& limits = context.properties().limits;
    // Leave room for the shadow map and anything else in the stage.
    capacity_ = std::min(maxTextures,
                         std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages) - 8);
    maxDimension_ = limits.maxImageDimension2D;
    blitMips_ = canBlit(context.physicalDevice(), VK_FORMAT_R8G8B8A8_SRGB)
                && canBlit(context.physicalDevice(), VK_FORMAT_R8G8B8A8_UNORM);
    compressionBC_ = context.textureCompressionBC();
    repeat_ = makeSampler(context, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    clamp_ = makeSampler(context, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    white_ = Image(context.allocator(), {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, textureUsage, VK_IMAGE_ASPECT_COLOR_BIT);

    views_.assign(capacity_, VK_NULL_HANDLE);
    freeIndices_.reserve(capacity_);
    for (std::uint32_t i = capacity_ - 1; i > 0; --i)
        freeIndices_.push_back(i);
    for (std::uint32_t i = 0; i < capacity_; ++i)
        markDirty(i);
}

TextureCache::~TextureCache() = default;

void TextureCache::markDirty(std::uint32_t index)
{
    for (auto& list : dirty_)
        list.push_back(index);
}

void TextureCache::retire(Image image, std::uint32_t index)
{
    retired_.push_back({std::move(image), index, frame_});
}

void TextureCache::beginFrame(std::uint64_t frame)
{
    frame_ = frame;
    uploaded_ = 0;
    resized_ = 0;
}

void TextureCache::setQualityCap(int cap)
{
    qualityCap_ = std::clamp(cap, 0, maxQuality);
    if (quality_ <= qualityCap_)
        return;
    quality_ = qualityCap_;
    for (auto& [key, entry] : entries_) {
        entry.wanted = atQuality(entry, entry.screen, quality_);
        if (entry.valid && entry.target < entry.wanted)
            requestResize(entry, entry.wanted);
    }
}

void TextureCache::setSideCap(std::uint32_t side)
{
    if (side == sideCap_)
        return;
    sideCap_ = side;
    // Smaller now; bigger again as each is next looked up.
    for (auto& [key, entry] : entries_) {
        entry.wanted = atQuality(entry, entry.screen, quality_);
        if (entry.valid && entry.target < entry.wanted)
            requestResize(entry, entry.wanted);
    }
}

std::uint32_t TextureCache::atQuality(const Entry& entry, std::uint32_t level, int quality) const
{
    // From the binary: each quality level below VeryHigh leaves out one more
    // mip, and non-albedo maps one more besides
    // (DebugAdditionalNumberOfMipsToSkipForNonAlbedoTextures = 1).
    auto skip = static_cast<std::uint32_t>(maxQuality - quality);
    if (skip > 0 && !entry.srgb)
        ++skip;
    std::uint32_t coarsest = entry.maxSkip;
    if (sideCap_ > 0 && entry.valid) {
        // Down to the cap, as far as the levels there are go.
        const std::uint32_t longSide = std::max(entry.source->width, entry.source->height);
        const std::uint32_t levels = entry.blocks ? entry.blockLevels
                                     : blitMips_  ? mipCount(entry.source->width, entry.source->height)
                                                  : 1;
        const auto fit = static_cast<std::uint32_t>(longSide > sideCap_ ? std::bit_width((longSide - 1) / sideCap_) : 0);
        coarsest = std::max(coarsest, std::min(fit, levels - 1));
        skip = std::max(skip, std::min(fit, levels - 1));
    }
    return std::min(std::max(level, skip), coarsest);
}

std::uint32_t TextureCache::screenLevel(const Entry& entry, float texels) const
{
    std::uint32_t level = 0;
    if (texels <= 0.0f) {
        level = entry.maxSkip; // off screen: as small as streaming goes
    } else if (std::isfinite(texels)) {
        // The coarsest level still at least texels across: the exponent of
        // the ratio is floor(log2), read straight from the float's bits.
        const float ratio = static_cast<float>(std::max(entry.source->width, entry.source->height)) / texels;
        if (ratio >= 2.0f)
            level = (std::bit_cast<std::uint32_t>(ratio) >> 23) - 127;
    }
    return std::min(level, entry.maxSkip);
}

std::uint32_t TextureCache::lookup(const ImagePtr& image, bool srgb, bool* waiting, float texels)
{
    const std::uintptr_t key = std::bit_cast<std::uintptr_t>(image.get()) | (srgb ? 1u : 0u);
    Entry* entry = lastEntry_;
    if (key != lastKey_ || !entry) {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            Entry fresh{.source = image, .srgb = srgb};
            // Bad images get an entry too, so they're judged once, but never a queue spot.
            const bool valid = !image->empty() && image->rgba.size() == image->byteSize()
                               && image->width <= maxDimension_ && image->height <= maxDimension_;
            fresh.valid = valid;
            if (valid && image->compressed && compressionBC_) {
                // As many levels as are whole, from the full size down.
                const BlockCompressed& blocks = *image->compressed;
                std::uint32_t levels = 0;
                while (levels < blocks.levels.size() && levels < mipCount(image->width, image->height)
                       && blocks.levels[levels].size()
                              == blockLevelBytes(blocks.format, std::max(image->width >> levels, 1u),
                                                 std::max(image->height >> levels, 1u)))
                    ++levels;
                if (levels > 0) {
                    fresh.blocks = &blocks;
                    fresh.blockLevels = levels;
                }
            }
            if (valid) {
                const std::uint32_t shortSide = std::min(image->width, image->height);
                if ((blitMips_ || fresh.blocks) && shortSide > minStreamedSize)
                    fresh.maxSkip = static_cast<std::uint32_t>(std::bit_width(shortSide / minStreamedSize) - 1);
                // Compressed levels can only be left out, not made.
                if (fresh.blocks)
                    fresh.maxSkip = std::min(fresh.maxSkip, fresh.blockLevels - 1);
                fresh.wanted = fresh.target = atQuality(fresh, 0, quality_);
                fresh.queued = true;
                queue_.push_back(key);
            }
            it = entries_.emplace(key, std::move(fresh)).first;
        }
        lastKey_ = key;
        lastEntry_ = entry = &it->second;
    }
    entry->lastUsed = frame_;
    if (entry->index == 0 && !entry->queued && entry->valid) {
        // Dropped to fit the budget, and wanted again.
        entry->queued = true;
        entry->screen = entry->floor = 0;
        entry->wanted = entry->target = atQuality(*entry, 0, quality_);
        queue_.push_back(key);
    }
    // Many items share a texture: only a bigger one this frame changes what's wanted.
    if (texels >= 0.0f && entry->valid && (entry->wantedFrame != frame_ || texels > entry->texels)) {
        if (texels > 0.0f)
            entry->lastSeen = frame_;
        const std::uint32_t level = screenLevel(*entry, texels);
        if (entry->wantedFrame != frame_ || level < entry->screen) {
            entry->screen = level;
            entry->wanted = atQuality(*entry, level, quality_);
        }
        entry->texels = entry->wantedFrame == frame_ ? std::max(entry->texels, texels) : texels;
        entry->wantedFrame = frame_;
        if (entry->queued)
            entry->target = std::max(entry->wanted, entry->floor); // the first upload is what the screen wants
    }
    if (waiting)
        *waiting = entry->queued;
    return entry->index;
}

VkDeviceSize TextureCache::bytesAt(const Entry& entry, std::uint32_t skip)
{
    const std::uint32_t width = std::max(entry.source->width >> skip, 1u);
    const std::uint32_t height = std::max(entry.source->height >> skip, 1u);
    const VkFormat format = vulkanFormat(entry.blocks, false);
    const std::uint32_t levels = entry.blocks ? entry.blockLevels - std::min(skip, entry.blockLevels - 1)
                                 : blitMips_  ? mipCount(width, height)
                                              : 1;
    // Sides under 2^15, so they, the format and the level count share a key.
    const std::uint64_t key = std::uint64_t{width} | std::uint64_t{height} << 16 | std::uint64_t{levels} << 32
                              | static_cast<std::uint64_t>(format) << 40;
    auto [it, added] = imageBytes_.try_emplace(key, 0);
    if (added) {
        const VkImageCreateInfo image{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = levels,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = textureUsage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VkDeviceImageMemoryRequirements info{.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                                   .pCreateInfo = &image};
        VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vkGetDeviceImageMemoryRequirements(context_->device(), &info, &requirements);
        it->second = entry.blocks ? requirements.memoryRequirements.size
                                  : std::max(requirements.memoryRequirements.size, mipChainBytes(width, height));
    }
    return it->second;
}

VkDeviceSize TextureCache::stagingBytes(const Entry& entry, std::uint32_t skip)
{
    if (!entry.blocks)
        return entry.source->byteSize();
    VkDeviceSize bytes = 0;
    for (std::uint32_t level = skip; level < entry.blockLevels; ++level)
        bytes += entry.blocks->levels[level].size();
    return bytes;
}

void TextureCache::requestResize(Entry& entry, std::uint32_t target)
{
    entry.target = target;
    if (entry.index == 0 || entry.resizing || target == entry.skip)
        return;
    entry.resizing = true;
    resizes_.push_back(std::bit_cast<std::uintptr_t>(entry.source.get()) | (entry.srgb ? 1u : 0u));
}

void TextureCache::upload(VkCommandBuffer cmd, const std::function<Buffer&(VkDeviceSize)>& staging)
{
    struct Job {
        Entry* entry;
        std::uintptr_t key;
        std::uint32_t target; // levels of the source left out of image
        bool fromSource;      // else cut from the entry's current image
        VkDeviceSize offset = 0;
        Image image;
        Image transient; // the full chain the target level comes out of
    };
    std::vector<Job> jobs;
    std::vector<std::uintptr_t> deferred; // back into the queue: no room this frame
    // A frame's staging, at most an eighth of the budget (but always one texture).
    const VkDeviceSize frameBytes = std::min(uploadBudget, std::max(budget_ / 8, VkDeviceSize{16} << 20));
    VkDeviceSize total = whiteReady_ ? 0 : 4;
    std::size_t taken = 0;
    std::size_t indices = freeIndices_.size();
    for (; taken < queue_.size() && indices > 0; ++taken) {
        auto it = entries_.find(queue_[taken]);
        if (it == entries_.end())
            continue; // collected before its turn
        const VkDeviceSize bytes = stagingBytes(it->second, it->second.target);
        if (!jobs.empty() && total + bytes > frameBytes)
            break;
        jobs.push_back(
            {.entry = &it->second, .key = it->first, .target = it->second.target, .fromSource = true, .offset = total});
        total += bytes;
        --indices;
    }
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(taken));
    const std::size_t firstResize = jobs.size();
    std::vector<std::uintptr_t> laterResizes;
    for (std::uintptr_t key : resizes_) {
        auto it = entries_.find(key);
        if (it == entries_.end())
            continue;
        Entry& entry = it->second;
        if (entry.index == 0 || entry.target == entry.skip) {
            entry.resizing = false;
            continue;
        }
        const bool grow = entry.target < entry.skip;
        const VkDeviceSize bytes = grow ? stagingBytes(entry, entry.target) : 0;
        if (grow && total > 0 && total + bytes > frameBytes) {
            laterResizes.push_back(key);
            continue;
        }
        jobs.push_back({.entry = &entry, .key = key, .target = entry.target, .fromSource = grow, .offset = total});
        total += bytes;
    }
    resizes_ = std::move(laterResizes);

    // The images first: what doesn't fit in memory now waits.
    bool outOfMemory = false;
    std::erase_if(jobs, [&](Job& job) {
        const lunar::Image& source = *job.entry->source;
        const VkExtent2D extent{std::max(source.width >> job.target, 1u), std::max(source.height >> job.target, 1u)};
        const VkFormat format = vulkanFormat(job.entry->blocks, job.entry->srgb);
        const std::uint32_t levels = job.entry->blocks ? job.entry->blockLevels - job.target
                                     : blitMips_       ? mipCount(extent.width, extent.height)
                                                       : 1;
        try {
            job.image = Image(context_->allocator(), extent, format, textureUsage, VK_IMAGE_ASPECT_COLOR_BIT, levels);
            if (job.fromSource && job.target > 0 && !job.entry->blocks)
                job.transient = Image(context_->allocator(), {source.width, source.height}, format, textureUsage,
                                      VK_IMAGE_ASPECT_COLOR_BIT, mipCount(source.width, source.height));
            return false;
        } catch (const OutOfMemoryError&) {
            outOfMemory = true;
            if (job.entry->index == 0)
                deferred.push_back(job.key);
            else
                job.entry->resizing = false; // collect() asks again if it still should
            return true;
        }
    });
    if (outOfMemory)
        ++outOfMemory_;
    auto requeue = [&] {
        queue_.insert(queue_.begin(), deferred.begin(), deferred.end());
        for (Job& job : jobs) {
            if (job.entry->index == 0)
                queue_.insert(queue_.begin(), job.key);
            else
                job.entry->resizing = false;
        }
    };
    if (jobs.empty() && whiteReady_) {
        requeue();
        return;
    }

    // Staging only for what comes from the CPU, packed in job order (each
    // on a 16-byte boundary, as compressed copies need).
    total = 0;
    for (Job& job : jobs) {
        if (!job.fromSource)
            continue;
        job.offset = total;
        total = (total + stagingBytes(*job.entry, job.target) + 15) & ~VkDeviceSize{15};
    }
    if (!whiteReady_)
        total += 4;
    Buffer* buffer = nullptr;
    std::byte* out = nullptr;
    if (total > 0) {
        try {
            buffer = &staging(total);
        } catch (const OutOfMemoryError&) {
            ++outOfMemory_;
            requeue();
            return;
        }
        out = buffer->mapped().data();
    }
    queue_.insert(queue_.begin(), deferred.begin(), deferred.end());

    std::vector<VkImageMemoryBarrier2> list;
    if (!whiteReady_) {
        const std::uint32_t white = 0xffffffffu;
        std::memcpy(out + total - 4, &white, 4);
        list.push_back(
            levelBarrier(white_.get(), 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    }
    // Images whose whole chain is made here: fresh full-size ones and the transient ones.
    std::vector<const Image*> chains;
    for (Job& job : jobs) {
        list.push_back(levelBarrier(job.image.get(), 0, job.image.mipLevels(), VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        if (job.fromSource && job.entry->blocks) {
            VkDeviceSize at = job.offset;
            for (std::uint32_t level = job.target; level < job.entry->blockLevels; ++level) {
                const std::vector<std::uint8_t>& data = job.entry->blocks->levels[level];
                std::memcpy(out + at, data.data(), data.size());
                at += data.size();
            }
        } else if (job.fromSource) {
            const lunar::Image& pixels = *job.entry->source;
            std::memcpy(out + job.offset, pixels.rgba.data(), pixels.byteSize());
            // On the GPU now: pixels mapped from a file needn't stay in memory
            // (a resize reads them back in).
            pixels.rgba.evict();
            const Image& chain = job.transient.get() != VK_NULL_HANDLE ? job.transient : job.image;
            if (&chain == &job.transient)
                list.push_back(levelBarrier(chain.get(), 0, chain.mipLevels(), VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
            chains.push_back(&chain);
        } else {
            list.push_back(levelBarrier(job.entry->image.get(), 0, job.entry->image.mipLevels(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        }
    }
    barriers(cmd, list);

    auto copy = [&](VkImage image, VkDeviceSize offset, std::uint32_t width, std::uint32_t height) {
        const VkBufferImageCopy region{
            .bufferOffset = offset,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(cmd, buffer->get(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    };
    if (!whiteReady_)
        copy(white_.get(), total - 4, 1, 1);
    std::vector<VkBufferImageCopy> levelCopies;
    for (const Job& job : jobs) {
        if (!job.fromSource)
            continue;
        if (job.entry->blocks) {
            // Every level as the file has it.
            levelCopies.clear();
            VkDeviceSize at = job.offset;
            for (std::uint32_t level = 0; level < job.image.mipLevels(); ++level) {
                const VkExtent2D extent = job.image.extent();
                levelCopies.push_back({
                    .bufferOffset = at,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                    .imageExtent = {static_cast<std::uint32_t>(mipSize(extent.width, level)),
                                    static_cast<std::uint32_t>(mipSize(extent.height, level)), 1},
                });
                at += job.entry->blocks->levels[job.target + level].size();
            }
            vkCmdCopyBufferToImage(cmd, buffer->get(), job.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(levelCopies.size()), levelCopies.data());
            continue;
        }
        const lunar::Image& pixels = *job.entry->source;
        copy(job.transient.get() != VK_NULL_HANDLE ? job.transient.get() : job.image.get(), job.offset, pixels.width,
             pixels.height);
    }

    // Each level is a linear downscale of the one above, every chain's level
    // at once so the barriers batch.
    std::uint32_t maxLevels = 1;
    for (const Image* chain : chains)
        maxLevels = std::max(maxLevels, chain->mipLevels());
    for (std::uint32_t level = 1; level < maxLevels; ++level) {
        list.clear();
        for (const Image* chain : chains)
            if (level < chain->mipLevels())
                list.push_back(levelBarrier(chain->get(), level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        barriers(cmd, list);
        for (const Image* chain : chains) {
            if (level >= chain->mipLevels())
                continue;
            const VkExtent2D extent = chain->extent();
            const VkImageBlit blit{
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
                .srcOffsets = {{0, 0, 0}, {mipSize(extent.width, level - 1), mipSize(extent.height, level - 1), 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                .dstOffsets = {{0, 0, 0}, {mipSize(extent.width, level), mipSize(extent.height, level), 1}},
            };
            vkCmdBlitImage(cmd, chain->get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain->get(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
    }

    // Smaller images take their levels from a bigger chain: the transient
    // one, or the image they replace.
    list.clear();
    for (const Job& job : jobs)
        if (job.transient.get() != VK_NULL_HANDLE)
            list.push_back(levelBarrier(job.transient.get(), job.transient.mipLevels() - 1, 1,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    barriers(cmd, list);
    std::vector<VkImageCopy> regions;
    for (const Job& job : jobs) {
        const bool cut = job.transient.get() != VK_NULL_HANDLE || !job.fromSource;
        if (!cut)
            continue;
        const Image& from = job.fromSource ? job.transient : job.entry->image;
        const std::uint32_t first = job.fromSource ? job.target : job.target - job.entry->skip;
        regions.clear();
        for (std::uint32_t level = 0; level < job.image.mipLevels() && first + level < from.mipLevels(); ++level) {
            const VkExtent2D extent = job.image.extent();
            regions.push_back({
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, first + level, 0, 1},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                .extent = {static_cast<std::uint32_t>(mipSize(extent.width, level)),
                           static_cast<std::uint32_t>(mipSize(extent.height, level)), 1},
            });
        }
        vkCmdCopyImage(cmd, from.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, job.image.get(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()),
                       regions.data());
    }

    list.clear();
    if (!whiteReady_) {
        list.push_back(levelBarrier(white_.get(), 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        whiteReady_ = true;
    }
    for (const Job& job : jobs) {
        const Image& image = job.image;
        const std::uint32_t last = image.mipLevels() - 1;
        const bool chained = job.fromSource && job.transient.get() == VK_NULL_HANDLE && !job.entry->blocks;
        if (chained && last > 0)
            list.push_back(levelBarrier(image.get(), 0, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        list.push_back(levelBarrier(image.get(), chained ? last : 0, chained ? 1 : image.mipLevels(),
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }
    barriers(cmd, list);

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        Job& job = jobs[i];
        Entry& entry = *job.entry;
        residentBytes_ += job.image.memorySize();
        if (entry.index == 0) {
            entry.index = freeIndices_.back();
            freeIndices_.pop_back();
            entry.queued = false;
            ++resident_;
            ++uploaded_;
        } else {
            residentBytes_ -= entry.image.memorySize();
            retire(std::move(entry.image), 0);
            entry.resizing = false;
            ++resized_;
        }
        if (job.transient.get() != VK_NULL_HANDLE)
            retire(std::move(job.transient), 0);
        entry.image = std::move(job.image);
        entry.skip = job.target;
        views_[entry.index] = entry.image.view();
        markDirty(entry.index);
        // A resize that came after this one was decided goes again.
        if (i >= firstResize && entry.target != entry.skip)
            requestResize(entry, entry.target);
    }
}

void TextureCache::updateDescriptors(VkDescriptorSet set, std::uint32_t binding, std::uint32_t slot)
{
    std::vector<std::uint32_t>& dirty = dirty_.at(slot);
    if (dirty.empty())
        return;
    std::ranges::sort(dirty);
    const auto [first, last] = std::ranges::unique(dirty);
    dirty.erase(first, last);

    std::vector<VkDescriptorImageInfo> images;
    images.reserve(dirty.size());
    for (std::uint32_t index : dirty)
        images.push_back(
            {VK_NULL_HANDLE, views_[index] ? views_[index] : white_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    // Runs of consecutive indices share a write.
    std::vector<VkWriteDescriptorSet> writes;
    for (std::size_t i = 0; i < dirty.size();) {
        std::size_t end = i + 1;
        while (end < dirty.size() && dirty[end] == dirty[end - 1] + 1)
            ++end;
        writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .dstArrayElement = dirty[i],
            .descriptorCount = static_cast<std::uint32_t>(end - i),
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = images.data() + i,
        });
        i = end;
    }
    vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    dirty.clear();
}

void TextureCache::collect()
{
    // A retired image goes once every slot's set has dropped it and the
    // frames that could still have it bound are done.
    const auto slots = static_cast<std::uint64_t>(dirty_.size());
    std::erase_if(retired_, [&](Retired& retired) {
        if (retired.frame + slots + 1 > frame_)
            return false;
        if (retired.index != 0)
            freeIndices_.push_back(retired.index);
        return true;
    });

    lastEntry_ = nullptr;
    auto drop = [&](Entry& entry) {
        if (entry.index != 0) {
            views_[entry.index] = VK_NULL_HANDLE;
            markDirty(entry.index);
            residentBytes_ -= entry.image.memorySize();
            retire(std::move(entry.image), entry.index);
            entry.index = 0;
            --resident_;
        }
    };
    std::erase_if(entries_, [&](auto& pair) {
        Entry& entry = pair.second;
        if (entry.lastUsed + keepUnusedFrames >= frame_)
            return false;
        drop(entry);
        return true;
    });

    // Fit what's to be resident into the budget. Bytes are the targets',
    // so resizes decided but not yet done count as done.
    std::vector<Entry*> live;
    live.reserve(entries_.size());
    VkDeviceSize projected = 0;
    for (auto& [key, entry] : entries_) {
        if (entry.index == 0 && !entry.queued)
            continue;
        live.push_back(&entry);
        projected += bytesAt(entry, entry.target);
    }
    const std::uint64_t recently = slots + 2; // seen within the frames in flight
    auto shrink = [&](Entry& entry, std::uint32_t skip) {
        if (skip <= entry.target)
            return;
        projected -= bytesAt(entry, entry.target) - bytesAt(entry, skip);
        if (entry.queued)
            entry.target = entry.floor = skip;
        else
            requestResize(entry, skip);
    };
    const bool squeezed = projected > budget_;
    if (projected > budget_) {
        // Not used this frame at all: out, longest unused first.
        std::ranges::sort(live, std::less{}, &Entry::lastUsed);
        for (Entry* entry : live) {
            if (projected <= budget_ || entry->lastUsed == frame_)
                break;
            if (entry->index != 0) {
                projected -= bytesAt(*entry, entry->target);
                drop(*entry);
                entry->queued = false; // comes back through use() if it's wanted again
            }
        }
        std::erase_if(live, [](const Entry* entry) { return entry->index == 0 && !entry->queued; });
    }
    if (projected > budget_) {
        // Used but not on screen lately: as small as streaming goes.
        std::ranges::sort(live, std::less{}, &Entry::lastSeen);
        for (Entry* entry : live) {
            if (projected <= budget_ || entry->lastSeen + recently > frame_)
                break;
            shrink(*entry, entry->maxSkip);
        }
    }
    if (projected > budget_) {
        // Finer than the screen needs: what it needs, biggest saving first.
        std::ranges::sort(live, std::greater{}, [&](const Entry* entry) {
            return entry->wanted > entry->target ? bytesAt(*entry, entry->target) - bytesAt(*entry, entry->wanted) : 0;
        });
        for (Entry* entry : live) {
            if (projected <= budget_)
                break;
            shrink(*entry, entry->wanted);
        }
    }
    // Still too much: a level coarser for the biggest ones, as far as the
    // next lower quality level would go, then that quality level (a level
    // at a time) until it fits.
    bool lowered = false;
    while (projected > budget_) {
        std::ranges::sort(live, std::greater{}, [&](const Entry* entry) { return bytesAt(*entry, entry->target); });
        const int lower = std::max(quality_ - 1, 0);
        for (Entry* entry : live) {
            if (projected <= budget_)
                break;
            shrink(*entry, std::min(entry->target + 1, atQuality(*entry, entry->target, lower)));
        }
        if (projected <= budget_ || quality_ == 0)
            break;
        --quality_;
        lowered = true;
        for (Entry* entry : live) {
            entry->wanted = atQuality(*entry, entry->screen, quality_);
            shrink(*entry, atQuality(*entry, entry->target, quality_));
        }
    }
    // Back up a level once what the screen wants at it fits, for a while.
    // GUESS: a second, against flapping (the client damps this too:
    // PerformanceControlTextureStreamingAntiFlapping*).
    if (!lowered && !squeezed && quality_ < qualityCap_) {
        VkDeviceSize finer = 0;
        for (const Entry* entry : live)
            finer += bytesAt(*entry, entry->wantedFrame == frame_ ? atQuality(*entry, entry->screen, quality_ + 1)
                                                                  : entry->target);
        if (finer > budget_)
            comfortableSince_ = frame_;
        else if (frame_ > comfortableSince_ + 60) {
            ++quality_;
            comfortableSince_ = frame_;
        }
    } else {
        comfortableSince_ = frame_;
    }

    // Room left: finer levels for what's on screen and wants them, the most
    // blurred first. Not while squeezing (it would undo it), and not to the
    // brim, so new textures fit without squeezing at once.
    const VkDeviceSize growLimit = budget_ - budget_ / 16;
    if (!squeezed && projected < growLimit) {
        std::vector<Entry*> grow;
        for (Entry* entry : live)
            if (entry->index != 0 && entry->wantedFrame == frame_ && entry->wanted < entry->target)
                grow.push_back(entry);
        std::ranges::sort(grow, std::greater{}, [](const Entry* entry) { return entry->target - entry->wanted; });
        for (Entry* entry : grow) {
            const VkDeviceSize extra = bytesAt(*entry, entry->wanted) - bytesAt(*entry, entry->target);
            if (projected + extra > growLimit)
                continue;
            projected += extra;
            requestResize(*entry, entry->wanted);
        }
    }
    // Resizes that a later decision made moot: target back where it is.
    for (std::uintptr_t key : resizes_)
        if (auto it = entries_.find(key); it != entries_.end() && it->second.target == it->second.skip)
            it->second.resizing = false;
    std::erase_if(resizes_, [&](std::uintptr_t key) {
        auto it = entries_.find(key);
        return it == entries_.end() || !it->second.resizing;
    });
}

} // namespace lunar::render
