// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/AppLifecycle.hpp"

#import <AppKit/AppKit.h>

// Window delegate forwarding windowShouldClose: to the lifecycle hooks.
// The hooks pointer is non-owning; the composition root keeps the hooks
// alive for the lifetime of the window (and nils it before destroying them).
@interface RivetWindowDelegate : NSObject <NSWindowDelegate> {
@public
    rivet::platform::LifecycleHooks* hooks;
}
@end

namespace rivet::platform {

// Implements -[NSApplicationDelegate applicationShouldTerminate:] on top of
// the hooks: NSTerminateNow / NSTerminateCancel for synchronous answers,
// NSTerminateLater (followed by -[NSApp replyToApplicationShouldTerminate:])
// when the app layer decides asynchronously. Null hooks = terminate now.
NSApplicationTerminateReply macosApplicationShouldTerminate(LifecycleHooks* hooks);

} // namespace rivet::platform
