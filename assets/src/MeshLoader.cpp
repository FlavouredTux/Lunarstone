#include <lunar/assets/MeshLoader.hpp>

#include "DecodedMeshes.hpp"

#include <lunar/assets/MeshSimplify.hpp>
#include <lunar/assets/Web.hpp>

#include <cstdlib>
#include <exception>
#include <format>
#include <print>
#include <utility>

namespace lunar::assets {

struct MeshLoader::Entry {
    ContentId id; // meaningless when the MeshId didn't parse, but then it never loads
    MeshState state = MeshState::Unknown;
    std::shared_ptr<const MeshFile> file;
    std::shared_ptr<const MeshData> mesh; // shares ownership with file
    std::string error;
    std::vector<std::pair<std::string, Callback>> callbacks;
};

MeshLoader::MeshLoader(std::shared_ptr<AssetCache> cache, unsigned threads, bool levelsOfDetail)
    : cache_(std::move(cache)), levelsOfDetail_(levelsOfDetail),
      derived_(std::make_unique<DerivedCache>(cache_->config().decodedDir, cache_->config().decodedCacheBytes)),
      decoded_(std::make_unique<DecodedMeshes>(*derived_, levelsOfDetail))
{
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this](const std::stop_token& stop) { workerLoop(stop); });
}

MeshLoader::~MeshLoader()
{
    // Stop and join here, while the queue and maps the workers use still exist.
    // Every worker is told first and then woken under the lock, rather than
    // trusting the stop token's own wakeup: a teleport froze joining an idle
    // worker that never woke.
    for (std::jthread& worker : workers_)
        worker.request_stop();
    {
        const std::scoped_lock lock(mutex_);
        changed_.notify_all();
    }
    workers_.clear();
}

std::shared_ptr<MeshLoader::Entry> MeshLoader::entryFor(std::string_view meshId, bool start)
{
    std::unique_lock lock(mutex_);
    std::shared_ptr<Entry> entry;
    if (auto found = byId_.find(std::string(meshId)); found != byId_.end()) {
        entry = found->second;
    } else {
        auto id = parseContentId(meshId);
        if (id && (id->kind == ContentId::Kind::Thumbnail || id->kind == ContentId::Kind::Cdn))
            id = std::unexpected(std::format("\"{}\" is an image, not a mesh", meshId));
        if (!id) {
            entry = std::make_shared<Entry>();
            entry->state = MeshState::Failed;
            entry->error = id.error();
        } else {
            auto& shared = byKey_[id->key()];
            if (!shared) {
                shared = std::make_shared<Entry>();
                shared->id = std::move(*id);
            }
            entry = shared;
        }
        byId_.emplace(meshId, entry);
    }

    if (start && entry->state == MeshState::Unknown) {
        entry->state = MeshState::Loading;
        ++pending_;
        queue_.push_back(entry);
        lock.unlock();
        changed_.notify_all();
    }
    return entry;
}

void MeshLoader::request(std::string_view meshId)
{
    entryFor(meshId, true);
}

void MeshLoader::request(std::string_view meshId, Callback done)
{
    auto entry = entryFor(meshId, true);
    std::unique_lock lock(mutex_);
    if (entry->state == MeshState::Loading) {
        entry->callbacks.emplace_back(meshId, std::move(done));
        return;
    }
    auto file = entry->file;
    std::string error = entry->error;
    lock.unlock();
    done(meshId, file, error);
}

std::shared_ptr<const MeshData> MeshLoader::mesh(std::string_view meshId)
{
    auto entry = entryFor(meshId, true);
    const std::scoped_lock lock(mutex_);
    return entry->mesh;
}

std::shared_ptr<const MeshFile> MeshLoader::file(std::string_view meshId)
{
    auto entry = entryFor(meshId, true);
    const std::scoped_lock lock(mutex_);
    return entry->file;
}

MeshState MeshLoader::state(std::string_view meshId) const
{
    const std::scoped_lock lock(mutex_);
    auto found = byId_.find(std::string(meshId));
    return found == byId_.end() ? MeshState::Unknown : found->second->state;
}

std::string MeshLoader::error(std::string_view meshId) const
{
    const std::scoped_lock lock(mutex_);
    auto found = byId_.find(std::string(meshId));
    return found == byId_.end() ? std::string() : found->second->error;
}

std::expected<std::shared_ptr<const MeshFile>, std::string> MeshLoader::load(std::string_view meshId)
{
    auto entry = entryFor(meshId, false);
    std::unique_lock lock(mutex_);
    if (entry->state == MeshState::Unknown) {
        entry->state = MeshState::Loading;
        ++pending_;
        lock.unlock();
        run(entry);
        lock.lock();
    }
    changed_.wait(lock, [&] { return entry->state != MeshState::Loading; });
    if (entry->state == MeshState::Failed)
        return std::unexpected(entry->error);
    return entry->file;
}

void MeshLoader::wait()
{
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return pending_ == 0; });
}

std::size_t MeshLoader::pending() const
{
    const std::scoped_lock lock(mutex_);
    return pending_;
}

std::uint64_t MeshLoader::generation() const
{
    const std::scoped_lock lock(mutex_);
    return generation_;
}

MeshProvider MeshLoader::provider()
{
    return [this](std::string_view meshId) { return mesh(meshId); };
}

void MeshLoader::workerLoop(const std::stop_token& stop)
{
    const HttpStopScope downloads(stop);
    while (true) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [&] { return !queue_.empty(); }))
                return;
            entry = std::move(queue_.front());
            queue_.pop_front();
        }
        run(entry);
    }
}

void MeshLoader::run(const std::shared_ptr<Entry>& entry)
{
    // id is set before the entry can be Loading and never changes after.
    const ContentId& id = entry->id;
    try {
        auto bytes = cache_->fetch(id);
        if (!bytes) {
            finish(entry, nullptr, bytes.error());
            return;
        }
        // Made from these bytes before: read back, not parsed and simplified again.
        if (std::optional<MeshFile> kept = decoded_->find(**bytes)) {
            finish(entry, std::make_shared<const MeshFile>(std::move(*kept)), {});
            return;
        }
        auto parsed = parseMesh(**bytes);
        if (!parsed) {
            finish(entry, nullptr, std::format("{}: {}", id.key(), parsed.error()));
            return;
        }
        if (levelsOfDetail_)
            generateLods(parsed->mesh);
        decoded_->keep(**bytes, *parsed);
        finish(entry, std::make_shared<const MeshFile>(std::move(*parsed)), {});
    } catch (const std::exception& e) {
        finish(entry, nullptr, std::format("{}: {}", id.key(), e.what()));
    }
}

void MeshLoader::finish(const std::shared_ptr<Entry>& entry, std::shared_ptr<const MeshFile> file, std::string error)
{
    std::vector<std::pair<std::string, Callback>> callbacks;
    {
        const std::scoped_lock lock(mutex_);
        if (file) {
            entry->mesh = std::shared_ptr<const MeshData>(file, &file->mesh);
            entry->state = MeshState::Ready;
        } else {
            entry->state = MeshState::Failed;
        }
        if (!error.empty() && std::getenv("LUNAR_MESH_ERRORS")) // research
            std::println(stderr, "mesh failed: {}", error);
        entry->file = std::move(file);
        entry->error = std::move(error);
        callbacks = std::exchange(entry->callbacks, {});
    }
    changed_.notify_all();
    // The entry no longer changes, so it's safe to read without the lock.
    for (auto& [meshId, done] : callbacks)
        done(meshId, entry->file, entry->error);
    // Only now does wait() see it done, so its callbacks have all run.
    {
        const std::scoped_lock lock(mutex_);
        --pending_;
        ++generation_;
    }
    changed_.notify_all();
}

} // namespace lunar::assets
