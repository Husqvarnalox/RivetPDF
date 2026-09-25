// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"

#include <utility>

namespace rivet::pdf {

// An axis-aligned box in PDF USER space (points, origin bottom-left, y-up),
// the space /MediaBox and /CropBox are written in. Unlike core::Rect (origin
// + size in a y-down display space) the fields are the four PDF edges, so
// top is the LARGER y.
struct PdfBox {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;

    constexpr double width() const { return right - left; }
    constexpr double height() const { return top - bottom; }

    // Finite edges with a strictly positive width and height.
    bool isValid() const;

    // Whether `inner` lies inside this box, allowing `epsilon` points of
    // slack on every edge (box edges usually round-trip through floats).
    bool contains(const PdfBox& inner, double epsilon = 0.0) const;

    // Overlap of the two boxes; an invalid (zero-area) box when they do not
    // overlap.
    PdfBox intersection(const PdfBox& other) const;

    constexpr bool operator==(const PdfBox&) const = default;
};

// How a page is presented: the absolute effective /Rotate plus the visible
// box in user space (the effective crop box, i.e. CropBox ∩ MediaBox). A
// page's NATIVE view is what its dictionary says; the editor's page model
// can present a page through any other view (rotate/crop without touching
// the document), and saving materializes the view into /Rotate + /CropBox.
struct PdfPageView {
    core::PageRotation rotation = core::PageRotation::None;
    PdfBox cropBox;

    constexpr bool operator==(const PdfPageView&) const = default;
};

// Pure view <-> display-space mapping. Display space is the one renderPage,
// the text APIs and links use (docs/ARCHITECTURE.md section 4): points,
// origin at the top-left of the DISPLAYED page, y-down, rotation clockwise.
// For a user point (x, y) with dx0 = x - crop.left, dy0 = y - crop.bottom
// and crop size (w0, h0):
//   None:         displayX = dx0,       displayY = h0 - dy0
//   Clockwise90:  displayX = dy0,       displayY = dx0
//   Clockwise180: displayX = w0 - dx0,  displayY = dy0
//   Clockwise270: displayX = h0 - dy0,  displayY = w0 - dx0
// This is exactly the display matrix PDFium builds for rendering
// (CPDF_Page::UpdateDimensions' page_matrix_ composed with the y-flip of
// GetDisplayMatrixForFloatRect) and the convention PageTransform implements.
// A view with an invalid crop box yields meaningless (possibly non-finite)
// results; callers validate first.

// Displayed size of the view: the crop box size, width/height swapped for
// odd quarter turns.
core::Size displaySize(const PdfPageView& view);

core::Point userToDisplay(const PdfPageView& view, double x, double y);

// Exact inverse of userToDisplay: returns the user-space (x, y).
std::pair<double, double> displayToUser(const PdfPageView& view, core::Point displayPoint);

// userToDisplay as an affine matrix (core::Matrix convention: x' = a*x +
// c*y + tx, y' = b*x + d*y + ty). Used to compose render transforms.
core::Matrix userToDisplayMatrix(const PdfPageView& view);

// Maps a user-space box to display space (two opposite corners, normalized)
// and clamps the result into the displayed page bounds. Invalid boxes yield
// an empty rect; boxes entirely outside the view clamp to a zero-area rect on
// the nearest edge.
core::Rect userBoxToDisplayRect(const PdfPageView& view, const PdfBox& box);

// Maps a display-space rect back to user space (no clamping). An empty rect
// maps to a zero-area (invalid) box; a non-finite rect to a non-finite box.
PdfBox displayRectToUserBox(const PdfPageView& view, const core::Rect& rect);

} // namespace rivet::pdf
