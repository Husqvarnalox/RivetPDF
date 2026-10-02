// SPDX-License-Identifier: MPL-2.0
#import "MacosAlertService.h"
#import "MacosClipboard.h"
#import "MacosExternalUrlOpener.h"
#import "MacosPrintService.h"
#import "MacosFileDialog.h"
#import "MacosImageDecoder.h"
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
// same ShellController::handleOpenRequest() path as the toolbar button; page
// editing items carry their PageEditCommand as the item tag, route through
// performPageEdit() and validate against canPerformPageEdit() (NSMenu
// auto-enables items through item validation).
@interface RivetAppBridge : NSObject {
@public
    rivet::app::ShellController* shell; // non-owning; main() owns the shell
}
- (IBAction)openDocument:(id)sender;
- (IBAction)pageEdit:(id)sender;
- (IBAction)annotationCommand:(id)sender;
- (IBAction)contentCommand:(id)sender;
@end

@implementation RivetAppBridge
- (IBAction)openDocument:(id)sender {
    if (shell != nullptr) shell->handleOpenRequest();
}
- (IBAction)pageEdit:(id)sender {
    if (shell == nullptr) return;
    NSMenuItem* item = sender;
    if (item == nullptr) return;
    shell->performPageEdit(static_cast<rivet::app::PageEditCommand>(item.tag));
}
- (IBAction)annotationCommand:(id)sender {
    if (shell == nullptr) return;
    NSMenuItem* item = sender;
    if (item == nullptr) return;
    shell->performAnnotation(static_cast<rivet::app::AnnotationCommand>(item.tag));
}
- (IBAction)contentCommand:(id)sender {
    if (shell == nullptr) return;
    NSMenuItem* item = sender;
    if (item == nullptr) return;
    shell->performContent(static_cast<rivet::app::ContentCommand>(item.tag));
}
- (IBAction)fileCommand:(id)sender {
    if (shell == nullptr) return;
    NSMenuItem* item = sender;
    if (item == nullptr) return;
    shell->performFile(static_cast<rivet::app::FileCommand>(item.tag));
}
// NSMenuItemValidation: gray out commands that cannot run.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (shell == nullptr) return NO;
    if (item.action == @selector(pageEdit:)) {
        return shell->canPerformPageEdit(static_cast<rivet::app::PageEditCommand>(item.tag)) ? YES : NO;
    }
    if (item.action == @selector(annotationCommand:)) {
        return shell->canPerformAnnotation(static_cast<rivet::app::AnnotationCommand>(item.tag)) ? YES : NO;
    }
    if (item.action == @selector(contentCommand:)) {
        return shell->canPerformContent(static_cast<rivet::app::ContentCommand>(item.tag)) ? YES : NO;
    }
    if (item.action == @selector(fileCommand:)) {
        return shell->canPerformFile(static_cast<rivet::app::FileCommand>(item.tag)) ? YES : NO;
    }
    return YES;
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
        [fileMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* saveItem =
            [fileMenu addItemWithTitle:@"Save" action:@selector(fileCommand:) keyEquivalent:@"s"];
        saveItem.tag = static_cast<NSInteger>(rivet::app::FileCommand::Save);
        [saveItem setTarget:bridge];
        NSMenuItem* saveAsItem =
            [fileMenu addItemWithTitle:@"Save As…" action:@selector(fileCommand:) keyEquivalent:@"S"];
        saveAsItem.tag = static_cast<NSInteger>(rivet::app::FileCommand::SaveAs);
        [saveAsItem setTarget:bridge];
        saveAsItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
        [fileMenu addItem:[NSMenuItem separatorItem]];
        const std::pair<NSString*, rivet::app::FileCommand> fileItems[] = {
            {@"Insert Pages Before Current…", rivet::app::FileCommand::ImportBefore},
            {@"Insert Pages After Current…", rivet::app::FileCommand::ImportAfter},
            {@"Merge with PDF…", rivet::app::FileCommand::Merge},
            {@"Export Selected Pages…", rivet::app::FileCommand::Extract},
            {@"Split PDF by Ranges…", rivet::app::FileCommand::Split},
        };
        for (const auto& [title, command] : fileItems) {
            NSMenuItem* item =
                [fileMenu addItemWithTitle:title action:@selector(fileCommand:) keyEquivalent:@""];
            item.tag = static_cast<NSInteger>(command);
            [item setTarget:bridge];
        }
        [fileMenuItem setSubmenu:fileMenu];

        // Edit > Undo/Redo (the shell's page command history is the single
        // source of truth; Rivet has no other undo).
        NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:editMenuItem];
        NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Undo"
                            action:@selector(pageEdit:)
                     keyEquivalent:@"z"].tag = static_cast<NSInteger>(rivet::app::PageEditCommand::Undo);
        NSMenuItem* redoItem =
            [editMenu addItemWithTitle:@"Redo" action:@selector(pageEdit:) keyEquivalent:@"Z"];
        redoItem.tag = static_cast<NSInteger>(rivet::app::PageEditCommand::Redo);
        redoItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
        for (NSMenuItem* item in editMenu.itemArray) [item setTarget:bridge];
        [editMenuItem setSubmenu:editMenu];

        // Page > structure editing over the selection (or the current page).
        NSMenuItem* pageMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:pageMenuItem];
        NSMenu* pageMenu = [[NSMenu alloc] initWithTitle:@"Page"];
        const std::pair<NSString*, rivet::app::PageEditCommand> pageItems[] = {
            {@"Rotate Left", rivet::app::PageEditCommand::RotateLeft},
            {@"Rotate Right", rivet::app::PageEditCommand::RotateRight},
            {@"Duplicate", rivet::app::PageEditCommand::DuplicatePages},
            {@"Delete", rivet::app::PageEditCommand::DeletePages},
            {@"Crop…", rivet::app::PageEditCommand::Crop},
            {@"Reset Crop", rivet::app::PageEditCommand::ResetCrop},
        };
        for (const auto& [title, command] : pageItems) {
            NSMenuItem* item = [pageMenu addItemWithTitle:title action:@selector(pageEdit:) keyEquivalent:@""];
            item.tag = static_cast<NSInteger>(command);
            [item setTarget:bridge];
        }
        [pageMenuItem setSubmenu:pageMenu];

        // Content > the content-editing tools and selection commands (ADR-0014/
        // 0015); tags are ContentCommand values. Cmd+Shift+E / Cmd+Shift+T are
        // free (Cmd+E / Cmd+T are not used).
        NSMenuItem* contentMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:contentMenuItem];
        NSMenu* contentMenu = [[NSMenu alloc] initWithTitle:@"Content"];
        const struct {
            NSString* title;
            NSString* key;
            rivet::app::ContentCommand command;
            bool separatorBefore;
        } contentItems[] = {
            {@"Edit Objects", @"E", rivet::app::ContentCommand::ToolEdit, false},
            {@"Add Text", @"T", rivet::app::ContentCommand::ToolAddText, false},
            {@"Edit Text", @"", rivet::app::ContentCommand::EditText, true},
            {@"Replace Image…", @"", rivet::app::ContentCommand::ReplaceImage, false},
            {@"Bring to Front", @"", rivet::app::ContentCommand::BringToFront, false},
            {@"Delete Object", @"", rivet::app::ContentCommand::DeleteObject, false},
        };
        for (const auto& entry : contentItems) {
            if (entry.separatorBefore) [contentMenu addItem:[NSMenuItem separatorItem]];
            NSMenuItem* item = [contentMenu addItemWithTitle:entry.title
                                                      action:@selector(contentCommand:)
                                               keyEquivalent:entry.key];
            if (entry.key.length > 0) {
                item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
            }
            item.tag = static_cast<NSInteger>(entry.command);
            [item setTarget:bridge];
        }
        [contentMenuItem setSubmenu:contentMenu];

        // Annotate > tools (no single-letter shortcuts: they would clash with
        // typing) and annotation commands; tags are AnnotationCommand values.
        NSMenuItem* annotateMenuItem = [[NSMenuItem alloc] init];
        [menuBar addItem:annotateMenuItem];
        NSMenu* annotateMenu = [[NSMenu alloc] initWithTitle:@"Annotate"];
        const std::pair<NSString*, rivet::app::AnnotationCommand> toolItems[] = {
            {@"Select Tool", rivet::app::AnnotationCommand::ToolSelect},
            {@"Highlight", rivet::app::AnnotationCommand::ToolHighlight},
            {@"Underline", rivet::app::AnnotationCommand::ToolUnderline},
            {@"Strike Out", rivet::app::AnnotationCommand::ToolStrikeOut},
            {@"Note", rivet::app::AnnotationCommand::ToolNote},
            {@"Ink", rivet::app::AnnotationCommand::ToolInk},
            {@"Rectangle", rivet::app::AnnotationCommand::ToolRectangle},
            {@"Ellipse", rivet::app::AnnotationCommand::ToolEllipse},
            {@"Line", rivet::app::AnnotationCommand::ToolLine},
            {@"Arrow", rivet::app::AnnotationCommand::ToolArrow},
            {@"Stamp", rivet::app::AnnotationCommand::ToolStamp},
        };
        for (const auto& [title, command] : toolItems) {
            NSMenuItem* item = [annotateMenu addItemWithTitle:title
                                                       action:@selector(annotationCommand:)
                                                keyEquivalent:@""];
            item.tag = static_cast<NSInteger>(command);
            [item setTarget:bridge];
        }
        [annotateMenu addItem:[NSMenuItem separatorItem]];
        const std::pair<NSString*, rivet::app::AnnotationCommand> actionItems[] = {
            {@"Edit Note", rivet::app::AnnotationCommand::EditNote},
            {@"Delete Annotation", rivet::app::AnnotationCommand::DeleteAnnotation},
        };
        for (const auto& [title, command] : actionItems) {
            NSMenuItem* item = [annotateMenu addItemWithTitle:title
                                                       action:@selector(annotationCommand:)
                                                keyEquivalent:@""];
            item.tag = static_cast<NSInteger>(command);
            [item setTarget:bridge];
        }
        [annotateMenuItem setSubmenu:annotateMenu];

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
