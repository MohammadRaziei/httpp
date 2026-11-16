#include "utest/utest.h"
#include "httpp/file_lock.hpp"
#include "httpp/path.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using httpp::file_lock;
using httpp::path;
using std::chrono::milliseconds;
using clk = std::chrono::steady_clock;

namespace {

path fresh_dir(const char* name) {
    path d = path(std::string("test_tmp_file_lock_") + name);
    std::vector<path> v;
    d.iterdir(v);
    for (const path& e : v) e.unlink();
    d.mkdir();
    return d;
}

double seconds_since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

// Someone else's live lock: holds <target>.lock and rewrites `alive` until released.
struct fake_owner {
    path dir;
    std::atomic<bool> stop{false};
    std::thread beat;
    explicit fake_owner(const path& target) : dir(target.str() + ".lock") {
        dir.mkdir();
        beat = std::thread([this] {
            for (int n = 1; !stop; ++n) {
                (dir / "alive").write_text("999999 " + std::to_string(n) + "\n");
                std::this_thread::sleep_for(milliseconds(50));
            }
        });
    }
    void release() {
        stop = true;
        if (beat.joinable()) beat.join();
        (dir / "alive").unlink();
        dir.rmdir();
    }
    ~fake_owner() { release(); }
};

}  // namespace

UTEST(file_lock, holds_the_lock_directory_while_it_lives_and_removes_it_after) {
    const path d = fresh_dir("basic");
    const path target = d / "data.txt";
    {
        file_lock lock(target);
        ASSERT_TRUE(lock.owned());
        ASSERT_FALSE(lock.waited());
        ASSERT_TRUE(path(target.str() + ".lock").is_dir());
        ASSERT_TRUE((path(target.str() + ".lock") / "alive").exists());
        ASSERT_FALSE(target.exists());  // the target itself is never touched, it need not exist
    }
    ASSERT_FALSE(path(target.str() + ".lock").exists());
    { file_lock again(target); ASSERT_TRUE(again.owned()); }  // and it can be taken again
    d.rmdir();
}

UTEST(file_lock, a_second_taker_waits_until_the_first_lets_go) {
    const path d = fresh_dir("wait");
    const path target = d / "data.txt";
    auto first = std::make_unique<file_lock>(target);
    std::atomic<bool> got{false};
    bool waited = false;
    std::thread other([&] {
        file_lock second(target);
        waited = second.waited();
        got = second.owned();
    });
    std::this_thread::sleep_for(milliseconds(400));
    ASSERT_FALSE(got.load());  // still held by the first
    first.reset();
    other.join();
    ASSERT_TRUE(got.load());
    ASSERT_TRUE(waited);
    d.rmdir();
}

// The reason the lock exists: read-modify-write from several threads loses no update.
UTEST(file_lock, serializes_a_read_modify_write_across_threads) {
    const path d = fresh_dir("counter");
    const path target = d / "count.txt";
    target.write_text("0");
    const int threads = 4, rounds = 15;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = 0; i < rounds; ++i) {
                file_lock lock(target, milliseconds(3000));
                const int now = std::stoi(target.read_text());
                target.write_text_atomic(std::to_string(now + 1));
            }
        });
    for (auto& th : pool) th.join();
    ASSERT_EQ(threads * rounds, std::stoi(target.read_text()));
    target.unlink();
    d.rmdir();
}

UTEST(file_lock, waits_for_a_live_owner_and_takes_over_when_it_is_gone) {
    const path d = fresh_dir("live");
    const path target = d / "data.txt";
    fake_owner owner(target);
    std::thread releaser([&] {
        std::this_thread::sleep_for(milliseconds(700));
        owner.release();
    });
    const auto t = clk::now();
    file_lock lock(target, milliseconds(3000));  // stale_after far longer than the owner lives: not taken over early
    const double took = seconds_since(t);
    releaser.join();
    ASSERT_TRUE(lock.owned());
    ASSERT_TRUE(lock.waited());
    ASSERT_TRUE(took > 0.6);
    ASSERT_TRUE(took < 2.5);
}

UTEST(file_lock, takes_over_a_lock_whose_owner_died_after_stale_after) {
    const path d = fresh_dir("dead");
    const path target = d / "data.txt";
    path dead(target.str() + ".lock");
    dead.mkdir();
    (dead / "alive").write_text("999999 1\n");  // never changes again
    const auto t = clk::now();
    {
        file_lock lock(target, milliseconds(1000));
        const double took = seconds_since(t);
        ASSERT_TRUE(lock.owned());
        ASSERT_TRUE(lock.waited());
        ASSERT_TRUE(took >= 0.9);   // it waited the whole stale_after first
        ASSERT_TRUE(took < 4.0);
    }
    ASSERT_FALSE(dead.exists());
    d.rmdir();
}

UTEST(file_lock, goes_on_unlocked_when_the_lock_cannot_be_made) {
    const path target = path("test_tmp_file_lock_no_such_dir") / "x" / "data.txt";
    const auto t = clk::now();
    file_lock lock(target);
    ASSERT_FALSE(lock.owned());
    ASSERT_FALSE(lock.waited());
    ASSERT_TRUE(seconds_since(t) < 1.0);
}
