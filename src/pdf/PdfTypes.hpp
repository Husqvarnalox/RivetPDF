#pragma once

#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <string>

namespace rivet::pdf {

// Document-level metadata reported by a PDF backend.
struct PdfDocumentInfo {
    std::size_t pageCount = 0;
    bool isEncrypted = false;
    // Document title from metadata. May be empty. Rivet never logs document
    // contents or metadata.
    std::string title;
};

// Per-page metadata. sizePoints is the display size AFTER rotation has been
// applied (i.e. the size of the box the page occupies when drawn), while
// rotation records the rotation that was applied.
//
// mediaBox and view describe the page in PDF user space: view is the page's
// NATIVE view (its effective /Rotate and effective crop box, CropBox ∩
// MediaBox), and sizePoints == displaySize(view), rotation == view.rotation.
struct PdfPageInfo {
    std::size_t index = 0;
    core::Size sizePoints;
    core::PageRotation rotation = core::PageRotation::None;
    PdfBox mediaBox;
    PdfPageView view;

    PdfPageInfo() = default;

    // For backends that only know the displayed size (test fakes): the boxes
    // are zero-origin and consistent with the size and rotation, so
    // displaySize(view) == sizePoints still holds.
    PdfPageInfo(std::size_t pageIndex, core::Size displaySizePoints, core::PageRotation pageRotation)
        : index(pageIndex), sizePoints(displaySizePoints), rotation(pageRotation) {
        const bool swapped = pageRotation == core::PageRotation::Clockwise90 ||
                             pageRotation == core::PageRotation::Clockwise270;
        const double userWidth = swapped ? displaySizePoints.height : displaySizePoints.width;
        const double userHeight = swapped ? displaySizePoints.width : displaySizePoints.height;
        mediaBox = PdfBox{0.0, 0.0, userWidth, userHeight};
        view = PdfPageView{pageRotation, mediaBox};
    }
};

} // namespace rivet::pdf
