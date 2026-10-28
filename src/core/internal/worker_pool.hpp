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
    worker_pool(std::size_t base, std::size_t max, std::chrono::microseconds grow_after = std::chrono::microseconds(500))
        : base_(std::max<std::size_t>(base, 1)), max_(std::max(max, base_)), help_after(grow_after) {
        std::lock_guard<std::mutex> lk(mu_);
        for (std::size_t i = 0; i < base_; ++i) add_thread_locked(false);
        scout_ = std::thread([this] { scout(); });
    }
    ~worker_pool() { shutdown(); }
    worker_pool(const worker_pool&) = delete;
    worker_pool& operator=(const worker_pool&) = delete;

    // False once shut down.
    bool enqueue(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return false;
        jobs_.push_back({std::move(job), std::chrono::steady_clock::now()});
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
            scout_cv_.notify_one();
        }
        for (auto& [id, t] : all) if (t.joinable()) t.join();
        if (scout_.joinable()) scout_.join();
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
    // While some thread is running or about to run, a new job is NOT handed to another thread at once:
    // that thread usually finishes its job in microseconds and takes the next one itself, which beats
    // a second thread (two threads handing the Python lock back and forth is slower than one). The
    // scout steps in when a job has waited `help_after`, which a blocked handler makes certain.
    void wake_locked(bool force = false) {
        if (stop_ || woken_ >= jobs_.size()) return; // after shutdown no thread may be added: nobody would join it
        if (!force && running_ + woken_ > 0) {
            if (!armed_) { armed_ = true; scout_cv_.notify_one(); }
            return;
        }
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

    // Sleeps until a job is waiting behind a busy thread, then checks every `help_after`: a job that has
    // waited that long gets a thread of its own.
    void scout() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            scout_cv_.wait(lk, [this] { return stop_ || armed_; });
            if (stop_) return;
            scout_cv_.wait_for(lk, help_after, [this] { return stop_; });
            if (stop_) return;
            if (jobs_.empty()) { armed_ = false; continue; }
            if (std::chrono::steady_clock::now() - jobs_.front().at >= help_after) {
                // One thread per 500 us of waiting, as before for the default wait: a pool that waits longer before it
                // grows (to keep CPU-bound work on few threads) adds that many threads per step, so a burst of blocked
                // handlers does not have to wait one step for every thread.
                const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(help_after / std::chrono::microseconds(500)));
                for (std::size_t i = 0; i < step; ++i) wake_locked(true);
            }
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
                auto job = std::move(jobs_.front().fn);
                jobs_.pop_front();
                ++running_;
                wake_locked(); // more queued work than threads on the way: the scout will see to it
                lk.unlock();
                try { job(); } catch (...) {} // a job must not kill its worker
                job = nullptr;                // drop captured state outside the lock
                lk.lock();
                --running_;
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
    struct queued {
        std::function<void()> fn;
        std::chrono::steady_clock::time_point at;
    };
    const std::chrono::microseconds help_after;
    std::deque<queued> jobs_;
    std::unordered_map<std::thread::id, std::thread> threads_;
    std::vector<std::thread::id> exited_;
    std::vector<waiter*> idle_; // sleeping threads, the most recent last
    std::size_t base_, max_;
    std::size_t woken_ = 0;     // woken or just added, not yet running
    std::size_t running_ = 0;   // threads inside a job
    bool armed_ = false;        // the scout is watching the queue
    std::condition_variable scout_cv_;
    std::thread scout_;
    bool stop_ = false;
};

} // namespace httpp::detail
