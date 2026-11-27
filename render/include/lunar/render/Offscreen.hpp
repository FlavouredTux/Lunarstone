#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Renderer.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace lunar::render {

class Context;

// Renders frames into a colour image of its own and copies them back to the
// CPU. For tests, thumbnails and `lunarstone --render`.
class OffscreenRenderer {
public:
    static constexpr VkFormat colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    static constexpr VkFormat unormFormat = VK_FORMAT_R8G8B8A8_UNORM; // RenderTarget::unormView's

    static std::expected<std::unique_ptr<OffscreenRenderer>, std::string> create(Context& context, std::uint32_t width,
                                                                                 std::uint32_t height);

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;
    ~OffscreenRenderer();

    // Draws, waits for the GPU and returns the pixels (sRGB encoded).
    PixelImage render(const Camera& camera, std::span<const DrawItem> items, const Lighting& lighting = {});

    // Turns on measuring the GPU's own time per frame with timestamp queries;
    // what gpuMilliseconds() then says. False when the device can't (nothing
    // is recorded and gpuMilliseconds() stays 0). Off until asked for, so
    // nothing changes for anyone else.
    bool setTimestamps(bool on);
    // What the GPU spent on the last frame, in milliseconds: the time from
    // the first to the last command in its command buffer, 0 when they are off.
    double gpuMilliseconds() const { return gpuMilliseconds_; }
    // What the last render spent copying the picture back, in milliseconds.
    double readbackMilliseconds() const { return readbackMilliseconds_; }

    // As setTimestamps, but for the renderer's per-pass stamps
    // (MeshRenderer::setPassTimestamps): render() then stamps the end of the
    // frame after its readback copy and reads the passes back once the fence
    // has signalled. Off until asked for, and false when the device can't.
    bool setPassTimestamps(bool on);

    MeshRenderer& renderer() { return *renderer_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

private:
    // One query before everything the frame draws and one after the copy back.
    static constexpr std::uint32_t timestampCount = 2;

    OffscreenRenderer(Context& context, std::uint32_t width, std::uint32_t height)
        : context_(&context), width_(width), height_(height)
    {
    }
    void readTimestamps();

    Context* context_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::unique_ptr<MeshRenderer> renderer_;
    Image color_;
    UniqueImageView unormView_; // color_ as UNORM, for overlays
    Buffer readback_;
    UniqueQueryPool timestamps_; // only when setTimestamps(true) has been called
    bool timestampsOn_ = false;
    bool passTimestampsOn_ = false; // the renderer's own pool, setPassTimestamps
    double gpuMilliseconds_ = 0.0;
    double readbackMilliseconds_ = 0.0;
};

} // namespace lunar::render
