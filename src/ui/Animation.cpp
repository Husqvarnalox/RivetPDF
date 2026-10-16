// SPDX-License-Identifier: MPL-2.0
#include "ui/Animation.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

std::function<double()>& testClock() {
    static std::function<double()> clock;
    return clock;
}

} // namespace

double animationNow() {
    if (testClock()) return testClock()();
    using Seconds = std::chrono::duration<double>;
    return std::chrono::duration_cast<Seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void setAnimationClockForTesting(std::function<double()> clock) { testClock() = std::move(clock); }

double fadeProgress(double start, double duration) {
    if (duration <= 0.0) return 1.0;
    const double t = std::clamp((animationNow() - start) / duration, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace rivet::ui
