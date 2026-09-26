// SPDX-License-Identifier: MPL-2.0
#import "MacosLifecycle.h"

@implementation RivetWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    return hooks == nullptr || hooks->dispatchCloseRequest() ? YES : NO;
}
@end

namespace rivet::platform {

NSApplicationTerminateReply macosApplicationShouldTerminate(LifecycleHooks* hooks) {
    if (hooks == nullptr) return NSTerminateNow;
    const LifecycleHooks::QuitDecision decision = hooks->dispatchQuitRequest([](bool proceed) {
        // Main thread (QuitReply contract); AppKit is waiting in a modal
        // run loop mode for this answer.
        [NSApp replyToApplicationShouldTerminate:proceed ? YES : NO];
    });
    switch (decision) {
        case LifecycleHooks::QuitDecision::Proceed: return NSTerminateNow;
        case LifecycleHooks::QuitDecision::Cancel: return NSTerminateCancel;
        case LifecycleHooks::QuitDecision::Pending: return NSTerminateLater;
    }
    return NSTerminateCancel;
}

} // namespace rivet::platform
