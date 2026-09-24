#pragma once

#include <functional>

namespace rivet::core {

// Abstraction for marshaling work to the platform main/UI thread.
// Implemented by the platform layer (e.g. dispatch to the macOS main queue).
//
// Implementations must be safe to call from any thread and must keep the
// process alive until posted tasks have run (or drop them only at shutdown).
struct IMainThreadDispatcher {
    virtual ~IMainThreadDispatcher() = default;

    virtual void post(std::function<void()> task) = 0;
};

} // namespace rivet::core
