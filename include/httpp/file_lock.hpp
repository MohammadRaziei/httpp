// httpp::file_lock: a lock on a file (or on anything named by a path) that holds between processes, not only threads.
//
//     {
//         httpp::file_lock lock(dest);              // waits until nobody else holds it
//         ... read, change and write dest (path::write_text_atomic) ...
//     }                                             // released here
//
// write_text_atomic() keeps a reader from seeing a half-written file; this keeps two writers from both reading the old
// state and each putting back their own change. download takes one around each file it fetches.
//
// How it works: whoever creates the directory "<target>.lock" owns the lock (creating a directory is atomic on every
// file system, path::mkdir), and the owner rewrites "<target>.lock/alive" a few times per `stale_after`. A waiter judges
// by its own clock whether that file *changes*, not by file times, so clock skew and coarse mtimes do not matter; when
// it has not changed for `stale_after` the owner is taken for dead and the lock is taken over. A lock that keeps being
// updated is waited for as long as it lives. If the lock directory cannot be created (a read-only directory) the
// constructor returns at once with owned() false: the caller goes ahead unlocked.
//
// The lock only saves work and serializes well-behaved users; two waiters that take over the same dead lock at the same
// instant can both run. Not copyable and not movable.
#pragma once

#include "httpp/export.hpp"
#include "httpp/path.hpp"

#include <chrono>
#include <memory>

namespace httpp {

class file_lock {
public:
    // Blocks until the lock is held (or cannot be made). `stale_after` is how long an unchanged heartbeat is waited for
    // before its owner is taken for dead; the heartbeat and the polling follow from it.
    HTTPP_API explicit file_lock(const path& target, std::chrono::milliseconds stale_after = std::chrono::seconds(15));
    HTTPP_API ~file_lock();
    file_lock(const file_lock&) = delete;
    file_lock& operator=(const file_lock&) = delete;

    // True when this object holds the lock (false: it could not be created, see above).
    HTTPP_API bool owned() const noexcept;
    // True when someone else held the lock at some point, i.e. what it protects may have changed meanwhile.
    HTTPP_API bool waited() const noexcept;

private:
    struct state;
    std::unique_ptr<state> st_;
};

}  // namespace httpp
