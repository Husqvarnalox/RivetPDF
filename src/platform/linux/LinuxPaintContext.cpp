// SPDX-License-Identifier: MPL-2.0
#include "LinuxPaintContext.hpp"

#include "LinuxTextShaper.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace rivet::platform {

namespace {

void setColor(cairo_t* cr, const ui::Color& color) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
}

void addRect(cairo_t* cr, const core::Rect& rect) {
    cairo_rectangle(cr, rect.minX(), rect.minY(), rect.size.width, rect.size.height);
}

void addPath(cairo_t* cr, const ui::Path& path) {
    using Op = ui::PathSegment::Op;
    for (const ui::PathSegment& segment : path.segments) {
        switch (segment.op) {
        case Op::MoveTo: cairo_move_to(cr, segment.p.x, segment.p.y); break;
        case Op::LineTo:
            // CoreGraphics starts a subpath at a LineTo without a current
            // point; cairo's line_to does the same (acts as move_to).
            cairo_line_to(cr, segment.p.x, segment.p.y);
            break;
        case Op::CubicTo:
            cairo_curve_to(cr, segment.c1.x, segment.c1.y, segment.c2.x, segment.c2.y, segment.p.x,
                           segment.p.y);
            break;
        case Op::Close: cairo_close_path(cr); break;
        }
    }
}

// True when every pixel is opaque: then straight == premultiplied and the
// rows can be handed to cairo without conversion (little-endian only).
bool isOpaque(const unsigned char* pixels, int width, int height, std::size_t stride) {
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            if (row[static_cast<std::size_t>(x) * 4 + 3] != 255) return false;
        }
    }
    return true;
}

void showRuns(cairo_t* cr, const text::ShapedLine& line, double originX, double baseline,
              double size) {
    std::vector<cairo_glyph_t> glyphs;
    for (const text::ShapedRun& run : line.runs) {
        if (run.face == nullptr || run.glyphs.empty()) continue;
        glyphs.clear();
        glyphs.reserve(run.glyphs.size());
        for (const text::ShapedGlyph& glyph : run.glyphs) {
            glyphs.push_back(cairo_glyph_t{glyph.index, originX + glyph.x, baseline + glyph.y});
        }
        cairo_set_font_face(cr, run.face);
        cairo_set_font_size(cr, size);
        cairo_show_glyphs(cr, glyphs.data(), static_cast<int>(glyphs.size()));
    }
}

} // namespace

cairo_surface_t* createSurfaceFromStraightBgra(const unsigned char* pixels, int width, int height,
                                               std::size_t stride) {
    if (pixels == nullptr || width <= 0 || height <= 0 ||
        stride < static_cast<std::size_t>(width) * 4) {
        return nullptr;
    }
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char* out = cairo_image_surface_get_data(surface);
    const auto outStride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface));
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels + static_cast<std::size_t>(y) * stride;
        auto* dst = reinterpret_cast<std::uint32_t*>(
            static_cast<void*>(out + static_cast<std::size_t>(y) * outStride));
        for (int x = 0; x < width; ++x) {
            const std::uint32_t b = src[0];
            const std::uint32_t g = src[1];
            const std::uint32_t r = src[2];
            const std::uint32_t a = src[3];
            src += 4;
            // CAIRO_FORMAT_ARGB32: native-endian 32-bit words, premultiplied.
            const auto premultiply = [a](std::uint32_t c) { return (c * a + 127u) / 255u; };
            dst[x] = (a << 24) | (premultiply(r) << 16) | (premultiply(g) << 8) | premultiply(b);
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

LinuxPaintContext::LinuxPaintContext(cairo_t* cr, double backingScale)
    : cr_(cr), backingScale_(backingScale > 0.0 ? backingScale : 1.0) {}

void LinuxPaintContext::pushClip(const core::Rect& logicalRect) {
    // Clip in the CURRENT space, then move the origin to the rect's origin
    // (the same order as the CoreGraphics implementation).
    cairo_save(cr_);
    addRect(cr_, logicalRect);
    cairo_clip(cr_);
    cairo_translate(cr_, logicalRect.minX(), logicalRect.minY());
}

void LinuxPaintContext::popClip() {
    cairo_restore(cr_);
}

void LinuxPaintContext::fillRect(const core::Rect& rect, const ui::Color& color) {
    if (rect.isEmpty()) return;
    setColor(cr_, color);
    addRect(cr_, rect);
    cairo_fill(cr_);
}

void LinuxPaintContext::fillRoundedRect(const core::Rect& rect, const ui::Color& color,
                                        double cornerRadius) {
    if (rect.isEmpty()) return;
    const double radius =
        std::clamp(cornerRadius, 0.0, std::min(rect.size.width, rect.size.height) / 2.0);
    setColor(cr_, color);
    if (radius <= 0.0) {
        addRect(cr_, rect);
    } else {
        constexpr double kPi = std::numbers::pi;
        const double x0 = rect.minX();
        const double y0 = rect.minY();
        const double x1 = rect.maxX();
        const double y1 = rect.maxY();
        cairo_new_sub_path(cr_);
        cairo_arc(cr_, x1 - radius, y0 + radius, radius, -kPi / 2.0, 0.0);
        cairo_arc(cr_, x1 - radius, y1 - radius, radius, 0.0, kPi / 2.0);
        cairo_arc(cr_, x0 + radius, y1 - radius, radius, kPi / 2.0, kPi);
        cairo_arc(cr_, x0 + radius, y0 + radius, radius, kPi, 1.5 * kPi);
        cairo_close_path(cr_);
    }
    cairo_fill(cr_);
}

void LinuxPaintContext::strokeRect(const core::Rect& rect, const ui::Color& color, double strokeWidth) {
    if (rect.isEmpty()) return;
    setColor(cr_, color);
    cairo_set_line_width(cr_, strokeWidth);
    addRect(cr_, rect);
    cairo_stroke(cr_);
}

void LinuxPaintContext::drawLine(core::Point from, core::Point to, const ui::Color& color,
                                 double strokeWidth) {
    setColor(cr_, color);
    cairo_set_line_width(cr_, strokeWidth);
    cairo_move_to(cr_, from.x, from.y);
    cairo_line_to(cr_, to.x, to.y);
    cairo_stroke(cr_);
}

void LinuxPaintContext::strokeDashedRect(const core::Rect& rect, const ui::Color& color,
                                         double strokeWidth, double dash) {
    if (rect.isEmpty()) return;
    cairo_save(cr_);
    setColor(cr_, color);
    cairo_set_line_width(cr_, strokeWidth);
    if (dash > 0.0) {
        const double lengths[] = {dash, dash};
        cairo_set_dash(cr_, lengths, 2, 0.0);
    }
    addRect(cr_, rect);
    cairo_stroke(cr_);
    cairo_restore(cr_);
}

void LinuxPaintContext::fillPath(const ui::Path& path, const ui::Color& color) {
    if (path.empty()) return;
    setColor(cr_, color);
    cairo_new_path(cr_);
    addPath(cr_, path);
    cairo_set_fill_rule(cr_, CAIRO_FILL_RULE_WINDING);
    cairo_fill(cr_);
}

void LinuxPaintContext::strokePath(const ui::Path& path, const ui::Color& color, double strokeWidth,
                                   ui::LineCap cap, ui::LineJoin join) {
    if (path.empty()) return;
    cairo_save(cr_);
    setColor(cr_, color);
    cairo_set_line_width(cr_, strokeWidth);
    cairo_set_line_cap(cr_, cap == ui::LineCap::Round ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr_, join == ui::LineJoin::Round ? CAIRO_LINE_JOIN_ROUND : CAIRO_LINE_JOIN_MITER);
    cairo_new_path(cr_);
    addPath(cr_, path);
    cairo_stroke(cr_);
    cairo_restore(cr_);
}

void LinuxPaintContext::drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) {
    if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0) return;
    if (destLogicalRect.isEmpty()) return;
    if (bitmap.width() > 32767 || bitmap.height() > 32767) return; // cairo's surface limit

    const auto width = static_cast<int>(bitmap.width());
    const auto height = static_cast<int>(bitmap.height());
    const auto* pixels = reinterpret_cast<const unsigned char*>(bitmap.data());
    const std::size_t stride = bitmap.stride();

    cairo_surface_t* surface = nullptr;
    if constexpr (std::endian::native == std::endian::little) {
        // Fast path: opaque tiles (pages are filled white) are already valid
        // premultiplied ARGB32 words; cairo only reads the borrowed rows.
        if (stride % 4 == 0 && stride <= static_cast<std::size_t>(INT32_MAX) &&
            isOpaque(pixels, width, height, stride)) {
            surface = cairo_image_surface_create_for_data(const_cast<unsigned char*>(pixels),
                                                          CAIRO_FORMAT_ARGB32, width, height,
                                                          static_cast<int>(stride));
            if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(surface);
                surface = nullptr;
            }
        }
    }
    if (surface == nullptr) surface = createSurfaceFromStraightBgra(pixels, width, height, stride);
    if (surface == nullptr) return;

    cairo_save(cr_);
    // Aliased clip: adjacent page tiles (rects snapped to device pixels by
    // the viewport) must not blend with what lies below at their shared
    // edge, or thin seams show through solid fills (QA #18).
    cairo_set_antialias(cr_, CAIRO_ANTIALIAS_NONE);
    addRect(cr_, destLogicalRect);
    cairo_clip(cr_);
    cairo_translate(cr_, destLogicalRect.minX(), destLogicalRect.minY());
    cairo_scale(cr_, destLogicalRect.size.width / bitmap.width(),
                destLogicalRect.size.height / bitmap.height());
    cairo_set_source_surface(cr_, surface, 0.0, 0.0);
    cairo_pattern_t* pattern = cairo_get_source(cr_);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
    cairo_paint(cr_);
    cairo_restore(cr_);
    // Drops cairo's reference before the borrowed rows can go away.
    cairo_surface_finish(surface);
    cairo_surface_destroy(surface);
}

core::Size LinuxPaintContext::measureText(std::string_view text, const ui::Font& font) const {
    const text::LineMetrics metrics = text::measureLine(text, font);
    return core::Size{metrics.width, metrics.ascent + metrics.descent};
}

void LinuxPaintContext::drawText(std::string_view text, const core::Rect& rect, const ui::Font& font,
                                 const ui::Color& color, ui::TextAlign align) {
    if (text.empty() || rect.isEmpty()) return;
    const auto line = text::shapeLine(text, font);

    double x = rect.minX();
    switch (align) {
    case ui::TextAlign::Left: break;
    case ui::TextAlign::Center: x = rect.minX() + (rect.size.width - line->width) / 2.0; break;
    case ui::TextAlign::Right: x = rect.maxX() - line->width; break;
    }
    // Same vertical centering as the CoreText path.
    const double lineBox = line->ascent + line->descent;
    const double baseline = rect.minY() + (rect.size.height - lineBox) / 2.0 + line->ascent;

    cairo_save(cr_);
    setColor(cr_, color);
    showRuns(cr_, *line, x, baseline, font.size);
    cairo_restore(cr_);
}

void LinuxPaintContext::drawTextInBox(std::string_view text, const core::Rect& box, int quarterTurns,
                                      const ui::Font& font, const ui::Color& color) {
    if (text.empty() || box.isEmpty()) return;

    ui::Font boldFont = font;
    boldFont.weight = ui::Font::Weight::Bold;
    const text::LineMetrics metrics = text::measureLine(text, boldFont);
    const double lineHeight = metrics.ascent + metrics.descent;
    if (metrics.width <= 0.0 || lineHeight <= 0.0) return;

    const int turns = ((quarterTurns % 4) + 4) % 4;
    const bool swapped = (turns % 2) != 0;
    const double availWidth = (swapped ? box.size.height : box.size.width) * 0.9;
    const double availHeight = (swapped ? box.size.width : box.size.height) * 0.9;
    const double scale = std::fmin(availWidth / metrics.width, availHeight / lineHeight);
    if (!(scale > 0.0)) return;

    const core::Point center = box.center();
    cairo_save(cr_);
    cairo_translate(cr_, center.x, center.y);
    // y-down space: a positive angle turns clockwise on screen.
    cairo_rotate(cr_, static_cast<double>(turns) * (std::numbers::pi / 2.0));
    cairo_scale(cr_, scale, scale);
    drawText(text,
             core::Rect{core::Point{-metrics.width / 2.0, -lineHeight / 2.0},
                        core::Size{metrics.width, lineHeight}},
             boldFont, color, ui::TextAlign::Left);
    cairo_restore(cr_);
}

} // namespace rivet::platform
