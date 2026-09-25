// SPDX-License-Identifier: MPL-2.0
#import "MacosPrintService.h"

#import <AppKit/AppKit.h>

namespace {
constexpr double kMaxPrintDensity = 2.0; // device px per point (≈144 dpi at cap)
} // namespace

// The print-time drawing surface: one page per -rectForPage, content rendered
// by the Rivet callback into a bitmap and composited into the print context.
// AppKit drives page ranges and paper; this view only supplies geometry and
// pixels. NOTE: ObjC classes must live at global scope (no C++ namespaces).
@interface RivetPrintView : NSView {
@public
    rivet::platform::PrintRequest* request;
}
- (instancetype)initWithRequest:(rivet::platform::PrintRequest*)printRequest;
- (BOOL)knowsPageRange:(NSRange*)range;
- (NSRect)rectForPage:(NSInteger)pageIndex;
- (void)drawRect:(NSRect)dirtyRect;
@end

@implementation RivetPrintView
- (instancetype)initWithRequest:(rivet::platform::PrintRequest*)printRequest {
    // The frame is irrelevant: rectForPage supplies each page's geometry.
    self = [super initWithFrame:NSMakeRect(0, 0, 612, 792)];
    if (self != nil) {
        request = printRequest;
    }
    return self;
}

- (BOOL)isFlipped {
    // Rivet's page display space is top-left origin / y-down, matching a
    // flipped view 1:1 (same convention as the screen paint path).
    return YES;
}

- (BOOL)knowsPageRange:(NSRange*)range {
    range->location = 1; // AppKit page numbers are 1-based
    range->length = request->pageSizesPoints.size();
    return YES;
}

- (NSRect)rectForPage:(NSInteger)pageIndex {
    const std::size_t index = static_cast<std::size_t>(pageIndex - 1);
    if (index >= request->pageSizesPoints.size()) return NSZeroRect;
    const rivet::core::Size& size = request->pageSizesPoints[index];
    return NSMakeRect(0, 0, size.width, size.height);
}

- (void)drawRect:(NSRect)dirtyRect {
    if (request == nullptr || request->renderPage == nullptr) return;
    // The visible page: AppKit clips and translates the context per page via
    // rectForPage; the page under draw is the one whose rect intersects the
    // dirty rect (one page per sheet in this flow).
    NSGraphicsContext* graphicsContext = [NSGraphicsContext currentContext];
    if (graphicsContext == nil) return;
    CGContextRef context = graphicsContext.CGContext;
    if (context == nullptr) return;

    // AppKit may draw a page more than once; page identity comes from the
    // print operation's current page (the context is already set up for it).
    const NSInteger current = [[NSPrintOperation currentOperation] currentPage];
    const std::size_t index = static_cast<std::size_t>(current - 1);
    if (index >= request->pageSizesPoints.size()) return;

    const rivet::core::Size size = request->pageSizesPoints[index];
    // Density cap: bounded full-page bitmaps (see the header note on banding).
    // The callback renders the whole page (the shell binds the page rect).
    auto bitmap = request->renderPage(index, kMaxPrintDensity);
    if (!bitmap.has_value()) {
        // Leave the sheet blank rather than failing the whole operation.
        return;
    }
    const rivet::core::Bitmap& page = *bitmap;

    const auto bitmapInfo = static_cast<CGImageAlphaInfo>(
        static_cast<unsigned>(kCGImageAlphaFirst) |
        static_cast<unsigned>(kCGBitmapByteOrder32Little));
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, page.data(), page.sizeBytes(), nullptr);
    CGImageRef image =
        CGImageCreate(page.width(), page.height(), 8, 32, page.stride(), colorSpace, bitmapInfo,
                      provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    if (image == nullptr) return;

    CGContextSaveGState(context);
    // Flipped view: flip around the page rect so the bitmap's first row lands
    // on the page's top edge.
    CGContextTranslateCTM(context, 0.0, size.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, size.width, size.height), image);
    CGContextRestoreGState(context);
    CGImageRelease(image);
}
@end

namespace rivet::platform {

core::Status MacosPrintService::printDocument(const PrintRequest& printRequest) {
    if (printRequest.pageSizesPoints.empty()) {
        return std::unexpected(
            core::Error{core::ErrorCode::InvalidArgument, "nothing to print", "platform"});
    }
    request_ = printRequest;

    RivetPrintView* view = [[RivetPrintView alloc] initWithRequest:&request_];
    // The user's shared print info drives the dialog (printer, page range,
    // paper). PDF output via the panel's PDF button needs no printer.
    NSPrintInfo* printInfo = [[NSPrintInfo sharedPrintInfo] copy];
    NSPrintOperation* operation =
        [NSPrintOperation printOperationWithView:view printInfo:printInfo];
    operation.showsPrintPanel = YES;
    operation.canSpawnSeparateThread = NO;
    const BOOL ran = [operation runOperation];
    request_.renderPage = nullptr;
    request_.pageSizesPoints.clear();
    if (!ran) {
        return std::unexpected(core::Error{core::ErrorCode::Cancelled, "print cancelled", "platform"});
    }
    return core::ok();
}

} // namespace rivet::platform
