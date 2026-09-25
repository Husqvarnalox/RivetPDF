// SPDX-License-Identifier: MPL-2.0
#import "MacosExternalUrlOpener.h"

#import <AppKit/AppKit.h>

namespace rivet::platform {

core::Status MacosExternalUrlOpener::openUrl(const std::string& url) {
    // Second line of defense behind the caller's scheme allow-list: only a
    // well-formed absolute URL is accepted (contains a scheme separator and
    // nothing before it, no whitespace/control characters).
    const auto schemeEnd = url.find("://");
    const bool hasMailto = url.rfind("mailto:", 0) == 0;
    if (!hasMailto && (schemeEnd == std::string::npos || schemeEnd == 0)) {
        return std::unexpected(
            rivet::core::Error{rivet::core::ErrorCode::InvalidArgument,
                               "URL is not an absolute http/https/mailto URL", "platform"});
    }
    for (const char c : url) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x21 || uc == 0x7F) {
            return std::unexpected(rivet::core::Error{
                rivet::core::ErrorCode::InvalidArgument, "URL contains control characters", "platform"});
        }
    }

    NSString* string = [NSString stringWithUTF8String:url.c_str()];
    if (string == nil) {
        return std::unexpected(rivet::core::Error{rivet::core::ErrorCode::InvalidArgument,
                                                  "URL is not valid UTF-8", "platform"});
    }
    NSURL* nsUrl = [NSURL URLWithString:string];
    if (nsUrl == nil || nsUrl.scheme.length == 0) {
        return std::unexpected(
            rivet::core::Error{rivet::core::ErrorCode::InvalidArgument, "malformed URL", "platform"});
    }
    if (![[NSWorkspace sharedWorkspace] openURL:nsUrl]) {
        return std::unexpected(rivet::core::Error{rivet::core::ErrorCode::Internal,
                                                  "the system refused to open the URL", "platform"});
    }
    return rivet::core::ok();
}

} // namespace rivet::platform
