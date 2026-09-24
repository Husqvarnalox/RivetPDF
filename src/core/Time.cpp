#include "core/Time.hpp"

namespace rivet::core {

std::int64_t wallClockMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace rivet::core
