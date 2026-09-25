// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "core/Error.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <memory>

// PDFium public headers. Allowed here only: this file lives inside
// src/pdf/pdfium/, the single directory where FPDF_* types may appear.
#include "fpdfview.h"

namespace rivet::pdf {

// Extracts the text of the given page as detached, Rivet-owned plain data
// (a PdfTextPage holds no engine handles, so it may outlive this call and be
// cached freely). All PDFium calls (page load, text load, per-char queries,
// rect queries, and the RAII handle closes) run inside the CALLER's single
// acquisition of the global PDFium call gate: this function NEVER acquires
// the gate itself and must only be called from inside globalPdfiumCallGate()
// .invoke() (see PdfiumCallGate.hpp - a nested acquisition self-deadlocks).
//
// Char bounds are reported in the display space of `view` (null = the
// page's native view); a view that is invalid or exceeds the media box is
// rejected with InvalidArgument.
//
// pageCount bounds the range check on pageIndex (InvalidArgument on overflow);
// a page or text page that PDFium fails to load reports InvalidDocument with
// FPDF_GetLastError(). Never logs document contents.
core::Result<std::shared_ptr<const PdfTextPage>> extractTextPage(FPDF_DOCUMENT document,
                                                                 std::size_t pageIndex,
                                                                 std::size_t pageCount,
                                                                 const PdfPageView* view = nullptr);

} // namespace rivet::pdf
