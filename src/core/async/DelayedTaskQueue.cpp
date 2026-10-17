// SPDX-License-Identifier: MPL-2.0
#include "core/async/DelayedTaskQueue.hpp"

#include <algorithm>
#include <utility>

namespace rivet::core {

bool DelayedTaskQueue::later(const Entry& a, const Entry& b) {
    if (a.deadline != b.deadline) return a.deadline > b.deadline;
    return a.sequence > b.sequence;
}

bool DelayedTaskQueue::push(Clock::time_point deadline, std::function<void()> task) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool earliest = heap_.empty() || deadline < heap_.front().deadline;
    heap_.push_back(Entry{deadline, nextSequence_++, std::move(task)});
    std::push_heap(heap_.begin(), heap_.end(), later);
    return earliest;
}

std::optional<DelayedTaskQueue::Clock::time_point> DelayedTaskQueue::nextDeadline() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (heap_.empty()) return std::nullopt;
    return heap_.front().deadline;
}

std::vector<std::function<void()>> DelayedTaskQueue::takeDue(Clock::time_point now) {
    std::vector<std::function<void()>> due;
    const std::lock_guard<std::mutex> lock(mutex_);
    while (!heap_.empty() && heap_.front().deadline <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), later);
        due.push_back(std::move(heap_.back().task));
        heap_.pop_back();
    }
    return due;
}

std::size_t DelayedTaskQueue::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

std::optional<int> DelayedTaskQueue::millisecondsUntilNext(Clock::time_point now, int maxMs) const {
    const std::optional<Clock::time_point> next = nextDeadline();
    if (!next) return std::nullopt;
    if (*next <= now) return 0;
    const long long wait = std::chrono::ceil<std::chrono::milliseconds>(*next - now).count();
    return static_cast<int>(std::min<long long>(wait, maxMs));
}

} // namespace rivet::core
