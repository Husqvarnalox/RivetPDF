#pragma once

#include <chrono>
#include <cstdint>

namespace rivet::core {

using SteadyClock = std::chrono::steady_clock;

// Milliseconds since the Unix epoch (wall clock). Used for log timestamps.
std::int64_t wallClockMilliseconds();

std::int64_t wallClockMilliseconds();

// Simple monotonic interval timer for measuring operations.
class Stopwatch {
public:
    Stopwatch() : start_(SteadyClock::now()) {}

    void restart() { start_ = SteadyClock::now(); }

    double elapsedSeconds() const {
        return std::chrono::duration<double>(SteadyClock::now() - start_).count();
    }

    std::int64_t elapsedMilliseconds() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - start_).count();
    }

    std::int64_t elapsedMicroseconds() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start_).count();
    }

private:
    SteadyClock::time_point start_;
};

} // namespace rivet::core
