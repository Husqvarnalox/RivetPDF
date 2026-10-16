// SPDX-License-Identifier: MPL-2.0
#pragma once

// Portable process helpers for tests (MSVC has no <unistd.h>).

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace rivet::test {

// Current process id, for unique scratch-directory names.
inline long processId() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// True when running as the superuser (permission bits are then bypassed).
// Always false on Windows, whose tests skip POSIX permission checks.
inline bool runningAsRoot() {
#if defined(_WIN32)
    return false;
#else
    return ::geteuid() == 0;
#endif
}

} // namespace rivet::test
