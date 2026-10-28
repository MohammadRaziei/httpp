// The worker pool behind `def` handlers (src/core/internal/worker_pool.hpp). A light load must stay on
// a few hot threads (every extra thread is another thread fighting for the Python lock), a blocked
// thread must never leave queued work waiting, and growth must still happen when every thread is busy.
#include "utest/utest.h"
#include "internal/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <thread>

using httpp::detail::worker_pool;
using namespace std::chrono_literals;

UTEST(worker_pool, runs_every_job) {
    std::atomic<int> n{0};
    {
        worker_pool pool(4, 8);
        for (int i = 0; i < 2000; ++i) ASSERT_TRUE(pool.enqueue([&] { ++n; }));
    } // the destructor finishes the queue
    ASSERT_EQ(2000, n.load());
}

UTEST(worker_pool, refuses_jobs_after_shutdown) {
    worker_pool pool(2, 2);
    pool.shutdown();
    ASSERT_FALSE(pool.enqueue([] {}));
}

UTEST(worker_pool, a_light_sequential_load_stays_on_few_threads) {
    worker_pool pool(8, 8);
    std::mutex mu;
    std::set<std::thread::id> seen;
    for (int i = 0; i < 300; ++i) {
        std::promise<void> done;
        auto f = done.get_future();
        pool.enqueue([&] {
            { std::lock_guard<std::mutex> lk(mu); seen.insert(std::this_thread::get_id()); }
            done.set_value();
        });
        f.wait();
        std::this_thread::sleep_for(200us); // the worker goes back to sleep before the next job
    }
    ASSERT_TRUE(seen.size() <= 3u); // the old first-in first-out wake-up rotated through all 8
}

UTEST(worker_pool, queued_jobs_do_not_wait_behind_a_blocked_thread) {
    std::promise<void> release, first_started, three; // declared before the pool: it is destroyed (joined) first
    std::atomic<int> n{0};
    auto gate = release.get_future().share();
    worker_pool pool(2, 2);
    pool.enqueue([&] { first_started.set_value(); gate.wait(); });
    first_started.get_future().wait();
    for (int i = 0; i < 3; ++i) pool.enqueue([&] { if (++n == 3) three.set_value(); });
    ASSERT_TRUE(three.get_future().wait_for(2s) == std::future_status::ready); // run by the other thread
    release.set_value();
}

UTEST(worker_pool, grows_when_every_thread_is_busy) {
    worker_pool pool(1, 6);
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<int> done{0};
    for (int i = 0; i < 4; ++i) pool.enqueue([&] { std::this_thread::sleep_for(300ms); ++done; });
    while (done < 4 && std::chrono::steady_clock::now() - t0 < 5s) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(4, done.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 900ms); // in parallel, not 4 x 300 ms
}

UTEST(worker_pool, a_backlog_of_short_jobs_is_drained_by_the_running_thread) {
    // While one thread is busy with short jobs, queued jobs wait for it a moment instead of waking
    // another thread for each of them (two threads handing the Python lock back and forth is slower than one).
    worker_pool pool(8, 8);
    std::this_thread::sleep_for(50ms); // the threads have gone to sleep (fresh threads would all grab jobs)
    std::mutex mu;
    std::set<std::thread::id> seen;
    std::atomic<int> n{0};
    std::promise<void> all;
    for (int i = 0; i < 40; ++i)
        pool.enqueue([&] {
            { std::lock_guard<std::mutex> lk(mu); seen.insert(std::this_thread::get_id()); }
            for (auto end = std::chrono::steady_clock::now() + 5us; std::chrono::steady_clock::now() < end;) {} // far shorter than the extra-thread delay
            if (++n == 40) all.set_value();
        });
    ASSERT_TRUE(all.get_future().wait_for(10s) == std::future_status::ready);
    ASSERT_TRUE(seen.size() <= 3u);
}

UTEST(worker_pool, a_long_job_gets_help_after_a_short_delay) {
    std::promise<void> release, started;
    std::atomic<int> n{0};
    auto gate = release.get_future().share();
    worker_pool pool(4, 4);
    pool.enqueue([&] { started.set_value(); gate.wait(); });
    started.get_future().wait();
    const auto t0 = std::chrono::steady_clock::now();
    pool.enqueue([&] { ++n; });
    while (n == 0 && std::chrono::steady_clock::now() - t0 < 2s) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(1, n.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 100ms);
    release.set_value();
}

UTEST(worker_pool, a_burst_of_blocked_jobs_gets_its_threads_in_a_few_doubling_steps) {
    // One starting thread, a 5 ms wait before growing: 32 jobs that all block must still all be running
    // after about 6 steps (1, 2, 4, 8, 16, 32), not after 32 steps.
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> running{0};
    worker_pool pool(1, 64, 5ms);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 32; ++i) pool.enqueue([&] { ++running; gate.wait(); });
    while (running < 32 && std::chrono::steady_clock::now() - t0 < 2s) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(32, running.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 120ms);
    release.set_value();
}

UTEST(worker_pool, short_jobs_do_not_make_a_pool_with_a_long_wait_grow) {
    worker_pool pool(1, 64, 50ms);
    std::mutex mu;
    std::set<std::thread::id> seen;
    std::atomic<int> done{0};
    for (int i = 0; i < 2000; ++i) {
        pool.enqueue([&] {
            { std::lock_guard<std::mutex> lk(mu); seen.insert(std::this_thread::get_id()); }
            ++done;
        });
    }
    while (done < 2000) std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(seen.size() <= 2u);
}
