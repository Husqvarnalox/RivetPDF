#pragma once

#include "pdf/PdfSystem.hpp" // also re-exports core::Result into rivet::pdf

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
    // used for unencrypted files.
    core::Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                            std::string_view password) override;
};

} // namespace rivet::pdf
