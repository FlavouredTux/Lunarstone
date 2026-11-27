#pragma once

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Animator.hpp>
#include <lunar/avatar/Clip.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lunar::avatar {

// Loads animations by AnimationId (rbxassetid://N, the www.roblox.com asset
// URLs, rbxasset:// files) through an AssetCache, in the background, once
// per asset. Thread-safe.
class ClipLoader {
public:
    explicit ClipLoader(std::shared_ptr<assets::AssetCache> cache);
    ~ClipLoader();
    ClipLoader(const ClipLoader&) = delete;
    ClipLoader& operator=(const ClipLoader&) = delete;

    // Starts loading unless it already has; the source gives null until the
    // clip is ready (and forever if it failed).
    AnimationTrack::ClipSource source(std::string_view animationId);
    // The clip if it's loaded, else null after starting the load.
    ClipPtr clip(std::string_view animationId);
    // Null while pending, the clip on success, or the completed load error.
    std::expected<ClipPtr, std::string> poll(std::string_view animationId);
    // Loads on this thread if nobody has yet, else waits for whoever is.
    std::expected<ClipPtr, std::string> load(std::string_view animationId);
    // Blocks until every requested clip has loaded or failed.
    void wait();

    // Adds a clip under an id without loading anything (tests, built-ins).
    void insert(std::string_view animationId, ClipPtr clip);

private:
    struct Entry;
    std::shared_ptr<Entry> entryFor(std::string_view animationId, bool start);
    void run(const std::shared_ptr<Entry>& entry);
    void workerLoop(const std::stop_token& stop);

    // A few loaders for all the clips, started as loads wait for one: a
    // thread for each clip was hundreds in a busy game, each kept (and its
    // stack mapped) until the loader went away.
    static constexpr std::size_t maxWorkers = 4;

    std::shared_ptr<assets::AssetCache> cache_;
    std::mutex mutex_;
    std::condition_variable_any queued_; // workers wait here for a load
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
    std::deque<std::shared_ptr<Entry>> pending_; // asked for, not yet taken by a worker
    std::size_t idle_ = 0;                       // workers waiting for one
    std::vector<std::jthread> workers_;          // last: stopped and joined first
};

} // namespace lunar::avatar
