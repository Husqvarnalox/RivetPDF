#include "PdfiumEngine.h"

#include "PdfiumDocument.h"
#include "PdfiumLibrary.h"
#include "core/Log.hpp"

#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "fpdfview.h"

namespace rivet::pdf {

bool PdfiumEngine::isAvailable() const {
    return true;
}

std::string_view PdfiumEngine::backendName() const {
    return "pdfium";
}

core::Result<std::unique_ptr<PdfDocument>> PdfiumEngine::openDocument(const std::filesystem::path& path,
                                                                      std::string_view password) {
    // Only the file path is ever logged, never document contents or metadata.
    std::error_code existsError;
    if (path.empty() || !std::filesystem::exists(path, existsError) || existsError) {
        return std::unexpected(core::makeError(core::ErrorCode::NotFound,
                                               "PDF file not found: " + path.string(), "pdf"));
    }

    core::log::debug("pdf: opening document: " + path.string());

    PdfiumLibrary::instance(); // ensure the library is initialized

    const std::string passwordString(password);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.string().c_str(), passwordString.c_str());
    if (document == nullptr) {
        const int lastError = static_cast<int>(FPDF_GetLastError());
        switch (lastError) {
            case FPDF_ERR_PASSWORD:
                return std::unexpected(core::makeError(
                    core::ErrorCode::InvalidDocument,
                    std::string("PDFium could not open the document: password required or incorrect") +
                        (passwordString.empty() ? " (no password was provided)"
                                                : " (the provided password was rejected)"),
                    "pdf"));
            case FPDF_ERR_FILE:
                return std::unexpected(core::makeError(
                    core::ErrorCode::Io,
                    "PDFium could not read the file (FPDF error " + std::to_string(lastError) + ")",
                    "pdf"));
            case FPDF_ERR_FORMAT:
                return std::unexpected(core::makeError(
                    core::ErrorCode::InvalidDocument,
                    "the file is not a valid PDF document (FPDF error " + std::to_string(lastError) + ")",
                    "pdf"));
            case FPDF_ERR_SECURITY:
                return std::unexpected(core::makeError(
                    core::ErrorCode::InvalidDocument,
                    "the document uses an unsupported security handler (FPDF error " +
                        std::to_string(lastError) + ")",
                    "pdf"));
            case FPDF_ERR_SUCCESS:
                return std::unexpected(core::makeError(
                    core::ErrorCode::Internal,
                    "PDFium returned no document without reporting an error", "pdf"));
            default:
                return std::unexpected(core::makeError(
                    core::ErrorCode::InvalidDocument,
                    "PDFium failed to open the document (FPDF error " + std::to_string(lastError) + ")",
                    "pdf"));
        }
    }

    // A document that was opened with a non-empty password required that
    // password, i.e. it is encrypted. PDFium exposes no richer public query,
    // so this open-time observation is the recorded state.
    auto pdfiumDocument = std::make_unique<PdfiumDocument>(document, !passwordString.empty());
    return std::unique_ptr<PdfDocument>(std::move(pdfiumDocument));
}

} // namespace rivet::pdf
