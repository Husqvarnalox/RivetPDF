// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/async/DelayedTaskQueue.hpp"

#include <chrono>
#include <functional>
#include <vector>

// The timer bookkeeping behind IMainThreadDispatcher::postDelayed on Linux
// and Windows (QA bug 15: caret blink and tooltip delays no longer sleep on
// a worker thread).

using rivet::core::DelayedTaskQueue;
using namespace std::chrono_literals;

RIVET_TEST(delayedTaskQueueRunsDueTasksInDeadlineOrder) {
    DelayedTaskQueue queue;
    const auto t0 = DelayedTaskQueue::Clock::time_point{} + 1h;
    std::vector<int> ran;
    CHECK(queue.push(t0 + 50ms, [&] { ran.push_back(50); }));
    CHECK(queue.push(t0 + 10ms, [&] { ran.push_back(10); }));  // new earliest
    CHECK(!queue.push(t0 + 30ms, [&] { ran.push_back(30); })); // not the earliest
    CHECK(!queue.push(t0 + 10ms, [&] { ran.push_back(11); })); // tie: push order
    CHECK(queue.size() == 4);
    CHECK(queue.nextDeadline() == t0 + 10ms);

    CHECK(queue.millisecondsUntilNext(t0, 1000) == 10);
    CHECK(queue.millisecondsUntilNext(t0, 5) == 5);
    CHECK(queue.millisecondsUntilNext(t0 + 20ms, 1000) == 0);
    CHECK(queue.millisecondsUntilNext(t0 + 9500us, 1000) == 1); // rounded up, never early

    CHECK(queue.takeDue(t0 + 5ms).empty());
    for (auto& task : queue.takeDue(t0 + 30ms)) task();
    CHECK((ran == std::vector<int>{10, 11, 30}));
    CHECK(queue.size() == 1);
    for (auto& task : queue.takeDue(t0 + 1s)) task();
    CHECK((ran == std::vector<int>{10, 11, 30, 50}));
    CHECK(!queue.nextDeadline().has_value());
    CHECK(!queue.millisecondsUntilNext(t0, 1000).has_value());
}
