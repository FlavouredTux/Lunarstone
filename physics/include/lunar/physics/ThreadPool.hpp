#pragma once

// A few worker threads for the physics step, used fork-join: run(n, fn)
// calls fn(i) for every i below n, spread over the workers and the calling
// thread, and returns once every call is done. One job at a time, never
// nested: fn must not call run. Which thread runs which i varies, so callers
// keep results per i and combine them in order; then nothing depends on the
// thread count.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lunar::physics {

class ThreadPool {
public:
    // threads counts the caller: 1 starts no workers and runs everything on it.
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned threads() const { return static_cast<unsigned>(workers_.size()) + 1; }
    // An exception from fn comes out here, once every call has finished;
    // the indices not yet started are skipped.
    void run(std::size_t count, std::function_ref<void(std::size_t)> fn);

    // LUNAR_PHYSICS_THREADS when set (1: one thread), else one less than the
    // hardware threads, at most 8: the renderer and the scripts keep one.
    static unsigned defaultThreads();

private:
    void work(const std::stop_token& stop);
    void drain();

    std::mutex mutex_;
    std::condition_variable_any wake_; // workers wait here for a job
    std::condition_variable left_;     // and the caller for them to leave it
    // The job, written only under mutex_ while no worker is inside one.
    std::uint64_t generation_ = 0;
    unsigned inside_ = 0; // workers in the current job
    std::size_t count_ = 0;
    const std::function_ref<void(std::size_t)>* job_ = nullptr; // run's argument
    std::exception_ptr error_;
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> failed_{false};
    std::vector<std::jthread> workers_; // last: stopped and joined first
};

} // namespace lunar::physics
