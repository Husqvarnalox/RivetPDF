#import "MacosMainThreadDispatcher.h"

#import <dispatch/dispatch.h>

namespace rivet::platform {

void MacosMainThreadDispatcher::post(std::function<void()> task) {
    dispatch_async(dispatch_get_main_queue(), ^ {
        task();
    });
}

bool MacosMainThreadDispatcher::postDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
    if (!task) return true;
    const long long nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds > 0 ? nanoseconds : 0), dispatch_get_main_queue(), ^ {
        task();
    });
    return true;
}

} // namespace rivet::platform
