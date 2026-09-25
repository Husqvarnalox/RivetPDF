#pragma once

#include "core/Error.hpp"
#include "core/Bitmap.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    virtual core::Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const = 0;

    // Rasterizes a sub-rectangle of a page.
    //
    // pageRectPoints is expressed in rotated page display coordinates
    // (points, origin at the top-left of the displayed page, y-down) and must
    // be contained in the page bounds. devicePixelsPerPoint is the physical
    // device pixel scale (zoom * display backing scale). The output bitmap
    // covers exactly pageRectPoints at that scale.
    virtual core::Result<core::Bitmap> renderPage(std::size_t pageIndex,
                                                  const core::Rect& pageRectPoints,
                                                  double devicePixelsPerPoint) = 0;

    // View-aware reads. The page is presented through `view` (absolute
    // rotation + crop box in the page's user space, see PdfPageGeometry.hpp)
    // instead of its native /Rotate and crop box; every rect/point in and
    // out is in the display space OF THE VIEW. The classic overloads are the
    // native view. A view whose crop box is invalid or not within the page's
    // media box is rejected with InvalidArgument. Link destinations keep
    // their native display point plus the user-space point (a destination
    // targets another page, whose view the caller maps itself).
    //
    // These are non-virtual and dispatch to the protected *InView hooks
    // below, so that backends and test fakes overriding only the classic
    // overloads do not hide them (GCC's -Woverloaded-virtual, part of -Wall,
    // flags a derived override that hides a virtual overload).
    core::Result<core::Bitmap> renderPage(std::size_t pageIndex,
                                          const PdfPageView& view,
                                          const core::Rect& pageRectPoints,
                                          double devicePixelsPerPoint) {
        return renderPageInView(pageIndex, view, pageRectPoints, devicePixelsPerPoint);
    }

    core::Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t pageIndex,
                                                              const PdfPageView& view) const {
        return textPageInView(pageIndex, view);
    }

    core::Result<std::vector<PdfPageLink>> pageLinks(std::size_t pageIndex,
                                                     const PdfPageView& view) const {
        return pageLinksInView(pageIndex, view);
    }

    // Document outline ("bookmarks") as a Rivet-owned tree; the synthetic
    // root wraps the top-level items as children. std::nullopt = the document
    // has no outline (not an error). The tree is depth/node-bounded and
    // cycle-safe at extraction time. Default: NotAvailable.
    virtual core::Result<std::optional<PdfOutlineNode>> outline() const {
        (void)0;
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "this backend has no outline support", "pdf"));
    }

    // Label of a page per the PDF page-label tree ("i", "A-1", ...), or an
    // empty string when the document defines no labels. Default: NotAvailable.
    virtual core::Result<std::string> pageLabel(std::size_t pageIndex) const {
        (void)pageIndex;
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "this backend has no page label support", "pdf"));
    }

    // All links of a page. Default: NotAvailable.
    virtual core::Result<std::vector<PdfPageLink>> pageLinks(std::size_t pageIndex) const {
        (void)pageIndex;
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "this backend has no link support", "pdf"));
    }

    // Extracts the text of a page. The returned PdfTextPage is immutable,
    // Rivet-owned data with no engine handles, so it may outlive any call and
    // be cached freely. Default: NotAvailable (backends without text support).
    virtual core::Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t pageIndex) const {
        (void)pageIndex;
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "this backend has no text support", "pdf"));
    }

protected:
    // Hooks behind the view-aware overloads. The defaults IGNORE the view and
    // forward to the native-view overloads: that is test-backend behavior
    // (fakes that never present pages through a different view). Real
    // backends override all three.
    virtual core::Result<core::Bitmap> renderPageInView(std::size_t pageIndex,
                                                        const PdfPageView& view,
                                                        const core::Rect& pageRectPoints,
                                                        double devicePixelsPerPoint) {
        (void)view;
        return renderPage(pageIndex, pageRectPoints, devicePixelsPerPoint);
    }

    virtual core::Result<std::shared_ptr<const PdfTextPage>> textPageInView(std::size_t pageIndex,
                                                                            const PdfPageView& view) const {
        (void)view;
        return textPage(pageIndex);
    }

    virtual core::Result<std::vector<PdfPageLink>> pageLinksInView(std::size_t pageIndex,
                                                                   const PdfPageView& view) const {
        (void)view;
        return pageLinks(pageIndex);
    }
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
    virtual core::Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                                    std::string_view password = {}) = 0;

    // Materializes a page list (see PdfAssemblyRequest) into a new PDF
    // streamed to `sink`. The documents referenced by the request are only
    // READ: their live state (page tree, handles) is never modified, so the
    // editor keeps presenting them while a save runs. Every document pointer
    // must be a document opened by THIS engine (InvalidArgument otherwise).
    // On error the bytes already handed to the sink are unspecified and must
    // be discarded; success means the sink received one complete document.
    //
    // The sink must not call back into the engine or any document (backends
    // may hold internal locks while writing). Default: NotAvailable (null
    // engine, test fakes).
    virtual core::Status assembleDocument(const PdfAssemblyRequest& request, IPdfByteSink& sink) {
        (void)request;
        (void)sink;
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "this backend cannot write documents", "pdf"));
    }
};

} // namespace rivet::pdf
