// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownPdfExport.hpp"

#include "app/MarkdownGraphics.hpp"
#include "app/UrlPolicy.hpp"
#include "markdown/MarkdownPaginate.hpp"
#include "markdown/MarkdownParser.hpp"
#include "markdown/Utf8.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFonts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rivet::app {

namespace {

using markdown::DecorationKind;
using markdown::TextKind;

// Bytes of BGRA pixels one export may embed in total (each appearance of an
// image is a copy); beyond it images are drawn as placeholders. Bounds the
// memory a document repeating a large image many times can demand.
constexpr std::uint64_t kMaxExportImageBytes = 512ull * 1024 * 1024;

// Colors (grey levels / RGB), mirroring the preview's palette in print form.
constexpr pdf::PdfColor kBlack{0.0F, 0.0F, 0.0F};
constexpr pdf::PdfColor kCodeText{0.2F, 0.2F, 0.2F};
constexpr pdf::PdfColor kQuoteText{0.4F, 0.4F, 0.4F};
constexpr pdf::PdfColor kLinkText{0.1F, 0.3F, 0.75F};
constexpr pdf::PdfColor kPlaceholderText{0.5F, 0.5F, 0.5F};
constexpr pdf::PdfColor kCodeBackground{0.95F, 0.95F, 0.95F};
constexpr pdf::PdfColor kQuoteBar{0.8F, 0.8F, 0.8F};
constexpr pdf::PdfColor kRule{0.8F, 0.8F, 0.8F};
constexpr pdf::PdfColor kCheckbox{0.45F, 0.45F, 0.45F};
constexpr pdf::PdfColor kPlaceholderBorder{0.7F, 0.7F, 0.7F};
constexpr pdf::PdfColor kTableGrid{0.75F, 0.75F, 0.75F};
constexpr pdf::PdfColor kTableHeader{0.93F, 0.93F, 0.93F};

bool isHeading(TextKind kind) { return kind >= TextKind::H1 && kind <= TextKind::H6; }

pdf::PdfColor colorFor(const markdown::TextStyle& style) {
    switch (style.kind) {
    case TextKind::Code: return kCodeText;
    case TextKind::Quote: return kQuoteText;
    case TextKind::Link: return kLinkText;
    default: return kBlack;
    }
}

// A link URI the build accepts (pdf::validatePdfBuild's rules, plus the
// application's scheme allow-list): http(s) with an authority or mailto
// with an address, printable ASCII only, bounded length.
bool exportableUri(std::string_view uri) {
    if (!isAllowedExternalUrlScheme(uri) || uri.size() > pdf::kMaxBuildUriBytes) return false;
    for (const char c : uri) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7F) return false;
    }
    if (uri.rfind("mailto:", 0) == 0) return uri.size() > 7;
    const std::size_t rest = uri.find("//") + 2;
    return uri.size() > rest;
}

// Replaces what the font cannot show; counts replaced characters.
std::string exportText(std::string_view utf8, pdf::PdfBundledFont font, std::size_t& replaced) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const markdown::utf8::Decoded d = markdown::utf8::decode(utf8, i);
        const char32_t written = d.valid ? exportCodepoint(font, d.codePoint) : U'?';
        if (written != d.codePoint || !d.valid) ++replaced;
        markdown::utf8::append(out, written);
        i += d.length;
    }
    return out;
}

// IImageSizeProvider over the snapshot: Known only with pixels.
class SnapshotImageSizes final : public markdown::IImageSizeProvider {
public:
    explicit SnapshotImageSizes(const ExportImageMap& images) : images_(images) {}
    markdown::ImageInfo imageInfo(std::string_view url) const override {
        const auto it = images_.find(std::string(url));
        if (it == images_.end()) return {};
        markdown::ImageInfo info = it->second.info;
        if (info.state == markdown::ImageState::Known && it->second.bitmap == nullptr) info = {};
        return info;
    }

private:
    const ExportImageMap& images_;
};

class Emitter {
public:
    Emitter(const markdown::MarkdownLayout& layout, const MarkdownExportOptions& options,
            const ExportImageMap& images, MarkdownExportResult& result)
        : layout_(layout), options_(options), images_(images), result_(result),
          contentWidth_(options.pageWidth - 2.0 * options.margin) {}

    void emitPage(const markdown::PageSlice& slice) {
        page_ = &result_.document.pages.emplace_back();
        page_->width = options_.pageWidth;
        page_->height = options_.pageHeight;
        slice_ = slice;

        // Backgrounds and markers first (painted under the text), in layout order.
        for (const std::size_t index : layout_.decorationsInRange(slice.top, slice.bottom)) {
            emitDecoration(layout_.decorations[index]);
        }
        const auto [b0, b1] = layout_.blockRangeForY(slice.top, slice.bottom);
        for (std::size_t b = b0; b < b1; ++b) emitBlock(layout_.blocks[b]);
    }

private:
    // --- geometry ------------------------------------------------------------

    double pdfX(double x) const { return options_.margin + x; }
    double pdfY(double y) const { return options_.pageHeight - options_.margin - (y - slice_.top); }

    // Whether layout y `top` belongs to this slice (an atom is placed on the
    // page holding its top edge).
    bool ownsTop(double top) const { return top >= slice_.top && top < slice_.bottom; }

    // The layout rect clipped to the slice and the content box, in page
    // space; nullopt when nothing of it is visible.
    std::optional<pdf::PdfBox> clippedBox(double x0, double y0, double x1, double y1) const {
        const double left = std::max(x0, 0.0);
        const double right = std::min(x1, contentWidth_);
        const double top = std::max(y0, slice_.top);
        const double bottom = std::min(y1, slice_.bottom);
        if (!(right > left) || !(bottom > top)) return std::nullopt;
        pdf::PdfBox box{pdfX(left), pdfY(bottom), pdfX(right), pdfY(top)};
        if (!box.isValid()) return std::nullopt;
        return box;
    }
    std::optional<pdf::PdfBox> clippedBox(const core::Rect& r) const {
        return clippedBox(r.minX(), r.minY(), r.maxX(), r.maxY());
    }

    bool full() const { return page_->items.size() >= pdf::kMaxBuildItemsPerPage; }

    void fill(const std::optional<pdf::PdfBox>& box, pdf::PdfColor color) {
        if (!box.has_value() || full()) return;
        pdf::PdfBuildRect rect;
        rect.box = *box;
        rect.fill = color;
        page_->items.emplace_back(std::move(rect));
    }

    void stroke(const std::optional<pdf::PdfBox>& box, pdf::PdfColor color, float width,
                std::optional<pdf::PdfColor> fillColor = std::nullopt) {
        if (!box.has_value() || full()) return;
        pdf::PdfBuildRect rect;
        rect.box = *box;
        rect.stroke = color;
        rect.strokeWidth = width;
        rect.fill = fillColor;
        page_->items.emplace_back(std::move(rect));
    }

    // One line of text at layout (x, baseline). Runs longer than a build
    // text item allows are split at code point boundaries. A piece whose
    // origin falls outside the page is dropped: it would lie in a part of a
    // straddling (taller than a page) line or of an overflowing code line
    // that no page shows, and the build rejects origins outside the page.
    void text(std::string_view utf8, const markdown::TextStyle& style, double x, double baseline,
              pdf::PdfColor color) {
        textInFont(utf8, exportFontFor(style), style.size, x, baseline, color);
    }

    void textInFont(std::string_view utf8, pdf::PdfBundledFont font, double size, double x, double baseline,
                    pdf::PdfColor color) {
        // Script glyphs of a formula scaled far down can drop below the
        // smallest size a build accepts: invisible anyway, not written.
        if (utf8.empty() || !(size >= pdf::kMinFontSize) || size > pdf::kMaxFontSize) return;
        markdown::TextStyle style; // only the size is used below
        style.size = size;
        const std::string written = exportText(utf8, font, result_.replacedCharacters);
        const pdf::BundledFontMetrics& metrics = pdf::bundledFontMetrics(font);
        const double y = pdfY(baseline);
        std::size_t start = 0;
        double offset = 0.0;
        while (start < written.size()) {
            std::size_t end = std::min(written.size(), start + pdf::kMaxBuildTextBytes);
            end = end < written.size() ? markdown::utf8::snapToBoundary(written, end) : end;
            if (end <= start) break;
            const std::string_view piece = std::string_view(written).substr(start, end - start);
            const double px = pdfX(x + offset);
            const bool onPage = px >= 0.0 && px <= options_.pageWidth && y >= 0.0 && y <= options_.pageHeight;
            if (onPage && !full()) {
                pdf::PdfBuildText item;
                item.font = font;
                item.fontSize = style.size;
                item.color = color;
                item.origin = pdf::PdfPoint{px, y};
                item.utf8 = std::string(piece);
                page_->items.emplace_back(std::move(item));
            }
            offset += metrics.textWidth(piece, style.size);
            start = end;
        }
    }

    // --- decorations -----------------------------------------------------------

    void emitDecoration(const markdown::Decoration& d) {
        const core::Rect& r = d.rect;
        switch (d.kind) {
        case DecorationKind::CodeBackground: fill(clippedBox(r), kCodeBackground); break;
        case DecorationKind::QuoteBar: fill(clippedBox(r), kQuoteBar); break;
        case DecorationKind::Rule:
            if (ownsTop(r.minY())) fill(clippedBox(r), kRule);
            break;
        case DecorationKind::ListBullet: {
            // A filled square (there is no circle primitive yet).
            if (!ownsTop(r.minY())) break;
            const double side = std::min(r.size.width, r.size.height);
            const core::Point c = r.center();
            fill(clippedBox(c.x - side / 2.0, c.y - side / 2.0, c.x + side / 2.0, c.y + side / 2.0), kBlack);
            break;
        }
        case DecorationKind::ListNumber:
            if (ownsTop(r.minY())) text(d.text, d.style, r.minX(), d.baseline, kBlack);
            break;
        case DecorationKind::TaskCheckbox: {
            if (!ownsTop(r.minY())) break;
            stroke(clippedBox(r), kCheckbox, 1.0F);
            if (d.checked) {
                const double inset = std::min(r.size.width, r.size.height) * 0.25;
                fill(clippedBox(r.minX() + inset, r.minY() + inset, r.maxX() - inset, r.maxY() - inset), kBlack);
            }
            break;
        }
        case DecorationKind::ImagePlaceholder:
            if (ownsTop(r.minY())) emitImage(d);
            break;
        case DecorationKind::Notice:
            if (ownsTop(r.minY())) text(d.text, d.style, r.minX(), d.baseline, graphicColor(GraphicRole::Notice));
            break;
        }
    }

    // --- diagrams and formulas ------------------------------------------------

    static pdf::PdfColor graphicColor(GraphicRole role) {
        return toPdfColor(markdownGraphicColor(MarkdownPreviewPalette{}, role));
    }

    // A path in layout coordinates (already scaled); dropped when any point
    // falls outside the page (the layout keeps graphics inside the content
    // box, so this only guards against arithmetic slack).
    void path(const std::vector<markdown::mermaid::PathSegment>& segments, const markdown::LayoutGraphic& g,
              std::optional<pdf::PdfColor> fillColor, std::optional<pdf::PdfColor> strokeColor, double width,
              markdown::mermaid::DashStyle dash, bool roundJoins) {
        if (segments.empty() || full() || (!fillColor && !strokeColor)) return;
        if (segments.size() > pdf::kMaxBuildPathSegments) return;
        pdf::PdfBuildPath item;
        item.fill = fillColor;
        item.stroke = strokeColor;
        item.strokeWidth = static_cast<float>(std::clamp(width * g.scale, 0.05, pdf::kMaxBuildStrokeWidth));
        item.roundJoins = roundJoins;
        for (const double d : diagramDashPattern(dash, width)) {
            item.dash.push_back(static_cast<float>(std::clamp(d * g.scale, 0.05, pdf::kMaxBuildDashLength)));
        }
        const pdf::PdfBox pageBox{0.0, 0.0, options_.pageWidth, options_.pageHeight};
        bool inside = true;
        const auto map = [&](core::Point p) {
            const pdf::PdfPoint q{pdfX(g.origin.x + p.x * g.scale), pdfY(g.origin.y + p.y * g.scale)};
            inside = inside && std::isfinite(q.x) && std::isfinite(q.y) && q.x >= pageBox.left &&
                     q.x <= pageBox.right && q.y >= pageBox.bottom && q.y <= pageBox.top;
            return q;
        };
        for (const markdown::mermaid::PathSegment& s : segments) {
            pdf::PdfPathSegment out;
            switch (s.kind) {
            case markdown::mermaid::PathSegment::Kind::MoveTo: out.op = pdf::PdfPathSegment::Op::MoveTo; break;
            case markdown::mermaid::PathSegment::Kind::LineTo: out.op = pdf::PdfPathSegment::Op::LineTo; break;
            case markdown::mermaid::PathSegment::Kind::CubicTo: out.op = pdf::PdfPathSegment::Op::CubicTo; break;
            case markdown::mermaid::PathSegment::Kind::Close: out.op = pdf::PdfPathSegment::Op::Close; break;
            }
            if (out.op != pdf::PdfPathSegment::Op::Close) out.p = map(s.p);
            if (out.op == pdf::PdfPathSegment::Op::CubicTo) {
                out.c1 = map(s.c1);
                out.c2 = map(s.c2);
            }
            item.segments.push_back(out);
        }
        if (!inside || item.segments.front().op != pdf::PdfPathSegment::Op::MoveTo) return;
        page_->items.emplace_back(std::move(item));
    }

    // A diagram or formula; the caller decides the page (atoms go on the page
    // holding their top). `mathColor`: normal formula glyphs.
    void emitGraphic(const markdown::LayoutGraphic& g, pdf::PdfColor mathColor) {
        namespace mm = markdown::mermaid;
        namespace mt = markdown::math;
        const auto at = [&](core::Point p) {
            return core::Point{g.origin.x + p.x * g.scale, g.origin.y + p.y * g.scale};
        };
        if (g.math != nullptr) {
            const pdf::PdfColor errorColor = graphicColor(GraphicRole::MathError);
            for (const mt::MathGlyphRun& run : g.math->runs) {
                const pdf::PdfColor color = run.color == mt::MathColor::Error ? errorColor : mathColor;
                if (run.font == mt::MathFont::Math) {
                    for (const MathGlyphPiece& piece : mathGlyphPieces(run.utf8, run.size)) {
                        const core::Point o = at(core::Point{run.origin.x + piece.x, run.origin.y});
                        textInFont(piece.utf8, piece.font, run.size * g.scale, o.x, o.y, color);
                    }
                } else {
                    const core::Point o = at(run.origin);
                    text(run.utf8, mathTextStyle(run.font, run.size * g.scale), o.x, o.y, color);
                }
            }
            for (const mt::MathRule& rule : g.math->rules) {
                const core::Point a = at(rule.rect.origin);
                fill(clippedBox(a.x, a.y, a.x + rule.rect.size.width * g.scale, a.y + rule.rect.size.height * g.scale),
                     rule.color == mt::MathColor::Error ? errorColor : mathColor);
            }
            return;
        }
        if (g.diagram == nullptr) return;
        const auto color = [](mm::DiagramColor c) { return graphicColor(graphicRole(c)); };
        for (const mm::DiagramItem& item : g.diagram->items) {
            std::visit(
                [&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, mm::DiagramRect>) {
                        const std::optional<pdf::PdfColor> f =
                            v.filled ? std::optional<pdf::PdfColor>(color(v.fill)) : std::nullopt;
                        const std::optional<pdf::PdfColor> s =
                            v.stroked ? std::optional<pdf::PdfColor>(color(v.stroke)) : std::nullopt;
                        path(diagramRectSegments(v.rect, v.cornerRadius), g, f, s, v.strokeWidth, v.dash, false);
                    } else if constexpr (std::is_same_v<T, mm::DiagramPath>) {
                        const std::optional<pdf::PdfColor> f =
                            v.filled ? std::optional<pdf::PdfColor>(color(v.fill)) : std::nullopt;
                        const std::optional<pdf::PdfColor> s =
                            v.stroked ? std::optional<pdf::PdfColor>(color(v.stroke)) : std::nullopt;
                        path(v.segments, g, f, s, v.strokeWidth, v.dash, false);
                    } else if constexpr (std::is_same_v<T, mm::DiagramPolyline>) {
                        std::vector<mm::PathSegment> line;
                        for (std::size_t i = 0; i < v.points.size(); ++i) {
                            line.push_back(mm::PathSegment{i == 0 ? mm::PathSegment::Kind::MoveTo
                                                                  : mm::PathSegment::Kind::LineTo,
                                                           v.points[i], {}, {}});
                        }
                        if (line.size() >= 2) path(line, g, std::nullopt, color(v.color), v.width, v.dash, true);
                    } else {
                        const core::Point o = at(v.origin);
                        text(v.utf8, mm::diagramTextStyle(v.font, v.size * g.scale), o.x, o.y, color(v.color));
                    }
                },
                item);
        }
    }

    void emitImage(const markdown::Decoration& d) {
        if (d.imageState == markdown::ImageState::Known) {
            const auto it = images_.find(d.url);
            if (it != images_.end() && it->second.bitmap != nullptr) {
                if (embedBitmap(*it->second.bitmap, d.rect)) return;
                ++result_.skippedImages;
            }
        }
        placeholder(d);
    }

    // The bitmap stretched onto the rect's visible part (rows outside the
    // slice are cropped; the layout caps images at the content height, so
    // that only happens for a pathological slice). False when it cannot be
    // embedded (limits, budget, unusable bitmap).
    bool embedBitmap(const core::Bitmap& bitmap, const core::Rect& r) {
        if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0 || r.size.height <= 0.0) return false;
        const std::optional<pdf::PdfBox> box = clippedBox(r);
        if (!box.has_value()) return true; // nothing visible on this page: not a failure
        const double visibleTop = std::max(r.minY(), slice_.top);
        const double visibleBottom = std::min(r.maxY(), slice_.bottom);
        const double rowsPerUnit = static_cast<double>(bitmap.height()) / r.size.height;
        auto row0 = static_cast<std::uint32_t>(std::clamp(std::floor((visibleTop - r.minY()) * rowsPerUnit), 0.0,
                                                          static_cast<double>(bitmap.height() - 1)));
        auto row1 = static_cast<std::uint32_t>(std::clamp(std::ceil((visibleBottom - r.minY()) * rowsPerUnit),
                                                          static_cast<double>(row0 + 1),
                                                          static_cast<double>(bitmap.height())));
        pdf::PdfImageData image;
        image.format = pdf::PdfImageData::Format::Bgra;
        image.width = bitmap.width();
        image.height = row1 - row0;
        image.stride = image.width * 4u;
        const std::uint64_t bytes = std::uint64_t{image.stride} * image.height;
        // Check the limits before copying anything.
        if (image.width > pdf::kMaxImageSide || image.height > pdf::kMaxImageSide ||
            std::uint64_t{image.width} * image.height > pdf::kMaxImagePixels ||
            imageBytes_ + bytes > kMaxExportImageBytes || bitmap.stride() < image.stride) {
            return false;
        }
        image.bytes.resize(static_cast<std::size_t>(bytes));
        const auto* src = reinterpret_cast<const std::uint8_t*>(bitmap.data());
        for (std::uint32_t y = 0; y < image.height; ++y) {
            std::memcpy(image.bytes.data() + std::size_t{y} * image.stride,
                        src + std::size_t{row0 + y} * bitmap.stride(), image.stride);
        }
        if (!pdf::validateImageData(image).has_value() || full()) return false;
        imageBytes_ += bytes;
        page_->items.emplace_back(pdf::PdfBuildImage{std::move(image), *box});
        return true;
    }

    void placeholder(const markdown::Decoration& d) {
        const core::Rect& r = d.rect;
        stroke(clippedBox(r), kPlaceholderBorder, 1.0F);
        const std::string_view alt = d.text.empty() ? std::string_view("image") : std::string_view(d.text);
        markdown::TextStyle style;
        style.size = options_.typography.bodySize;
        const double pad = 6.0;
        const pdf::PdfBundledFont font = exportFontFor(style);
        std::size_t ignored = 0;
        const double width = pdf::bundledFontMetrics(font).textWidth(exportText(alt, font, ignored), style.size);
        // Only when it fits entirely; the alt text is a hint, not content.
        if (width > r.size.width - 2.0 * pad || r.size.height < style.size * 1.4) return;
        if (r.maxY() > slice_.bottom) return;
        text(alt, style, r.minX() + pad, r.minY() + r.size.height / 2.0 + style.size * 0.35, kPlaceholderText);
    }

    // --- blocks ------------------------------------------------------------------

    void emitBlock(const markdown::LayoutBlock& block) {
        if (block.kind == markdown::LayoutBlockKind::Graphic) {
            if (block.graphic == 0 || block.graphic > layout_.graphics.size()) return;
            const markdown::LayoutGraphic& g = layout_.graphics[block.graphic - 1];
            if (ownsTop(g.bounds.minY())) emitGraphic(g, kBlack);
            return;
        }
        if (block.kind == markdown::LayoutBlockKind::Table) {
            for (std::uint32_t c = block.cellBegin; c < block.cellEnd; ++c) {
                const markdown::TableCellLayout& cell = layout_.tableCells[c];
                if (cell.rect.maxY() <= slice_.top || cell.rect.minY() >= slice_.bottom) continue;
                stroke(clippedBox(cell.rect), kTableGrid, 0.5F,
                       cell.header ? std::optional<pdf::PdfColor>(kTableHeader) : std::nullopt);
            }
            for (std::uint32_t c = block.cellBegin; c < block.cellEnd; ++c) {
                const markdown::TableCellLayout& cell = layout_.tableCells[c];
                emitLines(block, cell.lineBegin, cell.lineEnd);
            }
            return;
        }
        emitLines(block, block.lineBegin, block.lineEnd);
    }

    void emitLines(const markdown::LayoutBlock& block, std::uint32_t begin, std::uint32_t end) {
        const bool codeBlock =
            block.kind == markdown::LayoutBlockKind::Code || block.kind == markdown::LayoutBlockKind::Html;
        for (std::uint32_t l = begin; l < end; ++l) {
            const markdown::LayoutLine& line = layout_.lines[l];
            if (!ownsTop(line.top)) continue;
            for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) emitRun(layout_.runs[r], codeBlock);
        }
    }

    void emitRun(const markdown::LayoutRun& run, bool codeBlock) {
        if (run.text.empty()) return;
        const double size = run.style.size;
        if (run.graphic != 0) { // an inline formula
            if (run.graphic <= layout_.graphics.size()) emitGraphic(layout_.graphics[run.graphic - 1], colorFor(run.style));
            return;
        }
        if (run.isImage) {
            // An inline image is shown as its alt text, like the preview.
            text(run.text, run.style, run.x, run.baseline, kPlaceholderText);
            return;
        }
        const pdf::BundledFontMetrics& metrics = pdf::bundledFontMetrics(exportFontFor(run.style));
        const double ascent = metrics.ascent() / metrics.unitsPerEm() * size;
        const double descent = -metrics.descent() / metrics.unitsPerEm() * size;
        if (run.style.monospace && !codeBlock) {
            fill(clippedBox(run.x - 1.0, run.baseline - ascent, run.x + run.width + 1.0, run.baseline + descent),
                 kCodeBackground);
        }
        const pdf::PdfColor color = colorFor(run.style);
        text(run.text, run.style, run.x, run.baseline, color);
        if (run.style.strike) {
            const double h = 0.06 * size;
            const double y = run.baseline - 0.3 * size;
            fill(clippedBox(run.x, y - h / 2.0, run.x + run.width, y + h / 2.0), color);
        }
        if (run.link != 0 && run.link <= layout_.links.size()) {
            const double top = run.baseline + 0.1 * size;
            fill(clippedBox(run.x, top, run.x + run.width, top + 0.05 * size), color);
            const std::string& url = layout_.links[run.link - 1].url;
            if (exportableUri(url) && page_->links.size() < pdf::kMaxBuildLinksPerPage) {
                if (const std::optional<pdf::PdfBox> box =
                        clippedBox(run.x, run.baseline - ascent, run.x + run.width, run.baseline + descent)) {
                    page_->links.push_back(pdf::PdfBuildLink{*box, url});
                }
            }
        }
    }

    const markdown::MarkdownLayout& layout_;
    const MarkdownExportOptions& options_;
    const ExportImageMap& images_;
    MarkdownExportResult& result_;
    double contentWidth_;
    pdf::PdfBuildPage* page_ = nullptr;
    markdown::PageSlice slice_;
    std::uint64_t imageBytes_ = 0;
};

core::Error exportError(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), "app");
}

} // namespace

pdf::PdfBundledFont exportFontFor(const markdown::TextStyle& style) {
    const bool bold = style.bold || isHeading(style.kind);
    if (style.monospace || style.kind == TextKind::Code) {
        return bold ? pdf::PdfBundledFont::MonoBold : pdf::PdfBundledFont::MonoRegular;
    }
    return bold ? pdf::PdfBundledFont::SansBold : pdf::PdfBundledFont::SansRegular;
}

char32_t exportCodepoint(pdf::PdfBundledFont font, char32_t cp) {
    const bool control = cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
    return !control && pdf::bundledFontCovers(font, cp) ? cp : U'?';
}

markdown::TextMetrics BundledFontMeasurer::measure(std::string_view utf8, const markdown::TextStyle& style) const {
    const pdf::PdfBundledFont font = exportFontFor(style);
    const pdf::BundledFontMetrics& metrics = pdf::bundledFontMetrics(font);
    markdown::TextMetrics m;
    double thousandths = 0.0;
    for (std::size_t i = 0; i < utf8.size();) {
        const markdown::utf8::Decoded d = markdown::utf8::decode(utf8, i);
        thousandths += metrics.pdfWidth(d.valid ? exportCodepoint(font, d.codePoint) : U'?');
        i += d.length;
    }
    // Same arithmetic as BundledFontMetrics::textWidth (sum, then scale).
    m.width = thousandths * style.size / 1000.0;
    m.ascent = metrics.ascent() / metrics.unitsPerEm() * style.size;
    m.descent = -metrics.descent() / metrics.unitsPerEm() * style.size;
    return m;
}

markdown::Typography exportTypography() {
    markdown::Typography t;
    t.bodySize = 11.0;
    t.codeSize = 9.5;
    t.tableSize = 10.0;
    t.headingSize = {22.0, 18.0, 15.0, 13.0, 12.0, 11.0};
    t.pageMarginX = 0.0;
    t.pageMarginY = 0.0;
    // Spacing scaled with the smaller body size.
    t.paragraphGap = 9.0;
    t.bulletSize = 4.0;
    t.checkboxSize = 10.0;
    t.listIndent = 22.0;
    t.codePadding = 8.0;
    t.tableCellPaddingX = 6.0;
    t.tableCellPaddingY = 3.0;
    return t;
}

core::Result<MarkdownExportResult> buildMarkdownPdf(std::string_view source, const MarkdownExportOptions& options,
                                                    const ExportImageMap& images) {
    const auto finitePositive = [](double v) { return std::isfinite(v) && v > 0.0; };
    if (!finitePositive(options.pageWidth) || !finitePositive(options.pageHeight) || !std::isfinite(options.margin) ||
        options.margin < 0.0 || options.pageWidth < pdf::kMinBuildPageSize ||
        options.pageHeight < pdf::kMinBuildPageSize || options.pageWidth > pdf::kMaxBuildPageSize ||
        options.pageHeight > pdf::kMaxBuildPageSize) {
        return std::unexpected(exportError(core::ErrorCode::InvalidArgument, "invalid export page size"));
    }
    const double contentWidth = options.pageWidth - 2.0 * options.margin;
    const double contentHeight = options.pageHeight - 2.0 * options.margin;
    if (contentWidth < 36.0 || contentHeight < 36.0) {
        return std::unexpected(exportError(core::ErrorCode::InvalidArgument, "export margins leave no room"));
    }

    MarkdownExportOptions effective = options;
    effective.typography.pageMarginX = 0.0;
    effective.typography.pageMarginY = 0.0;
    effective.typography.maxImageHeight = std::min(effective.typography.maxImageHeight, contentHeight);
    if (!(effective.typography.maxImageHeight > 0.0)) effective.typography.maxImageHeight = contentHeight;
    // A diagram always fits on one page too.
    if (!(effective.typography.maxGraphicHeight > 0.0) || effective.typography.maxGraphicHeight > contentHeight) {
        effective.typography.maxGraphicHeight = contentHeight;
    }

    const markdown::MarkdownDocument document = markdown::makeMarkdownParser()->parse(source);
    const BundledFontMeasurer measurer;
    const BundledMathMeasurer mathMeasurer(measurer);
    const SnapshotImageSizes sizes(images);
    const markdown::MarkdownLayout layout = markdown::layoutMarkdown(document, contentWidth, measurer, &sizes,
                                                                     effective.typography, nullptr, &mathMeasurer);
    const std::vector<markdown::PageSlice> slices = markdown::paginateLayout(layout, contentHeight);
    if (slices.size() > pdf::kMaxBuildPages) {
        return std::unexpected(exportError(core::ErrorCode::Unsupported, "the document is too long to export"));
    }

    MarkdownExportResult result;
    result.document.pages.reserve(slices.size());
    Emitter emitter(layout, effective, images, result);
    for (const markdown::PageSlice& slice : slices) emitter.emitPage(slice);
    result.pageCount = result.document.pages.size();
    if (core::Status valid = pdf::validatePdfBuild(result.document); !valid) {
        // The detail can name a document character: not reported.
        return std::unexpected(exportError(core::ErrorCode::Internal, "export produced an invalid page description"));
    }
    return result;
}

} // namespace rivet::app
