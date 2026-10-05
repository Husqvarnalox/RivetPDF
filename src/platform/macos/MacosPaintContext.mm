#include "MacosPaintContext.h"

#include <CoreText/CoreText.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
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
    static std::map<std::tuple<double, int, bool, bool>, CTFontRef> cache;

    const std::tuple<double, int, bool, bool> key{font.size, static_cast<int>(font.weight), font.italic,
                                                  font.monospace};
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    CTFontRef created = CTFontCreateUIFontForLanguage(
        font.monospace ? kCTFontUIFontUserFixedPitch : kCTFontUIFontSystem, font.size, nullptr);
    if (created != nullptr) {
        // Semibold and Bold both come from the symbolic bold trait; the system
        // font family has no distinct semibold member exposed this way.
        CTFontSymbolicTraits traits = 0;
        if (font.weight != rivet::ui::Font::Weight::Regular) traits |= kCTFontTraitBold;
        if (font.italic) traits |= kCTFontTraitItalic;
        if (traits != 0) {
            CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(created, font.size, nullptr, traits, traits);
            if (styled == nullptr && font.italic) {
                // No italic face for this family/weight: synthetic oblique
                // (about 12 degrees of shear) on top of the bold variant.
                CTFontRef base = created;
                if (font.weight != rivet::ui::Font::Weight::Regular) {
                    CTFontRef bold = CTFontCreateCopyWithSymbolicTraits(
                        created, font.size, nullptr, kCTFontTraitBold, kCTFontTraitBold);
                    if (bold != nullptr) base = bold;
                }
                const CGAffineTransform shear = CGAffineTransformMake(1.0, 0.0, 0.21, 1.0, 0.0, 0.0);
                styled = CTFontCreateCopyWithAttributes(base, font.size, &shear, nullptr);
                if (base != created) CFRelease(base);
            }
            // Otherwise (bold unavailable) the regular face is kept.
            if (styled != nullptr) {
                CFRelease(created);
                created = styled;
            }
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

// Adds the path's subpaths to the context's current path.
void addPathToContext(CGContextRef context, const rivet::ui::Path& path) {
    using Op = rivet::ui::PathSegment::Op;
    for (const rivet::ui::PathSegment& segment : path.segments) {
        switch (segment.op) {
        case Op::MoveTo: CGContextMoveToPoint(context, segment.p.x, segment.p.y); break;
        case Op::LineTo: CGContextAddLineToPoint(context, segment.p.x, segment.p.y); break;
        case Op::CubicTo:
            CGContextAddCurveToPoint(context, segment.c1.x, segment.c1.y, segment.c2.x,
                                     segment.c2.y, segment.p.x, segment.p.y);
            break;
        case Op::Close: CGContextClosePath(context); break;
        }
    }
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

void MacosPaintContext::strokeDashedRect(const core::Rect& rect, const rivet::ui::Color& color,
                                         double strokeWidth, double dash) {
    if (rect.isEmpty()) return;
    CGContextSaveGState(context_);
    applyStrokeColor(context_, color);
    CGContextSetLineWidth(context_, strokeWidth);
    if (dash > 0.0) {
        const CGFloat lengths[] = {static_cast<CGFloat>(dash), static_cast<CGFloat>(dash)};
        CGContextSetLineDash(context_, 0.0, lengths, 2);
    }
    CGContextStrokeRect(context_, toCGRect(rect));
    CGContextRestoreGState(context_);
}

void MacosPaintContext::fillPath(const rivet::ui::Path& path, const rivet::ui::Color& color) {
    if (path.empty()) return;
    applyFillColor(context_, color);
    CGContextBeginPath(context_);
    addPathToContext(context_, path);
    CGContextFillPath(context_); // nonzero winding
}

void MacosPaintContext::strokePath(const rivet::ui::Path& path, const rivet::ui::Color& color,
                                   double strokeWidth, rivet::ui::LineCap cap,
                                   rivet::ui::LineJoin join) {
    if (path.empty()) return;
    CGContextSaveGState(context_);
    applyStrokeColor(context_, color);
    CGContextSetLineWidth(context_, strokeWidth);
    CGContextSetLineCap(context_, cap == rivet::ui::LineCap::Round ? kCGLineCapRound : kCGLineCapButt);
    CGContextSetLineJoin(context_,
                         join == rivet::ui::LineJoin::Round ? kCGLineJoinRound : kCGLineJoinMiter);
    CGContextBeginPath(context_);
    addPathToContext(context_, path);
    CGContextStrokePath(context_);
    CGContextRestoreGState(context_);
}

void MacosPaintContext::drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) {
    if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0) return;
    if (destLogicalRect.isEmpty()) return;

    // core::Bitmap is 8bpc straight-alpha BGRA in memory, which is exactly
    // little-endian ARGB for CoreGraphics; kCGImageAlphaFirst is the matching
    // straight-alpha layout (tiles are opaque in practice today — pages are
    // filled white — but the info must match the actual contract). The alpha
    // and byte-order infos are separate enum types; OR them numerically
    // before the parameter cast.
    const auto bitmapInfo = static_cast<CGImageAlphaInfo>(
        static_cast<unsigned>(kCGImageAlphaFirst) |
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

void MacosPaintContext::drawTextInBox(std::string_view text, const core::Rect& box,
                                      int quarterTurns, const rivet::ui::Font& font,
                                      const rivet::ui::Color& color) {
    if (text.empty() || box.isEmpty()) return;

    rivet::ui::Font boldFont = font;
    boldFont.weight = rivet::ui::Font::Weight::Bold;
    const TextMetrics metrics = measureLine(text, boldFont);
    const double lineHeight = metrics.ascent + metrics.descent;
    if (metrics.width <= 0.0 || lineHeight <= 0.0) return;

    const int turns = ((quarterTurns % 4) + 4) % 4;
    const bool swapped = (turns % 2) != 0;
    const double availWidth = (swapped ? box.size.height : box.size.width) * 0.9;
    const double availHeight = (swapped ? box.size.width : box.size.height) * 0.9;
    const double scale = std::fmin(availWidth / metrics.width, availHeight / lineHeight);
    if (!(scale > 0.0)) return;

    const core::Point center = box.center();
    CGContextSaveGState(context_);
    CGContextTranslateCTM(context_, center.x, center.y);
    // In this y-down context a positive angle turns clockwise on screen.
    CGContextRotateCTM(context_, static_cast<CGFloat>(turns) * (M_PI / 2.0));
    CGContextScaleCTM(context_, scale, scale);
    drawText(text,
             core::Rect{core::Point{-metrics.width / 2.0, -lineHeight / 2.0},
                        core::Size{metrics.width, lineHeight}},
             boldFont, color, rivet::ui::TextAlign::Left);
    CGContextRestoreGState(context_);
}

} // namespace rivet::platform
