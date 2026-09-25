// SPDX-License-Identifier: MPL-2.0
#import "MacosPrintService.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <string>
#include <system_error>

namespace {

rivet::core::Error printError(rivet::core::ErrorCode code, std::string message) {
    return rivet::core::makeError(code, std::move(message), "platform");
}

// Owns an NSPrintInfo inside the portable PrintSettings handle.
std::shared_ptr<void> wrapPrintInfo(NSPrintInfo* info) {
    return std::shared_ptr<void>(const_cast<void*>(CFBridgingRetain(info)),
                                 [](void* retained) { CFBridgingRelease(retained); });
}

NSPrintInfo* unwrapPrintInfo(const std::shared_ptr<void>& handle) {
    if (handle == nullptr) return nil;
    id object = (__bridge id)handle.get();
    return [object isKindOfClass:[NSPrintInfo class]] ? (NSPrintInfo*)object : nil;
}

// Straight-alpha BGRA (core::PixelFormat::BGRA8888Straight) as CoreGraphics
// describes it: 32-bit little-endian ARGB words, non-premultiplied alpha.
constexpr CGBitmapInfo kBandBitmapInfo = static_cast<CGBitmapInfo>(
    static_cast<unsigned>(kCGImageAlphaFirst) | static_cast<unsigned>(kCGBitmapByteOrder32Little));

// A file-backed CGImage over one band. Validates the file size first:
// CGDataProviderCreateWithURL reads lazily and would not notice a short or
// missing file until drawing.
rivet::core::Result<CGImageRef> createBandImage(const rivet::platform::PrintSpoolBand& band) {
    const std::size_t minStride = std::size_t{band.pixelWidth} * 4;
    if (band.pixelWidth == 0 || band.pixelHeight == 0 || band.stride < minStride ||
        band.stride > SIZE_MAX / band.pixelHeight) {
        return std::unexpected(printError(rivet::core::ErrorCode::InvalidArgument,
                                          "invalid print band geometry"));
    }
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(band.file, error);
    if (error || size != band.stride * band.pixelHeight) {
        return std::unexpected(printError(rivet::core::ErrorCode::Io,
                                          "print spool file is missing or truncated: " +
                                              band.file.filename().string()));
    }
    const std::string path = band.file.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.size()), false);
    CGDataProviderRef provider = url != nullptr ? CGDataProviderCreateWithURL(url) : nullptr;
    if (url != nullptr) CFRelease(url);
    if (provider == nullptr) {
        return std::unexpected(printError(rivet::core::ErrorCode::Io, "cannot open print spool file"));
    }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(band.pixelWidth, band.pixelHeight, 8, 32, band.stride,
                                     colorSpace, kBandBitmapInfo, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (image == nullptr) {
        return std::unexpected(printError(rivet::core::ErrorCode::Io, "cannot decode print spool file"));
    }
    return image;
}

} // namespace

// The print-time drawing surface: one spooled page per AppKit page. Pages
// are laid out at the view origin (page identity comes from the operation's
// current page), each scaled down uniformly to fit the printable area when
// it is larger (never up); orientation follows the page's display size.
// drawRect only COMPOSITES the pre-rendered band images. NOTE: ObjC classes
// must live at global scope (no C++ namespaces).
@interface RivetSpoolPrintView : NSView {
@public
    const rivet::platform::PrintSpoolDescription* spool;
    rivet::core::Error* failure; // first draw-time failure (owned by printSpool)
    NSSize printableSize;        // view units available on one sheet
}
- (instancetype)initWithSpool:(const rivet::platform::PrintSpoolDescription*)printSpool
                      failure:(rivet::core::Error*)drawFailure
                printableSize:(NSSize)size;
@end

@implementation RivetSpoolPrintView
- (instancetype)initWithSpool:(const rivet::platform::PrintSpoolDescription*)printSpool
                      failure:(rivet::core::Error*)drawFailure
                printableSize:(NSSize)size {
    // The frame must cover every page rect (all laid out at the origin).
    NSSize extent = NSMakeSize(1.0, 1.0);
    self = [super initWithFrame:NSZeroRect];
    if (self != nil) {
        spool = printSpool;
        failure = drawFailure;
        printableSize = size;
        for (std::size_t index = 0; index < spool->pages.size(); ++index) {
            const NSRect rect = [self rectForPage:static_cast<NSInteger>(index + 1)];
            extent.width = std::max(extent.width, NSWidth(rect));
            extent.height = std::max(extent.height, NSHeight(rect));
        }
        [self setFrameSize:extent];
    }
    return self;
}

- (BOOL)isFlipped {
    // Rivet's page display space is top-left origin / y-down.
    return YES;
}

- (BOOL)knowsPageRange:(NSRange*)range {
    range->location = 1; // AppKit page numbers are 1-based
    range->length = spool->pages.size();
    return YES;
}

// Uniform scale (<= 1) that fits the page on one sheet.
- (double)fitScaleForPage:(const rivet::platform::PrintSpoolPage&)page {
    const rivet::core::Size size = page.displaySizePoints;
    if (size.isEmpty() || printableSize.width <= 0.0 || printableSize.height <= 0.0) return 1.0;
    return std::min({1.0, printableSize.width / size.width, printableSize.height / size.height});
}

- (NSRect)rectForPage:(NSInteger)pageNumber {
    const auto index = static_cast<std::size_t>(pageNumber - 1);
    if (pageNumber < 1 || index >= spool->pages.size()) return NSZeroRect;
    const rivet::platform::PrintSpoolPage& page = spool->pages[index];
    const double scale = [self fitScaleForPage:page];
    return NSMakeRect(0.0, 0.0, page.displaySizePoints.width * scale,
                      page.displaySizePoints.height * scale);
}

- (void)failWith:(rivet::core::Error)error {
    if (failure->code == rivet::core::ErrorCode::None) *failure = std::move(error);
    // Cancels the job: -runOperation returns NO and nothing is output
    // (tests/platform/TestPrintService.mm verifies this).
    [NSPrintOperation currentOperation].printInfo.jobDisposition = NSPrintCancelJob;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSPrintOperation* operation = [NSPrintOperation currentOperation];
    const auto index = static_cast<std::size_t>(operation.currentPage - 1);
    if (operation == nil || index >= spool->pages.size()) return;
    if (failure->code != rivet::core::ErrorCode::None) return; // job already cancelled
    CGContextRef context = [NSGraphicsContext currentContext].CGContext;
    if (context == nullptr) {
        [self failWith:printError(rivet::core::ErrorCode::Internal, "no print graphics context")];
        return;
    }

    const rivet::platform::PrintSpoolPage& page = spool->pages[index];
    const double scale = [self fitScaleForPage:page];
    CGContextSaveGState(context);
    CGContextScaleCTM(context, scale, scale);
    // Bands abut exactly; antialiased image edges would leave hairline seams.
    CGContextSetShouldAntialias(context, false);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    for (const rivet::platform::PrintSpoolBand& band : page.bands) {
        auto image = createBandImage(band);
        if (!image.has_value()) {
            CGContextRestoreGState(context);
            [self failWith:image.error()];
            return;
        }
        const rivet::core::Rect& rect = band.rectPoints;
        CGContextSaveGState(context);
        // Flipped view: flip around the band so the image's first row lands
        // on the band's top edge.
        CGContextTranslateCTM(context, rect.minX(), rect.maxY());
        CGContextScaleCTM(context, 1.0, -1.0);
        CGContextDrawImage(context, CGRectMake(0.0, 0.0, rect.size.width, rect.size.height), *image);
        CGContextRestoreGState(context);
        CGImageRelease(*image);
    }
    CGContextRestoreGState(context);
}
@end

namespace rivet::platform {

core::Result<PrintSettings> MacosPrintService::choosePrintSettings(const PrintSetup& setup) {
    if (setup.pageCount == 0) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "nothing to print"));
    }
    NSPrintInfo* info = [[NSPrintInfo sharedPrintInfo] copy];
    const core::Size first = setup.firstPageSizePoints;
    info.orientation = first.width > first.height ? NSPaperOrientationLandscape
                                                  : NSPaperOrientationPortrait;
    NSMutableDictionary* dictionary = info.dictionary;
    dictionary[NSPrintAllPages] = @YES;
    dictionary[NSPrintFirstPage] = @1;
    dictionary[NSPrintLastPage] = @(setup.pageCount);

    // No preview: a preview would rasterize pages on the main thread.
    NSPrintPanel* panel = [NSPrintPanel printPanel];
    panel.options = NSPrintPanelShowsCopies | NSPrintPanelShowsPageRange |
                    NSPrintPanelShowsPaperSize | NSPrintPanelShowsOrientation |
                    NSPrintPanelShowsScaling;
    if ([panel runModalWithPrintInfo:info] != NSModalResponseOK) {
        return std::unexpected(printError(core::ErrorCode::Cancelled, "print cancelled"));
    }

    PrintSettings settings;
    settings.jobTitle = setup.jobTitle;
    settings.firstPage = 0;
    settings.lastPage = setup.pageCount - 1;
    if (![dictionary[NSPrintAllPages] boolValue]) {
        // AppKit page numbers are 1-based; clamp to the document.
        const NSInteger from = [dictionary[NSPrintFirstPage] integerValue];
        const NSInteger to = [dictionary[NSPrintLastPage] integerValue];
        const auto count = static_cast<NSInteger>(setup.pageCount);
        const NSInteger clampedFrom = std::clamp<NSInteger>(from, 1, count);
        const NSInteger clampedTo = std::clamp<NSInteger>(to, 1, count);
        if (clampedTo < clampedFrom) {
            return std::unexpected(printError(core::ErrorCode::InvalidArgument,
                                              "the selected page range is empty"));
        }
        settings.firstPage = static_cast<std::size_t>(clampedFrom - 1);
        settings.lastPage = static_cast<std::size_t>(clampedTo - 1);
    }
    settings.platformHandle = wrapPrintInfo(info);
    return settings;
}

PrintSettings MacosPrintService::settingsForSavingPdf(const std::filesystem::path& output,
                                                      std::string jobTitle) {
    NSPrintInfo* info = [[NSPrintInfo sharedPrintInfo] copy];
    info.jobDisposition = NSPrintSaveJob;
    const std::string path = output.string();
    info.dictionary[NSPrintJobSavingURL] =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    PrintSettings settings;
    settings.jobTitle = std::move(jobTitle);
    settings.platformHandle = wrapPrintInfo(info);
    return settings;
}

core::Status MacosPrintService::printSpool(const PrintSettings& settings,
                                           const PrintSpoolDescription& spool) {
    NSPrintInfo* chosen = unwrapPrintInfo(settings.platformHandle);
    if (chosen == nil) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument,
                                          "print settings did not come from this print service"));
    }
    if (spool.pages.empty()) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "nothing to print"));
    }

    NSPrintInfo* info = [chosen copy];
    // The spool already contains exactly the chosen pages: print all of it.
    info.dictionary[NSPrintAllPages] = @YES;
    info.dictionary[NSPrintFirstPage] = @1;
    info.dictionary[NSPrintLastPage] = @(spool.pages.size());
    // Print into the printer's imageable area, centered.
    const NSSize paper = info.paperSize;
    const NSRect imageable = info.imageablePageBounds;
    info.leftMargin = NSMinX(imageable);
    info.bottomMargin = NSMinY(imageable);
    info.rightMargin = std::max(0.0, paper.width - NSMaxX(imageable));
    info.topMargin = std::max(0.0, paper.height - NSMaxY(imageable));
    info.horizontallyCentered = YES;
    info.verticallyCentered = YES;
    const double userScale = info.scalingFactor > 0.0 ? info.scalingFactor : 1.0;
    const NSSize printable = NSMakeSize(NSWidth(imageable) / userScale, NSHeight(imageable) / userScale);

    core::Error failure;
    RivetSpoolPrintView* view = [[RivetSpoolPrintView alloc] initWithSpool:&spool
                                                                   failure:&failure
                                                             printableSize:printable];
    NSPrintOperation* operation = [NSPrintOperation printOperationWithView:view printInfo:info];
    operation.showsPrintPanel = NO;
    operation.showsProgressPanel = showsProgressPanel_ ? YES : NO;
    operation.canSpawnSeparateThread = NO; // the spool must outlive the drawing
    if (!settings.jobTitle.empty()) {
        operation.jobTitle = [NSString stringWithUTF8String:settings.jobTitle.c_str()];
    }
    const BOOL ran = [operation runOperation];
    if (failure.code != core::ErrorCode::None) return std::unexpected(failure);
    if (!ran) return std::unexpected(printError(core::ErrorCode::Cancelled, "print cancelled"));
    return core::ok();
}

} // namespace rivet::platform
