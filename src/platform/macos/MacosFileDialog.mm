#import "MacosFileDialog.h"

#import <AppKit/AppKit.h>

#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#define RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS 1
#else
#define RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS 0
#endif

namespace rivet::platform {

core::Result<std::filesystem::path> MacosFileDialog::openPdf() {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Open PDF"];
    [panel setPrompt:@"Open"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];

#if RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS
    UTType* pdfType = [UTType typeWithIdentifier:@"com.adobe.pdf"];
    if (pdfType == nil) pdfType = [UTType typeWithFilenameExtension:@"pdf"];
    if (pdfType != nil) {
        [panel setAllowedContentTypes:@[ pdfType ]];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [panel setAllowedFileTypes:@[ @"pdf" ]];
#pragma clang diagnostic pop
    }
#else
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [panel setAllowedFileTypes:@[ @"pdf" ]];
#pragma clang diagnostic pop
#endif

    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) {
        return std::unexpected(
            core::makeError(core::ErrorCode::Cancelled, "user dismissed the open dialog",
                            "platform.macos"));
    }
    NSURL* url = panel.URLs.firstObject;
    if (url.path == nil) {
        return std::unexpected(
            core::makeError(core::ErrorCode::Io, "selected URL has no file path", "platform.macos"));
    }
    return std::filesystem::path(url.path.UTF8String);
}

} // namespace rivet::platform
