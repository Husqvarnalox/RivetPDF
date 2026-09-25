// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Size.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rivet::platform {

// Renders one page for printing. The callback is the RIVET render pipeline
// (PdfDocument::renderPage over the whole page) - printing never uses an
// alternate PDF engine. pageRectPoints is the whole displayed page; the
// returned bitmap must cover exactly that region at devicePixelsPerPoint.
using PrintPageRenderer =
    std::function<core::Result<core::Bitmap>(std::size_t pageIndex, double devicePixelsPerPoint)>;

// Everything the platform print implementation needs. Page geometry comes
// from the document layout; content always comes from the renderer callback.
struct PrintRequest {
    std::string jobTitle;
    std::vector<core::Size> pageSizesPoints; // display size per page
    PrintPageRenderer renderPage;
};

// Platform print service. printDocument() shows the native print dialog and
// runs the print operation (or reports Cancelled). The AppKit implementation
// uses the standard printing infrastructure for dialog/page-range/paper
// handling ONLY - page content is acquired exclusively through the Rivet
// render callback above (no PDFKit, no alternate engine).
class IPrintService {
public:
    virtual ~IPrintService() = default;

    virtual core::Status printDocument(const PrintRequest& request) = 0;
};

} // namespace rivet::platform
