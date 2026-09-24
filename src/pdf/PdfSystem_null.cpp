#include "pdf/PdfSystem.hpp"

#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace rivet::pdf {
namespace {

// Placeholder backend for Rivet builds without PDFium (RIVET_WITH_PDFIUM=OFF).
// It never opens a document: every open reports NotAvailable, so no
// PdfDocument method can ever be reached through this backend.
class NullEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return false; }

    std::string_view backendName() const override { return "none"; }

    core::Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& /*path*/,
                                                            std::string_view /*password*/) override {
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "Rivet was built without PDF support (RIVET_WITH_PDFIUM=OFF)",
                                               "pdf"));
    }
};

} // namespace

std::unique_ptr<PdfEngine> createEngine() {
    return std::make_unique<NullEngine>();
}

} // namespace rivet::pdf
