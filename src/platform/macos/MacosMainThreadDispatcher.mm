#import "MacosMainThreadDispatcher.h"

#import <dispatch/dispatch.h>

namespace rivet::platform {

void MacosMainThreadDispatcher::post(std::function<void()> task) {
    dispatch_async(dispatch_get_main_queue(), ^ {
        task();
    });
}

} // namespace rivet::platform
