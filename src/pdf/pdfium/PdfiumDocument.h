#pragma once

#include "pdf/PdfSystem.hpp" // also re-exports core::Result into rivet::pdf

#include "core/Bitmap.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// PDFium public headers. Allowed here only: this file lives inside
// src/pdf/pdfium/, the single directory where FPDF_* types may appear.
#include "fpdfview.h"

namespace rivet::pdf {

class PdfiumEngine;
class PdfiumFileSource;
namespace internal {
class ContentState;
}

// PDFium-backed PdfDocument. Constructed only by the PDFium backend
// (PdfiumEngine::openDocument): it takes ownership of a document handle
// returned by FPDF_LoadCustomDocument and closes it deterministically on
// destruction. All page/bitmap/text-page handles are RAII-wrapped internally.
//
// PDFium call-gate discipline (PdfiumCallGate): every public entry operation
// acquires the process-wide gate exactly once. The constructor is the tail of
// PdfiumEngine::openDocument's single acquisition, so it does not acquire;
// the destructor, pageInfo(), renderPage() and textPage() acquire themselves
// and must therefore never be called while the gate is held.
//
// The document never mutates its FPDF_DOCUMENT: page editing works on a
// Rivet page model presented through PdfPageViews, and saving reopens a
// private working copy from the SAME file source (see PdfiumAssembly.cpp).
class PdfiumDocument final : public PdfDocument {
public:
    // Internal to the pdfium backend: `document` must be a valid
    // FPDF_DOCUMENT loaded from `source` with `password`. isEncrypted
    // records whether a password was required to open the document; `owner`
    // identifies the engine that opened it (assembly requests may only mix
    // documents of one engine). Caller must hold the PDFium gate: this runs
    // FPDF_GetPageCount and FPDF_GetMetaText.
    PdfiumDocument(FPDF_DOCUMENT document,
                   bool isEncrypted,
                   std::shared_ptr<PdfiumFileSource> source,
                   std::string password,
                   const PdfiumEngine* owner);

    ~PdfiumDocument() override;

    PdfiumDocument(const PdfiumDocument&) = delete;
    PdfiumDocument& operator=(const PdfiumDocument&) = delete;

    // The view-aware overloads of the base class stay visible next to the
    // overrides below.
    using PdfDocument::pageLinks;
    using PdfDocument::renderPage;
    using PdfDocument::textPage;

    const PdfDocumentInfo& info() const override;

    core::Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override;

    core::Result<core::Bitmap> renderPage(std::size_t pageIndex,
                                          const core::Rect& pageRectPoints,
                                          double devicePixelsPerPoint) override;

    core::Result<std::optional<PdfOutlineNode>> outline() const override;

    core::Result<std::string> pageLabel(std::size_t pageIndex) const override;

    core::Result<std::vector<PdfPageLink>> pageLinks(std::size_t pageIndex) const override;

    // Read from a private reader document (see below), never from the live
    // handle; the result per page is cached.
    core::Result<PdfPageAnnotationsPtr> annotations(std::size_t pageIndex) const override;

    core::Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t pageIndex) const override;

    core::Result<PdfPageContentPtr> pageContent(std::size_t pageIndex,
                                                const PdfPageContentEditsPtr& edits) const override;

    // Adapter-internal accessors for the assembly (PdfiumAssembly.cpp), which
    // runs inside its own gate acquisition. Nothing here is reachable through
    // the PdfDocument interface.
    FPDF_DOCUMENT handle() const { return document_; }
    const PdfiumEngine* owner() const { return owner_; }
    const std::shared_ptr<PdfiumFileSource>& fileSource() const { return source_; }
    const std::string& openPassword() const { return password_; }

    // Ok when page `pageIndex` passed the regeneration fidelity probe
    // (ADR-0017), InvalidArgument (with the probe's reason) when its content
    // streams cannot be rewritten without loss. Runs the extraction and the
    // probe on first use (cached). CALLER HOLDS THE GATE; never acquires it.
    core::Status requireContentEditable(std::size_t pageIndex) const;

protected:
    core::Result<core::Bitmap> renderPageInView(std::size_t pageIndex,
                                                const PdfPageView& view,
                                                std::span<const std::uint32_t> hiddenAnnotations,
                                                const PdfPageContentEditsPtr& content,
                                                const core::Rect& pageRectPoints,
                                                double devicePixelsPerPoint) override;

    core::Result<std::shared_ptr<const PdfTextPage>> textPageInView(
        std::size_t pageIndex, const PdfPageView& view, const PdfPageContentEditsPtr& content) const override;

    core::Result<std::vector<PdfPageLink>> pageLinksInView(std::size_t pageIndex,
                                                           const PdfPageView& view) const override;

private:
    // The single code path behind both renderPage flavors (and likewise for
    // text and links): a null view means the page's native view. Each
    // acquires the gate exactly once.
    core::Result<core::Bitmap> renderPageImpl(std::size_t pageIndex,
                                              const PdfPageView* view,
                                              std::span<const std::uint32_t> hiddenAnnotations,
                                              const PdfPageContentEditsPtr& content,
                                              const core::Rect& pageRectPoints,
                                              double devicePixelsPerPoint);
    core::Result<std::shared_ptr<const PdfTextPage>> textPageImpl(std::size_t pageIndex,
                                                                  const PdfPageView* view,
                                                                  const PdfPageContentEditsPtr& content) const;
    core::Result<std::vector<PdfPageLink>> pageLinksImpl(std::size_t pageIndex,
                                                         const PdfPageView* view) const;

    // The private, never-rendered reader document content extraction and
    // materialization work from (created lazily, separate from the
    // destructive annotation reader). Caller holds the gate.
    core::Result<FPDF_DOCUMENT> contentReader() const;

    // Reads a UTF-16LE metadata string (e.g. "Title") and converts it to
    // UTF-8. Empty when the key is missing. Caller must hold the PDFium gate
    // (this runs FPDF_GetMetaText); never acquires it.
    std::string metaText(const char* tag) const;

    FPDF_DOCUMENT document_ = nullptr;
    PdfDocumentInfo info_;
    // The bytes document_ was loaded from; must outlive document_ (PDFium
    // reads lazily), which the destructor guarantees by closing document_
    // in its body, before members are destroyed.
    std::shared_ptr<PdfiumFileSource> source_;
    // The password the document was opened with, retained so an assembly
    // can reopen a working copy of the same bytes (PDFium offers no way to
    // clone a loaded document or to reuse its decryption key). Trade-off:
    // the secret stays in process memory for the document's lifetime. It is
    // never logged, never part of any error message, never exposed through
    // PdfDocument, and it is wiped on destruction.
    std::string password_;
    const PdfiumEngine* owner_ = nullptr;

    // Annotation reading (annotations()). PDFium mutates the dictionaries of
    // a live document when it renders (generated appearance streams, Text
    // /Rect forced to 20x20, /F toggles) and reading colors through the
    // public API requires removing the appearance stream, so annotations
    // are read from a SEPARATE document loaded over the same file source,
    // which is never rendered. Created lazily; created, used and closed only
    // under the PDFium gate (the destructor closes it there), which also
    // serializes access to the mutable members. Reading destroys what it
    // reads, so a page is never read twice from the same reader instance:
    // results are cached (bounded FIFO, kAnnotationCacheCapacity pages) and
    // a page that was read before but has been evicted is read from a
    // freshly loaded reader (cheap: PDFium parses lazily).
    static constexpr std::size_t kAnnotationCacheCapacity = 64;
    // Content editing (Phase 5): the content reader, the per-page source
    // content + probe results and the materialized edited pages. Only touched
    // under the gate; closed by the destructor under the gate.
    mutable std::unique_ptr<internal::ContentState> content_;

    mutable FPDF_DOCUMENT annotationReader_ = nullptr;
    mutable std::unordered_map<std::size_t, PdfPageAnnotationsPtr> annotationCache_;
    mutable std::deque<std::size_t> annotationCacheOrder_;      // insertion order
    mutable std::unordered_set<std::size_t> annotationReaderReadPages_; // read from the current reader
};

} // namespace rivet::pdf
