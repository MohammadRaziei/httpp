#pragma once

// Internal. A small thread pool for the code that may block: server handlers and body
// providers. `base` threads stay; up to `max` threads run under load, and the extra ones leave
// again after a few idle seconds. Header-only so both the core and the tests can use it.

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
        for (std::size_t i = 0; i < base_; ++i) add_thread_locked();
    }
    ~worker_pool() { shutdown(); }
    worker_pool(const worker_pool&) = delete;
    worker_pool& operator=(const worker_pool&) = delete;

    // False once shut down.
    bool enqueue(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) return false;
            jobs_.push_back(std::move(job));
            reap_locked();
            if (idle_ < jobs_.size() && threads_.size() < max_) add_thread_locked(); // all busy: grow
        }
        cv_.notify_one();
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
        }
        cv_.notify_all();
        for (auto& [id, t] : all) if (t.joinable()) t.join();
    }

private:
    void add_thread_locked() {
        std::thread t([this] { run(); });
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

    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            ++idle_;
            cv_.wait_for(lk, std::chrono::seconds(5), [this] { return stop_ || !jobs_.empty(); });
            --idle_;
            if (!jobs_.empty()) {
                auto job = std::move(jobs_.front());
                jobs_.pop_front();
                lk.unlock();
                try { job(); } catch (...) {} // a job must not kill its worker
                job = nullptr;                // drop captured state outside the lock
                lk.lock();
                continue;
            }
            if (stop_) return;
            if (threads_.size() - exited_.size() > base_) { // idle for a while and above base: leave
                exited_.push_back(std::this_thread::get_id());
                return;
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::unordered_map<std::thread::id, std::thread> threads_;
    std::vector<std::thread::id> exited_;
    std::size_t base_, max_;
    std::size_t idle_ = 0;
    bool stop_ = false;
};

} // namespace httpp::detail
