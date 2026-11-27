#pragma once

#include <lunar/render/Allocator.hpp>
#include <lunar/render/Renderer.hpp>
#include <lunar/render/Texture.hpp>
#include <lunar/render/Vulkan.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace lunar::render {

class Context;

// Every texture the renderer samples, in one array of sampled images indexed
// per instance. Each distinct image (and colour space) gets one slot, freed
// once no frame used it for a while. Index 0 is a 1x1 white texture and
// stands in for anything not uploaded yet.
//
// Textures stream like the client's TextureManager2: each is kept on the GPU
// only as large as the screen needs (a mip level of the image, never below
// minStreamedSize on its short side), finer levels come in when the camera
// gets closer and the budget allows, and when the budget is exceeded
// textures not seen for a while go, then ones held finer than needed shrink,
// then the texture quality level drops (each level leaves out one more mip,
// non-colour maps one more still, as TextureQualityLevel Low..VeryHigh do).
// Mipmaps are made on the GPU; a level below the image's own is cut out of
// a full mip chain made in a transient image.
//
// Each frame-in-flight slot has its own descriptor set; changes reach a set
// the next time its slot records, after the GPU finished with it, so no set
// is ever written while a frame may read it.
class TextureCache {
public:
    static constexpr std::uint32_t maxTextures = 4096;
    static constexpr std::uint64_t keepUnusedFrames = MeshRenderer::keepUnusedFrames;
    static constexpr VkDeviceSize uploadBudget = MeshRenderer::textureUploadBudget;
    // From the binary: DebugLimitMinTextureResolutionWhenSkipMips = 64.
    static constexpr std::uint32_t minStreamedSize = 64;
    // TextureQualityLevel_Low .. TextureQualityLevel_VeryHigh.
    static constexpr int maxQuality = 3;
    // The texels-across argument of use(): no opinion (just in use, keep
    // what's wanted otherwise), and as sharp as the quality level allows.
    static constexpr float anySize = -1.0f;
    static constexpr float fullSize = std::numeric_limits<float>::infinity();

    TextureCache(Context& context, std::uint32_t slots);
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    ~TextureCache();

    std::uint32_t capacity() const { return capacity_; }
    VkSampler repeatSampler() const { return repeat_.get(); }
    VkSampler clampSampler() const { return clamp_.get(); }

    void beginFrame(std::uint64_t frame);
    // The image's index in the array this frame, or 0 while it waits to be
    // uploaded (and then waiting is set) or can't be: empty, malformed or
    // too big. srgb picks the colour space: on for colour maps, off for data
    // like normals. texels is how many texels across the screen shows of it
    // this frame: 0 for in use but off screen, fullSize for as sharp as
    // allowed, anySize for no opinion.
    std::uint32_t use(const ImagePtr& image, bool srgb, bool* waiting = nullptr, float texels = anySize)
    {
        return image ? lookup(image, srgb, waiting, texels) : 0;
    }
    // Records uploads and resizes queued so far, through a staging buffer
    // of at least the size asked for that lives until the frame is done.
    // Running out of device memory only defers some of them.
    void upload(VkCommandBuffer cmd, const std::function<Buffer&(VkDeviceSize)>& staging);
    // Writes every change slot hasn't seen into its set's binding.
    void updateDescriptors(VkDescriptorSet set, std::uint32_t binding, std::uint32_t slot);
    // Frees textures no frame used for keepUnusedFrames, then fits the rest
    // into the budget, deciding what the next upload() grows or shrinks.
    void collect();

    // Bytes the textures may take on the GPU; the textures shrink to fit.
    void setBudget(VkDeviceSize bytes) { budget_ = bytes; }
    VkDeviceSize budget() const { return budget_; }
    VkDeviceSize residentBytes() const { return residentBytes_; }
    // TextureQualityLevel, 0 (Low) to maxQuality (VeryHigh): lowered while
    // the textures in view don't fit the budget, raised again once they do.
    int quality() const { return quality_; }
    // The highest quality level it may use (a graphics preset's); lowering it
    // shrinks what's resident on the next frames.
    void setQualityCap(int cap);
    // The longest side a texture is drawn at, past streaming's floor (a
    // preset's: Potato's 16), 0 for no cap.
    void setSideCap(std::uint32_t side);
    // Allocations that failed for want of memory since the start.
    std::uint64_t outOfMemoryCount() const { return outOfMemory_; }

    std::size_t resident() const { return resident_; }
    std::size_t pending() const { return queue_.size() + resizes_.size(); }
    std::uint32_t uploadedLastFrame() const { return uploaded_; }
    // Textures resized (finer or coarser) last frame.
    std::uint32_t resizedLastFrame() const { return resized_; }

private:
    struct Entry {
        ImagePtr source;
        Image image;
        std::uint32_t index = 0; // 0 until uploaded
        std::uint32_t skip = 0;  // mip levels of the source left out of image
        // What the screen wants: the coarsest level that is still sharp
        // enough over this frame's uses, that within the quality level, and
        // what it is to be.
        std::uint32_t screen = 0;
        std::uint32_t wanted = 0;
        std::uint32_t target = 0;
        std::uint32_t maxSkip = 0; // coarsest level allowed (minStreamedSize)
        std::uint32_t floor = 0;   // while queued: the coarsest level the budget allows it
        // The source's compressed levels, uploaded as they are (and this
        // many of them intact), when the device samples their format.
        const BlockCompressed* blocks = nullptr;
        std::uint32_t blockLevels = 0;
        bool srgb = true;
        bool valid = false; // can be uploaded at all
        bool queued = false;
        bool resizing = false; // in resizes_
        std::uint64_t lastUsed = 0;
        std::uint64_t lastSeen = 0;    // last frame it was on screen
        std::uint64_t wantedFrame = 0; // frame wanted belongs to
        float texels = 0.0f;           // the most texels across asked for that frame
    };
    struct Retired {
        Image image;
        std::uint32_t index = 0; // 0: the slot stays (a resize, a transient image)
        std::uint64_t frame = 0;
    };

    std::uint32_t lookup(const ImagePtr& image, bool srgb, bool* waiting, float texels);
    // The level for this many texels across, within maxSkip.
    std::uint32_t screenLevel(const Entry& entry, float texels) const;
    // A level made coarse enough for a quality level.
    std::uint32_t atQuality(const Entry& entry, std::uint32_t level, int quality) const;
    void markDirty(std::uint32_t index);
    void retire(Image image, std::uint32_t index);
    void requestResize(Entry& entry, std::uint32_t target);
    // Device memory an entry's image takes with skip levels left out, as
    // the driver counts it (alignment and all).
    VkDeviceSize bytesAt(const Entry& entry, std::uint32_t skip);
    // Bytes staged to upload an entry at a level: the whole RGBA image (the
    // GPU cuts the level out), or the compressed levels from it down.
    static VkDeviceSize stagingBytes(const Entry& entry, std::uint32_t skip);

    Context* context_;
    std::uint32_t capacity_ = 0;
    std::uint32_t maxDimension_ = 0;
    bool blitMips_ = true;
    UniqueSampler repeat_;
    UniqueSampler clamp_;
    Image white_;
    bool whiteReady_ = false;

    std::unordered_map<std::uintptr_t, Entry> entries_; // image address | srgb
    // The last lookup: neighbouring items tend to share a texture.
    std::uintptr_t lastKey_ = 0;
    Entry* lastEntry_ = nullptr;
    std::vector<std::uintptr_t> queue_;   // waiting for their first upload
    std::vector<std::uintptr_t> resizes_; // resident ones to remake at their target
    std::vector<VkImageView> views_;      // per index; null means the white texture
    std::vector<std::uint32_t> freeIndices_;
    std::vector<std::vector<std::uint32_t>> dirty_; // per slot
    std::vector<Retired> retired_;
    bool compressionBC_ = false;
    std::unordered_map<std::uint64_t, VkDeviceSize> imageBytes_; // by size, format and levels
    std::uint64_t frame_ = 0;
    std::size_t resident_ = 0;
    VkDeviceSize residentBytes_ = 0;
    VkDeviceSize budget_ = std::numeric_limits<VkDeviceSize>::max();
    int quality_ = maxQuality;
    int qualityCap_ = maxQuality;
    std::uint32_t sideCap_ = 0;
    std::uint64_t comfortableSince_ = 0; // frame the textures last stopped needing the whole budget
    std::uint64_t outOfMemory_ = 0;
    std::uint32_t uploaded_ = 0;
    std::uint32_t resized_ = 0;
};

// Mip levels for a full chain down to 1x1.
std::uint32_t mipCount(std::uint32_t width, std::uint32_t height);

// Device bytes of an RGBA8 image with every mip level, width x height scaled
// down by skip levels (each side at least 1).
VkDeviceSize mipChainBytes(std::uint32_t width, std::uint32_t height, std::uint32_t skip = 0);

} // namespace lunar::render
