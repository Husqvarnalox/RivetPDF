// SPDX-License-Identifier: MPL-2.0
#import "MacosAlertService.h"
#import "MacosClipboard.h"
#import "MacosExternalUrlOpener.h"
#import "MacosPrintService.h"
#import "MacosFileDialog.h"
#import "MacosLifecycle.h"
#import "MacosMainThreadDispatcher.h"
#import "RivetContentView.h"

#include "app/ShellController.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#import <AppKit/AppKit.h>

#include <functional>
#include <memory>
#include <utility>

// Note: ObjC classes must live at global scope (no C++ namespaces).

// Bridges menu actions to the shell. The Open… menu item goes through the
// same ShellController::handleOpenRequest() path as the toolbar button.
@interface RivetAppBridge : NSObject {
@public
    rivet::app::ShellController* shell; // non-owning; main() owns the shell
}
- (IBAction)openDocument:(id)sender;
@end

@implementation RivetAppBridge
- (IBAction)openDocument:(id)sender {
    if (shell != nullptr) shell->handleOpenRequest();
}
@end

@interface RivetAppDelegate : NSObject <NSApplicationDelegate> {
@public
    // Destroys the shell. -[NSApplication terminate:] never returns: after
    // applicationWillTerminate: it calls exit(), which runs static
    // destructors (including PDFium's library teardown) WITHOUT unwinding
    // main()'s stack. The shell - and with it every document session, the
    // worker pool and in-flight backend work - must therefore be torn down
    // here, deterministically, before exit() starts.
    std::function<void()> teardown;
    // Quit interception (non-owning; main() keeps the hooks alive).
    rivet::platform::LifecycleHooks* hooks;
}
@property(nonatomic, strong) NSWindow* window; // keeps the window alive
// NSWindow.delegate is weak: the app delegate owns the window delegate.
@property(nonatomic, strong) RivetWindowDelegate* windowDelegate;
@end

@implementation RivetAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    return rivet::platform::macosApplicationShouldTerminate(hooks);
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    if (teardown) {
        auto run = std::move(teardown);
        teardown = nullptr;
        run();
    }
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        RivetAppDelegate* appDelegate = [[RivetAppDelegate alloc] init];
        [NSApp setDelegate:appDelegate];

        // Menu bar: app menu (Quit, cmd+Q) and File > Open… (cmd+O).
        RivetAppBridge* bridge = [[RivetAppBridge alloc] init];

        NSMenu* menuBar = [[NSMenu alloc] init];

        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit Rivet" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];

        NSMenuItem* fileMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:fileMenuItem];
        NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        NSMenuItem* openItem =
            [fileMenu addItemWithTitle:@"Open…" action:@selector(openDocument:) keyEquivalent:@"o"];
        [openItem setTarget:bridge];
        [fileMenuItem setSubmenu:fileMenu];

        [NSApp setMainMenu:menuBar];

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(120.0, 120.0, 1280.0, 850.0)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [window setTitle:@"Rivet"];
        [window setContentMinSize:NSMakeSize(940.0, 600.0)];
        appDelegate.window = window;

        RivetContentView* contentView =
            [[RivetContentView alloc] initWithFrame:window.contentView.bounds];
        [window setContentView:contentView];

        // Services must outlive the app shell and the run loop.
        const auto dispatcher = std::make_unique<rivet::platform::MacosMainThreadDispatcher>();
        const auto fileDialog = std::make_unique<rivet::platform::MacosFileDialog>();
        const auto clipboard = std::make_unique<rivet::platform::MacosClipboard>();
        const auto urlOpener = std::make_unique<rivet::platform::MacosExternalUrlOpener>();
        const auto printService = std::make_unique<rivet::platform::MacosPrintService>();
        const auto alertService = std::make_unique<rivet::platform::MacosAlertService>();
        const auto lifecycle = std::make_unique<rivet::platform::LifecycleHooks>();

        RivetWindowDelegate* windowDelegate = [[RivetWindowDelegate alloc] init];
        windowDelegate->hooks = lifecycle.get();
        appDelegate.windowDelegate = windowDelegate;
        [window setDelegate:windowDelegate];
        appDelegate->hooks = lifecycle.get();

        rivet::platform::ShellServices services;
        services.redrawSink = [contentView redrawSink];
        services.mainDispatcher = dispatcher.get();
        services.fileDialog = fileDialog.get();
        services.clipboard = clipboard.get();
        services.urlOpener = urlOpener.get();
        services.printService = printService.get();
        services.saveDialog = fileDialog.get();
        services.alerts = alertService.get();
        services.lifecycle = lifecycle.get();
        NSWindow* __weak weakWindow = window;
        services.setWindowTitle = [weakWindow](const std::string& title) {
            // May be called from the main thread only (shell is main-thread
            // only); the weak window survives shell teardown ordering.
            NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{
                weakWindow.title = nsTitle;
            });
        };
        services.setDocumentEdited = [weakWindow](bool edited) {
            // Main thread only (shell contract); applied synchronously so the
            // close-button dot is correct before any close/quit prompt.
            [weakWindow setDocumentEdited:edited ? YES : NO];
        };

        // The shell owns the widget tree; the view only borrows the root.
        auto shell = rivet::app::createShell(services);
        [contentView setRootWidget:&shell->rootWidget()];
        [contentView setKeyHandler:[shellPtr = shell.get()](const rivet::ui::KeyEvent& event) {
            return shellPtr->handleKeyEvent(event);
        }];
        bridge->shell = shell.get();
        appDelegate->teardown = [&shell, bridge, contentView, hooks = lifecycle.get()] {
            // Detach every borrower of the widget tree first, then destroy
            // the shell (workspace shutdown waits for background opens, the
            // sessions drain their worker streams, the scheduler joins).
            bridge->shell = nullptr;
            // Handlers may capture shell state: drop them before the shell.
            hooks->clearHandlers();
            [contentView setKeyHandler:nullptr];
            [contentView setRootWidget:nullptr];
            shell.reset();
        };

        [window makeKeyAndOrderFront:nil];

        // Open-on-launch: `rivet /path/to/document.pdf`. The path must exist;
        // failures surface in the shell's status label exactly like a failed
        // dialog open.
        if (argc > 1 && argv[1] != nullptr) {
            shell->openDocument(std::filesystem::path(argv[1]));
        }
        if (@available(macOS 14.0, *)) {
            [NSApp activate];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        }
        [NSApp run];

        // Reached only if the run loop is stopped without terminate: (e.g.
        // -[NSApp stop:]). Same teardown as applicationWillTerminate:.
        if (appDelegate->teardown) {
            auto run = std::move(appDelegate->teardown);
            appDelegate->teardown = nullptr;
            run();
        }
    }
    return 0;
}
