// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfAssembly.hpp"

namespace rivet::pdf {

class PdfiumEngine;

// PdfiumEngine::assembleDocument. Every document in `request` must be a
// PdfiumDocument opened by `engine`. Public entry operation of the adapter:
// acquires the PDFium call gate exactly once (see PdfiumAssembly.cpp), so it
// must never be called while the gate is held - and `sink` must not call
// back into the PDF layer.
core::Status assembleWithPdfium(const PdfiumEngine& engine,
                                const PdfAssemblyRequest& request,
                                IPdfByteSink& sink);

} // namespace rivet::pdf
