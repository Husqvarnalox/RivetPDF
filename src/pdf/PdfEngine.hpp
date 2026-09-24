#pragma once

#include "core/Error.hpp"
#include "core/Bitmap.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfTypes.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace rivet::pdf {

// An open PDF document. Owned handle wrapper: implementing classes keep the
// engine-specific document handle alive and release it deterministically on
// destruction. No engine-specific types may appear in this interface.
class PdfDocument {
public:
    virtual ~PdfDocument() = default;

    PdfDocument() = default;
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    virtual const PdfDocumentInfo& info() const = 0;

    // Page metadata for the given zero-based page index.
    virtual Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const = 0;

    // Rasterizes a sub-rectangle of a page.
    //
    // pageRectPoints is expressed in rotated page display coordinates
    // (points, origin at the top-left of the displayed page, y-down) and must
    // be contained in the page bounds. devicePixelsPerPoint is the physical
    // device pixel scale (zoom * display backing scale). The output bitmap
    // covers exactly pageRectPoints at that scale.
    virtual Result<core::Bitmap> renderPage(std::size_t pageIndex,
                                            const core::Rect& pageRectPoints,
                                            double devicePixelsPerPoint) = 0;
};

// A PDF backend. Rivet keeps the concrete engine (PDFium) behind this
// interface; subsystems above rivet_pdf never see engine types.
class PdfEngine {
public:
    virtual ~PdfEngine() = default;

    // Whether this build actually contains a working backend. When Rivet is
    // built with RIVET_WITH_PDFIUM=OFF this returns false and every other
    // operation reports NotAvailable.
    virtual bool isAvailable() const = 0;

    virtual std::string_view backendName() const = 0;

    // Opens a document. An empty password is used for unencrypted files.
    virtual Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                              std::string_view password = {}) = 0;
};

} // namespace rivet::pdf
