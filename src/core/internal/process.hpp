// Internal. The id of this process, as a number to put in a file name or a lock's heartbeat.
#pragma once

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>   // getpid
#endif

namespace httpp::detail {

inline long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

}  // namespace httpp::detail
