#include "PdfiumEngine.h"

#include "PdfiumAssembly.h"
#include "PdfiumCallGate.hpp"
#include "PdfiumDocument.h"
#include "PdfiumFileSource.h"
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

core::Error pdfiumLoadError(int lastError, bool passwordProvided) {
    switch (lastError) {
        case FPDF_ERR_PASSWORD:
            // Distinct code: the app open flow prompts for a password
            // instead of failing. Never include the password itself.
            return core::makeError(core::ErrorCode::PasswordRequired,
                                   std::string("the document is password protected") +
                                       (passwordProvided ? " (the provided password was rejected)"
                                                         : " (no password was provided)"),
                                   "pdf");
        case FPDF_ERR_FILE:
            return core::makeError(core::ErrorCode::Io,
                                   "PDFium could not read the file (FPDF error " +
                                       std::to_string(lastError) + ")",
                                   "pdf");
        case FPDF_ERR_FORMAT:
            return core::makeError(core::ErrorCode::InvalidDocument,
                                   "the file is not a valid PDF document (FPDF error " +
                                       std::to_string(lastError) + ")",
                                   "pdf");
        case FPDF_ERR_SECURITY:
            return core::makeError(core::ErrorCode::InvalidDocument,
                                   "the document uses an unsupported security handler (FPDF error " +
                                       std::to_string(lastError) + ")",
                                   "pdf");
        case FPDF_ERR_SUCCESS:
            return core::makeError(core::ErrorCode::Internal,
                                   "PDFium returned no document without reporting an error", "pdf");
        default:
            return core::makeError(core::ErrorCode::InvalidDocument,
                                   "PDFium failed to open the document (FPDF error " +
                                       std::to_string(lastError) + ")",
                                   "pdf");
    }
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

    // The document reads its bytes through a Rivet-owned open descriptor
    // (see PdfiumFileSource.h) instead of letting PDFium open the path:
    // the live document and every later working copy then share one inode,
    // whatever happens to the path on disk. Unopenable, non-regular and
    // empty files fail here, before PDFium sees them.
    auto source = PdfiumFileSource::open(path);
    if (!source.has_value()) {
        return std::unexpected(source.error());
    }
    const std::string passwordString(password);

    // One gate acquisition for the whole open operation:
    // FPDF_LoadCustomDocument (which reads through the source's callback),
    // FPDF_GetLastError and PdfiumDocument's constructor (which queries page
    // count and metadata) form a single PDFium-serialized step.
    //
    // A document that was opened with a non-empty password required that
    // password, i.e. it is encrypted. PDFium exposes no richer public query,
    // so this open-time observation is the recorded state.
    std::unique_ptr<PdfDocument> document;
    int lastError = 0;
    FPDF_FILEACCESS access = (*source)->fileAccess();
    globalPdfiumCallGate().invoke([&] {
        FPDF_DOCUMENT handle = FPDF_LoadCustomDocument(&access, passwordString.c_str());
        if (handle == nullptr) {
            lastError = static_cast<int>(FPDF_GetLastError());
            return;
        }
        try {
            document = std::make_unique<PdfiumDocument>(handle, !passwordString.empty(), *source,
                                                        passwordString, this);
        } catch (...) {
            FPDF_CloseDocument(handle); // still under the gate
            throw;
        }
    });
    if (document == nullptr) {
        return std::unexpected(pdfiumLoadError(lastError, !passwordString.empty()));
    }

    return document;
}

core::Status PdfiumEngine::assembleDocument(const PdfAssemblyRequest& request, IPdfByteSink& sink) {
    return assembleWithPdfium(*this, request, sink);
}

} // namespace rivet::pdf
