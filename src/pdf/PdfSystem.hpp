#pragma once

#include "pdf/PdfEngine.hpp"

#include <memory>

namespace rivet::pdf {

// Process-wide engine factory. Without PDFium (RIVET_WITH_PDFIUM=OFF) this
// returns a null backend: isAvailable()==false, openDocument reports
// NotAvailable. With PDFium it returns the PDFium-backed engine.
std::unique_ptr<PdfEngine> createEngine();

} // namespace rivet::pdf
