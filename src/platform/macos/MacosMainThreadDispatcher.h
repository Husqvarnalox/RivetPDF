#pragma once

#include "core/async/IMainThreadDispatcher.hpp"

namespace rivet::platform {

// Dispatches work to the macOS main queue. dispatch_async is thread-safe, so
// render worker threads can post completion work directly.
class MacosMainThreadDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override;
};

} // namespace rivet::platform
