// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::platform::subprocess {

// Absolute path of `name` found in an executable $PATH entry, or nullopt.
std::optional<std::filesystem::path> findExecutable(const std::string& name);

struct Result {
    bool started = false; // posix_spawn succeeded
    int exitCode = -1;    // -1 when killed by a signal
    std::string output;   // stdout (capped at 1 MiB)
};

// Runs `program` with `args` (argv[1..]) via posix_spawn: no shell, no
// string interpolation, stdin and stderr on /dev/null, the parent's
// environment. While the child runs, `pump(fd)` is called repeatedly with
// the child's stdout pipe; it must block until that fd (or anything else the
// caller cares about) is readable — the X11 shell passes a modal event-loop
// iteration so windows keep repainting. Waits for the child to exit.
Result run(const std::filesystem::path& program, const std::vector<std::string>& args,
           const std::function<void(int fd)>& pump);

// Starts `program` detached (no output capture) and reaps it on a
// background thread that captures nothing but the pid. Returns false when
// the spawn failed.
bool spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args);

} // namespace rivet::platform::subprocess
