#pragma once

#include "pdf/PdfSystem.hpp" // also re-exports core::Result into rivet::pdf

#include "core/Error.hpp"
#include "pdf/PdfAssembly.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace rivet::pdf {

// PDFium-backed PdfEngine. Constructed only by createEngine()
// (PdfSystem_pdfium.cpp); one process-wide instance is expected.
class PdfiumEngine final : public PdfEngine {
public:
    PdfiumEngine() = default;

    bool isAvailable() const override;

    std::string_view backendName() const override;

    // Opens a document, checking the path first and mapping PDFium's last
    // error onto Rivet's ErrorCode space on failure. An empty password is
    // used for unencrypted files. The document is loaded through a shared,
    // read-only PdfiumFileSource (FPDF_LoadCustomDocument), which it retains
    // together with the password so a save can reopen the same bytes.
    core::Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                            std::string_view password) override;

    // See PdfEngine::assembleDocument and PdfiumAssembly.cpp.
    core::Status assembleDocument(const PdfAssemblyRequest& request, IPdfByteSink& sink) override;
};

// Maps FPDF_GetLastError() after a failed document load onto a Rivet error.
// passwordProvided only shapes the message; the password itself is never
// included. Adapter-internal (shared by openDocument and the assembly).
core::Error pdfiumLoadError(int lastError, bool passwordProvided);

} // namespace rivet::pdf
