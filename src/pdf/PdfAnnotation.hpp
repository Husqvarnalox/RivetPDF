// SPDX-License-Identifier: MPL-2.0
#pragma once

// Engine-independent annotation data (Phase 4, see ADR-0011/0012).
//
// Everything here is plain Rivet-owned data: no engine handles, freely
// copyable and shareable across threads once built. ALL geometry is in the
// PDF USER SPACE of the page the annotation belongs to (points, origin
// bottom-left, y-up, before /Rotate and independent of the crop box) - the
// same space PdfPageView::cropBox uses. The editor maps it to display space
// through pdf/PdfPageGeometry; nothing above the editor sees user space.
//
// Strings (contents, author, ...) are UTF-8 and UNTRUSTED (they come from
// arbitrary files): never log them, never interpret them.

#include "core/Bitmap.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::pdf {

// Kinds Rivet understands. Everything else is `Other` (opaque: preserved
// byte-for-byte through save, drawn by the engine's raster, never edited).
enum class PdfAnnotationKind : std::uint8_t {
    Highlight,
    Underline,
    StrikeOut,
    Note,      // /Text (sticky note)
    Ink,
    Square,    // rectangle shape
    Circle,    // ellipse shape
    Line,      // Rivet line (persisted as /Ink + RivetShape, see ADR-0012)
    Arrow,     // Rivet arrow (idem)
    Stamp,
    Signature, // Rivet visual signature (persisted as /Stamp + /Name RivetSignature, ADR-0025)
    Other,
};

// Standard stamp names Rivet creates (written as /Subj, see ADR-0012).
enum class PdfStampName : std::uint8_t { Approved, Draft, Confidential, Final };

// Stable ASCII spelling ("Approved", ...) and the inverse (case-sensitive;
// nullopt for anything else).
const char* stampNameText(PdfStampName name);
std::optional<PdfStampName> parseStampName(std::string_view text);

// sRGB, components in [0, 1].
struct PdfColor {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    bool operator==(const PdfColor&) const = default;
};

struct PdfPoint {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const PdfPoint&) const = default;
};

// One /QuadPoints entry in the order PDFium (and Acrobat) use:
// p1 = top-left, p2 = top-right, p3 = bottom-left, p4 = bottom-right of the
// marked text run, "top" meaning the side the glyph tops face.
struct PdfQuad {
    PdfPoint p1;
    PdfPoint p2;
    PdfPoint p3;
    PdfPoint p4;
    bool operator==(const PdfQuad&) const = default;
};

// Hard limits (security / resource bounds). The reader treats an
// annotation that exceeds one as Other (opaque, preserved, not editable);
// the editor refuses to create or grow one past them.
inline constexpr std::size_t kMaxAnnotationsPerPage = 4096;  // read in detail; rest opaque
inline constexpr std::size_t kMaxQuadsPerAnnotation = 4096;
inline constexpr std::size_t kMaxInkStrokes = 256;
inline constexpr std::size_t kMaxInkPointsPerStroke = 10000;
inline constexpr std::size_t kMaxInkPointsTotal = 50000;
inline constexpr std::size_t kMaxContentsBytes = 64 * 1024; // UTF-8 bytes
inline constexpr std::size_t kMaxAuthorBytes = 1024;

// --- Visual signatures (ADR-0025) -----------------------------------------

enum class PdfSignatureSource : std::uint8_t {
    Ink,   // strokes drawn on the signature pad
    Text,  // a typed name set in the bundled serif face (Tinos Regular)
    Image, // a decoded PNG/JPEG
};

// The /Name a Rivet visual signature carries (its /Subtype is /Stamp).
inline constexpr const char* kRivetSignatureName = "RivetSignature";

// Limits of one signature appearance (also the SignatureStore caps).
inline constexpr std::size_t kMaxSignatureStrokes = 256;
inline constexpr std::size_t kMaxSignaturePoints = 8000; // all strokes together
inline constexpr std::size_t kMaxSignatureTextBytes = 256;
inline constexpr std::uint32_t kMaxSignatureImageSide = 1024;
// Aspect ratios (width / height) are kept inside this range: a pad stroke
// that is a flat line still gets a usable box.
inline constexpr double kMinSignatureAspect = 0.1;
inline constexpr double kMaxSignatureAspect = 20.0;

// What a signature looks like, independent of where it is placed. Immutable
// once built and shared between annotations (PdfAnnotationData::signature).
struct PdfSignatureAppearance {
    PdfSignatureSource source = PdfSignatureSource::Ink;
    // Ink: strokes in a box of height 1 and width `aspect` (y-up, origin
    // bottom-left). `inkWidth` is the pen width as a fraction of the box
    // height, so the line scales with the placed signature.
    std::vector<std::vector<PdfPoint>> strokes;
    double aspect = 1.0;
    float inkWidth = 0.04F;
    // Text: UTF-8, <= kMaxSignatureTextBytes, one line.
    std::string text;
    // Image: straight-alpha BGRA, each side <= kMaxSignatureImageSide.
    std::shared_ptr<const core::Bitmap> image;
};

using PdfSignatureAppearancePtr = std::shared_ptr<const PdfSignatureAppearance>;

// Pad strokes (any y-DOWN coordinate space, e.g. pad-local points) -> an Ink
// signature: the tight box of the points, padded by half the pen and to the
// aspect limits, mapped to height 1 (y flipped up). nullopt when there is no
// point or a limit (kMaxSignatureStrokes / kMaxSignaturePoints, non-finite
// coordinates) is exceeded.
std::optional<PdfSignatureAppearance> makeInkSignature(const std::vector<std::vector<PdfPoint>>& padStrokes,
                                                       double penWidth);

// Width / height of the signature's natural box (ink: `aspect`; text: the
// set line's advance over its ascent - descent; image: pixel aspect),
// clamped into [kMinSignatureAspect, kMaxSignatureAspect]; 1 when unknown.
double signatureAspect(const PdfSignatureAppearance& signature);

// Whether a signature is within its limits and drawable (non-empty strokes
// / text with at least one glyph / valid image).
bool isValidSignature(const PdfSignatureAppearance& signature);

// The largest box of `aspect` (content width / height) centered in
// `available`, where the content is turned `rotation` clockwise quarter
// turns (the content width then runs along user y for 90/270). Returned in
// the same space as `available` (normalized).
PdfBox fitSignatureBox(double aspect, const PdfBox& available, int rotation);

// The typed signature's font size that makes the line exactly fill a box of
// `width` x `height` (content orientation) without overflowing either side.
double signatureFontSize(std::string_view text, double width, double height);

// The editable description of one annotation. Which fields are meaningful
// depends on `kind`:
//   Highlight/Underline/StrikeOut: quads, color, opacity, contents.
//   Note:   rect (icon box), color, contents.
//   Ink:    inkStrokes, color, opacity, borderWidth.
//   Square/Circle: rect, color (stroke), interiorColor (fill, optional),
//           opacity, borderWidth.
//   Line/Arrow: lineStart, lineEnd, color, opacity, borderWidth (Arrow has
//           an open arrow head at lineEnd).
//   Stamp:  rect, stampName, color, rotation, contents.
//   Signature: rect, signature (the appearance source), color (ink/text),
//           rotation (like Stamp: reads upright on the page it was placed on).
// `rect` is ALWAYS the bounding box of the drawn appearance (normalized,
// left<right, bottom<top) - for markup/ink/line it is derived (see
// normalizeAnnotation) and is what /Rect is written as.
struct PdfAnnotationData {
    PdfAnnotationKind kind = PdfAnnotationKind::Other;
    PdfBox rect;
    PdfColor color{1.0F, 0.85F, 0.0F};
    std::optional<PdfColor> interiorColor;
    float opacity = 1.0F;     // [0.05, 1]
    float borderWidth = 1.0F; // points, [0.25, 50]
    std::vector<PdfQuad> quads;
    std::vector<std::vector<PdfPoint>> inkStrokes;
    PdfPoint lineStart;
    PdfPoint lineEnd;
    PdfStampName stampName = PdfStampName::Approved;
    // Clockwise quarter turns (0/90/180/270) of the appearance INSIDE rect,
    // so a stamp placed on a rotated page reads upright on screen.
    int rotation = 0;
    // Signature only (null for every other kind).
    PdfSignatureAppearancePtr signature;
    std::string contents; // UTF-8, <= kMaxContentsBytes
    std::string author;   // /T, UTF-8
    std::string name;     // /NM (unique name; Rivet writes a random UUID)

    bool operator==(const PdfAnnotationData&) const = default;
};

// Recomputes `rect` from the geometry for kinds whose box is derived
// (markup from quads, ink from strokes + borderWidth/2, line/arrow from the
// endpoints + width + arrow head) and clamps opacity/borderWidth into range.
// Square/Circle/Note/Stamp keep their rect (normalized).
void normalizeAnnotation(PdfAnnotationData& data);

// Validates limits and finite geometry. False = must not be written.
bool isWritableAnnotation(const PdfAnnotationData& data);

// One entry of a page's /Annots array as read from a document.
struct PdfPageAnnotation {
    std::uint32_t index = 0; // position in the page's /Annots array
    PdfAnnotationKind kind = PdfAnnotationKind::Other;
    // True when Rivet can edit it (kind != Other, within limits, and its
    // data could be read faithfully enough to rewrite it).
    bool editable = false;
    // Non-editable entries (Popup, Link, Widget, FreeText, third-party Line,
    // custom stamps, over-limit data, ...) are not hit-testable: they are
    // drawn by the engine raster and preserved untouched. Popups are listed
    // so their index is known.
    bool isPopup = false;
    // /Annots index of this annotation's /Popup entry, if it has one (it is
    // removed together with its parent).
    std::optional<std::uint32_t> popupIndex;
    std::uint32_t flags = 0; // /F
    // For editable entries: the full data. For others: kind, rect (as
    // stored, normalized) and nothing else meaningful.
    PdfAnnotationData data;
};

// All annotations of one page, in /Annots order. Immutable once built.
struct PdfPageAnnotations {
    std::vector<PdfPageAnnotation> items;
    // Size of the page's /Annots array. Can exceed items.size() when the page
    // has more than kMaxAnnotationsPerPage entries (the rest are opaque and
    // simply preserved).
    std::uint32_t annotsCount = 0;
};

using PdfPageAnnotationsPtr = std::shared_ptr<const PdfPageAnnotations>;

// Annotation edits applied to ONE page of an assembled document. Indices
// refer to the SOURCE page's /Annots array (PdfAssemblyPage::source /
// sourcePageIndex), as returned by PdfDocument::annotations().
//
// Applied after the page view: first every `removeIndices` entry is removed
// (descending), then `create` is appended in order, each with a generated
// appearance stream (see PdfAppearance). Editing an existing annotation =
// remove its index + create its new version (ADR-0012).
struct PdfPageAnnotationEdits {
    std::vector<std::uint32_t> removeIndices; // sorted ascending, unique
    std::vector<PdfAnnotationData> create;
    bool empty() const { return removeIndices.empty() && create.empty(); }
};

// Per output page: the /Annots index each `create` entry ended up at, in the
// order of PdfPageAnnotationEdits::create. Lets the editor re-identify the
// annotations it wrote after a save without re-reading the file.
struct PdfAssembledPageAnnotations {
    std::vector<std::uint32_t> createdIndices;
    std::uint32_t annotsCount = 0; // final /Annots size of the page
};

// --- Appearance ---------------------------------------------------------
//
// A Rivet-generated appearance as vector primitives in page user space. The
// SAME description is used by the on-screen overlay (mapped to display
// space by the editor and painted by the UI) and by the writer (turned into
// the /AP stream), so what you see while editing is what gets saved.

struct PdfPathSegment {
    enum class Op : std::uint8_t { MoveTo, LineTo, CubicTo, Close };
    Op op = Op::MoveTo;
    PdfPoint p;  // end point (MoveTo/LineTo/CubicTo)
    PdfPoint c1; // CubicTo control points
    PdfPoint c2;
};

struct PdfAppearancePath {
    std::vector<PdfPathSegment> segments;
    std::optional<PdfColor> fill;   // nonzero winding
    std::optional<PdfColor> stroke;
    float strokeWidth = 1.0F;
    bool roundJoins = false; // round caps + joins (ink) vs butt/miter
};

// A raster image (signatures): `bitmap` drawn into `box`, its content turned
// `rotation` clockwise quarter turns (y-up user space) inside the box.
struct PdfAppearanceImage {
    std::shared_ptr<const core::Bitmap> bitmap;
    PdfBox box;
    int rotation = 0;
};

// A single line of text in a standard font, laid out to fit `box` (after
// rotating the box content by `rotation` clockwise quarter turns, centered).
struct PdfAppearanceText {
    std::string text; // ASCII (stamp labels only)
    PdfBox box;
    int rotation = 0;
    PdfColor color;
    bool bold = true;
};

struct PdfAppearance {
    std::vector<PdfAppearanceImage> images; // painted first
    std::vector<PdfAppearancePath> paths; // painted in order, below texts
    std::vector<PdfAppearanceText> texts;
    float opacity = 1.0F; // constant alpha for the whole appearance
};

// Builds the appearance of an annotation (data must be normalized).
// Note icons, stamp frames, arrow heads, markup bars are all expressed here.
PdfAppearance buildAppearance(const PdfAnnotationData& data);

// Content stream for an appearance WITHOUT texts or images (all kinds but
// Stamp and Signature):
// pure ASCII PDF operators in user-space coordinates; begins with `/GS gs`
// when opacity < 1 (the writer sets /CA so PDFium adds that ExtGState).
std::string appearanceContentStream(const PdfAppearance& appearance);

} // namespace rivet::pdf
