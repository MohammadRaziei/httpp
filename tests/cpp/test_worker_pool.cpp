// The worker pool behind `def` handlers (src/core/internal/worker_pool.hpp). A light load must stay on
// a few hot threads (every extra thread is another thread fighting for the Python lock), a blocked
// thread must never leave queued work waiting, and growth must still happen when every thread is busy.
#include "utest/utest.h"
#include "internal/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using httpp::detail::worker_pool;
using namespace std::chrono_literals;

namespace {

// Opens a gate when the test function is left, also when an ASSERT returns early. Declare it AFTER the pool: the pool is then destroyed
// (joined) after the gate opened. Without it a timing assertion that fails on a slow runner leaves a thread blocked on the gate, the
// pool's destructor joins it forever, and CI hangs instead of reporting the failure (seen on macOS).
struct open_on_exit {
    std::promise<void>& gate;
    ~open_on_exit() {
        try { gate.set_value(); } catch (const std::future_error&) {}  // already opened by the test
    }
};

} // namespace

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
    open_on_exit opener{release};
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
    open_on_exit opener{release};
    pool.enqueue([&] { started.set_value(); gate.wait(); });
    started.get_future().wait();
    const auto t0 = std::chrono::steady_clock::now();
    pool.enqueue([&] { ++n; });
    while (n == 0 && std::chrono::steady_clock::now() - t0 < 2s) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(1, n.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 1s);  // help arrives; the exact delay is not the point
    release.set_value();
}

UTEST(worker_pool, a_burst_of_blocked_jobs_gets_its_threads_in_a_few_steps_even_with_a_long_wait) {
    // One starting thread, a 10 ms wait before growing (20 threads per step, see worker_pool::scout): 32 jobs that
    // all block must all be running after two steps (about 20 to 40 ms), not after 32 of them (320 ms). The bound
    // sits between the two with room for a slow CI runner (timer and thread creation on macOS).
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> running{0};
    worker_pool pool(1, 64, 10ms);
    open_on_exit opener{release};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 32; ++i) pool.enqueue([&] { ++running; gate.wait(); });
    while (running < 32 && std::chrono::steady_clock::now() - t0 < 2s) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(32, running.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 250ms);
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
    const auto t0 = std::chrono::steady_clock::now();
    while (done < 2000 && std::chrono::steady_clock::now() - t0 < 20s) std::this_thread::sleep_for(1ms);  // a bound: a lost job fails, not hangs
    ASSERT_EQ(2000, done.load());
    ASSERT_TRUE(seen.size() <= 2u);
}

// ---- the thread exit hook (ADR-0112): a worker runs it as its last act, never with the pool's lock held ----

namespace {

std::atomic<int> g_exit_calls{0};
worker_pool* g_hook_pool = nullptr;

void count_exit() noexcept { ++g_exit_calls; }

// Takes the pool's lock (enqueue locks it): a hook that ran inside the lock would deadlock here.
void enqueue_then_count_exit() noexcept {
    if (g_hook_pool) g_hook_pool->enqueue([] {});
    ++g_exit_calls;
}

template <class Pred>
bool wait_until(Pred done, std::chrono::milliseconds limit) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - t0 > limit) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

struct gate_state {
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> running{0};
};

// Makes `n` jobs run at the same time, so the pool grows to `n` threads, then lets them all go. False when they did not all start.
bool run_n_blocked_jobs(worker_pool& pool, int n) {
    auto state = std::make_shared<gate_state>(); // the jobs own it: they may outlive this function
    for (int i = 0; i < n; ++i) pool.enqueue([state] { ++state->running; state->gate.wait(); });
    const bool all_running = wait_until([&] { return state->running.load() == n; }, 5s);
    state->release.set_value(); // also when not all started: no thread stays blocked
    return all_running;
}

} // namespace

UTEST(worker_pool, every_worker_runs_the_exit_hook_once_when_the_pool_shuts_down) {
    g_exit_calls = 0;
    {
        worker_pool pool(4, 4, 500us, &count_exit);
    } // the destructor shuts down and joins; the scout thread runs no job and no hook
    ASSERT_EQ(4, g_exit_calls.load());
}

UTEST(worker_pool, a_thread_that_leaves_after_idling_runs_the_exit_hook_and_the_base_threads_stay) {
    g_exit_calls = 0;
    {
        worker_pool pool(1, 4, 500us, &count_exit, 30ms);
        ASSERT_TRUE(run_n_blocked_jobs(pool, 4));                              // 4 threads now
        ASSERT_TRUE(wait_until([] { return g_exit_calls.load() == 3; }, 5s));  // the 3 above base leave after 30 ms idle
        std::this_thread::sleep_for(150ms);
        ASSERT_EQ(3, g_exit_calls.load());                                      // the base thread stays
        std::promise<void> ran;
        ASSERT_TRUE(pool.enqueue([&] { ran.set_value(); }));
        ASSERT_TRUE(ran.get_future().wait_for(2s) == std::future_status::ready); // and the pool still works
    }
    ASSERT_EQ(4, g_exit_calls.load()); // the base thread runs the hook at shutdown
}

UTEST(worker_pool, the_exit_hook_runs_without_the_pool_lock) {
    g_exit_calls = 0;
    {
        worker_pool pool(1, 2, 500us, &enqueue_then_count_exit, 30ms);
        g_hook_pool = &pool;
        ASSERT_TRUE(run_n_blocked_jobs(pool, 2));
        ASSERT_TRUE(wait_until([] { return g_exit_calls.load() == 1; }, 5s)); // an idle exit: the hook enqueued from inside
    }
    g_hook_pool = nullptr;
    ASSERT_EQ(2, g_exit_calls.load()); // and the base thread at shutdown (enqueue then answers false, but needs the lock too)
}
