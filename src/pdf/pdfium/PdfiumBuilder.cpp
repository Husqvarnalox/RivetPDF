// SPDX-License-Identifier: MPL-2.0

#include "PdfiumBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "PdfiumCallGate.hpp"
#include "PdfiumContent.h"          // internal::FontCache
#include "PdfiumDisplayTransform.h" // internal::ScopedPage
#include "PdfiumStrings.h"
#include "PdfiumWriting.h"
#include "fpdf_annot.h" // FPDFPage_CreateAnnot, FPDFAnnot_SetRect, FPDFAnnot_SetURI
#include "fpdf_edit.h"  // FPDF_CreateNewDocument, FPDFPage_New, page objects
#include "fpdfview.h"

// Document builder (ADR-0023): materializes a validated PdfBuildDocument
// into a brand-new PDF.
//
//   1. validatePdfBuild - before the gate (pure data, no engine involved).
//   2. Under ONE gate acquisition: FPDF_CreateNewDocument; per page
//      FPDFPage_New, the items in order (later items paint over earlier
//      ones), the link annotations, FPDFPage_GenerateContent, FPDF_ClosePage;
//      then FPDF_SaveAsCopy (FPDF_NO_INCREMENTAL) into the sink through the
//      assembly's sink adapter (PdfiumWriting.h).
//
// Items:
//   - Rect:  FPDFPageObj_CreateNewRect, fill (nonzero winding) and/or stroke
//            colors and width, FPDFPath_SetDrawMode.
//   - Text:  FPDFPageObj_CreateTextObj with the bundled font loaded once per
//            document (internal::FontCache: FPDFText_LoadFont, TrueType,
//            CID-keyed, the whole subset program embedded - exactly what
//            content edits do), FPDFText_SetText in UTF-16, fill color,
//            matrix {1,0,0,1,x,y}: the text object's origin is the baseline
//            start of its first glyph. The written widths come from the
//            font's hmtx, so BundledFontMetrics measures the same advances
//            (no kerning is applied).
//   - Path:  FPDFPageObj_CreateNewPath at the first MoveTo, then
//            FPDFPath_MoveTo / LineTo / BezierTo / Close, fill and stroke
//            like a Rect, FPDFPageObj_SetLineJoin/SetLineCap and
//            FPDFPageObj_SetDashArray for dashed strokes.
//   - Image: FPDFPageObj_NewImageObj; JPEG bytes are embedded inline as
//            /DCTDecode, BGRA pixels through a borrowed FPDF_BITMAP (alpha
//            becomes a soft mask); matrix {w,0,0,h,left,bottom}.
//   - Link:  FPDFPage_CreateAnnot(LINK), FPDFAnnot_SetRect, a zero-width
//            border (no visible frame), FPDFAnnot_SetURI (validated ASCII).
//
// Font lifetime: the FontCache closes its FPDF_FONT handles (FPDFFont_Close)
// after the last page was generated and BEFORE the save, like the assembly's
// content pass; the text objects keep the font alive inside the document,
// so it is written normally.
//
// Error model: InvalidArgument from validation; Internal naming the failed
// step for PDFium failures (never document content); the sink's own error
// for sink failures; bad_alloc -> OutOfMemory. Every handle is RAII-closed
// on every path and exceptions never cross the boundary.

namespace rivet::pdf {
namespace {

using internal::FontCache;
using internal::ScopedDocument;
using internal::ScopedPage;

core::Status fail(std::string message) {
    return std::unexpected(core::makeError(core::ErrorCode::Internal, std::move(message), "pdf"));
}

// A page object not (yet) owned by a page.
class OwnedObject {
public:
    explicit OwnedObject(FPDF_PAGEOBJECT object) : object_(object) {}
    ~OwnedObject() {
        if (object_ != nullptr) {
            FPDFPageObj_Destroy(object_);
        }
    }
    OwnedObject(const OwnedObject&) = delete;
    OwnedObject& operator=(const OwnedObject&) = delete;
    FPDF_PAGEOBJECT get() const { return object_; }
    FPDF_PAGEOBJECT release() { return std::exchange(object_, nullptr); }

private:
    FPDF_PAGEOBJECT object_;
};

class ScopedAnnotation {
public:
    explicit ScopedAnnotation(FPDF_ANNOTATION annotation) : annotation_(annotation) {}
    ~ScopedAnnotation() {
        if (annotation_ != nullptr) {
            FPDFPage_CloseAnnot(annotation_);
        }
    }
    ScopedAnnotation(const ScopedAnnotation&) = delete;
    ScopedAnnotation& operator=(const ScopedAnnotation&) = delete;
    FPDF_ANNOTATION get() const { return annotation_; }

private:
    FPDF_ANNOTATION annotation_;
};

unsigned int toByte(float v) {
    return static_cast<unsigned int>(std::lround(static_cast<double>(std::clamp(v, 0.0F, 1.0F)) * 255.0));
}

std::vector<FPDF_WCHAR> toWide(const std::string& utf8) {
    const std::vector<std::uint8_t> bytes = internal::utf8ToUtf16le(utf8);
    std::vector<FPDF_WCHAR> units(bytes.size() / 2);
    for (std::size_t i = 0; i < units.size(); ++i) {
        units[i] = static_cast<FPDF_WCHAR>(static_cast<unsigned>(bytes[2 * i]) |
                                           (static_cast<unsigned>(bytes[2 * i + 1]) << 8));
    }
    return units;
}

FS_MATRIX matrix(double a, double d, double e, double f) {
    return FS_MATRIX{static_cast<float>(a), 0.0F, 0.0F, static_cast<float>(d), static_cast<float>(e),
                     static_cast<float>(f)};
}

// Every helper below runs inside the caller's gate acquisition.

core::Status insert(FPDF_PAGE page, OwnedObject& object) {
    FPDFPage_InsertObject(page, object.release());
    return core::ok();
}

core::Status addRect(FPDF_PAGE page, const PdfBuildRect& rect) {
    OwnedObject object(FPDFPageObj_CreateNewRect(static_cast<float>(rect.box.left), static_cast<float>(rect.box.bottom),
                                                 static_cast<float>(rect.box.width()),
                                                 static_cast<float>(rect.box.height())));
    if (object.get() == nullptr) return fail("PDFium could not create a rectangle");
    if (rect.fill.has_value() &&
        FPDFPageObj_SetFillColor(object.get(), toByte(rect.fill->r), toByte(rect.fill->g), toByte(rect.fill->b), 255) ==
            0) {
        return fail("PDFium could not set a rectangle's fill color");
    }
    if (rect.stroke.has_value()) {
        if (FPDFPageObj_SetStrokeColor(object.get(), toByte(rect.stroke->r), toByte(rect.stroke->g),
                                       toByte(rect.stroke->b), 255) == 0 ||
            FPDFPageObj_SetStrokeWidth(object.get(), rect.strokeWidth) == 0) {
            return fail("PDFium could not set a rectangle's stroke");
        }
    }
    if (FPDFPath_SetDrawMode(object.get(), rect.fill.has_value() ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE,
                             rect.stroke.has_value() ? 1 : 0) == 0) {
        return fail("PDFium could not set a rectangle's draw mode");
    }
    return insert(page, object);
}

core::Status addPath(FPDF_PAGE page, const PdfBuildPath& path) {
    const PdfPathSegment& first = path.segments.front(); // validated: a MoveTo
    OwnedObject object(FPDFPageObj_CreateNewPath(static_cast<float>(first.p.x), static_cast<float>(first.p.y)));
    if (object.get() == nullptr) return fail("PDFium could not create a path");
    for (std::size_t i = 1; i < path.segments.size(); ++i) {
        const PdfPathSegment& s = path.segments[i];
        const auto x = static_cast<float>(s.p.x);
        const auto y = static_cast<float>(s.p.y);
        FPDF_BOOL ok = 1;
        switch (s.op) {
        case PdfPathSegment::Op::MoveTo: ok = FPDFPath_MoveTo(object.get(), x, y); break;
        case PdfPathSegment::Op::LineTo: ok = FPDFPath_LineTo(object.get(), x, y); break;
        case PdfPathSegment::Op::CubicTo:
            ok = FPDFPath_BezierTo(object.get(), static_cast<float>(s.c1.x), static_cast<float>(s.c1.y),
                                   static_cast<float>(s.c2.x), static_cast<float>(s.c2.y), x, y);
            break;
        case PdfPathSegment::Op::Close: ok = FPDFPath_Close(object.get()); break;
        }
        if (ok == 0) return fail("PDFium could not add a path segment");
    }
    if (path.fill.has_value() &&
        FPDFPageObj_SetFillColor(object.get(), toByte(path.fill->r), toByte(path.fill->g), toByte(path.fill->b), 255) ==
            0) {
        return fail("PDFium could not set a path's fill color");
    }
    if (path.stroke.has_value()) {
        if (FPDFPageObj_SetStrokeColor(object.get(), toByte(path.stroke->r), toByte(path.stroke->g),
                                       toByte(path.stroke->b), 255) == 0 ||
            FPDFPageObj_SetStrokeWidth(object.get(), path.strokeWidth) == 0 ||
            FPDFPageObj_SetLineJoin(object.get(), path.roundJoins ? FPDF_LINEJOIN_ROUND : FPDF_LINEJOIN_MITER) == 0 ||
            FPDFPageObj_SetLineCap(object.get(), path.roundJoins ? FPDF_LINECAP_ROUND : FPDF_LINECAP_BUTT) == 0) {
            return fail("PDFium could not set a path's stroke");
        }
        if (!path.dash.empty() &&
            FPDFPageObj_SetDashArray(object.get(), path.dash.data(), path.dash.size(), 0.0F) == 0) {
            return fail("PDFium could not set a path's dash pattern");
        }
    }
    if (FPDFPath_SetDrawMode(object.get(), path.fill.has_value() ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE,
                             path.stroke.has_value() ? 1 : 0) == 0) {
        return fail("PDFium could not set a path's draw mode");
    }
    return insert(page, object);
}

core::Status addText(FPDF_DOCUMENT document, FPDF_PAGE page, const PdfBuildText& text, FontCache& fonts) {
    const FPDF_FONT font = fonts.get(text.font);
    if (font == nullptr) return fail("PDFium rejected a bundled font program");
    OwnedObject object(FPDFPageObj_CreateTextObj(document, font, static_cast<float>(text.fontSize)));
    if (object.get() == nullptr) return fail("PDFium could not create a text object");
    const std::vector<FPDF_WCHAR> wide = toWide(text.utf8);
    if (FPDFText_SetText(object.get(), wide.data()) == 0) return fail("PDFium could not set the text of a run");
    if (FPDFPageObj_SetFillColor(object.get(), toByte(text.color.r), toByte(text.color.g), toByte(text.color.b),
                                 255) == 0) {
        return fail("PDFium could not set a text color");
    }
    const FS_MATRIX m = matrix(1.0, 1.0, text.origin.x, text.origin.y);
    if (FPDFPageObj_SetMatrix(object.get(), &m) == 0) return fail("PDFium could not place a text run");
    return insert(page, object);
}

core::Status addImage(FPDF_DOCUMENT document, FPDF_PAGE page, const PdfBuildImage& image) {
    OwnedObject object(FPDFPageObj_NewImageObj(document));
    if (object.get() == nullptr) return fail("PDFium could not create an image object");
    const PdfImageData& data = image.image;
    FPDF_PAGE pages[1] = {page};
    if (data.format == PdfImageData::Format::Jpeg) {
        internal::JpegSource source{data.bytes.data(), data.bytes.size()};
        FPDF_FILEACCESS access{};
        access.m_FileLen = static_cast<unsigned long>(data.bytes.size());
        access.m_GetBlock = &internal::readJpegBlock;
        access.m_Param = &source;
        if (FPDFImageObj_LoadJpegFileInline(pages, 1, object.get(), &access) == 0) {
            return std::unexpected(
                core::makeError(core::ErrorCode::InvalidArgument, "an image's JPEG data could not be decoded", "pdf"));
        }
    } else {
        // PDFium only reads the buffer; the const_cast is not written through.
        FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(static_cast<int>(data.width), static_cast<int>(data.height),
                                                 FPDFBitmap_BGRA, const_cast<std::uint8_t*>(data.bytes.data()),
                                                 static_cast<int>(data.stride));
        if (bitmap == nullptr) return fail("PDFium could not wrap an image's pixels");
        const bool ok = FPDFImageObj_SetBitmap(pages, 1, object.get(), bitmap) != 0;
        FPDFBitmap_Destroy(bitmap);
        if (!ok) return fail("PDFium could not set an image's pixels");
    }
    const FS_MATRIX m = matrix(image.box.width(), image.box.height(), image.box.left, image.box.bottom);
    if (FPDFPageObj_SetMatrix(object.get(), &m) == 0) return fail("PDFium could not place an image");
    return insert(page, object);
}

core::Status addLink(FPDF_PAGE page, const PdfBuildLink& link) {
    ScopedAnnotation annotation(FPDFPage_CreateAnnot(page, FPDF_ANNOT_LINK));
    if (annotation.get() == nullptr) return fail("PDFium could not create a link annotation");
    const FS_RECTF rect{static_cast<float>(link.box.left), static_cast<float>(link.box.top),
                        static_cast<float>(link.box.right), static_cast<float>(link.box.bottom)};
    if (FPDFAnnot_SetRect(annotation.get(), &rect) == 0) return fail("PDFium could not set a link rectangle");
    if (FPDFAnnot_SetBorder(annotation.get(), 0.0F, 0.0F, 0.0F) == 0) {
        return fail("PDFium could not set a link border");
    }
    if (FPDFAnnot_SetURI(annotation.get(), link.uri.c_str()) == 0) return fail("PDFium could not set a link URI");
    return core::ok();
}

core::Status buildPage(FPDF_DOCUMENT document, int index, const PdfBuildPage& spec, FontCache& fonts) {
    ScopedPage page(FPDFPage_New(document, index, spec.width, spec.height));
    if (page.get() == nullptr) return fail("PDFium could not create page " + std::to_string(index));
    for (const PdfBuildItem& item : spec.items) {
        const core::Status status = std::visit(
            [&](const auto& value) -> core::Status {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, PdfBuildRect>) {
                    return addRect(page.get(), value);
                } else if constexpr (std::is_same_v<T, PdfBuildText>) {
                    return addText(document, page.get(), value, fonts);
                } else if constexpr (std::is_same_v<T, PdfBuildPath>) {
                    return addPath(page.get(), value);
                } else {
                    static_assert(std::is_same_v<T, PdfBuildImage>);
                    return addImage(document, page.get(), value);
                }
            },
            item);
        if (!status) return status;
    }
    for (const PdfBuildLink& link : spec.links) {
        if (core::Status status = addLink(page.get(), link); !status) return status;
    }
    if (FPDFPage_GenerateContent(page.get()) == 0) {
        return fail("PDFium could not generate the content of page " + std::to_string(index));
    }
    return core::ok();
}

} // namespace

core::Status buildWithPdfium(const PdfBuildDocument& build, IPdfByteSink& sink) {
    try {
        if (core::Status valid = validatePdfBuild(build); !valid) return valid;
        // Public entry operation: ONE gate acquisition for the whole build
        // and save. Every helper above assumes the gate is held.
        return globalPdfiumCallGate().invoke([&]() -> core::Status {
            ScopedDocument document(FPDF_CreateNewDocument());
            if (document.get() == nullptr) return fail("PDFium could not create a document");
            {
                // Closed (destructor) before the save and before `document`.
                FontCache fonts(document.get());
                for (std::size_t i = 0; i < build.pages.size(); ++i) {
                    if (core::Status status = buildPage(document.get(), static_cast<int>(i), build.pages[i], fonts);
                        !status) {
                        return status;
                    }
                }
            }
            const int count = FPDF_GetPageCount(document.get());
            if (count < 0 || static_cast<std::size_t>(count) != build.pages.size()) {
                return fail("the built document has an unexpected page count");
            }
            return internal::saveDocumentToSink(document.get(), sink);
        });
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::makeError(core::ErrorCode::OutOfMemory,
                                               "out of memory while building the document", "pdf"));
    } catch (const std::exception&) {
        return fail("unexpected failure while building the document");
    }
}

} // namespace rivet::pdf
