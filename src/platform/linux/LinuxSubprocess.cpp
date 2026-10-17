// SPDX-License-Identifier: MPL-2.0
#include "LinuxSubprocess.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rivet::platform::subprocess {

namespace {

constexpr std::size_t kMaxOutput = 1024 * 1024;

std::vector<char*> makeArgv(const std::filesystem::path& program, const std::vector<std::string>& args,
                            std::string& programStorage) {
    programStorage = program.string();
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(programStorage.data());
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    return argv;
}

int waitForExit(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

std::optional<std::filesystem::path> findExecutable(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) return std::nullopt;
    const char* path = std::getenv("PATH");
    const std::string_view entries = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= entries.size()) {
        std::size_t end = entries.find(':', start);
        if (end == std::string_view::npos) end = entries.size();
        const std::string_view dir = entries.substr(start, end - start);
        start = end + 1;
        if (dir.empty() || dir.front() != '/') continue; // never resolve relative to the cwd
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

Result run(const std::filesystem::path& program, const std::vector<std::string>& args,
           const std::function<void(int fd)>& pump) {
    Result result;
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return result;
    for (const int fd : fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

    std::string programStorage;
    std::vector<char*> argv = makeArgv(program, args, programStorage);
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, programStorage.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (spawned != 0) {
        ::close(fds[0]);
        return result;
    }
    result.started = true;

    const int flags = ::fcntl(fds[0], F_GETFL);
    if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    char buffer[4096];
    bool open = true;
    while (open) {
        for (;;) {
            const ssize_t got = ::read(fds[0], buffer, sizeof buffer);
            if (got > 0) {
                if (result.output.size() < kMaxOutput) result.output.append(buffer, static_cast<std::size_t>(got));
                continue;
            }
            if (got == 0) open = false; // EOF: the child closed stdout
            else if (errno == EINTR) continue;
            break; // EAGAIN: nothing more right now
        }
        if (open) pump(fds[0]);
    }
    ::close(fds[0]);
    result.exitCode = waitForExit(pid);
    return result;
}

bool spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    std::string programStorage;
    std::vector<char*> argv = makeArgv(program, args, programStorage);
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, programStorage.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return false;
    // Reaper: owns nothing but the pid, so it needs no lifetime fence.
    std::thread([pid] { waitForExit(pid); }).detach();
    return true;
}

} // namespace rivet::platform::subprocess
