// SPDX-License-Identifier: MPL-2.0
#pragma once

// The centralized user <-> display mapping of page content objects
// (ADR-0014 "Coordinate spaces"). Object matrices and quads are PDF user
// space; the editor shows and edits them in the DISPLAY space of the page's
// current view (points, origin top-left, y-down, /Rotate and crop applied).
// Every conversion goes through pdf::userToDisplayMatrix(view), so rotation,
// CropBox offsets and nested transforms are handled in one place:
//
//   display = V * objectMatrix * local          V = userToDisplayMatrix(view)
//
// Pointer deltas map back with the inverse LINEAR part of V; a display-space
// scale/translate T_d of an object maps to the user-space transform
// T_u = V^-1 * T_d * V (applied after the object's own matrix).

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <optional>

namespace rivet::editor::geometry {

// display = V * objectMatrix.
core::Matrix objectToDisplay(const pdf::PdfPageView& view, const core::Matrix& objectMatrix);

// A user-space quad (any vertex order) mapped to display space and put in a
// canonical order: clockwise on screen, starting at the top-most (then
// left-most) vertex.
std::array<core::Point, 4> quadToDisplay(const pdf::PdfPageView& view, const std::array<pdf::PdfPoint, 4>& quad);

// Unclamped, normalized display rect of a user-space box.
core::Rect boxToDisplay(const pdf::PdfPageView& view, const pdf::PdfBox& box);

// The user-space translation matching a display-space pointer delta
// (inverse linear part of V). nullopt for a non-finite delta.
std::optional<core::Matrix> userTranslationForDisplayDelta(const pdf::PdfPageView& view, core::Point displayDelta);

// The user-space transform T such that an object whose display bounds are
// `from` gets display bounds `to` when T is applied after its own matrix:
// T = V^-1 * S * V with S the display-space scale + translate mapping `from`
// onto `to`. nullopt when a rect is empty or non-finite.
std::optional<core::Matrix> userTransformForDisplayRects(const pdf::PdfPageView& view, const core::Rect& from,
                                                         const core::Rect& to);

// Direction of the local +x axis of a user-space matrix as seen on the
// displayed page, in degrees clockwise (screen), in (-180, 180].
double displayAngleDegrees(const pdf::PdfPageView& view, const core::Matrix& userMatrix);

// Direction of the local +x axis in USER space, radians counter-clockwise
// (atan2(b, a)).
double userAngleRadians(const core::Matrix& userMatrix);

// A rigid (rotation + translation) block placement for text that must READ
// UPRIGHT on the displayed page whatever the page's /Rotate: block frame
// (x along the baseline, y up) with its origin at `displayOrigin`.
core::Matrix uprightPlacement(const pdf::PdfPageView& view, core::Point displayOrigin);

// Rotation + translation only (no scale, skew or mirror), within `epsilon`.
bool isRigid(const core::Matrix& m, double epsilon = 1e-6);

// Rotation x one uniform positive scale + translation (no skew, no mirror),
// within a relative `epsilon`.
bool isSimilarity(const core::Matrix& m, double epsilon = 1e-3);

// Whether `p` lies inside the quad (any vertex order, convex) or within
// `tolerance` of its outline.
bool pointInConvexQuad(core::Point p, const std::array<core::Point, 4>& quad, double tolerance);

} // namespace rivet::editor::geometry
