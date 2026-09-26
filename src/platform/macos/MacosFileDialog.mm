// SPDX-License-Identifier: MPL-2.0
#import "MacosFileDialog.h"

#import <AppKit/AppKit.h>

#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#define RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS 1
#else
#define RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS 0
#endif

namespace rivet::platform {

namespace {

NSString* toNSString(const std::string& text) {
    NSString* result = [NSString stringWithUTF8String:text.c_str()];
    return result != nil ? result : @"";
}

// Restricts a panel to PDF documents.
void restrictToPdf(NSSavePanel* panel) {
#if RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS
    UTType* pdfType = [UTType typeWithIdentifier:@"com.adobe.pdf"];
    if (pdfType == nil) pdfType = [UTType typeWithFilenameExtension:@"pdf"];
    if (pdfType != nil) {
        [panel setAllowedContentTypes:@[ pdfType ]];
        return;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [panel setAllowedFileTypes:@[ @"pdf" ]];
#pragma clang diagnostic pop
}

core::Error cancelledError() {
    return core::makeError(core::ErrorCode::Cancelled, "user dismissed the open dialog",
                           "platform.macos");
}

} // namespace

core::Result<std::filesystem::path> MacosFileDialog::openPdf() {
    auto chosen = openPdfs(OpenOptions{});
    if (!chosen) return std::unexpected(chosen.error());
    return std::move(chosen->front());
}

core::Result<std::vector<std::filesystem::path>> MacosFileDialog::openPdfs(
    const OpenOptions& options) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:toNSString(options.title)];
    [panel setPrompt:toNSString(options.prompt)];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:options.allowMultiple ? YES : NO];
    restrictToPdf(panel);

    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) {
        return std::unexpected(cancelledError());
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(panel.URLs.count);
    for (NSURL* url in panel.URLs) {
        if (url.path == nil) {
            return std::unexpected(core::makeError(
                core::ErrorCode::Io, "selected URL has no file path", "platform.macos"));
        }
        paths.emplace_back(url.path.UTF8String);
    }
    return paths;
}

std::optional<std::filesystem::path> MacosFileDialog::runSavePanel(
    const ISaveDialog::Options& options) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setTitle:toNSString(options.title)];
    [panel setPrompt:toNSString(options.prompt)];
    [panel setCanCreateDirectories:YES];
    [panel setExtensionHidden:NO];
    restrictToPdf(panel); // also appends ".pdf" when the user omits it
    if (!options.suggestedName.empty()) {
        [panel setNameFieldStringValue:toNSString(options.suggestedName)];
    }
    if (!options.initialDirectory.empty()) {
        NSString* dir = toNSString(options.initialDirectory.string());
        [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
    }

    // NSSavePanel itself confirms replacing an existing file.
    if ([panel runModal] != NSModalResponseOK) return std::nullopt;
    NSURL* url = panel.URL;
    if (url == nil || url.path == nil) return std::nullopt;
    return std::filesystem::path(url.path.UTF8String);
}

} // namespace rivet::platform
