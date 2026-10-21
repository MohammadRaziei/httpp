#pragma once

// Internal. A small thread pool for the code that may block: server handlers and body
// providers. `base` threads stay; up to `max` threads run under load, and the extra ones leave
// again after a few idle seconds. Sleeping threads wake last-in first-out. Header-only so both the core and the tests can use it.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httpp::detail {

class worker_pool {
public:
    worker_pool(std::size_t base, std::size_t max) : base_(std::max<std::size_t>(base, 1)), max_(std::max(max, base_)) {
        std::lock_guard<std::mutex> lk(mu_);
        for (std::size_t i = 0; i < base_; ++i) add_thread_locked(false);
    }
    ~worker_pool() { shutdown(); }
    worker_pool(const worker_pool&) = delete;
    worker_pool& operator=(const worker_pool&) = delete;

    // False once shut down.
    bool enqueue(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return false;
        jobs_.push_back(std::move(job));
        reap_locked();
        wake_locked();
        return true;
    }

    // Finishes the queued jobs, then joins every thread. Safe to call twice.
    void shutdown() {
        std::unordered_map<std::thread::id, std::thread> all;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            all.swap(threads_);
            exited_.clear();
            for (auto* w : idle_) w->cv.notify_one();
        }
        for (auto& [id, t] : all) if (t.joinable()) t.join();
    }

private:
    // One per sleeping thread. Sleepers form a stack: the thread that went to sleep last wakes first,
    // so a light load stays on one or two hot threads instead of rotating through all of them (every
    // extra thread is one more fighting for the Python lock, and one more futex wake).
    struct waiter {
        std::condition_variable cv;
        bool go = false;
    };

    std::size_t live_locked() const { return threads_.size() - exited_.size(); }

    // Makes sure there is a thread on its way for every queued job: wakes the hottest sleeper, or
    // grows the pool when all threads are busy. A thread already woken but not yet running counts,
    // so a burst of jobs does not wake the whole pool for work one thread would finish.
    void wake_locked() {
        if (stop_ || woken_ >= jobs_.size()) return; // after shutdown no thread may be added: nobody would join it
        if (!idle_.empty()) {
            waiter* w = idle_.back();
            idle_.pop_back();
            w->go = true;
            ++woken_;
            w->cv.notify_one();
        } else if (live_locked() < max_) {
            ++woken_;
            add_thread_locked(true);
        }
    }

    void add_thread_locked(bool counted) {
        std::thread t([this, counted] { run(counted); });
        const auto id = t.get_id();
        threads_.emplace(id, std::move(t));
    }

    // Joins threads that left after idling (a finished std::thread keeps its stack until joined).
    void reap_locked() {
        for (auto id : exited_) {
            auto it = threads_.find(id);
            if (it == threads_.end()) continue;
            if (it->second.joinable()) it->second.join(); // it is already past its last unlock
            threads_.erase(it);
        }
        exited_.clear();
    }

    void run(bool counted) {
        std::unique_lock<std::mutex> lk(mu_);
        if (counted) --woken_; // a thread added by wake_locked: it is running now
        for (;;) {
            if (!jobs_.empty()) {
                auto job = std::move(jobs_.front());
                jobs_.pop_front();
                wake_locked(); // more queued work than threads on the way: wake the next one
                lk.unlock();
                try { job(); } catch (...) {} // a job must not kill its worker
                job = nullptr;                // drop captured state outside the lock
                lk.lock();
                continue;
            }
            if (stop_) return;
            waiter w;
            idle_.push_back(&w);
            w.cv.wait_for(lk, std::chrono::seconds(5), [&] { return w.go || stop_; });
            if (w.go) { --woken_; continue; }
            idle_.erase(std::find(idle_.begin(), idle_.end(), &w)); // timed out, or shutting down
            if (stop_) continue;                                    // the queue is finished first
            if (jobs_.empty() && live_locked() > base_) { // idle for a while and above base: leave
                exited_.push_back(std::this_thread::get_id());
                return;
            }
        }
    }

    std::mutex mu_;
    std::deque<std::function<void()>> jobs_;
    std::unordered_map<std::thread::id, std::thread> threads_;
    std::vector<std::thread::id> exited_;
    std::vector<waiter*> idle_; // sleeping threads, the most recent last
    std::size_t base_, max_;
    std::size_t woken_ = 0;     // woken or just added, not yet running
    bool stop_ = false;
};

} // namespace httpp::detail
