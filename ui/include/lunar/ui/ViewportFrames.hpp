#pragma once

#include <lunar/Classes.hpp>
#include <lunar/render/Allocator.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lunar::render {
class Context;
}

namespace lunar::ui {

class UiRenderer;

// Draws ViewportFrames: each one on screen keeps a picture of the parts,
// meshes and unions under it through its CurrentCamera, lit by its Ambient,
// LightColor and LightDirection, that the GUI shows on its rect as an external
// image (GuiEngine::setViewportProvider), so it clips, tints and sorts like
// any image. A picture is drawn again only when something under the frame,
// its camera or its size changed, or an asset it waited for came, by a
// MeshRenderer of its own that nothing is made for until a frame needs it.
// show() reads the instances, so all of it but the recording happens where
// the GUI is drawn.
class ViewportFrames {
public:
    // Pictures drawn in one frame at most; the rest wait for the next frames,
    // their old pictures on show. An inventory's fill the first few frames.
    static constexpr std::uint32_t perFrame = 4;
    // Frames off screen before a picture goes.
    static constexpr std::uint64_t keepUnseenFrames = 240;
    // Frames between looks at whether what a picture waits for has loaded.
    static constexpr std::uint64_t retryFrames = 15;

    // What the parts are drawn with, as for the 3D view. Without providers
    // meshes draw as boxes and nothing is textured.
    struct Assets {
        render::MeshProvider meshes;
        render::TextureProvider textures;
        render::MaterialTextureProvider materials;
    };

    ViewportFrames(render::Context& context, UiRenderer& ui, std::uint32_t slots);
    ~ViewportFrames();
    ViewportFrames(const ViewportFrames&) = delete;
    ViewportFrames& operator=(const ViewportFrames&) = delete;

    void setAssets(Assets assets);
    // The 3D view's settings: what they say about textures and levels of
    // detail goes for the pictures too. Their shadows and effects don't.
    void setGraphics(const render::GraphicsSettings& settings);

    // Before the GUI is drawn each frame.
    void beginFrame();
    // What the GUI draws for the frame at this size: see ViewportProvider.
    std::shared_ptr<const Image> show(ViewportFrame& frame, std::uint32_t width, std::uint32_t height);

    // Pictures drawn so far.
    std::uint64_t drawn() const { return drawn_; }
    std::size_t pictures() const { return pictures_.size(); }

private:
    struct Picture;
    // What has to outlive the frames in flight that may still read it.
    struct Retired {
        render::Image image;
        render::UniqueImageView view;
        std::uint64_t at = 0; // recorded_ when it stopped being shown
    };

    void record(VkCommandBuffer cmd, std::uint32_t slot);
    bool ensureRenderer();
    void watch(ViewportFrame& frame, Picture& picture);
    void extract(const ViewportFrame& frame, Picture& picture);
    void target(Picture& picture);
    void retire(Picture& picture);

    render::Context* context_;
    UiRenderer* ui_;
    std::uint32_t slots_;
    std::unique_ptr<render::MeshRenderer> renderer_;
    bool failed_ = false; // the renderer couldn't be made: no pictures
    Assets assets_;
    render::GraphicsSettings graphics_; // pictureGraphics of the 3D view's
    std::unordered_map<const ViewportFrame*, std::unique_ptr<Picture>> pictures_;
    std::vector<Picture*> planned_; // what this frame's prepass draws
    bool starved_ = false;          // the last frame had more to draw than it could
    bool stillStarved_ = false;
    std::vector<Retired> retired_;
    std::uint64_t frame_ = 0;    // beginFrame()s
    std::uint64_t recorded_ = 0; // prepasses
    std::uint64_t drawn_ = 0;
};

} // namespace lunar::ui
