#include "httpp/file_lock.hpp"

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h> // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace httpp {

namespace {

long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// The heartbeat file may not be there yet (the owner has just made the directory): empty then.
std::string read_head(const path& file, std::size_t limit) {
    try {
        return file.read_text(0, limit);
    } catch (const std::runtime_error&) {
        return std::string();
    }
}

std::chrono::milliseconds clamp(std::chrono::milliseconds v, std::chrono::milliseconds most) {
    return v < std::chrono::milliseconds(5) ? std::chrono::milliseconds(5) : (v > most ? most : v);
}

}  // namespace

struct file_lock::state {
    path dir;
    std::chrono::milliseconds heartbeat{1000};
    bool owned = false;
    bool waited = false;
    bool stop = false;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread heart;

    void beat(unsigned long long n) const {
        // plain write, not write_text_atomic: a waiter may have the file open (Windows would refuse the rename), and a
        // failed beat is harmless
        std::ofstream out((dir / "alive").native(), std::ios::binary | std::ios::trunc);
        out << current_pid() << ' ' << n << '\n';  // who owns the lock (for whoever looks at a stuck one), and a beat count
    }

    void run_heartbeat() {
        std::unique_lock<std::mutex> lk(mutex);
        unsigned long long n = 0;
        while (!stop) {
            beat(++n);
            wake.wait_for(lk, heartbeat, [this] { return stop; });
        }
    }
};

file_lock::file_lock(const path& target, std::chrono::milliseconds stale_after) : st_(new state) {
    using clock = std::chrono::steady_clock;
    st_->dir = path(target.str() + ".lock");
    // the owner proves it lives twice per wait window (at most every 200 ms); waiters look more often than that
    // at most 200 ms: a waiter's threshold is whole seconds (>= 1 s), so it always sees several beats
    st_->heartbeat = clamp(stale_after / 2, std::chrono::milliseconds(200));
    const auto poll = clamp(stale_after / 15, std::chrono::milliseconds(200));
    std::string last_seen;
    auto last_change = clock::now();
    bool first = true;
    int missing = 0;
    for (;;) {
        if (st_->dir.mkdir()) {
            st_->owned = true;
            break;
        }
        if (!st_->dir.is_dir()) {
            // mkdir failed and there is no directory: either it cannot be made (a read-only folder: it fails every time), or its owner
            // let go between the two calls. Take it again before going on without a lock: skipping it here would let two writers in.
            if (++missing > 3) break;
            continue;
        }
        missing = 0;
        st_->waited = true;
        const std::string seen = read_head(st_->dir / "alive", 64);
        const auto now = clock::now();
        if (first || seen != last_seen) {  // the owner is alive (or this is the first look)
            last_seen = seen;
            last_change = now;
            first = false;
        } else if (now - last_change > stale_after) {  // no sign of life for the whole timeout: its owner died
            (st_->dir / "alive").unlink();
            st_->dir.rmdir();
            first = true;
            continue;
        }
        // ponytail: a lock that keeps being updated is waited for without limit; upgrade path: a cap
        std::this_thread::sleep_for(poll);
    }
    if (st_->owned) {
        st_->beat(0);
        state* s = st_.get();
        s->heart = std::thread([s] { s->run_heartbeat(); });
    }
}

file_lock::~file_lock() {
    if (!st_ || !st_->owned) return;  // never touch a lock that belongs to someone else
    {
        std::lock_guard<std::mutex> guard(st_->mutex);
        st_->stop = true;
    }
    st_->wake.notify_all();
    if (st_->heart.joinable()) st_->heart.join();
    (st_->dir / "alive").unlink();
    st_->dir.rmdir();
}

bool file_lock::owned() const noexcept { return st_->owned; }
bool file_lock::waited() const noexcept { return st_->waited; }

}  // namespace httpp
