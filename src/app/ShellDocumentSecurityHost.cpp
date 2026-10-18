// SPDX-License-Identifier: MPL-2.0
#include "app/ShellDocumentSecurityHost.hpp"

#include "app/PrintCoordinator.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"

#include <fstream>
#include <system_error>

namespace rivet::app {

namespace {

// Removes the private temp directory when the capture is over.
struct TempDirectory {
    std::filesystem::path path;
    ~TempDirectory() {
        if (path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

core::Error error(core::ErrorCode code, const char* message) { return core::makeError(code, message, "app"); }

} // namespace

SecurityDocumentInfo ShellDocumentSecurityHost::activeDocument() {
    SecurityDocumentInfo info;
    DocumentTab* tab = context_.readyPdfTab();
    if (tab == nullptr) return info;
    editor::DocumentSession& session = *tab->session();
    info.available = true;
    info.tab = tab->id();
    info.title = tab->title();
    info.path = session.path();
    info.currentPage = tab->currentPage();
    info.pageCount = session.pageCount();
    info.dirty = tab->isDirty();
    info.encrypted = session.info().isEncrypted;
    editor::SignatureService& signatures = session.signatures();
    if (signatures.state() == editor::SignatureService::State::NotLoaded) signatures.ensureLoaded();
    if (signatures.state() == editor::SignatureService::State::Ready) {
        info.signatures = signatures.hasSignatures() ? SecurityDocumentInfo::Signatures::Present
                                                     : SecurityDocumentInfo::Signatures::None;
    }
    return info;
}

std::optional<std::filesystem::path> ShellDocumentSecurityHost::chooseDestination(const std::string& suggestedName,
                                                                                  const std::string& title) {
    if (context_.services.saveDialog == nullptr) {
        if (context_.setStatus) context_.setStatus("Save dialogs are not available on this platform backend");
        return std::nullopt;
    }
    platform::ISaveDialog::Options options;
    options.suggestedName = suggestedName;
    options.title = title;
    options.prompt = "Save";
    return context_.services.saveDialog->runSavePanel(options);
}

core::Result<PlainPdfProducer> ShellDocumentSecurityHost::captureSave(TabId tab) {
    DocumentTab* current = context_.readyPdfTab();
    if (current == nullptr || current->id() != tab) {
        return std::unexpected(error(core::ErrorCode::NotFound, "the document is no longer open"));
    }
    auto directory = createPrivatePrintDirectory();
    if (!directory.has_value()) return std::unexpected(directory.error());
    auto temp = std::make_shared<TempDirectory>();
    temp->path = *directory;
    const std::filesystem::path file = *directory / "capture.pdf";
    core::Result<editor::DocumentWriteJob> job = editor::makeSaveJob(*current->session(), file);
    if (!job.has_value()) return std::unexpected(job.error());
    job->prepareRebase = false;
    auto captured = std::make_shared<editor::DocumentWriteJob>(std::move(*job));
    pdf::PdfEngine* engine = &engine_;
    return PlainPdfProducer([engine, captured, temp, file](const std::function<bool()>& cancelled) -> core::Result<PdfBytes> {
        editor::DocumentWriteControl control;
        control.cancelled = cancelled;
        editor::DocumentWriteResult written = editor::runDocumentWrite(*engine, *captured, control);
        if (!written.written.has_value()) return std::unexpected(written.written.error());
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) return std::unexpected(error(core::ErrorCode::Io, "cannot read the captured document"));
        const std::streamoff size = in.tellg();
        if (size < 0) return std::unexpected(error(core::ErrorCode::Io, "cannot size the captured document"));
        in.seekg(0);
        PdfBytes bytes(static_cast<std::size_t>(size));
        if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
            return std::unexpected(error(core::ErrorCode::Io, "cannot read the captured document"));
        }
        return bytes;
    });
}

void ShellDocumentSecurityHost::commitPendingEdits() { context_.pendingEdits.commitAll(); }

} // namespace rivet::app
