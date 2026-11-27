#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/Allocator.hpp>
#include <lunar/render/Camera.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/render/LightClusters.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/LocalLights.hpp>
#include <lunar/render/Passes.hpp>
#include <lunar/render/Scene.hpp>
#include <lunar/render/Texture.hpp>
#include <lunar/render/Vulkan.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::render {

class RayScene;
class EffectPass;
class PostPass;
class HighlightPass;

class Context;
class LightGrid;
class TextureCache;
struct FrameUniforms;

// One colour image to draw into, offscreen or from a swapchain. The renderer
// takes it from any layout (contents are discarded) and leaves it in
// finalLayout: VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL to read it back,
// VK_IMAGE_LAYOUT_PRESENT_SRC_KHR to present it.
struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // The same image viewed as UNORM when the target allows it, so a
    // FrameOverlay can blend in sRGB space the way Roblox's GUI does. Null
    // otherwise; overlays then draw through view.
    VkImageView unormView = VK_NULL_HANDLE;
    VkFormat unormFormat = VK_FORMAT_UNDEFINED;
    // Whether the image can be blitted to (TRANSFER_DST): what a render
    // scale below 1 needs; without it the scene draws at full size.
    bool blitTarget = false;
    // What the renderer hands its FrameOverlay so it can draw into the
    // world (billboards and surface GUIs): the frame's depth, reversed Z and
    // left in VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL for testing against,
    // and the view and projection the scene was drawn with. Null and unset
    // on the targets callers pass in; no depth when the scene was drawn
    // smaller (renderScale) than the target.
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::optional<Mat4> viewProjection;
};

// Draws on top of a recorded frame, into the same target, after the scene
// (the 2D UI). record() runs outside any rendering scope with the target
// in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL and must leave it there; it
// may record copies first. slot is the frame-in-flight slot, as for the
// renderer.
class FrameOverlay {
public:
    virtual ~FrameOverlay() = default;
    virtual void record(VkCommandBuffer cmd, std::uint32_t slot, const RenderTarget& target) = 0;
};

// The sun's shadow map for one view: an orthographic box looking along the
// sunlight, sized to the part of the view frustum within shadowDistance of
// the camera and moved in whole texels so its edges don't crawl.
struct SunShadow {
    Mat4 view;           // world to light space, looking down -Z towards the ground
    Mat4 viewProjection; // world to the map: x, y in [-1, 1], depth in [0, 1]
    float radius = 0.0f; // half the box's width, studs
    float depth = 0.0f;  // studs from the light's eye to the far side

    // The defaults (GraphicsSettings' Average).
    static constexpr std::uint32_t resolution = 2048;
    static constexpr float shadowDistance = 160.0f; // studs from the camera
    static constexpr float casterReach = 1000.0f;   // casters this far sunwards still count
    std::uint32_t texels = resolution;              // this map's, on a side

    static SunShadow fit(const Camera& camera, float aspect, Vector3 sunDirection, float distance = shadowDistance,
                         std::uint32_t texels = resolution);
    // Whether a sphere can cast into the box.
    bool reaches(Vector3 center, float sphereRadius) const;
};

// What the last recorded frame did.
struct FrameStats {
    std::uint32_t items = 0;   // handed in
    std::uint32_t visible = 0; // left after frustum culling
    std::uint32_t tooFar = 0;  // left out past the draw distance
    // Visible items drawn at each level of detail of their meshes (the last
    // counts that level and coarser ones).
    std::array<std::uint32_t, 4> levels{};
    std::uint32_t drawCalls = 0;
    std::uint32_t shadowCasters = 0;
    std::uint64_t triangles = 0;
    std::uint32_t meshesUploaded = 0; // meshes whose buffers were filled this frame. With ray queries
    // each also builds a bottom level, but one whose buffers did not fit still counts here, so this
    // overcounts the builds: blasBuilds is the true number of them.
    std::uint32_t meshesResident = 0;
    std::uint64_t meshBytes = 0;        // vertex and index buffers of the resident meshes
    std::uint64_t deviceBytes = 0;      // all the renderer's device memory (Allocator::allocatedBytes)
    std::uint64_t deviceLocalBytes = 0; // of it, in video memory (Allocator::deviceLocalBytes)
    std::uint64_t memoryBudget = 0;     // what deviceLocalBytes should stay under, see MemoryBudget::target
    std::uint64_t textureBytes = 0;     // the resident textures
    std::uint64_t textureBudget = 0;    // what they may take: the memory budget less everything else
    std::uint32_t texturesUploaded = 0;
    std::uint32_t texturesResident = 0;
    std::uint32_t texturesPending = 0; // waiting for a later frame's upload budget
    std::uint32_t texturesResized = 0; // made finer or coarser to fit the screen and the budget
    int textureQuality = 3;            // TextureQualityLevel: 0 Low .. 3 VeryHigh
    std::uint64_t outOfMemory = 0;     // allocations that found the device full, since the start
    double recordMilliseconds = 0.0;   // CPU time spent in record()
    // Of it, by phase: the slot, budget and lights; culling, sorting and writing the
    // instances; the light grid, uniforms and uploads; the sun's shadow pass; the local
    // lights' shadows; the solid pass; and everything after.
    struct RecordPhases {
        double setup = 0, build = 0, prepare = 0, sun = 0, local = 0, solid = 0, rest = 0;
        // Of build: cull, collectEffects, uploadTextures, sortBlended, orderEffects, sortSolid, writeInstances.
        double steps[7] = {};
    } phases;
    std::uint32_t effectBatches = 0;   // effect geometry drawn (beams, trails...), see addEffects
    std::uint32_t effectTriangles = 0;
    std::uint32_t localLights = 0;        // shining this frame (setLights)
    std::uint32_t pixelLights = 0;        // of them, lighting each pixel near the camera
    std::uint32_t clusteredLights = 0;    // light indices over all the view's clusters
    std::uint32_t shadowedLights = 0;     // with their shadows drawn into the atlas (Future, not ray traced)
    std::uint32_t localShadowCasters = 0; // instances drawn into it, over all six faces of each
    std::uint32_t blasBuilds = 0;         // acceleration structures built this frame
    std::uint32_t tlasBuilds = 0;
    std::uint32_t tlasRefits = 0;   // of tlasBuilds, the last one refitted (RayScene::buildTop)
    std::uint32_t rayInstances = 0; // the instances in the top level one
    bool postEffects = false;       // Lighting's bloom, blur, sun rays or depth of field drawn (PostPass)
};

// Draws DrawItems lit, shaded by material and fogged. Each distinct mesh and
// texture is uploaded once, the first frame it shows up in, and kept on the
// GPU while frames keep using it. Items are culled against the view and the
// draw distance and take the level of detail LodSelector picks for their
// mesh, then opaque ones are drawn instanced (one draw per mesh and level),
// cut-out ones (texture alpha as holes) likewise, decals blended in zIndex
// order, and transparent ones sorted back to front and blended. With
// Lighting::shadows, opaque and cut-out parts that cast shadows are first
// drawn into a sun shadow map around the camera. Lighting::post's effects
// (and ColorCorrection with them) go on last, before the overlay.
//
// Frames are recorded into a caller's command buffer. Each of the
// framesInFlight slots owns its instance, uniform and staging buffers and its
// depth and shadow images; recording into a slot is only allowed once the GPU finished
// the previous frame that used it (the caller waits on that frame's fence).
class MeshRenderer {
public:
    // colorFormat must match the targets' views; prefer an _SRGB format.
    // rayQueries false keeps the renderer off a ray tracing context's ray
    // queries, for small pictures that have no use for them.
    static std::expected<std::unique_ptr<MeshRenderer>, std::string> create(Context& context, VkFormat colorFormat,
                                                                            std::uint32_t framesInFlight = 2,
                                                                            bool rayQueries = true);

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;
    ~MeshRenderer();

    void record(VkCommandBuffer cmd, std::uint32_t slot, const RenderTarget& target, const Camera& camera,
                std::span<const DrawItem> items, const Lighting& lighting = {});

    const FrameStats& stats() const { return stats_; }
    std::size_t meshCount() const { return meshes_.size(); }
    std::size_t textureCount() const;

    // Writes a timestamp at every pass boundary and reads the frame's per-pass
    // GPU time back into passMilliseconds(). Off until asked for: without it
    // no query pool exists and record() records exactly what it did before.
    // Presenter's frames are in flight and never read, which is harmless: a
    // reset in a later frame only makes the numbers of an earlier one stale.
    bool setPassTimestamps(bool on); // false when the device can't write timestamps
    // Reads the frame's pass times, which the caller must have waited for
    // (OffscreenRenderer does, after its fence).
    void readPassTimestamps();
    const PassTimes& passMilliseconds() const { return passMilliseconds_; }
    // Stamps the end of the frame, so the readback copy the caller records
    // after record() is the Readback pass.
    void endPassTimestamps(VkCommandBuffer cmd);

    // Real textures for built-in materials and for stud surfaces; without
    // them (or while they return nothing) the procedural looks stay.
    void setMaterialTextures(MaterialTextureProvider provider) { materialTextures_ = std::move(provider); }
    void setSurfaceTextures(SurfaceTextureProvider provider) { surfaceTextures_ = std::move(provider); }
    // How terrain materials look; without it terrain draws its vertex colours.
    void setTerrainMaterials(TerrainMaterialProvider provider) { terrainMaterials_ = std::move(provider); }

    // A graphics preset's settings (all but terrainDistance, which is the
    // terrain's own); its draw distance replaces setDrawDistance's.
    void setGraphics(const GraphicsSettings& settings);
    const GraphicsSettings& graphics() const { return graphics_; }

    // Parts (not terrain) further than this from the camera aren't drawn,
    // as the client's graphics quality levels do (qualityDrawDistance).
    void setDrawDistance(float studs) { drawDistance_ = studs; }
    float drawDistance() const { return drawDistance_; }

    // Drawn after every frame until replaced or cleared with null; not owned.
    void setOverlay(FrameOverlay* overlay) { overlay_ = overlay; }
    // Asked for effect geometry every frame, blended in with the transparent
    // items, until removed; not owned.
    void addEffects(EffectSource* source);
    void removeEffects(EffectSource* source);
    FrameOverlay* overlay() const { return overlay_; }
    // Asked for the frame's local lights (PointLight, SpotLight,
    // SurfaceLight) every frame until replaced or cleared with null; not
    // owned. They light through the light grid, and with Future lighting
    // (or ray tracing) each pixel near the camera too.
    void setLights(LightSource* source) { lightSource_ = source; }

    VkFormat colorFormat() const { return colorFormat_; }
    static constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    // Ray traced, what the opaque pass and the decals hand the denoiser of
    // the local lights' shared shadow ray (mesh.frag's DENOISE outputs), and
    // the ray's visibility it blurs back and forth.
    static constexpr VkFormat denoiseFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat denoiseVisibilityFormat = VK_FORMAT_R32_SFLOAT;
    // The decal layer: the decals on solid parts, their colour premultiplied,
    // which the opaque pass lights with the surfaces under them. sRGB like
    // the textures it's from, blended in linear.
    static constexpr VkFormat decalLayerFormat = VK_FORMAT_R8G8B8A8_SRGB;
    // Highlights drawn at once, as the client draws (DrawItem::highlight).
    static constexpr std::size_t maxHighlights = 31;
    static constexpr VkFormat shadowFormat = VK_FORMAT_D32_SFLOAT;
    // Local lights' shadow atlas (Future lighting): texels on a side, and
    // of each cube face.
    static constexpr std::uint32_t localShadowAtlas = 4096;
    static constexpr std::uint32_t localShadowTile = 256;
    // Meshes and textures no frame used for this many frames are freed.
    static constexpr std::uint64_t keepUnusedFrames = 240;
    // Mesh bytes on the GPU past which unused meshes go sooner, oldest
    // first; at most half the memory budget.
    static constexpr VkDeviceSize meshBudget = VkDeviceSize{768} << 20;
    // Frames between asking the driver how much memory is free.
    static constexpr std::uint64_t budgetInterval = 30;
    // Texture bytes uploaded per frame at most (but always one texture);
    // the rest wait for later frames. Copying 96 MB into staging took 8-9
    // ms of a frame by itself, 32 MB 3-4 ms: at 8 MB a frame, the textures
    // a room brings come in over a few frames without holding one up.
    static constexpr VkDeviceSize textureUploadBudget = VkDeviceSize{8} << 20;
    // Staging a slot keeps for its next uploads once they're done.
    static constexpr VkDeviceSize spareStagingBytes = VkDeviceSize{64} << 20;

private:
    static constexpr std::size_t maxLevels = 8; // levels of detail kept per mesh
    // Shadowed local lights the atlas holds at once, six tiles each.
    static constexpr std::uint32_t atlasPlaces
        = (localShadowAtlas / localShadowTile) * (localShadowAtlas / localShadowTile) / 6;
    // One timestamp at the frame's start and one after each pass.
    static constexpr std::uint32_t passQueryCount = static_cast<std::uint32_t>(gpuPassCount) + 1;
    struct GpuMesh {
        struct Level {
            std::uint32_t firstIndex = 0;
            std::uint32_t indexCount = 0;
        };
        MeshPtr source; // keeps the key's address from being reused
        Buffer vertices;
        Buffer indices; // every level's, the full one first
        std::array<Level, maxLevels> levels{};
        std::uint32_t levelCount = 1;
        // How far each level strays from the full one (MeshData::lodErrors),
        // when known: those meshes pick levels by screen size.
        std::array<float, maxLevels> errors{};
        bool screenSized = false;
        VkDeviceSize bytes = 0; // of both buffers
        // Not on the GPU (it was full); tried again from this frame on.
        std::uint64_t retryFrame = 0;
        Vector3 center;
        Vector3 halfExtent;
        float radius = 0.0f;
        std::uint64_t lastUsed = 0; // last frame an item referred to it
        // Its bottom level's and buffers' device addresses once it has a
        // bottom level (RayScene), 0 before.
        VkDeviceAddress rayBottom = 0;
        VkDeviceAddress rayVertices = 0;
        VkDeviceAddress rayIndices = 0;
        // Last frame each level was drawn in, opaque and cut out, as which
        // draw; the same for shadow casters.
        std::uint64_t drawFrame[2][maxLevels]{};
        std::uint32_t drawIndex[2][maxLevels]{};
        std::uint64_t shadowFrame[2][maxLevels]{};
        std::uint32_t shadowIndex[2][maxLevels]{};
    };
    struct Slot {
        Buffer instances;
        Buffer shades;              // how each instance shades, read by the fragment shader
        std::uint32_t capacity = 0; // in instances
        Buffer shadowInstances;     // model matrices (and cut-out textures) of shadow casters
        std::uint32_t shadowCapacity = 0;
        Image shadowMap;
        Buffer uniforms;
        VkDescriptorSet descriptors = VK_NULL_HANDLE;
        std::vector<Buffer> staging; // uploads recorded into this slot's last frame
        // Staging buffers free again, for the next uploads: allocating fresh
        // mapped memory every frame took 15-21 ms per 100 MB.
        std::vector<Buffer> spareStaging;
        Image scene; // the 3D image when GraphicsSettings::renderScale shrinks it or post effects read it
        UniqueImageView sceneUnorm; // the same as UNORM, for effects blended in sRGB space
        // Ray queries: what each top-level instance is (RayInstance), and
        // the frame's build scratch.
        Buffer rayInstances;
        std::uint32_t rayCapacity = 0;
        std::vector<Buffer> rayScratch;
        Image depth;
        Image sceneDepth; // depth before the water, which water reads
        Image decalLayer; // decalLayerFormat: drawn after the depth pre-pass, read by the opaque pass
        // Ray traced: the denoiser's inputs (what the shared ray changes,
        // that times what it found, the surface: mesh.frag's outRest,
        // outRestSeen, outGuide), the two visibility images its passes
        // blur from one into the other, and its sets: blurring into [0]
        // (reading [1]) and into [1] (reading [0], as the composite does).
        Image rest;
        Image restSeen;
        Image restGuide;
        Image restVisibility[2];
        VkDescriptorSet denoiseSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        // A scene drawn smaller (renderScale): its depth at the target's size,
        // for the overlay's billboards and surface GUIs to test against.
        Image overlayDepth;
        // The frame's local lights (lights.glsl's LocalLight) and their
        // clusters' words and indices.
        Buffer lights;
        std::uint32_t lightCapacity = 0;
        Buffer clusters;
        std::uint32_t clusterCapacity = 0;
        // Shadowed local lights' cube maps, six faces of localShadowTile
        // texels each (1 x 1 until a frame needs them), their faces'
        // world to atlas matrices, and what's drawn into them.
        Image localShadows;
        bool localShadowsReady = false; // in DEPTH_READ_ONLY_OPTIMAL, its tiles as drawn
        // What each place's tiles were last drawn of (LocalShadow).
        std::vector<std::uint64_t> localShadowSignatures;
        // And what that signature was worked out from: the light and its
        // nearby casters' looks, in order. The same again means the same
        // signature, without testing each caster against the light.
        std::vector<std::uint64_t> localShadowInputs;
        Buffer localShadowMatrices;
        std::uint32_t localShadowMatrixCapacity = 0;
        Buffer localShadowInstances;
        std::uint32_t localShadowInstanceCapacity = 0;
    };

    // What record() works out about a frame, handed from step to step (Renderer.cpp).
    struct Frame;

    explicit MeshRenderer(Context& context);
    void createPipelines();
    void createDescriptors();
    // Ray traced: the denoiser's layout, pool, blur and composite.
    void createDenoiser();
    Slot& prepareSlot(std::uint32_t slot, VkExtent2D extent);
    // A mapped staging buffer of at least `size` bytes for this slot's frame:
    // a spare one if it fits, else a new one. Throws OutOfMemoryError.
    Buffer& stagingBuffer(Slot& slot, VkDeviceSize size);
    void reserveInstances(Slot& slot, std::size_t count);
    GpuMesh* findMesh(const MeshPtr& mesh, std::vector<GpuMesh*>& uploads);
    void upload(VkCommandBuffer cmd, Slot& slot, std::span<GpuMesh* const> meshes);
    // The timestamp that ends a pass's slot in the pool, so the gap to the
    // next is what the pass took; nothing at all when they are off.
    void markPass(VkCommandBuffer cmd, GpuPass pass);
    // Copies the depth drawn so far into slot.sceneDepth, between two passes.
    static void copySceneDepth(VkCommandBuffer cmd, Slot& slot, VkExtent2D extent);
    // The highlights over the scene once it's drawn: `draw(i, onTop)` draws
    // the i-th of `count` highlighted items into the mask if it's of that
    // depth mode; styles[id - 1] says how highlight `id` looks.
    void recordHighlights(VkCommandBuffer cmd, std::uint32_t slot, Slot& s, const RenderTarget& scene,
                          std::size_t count, const std::function<void(std::size_t, bool)>& draw,
                          std::span<const HighlightStyle* const> styles);
    void fillSky(FrameUniforms& uniforms, const Camera& camera, float aspect, const Lighting& lighting);
    // Uploads the light grid's changed chunks and says where it is.
    void recordLightGrid(VkCommandBuffer cmd, Slot& slot, FrameUniforms& uniforms);
    // Writes the frame's lights and clusters into the slot's buffers (made
    // bigger as needed, their descriptors written again).
    // Points the slot's descriptors at its local shadow atlas and matrices.
    void writeLocalShadowDescriptors(Slot& slot);
    // shadowAt: each light's first face matrix in the atlas, or -1.
    void writeLights(Slot& slot, std::span<const LocalLight> lights, bool traced, std::span<const int> shadowAt);
    // Asks the driver for the memory budget now and then, and hands the
    // textures what the rest leaves.
    void updateBudget();
    // A skybox face's mean linear colour, worked out once per image.
    Color3 skyAverage(const ImagePtr& image);

    // record()'s steps, in the order it takes them.
    // The slot's own image the scene is drawn into: at renderScale when
    // scaled, else at the target's size for the post effects to read.
    RenderTarget sizedScene(std::uint32_t slot, const RenderTarget& target, bool scaled);
    // Sorts the items into the frame's passes, culled, and queues the
    // textures they use, as sharp as they show.
    void cull(Frame& frame);
    // The effect sources' geometry, its textures queued with the items'.
    void collectEffects(Frame& frame);
    // Uploads what the frame queued (and the textures tiled everywhere), then
    // sorts in the see-through items that were waiting for theirs.
    void uploadTextures(Frame& frame);
    // The effect batches farthest first, written for the effect pass.
    void orderEffects(Frame& frame);
    // The visible items' instances and shades, and the sun's casters'.
    void writeInstances(Frame& frame);
    // The frame's uniforms, all but the light grid's and the local lights'.
    void fillUniforms(Frame& frame);
    void fillTerrainLayers(FrameUniforms& uniforms, std::span<const TerrainLayer> layers, double time);
    // The local lights lit per pixel, their clusters, and which get shadow
    // maps (and whether those are drawn again).
    void planLocalLights(Frame& frame);
    // Which of `near` get a place in the shadow atlas, nearest first, and
    // where: a light keeps its place while it stays shadowed.
    std::vector<std::pair<std::size_t, std::uint32_t>> placeLocalShadows(std::span<const LocalLight> near,
                                                                         Vector3 eye, bool shadowing);
    // Ray traced: the frame's top level and what mesh.frag reads of each instance.
    void buildRayScene(Frame& frame);
    void recordSunShadow(Frame& frame);
    void recordLocalShadows(Frame& frame);
    // The viewport, scissor, descriptors and instances every scene scope draws with.
    void bindScene(const Frame& frame);
    // The depth pre-pass, the decal layer, the opaque and cut-out items, the
    // sky and the decals; leaves the solid scope open.
    void recordSolid(Frame& frame);
    // The opaque items' depth and the decal layer over it, each in a scope
    // of its own; opens the solid scope after them.
    void recordDecalLayer(Frame& frame);
    // Ray traced: the local lights' shared shadow ray blurred and composited.
    void recordDenoise(Frame& frame);
    // Water, then the transparent items with the effects among them; ends
    // the scene's scope.
    void recordTransparent(Frame& frame);
    void recordPost(Frame& frame);
    void recordUpscale(Frame& frame);
    void recordOverlay(Frame& frame);
    // A scaled scene's depth blitted up to the target's size, for the overlay.
    VkImageView blitOverlayDepth(Frame& frame);
    // Frees the meshes no frame used for a while (sooner over budget) and
    // the textures likewise, and takes the frame's memory stats.
    void releaseUnused();

    Context* context_;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<TextureCache> textures_; // before the layout: it owns the layout's samplers
    UniqueDescriptorSetLayout setLayout_;
    UniqueDescriptorPool descriptorPool_;
    UniqueSampler shadowSampler_;
    UniqueSampler depthSampler_; // plain reads of sceneDepth
    // The client's voxel light grid and its 3D texture (red sky, green sun),
    // which wraps around: texel = world voxel modulo the grid's size.
    std::unique_ptr<LightGrid> lightGrid_;
    // Acceleration structures, when the context has ray queries; before
    // meshes_, whose bottom levels it keeps, so it goes after them.
    std::unique_ptr<RayScene> rays_;
    Image lightGridImage_;
    UniqueSampler lightGridSampler_;
    bool lightGridCleared_ = false;
    // Its local light (RGBA: colour, strength over 120), the client's
    // LightMapTexture; cleared to black.
    Image lightMapImage_;
    LightSource* lightSource_ = nullptr;
    std::vector<LocalLight> lights_; // the frame's, kept for its capacity
    LightClusters clusters_;
    // Each shadowed local light's place in the atlas, kept while it stays
    // shadowed so its tiles can be reused.
    std::unordered_map<const Instance*, std::uint32_t> localShadowPlaces_;
    UniquePipelineLayout layout_;
    UniquePipeline opaque_;
    UniquePipeline cutout_;
    UniquePipeline depthPrepass_;  // the opaque items' depth, before they're shaded
    UniquePipeline depthOnly_;     // the same in a scope of its own, ahead of the decal layer
    UniquePipeline opaqueTerrain_; // the same two with terrain (and water) shading
    UniquePipeline cutoutTerrain_;
    UniquePipeline decal_;      // decals lit over what's behind them, after the opaque pass
    UniquePipeline decalLayer_; // decals on solid parts into the decal layer, before it
    UniquePipeline transparent_;
    UniquePipeline water_;      // terrain water, its vertices moved by the swell
    UniquePipeline waterDepth_; // terrain water's depth before it's blended
    UniquePipeline shadow_;
    UniquePipeline shadowCutout_;
    UniquePipeline localShadow_; // the same into a face of a local light's cube map
    UniquePipeline localShadowCutout_;
    UniquePipeline sky_;
    // Ray traced, the sky drawn with the opaque pass's denoiser outputs bound
    // (it writes none of them), so the decals can follow it in one scope.
    UniquePipeline skyDenoise_;
    // The denoiser (createDenoiser): off without ray queries or with
    // LUNAR_RT_SKIP=denoise.
    bool denoise_ = false;
    UniqueDescriptorSetLayout denoiseSetLayout_;
    UniquePipelineLayout denoiseLayout_;
    UniqueDescriptorPool denoisePool_;
    std::array<UniquePipeline, 4> denoiseBlur_; // steps 1 (from the rays), 2, 4 and 8
    UniquePipeline denoiseComposite_[2]; // adding what the swap adds, and taking away what it takes
    // Highlights' mask over the scene's depth (DepthMode Occluded), and regardless of it (AlwaysOnTop).
    UniquePipeline highlightMask_;
    UniquePipeline highlightMaskOnTop_;
    MaterialTextureProvider materialTextures_;
    SurfaceTextureProvider surfaceTextures_;
    TerrainMaterialProvider terrainMaterials_;
    struct SkyAverage {
        ImagePtr image; // keeps the key's address from being reused
        Color3 mean;
        std::uint64_t lastUsed = 0;
    };
    std::unordered_map<const lunar::Image*, SkyAverage> skyAverages_;
    FrameOverlay* overlay_ = nullptr;
    // Whether depth images can be blitted (overlayDepth is a scaled scene's
    // depth blitted up to the target's size).
    bool depthBlit_ = false;
    std::unique_ptr<EffectPass> effects_;
    std::unique_ptr<PostPass> post_;
    std::unique_ptr<HighlightPass> highlights_;
    std::vector<EffectSource*> effectSources_;
    EffectGeometry effectGeometry_; // the frame's, kept for its capacity
    float drawDistance_ = 100000.0f; // the client's at the top quality levels
    GraphicsSettings graphics_;
    std::vector<Slot> slots_;
    std::unordered_map<const MeshData*, GpuMesh> meshes_;
    MemoryBudget memoryBudget_;
    // Below what allocations last failed at; creeps back up a MiB a frame.
    VkDeviceSize ceiling_ = ~VkDeviceSize{0};
    std::uint64_t meshOutOfMemory_ = 0;
    std::uint64_t failuresSeen_ = 0;
    VkDeviceSize currentMeshBudget_ = meshBudget;
    std::uint64_t frame_ = 0;
    FrameStats stats_;
    // The frame's pass boundaries, when asked for (setPassTimestamps(true)):
    // query 0 is the frame's start, the rest one per pass. Null and every
    // markPass a branch away, so an unmeasured frame records as it always did.
    UniqueQueryPool passTimestamps_;
    PassTimes passMilliseconds_{};
};

} // namespace lunar::render
