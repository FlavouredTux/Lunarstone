#pragma once

#include <lunar/Image.hpp>
#include <lunar/assets/AssetCache.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lunar::assets {

// What the renderer asks for a texture by its content id. Returns null while
// it's loading or if it can't be loaded; never blocks. The same id always
// gives the same pointer. Matches render::TextureProvider.
using TextureProvider = std::function<std::shared_ptr<const Image>(std::string_view contentId)>;

enum class TextureState : std::uint8_t { Unknown, Loading, Ready, Failed };

// The bytes of an image asset, or of something that points at one: a Decal
// asset id (Roblox serves a model holding a Decal whose Texture is the real
// image) or a TexturePack (an XML list of PBR maps; its colour map is the
// image). Returns the content id to follow, or empty for anything else.
std::string followImageIndirection(std::string_view bytes);

// Loads and decodes textures on a pool of worker threads, like MeshLoader
// does meshes: each asset is fetched and decoded once, spellings of one id
// share the work, and failures are remembered. Every method is thread-safe.
// With the cache's decodedDir set, what it decodes is kept there too, and
// the next time those bytes come it maps the file instead of decoding: the
// image's pixels are then the file's (PixelBuffer::external), read in as
// they're touched and dropped from memory again by evict().
class DecodedTextures;
class DerivedCache;

class TextureLoader {
public:
    using Callback = std::function<void(std::string_view contentId, const std::shared_ptr<const Image>& image,
                                        std::string_view error)>;

    // From the binary: the client's texture transcoder keeps at most this
    // many texels on a side (TranscoderTextureDimensionMax = 1024); Roblox
    // stores uploads no bigger either.
    static constexpr std::uint32_t defaultMaxDimension = 1024;

    // Decoding from the cache is CPU work: half the cores (at least four),
    // as terrain LOD and the light grid take, leaves the rest to the game.
    static unsigned defaultThreads();
    // Images bigger than maxDimension on a side are halved until they fit.
    explicit TextureLoader(std::shared_ptr<AssetCache> cache, unsigned threads = defaultThreads(),
                           std::uint32_t maxDimension = defaultMaxDimension);
    ~TextureLoader();
    TextureLoader(const TextureLoader&) = delete;
    TextureLoader& operator=(const TextureLoader&) = delete;

    // Starts loading in the background unless it already has.
    void request(std::string_view contentId);
    // The same, then calls done once the texture is ready or has failed: on
    // the worker that finished it, or right here if that already happened.
    void request(std::string_view contentId, Callback done);

    // The image if it's loaded, else null after starting the load.
    std::shared_ptr<const Image> image(std::string_view contentId);
    TextureState state(std::string_view contentId) const;
    std::string error(std::string_view contentId) const; // empty unless Failed

    // Loads on this thread if nobody has yet (or waits for the worker that is).
    std::expected<std::shared_ptr<const Image>, std::string> load(std::string_view contentId);

    // Blocks until every requested texture has finished.
    void wait();
    std::size_t pending() const;
    // Goes up by one whenever a texture finishes.
    std::uint64_t generation() const;

    TextureProvider provider();

private:
    struct Entry;
    std::shared_ptr<Entry> entryFor(std::string_view contentId, bool start);
    void run(const std::shared_ptr<Entry>& entry);
    void finish(const std::shared_ptr<Entry>& entry, std::shared_ptr<const Image> image, std::string error);
    void workerLoop(const std::stop_token& stop);

    std::shared_ptr<AssetCache> cache_;
    std::uint32_t maxDimension_;
    // Decoded textures kept on disk (AssetConfig::decodedDir), mapped back in.
    std::unique_ptr<DerivedCache> derived_;
    std::unique_ptr<DecodedTextures> decoded_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> byKey_; // canonical ContentId key
    std::unordered_map<std::string, std::shared_ptr<Entry>> byId_;  // as written in the place
    std::deque<std::shared_ptr<Entry>> queue_;
    std::size_t pending_ = 0;
    std::uint64_t generation_ = 0;
    std::vector<std::jthread> workers_; // last, so they stop before the rest goes away
};

} // namespace lunar::assets
