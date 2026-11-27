#include "render_gpu.hpp"

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Presenter.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

using namespace lunar::render;

// --- RangeAllocator (no GPU) -----------------------------------------------------

TEST(range_allocator_aligns_and_fills)
{
    RangeAllocator ranges(1024);
    CHECK(ranges.allocate(10, 1) == 0u);
    CHECK(ranges.allocate(16, 256) == 256u); // skips to the alignment
    CHECK(ranges.allocate(100, 1) == 10u);   // first fit uses the gap
    CHECK_EQ(ranges.used(), 126u);
    CHECK(!ranges.allocate(2048, 1).has_value());
    CHECK(!ranges.allocate(0, 1).has_value());
    CHECK(!ranges.allocate(8, 3).has_value()); // not a power of two
}

TEST(range_allocator_merges_freed_neighbours)
{
    RangeAllocator ranges(300);
    CHECK(ranges.allocate(100, 1) == 0u);
    CHECK(ranges.allocate(100, 1) == 100u);
    CHECK(ranges.allocate(100, 1) == 200u);
    CHECK(!ranges.allocate(1, 1).has_value());
    ranges.free(0, 100);
    ranges.free(200, 100);
    CHECK_EQ(ranges.freeRangeCount(), 2u);
    CHECK(!ranges.allocate(150, 1).has_value());
    ranges.free(100, 100);
    CHECK_EQ(ranges.freeRangeCount(), 1u);
    CHECK_EQ(ranges.used(), 0u);
    CHECK(ranges.allocate(300, 1) == 0u);
}

// --- Memory budget (no GPU) -----------------------------------------------------

TEST(budget_keeps_others_memory_and_headroom)
{
    constexpr VkDeviceSize mib = VkDeviceSize{1} << 20;
    MemoryBudget budget{.heapSize = 8192 * mib, .available = 2500 * mib, .used = 700 * mib, .allocated = 600 * mib};
    // Without the driver's numbers: half the heap.
    CHECK_EQ(budgetTarget(budget, std::nullopt), 4096 * mib);
    budget.fromDriver = true;
    // 2500 the driver allows, 100 of which the driver itself holds for us,
    // less a sixteenth of the heap.
    CHECK_EQ(budgetTarget(budget, std::nullopt), (2500 - 100 - 512) * mib);
    CHECK_EQ(budgetTarget(budget, 300), 300 * mib);
    budget.available = 200 * mib;
    CHECK_EQ(budgetTarget(budget, std::nullopt), 0u);
}

TEST(budget_override_reads_whole_megabytes)
{
    // NOLINTBEGIN(concurrency-mt-unsafe): the tests run on one thread
    setenv("LUNAR_GPU_BUDGET_MB", "512", 1);
    CHECK(budgetOverride() == std::optional<std::uint64_t>(512));
    for (const char* bad : {"", "0", "-5", "12abc", "lots"}) {
        setenv("LUNAR_GPU_BUDGET_MB", bad, 1);
        CHECK(!budgetOverride().has_value());
    }
    unsetenv("LUNAR_GPU_BUDGET_MB");
    CHECK(!budgetOverride().has_value());
    // NOLINTEND(concurrency-mt-unsafe)
}

TEST(out_of_memory_results_throw_their_own_error)
{
    for (const VkResult result : {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY}) {
        bool caught = false;
        try {
            vkCheck(result, "vkAllocateMemory");
        } catch (const OutOfMemoryError&) {
            caught = true;
        }
        CHECK(caught);
    }
    bool other = false;
    try {
        vkCheck(VK_ERROR_DEVICE_LOST, "vkQueueSubmit2");
    } catch (const VulkanError& e) {
        other = dynamic_cast<const OutOfMemoryError*>(&e) == nullptr;
    }
    CHECK(other);
}

// --- Context and allocator -----------------------------------------------------

GPU_TEST(vulkan_context_is_ready)
{
    CHECK(context.device() != VK_NULL_HANDLE);
    CHECK(context.queue() != VK_NULL_HANDLE);
    CHECK(context.properties().apiVersion >= VK_API_VERSION_1_3);
    CHECK(context.surface() == VK_NULL_HANDLE); // offscreen only
    context.submitAndWait([](VkCommandBuffer) {});
}

GPU_TEST(vulkan_allocator_reports_its_budget)
{
    const MemoryBudget budget = context.allocator().budget();
    CHECK(budget.heapSize > 0);
    CHECK(budget.available <= budget.heapSize);
    CHECK_EQ(budget.allocated, context.allocator().deviceLocalBytes());
    const Buffer buffer(context.allocator(), 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryUsage::GpuOnly);
    CHECK(buffer.memorySize() >= 4096);
    CHECK(context.allocator().deviceLocalBytes() >= buffer.memorySize());
    CHECK(budget.target <= budget.heapSize);
}

GPU_TEST(vulkan_buffers_share_blocks)
{
    Allocator& allocator = context.allocator();
    const std::size_t before = allocator.blockCount();
    {
        std::vector<Buffer> buffers;
        buffers.reserve(16);
        for (int i = 0; i < 16; ++i)
            buffers.emplace_back(allocator, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
        CHECK(allocator.blockCount() <= before + 1);
        for (const Buffer& buffer : buffers) {
            CHECK_EQ(buffer.mapped().size(), 4096u);
            std::memset(buffer.mapped().data(), 0x5A, buffer.mapped().size());
        }
        // Bigger than half a block: memory of its own, no new block.
        const std::size_t blocks = allocator.blockCount();
        Buffer big(allocator, Allocator::blockSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::GpuOnly);
        CHECK(big.get() != VK_NULL_HANDLE);
        CHECK_EQ(allocator.blockCount(), blocks);
    }
    CHECK(allocator.blockCount() <= before + 1);
}

GPU_TEST(vulkan_copies_between_buffers)
{
    Allocator& allocator = context.allocator();
    Buffer source(allocator, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload);
    Buffer middle(allocator, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  MemoryUsage::GpuOnly);
    Buffer target(allocator, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback);
    for (std::size_t i = 0; i < 256; ++i)
        source.mapped()[i] = static_cast<std::byte>(i);

    context.submitAndWait([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, 256};
        vkCmdCopyBuffer(cmd, source.get(), middle.get(), 1, &region);
        VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT,
        };
        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
        vkCmdCopyBuffer(cmd, middle.get(), target.get(), 1, &region);
        vkCmdPipelineBarrier2(cmd, &dependency);
    });
    for (std::size_t i = 0; i < 256; ++i)
        CHECK(target.mapped()[i] == static_cast<std::byte>(i));
}

GPU_TEST(presenter_needs_a_surface)
{
    auto presenter = Presenter::create(context, {640, 480});
    CHECK(!presenter.has_value());
}
