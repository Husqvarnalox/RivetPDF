// SPDX-License-Identifier: MPL-2.0
#pragma once

// Editor-level annotation types (Phase 4, ADR-0011): the per-page state kept
// in the page model, and the display-space view of an annotation that the
// UI consumes. Stored data (pdf::PdfAnnotationData) is in PDF user space;
// everything named *View / Display* here is in the DISPLAY space of the
// page's current view (points, origin top-left, y-down, rotation applied).

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::editor {

// An annotation Rivet draws itself (created or edited), in z-order.
struct OverlayAnnotation {
    core::AnnotationId id;
    pdf::PdfAnnotationData data; // user space of the entry's source page
    // Set when the annotation already exists in the source file at this
    // /Annots index (after a save); that index is then also suppressed.
    std::optional<std::uint32_t> fileIndex;
};

// What differs from the source page's own annotations. Immutable and shared
// (commands swap pointers). A null pointer in PageEntry means "untouched".
struct PageAnnotationState {
    // Sorted unique /Annots indices of the entry's source page that the
    // raster must not draw (deleted, or superseded by an overlay version).
    // A suppressed annotation's popup is suppressed with it.
    std::vector<std::uint32_t> suppressed;
    // Rivet-drawn annotations in z-order (last = topmost).
    std::vector<OverlayAnnotation> overlay;
};

using PageAnnotationStatePtr = std::shared_ptr<const PageAnnotationState>;

// What the UI may do with an annotation (bitmask).
enum AnnotationCap : unsigned {
    kCapMove = 1,
    kCapResize = 2,
    kCapRestyle = 4,
    kCapEditContents = 8,
    kCapDelete = 16,
};

// Capabilities of a kind (see ADR-0011): markup restyle/contents/delete; a
// note moves too; shapes, ink, lines and stamps move/resize/restyle/delete.
unsigned annotationCaps(pdf::PdfAnnotationKind kind);

struct AnnotationStyle {
    pdf::PdfColor color;
    std::optional<pdf::PdfColor> interiorColor;
    float opacity = 1.0F;
    float borderWidth = 1.0F; // points (the UI applies the zoom)
};

// Rivet's appearance mapped to display space (see pdf::PdfAppearance).
struct DisplayPathSegment {
    pdf::PdfPathSegment::Op op = pdf::PdfPathSegment::Op::MoveTo;
    core::Point p;
    core::Point c1;
    core::Point c2;
};

struct DisplayPath {
    std::vector<DisplayPathSegment> segments;
    std::optional<pdf::PdfColor> fill;
    std::optional<pdf::PdfColor> stroke;
    float strokeWidth = 1.0F; // points
    bool roundJoins = false;
};

struct DisplayText {
    std::string text; // ASCII
    core::Rect box;   // display space
    // Clockwise quarter turns in degrees (0/90/180/270) of the text inside
    // `box`: (text rotation + view rotation) mod 360.
    int rotation = 0;
    pdf::PdfColor color;
    bool bold = true;
};

struct DisplayAppearance {
    std::vector<DisplayPath> paths; // painted in order, below texts
    std::vector<DisplayText> texts;
    float opacity = 1.0F;
};

using DisplayQuad = std::array<core::Point, 4>; // PDFium order: TL, TR, BL, BR of the run

// One selectable annotation of a page, in display space of the page's
// current view.
struct AnnotationView {
    core::AnnotationId id;
    pdf::PdfAnnotationKind kind = pdf::PdfAnnotationKind::Other;
    unsigned caps = 0; // AnnotationCap bits
    core::Rect bounds; // bounding box of the drawn appearance
    std::vector<DisplayQuad> quads;
    std::vector<std::vector<core::Point>> strokes;
    core::Point lineStart;
    core::Point lineEnd;
    AnnotationStyle style;
    std::string contents; // UTF-8, untrusted
    std::string author;   // UTF-8, untrusted
    pdf::PdfStampName stampName = pdf::PdfStampName::Approved;
    // True for Rivet-drawn (overlay) annotations; false = the raster draws it.
    bool drawnByOverlay = false;
    // Set for overlay-drawn annotations only.
    std::optional<DisplayAppearance> appearance;
};

} // namespace rivet::editor
