// SPDX-License-Identifier: MPL-2.0

#include "PdfiumAnnotations.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "PdfiumStrings.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"

// Writes Rivet annotations through the public FPDFPage_*/FPDFAnnot_* API
// (ADR-0012). The ORDER of the calls matters (verified against the pinned
// PDFium source):
//   1. FPDFAnnot_SetColor / SetBorder / AppendObject / SetAP all key off
//      the presence of a normal appearance stream: SetColor FAILS once one
//      exists, SetBorder removes it, AppendObject bases the new form's
//      /BBox on /Rect at that moment. So /Rect, /F and the colors (which
//      also write /CA) go first, then the border, then geometry, then
//      strings, and the appearance last.
//   2. SetStringValue always writes PDF *string* objects (no names), hence
//      /Subj, /RivetShape and /RivetRotation carry the information that
//      cannot be written as /Name or /Subtype.
//   3. FPDFAnnot_SetAP(content) puts /Resources (the /GS ExtGState for
//      /CA < 1) on the stream only when the annotation's /CA is below 1,
//      which is why the opacity is written as /CA before the AP.

namespace rivet::pdf::internal {
namespace {

core::Error internalError(std::string message) {
    return core::makeError(core::ErrorCode::Internal, std::move(message), "pdf");
}

unsigned int toByte(float component) {
    const float clamped = std::clamp(component, 0.0F, 1.0F);
    return static_cast<unsigned int>(std::lround(static_cast<double>(clamped) * 255.0));
}

// FPDF_WIDESTRING (NUL-terminated UTF-16) from UTF-8.
std::vector<FPDF_WCHAR> wide(std::string_view text) {
    const std::vector<std::uint8_t> bytes = utf8ToUtf16le(text);
    std::vector<FPDF_WCHAR> units(bytes.size() / 2);
    for (std::size_t i = 0; i < units.size(); ++i) {
        units[i] = static_cast<FPDF_WCHAR>(static_cast<unsigned>(bytes[2 * i]) |
                                           (static_cast<unsigned>(bytes[2 * i + 1]) << 8));
    }
    return units;
}

bool setString(FPDF_ANNOTATION annot, const char* key, std::string_view value) {
    const std::vector<FPDF_WCHAR> units = wide(value);
    return FPDFAnnot_SetStringValue(annot, key, units.data()) != 0;
}

// Random (version 4, variant 1) UUID in the canonical 8-4-4-4-12 form.
std::string randomUuid() {
    std::random_device device;
    std::uint8_t bytes[16];
    for (std::size_t i = 0; i < 16; i += 4) {
        const std::uint32_t word = device();
        for (std::size_t k = 0; k < 4; ++k) {
            bytes[i + k] = static_cast<std::uint8_t>((word >> (8 * k)) & 0xFFu);
        }
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3Fu) | 0x80u);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 0x0Fu];
    }
    return out;
}

// Current UTC time as a PDF date string "D:YYYYMMDDHHmmSSZ" (civil-from-days
// conversion by H. Hinnant; no gmtime, so no shared static state).
std::string pdfDateNow() {
    using namespace std::chrono;
    const auto seconds = floor<std::chrono::seconds>(system_clock::now()).time_since_epoch().count();
    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "D:%04lld%02lld%02lld%02lld%02lld%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                  static_cast<long long>(rem % 60));
    return buffer;
}

std::optional<FPDF_ANNOTATION_SUBTYPE> subtypeOf(PdfAnnotationKind kind) {
    switch (kind) {
    case PdfAnnotationKind::Highlight:
        return FPDF_ANNOT_HIGHLIGHT;
    case PdfAnnotationKind::Underline:
        return FPDF_ANNOT_UNDERLINE;
    case PdfAnnotationKind::StrikeOut:
        return FPDF_ANNOT_STRIKEOUT;
    case PdfAnnotationKind::Note:
        return FPDF_ANNOT_TEXT;
    case PdfAnnotationKind::Ink:
    case PdfAnnotationKind::Line:
    case PdfAnnotationKind::Arrow:
        return FPDF_ANNOT_INK; // Line/Arrow: PDFium cannot create /Line (ADR-0012)
    case PdfAnnotationKind::Square:
        return FPDF_ANNOT_SQUARE;
    case PdfAnnotationKind::Circle:
        return FPDF_ANNOT_CIRCLE;
    case PdfAnnotationKind::Stamp:
        return FPDF_ANNOT_STAMP;
    case PdfAnnotationKind::Other:
        break;
    }
    return std::nullopt;
}

bool addStroke(FPDF_ANNOTATION annot, const std::vector<PdfPoint>& points) {
    std::vector<FS_POINTF> buffer;
    buffer.reserve(points.size());
    for (const PdfPoint& p : points) {
        buffer.push_back(FS_POINTF{static_cast<float>(p.x), static_cast<float>(p.y)});
    }
    return FPDFAnnot_AddInkStroke(annot, buffer.data(), buffer.size()) >= 0;
}

// Owns a page object until FPDFAnnot_AppendObject takes it over.
class ScopedPageObject {
public:
    explicit ScopedPageObject(FPDF_PAGEOBJECT object) : object_(object) {}
    ~ScopedPageObject() {
        if (object_ != nullptr) {
            FPDFPageObj_Destroy(object_);
        }
    }
    ScopedPageObject(const ScopedPageObject&) = delete;
    ScopedPageObject& operator=(const ScopedPageObject&) = delete;

    FPDF_PAGEOBJECT get() const { return object_; }
    FPDF_PAGEOBJECT release() {
        FPDF_PAGEOBJECT out = object_;
        object_ = nullptr;
        return out;
    }

private:
    FPDF_PAGEOBJECT object_;
};

bool appendPath(FPDF_ANNOTATION annot, const PdfAppearancePath& path, float opacity) {
    if (path.segments.empty() || path.segments.front().op != PdfPathSegment::Op::MoveTo) {
        return false;
    }
    const PdfPoint start = path.segments.front().p;
    ScopedPageObject object(
        FPDFPageObj_CreateNewPath(static_cast<float>(start.x), static_cast<float>(start.y)));
    if (object.get() == nullptr) {
        return false;
    }
    FPDF_PAGEOBJECT obj = object.get();
    for (std::size_t i = 1; i < path.segments.size(); ++i) {
        const PdfPathSegment& seg = path.segments[i];
        bool ok = true;
        switch (seg.op) {
        case PdfPathSegment::Op::MoveTo:
            ok = FPDFPath_MoveTo(obj, static_cast<float>(seg.p.x), static_cast<float>(seg.p.y)) != 0;
            break;
        case PdfPathSegment::Op::LineTo:
            ok = FPDFPath_LineTo(obj, static_cast<float>(seg.p.x), static_cast<float>(seg.p.y)) != 0;
            break;
        case PdfPathSegment::Op::CubicTo:
            ok = FPDFPath_BezierTo(obj, static_cast<float>(seg.c1.x), static_cast<float>(seg.c1.y),
                                   static_cast<float>(seg.c2.x), static_cast<float>(seg.c2.y),
                                   static_cast<float>(seg.p.x), static_cast<float>(seg.p.y)) != 0;
            break;
        case PdfPathSegment::Op::Close:
            ok = FPDFPath_Close(obj) != 0;
            break;
        }
        if (!ok) {
            return false;
        }
    }
    const unsigned int alpha = toByte(opacity);
    if (FPDFPath_SetDrawMode(obj, path.fill ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE,
                             path.stroke ? 1 : 0) == 0) {
        return false;
    }
    if (path.stroke) {
        if (FPDFPageObj_SetStrokeColor(obj, toByte(path.stroke->r), toByte(path.stroke->g),
                                       toByte(path.stroke->b), alpha) == 0 ||
            FPDFPageObj_SetStrokeWidth(obj, path.strokeWidth) == 0) {
            return false;
        }
    }
    if (path.fill && FPDFPageObj_SetFillColor(obj, toByte(path.fill->r), toByte(path.fill->g),
                                              toByte(path.fill->b), alpha) == 0) {
        return false;
    }
    if (path.roundJoins) {
        FPDFPageObj_SetLineCap(obj, FPDF_LINECAP_ROUND);
        FPDFPageObj_SetLineJoin(obj, FPDF_LINEJOIN_ROUND);
    }
    if (FPDFAnnot_AppendObject(annot, obj) == 0) {
        return false; // still owned by `object`
    }
    object.release();
    return true;
}

// A single line of text fitted into text.box: laid out at 100 pt, scaled to
// 90% of the box (swapped for quarter turns), rotated CLOCKWISE by
// text.rotation (y-up user space) and centered.
bool appendText(FPDF_DOCUMENT doc, FPDF_ANNOTATION annot, const PdfAppearanceText& text, float opacity) {
    if (text.text.empty()) {
        return true;
    }
    ScopedPageObject object(FPDFPageObj_NewTextObj(doc, text.bold ? "Helvetica-Bold" : "Helvetica", 100.0F));
    if (object.get() == nullptr) {
        return false;
    }
    FPDF_PAGEOBJECT obj = object.get();
    const std::vector<FPDF_WCHAR> units = wide(text.text);
    if (FPDFText_SetText(obj, units.data()) == 0) {
        return false;
    }
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    if (FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top) == 0) {
        return false;
    }
    const double w = static_cast<double>(right) - static_cast<double>(left);
    const double h = static_cast<double>(top) - static_cast<double>(bottom);
    if (!(w > 0.0) || !(h > 0.0)) {
        return false;
    }
    const bool quarter = text.rotation == 90 || text.rotation == 270;
    const double availW = quarter ? text.box.height() : text.box.width();
    const double availH = quarter ? text.box.width() : text.box.height();
    const double scale = 0.9 * std::min(availW / w, availH / h);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return false;
    }
    // Clockwise by theta in y-up space: x' = x cos + y sin, y' = -x sin + y cos.
    const double theta = static_cast<double>(text.rotation) * 3.14159265358979323846 / 180.0;
    const double cosT = std::round(std::cos(theta));
    const double sinT = std::round(std::sin(theta)); // exact for quarter turns
    const double a = scale * cosT;
    const double b = -scale * sinT;
    const double c = scale * sinT;
    const double d = scale * cosT;
    const double cx = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
    const double cy = (static_cast<double>(bottom) + static_cast<double>(top)) / 2.0;
    const double boxCx = (text.box.left + text.box.right) / 2.0;
    const double boxCy = (text.box.bottom + text.box.top) / 2.0;
    const double e = boxCx - (a * cx + c * cy);
    const double f = boxCy - (b * cx + d * cy);
    FPDFPageObj_Transform(obj, a, b, c, d, e, f);

    if (FPDFPageObj_SetFillColor(obj, toByte(text.color.r), toByte(text.color.g), toByte(text.color.b),
                                 toByte(opacity)) == 0) {
        return false;
    }
    if (FPDFAnnot_AppendObject(annot, obj) == 0) {
        return false;
    }
    object.release();
    return true;
}

// Creates one annotation on `page` per the recipe in the header comment.
core::Status createAnnotation(FPDF_DOCUMENT doc, FPDF_PAGE page, PdfAnnotationData data) {
    normalizeAnnotation(data);
    const auto subtype = subtypeOf(data.kind);
    if (!subtype || !isWritableAnnotation(data)) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "annotation cannot be written", "pdf"));
    }
    const PdfAppearance appearance = buildAppearance(data);

    ScopedAnnot annot(FPDFPage_CreateAnnot(page, *subtype));
    FPDF_ANNOTATION a = annot.get();
    if (a == nullptr) {
        return std::unexpected(internalError("PDFium could not create an annotation"));
    }
    const char* failed = nullptr;
    auto step = [&failed](bool ok, const char* what) {
        if (!ok && failed == nullptr) {
            failed = what;
        }
    };

    const FS_RECTF rect{static_cast<float>(data.rect.left), static_cast<float>(data.rect.top),
                        static_cast<float>(data.rect.right), static_cast<float>(data.rect.bottom)};
    step(FPDFAnnot_SetRect(a, &rect) != 0, "rect");
    step(FPDFAnnot_SetFlags(a, FPDF_ANNOT_FLAG_PRINT) != 0, "flags");

    const unsigned int alpha = toByte(data.opacity);
    step(FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, toByte(data.color.r), toByte(data.color.g),
                            toByte(data.color.b), alpha) != 0,
         "color");
    if ((data.kind == PdfAnnotationKind::Square || data.kind == PdfAnnotationKind::Circle) &&
        data.interiorColor) {
        step(FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_InteriorColor, toByte(data.interiorColor->r),
                                toByte(data.interiorColor->g), toByte(data.interiorColor->b), alpha) != 0,
             "interior color");
    }

    switch (data.kind) {
    case PdfAnnotationKind::Ink:
    case PdfAnnotationKind::Line:
    case PdfAnnotationKind::Arrow:
    case PdfAnnotationKind::Square:
    case PdfAnnotationKind::Circle:
        step(FPDFAnnot_SetBorder(a, 0.0F, 0.0F, data.borderWidth) != 0, "border");
        break;
    default:
        break;
    }

    switch (data.kind) {
    case PdfAnnotationKind::Highlight:
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut:
        for (const PdfQuad& q : data.quads) {
            const FS_QUADPOINTSF quad{static_cast<float>(q.p1.x), static_cast<float>(q.p1.y),
                                      static_cast<float>(q.p2.x), static_cast<float>(q.p2.y),
                                      static_cast<float>(q.p3.x), static_cast<float>(q.p3.y),
                                      static_cast<float>(q.p4.x), static_cast<float>(q.p4.y)};
            step(FPDFAnnot_AppendAttachmentPoints(a, &quad) != 0, "quad points");
        }
        break;
    case PdfAnnotationKind::Ink:
        for (const auto& stroke : data.inkStrokes) {
            step(addStroke(a, stroke), "ink stroke");
        }
        break;
    case PdfAnnotationKind::Line:
        step(addStroke(a, {data.lineStart, data.lineEnd}), "line stroke");
        break;
    case PdfAnnotationKind::Arrow: {
        step(addStroke(a, {data.lineStart, data.lineEnd}), "arrow shaft");
        // The wings come from the appearance so reader, writer and drawing agree.
        const auto& head = appearance.paths.size() > 1 ? appearance.paths[1].segments
                                                       : std::vector<PdfPathSegment>{};
        if (head.size() == 3) {
            step(addStroke(a, {head[0].p, head[1].p, head[2].p}), "arrow head");
        } else {
            step(false, "arrow head");
        }
        break;
    }
    default:
        break;
    }

    if (!data.contents.empty()) step(setString(a, "Contents", data.contents), "contents");
    if (!data.author.empty()) step(setString(a, "T", data.author), "author");
    step(setString(a, "NM", data.name.empty() ? randomUuid() : data.name), "name");
    step(setString(a, "M", pdfDateNow()), "modification date");
    if (data.kind == PdfAnnotationKind::Stamp) {
        step(setString(a, "Subj", stampNameText(data.stampName)), "subject");
        step(setString(a, "RivetRotation", std::to_string(data.rotation)), "rotation");
    } else if (data.kind == PdfAnnotationKind::Line || data.kind == PdfAnnotationKind::Arrow) {
        const char* shape = data.kind == PdfAnnotationKind::Line ? "Line" : "Arrow";
        step(setString(a, "Subj", shape), "subject");
        step(setString(a, "RivetShape", shape), "shape");
    }
    if (failed != nullptr) {
        return std::unexpected(internalError(std::string("PDFium failed to write annotation ") + failed));
    }

    // Appearance last.
    if (data.kind == PdfAnnotationKind::Stamp) {
        for (const PdfAppearancePath& path : appearance.paths) {
            step(appendPath(a, path, appearance.opacity), "stamp path");
        }
        for (const PdfAppearanceText& text : appearance.texts) {
            step(appendText(doc, a, text, appearance.opacity), "stamp text");
        }
    } else {
        const std::string content = appearanceContentStream(appearance);
        const std::vector<FPDF_WCHAR> units = wide(content);
        step(FPDFAnnot_SetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, units.data()) != 0, "appearance");
    }
    if (failed != nullptr) {
        return std::unexpected(internalError(std::string("PDFium failed to write annotation ") + failed));
    }
    return core::ok();
}

} // namespace

core::Status applyAnnotationEdits(FPDF_DOCUMENT working,
                                  FPDF_PAGE page,
                                  const PdfPageAnnotationEdits& edits,
                                  PdfAssembledPageAnnotations& report) {
    const int before = FPDFPage_GetAnnotCount(page);
    for (auto it = edits.removeIndices.rbegin(); it != edits.removeIndices.rend(); ++it) {
        if (*it >= static_cast<std::uint32_t>(std::max(0, before)) ||
            FPDFPage_RemoveAnnot(page, static_cast<int>(*it)) == 0) {
            return std::unexpected(internalError("PDFium failed to remove an annotation"));
        }
    }
    int count = FPDFPage_GetAnnotCount(page);
    if (count != before - static_cast<int>(edits.removeIndices.size())) {
        return std::unexpected(internalError("annotation count mismatch after removals"));
    }

    report.createdIndices.clear();
    for (const PdfAnnotationData& data : edits.create) {
        if (auto created = createAnnotation(working, page, data); !created.has_value()) {
            return created;
        }
        if (FPDFPage_GetAnnotCount(page) != count + 1) {
            return std::unexpected(internalError("annotation count mismatch after creation"));
        }
        report.createdIndices.push_back(static_cast<std::uint32_t>(count));
        ++count;
    }
    report.annotsCount = static_cast<std::uint32_t>(count);
    return core::ok();
}

} // namespace rivet::pdf::internal
