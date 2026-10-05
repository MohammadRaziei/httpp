#pragma once

// Internal. A small, bounded pool of worker threads for the *blocking* client
// operations that are exposed asynchronously (run_async(), async_client). It is
// shared by the whole process and started on first use.
//
// This is deliberately honest about what it is: the requests still use blocking
// sockets, each on one pool thread, so concurrency is bounded by the pool size
// (default: 4 x cores, between 8 and 64). What it buys is that the *caller* is
// never blocked: C++ gets a std::future, Python gets an awaitable.
//
// The pool is intentionally leaked (never destroyed): its threads may be inside
// a network call when the process exits, and joining them during static
// destruction (or Python interpreter shutdown) could hang.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace httpp::detail {

class blocking_pool {
public:
    static blocking_pool& instance() {
        static blocking_pool* pool = new blocking_pool(); // leaked on purpose, see above
        return *pool;
    }

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
            if (jobs_.size() > idle_ && threads_ < max_threads_) { // more work than waiting threads
                threads_++;
                std::thread([this] { work(); }).detach();
            }
        }
        wake_.notify_one();
    }

private:
    blocking_pool() {
        const unsigned hw = std::thread::hardware_concurrency();
        max_threads_ = std::clamp<std::size_t>(static_cast<std::size_t>(hw ? hw : 2) * 4, 8, 64);
    }

    void work() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            idle_++;
            wake_.wait(lock, [this] { return !jobs_.empty(); });
            idle_--;
            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::size_t threads_ = 0;
    std::size_t idle_ = 0;
    std::size_t max_threads_ = 8;
};

} // namespace httpp::detail
