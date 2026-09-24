#pragma once

#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"

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
struct PdfPageInfo {
    std::size_t index = 0;
    core::Size sizePoints;
    core::PageRotation rotation = core::PageRotation::None;
};

} // namespace rivet::pdf
