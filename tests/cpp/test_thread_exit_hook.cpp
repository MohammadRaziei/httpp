// The thread exit hook (ADR-0112, include/httpp/utils.hpp): a function that httpp runs on each thread it started, as the last thing that
// thread does. The Python binding frees the thread state it kept for the thread there.
#include "utest/utest.h"
#include "httpp/server.hpp"
#include "httpp/utils.hpp"
#include "local_server.hpp"

#include <atomic>

namespace {

std::atomic<int> g_calls{0};

void count_call() noexcept { ++g_calls; }

// Installs the counting hook for one test and takes it away again, also when an ASSERT returns early.
struct counting_hook {
    counting_hook() {
        g_calls = 0;
        httpp::set_thread_exit_hook(&count_call);
    }
    ~counting_hook() { httpp::set_thread_exit_hook(nullptr); }
};

}  // namespace

UTEST(thread_exit_hook, run_calls_the_installed_function_and_does_nothing_without_one) {
    httpp::run_thread_exit_hook();  // none installed
    {
        counting_hook hook;
        httpp::run_thread_exit_hook();
        httpp::run_thread_exit_hook();
        ASSERT_EQ(2, g_calls.load());
    }
    httpp::run_thread_exit_hook();  // removed again
    ASSERT_EQ(2, g_calls.load());
}

UTEST(thread_exit_hook, a_stopped_server_has_run_it_on_each_of_its_worker_threads) {
    counting_hook hook;
    {
        testutil::local_server s([](httpp::server& srv) { srv.thread_pool(3, 3); });
        ASSERT_EQ(0, g_calls.load());  // nothing ends while the server runs
    }                                  // stop() and join: the pool shut down inside listen_after_bind
    ASSERT_TRUE(g_calls.load() >= 3);
}
