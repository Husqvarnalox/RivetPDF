// SPDX-License-Identifier: MPL-2.0
#import "MacosTextRecognizer.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.macos.ocr";

// Most lines / words kept from one page: Vision output is untrusted-size data
// only in the sense of resource use, but a runaway page must not exhaust
// memory downstream.
constexpr std::size_t kMaxLines = 20000;
constexpr std::size_t kMaxWordsPerPage = 50000;

// Messages never carry recognized text.
core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

template <typename T>
class CfRef {
public:
    explicit CfRef(T ref = nullptr) : ref_(ref) {}
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    ~CfRef() {
        if (ref_ != nullptr) CFRelease(ref_);
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    T ref_;
};

// Vision's normalized points have their origin at the bottom-left.
core::Point toPixels(CGPoint p, double width, double height) {
    return core::Point{static_cast<double>(p.x) * width, (1.0 - static_cast<double>(p.y)) * height};
}

// The word ranges of `text`: maximal runs of non-whitespace characters.
std::vector<NSRange> wordRanges(NSString* text) {
    std::vector<NSRange> ranges;
    const NSUInteger length = text.length;
    NSCharacterSet* spaces = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    NSUInteger i = 0;
    while (i < length) {
        while (i < length && [spaces characterIsMember:[text characterAtIndex:i]]) ++i;
        const NSUInteger start = i;
        while (i < length && ![spaces characterIsMember:[text characterAtIndex:i]]) ++i;
        if (i > start) ranges.push_back(NSMakeRange(start, i - start));
    }
    return ranges;
}

} // namespace

core::Result<RecognizedPage> MacosTextRecognizer::recognize(const core::Bitmap& bitmap,
                                                            std::span<const std::string> languages,
                                                            std::stop_token stop) const {
    @autoreleasepool {
        try {
            if (stop.stop_requested()) {
                return std::unexpected(fail(core::ErrorCode::Cancelled, "text recognition cancelled"));
            }
            if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0) {
                return std::unexpected(fail(core::ErrorCode::InvalidArgument, "the bitmap is empty"));
            }
            const double width = bitmap.width();
            const double height = bitmap.height();

            // The bitmap is BGRA with straight alpha on an opaque page; the
            // alpha channel is ignored. The provider borrows the pixels, which
            // outlive the request below.
            CfRef<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
            CfRef<CGDataProviderRef> provider(
                CGDataProviderCreateWithData(nullptr, bitmap.data(), bitmap.sizeBytes(), nullptr));
            if (!space || !provider) {
                return std::unexpected(fail(core::ErrorCode::Internal, "could not wrap the bitmap"));
            }
            CfRef<CGImageRef> image(CGImageCreate(bitmap.width(), bitmap.height(), 8, 32, bitmap.stride(),
                                                  space.get(),
                                                  static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst),
                                                  provider.get(), nullptr, false, kCGRenderingIntentDefault));
            if (!image) {
                return std::unexpected(fail(core::ErrorCode::Internal, "could not wrap the bitmap"));
            }

            VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
            request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            request.usesLanguageCorrection = YES;

            if (!languages.empty()) {
                NSError* langError = nil;
                NSArray<NSString*>* supported = [request supportedRecognitionLanguagesAndReturnError:&langError];
                NSMutableArray<NSString*>* chosen = [NSMutableArray array];
                for (const std::string& tag : languages) {
                    NSString* wanted = [NSString stringWithUTF8String:tag.c_str()];
                    if (wanted == nil) continue;
                    for (NSString* candidate in supported) {
                        if ([candidate caseInsensitiveCompare:wanted] == NSOrderedSame) {
                            [chosen addObject:candidate];
                            break;
                        }
                    }
                }
                if (chosen.count > 0) request.recognitionLanguages = chosen;
            }

            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:image.get()
                                                                                    options:@{}];
            // Vision cannot be interrupted from outside except through the
            // request's own cancel().
            VNRecognizeTextRequest* __weak weakRequest = request;
            std::stop_callback onStop(stop, [weakRequest] { [weakRequest cancel]; });

            NSError* error = nil;
            const BOOL performed = [handler performRequests:@[ request ] error:&error];
            if (stop.stop_requested()) {
                return std::unexpected(fail(core::ErrorCode::Cancelled, "text recognition cancelled"));
            }
            if (!performed) {
                return std::unexpected(fail(core::ErrorCode::Internal, "the recognition request failed"));
            }

            RecognizedPage page;
            std::size_t wordCount = 0;
            for (VNRecognizedTextObservation* observation in request.results) {
                if (page.lines.size() >= kMaxLines || wordCount >= kMaxWordsPerPage) break;
                VNRecognizedText* best = [[observation topCandidates:1] firstObject];
                if (best == nil) continue;
                NSString* string = best.string;
                RecognizedLine line;
                line.confidence = best.confidence;
                for (const NSRange range : wordRanges(string)) {
                    if (wordCount >= kMaxWordsPerPage) break;
                    NSError* boxError = nil;
                    VNRectangleObservation* box = [best boundingBoxForRange:range error:&boxError];
                    if (box == nil) continue;
                    RecognizedWord word;
                    const char* utf8 = [[string substringWithRange:range] UTF8String];
                    if (utf8 == nullptr) continue;
                    word.text = utf8;
                    // Vision orders the corners in the text's own orientation.
                    word.quad = {toPixels(box.topLeft, width, height), toPixels(box.topRight, width, height),
                                 toPixels(box.bottomRight, width, height), toPixels(box.bottomLeft, width, height)};
                    word.confidence = best.confidence;
                    line.words.push_back(std::move(word));
                    ++wordCount;
                }
                if (!line.words.empty()) page.lines.push_back(std::move(line));
            }
            return page;
        } catch (const std::bad_alloc&) {
            return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while recognizing"));
        }
    }
}

} // namespace rivet::platform
