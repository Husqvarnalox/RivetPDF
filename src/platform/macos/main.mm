// SPDX-License-Identifier: MPL-2.0
#import "MacosAlertService.h"
#import "MacosClipboard.h"
#import "MacosExternalUrlOpener.h"
#import "MacosPrintService.h"
#import "MacosFileDialog.h"
#import "MacosImageDecoder.h"
#import "MacosImageEncoder.h"
#import "MacosLifecycle.h"
#import "MacosMainThreadDispatcher.h"
#import "RivetContentView.h"

#include "app/MenuModel.hpp"
#include "app/ShellController.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#import <AppKit/AppKit.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Note: ObjC classes must live at global scope (no C++ namespaces).

// Bridges menu actions to the shell. The menu bar is built generically from
// the portable model (app/MenuModel.hpp): each NSMenuItem's tag is the item's
// index into `commands`, and every item shares the single -command: action,
// which routes through ShellController::perform() and validates through
// canPerform()/isChecked() (NSMenu auto-enables items through validation).
@interface RivetAppBridge : NSObject {
@public
    rivet::app::ShellController* shell; // non-owning; main() owns the shell
    std::vector<rivet::app::AppCommand> commands; // flat menu index -> command
}
- (IBAction)command:(id)sender;
@end

@implementation RivetAppBridge
- (const rivet::app::AppCommand*)commandForItem:(NSMenuItem*)item {
    if (item == nil || item.tag < 0 || static_cast<std::size_t>(item.tag) >= commands.size()) return nullptr;
    return &commands[static_cast<std::size_t>(item.tag)];
}
- (IBAction)command:(id)sender {
    if (shell == nullptr) return;
    const rivet::app::AppCommand* command = [self commandForItem:sender];
    if (command != nullptr) shell->perform(*command);
}
// NSMenuItemValidation: gray out commands that cannot run; sync check marks.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (shell == nullptr) return NO;
    const rivet::app::AppCommand* command = [self commandForItem:item];
    if (command == nullptr) return YES;
    if (const auto checked = shell->isChecked(*command); checked.has_value()) {
        item.state = *checked ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return shell->canPerform(*command) ? YES : NO;
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
    // Opens files handed over by the system (non-owning shell access).
    std::function<void(const std::vector<std::filesystem::path>&)> openPaths;
}
@property(nonatomic, strong) NSWindow* window; // keeps the window alive
// NSWindow.delegate is weak: the app delegate owns the window delegate.
@property(nonatomic, strong) RivetWindowDelegate* windowDelegate;
@end

@implementation RivetAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application {
    return YES;
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    std::vector<std::filesystem::path> paths;
    for (NSURL* url in urls) {
        if (url.isFileURL && url.path != nil) paths.emplace_back(url.path.UTF8String);
    }
    if (openPaths && !paths.empty()) openPaths(paths);
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

        // Menu bar: the app menu (Quit, cmd+Q) is macOS-specific; every other
        // menu is rendered from the portable model.
        RivetAppBridge* bridge = [[RivetAppBridge alloc] init];

        NSMenu* menuBar = [[NSMenu alloc] init];

        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit Rivet" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];

        const std::vector<rivet::app::Menu> modelBar = rivet::app::buildMenuBar();
        NSInteger flatIndex = 0;
        for (const rivet::app::Menu& model : modelBar) {
            NSMenuItem* menuItem = [[NSMenuItem alloc] init];
            [menuBar addItem:menuItem];
            NSMenu* menu = [[NSMenu alloc] initWithTitle:[NSString stringWithUTF8String:model.title.c_str()]];
            for (const rivet::app::MenuItem& entry : model.items) {
                bridge->commands.push_back(entry.command);
                const NSInteger tag = flatIndex++;
                // macOS shows Quit in the application menu only.
                if (entry.role == rivet::app::MenuRole::Quit) continue;
                if (entry.separatorBefore) [menu addItem:[NSMenuItem separatorItem]];
                NSString* key = @"";
                NSEventModifierFlags mask = 0;
                if (entry.shortcut.has_value()) {
                    const rivet::app::Shortcut& sc = *entry.shortcut;
                    NSString* lower = [NSString stringWithUTF8String:sc.key.c_str()];
                    key = sc.shift ? lower.uppercaseString : lower;
                    if (sc.primary) mask |= NSEventModifierFlagCommand;
                    if (sc.shift) mask |= NSEventModifierFlagShift;
                    if (sc.alt) mask |= NSEventModifierFlagOption;
                }
                NSMenuItem* item = [menu addItemWithTitle:[NSString stringWithUTF8String:entry.title.c_str()]
                                                   action:@selector(command:)
                                            keyEquivalent:key];
                if (entry.shortcut.has_value()) item.keyEquivalentModifierMask = mask;
                item.tag = tag;
                [item setTarget:bridge];
            }
            [menuItem setSubmenu:menu];
        }

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
        const auto imageDecoder = std::make_unique<rivet::platform::MacosImageDecoder>();
        const auto imageEncoder = std::make_unique<rivet::platform::MacosImageEncoder>();
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
        services.imageDecoder = imageDecoder.get();
        services.imageEncoder = imageEncoder.get();
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
        const auto openPaths = [shellPtr = shell.get()](const std::vector<std::filesystem::path>& paths) {
            for (const auto& path : paths) shellPtr->openDocument(path);
        };
        appDelegate->openPaths = openPaths;
        [contentView setFileDropHandler:openPaths];
        appDelegate->teardown = [&shell, appDelegate, bridge, contentView, hooks = lifecycle.get()] {
            // Detach every borrower of the widget tree first, then destroy
            // the shell (workspace shutdown waits for background opens, the
            // sessions drain their worker streams, the scheduler joins).
            bridge->shell = nullptr;
            appDelegate->openPaths = nullptr;
            [contentView setFileDropHandler:nullptr];
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
        // Every non-option argument is opened (PDF or Markdown by extension).
        for (int i = 1; i < argc; ++i) {
            if (argv[i] != nullptr && argv[i][0] != '-') shell->openDocument(std::filesystem::path(argv[i]));
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
