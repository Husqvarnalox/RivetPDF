// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#import "platform/macos/MacosPrintService.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Non-interactive AppKit print jobs saved to PDF files in a temp directory
// (NSPrintSaveJob, no panels): the flow printSpool() uses in production,
// minus the printer. Runs on the test process's main thread.

namespace {

struct Rgb {
    std::uint8_t r, g, b;
};

std::filesystem::path freshDirectory(const char* name) {
    auto path = std::filesystem::temp_directory_path() / (std::string{"rivet-print-test-"} + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

// Writes a solid-color band file (BGRA8888Straight, tight stride).
rivet::platform::PrintSpoolBand writeBand(const std::filesystem::path& file, std::uint32_t width,
                                          std::uint32_t height, Rgb color,
                                          const rivet::core::Rect& rectPoints) {
    std::vector<std::uint8_t> bytes(std::size_t{width} * height * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = color.b;
        bytes[i + 1] = color.g;
        bytes[i + 2] = color.r;
        bytes[i + 3] = 255;
    }
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return rivet::platform::PrintSpoolBand{file, width, height, std::size_t{width} * 4, rectPoints};
}

// Bounding box (in media points, top-left origin) of the pixels of an
// output page that are close to `color`; empty when none.
struct Box {
    long minX = 1'000'000, minY = 1'000'000, maxX = -1, maxY = -1;
    std::size_t count = 0;
    bool empty() const { return count == 0; }
};

struct RenderedPage {
    std::size_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;

    Box find(Rgb color) const {
        Box box;
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::uint8_t* p = &rgba[(y * width + x) * 4];
                if (std::abs(p[0] - color.r) < 40 && std::abs(p[1] - color.g) < 40 &&
                    std::abs(p[2] - color.b) < 40) {
                    box.minX = std::min(box.minX, static_cast<long>(x));
                    box.maxX = std::max(box.maxX, static_cast<long>(x));
                    box.minY = std::min(box.minY, static_cast<long>(y));
                    box.maxY = std::max(box.maxY, static_cast<long>(y));
                    ++box.count;
                }
            }
        }
        return box;
    }
};

std::vector<RenderedPage> renderPdf(const std::filesystem::path& file) {
    std::vector<RenderedPage> pages;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.string().c_str()]];
    CGPDFDocumentRef document = CGPDFDocumentCreateWithURL((__bridge CFURLRef)url);
    if (document == nullptr) return pages;
    for (std::size_t index = 1; index <= CGPDFDocumentGetNumberOfPages(document); ++index) {
        CGPDFPageRef page = CGPDFDocumentGetPage(document, index);
        const CGRect media = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
        RenderedPage rendered;
        rendered.width = static_cast<std::size_t>(media.size.width);
        rendered.height = static_cast<std::size_t>(media.size.height);
        rendered.rgba.assign(rendered.width * rendered.height * 4, 0);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        // Rows top-down in memory: CG bitmap contexts store the top row first.
        CGContextRef context = CGBitmapContextCreate(
            rendered.rgba.data(), rendered.width, rendered.height, 8, rendered.width * 4, colorSpace,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(colorSpace);
        CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
        CGContextFillRect(context, CGRectMake(0, 0, media.size.width, media.size.height));
        CGContextDrawPDFPage(context, page);
        CGContextRelease(context);
        pages.push_back(std::move(rendered));
    }
    CGPDFDocumentRelease(document);
    return pages;
}

} // namespace

// The AppKit mechanism printSpool() relies on: setting the running
// operation's printInfo.jobDisposition to NSPrintCancelJob from -drawRect
// makes -runOperation return NO, stops drawing the remaining pages and
// produces no output file.
@interface RivetCancellingTestView : NSView {
@public
    NSInteger cancelOnPage; // 0 = never
    NSInteger pagesDrawn;
}
@end

@implementation RivetCancellingTestView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)knowsPageRange:(NSRange*)range {
    range->location = 1;
    range->length = 3;
    return YES;
}
- (NSRect)rectForPage:(NSInteger)page {
    (void)page;
    return NSMakeRect(0, 0, 200, 200);
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    ++pagesDrawn;
    [[NSColor redColor] set];
    NSRectFill(NSMakeRect(10, 10, 50, 50));
    NSPrintOperation* operation = [NSPrintOperation currentOperation];
    if (operation.currentPage == cancelOnPage) operation.printInfo.jobDisposition = NSPrintCancelJob;
}
@end

namespace {

struct RawRun {
    BOOL ran = NO;
    NSInteger pagesDrawn = 0;
    bool outputExists = false;
};

RawRun runRawSaveJob(const std::filesystem::path& output, NSInteger cancelOnPage) {
    NSPrintInfo* info = [[NSPrintInfo sharedPrintInfo] copy];
    info.jobDisposition = NSPrintSaveJob;
    info.dictionary[NSPrintJobSavingURL] =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:output.string().c_str()]];
    RivetCancellingTestView* view = [[RivetCancellingTestView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    view->cancelOnPage = cancelOnPage;
    NSPrintOperation* operation = [NSPrintOperation printOperationWithView:view printInfo:info];
    operation.showsPrintPanel = NO;
    operation.showsProgressPanel = NO;
    RawRun run;
    run.ran = [operation runOperation];
    run.pagesDrawn = view->pagesDrawn;
    run.outputExists = std::filesystem::exists(output);
    return run;
}

} // namespace

RIVET_TEST(appKitCancelJobDispositionDuringDrawingSuppressesOutput) {
    [NSApplication sharedApplication];
    const auto directory = freshDirectory("disposition");

    const RawRun normal = runRawSaveJob(directory / "normal.pdf", 0);
    CHECK(normal.ran == YES);
    CHECK_EQ(normal.pagesDrawn, NSInteger{3});
    CHECK(normal.outputExists);

    const RawRun cancelled = runRawSaveJob(directory / "cancelled.pdf", 2);
    CHECK(cancelled.ran == NO);
    CHECK_EQ(cancelled.pagesDrawn, NSInteger{2}); // page 3 is never drawn
    CHECK(!cancelled.outputExists);
    std::filesystem::remove_all(directory);
}

RIVET_TEST(printSpoolCompositesBandsIntoEveryPage) {
    [NSApplication sharedApplication];
    const auto directory = freshDirectory("composite");
    using rivet::core::Rect;

    // Page 1: portrait 200x300 pt, red top band + blue bottom band.
    // Page 2: landscape 300x200 pt, one green band.
    rivet::platform::PrintSpoolDescription spool;
    spool.pages.push_back({0, rivet::core::Size{200.0, 300.0},
                           {writeBand(directory / "p0b0", 100, 75, {255, 0, 0}, Rect{0, 0, 200, 150}),
                            writeBand(directory / "p0b1", 100, 75, {0, 0, 255}, Rect{0, 150, 200, 150})}});
    spool.pages.push_back({1, rivet::core::Size{300.0, 200.0},
                           {writeBand(directory / "p1b0", 150, 100, {0, 255, 0}, Rect{0, 0, 300, 200})}});

    rivet::platform::MacosPrintService service;
    service.setShowsProgressPanel(false);
    const auto output = directory / "out.pdf";
    const auto printed =
        service.printSpool(rivet::platform::MacosPrintService::settingsForSavingPdf(output, "test"), spool);
    CHECK(printed.has_value());

    const auto pages = renderPdf(output);
    CHECK_EQ(pages.size(), std::size_t{2});
    const Box red = pages[0].find({255, 0, 0});
    const Box blue = pages[0].find({0, 0, 255});
    CHECK(!red.empty());
    CHECK(!blue.empty());
    // Unscaled (the page fits the sheet), red above blue with no seam.
    CHECK_NEAR(static_cast<double>(red.maxX - red.minX + 1), 200.0, 2.0);
    CHECK_NEAR(static_cast<double>(blue.maxY - red.minY + 1), 300.0, 2.0);
    CHECK_LT(red.maxY, blue.minY);
    CHECK_LE(blue.minY - red.maxY, 1);
    const Box green = pages[1].find({0, 255, 0});
    CHECK_NEAR(static_cast<double>(green.maxX - green.minX + 1), 300.0, 2.0);
    CHECK_NEAR(static_cast<double>(green.maxY - green.minY + 1), 200.0, 2.0);
    std::filesystem::remove_all(directory);
}

RIVET_TEST(printSpoolScalesOversizedPagesToFitTheSheet) {
    [NSApplication sharedApplication];
    const auto directory = freshDirectory("oversized");
    using rivet::core::Rect;

    // A 2000x3000 pt poster: must land on ONE sheet, whole, aspect kept.
    rivet::platform::PrintSpoolDescription spool;
    spool.pages.push_back({0, rivet::core::Size{2000.0, 3000.0},
                           {writeBand(directory / "p0b0", 40, 60, {255, 0, 255}, Rect{0, 0, 2000, 3000})}});
    rivet::platform::MacosPrintService service;
    service.setShowsProgressPanel(false);
    const auto output = directory / "out.pdf";
    CHECK(service.printSpool(rivet::platform::MacosPrintService::settingsForSavingPdf(output, "poster"), spool)
              .has_value());

    const auto pages = renderPdf(output);
    CHECK_EQ(pages.size(), std::size_t{1});
    const Box magenta = pages[0].find({255, 0, 255});
    CHECK(!magenta.empty());
    const double width = static_cast<double>(magenta.maxX - magenta.minX + 1);
    const double height = static_cast<double>(magenta.maxY - magenta.minY + 1);
    CHECK_NEAR(width / height, 2.0 / 3.0, 0.02);
    // Fills the sheet along its limiting axis (not clipped, not tiny).
    const double fillsWidth = width / static_cast<double>(pages[0].width);
    const double fillsHeight = height / static_cast<double>(pages[0].height);
    CHECK_GT(std::max(fillsWidth, fillsHeight), 0.9);
    CHECK(magenta.minX >= 0 && magenta.maxX < static_cast<long>(pages[0].width));
    std::filesystem::remove_all(directory);
}

RIVET_TEST(printSpoolUnreadableBandFailsWithoutOutput) {
    [NSApplication sharedApplication];
    const auto directory = freshDirectory("unreadable");
    using rivet::core::Rect;

    rivet::platform::PrintSpoolDescription spool;
    spool.pages.push_back({0, rivet::core::Size{200.0, 200.0},
                           {writeBand(directory / "p0b0", 50, 50, {255, 0, 0}, Rect{0, 0, 200, 200})}});
    spool.pages.push_back({1, rivet::core::Size{200.0, 200.0},
                           {writeBand(directory / "p1b0", 50, 50, {0, 0, 255}, Rect{0, 0, 200, 200})}});
    spool.pages.push_back({2, rivet::core::Size{200.0, 200.0},
                           {writeBand(directory / "p2b0", 50, 50, {0, 255, 0}, Rect{0, 0, 200, 200})}});
    std::filesystem::remove(directory / "p1b0"); // vanishes before printing

    rivet::platform::MacosPrintService service;
    service.setShowsProgressPanel(false);
    const auto output = directory / "out.pdf";
    const auto printed =
        service.printSpool(rivet::platform::MacosPrintService::settingsForSavingPdf(output, "broken"), spool);
    CHECK(!printed.has_value());
    CHECK(printed.error().code == rivet::core::ErrorCode::Io);
    CHECK(!std::filesystem::exists(output)); // never a partial or blank job

    // A truncated file is caught the same way.
    std::ofstream(directory / "p1b0", std::ios::binary).write("xx", 2);
    const auto truncated =
        service.printSpool(rivet::platform::MacosPrintService::settingsForSavingPdf(output, "broken"), spool);
    CHECK(!truncated.has_value());
    CHECK(truncated.error().code == rivet::core::ErrorCode::Io);
    CHECK(!std::filesystem::exists(output));
    std::filesystem::remove_all(directory);
}
