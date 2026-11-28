#pragma once

// Internal. Put one of these first in the body of a thread that httpp starts: when the body ends (also by an exception), the function
// that httpp::set_thread_exit_hook() installed runs on this very thread, after everything else the thread did. It is a local object of
// the thread function on purpose, not a thread_local: a thread_local destructor on Windows runs inside DllMain with the loader lock
// held, where a hook that waits for the Python interpreter could deadlock against a thread that imports a module.

#include "httpp/utils.hpp"

namespace httpp::detail {

class thread_exit_scope {
public:
    thread_exit_scope() = default;
    thread_exit_scope(const thread_exit_scope&) = delete;
    thread_exit_scope& operator=(const thread_exit_scope&) = delete;
    ~thread_exit_scope() { run_thread_exit_hook(); }
};

} // namespace httpp::detail
