// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pure geometry behind the annotation service and commands: hit-test
// primitives, stroke simplification and the user <-> display mapping of
// annotation data. Display space = the page view's display space (points,
// origin top-left, y-down); stored data is PDF user space. All view
// conversions go through pdf/PdfPageGeometry.

#include "editor/Annotations.hpp"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <span>
#include <vector>

namespace rivet::editor::geometry {

// --- Primitives (display space, but frame-agnostic) --------------------------

// Distance from `p` to the segment a-b (to the point when a == b).
double distanceToSegment(core::Point p, core::Point a, core::Point b);

// Whether `p` lies inside the quad or within `tolerance` of its outline. The
// quad is in PDFium order (TL, TR, BL, BR), i.e. the polygon TL-TR-BR-BL.
bool pointInQuad(core::Point p, const DisplayQuad& quad, double tolerance = 0.0);

// Distance from `p` to the OUTLINE of the ellipse inscribed in `bounds`
// (a degenerate ellipse falls back to its segment/point).
double distanceToEllipseOutline(core::Point p, const core::Rect& bounds);

// Distance from `p` to the outline of the rectangle (0 on the outline).
double distanceToRectOutline(core::Point p, const core::Rect& rect);

// The two wing end points of an arrow head at `end` (same rule as the
// appearance builder: length max(6, 3 * width), +-30 degrees).
std::array<core::Point, 2> arrowHeadWings(core::Point start, core::Point end, double width);

// Ramer-Douglas-Peucker simplification: keeps the first and last point and
// every point needed so no removed point is farther than `tolerance` from the
// simplified polyline. Iterative (safe for long strokes); long strokes are
// simplified in windows of 1024 points so adversarial input stays near-linear.
std::vector<core::Point> reduceStroke(std::span<const core::Point> points, double tolerance);

// --- User <-> display mapping -------------------------------------------------

core::Point toDisplay(const pdf::PdfPageView& view, const pdf::PdfPoint& point);
pdf::PdfPoint toUser(const pdf::PdfPageView& view, core::Point point);

// Unclamped (annotations may lie outside the crop box): a user box -> the
// normalized display rect, and back.
core::Rect boxToDisplayRect(const pdf::PdfPageView& view, const pdf::PdfBox& box);
pdf::PdfBox rectToUserBox(const pdf::PdfPageView& view, const core::Rect& rect);

// The display-space view of stored (user space) annotation data. `data`
// should be normalized. Overlay-drawn annotations also get their appearance.
AnnotationView makeAnnotationView(core::AnnotationId id, const pdf::PdfAnnotationData& data,
                                  const pdf::PdfPageView& view, bool drawnByOverlay);

// Hit test of one annotation view (see AnnotationService::hitTest).
bool hitsAnnotation(const AnnotationView& view, core::Point point, double tolerancePoints);

} // namespace rivet::editor::geometry
