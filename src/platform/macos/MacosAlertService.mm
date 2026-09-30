// SPDX-License-Identifier: MPL-2.0
#import "MacosAlertService.h"

#import <AppKit/AppKit.h>

#include <string>

namespace rivet::platform {

namespace {

NSString* toNSString(std::string_view text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data()
                                                length:text.size()
                                              encoding:NSUTF8StringEncoding];
    return result != nil ? result : @"";
}

void markDestructive(NSButton* button) {
    if (@available(macOS 11.0, *)) {
        button.hasDestructiveAction = YES;
    }
}

} // namespace

SaveChangesChoice MacosAlertService::askSaveChanges(std::string_view documentTitle) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText =
        [NSString stringWithFormat:@"Do you want to save the changes made to the document “%@”?",
                                   toNSString(documentTitle)];
    alert.informativeText = @"Your changes will be lost if you don’t save them.";

    // First button is the default (Return) and rightmost; the standard
    // macOS order is Save | Cancel | Don't Save (leftmost).
    [alert addButtonWithTitle:@"Save"];
    NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033"; // Esc
    NSButton* dontSave = [alert addButtonWithTitle:@"Don’t Save"];
    dontSave.keyEquivalent = @"d";
    dontSave.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    markDestructive(dontSave);

    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) return SaveChangesChoice::Save;
    if (response == NSAlertThirdButtonReturn) return SaveChangesChoice::DontSave;
    return SaveChangesChoice::Cancel;
}

ReviewChangesChoice MacosAlertService::askReviewUnsavedChanges(std::size_t unsavedDocumentCount) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = [NSString
        stringWithFormat:@"You have %zu documents with unsaved changes. Do you want to review these "
                         @"changes before quitting?",
                         unsavedDocumentCount];
    alert.informativeText = @"If you don’t review your documents, all your changes will be lost.";

    [alert addButtonWithTitle:@"Review Changes…"];
    NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033";
    NSButton* discard = [alert addButtonWithTitle:@"Discard Changes"];
    markDestructive(discard);

    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) return ReviewChangesChoice::Review;
    if (response == NSAlertThirdButtonReturn) return ReviewChangesChoice::DiscardAll;
    return ReviewChangesChoice::Cancel;
}

void MacosAlertService::showError(std::string_view title, std::string_view message) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = toNSString(title);
    alert.informativeText = toNSString(message);
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

std::optional<std::string> MacosAlertService::promptForText(std::string_view title,
                                                            std::string_view message,
                                                            std::string_view defaultText) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = toNSString(title);
    alert.informativeText = toNSString(message);
    NSButton* ok = [alert addButtonWithTitle:@"OK"];
    (void)ok;
    NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033";

    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    field.stringValue = toNSString(defaultText);
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;

    if ([alert runModal] != NSAlertFirstButtonReturn) return std::nullopt;
    const char* utf8 = field.stringValue.UTF8String;
    return std::string(utf8 != nullptr ? utf8 : "");
}

} // namespace rivet::platform
