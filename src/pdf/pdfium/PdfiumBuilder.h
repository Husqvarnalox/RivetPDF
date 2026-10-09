// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfBuild.hpp"

namespace rivet::pdf {

// PdfiumEngine::buildDocument. Validates `build` (outside the gate), then
// acquires the PDFium call gate exactly once for the whole build and save
// (see PdfiumBuilder.cpp), so it must never be called while the gate is held
// - and `sink` must not call back into the PDF layer.
core::Status buildWithPdfium(const PdfBuildDocument& build, IPdfByteSink& sink);

} // namespace rivet::pdf
