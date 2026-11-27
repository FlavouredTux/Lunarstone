#pragma once

#include <lunar/Mesh.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/MeshFile.hpp>

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

// What the renderer asks for a mesh by its MeshId. Returns null while the mesh
// is loading or if it can't be loaded; never blocks.
using MeshProvider = std::function<std::shared_ptr<const MeshData>(std::string_view meshId)>;

enum class MeshState : std::uint8_t { Unknown, Loading, Ready, Failed };

class DecodedMeshes;
class DerivedCache;

// Loads meshes by MeshId on a pool of worker threads, so a place with hundreds
// of meshes doesn't stall a frame. Each asset is fetched and parsed once:
// spellings of the same id share one load, and failures are remembered too
// (they're reported once, not retried every frame). Every method is
// thread-safe. With the cache's decodedDir set, what it parses (with its
// levels of detail) is kept there too, and read back the next time the same
// bytes come.
class MeshLoader {
public:
    using Callback = std::function<void(std::string_view meshId, const std::shared_ptr<const MeshFile>& file,
                                        std::string_view error)>;

    // With levelsOfDetail, meshes get coarser levels of detail
    // (assets::generateLods) as they load; the ones files carry are dropped,
    // as the client drops them (MeshParseOptions::keepLods).
    explicit MeshLoader(std::shared_ptr<AssetCache> cache, unsigned threads = 8, bool levelsOfDetail = true);
    ~MeshLoader();
    MeshLoader(const MeshLoader&) = delete;
    MeshLoader& operator=(const MeshLoader&) = delete;

    // Starts loading in the background unless it already has.
    void request(std::string_view meshId);
    // The same, then calls done once the mesh is ready or has failed: on the
    // worker thread that finished it, or right here if that already happened.
    void request(std::string_view meshId, Callback done);

    // The mesh if it's loaded, else null after starting the load. Matches MeshProvider.
    std::shared_ptr<const MeshData> mesh(std::string_view meshId);
    // The same with the file's bounds and version, for sizing MeshParts.
    std::shared_ptr<const MeshFile> file(std::string_view meshId);
    MeshState state(std::string_view meshId) const;
    std::string error(std::string_view meshId) const; // empty unless Failed

    // Loads on this thread if nobody has yet (or waits for the worker that is).
    std::expected<std::shared_ptr<const MeshFile>, std::string> load(std::string_view meshId);

    // Blocks until every requested mesh has finished.
    void wait();
    std::size_t pending() const;
    // Goes up by one whenever a mesh finishes, so a scene can tell when to
    // swap placeholders for the meshes that arrived.
    std::uint64_t generation() const;

    MeshProvider provider();

private:
    struct Entry;
    std::shared_ptr<Entry> entryFor(std::string_view meshId, bool start);
    void run(const std::shared_ptr<Entry>& entry);
    void finish(const std::shared_ptr<Entry>& entry, std::shared_ptr<const MeshFile> file, std::string error);
    void workerLoop(const std::stop_token& stop);

    std::shared_ptr<AssetCache> cache_;
    bool levelsOfDetail_ = true;
    // Parsed meshes kept on disk (AssetConfig::decodedDir) and read back.
    std::unique_ptr<DerivedCache> derived_;
    std::unique_ptr<DecodedMeshes> decoded_;
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
