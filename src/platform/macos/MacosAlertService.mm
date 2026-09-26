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

} // namespace rivet::platform
