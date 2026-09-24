#import "MacosFileDialog.h"
#import "MacosMainThreadDispatcher.h"
#import "RivetContentView.h"

#include "app/ShellController.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#import <AppKit/AppKit.h>

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

@interface RivetAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window; // keeps the window alive
@end

@implementation RivetAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application {
    return YES;
}
@end

int main() {
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

        rivet::platform::ShellServices services;
        services.redrawSink = [contentView redrawSink];
        services.mainDispatcher = dispatcher.get();
        services.fileDialog = fileDialog.get();

        // The shell owns the widget tree; the view only borrows the root.
        auto shell = rivet::app::createShell(services);
        [contentView setRootWidget:&shell->rootWidget()];
        [contentView setKeyHandler:[shellPtr = shell.get()](const rivet::ui::KeyEvent& event) {
            return shellPtr->handleKeyEvent(event);
        }];
        bridge->shell = shell.get();

        [window makeKeyAndOrderFront:nil];
        if (@available(macOS 14.0, *)) {
            [NSApp activate];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        }
        [NSApp run];

        // The shell must outlive the view/run loop; tear it down only after
        // -run returns (the window may already be gone, so nothing touches
        // the widget tree afterwards).
        shell.reset();
    }
    return 0;
}
