// SPDX-License-Identifier: MPL-2.0
#import "MacosClipboard.h"

#import <AppKit/AppKit.h>

namespace rivet::platform {

core::Status MacosClipboard::setText(const std::string& text) {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    // Declare the types before setting data: plain UTF-8 text only.
    [pasteboard declareTypes:@[ NSPasteboardTypeString ] owner:nil];
    NSString* string = [NSString stringWithUTF8String:text.c_str()];
    if (string == nil) {
        // Unrepresentable content (invalid UTF-8 should not reach this layer;
        // treat it as an argument error rather than storing garbage).
        return std::unexpected(
            rivet::core::Error{rivet::core::ErrorCode::InvalidArgument,
                               "clipboard text is not valid UTF-8", "platform"});
    }
    if (![pasteboard setString:string forType:NSPasteboardTypeString]) {
        return std::unexpected(rivet::core::Error{rivet::core::ErrorCode::Internal,
                                                  "pasteboard rejected the text", "platform"});
    }
    return rivet::core::ok();
}

std::string MacosClipboard::text() const {
    NSString* string = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    return string != nil ? std::string(string.UTF8String) : std::string();
}

} // namespace rivet::platform
