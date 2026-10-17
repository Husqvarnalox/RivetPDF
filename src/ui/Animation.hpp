// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <functional>

namespace rivet::ui {

// Minimal motion support (ADR-0033 §7: 120 ms fades for hover and tooltips,
// no animated layout). A fading widget records when its target state
// changed, paints the interpolated value and invalidates itself while the
// fade runs, so the host paints the next frame. Main thread only.

// Seconds on a monotonic clock (steady_clock unless overridden).
double animationNow();
// Test hook: a fake clock (empty restores the steady clock).
void setAnimationClockForTesting(std::function<double()> clock);

// 0..1 progress of a fade that started at `start` and lasts `duration`
// seconds, eased (smoothstep). 1 once finished or when duration <= 0.
double fadeProgress(double start, double duration);

// One animated scalar: moves toward a target over a fixed duration.
class Fade {
public:
    explicit Fade(double duration) : duration_(duration) {}

    // Sets the target; the value then moves from where it is now.
    void setTarget(double goal) {
        if (goal == target_) return;
        from_ = value();
        target_ = goal;
        start_ = animationNow();
    }
    // Jumps to `value` without animating.
    void reset(double to) {
        from_ = to;
        target_ = to;
        start_ = -1.0;
    }
    double target() const { return target_; }
    double value() const {
        if (start_ < 0.0) return target_;
        const double t = fadeProgress(start_, duration_);
        return from_ + (target_ - from_) * t;
    }
    bool running() const { return start_ >= 0.0 && fadeProgress(start_, duration_) < 1.0; }

private:
    double duration_;
    double from_ = 0.0;
    double target_ = 0.0;
    double start_ = -1.0;
};

// Runs a task on the main thread after a delay (tooltips). Implemented by
// the shell (worker sleep + main-thread dispatcher, fenced by its lifetime).
class IDelayedRunner {
public:
    virtual void runAfter(std::chrono::milliseconds delay, std::function<void()> task) = 0;

protected:
    ~IDelayedRunner() = default;
};

} // namespace rivet::ui
