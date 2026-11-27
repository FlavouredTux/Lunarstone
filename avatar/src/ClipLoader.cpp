#include <lunar/avatar/ClipLoader.hpp>

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/ContentId.hpp>

#include <atomic>
#include <condition_variable>
#include <format>
#include <set>
#include <utility>

namespace lunar::avatar {

struct ClipLoader::Entry {
    std::string id; // as asked for, for messages
    std::mutex mutex;
    std::condition_variable done;
    bool started = false;
    bool finished = false;
    std::atomic<std::shared_ptr<const AnimationClipData>> clip;
    std::string error;
};

ClipLoader::ClipLoader(std::shared_ptr<assets::AssetCache> cache) : cache_(std::move(cache)) {}

ClipLoader::~ClipLoader()
{
    // Stopped and joined here, while the queue they wait on still exists;
    // told under the lock so none misses the wakeup. A load not yet taken
    // never runs (nobody can be waiting for it now).
    for (std::jthread& worker : workers_)
        worker.request_stop();
    {
        const std::scoped_lock lock(mutex_);
        queued_.notify_all();
    }
    workers_.clear();
}

void ClipLoader::workerLoop(const std::stop_token& stop)
{
    const assets::HttpStopScope cancellation(stop);
    std::unique_lock lock(mutex_);
    while (true) {
        ++idle_;
        const bool woke = queued_.wait(lock, stop, [&] { return !pending_.empty(); });
        --idle_;
        if (!woke)
            return;
        std::shared_ptr<Entry> entry = std::move(pending_.front());
        pending_.pop_front();
        lock.unlock();
        run(entry);
        lock.lock();
    }
}

namespace {

std::string keyOf(std::string_view animationId)
{
    auto parsed = assets::parseContentId(animationId);
    return parsed ? parsed->key() : std::string(animationId);
}

} // namespace

std::shared_ptr<ClipLoader::Entry> ClipLoader::entryFor(std::string_view animationId, bool start)
{
    std::shared_ptr<Entry> entry;
    bool launch = false;
    {
        std::scoped_lock lock(mutex_);
        auto& slot = entries_[keyOf(animationId)];
        if (!slot) {
            slot = std::make_shared<Entry>();
            slot->id = animationId;
        }
        entry = slot;
        if (start) {
            std::scoped_lock entryLock(entry->mutex);
            if (!entry->started) {
                entry->started = true;
                launch = true;
            }
        }
        if (launch) {
            pending_.push_back(entry);
            // Another worker only when every one is busy.
            if (idle_ < pending_.size() && workers_.size() < maxWorkers)
                workers_.emplace_back([this](const std::stop_token& stop) { workerLoop(stop); });
            else
                queued_.notify_one();
        }
    }
    return entry;
}

void ClipLoader::run(const std::shared_ptr<Entry>& entry)
{
    ClipPtr clip;
    std::string error;
    std::string id = entry->id;
    std::set<std::string> visited;
    for (int depth = 0; depth < 8; ++depth) {
        if (!visited.insert(keyOf(id)).second) {
            error = "cyclic Animation reference";
            break;
        }
        auto bytes = cache_->fetch(id);
        if (!bytes) {
            error = bytes.error();
            break;
        }
        auto model = loadModel(**bytes);
        if (!model) {
            error = model.error();
            break;
        }
        clip = compileAnimationAsset(model->roots);
        if (clip)
            break;
        const Animation* reference = nullptr;
        for (const auto& root : model->roots) {
            reference = instanceCast<Animation>(root.get());
            if (!reference)
                reference = instanceCast<Animation>(root->findFirstChildWhichIsA("Animation", true));
            if (reference && !reference->animationId().empty())
                break;
        }
        if (!reference || reference->animationId().empty()) {
            error = "no KeyframeSequence or Animation in the asset";
            break;
        }
        id = reference->animationId();
    }
    if (!clip && error.empty())
        error = "Animation reference nesting exceeds 8 assets";
    {
        std::scoped_lock lock(entry->mutex);
        entry->clip.store(std::move(clip));
        entry->error = error.empty() ? std::string() : std::format("{}: {}", entry->id, error);
        entry->finished = true;
    }
    entry->done.notify_all();
}

AnimationTrack::ClipSource ClipLoader::source(std::string_view animationId)
{
    std::shared_ptr<Entry> entry = entryFor(animationId, true);
    return [entry] { return entry->clip.load(); };
}

ClipPtr ClipLoader::clip(std::string_view animationId)
{
    return entryFor(animationId, true)->clip.load();
}

std::expected<ClipPtr, std::string> ClipLoader::poll(std::string_view animationId)
{
    auto entry = entryFor(animationId, true);
    const std::scoped_lock lock(entry->mutex);
    if (!entry->finished)
        return ClipPtr{};
    if (!entry->error.empty())
        return std::unexpected(entry->error);
    if (auto clip = entry->clip.load())
        return clip;
    return std::unexpected("no animation clip in completed load");
}

std::expected<ClipPtr, std::string> ClipLoader::load(std::string_view animationId)
{
    std::shared_ptr<Entry> entry = entryFor(animationId, true);
    std::unique_lock lock(entry->mutex);
    entry->done.wait(lock, [&] { return entry->finished; });
    if (!entry->error.empty())
        return std::unexpected(entry->error);
    return entry->clip.load();
}

void ClipLoader::wait()
{
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [key, entry] : entries_)
            entries.push_back(entry);
    }
    for (const auto& entry : entries) {
        std::unique_lock lock(entry->mutex);
        entry->done.wait(lock, [&] { return entry->finished || !entry->started; });
    }
}

void ClipLoader::insert(std::string_view animationId, ClipPtr clip)
{
    std::shared_ptr<Entry> entry = entryFor(animationId, false);
    {
        std::scoped_lock lock(entry->mutex);
        entry->started = true;
        entry->finished = true;
        entry->clip.store(std::move(clip));
    }
    entry->done.notify_all();
}

} // namespace lunar::avatar
