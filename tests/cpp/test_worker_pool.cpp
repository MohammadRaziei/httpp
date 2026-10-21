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
