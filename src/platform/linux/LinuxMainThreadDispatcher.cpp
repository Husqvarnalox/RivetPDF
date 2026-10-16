// SPDX-License-Identifier: MPL-2.0
#include "LinuxMainThreadDispatcher.hpp"

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace rivet::platform {

namespace {

void makeNonBlockingCloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int fdFlags = ::fcntl(fd, F_GETFD);
    if (fdFlags >= 0) ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
}

} // namespace

LinuxMainThreadDispatcher::LinuxMainThreadDispatcher() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
        readFd_ = fds[0];
        writeFd_ = fds[1];
        makeNonBlockingCloexec(readFd_);
        makeNonBlockingCloexec(writeFd_);
    }
}

LinuxMainThreadDispatcher::~LinuxMainThreadDispatcher() {
    if (readFd_ >= 0) ::close(readFd_);
    if (writeFd_ >= 0) ::close(writeFd_);
}

void LinuxMainThreadDispatcher::post(std::function<void()> task) {
    if (!task) return;
    bool wasEmpty = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wasEmpty = tasks_.empty();
        tasks_.push_back(std::move(task));
    }
    // One wakeup per empty -> non-empty transition is enough: drain() takes
    // the whole queue.
    if (wasEmpty) wake();
}

void LinuxMainThreadDispatcher::wake() const {
    if (writeFd_ < 0) return;
    const char byte = 1;
    // A full pipe already guarantees a pending wakeup, so EAGAIN is fine.
    while (::write(writeFd_, &byte, 1) < 0 && errno == EINTR) {
    }
}

void LinuxMainThreadDispatcher::consumeWakeups() const {
    if (readFd_ < 0) return;
    char buffer[256];
    for (;;) {
        const ssize_t got = ::read(readFd_, buffer, sizeof buffer);
        if (got > 0) continue;
        if (got < 0 && errno == EINTR) continue;
        break; // EAGAIN (empty) or EOF
    }
}

std::size_t LinuxMainThreadDispatcher::drain() {
    std::deque<std::function<void()>> batch;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(tasks_);
    }
    // Run outside the lock: tasks may post (re-entrancy) or block.
    for (auto& task : batch) task();
    return batch.size();
}

bool LinuxMainThreadDispatcher::hasPending() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return !tasks_.empty();
}

} // namespace rivet::platform
