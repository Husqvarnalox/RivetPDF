// SPDX-License-Identifier: MPL-2.0
#import "MacosFileDialog.h"

#import <AppKit/AppKit.h>

#include <cctype>
#include <string>
#include <vector>

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

// Restricts a panel to the given extensions (no dot). The first one is the
// default the save panel appends.
void restrictToExtensions(NSSavePanel* panel, const std::vector<std::string>& extensions) {
#if RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const std::string& extension : extensions) {
        UTType* type = [UTType typeWithFilenameExtension:toNSString(extension)];
        if (type != nil) [types addObject:type];
    }
    if (types.count == extensions.size()) {
        [panel setAllowedContentTypes:types];
        return;
    }
#endif
    NSMutableArray<NSString*>* names = [NSMutableArray array];
    for (const std::string& extension : extensions) [names addObject:toNSString(extension)];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [panel setAllowedFileTypes:names];
#pragma clang diagnostic pop
}

// Restricts a panel to PNG and JPEG images.
void restrictToImages(NSSavePanel* panel) {
#if RIVET_HAVE_UNIFORM_TYPE_IDENTIFIERS
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    UTType* png = [UTType typeWithIdentifier:@"public.png"];
    UTType* jpeg = [UTType typeWithIdentifier:@"public.jpeg"];
    if (png != nil) [types addObject:png];
    if (jpeg != nil) [types addObject:jpeg];
    if (types.count > 0) {
        [panel setAllowedContentTypes:types];
        return;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [panel setAllowedFileTypes:@[ @"png", @"jpg", @"jpeg" ]];
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

core::Result<std::filesystem::path> MacosFileDialog::openDocument() {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Open"];
    [panel setPrompt:@"Open"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    restrictToExtensions(panel, {"pdf", "md", "markdown", "mdown"});

    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
        return std::unexpected(cancelledError());
    }
    if (panel.URL.path == nil) {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "selected URL has no file path",
                                               "platform.macos"));
    }
    return std::filesystem::path(panel.URL.path.UTF8String);
}

core::Result<std::filesystem::path> MacosFileDialog::openImage() {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Replace Image"];
    [panel setPrompt:@"Choose"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    restrictToImages(panel);

    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
        return std::unexpected(cancelledError());
    }
    if (panel.URL.path == nil) {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "selected URL has no file path",
                                               "platform.macos"));
    }
    return std::filesystem::path(panel.URL.path.UTF8String);
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
    // Also appends the default extension when the user omits it.
    if (options.allowedExtensions.empty()) {
        restrictToPdf(panel);
    } else {
        restrictToExtensions(panel, options.allowedExtensions);
    }
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
    std::filesystem::path chosen(url.path.UTF8String);
    // The panel appends the extension on top of a typed one in some
    // configurations ("a.pdf" -> "a.pdf.pdf"); never keep the doubled
    // extension.
    std::vector<std::string> extensions = options.allowedExtensions;
    if (extensions.empty()) extensions.push_back("pdf");
    std::string name = chosen.filename().string();
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string& extension : extensions) {
        const std::string doubled = "." + extension + "." + extension;
        if (lower.size() > doubled.size() && lower.ends_with(doubled)) {
            name.resize(name.size() - extension.size() - 1);
            chosen.replace_filename(name);
            break;
        }
    }
    return chosen;
}

} // namespace rivet::platform
