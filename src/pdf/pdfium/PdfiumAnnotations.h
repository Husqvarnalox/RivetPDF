// SPDX-License-Identifier: MPL-2.0
#pragma once

// PDFium annotation adapter: reading a page's /Annots into Rivet data and
// writing annotation edits into a working document (ADR-0011/0012).
// INTERNAL to src/pdf/pdfium/. Every function here requires the caller to
// hold the PDFium gate and never acquires it.

#include "core/Error.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <cstddef>

#include "fpdf_annot.h"
#include "fpdfview.h"

namespace rivet::pdf::internal {

// RAII wrapper for FPDF_ANNOTATION handles (FPDFPage_GetAnnot /
// FPDFPage_CreateAnnot / FPDFPage_CloseAnnot). Must be closed while the page
// it came from is still open.
class ScopedAnnot {
public:
    explicit ScopedAnnot(FPDF_ANNOTATION annot = nullptr) : annot_(annot) {}
    ~ScopedAnnot() {
        if (annot_ != nullptr) {
            FPDFPage_CloseAnnot(annot_);
        }
    }
    ScopedAnnot(const ScopedAnnot&) = delete;
    ScopedAnnot& operator=(const ScopedAnnot&) = delete;

    FPDF_ANNOTATION get() const { return annot_; }

private:
    FPDF_ANNOTATION annot_;
};

// Reads page `pageIndex` of `reader` into Rivet data.
//
// `reader` MUST be a private document that is never rendered: reading is
// destructive for dictionaries (to read an annotation's colors through the
// public API its appearance stream is removed first), and PDFium itself
// mutates live dictionaries when rendering (generated APs, Text /Rect forced
// to 20x20, /F toggles). A second read of the same page of the same reader
// would therefore not see the original file, so callers cache the result.
core::Result<PdfPageAnnotations> readPageAnnotations(FPDF_DOCUMENT reader, std::size_t pageIndex);

// Applies `edits` to `page` of `working` (removals descending, then the
// creations appended in order, see the creation recipe in
// PdfiumAnnotationWriter.cpp) and fills `report` (createdIndices, annotsCount).
// `edits` must be validated: removeIndices ascending/unique and in range,
// every create entry writable.
core::Status applyAnnotationEdits(FPDF_DOCUMENT working,
                                  FPDF_PAGE page,
                                  const PdfPageAnnotationEdits& edits,
                                  PdfAssembledPageAnnotations& report);

} // namespace rivet::pdf::internal
