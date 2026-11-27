#include <lunar/ui/ViewportFrames.hpp>

#include <lunar/render/Context.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/ui/UiRenderer.hpp>

#include <algorithm>
#include <array>
#include <print>
#include <string_view>

namespace lunar::ui {

namespace {

constexpr VkFormat colorFormat = render::OffscreenRenderer::colorFormat;
constexpr VkFormat unormFormat = render::OffscreenRenderer::unormFormat;
// The longest side of a picture: the GUI scales what's bigger up to its rect.
constexpr std::uint32_t maxSide = 2048;

// The light a Roblox colour stands for, the way the client makes them (Lighting).
Color3 squared(Color3 c)
{
    return {c.r * c.r, c.g * c.g, c.b * c.b};
}

// LightDirection is where the light goes; the renderer wants where it's from.
render::Lighting lightingOf(const ViewportFrame& frame)
{
    render::Lighting lighting;
    lighting.transparentBackground = true;
    // The client's pictures have no shadows; off, the light grid stays full too.
    lighting.shadows = false;
    lighting.ambient = squared(frame.ambient());
    lighting.sunColor = squared(frame.lightColor());
    if (const Vector3 towards = -frame.lightDirection(); towards.magnitude() > 0.0f)
        lighting.sunDirection = towards.unit();
    return lighting;
}

render::GraphicsSettings pictureGraphics(render::GraphicsSettings settings)
{
    settings.shadows = false;
    settings.shadowResolution = 16; // cleared every picture and never read
    settings.postEffects = false;
    settings.pixelLights = false;
    settings.drawDistance = render::GraphicsSettings{}.drawDistance;
    settings.rayTracing = false;
    settings.reflectEverything = false;
    settings.ambientOcclusion = false;
    settings.shadowAllLights = false;
    settings.renderScale = 1.0f;
    return settings;
}

} // namespace

struct ViewportFrames::Picture {
    std::weak_ptr<Instance> frame;
    std::shared_ptr<const Image> key; // what the quads name it by
    bool registered = false;

    render::Image image;
    render::UniqueImageView view; // as UNORM, for the GUI
    VkExtent2D extent{};          // what's next drawn at
    bool everDrawn = false;

    std::vector<render::DrawItem> items;
    render::Camera camera;
    render::Lighting lighting;

    std::vector<Connection> connections;
    const Camera* watchedCamera = nullptr;
    bool rewatch = true;       // the tree under the frame changed
    bool stale = true;         // something changed: extract again, and draw
    bool redraw = true;        // items differ from what's on the picture
    std::uint32_t waiting = 0; // assets the items lack for now
    std::uint64_t retryAt = 0;
    std::uint64_t shownAt = 0;
    std::uint64_t drawnAt = 0; // frame_ it was last drawn in

    ~Picture()
    {
        for (Connection& connection : connections)
            connection.disconnect();
    }
};

ViewportFrames::ViewportFrames(render::Context& context, UiRenderer& ui, std::uint32_t slots)
    : context_(&context), ui_(&ui), slots_(std::max(slots, 1u)), graphics_(pictureGraphics({}))
{
    ui_->setPrepass(this, [this](VkCommandBuffer cmd, std::uint32_t slot) { record(cmd, slot); });
}

ViewportFrames::~ViewportFrames()
{
    context_->waitIdle();
    ui_->setPrepass(this, {});
    for (auto& [frame, picture] : pictures_)
        ui_->setExternalImage(picture->key.get(), {});
}

void ViewportFrames::setAssets(Assets assets)
{
    assets_ = std::move(assets);
    if (renderer_)
        renderer_->setMaterialTextures(assets_.materials);
    for (auto& [frame, picture] : pictures_)
        picture->stale = true;
}

void ViewportFrames::setGraphics(const render::GraphicsSettings& settings)
{
    const render::GraphicsSettings wanted = pictureGraphics(settings);
    if (wanted == graphics_)
        return;
    graphics_ = wanted;
    if (renderer_)
        renderer_->setGraphics(graphics_);
    for (auto& [frame, picture] : pictures_)
        picture->redraw = true;
}

bool ViewportFrames::ensureRenderer()
{
    if (renderer_ || failed_)
        return !failed_;
    // A slot per picture of each frame in flight: those are what a slot's
    // buffers and depth wait out.
    auto made = render::MeshRenderer::create(*context_, colorFormat, slots_ * perFrame, false);
    if (!made) {
        std::println(stderr, "ViewportFrame: {}", made.error());
        failed_ = true;
        return false;
    }
    renderer_ = std::move(*made);
    renderer_->setGraphics(graphics_);
    renderer_->setMaterialTextures(assets_.materials);
    return true;
}

void ViewportFrames::beginFrame()
{
    ++frame_;
    planned_.clear();
    starved_ = stillStarved_;
    stillStarved_ = false;
    std::erase_if(retired_, [this](const Retired& old) { return old.at + slots_ + 1 <= recorded_; });
    std::erase_if(pictures_, [this](auto& entry) {
        Picture& picture = *entry.second;
        if (picture.shownAt + keepUnseenFrames >= frame_)
            return false;
        retire(picture);
        return true;
    });
}

void ViewportFrames::retire(Picture& picture)
{
    if (picture.registered)
        ui_->setExternalImage(picture.key.get(), {});
    picture.registered = false;
    if (picture.image.get() != VK_NULL_HANDLE)
        retired_.push_back({std::move(picture.image), std::move(picture.view), recorded_});
}

void ViewportFrames::watch(ViewportFrame& frame, Picture& picture)
{
    for (Connection& connection : picture.connections)
        connection.disconnect();
    picture.connections.clear();
    Picture* at = &picture;
    auto stale = [at](std::string_view) { at->stale = true; };
    auto tree = [at](Instance*) { at->stale = at->rewatch = true; };

    // Not its place or size: scrolling a list changes those, and they don't
    // change what's in the picture (show() goes by the size it's asked for).
    picture.connections.push_back(frame.changed.connect([at](std::string_view property) {
        static constexpr std::array<std::string_view, 4> looks{"Ambient", "CurrentCamera", "LightColor",
                                                               "LightDirection"};
        if (std::ranges::contains(looks, property))
            at->stale = true;
    }));
    picture.connections.push_back(frame.descendantAdded.connect(tree));
    picture.connections.push_back(frame.descendantRemoving.connect(tree));
    auto under = [&](this auto& self, Instance& instance) -> void {
        for (const InstancePtr& child : instance.children()) {
            if (child->isA("GuiObject"))
                continue;
            picture.connections.push_back(child->changed.connect(stale));
            self(*child);
        }
    };
    under(frame);
    picture.watchedCamera = frame.currentCamera();
    if (Camera* camera = frame.currentCamera())
        picture.connections.push_back(camera->changed.connect([at](std::string_view property) {
            if (property == "CFrame" || property == "FieldOfView")
                at->stale = true;
        }));
    picture.rewatch = false;
}

void ViewportFrames::extract(const ViewportFrame& frame, Picture& picture)
{
    // Asked for and not there yet, or never to come: the difference doesn't
    // matter, a count that stays the same draws nothing new.
    std::uint32_t waiting = 0;
    render::MeshProvider meshes;
    if (assets_.meshes)
        meshes = [&](std::string_view id) {
            auto mesh = assets_.meshes(id);
            waiting += !mesh && !id.empty();
            return mesh;
        };
    render::TextureProvider textures;
    if (assets_.textures)
        textures = [&](std::string_view id) {
            auto image = assets_.textures(id);
            waiting += !image && !id.empty();
            return image;
        };
    picture.items = render::extractScene(frame, meshes, textures);
    picture.redraw = picture.redraw || picture.stale || waiting != picture.waiting;
    picture.stale = false;
    picture.waiting = waiting;
    picture.retryAt = frame_ + retryFrames;
}

std::shared_ptr<const Image> ViewportFrames::show(ViewportFrame& frame, std::uint32_t width, std::uint32_t height)
{
    if (failed_)
        return nullptr;
    std::unique_ptr<Picture>& entry = pictures_[&frame];
    // A frame gone and another made where it was isn't the same picture.
    if (entry && entry->frame.lock().get() != &frame) {
        retire(*entry);
        std::erase(planned_, entry.get());
        entry.reset();
    }
    if (!entry) {
        entry = std::make_unique<Picture>();
        entry->frame = frame.weak_from_this();
        auto key = std::make_shared<Image>();
        key->width = 1;
        key->height = 1;
        entry->key = std::move(key);
    }
    Picture& picture = *entry;
    picture.shownAt = frame_;

    const Camera* camera = frame.currentCamera();
    if (picture.rewatch || camera != picture.watchedCamera) {
        watch(frame, picture);
        picture.stale = true;
    }
    if (!camera)
        return nullptr;
    if (picture.stale || (picture.waiting > 0 && frame_ >= picture.retryAt)) {
        extract(frame, picture);
        picture.camera.cframe = camera->cframe();
        picture.camera.fieldOfView = camera->fieldOfView();
        picture.lighting = lightingOf(frame);
    }
    if (picture.items.empty())
        return nullptr;

    // Past the longest side, shrunk with its shape kept.
    const std::uint32_t longest = std::max(width, height);
    const VkExtent2D wanted = longest <= maxSide
                                  ? VkExtent2D{width, height}
                                  : VkExtent2D{std::max(width * maxSide / longest, 1u),
                                               std::max(height * maxSide / longest, 1u)};
    const bool resized = wanted.width != picture.extent.width || wanted.height != picture.extent.height;
    if (!picture.redraw && !resized)
        return picture.key;
    if (std::ranges::contains(planned_, &picture))
        return picture.key;

    // One drawn last frame lets the others have a turn when there are too many.
    const bool turn = !(starved_ && picture.everDrawn && picture.drawnAt + 1 >= frame_);
    if (planned_.size() < perFrame && turn && ensureRenderer()) {
        if (!picture.registered) {
            Picture* at = &picture;
            picture.registered = ui_->setExternalImage(picture.key.get(), [at] { return at->view.get(); });
            if (!picture.registered)
                return nullptr; // every texture's taken: try again next frame
        }
        picture.extent = wanted;
        planned_.push_back(&picture);
        return picture.key;
    }
    stillStarved_ = true;
    return picture.everDrawn ? picture.key : nullptr;
}

void ViewportFrames::target(Picture& picture)
{
    const VkExtent2D size = picture.extent;
    if (picture.image.get() != VK_NULL_HANDLE && picture.image.extent().width == size.width
        && picture.image.extent().height == size.height)
        return;
    if (picture.image.get() != VK_NULL_HANDLE)
        retired_.push_back({std::move(picture.image), std::move(picture.view), recorded_});
    picture.image = render::Image(context_->allocator(), size, colorFormat,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = picture.image.get(),
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = unormFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    render::vkCheck(vkCreateImageView(context_->device(), &viewInfo, nullptr, &view), "vkCreateImageView");
    picture.view = render::UniqueImageView(context_->device(), view);
}

void ViewportFrames::record(VkCommandBuffer cmd, std::uint32_t slot)
{
    ++recorded_;
    if (planned_.empty())
        return;
    // The frames before this one may still be reading what's drawn over.
    const VkMemoryBarrier2 reads{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &reads,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    for (std::uint32_t i = 0; i < planned_.size(); ++i) {
        Picture& picture = *planned_[i];
        target(picture);
        const render::RenderTarget into{
            .image = picture.image.get(),
            .view = picture.image.view(),
            .extent = picture.extent,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .unormView = picture.view.get(),
            .unormFormat = unormFormat,
        };
        renderer_->record(cmd, (slot % slots_) * perFrame + i, into, picture.camera, picture.items, picture.lighting);
        // Textures past the frame's upload budget come with the next one.
        picture.redraw = renderer_->stats().texturesPending > 0;
        picture.everDrawn = true;
        picture.drawnAt = frame_;
        ++drawn_;
    }
    planned_.clear();
}

} // namespace lunar::ui
