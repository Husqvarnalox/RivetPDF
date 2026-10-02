// SPDX-License-Identifier: MPL-2.0

#include "PdfiumAnnotations.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "PdfiumDisplayTransform.h"
#include "PdfiumStrings.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"

// Reads annotations through the public FPDFAnnot_* API only. The quirks
// this works around (all verified against the pinned PDFium source):
//   - FPDFAnnot_GetColor/SetColor FAIL while the annotation has a normal
//     appearance stream, so the AP is removed (FPDFAnnot_SetAP(null)) right
//     before the colors are read - which is why the reader document is
//     private and why callers cache the result (a second read would find the
//     AP gone). Everything that depends on the AP (border width through /BS,
//     the "has an appearance" test for notes) is read BEFORE removing it.
//   - /BS /W cannot be read through the API; the stroke width of the AP's
//     path objects is the best available proxy.
//   - Names/arrays (/Name, /IT, /Subtype, ...) read fine through
//     FPDFAnnot_GetStringValue, which decodes /Name objects.

namespace rivet::pdf::internal {
namespace {

// Matches the 20x20 box PDFium generates for a /Text annotation without an
// appearance (CPDF_GenerateAP::GenerateTextAP).
constexpr double kPdfiumNoteIconSize = 20.0;
// Hard cap on the AP objects inspected for the border width.
constexpr int kMaxApObjects = 1024;

PdfBox boxFromRect(const FS_RECTF& r) {
    PdfBox box{static_cast<double>(r.left), static_cast<double>(r.bottom), static_cast<double>(r.right),
               static_cast<double>(r.top)};
    if (box.left > box.right) std::swap(box.left, box.right);
    if (box.bottom > box.top) std::swap(box.bottom, box.top);
    return box;
}

// A string key as UTF-8. nullopt = longer than `maxBytes` UTF-8 bytes (a
// UTF-16 unit never encodes to fewer than one UTF-8 byte, so the unit count
// is a safe lower bound that avoids allocating absurd buffers). A missing key
// reads as an empty string (PDFium returns just the terminator).
std::optional<std::string> readString(FPDF_ANNOTATION annot, const char* key, std::size_t maxBytes) {
    const unsigned long needed = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
    if (needed < 2) {
        return std::string{};
    }
    if (needed / 2 - 1 > maxBytes) {
        return std::nullopt;
    }
    std::vector<FPDF_WCHAR> buffer((needed + 1) / 2);
    const unsigned long written = FPDFAnnot_GetStringValue(annot, key, buffer.data(), needed);
    if (written < 2) {
        return std::string{};
    }
    const std::size_t bytes = std::min<std::size_t>(written, needed) & ~std::size_t{1};
    std::string text = utf16leToUtf8(reinterpret_cast<const std::uint8_t*>(buffer.data()), bytes);
    if (text.size() > maxBytes) {
        return std::nullopt;
    }
    return text;
}

std::optional<PdfColor> readColor(FPDF_ANNOTATION annot, FPDFANNOT_COLORTYPE type) {
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    unsigned int a = 0;
    if (FPDFAnnot_GetColor(annot, type, &r, &g, &b, &a) == 0) {
        return std::nullopt;
    }
    return PdfColor{static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F,
                    static_cast<float>(b) / 255.0F};
}

// Largest stroke width among the path objects of the annotation's normal
// appearance; 0 when there is none. Loads the AP's form (read-only).
float maxApStrokeWidth(FPDF_ANNOTATION annot) {
    const int count = std::min(FPDFAnnot_GetObjectCount(annot), kMaxApObjects);
    float widest = 0.0F;
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT object = FPDFAnnot_GetObject(annot, i);
        if (object == nullptr || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) {
            continue;
        }
        float width = 0.0F;
        if (FPDFPageObj_GetStrokeWidth(object, &width) != 0 && std::isfinite(width)) {
            widest = std::max(widest, width);
        }
    }
    return widest;
}

std::optional<PdfAnnotationKind> kindOf(FPDF_ANNOTATION_SUBTYPE subtype) {
    switch (subtype) {
    case FPDF_ANNOT_HIGHLIGHT:
        return PdfAnnotationKind::Highlight;
    case FPDF_ANNOT_UNDERLINE:
        return PdfAnnotationKind::Underline;
    case FPDF_ANNOT_STRIKEOUT:
        return PdfAnnotationKind::StrikeOut;
    case FPDF_ANNOT_TEXT:
        return PdfAnnotationKind::Note;
    case FPDF_ANNOT_INK:
        return PdfAnnotationKind::Ink;
    case FPDF_ANNOT_SQUARE:
        return PdfAnnotationKind::Square;
    case FPDF_ANNOT_CIRCLE:
        return PdfAnnotationKind::Circle;
    case FPDF_ANNOT_STAMP:
        return PdfAnnotationKind::Stamp;
    default:
        return std::nullopt;
    }
}

bool isMarkup(PdfAnnotationKind kind) {
    return kind == PdfAnnotationKind::Highlight || kind == PdfAnnotationKind::Underline ||
           kind == PdfAnnotationKind::StrikeOut;
}

// Reads the quads of a markup annotation; false = over the limit / unreadable.
bool readQuads(FPDF_ANNOTATION annot, PdfAnnotationData& data) {
    const std::size_t count = FPDFAnnot_CountAttachmentPoints(annot);
    if (count == 0 || count > kMaxQuadsPerAnnotation) {
        return false;
    }
    data.quads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        FS_QUADPOINTSF q{};
        if (FPDFAnnot_GetAttachmentPoints(annot, i, &q) == 0) {
            return false;
        }
        data.quads.push_back(PdfQuad{{static_cast<double>(q.x1), static_cast<double>(q.y1)},
                                     {static_cast<double>(q.x2), static_cast<double>(q.y2)},
                                     {static_cast<double>(q.x3), static_cast<double>(q.y3)},
                                     {static_cast<double>(q.x4), static_cast<double>(q.y4)}});
    }
    return true;
}

// Reads the ink strokes; false = over the limits / unreadable.
bool readInkStrokes(FPDF_ANNOTATION annot, std::vector<std::vector<PdfPoint>>& strokes) {
    const unsigned long count = FPDFAnnot_GetInkListCount(annot);
    if (count == 0 || count > kMaxInkStrokes) {
        return false;
    }
    std::size_t total = 0;
    strokes.reserve(count);
    for (unsigned long i = 0; i < count; ++i) {
        const unsigned long points = FPDFAnnot_GetInkListPath(annot, i, nullptr, 0);
        if (points == 0 || points > kMaxInkPointsPerStroke) {
            return false;
        }
        total += points;
        if (total > kMaxInkPointsTotal) {
            return false;
        }
        std::vector<FS_POINTF> buffer(points);
        if (FPDFAnnot_GetInkListPath(annot, i, buffer.data(), points) != points) {
            return false;
        }
        std::vector<PdfPoint> stroke;
        stroke.reserve(points);
        for (const FS_POINTF& p : buffer) {
            stroke.push_back(PdfPoint{static_cast<double>(p.x), static_cast<double>(p.y)});
        }
        strokes.push_back(std::move(stroke));
    }
    return true;
}

std::optional<int> parseRotation(const std::string& text) {
    if (text == "0") return 0;
    if (text == "90") return 90;
    if (text == "180") return 180;
    if (text == "270") return 270;
    return std::nullopt;
}

// Fills `data` (kind already set, rect = stored rect) for an editable kind.
// False = not faithfully readable or over a limit: the caller degrades the
// entry to opaque.
bool readEditable(FPDF_ANNOTATION annot, PdfAnnotationData& data) {
    const PdfAnnotationKind kind = data.kind;

    // Strings (untrusted; limits enforced here and again by isWritable).
    const auto contents = readString(annot, "Contents", kMaxContentsBytes);
    const auto author = readString(annot, "T", kMaxAuthorBytes);
    const auto name = readString(annot, "NM", 256);
    if (!contents || !author || !name) {
        return false;
    }
    data.contents = *contents;
    data.author = *author;
    data.name = *name;

    // Geometry.
    if (isMarkup(kind)) {
        if (!readQuads(annot, data)) return false;
    } else if (kind == PdfAnnotationKind::Ink) {
        std::vector<std::vector<PdfPoint>> strokes;
        if (!readInkStrokes(annot, strokes)) return false;
        const auto shape = readString(annot, "RivetShape", 16);
        if (shape && *shape == "Line" && strokes.size() == 1 && strokes[0].size() == 2) {
            data.kind = PdfAnnotationKind::Line;
            data.lineStart = strokes[0][0];
            data.lineEnd = strokes[0][1];
        } else if (shape && *shape == "Arrow" && strokes.size() == 2 && strokes[0].size() == 2 &&
                   strokes[1].size() == 3) {
            data.kind = PdfAnnotationKind::Arrow;
            data.lineStart = strokes[0][0];
            data.lineEnd = strokes[0][1];
        } else {
            data.inkStrokes = std::move(strokes);
        }
    } else if (kind == PdfAnnotationKind::Stamp) {
        // /Name wins when it is a standard name; /Subj is where Rivet stores it.
        std::optional<PdfStampName> stamp;
        if (const auto nameValue = readString(annot, "Name", 64)) {
            stamp = parseStampName(*nameValue);
        }
        if (!stamp) {
            if (const auto subj = readString(annot, "Subj", 64)) {
                stamp = parseStampName(*subj);
            }
        }
        if (!stamp) {
            return false; // custom stamp: opaque
        }
        data.stampName = *stamp;
        if (const auto rotation = readString(annot, "RivetRotation", 8)) {
            data.rotation = parseRotation(*rotation).value_or(0);
        }
    }

    const bool hasAp = FPDFAnnot_HasKey(annot, "AP") != 0;

    // A note without an appearance is drawn by PDFium as a 20x20 icon at the
    // lower-left corner of /Rect.
    if (kind == PdfAnnotationKind::Note && !hasAp) {
        data.rect = PdfBox{data.rect.left, data.rect.bottom, data.rect.left + kPdfiumNoteIconSize,
                           data.rect.bottom + kPdfiumNoteIconSize};
    }

    // Border width (stroked kinds only), before the AP goes away.
    const bool stroked = data.kind == PdfAnnotationKind::Ink || data.kind == PdfAnnotationKind::Line ||
                         data.kind == PdfAnnotationKind::Arrow || kind == PdfAnnotationKind::Square ||
                         kind == PdfAnnotationKind::Circle;
    if (stroked) {
        float width = 0.0F;
        if (hasAp && FPDFAnnot_HasKey(annot, "BS") != 0) {
            width = maxApStrokeWidth(annot);
        }
        if (!(width > 0.0F)) {
            float h = 0.0F;
            float v = 0.0F;
            float w = 0.0F;
            width = FPDFAnnot_GetBorder(annot, &h, &v, &w) != 0 && std::isfinite(w) ? w : 1.0F;
        }
        data.borderWidth = width;
    }

    // Colors: only readable once the appearance is gone.
    if (hasAp) {
        FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
    }
    const auto color = readColor(annot, FPDFANNOT_COLORTYPE_Color);
    if (!color) {
        return false;
    }
    data.color = *color;
    if ((kind == PdfAnnotationKind::Square || kind == PdfAnnotationKind::Circle) &&
        FPDFAnnot_HasKey(annot, "IC") != 0) {
        data.interiorColor = readColor(annot, FPDFANNOT_COLORTYPE_InteriorColor);
    }
    float opacity = 1.0F;
    if (FPDFAnnot_GetNumberValue(annot, "CA", &opacity) == 0 || !std::isfinite(opacity)) {
        opacity = 1.0F;
    }
    data.opacity = opacity;

    normalizeAnnotation(data);
    return isWritableAnnotation(data);
}

} // namespace

core::Result<PdfPageAnnotations> readPageAnnotations(FPDF_DOCUMENT reader, std::size_t pageIndex) {
    ScopedPage page(FPDF_LoadPage(reader, static_cast<int>(pageIndex)));
    if (page.get() == nullptr) {
        const int lastError = static_cast<int>(FPDF_GetLastError());
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium failed to load page " + std::to_string(pageIndex) +
                                                   " (FPDF error " + std::to_string(lastError) + ")",
                                               "pdf"));
    }

    PdfPageAnnotations result;
    const int count = std::max(0, FPDFPage_GetAnnotCount(page.get()));
    result.annotsCount = static_cast<std::uint32_t>(count);
    const int detailed = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(count), kMaxAnnotationsPerPage));
    result.items.reserve(static_cast<std::size_t>(detailed));

    for (int i = 0; i < detailed; ++i) {
        PdfPageAnnotation item;
        item.index = static_cast<std::uint32_t>(i);

        ScopedAnnot annot(FPDFPage_GetAnnot(page.get(), i));
        if (annot.get() == nullptr) {
            result.items.push_back(std::move(item)); // not a dictionary: opaque
            continue;
        }
        item.flags = static_cast<std::uint32_t>(FPDFAnnot_GetFlags(annot.get()));

        FS_RECTF rect{};
        if (FPDFAnnot_GetRect(annot.get(), &rect) != 0) {
            item.data.rect = boxFromRect(rect);
        }
        const PdfBox storedRect = item.data.rect;

        {
            ScopedAnnot popup(FPDFAnnot_GetLinkedAnnot(annot.get(), "Popup"));
            if (popup.get() != nullptr) {
                const int popupIndex = FPDFPage_GetAnnotIndex(page.get(), popup.get());
                if (popupIndex >= 0) {
                    item.popupIndex = static_cast<std::uint32_t>(popupIndex);
                }
            }
        }

        const FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot.get());
        item.isPopup = subtype == FPDF_ANNOT_POPUP;
        const std::optional<PdfAnnotationKind> kind = kindOf(subtype);
        if (kind) {
            item.data.kind = *kind;
            if (readEditable(annot.get(), item.data)) {
                item.kind = item.data.kind;
                item.editable = true;
            } else {
                item.data = PdfAnnotationData{};
                item.data.rect = storedRect;
            }
        }
        result.items.push_back(std::move(item));
    }
    return result;
}

} // namespace rivet::pdf::internal
