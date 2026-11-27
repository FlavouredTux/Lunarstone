#include <lunar/physics/ThreadPool.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace lunar::physics {

// A job's lifetime, all under mutex_: run waits for every worker to have
// left the last job, writes the new one and bumps generation_; a worker
// enters (inside_ + 1) on seeing a new generation and leaves (inside_ - 1)
// once next_ runs past count_; run returns once inside_ is back to 0. A
// worker that wakes late finds the job drained (or the next one), and none
// is ever inside while the job is rewritten, so nothing waits on anything
// but the mutex, and no wait waits on another.

ThreadPool::ThreadPool(unsigned threads)
{
    for (unsigned i = 1; i < threads; ++i)
        workers_.emplace_back([this](const std::stop_token& stop) { work(stop); });
}

// The jthreads ask their loops to stop (which wakes them) and join.
ThreadPool::~ThreadPool() = default;

unsigned ThreadPool::defaultThreads()
{
    if (const char* env = std::getenv("LUNAR_PHYSICS_THREADS")) { // NOLINT(concurrency-mt-unsafe)
        const std::string_view text(env);
        unsigned value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{} && value > 0)
            return std::min(value, 64u);
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(hardware > 1 ? hardware - 1 : 1u, 1u, 8u);
}

void ThreadPool::run(std::size_t count, std::function_ref<void(std::size_t)> fn)
{
    if (workers_.empty() || count <= 1) {
        for (std::size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    {
        std::unique_lock lock(mutex_);
        left_.wait(lock, [&] { return inside_ == 0; }); // a late worker still in the last job
        job_ = &fn;
        count_ = count;
        error_ = nullptr;
        failed_.store(false, std::memory_order_relaxed);
        next_.store(0, std::memory_order_relaxed);
        ++generation_;
    }
    wake_.notify_all();
    drain();
    std::exception_ptr error;
    {
        std::unique_lock lock(mutex_);
        left_.wait(lock, [&] { return inside_ == 0; });
        error = std::exchange(error_, nullptr);
    }
    if (error)
        std::rethrow_exception(error);
}

void ThreadPool::work(const std::stop_token& stop)
{
    std::uint64_t seen = 0;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return generation_ != seen; }))
                return;
            seen = generation_;
            ++inside_;
        }
        drain();
        const std::scoped_lock lock(mutex_);
        if (--inside_ == 0)
            left_.notify_all();
    }
}

void ThreadPool::drain()
{
    try {
        for (std::size_t i = next_.fetch_add(1, std::memory_order_relaxed);
             i < count_ && !failed_.load(std::memory_order_relaxed); i = next_.fetch_add(1, std::memory_order_relaxed))
            (*job_)(i);
    } catch (...) {
        failed_.store(true, std::memory_order_relaxed);
        next_.store(count_, std::memory_order_relaxed);
        const std::scoped_lock lock(mutex_);
        if (!error_)
            error_ = std::current_exception();
    }
}

} // namespace lunar::physics
