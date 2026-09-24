#include "MacosPaintContext.h"

#include <CoreText/CoreText.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace rivet::platform {
namespace {

CGRect toCGRect(const core::Rect& rect) {
    return CGRectMake(rect.minX(), rect.minY(), rect.size.width, rect.size.height);
}

void applyFillColor(CGContextRef context, const rivet::ui::Color& color) {
    CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a);
}

void applyStrokeColor(CGContextRef context, const rivet::ui::Color& color) {
    CGContextSetRGBStrokeColor(context, color.r, color.g, color.b, color.a);
}

// Process-lifetime CTFont cache: creating fonts per draw call is slow, and the
// shell only uses a handful of (size, weight) combinations. Painting happens
// on the main thread, but measureText can be reached from layout code, so the
// cache is mutex-guarded regardless.
CTFontRef cachedFontFor(const rivet::ui::Font& font) {
    static std::mutex mutex;
    static std::map<std::pair<double, int>, CTFontRef> cache;

    const std::pair<double, int> key{font.size, static_cast<int>(font.weight)};
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    CTFontRef created = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, font.size, nullptr);
    if (created != nullptr && font.weight != rivet::ui::Font::Weight::Regular) {
        // Semibold and Bold both come from the symbolic bold trait; the system
        // font family has no distinct semibold member exposed this way.
        const CTFontSymbolicTraits bold = kCTFontTraitBold;
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(created, font.size, nullptr, bold, bold);
        if (styled != nullptr) {
            CFRelease(created);
            created = styled;
        }
    }

    const std::lock_guard<std::mutex> lock(mutex);
    const auto [it, inserted] = cache.emplace(key, created);
    if (!inserted && created != nullptr) CFRelease(created); // lost a race
    return it->second;
}

struct TextMetrics {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

// Layout of `text` in `font`: CTLineGetTypographicBounds width plus the
// ascent/descent needed for vertical centering. measureText() and drawText()
// both go through this, so they agree by construction.
TextMetrics measureLine(std::string_view text, const rivet::ui::Font& font) {
    TextMetrics metrics;
    if (text.empty()) return metrics;

    CTFontRef ctFont = cachedFontFor(font);
    CFStringRef cfText =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    if (cfText == nullptr) return metrics;

    const void* keys[] = {kCTFontAttributeName};
    const void* values[] = {ctFont};
    CFDictionaryRef attributes =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cfText, attributes);
    CTLineRef line = attributed != nullptr ? CTLineCreateWithAttributedString(attributed) : nullptr;
    if (line != nullptr) {
        CGFloat ascent = 0.0;
        CGFloat descent = 0.0;
        metrics.width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
        metrics.ascent = ascent;
        metrics.descent = descent;
        CFRelease(line);
    }
    if (attributed != nullptr) CFRelease(attributed);
    if (attributes != nullptr) CFRelease(attributes);
    CFRelease(cfText);
    return metrics;
}

// Draws one CoreText line right-side up in this y-down (flipped-view)
// context: CoreText itself draws y-up, so the caller has already flipped the
// CTM around the baseline before calling this.
void drawLineAtOrigin(std::string_view text, const rivet::ui::Font& font,
                      const rivet::ui::Color& color, CGContextRef context) {
    CTFontRef ctFont = cachedFontFor(font);
    CFStringRef cfText =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    if (cfText == nullptr) return;

    CGColorRef cgColor = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* values[] = {ctFont, cgColor};
    CFDictionaryRef attributes =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cfText, attributes);
    CTLineRef line = attributed != nullptr ? CTLineCreateWithAttributedString(attributed) : nullptr;
    if (line != nullptr) {
        CTLineDraw(line, context);
        CFRelease(line);
    }
    if (attributed != nullptr) CFRelease(attributed);
    if (attributes != nullptr) CFRelease(attributes);
    CGColorRelease(cgColor);
    CFRelease(cfText);
}

} // namespace

MacosPaintContext::MacosPaintContext(CGContextRef context, double backingScale)
    : context_(context), backingScale_(backingScale > 0.0 ? backingScale : 1.0) {}

void MacosPaintContext::pushClip(const core::Rect& logicalRect) {
    // Clip first (the rect is given in the CURRENT coordinate space), then
    // translate so rect.origin becomes (0,0) for the clipped children: after
    // CGContextTranslateCTM(ctx, tx, ty) a point p maps to p + (tx, ty) in the
    // previous space, so the offset must be +origin (local (0,0) has to land
    // on the frame origin).
    CGContextSaveGState(context_);
    CGContextClipToRect(context_, toCGRect(logicalRect));
    CGContextTranslateCTM(context_, logicalRect.minX(), logicalRect.minY());
}

void MacosPaintContext::popClip() {
    CGContextRestoreGState(context_);
}

void MacosPaintContext::fillRect(const core::Rect& rect, const rivet::ui::Color& color) {
    if (rect.isEmpty()) return;
    applyFillColor(context_, color);
    CGContextFillRect(context_, toCGRect(rect));
}

void MacosPaintContext::fillRoundedRect(const core::Rect& rect, const rivet::ui::Color& color,
                                        double cornerRadius) {
    if (rect.isEmpty()) return;
    applyFillColor(context_, color);
    CGPathRef path = CGPathCreateWithRoundedRect(toCGRect(rect), cornerRadius, cornerRadius, nullptr);
    CGContextAddPath(context_, path);
    CGContextFillPath(context_);
    CGPathRelease(path);
}

void MacosPaintContext::strokeRect(const core::Rect& rect, const rivet::ui::Color& color,
                                   double strokeWidth) {
    if (rect.isEmpty()) return;
    applyStrokeColor(context_, color);
    CGContextSetLineWidth(context_, strokeWidth);
    CGContextStrokeRect(context_, toCGRect(rect));
}

void MacosPaintContext::drawLine(core::Point from, core::Point to, const rivet::ui::Color& color,
                                 double strokeWidth) {
    applyStrokeColor(context_, color);
    CGContextSetLineWidth(context_, strokeWidth);
    CGContextMoveToPoint(context_, from.x, from.y);
    CGContextAddLineToPoint(context_, to.x, to.y);
    CGContextStrokePath(context_);
}

void MacosPaintContext::drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) {
    if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0) return;
    if (destLogicalRect.isEmpty()) return;

    // core::Bitmap is 8bpc premultiplied BGRA in memory, which is exactly
    // little-endian ARGB for CoreGraphics. The alpha and byte-order infos are
    // separate enum types; OR them numerically before the parameter cast.
    const auto bitmapInfo = static_cast<CGImageAlphaInfo>(
        static_cast<unsigned>(kCGImageAlphaPremultipliedFirst) |
        static_cast<unsigned>(kCGBitmapByteOrder32Little));
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, bitmap.data(), bitmap.sizeBytes(), nullptr);
    CGImageRef image = CGImageCreate(
        bitmap.width(), bitmap.height(), 8, 32, bitmap.stride(), colorSpace, bitmapInfo, provider,
        nullptr, false, kCGRenderingIntentDefault);

    if (image != nullptr) {
        CGContextSaveGState(context_);
        // CGImage renders bottom-up; flip around the destination so the first
        // bitmap row lands on the rect's top edge instead of the bottom.
        CGContextTranslateCTM(context_, destLogicalRect.minX(), destLogicalRect.maxY());
        CGContextScaleCTM(context_, 1.0, -1.0);
        CGContextDrawImage(
            context_, CGRectMake(0.0, 0.0, destLogicalRect.size.width, destLogicalRect.size.height),
            image);
        CGContextRestoreGState(context_);
        CGImageRelease(image);
    }

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);
}

core::Size MacosPaintContext::measureText(std::string_view text, const rivet::ui::Font& font) const {
    const TextMetrics metrics = measureLine(text, font);
    return core::Size{metrics.width, metrics.ascent + metrics.descent};
}

void MacosPaintContext::drawText(std::string_view text, const core::Rect& rect,
                                 const rivet::ui::Font& font, const rivet::ui::Color& color,
                                 rivet::ui::TextAlign align) {
    if (text.empty() || rect.isEmpty()) return;

    const TextMetrics metrics = measureLine(text, font);
    double x = rect.minX();
    switch (align) {
    case rivet::ui::TextAlign::Left: break;
    case rivet::ui::TextAlign::Center:
        x = rect.minX() + (rect.size.width - metrics.width) / 2.0;
        break;
    case rivet::ui::TextAlign::Right:
        x = rect.maxX() - metrics.width;
        break;
    }

    // Vertical centering: the line box is (ascent + descent) tall; its top
    // edge sits at rect.minY + (rectHeight - lineBox)/2 and the baseline is
    // lineBox top + ascent.
    const double lineBox = metrics.ascent + metrics.descent;
    const double baseline = rect.minY() + (rect.size.height - lineBox) / 2.0 + metrics.ascent;

    CGContextSaveGState(context_);
    // CoreText draws with y-up; flip around the baseline so the glyphs are
    // right-side up in this y-down context.
    CGContextTranslateCTM(context_, x, baseline);
    CGContextScaleCTM(context_, 1.0, -1.0);
    // AppKit contexts carry a persistent text position (advanced by every
    // CTLineDraw by the line's advance) and a text matrix; without pinning
    // both, each line ends up shifted right by the sum of all previously
    // drawn lines' widths.
    CGContextSetTextMatrix(context_, CGAffineTransformIdentity);
    CGContextSetTextPosition(context_, 0.0, 0.0);
    drawLineAtOrigin(text, font, color, context_);
    CGContextRestoreGState(context_);
}

} // namespace rivet::platform
