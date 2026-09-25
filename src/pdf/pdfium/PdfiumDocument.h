#pragma once

#include "pdf/PdfSystem.hpp" // also re-exports core::Result into rivet::pdf

#include "core/Bitmap.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfTypes.hpp"

#include <cstddef>
#include <string>

// PDFium public headers. Allowed here only: this file lives inside
// src/pdf/pdfium/, the single directory where FPDF_* types may appear.
#include "fpdfview.h"

namespace rivet::pdf {

// PDFium-backed PdfDocument. Constructed only by the PDFium backend
// (PdfiumEngine::openDocument): it takes ownership of a document handle
// returned by FPDF_LoadDocument and closes it deterministically on
// destruction. All page/bitmap handles are RAII-wrapped internally.
//
// PDFium call-gate discipline (PdfiumCallGate): every public entry operation
// acquires the process-wide gate exactly once. The constructor is the tail of
// PdfiumEngine::openDocument's single acquisition, so it does not acquire;
// the destructor, pageInfo() and renderPage() acquire themselves and must
// therefore never be called while the gate is held.
class PdfiumDocument final : public PdfDocument {
public:
    // Internal to the pdfium backend: `document` must be a valid
    // FPDF_DOCUMENT. isEncrypted records whether a password was required to
    // open the document. Caller must hold the PDFium gate: this runs
    // FPDF_GetPageCount and FPDF_GetMetaText.
    PdfiumDocument(FPDF_DOCUMENT document, bool isEncrypted);

    ~PdfiumDocument() override;

    PdfiumDocument(const PdfiumDocument&) = delete;
    PdfiumDocument& operator=(const PdfiumDocument&) = delete;

    const PdfDocumentInfo& info() const override;

    core::Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override;

    core::Result<core::Bitmap> renderPage(std::size_t pageIndex,
                                          const core::Rect& pageRectPoints,
                                          double devicePixelsPerPoint) override;

private:
    // Reads a UTF-16LE metadata string (e.g. "Title") and converts it to
    // UTF-8. Empty when the key is missing. Caller must hold the PDFium gate
    // (this runs FPDF_GetMetaText); never acquires it.
    std::string metaText(const char* tag) const;

    FPDF_DOCUMENT document_ = nullptr;
    PdfDocumentInfo info_;
};

} // namespace rivet::pdf
