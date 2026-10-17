// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownExportController.hpp"

#include "core/Error.hpp"
#include "core/io/AtomicFileWriter.hpp"
#include "pdf/PdfAssembly.hpp"

#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace rivet::app {

namespace {

core::Error cancelledError() { return core::makeError(core::ErrorCode::Cancelled, "cancelled", "app"); }

// Buffers the builder's small chunks into large writes to the atomic
// writer and polls cancellation before each one. Never calls back into the
// engine (IPdfByteSink contract).
class ForwardingSink final : public pdf::IPdfByteSink {
public:
    static constexpr std::size_t kBufferSize = 256 * 1024;

    ForwardingSink(core::io::IByteSink& out, std::function<bool()> cancelled)
        : out_(out), cancelled_(std::move(cancelled)) {
        buffer_.reserve(kBufferSize);
    }

    core::Status write(const void* data, std::size_t size) override {
        if (size == 0) return core::ok();
        if (data == nullptr) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "null output chunk", "app"));
        }
        if (buffer_.size() + size > kBufferSize) {
            if (core::Status flushed = flush(); !flushed) return flushed;
        }
        if (size >= kBufferSize) return forward(data, size);
        const auto* bytes = static_cast<const char*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        return core::ok();
    }

    core::Status flush() {
        if (buffer_.empty()) return core::ok();
        core::Status status = forward(buffer_.data(), buffer_.size());
        buffer_.clear();
        return status;
    }

private:
    core::Status forward(const void* data, std::size_t size) {
        if (cancelled_()) return std::unexpected(cancelledError());
        return out_.write(data, size);
    }

    core::io::IByteSink& out_;
    std::function<bool()> cancelled_;
    std::vector<char> buffer_;
};

// Builds the document and writes it atomically to `destination`.
core::Status writeBuild(pdf::PdfEngine& engine, const pdf::PdfBuildDocument& build,
                        const std::filesystem::path& destination, const std::function<bool()>& cancelled) {
    return core::io::writeFileAtomically(destination, [&](core::io::IByteSink& out) -> core::Status {
        ForwardingSink sink(out, cancelled);
        core::Status status = engine.buildDocument(build, sink);
        if (status.has_value()) status = sink.flush();
        if (status.has_value() && cancelled()) status = std::unexpected(cancelledError());
        return status;
    });
}

} // namespace

// A printout's temporary PDF: the opened document and its private
// directory. The document closes before the directory is removed.
struct PrintTempDocument {
    std::filesystem::path directory;
    std::unique_ptr<pdf::PdfDocument> document;

    PrintTempDocument() = default;
    PrintTempDocument(const PrintTempDocument&) = delete;
    PrintTempDocument& operator=(const PrintTempDocument&) = delete;
    ~PrintTempDocument() {
        document.reset();
        if (!directory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
};

// Hand-over slot between the print worker and its completion. The
// controller's destructor empties it after the workers are done, so a
// completion that never runs cannot keep a document past the engine.
struct MarkdownExportController::PrintSlot {
    std::mutex mutex;
    std::shared_ptr<PrintTempDocument> pending;
};

namespace {

std::string exportSummary(const MarkdownExportResult& built, const std::filesystem::path& destination) {
    std::string text = std::format("Exported {} {} to {}", built.pageCount, built.pageCount == 1 ? "page" : "pages",
                                   destination.filename().string());
    if (built.replacedCharacters > 0) {
        text += std::format(", {} {} outside the bundled fonts were written as '?'", built.replacedCharacters,
                            built.replacedCharacters == 1 ? "character" : "characters");
    }
    if (built.skippedImages > 0) {
        text += std::format(", {} {} skipped", built.skippedImages, built.skippedImages == 1 ? "image" : "images");
    }
    return text;
}

} // namespace

MarkdownExportController::MarkdownExportController(pdf::PdfEngine& engine, ShellContext& context,
                                                   core::TaskScheduler& scheduler, PrintCoordinator& printer,
                                                   StatusSink setStatus)
    : engine_(engine), context_(context), scheduler_(scheduler), printer_(printer),
      setStatus_(std::move(setStatus)), alive_(std::make_shared<std::atomic<bool>>(true)) {}

MarkdownExportController::~MarkdownExportController() {
    *alive_ = false;
    if (printCancel_ != nullptr) printCancel_->store(true);
    scope_.closeAndWait();
    if (printSlot_ != nullptr) {
        std::lock_guard<std::mutex> lock(printSlot_->mutex);
        printSlot_->pending.reset();
    }
}

MarkdownExportController::WorkerToken MarkdownExportController::enterWorkerScope() {
    std::optional<core::AsyncScope::Token> token = scope_.enter();
    if (!token.has_value()) return nullptr;
    return std::make_shared<core::AsyncScope::Token>(std::move(*token));
}

ExportImageMap MarkdownExportController::snapshotImages(const DocumentTab& tab) const {
    return images_ ? images_(tab) : ExportImageMap{};
}

void MarkdownExportController::deliver(std::function<void()> task) {
    // Workers only start when a dispatcher exists (checked by the callers).
    context_.services.mainDispatcher->post([alive = alive_, task = std::move(task)] {
        if (*alive) task();
    });
}

bool MarkdownExportController::canExport(const DocumentTab& tab) const {
    return tab.isMarkdown() && tab.state() == DocumentTab::State::Ready && tab.markdown() != nullptr &&
           engine_.isAvailable();
}

// --- Export as PDF ---------------------------------------------------------------

void MarkdownExportController::exportToPdf(DocumentTab& tab) {
    if (!tab.isMarkdown() || tab.state() != DocumentTab::State::Ready || tab.markdown() == nullptr) return;
    if (!engine_.isAvailable()) {
        setStatus_("Exporting Markdown requires the PDF backend");
        return;
    }
    if (exporting_) {
        setStatus_("An export is already running");
        return;
    }
    if (context_.services.saveDialog == nullptr || context_.services.mainDispatcher == nullptr) {
        setStatus_("File dialogs are not available on this platform backend");
        return;
    }
    context_.pendingEdits.commitAll();
    const TabId tabId = tab.id();
    platform::ISaveDialog::Options options;
    const std::filesystem::path stem = tab.path().stem();
    options.suggestedName = (stem.empty() ? std::string("Untitled") : stem.string()) + ".pdf";
    options.allowedExtensions = {"pdf"};
    options.title = "Export as PDF";
    options.prompt = "Export";
    std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return; // cancelled: nothing changes
    // The panel is modal and may have run completions (closing the tab, or
    // finishing another export).
    DocumentTab* current = context_.workspace.tabById(tabId);
    if (current == nullptr || current->state() != DocumentTab::State::Ready || current->markdown() == nullptr) {
        setStatus_("Export cancelled: the document was closed");
        return;
    }
    if (exporting_) {
        setStatus_("An export is already running");
        return;
    }
    if (chosen->extension().empty()) chosen->replace_extension(".pdf");

    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    exporting_ = true;
    setStatus_("Exporting…");
    auto result = std::make_shared<std::optional<core::Result<MarkdownExportResult>>>();
    auto written = std::make_shared<core::Status>();
    scheduler_.post([this, token = std::move(token), result, written, source = current->markdown()->source(),
                     images = snapshotImages(*current), options = options_, destination = *chosen]() mutable {
        const auto cancelled = [t = token.get()] { return t->cancelled(); };
        if (cancelled()) {
            *written = std::unexpected(cancelledError());
        } else {
            *result = buildMarkdownPdf(source, options, images);
            if (!result->value().has_value()) {
                *written = std::unexpected(result->value().error());
            } else if (cancelled()) {
                *written = std::unexpected(cancelledError());
            } else {
                *written = writeBuild(engine_, result->value()->document, destination, cancelled);
            }
        }
        deliver([this, result, written, destination] {
            exporting_ = false;
            if (!written->has_value()) {
                if (written->error().code == core::ErrorCode::Cancelled) {
                    setStatus_("Export cancelled");
                } else {
                    setStatus_("Export failed: " + core::describe(written->error()));
                }
                return;
            }
            setStatus_(exportSummary(*result->value(), destination));
        });
    });
}

// --- Print -----------------------------------------------------------------------------

void MarkdownExportController::print(DocumentTab& tab) {
    if (!tab.isMarkdown() || tab.state() != DocumentTab::State::Ready || tab.markdown() == nullptr) return;
    if (!engine_.isAvailable()) {
        setStatus_("Printing Markdown requires the PDF backend");
        return;
    }
    if (preparingPrint_ || printer_.isSpooling()) {
        setStatus_("Already preparing a print job (Esc to cancel)");
        return;
    }
    if (context_.services.printService == nullptr || context_.services.mainDispatcher == nullptr) {
        setStatus_("No print service available on this platform backend");
        return;
    }
    context_.pendingEdits.commitAll();
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return;

    preparingPrint_ = true;
    printCancel_ = std::make_shared<std::atomic<bool>>(false);
    setStatus_("Preparing to print…");
    printSlot_ = std::make_shared<PrintSlot>();
    auto slot = printSlot_;
    auto failure = std::make_shared<std::optional<core::Error>>();
    scheduler_.post([this, token = std::move(token), cancel = printCancel_, slot, failure,
                     source = tab.markdown()->source(), images = snapshotImages(tab), options = options_,
                     title = tab.title()]() mutable {
        const auto cancelled = [t = token.get(), cancel] { return t->cancelled() || cancel->load(); };
        const auto run = [&]() -> std::optional<core::Error> {
            if (cancelled()) return cancelledError();
            core::Result<MarkdownExportResult> built = buildMarkdownPdf(source, options, images);
            if (!built.has_value()) return std::move(built).error();
            if (cancelled()) return cancelledError();
            auto directory = createPrivatePrintDirectory();
            if (!directory.has_value()) return std::move(directory).error();
            auto temp = std::make_shared<PrintTempDocument>();
            temp->directory = std::move(*directory); // removed with `temp` on every path
            const std::filesystem::path file = temp->directory / "document.pdf";
            if (core::Status status = writeBuild(engine_, built->document, file, cancelled); !status) {
                return status.error();
            }
            if (cancelled()) return cancelledError();
            auto opened = engine_.openDocument(file);
            if (!opened.has_value()) return std::move(opened).error();
            temp->document = std::move(*opened);
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->pending = std::move(temp);
            return std::nullopt;
        };
        *failure = run();
        // Abandoned by the shell: the document must close now, while the
        // engine is guaranteed alive (the destructor waits for this task).
        if (token->cancelled()) {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->pending.reset();
            return;
        }
        deliver([this, slot, failure, cancel, title = std::move(title)] {
            preparingPrint_ = false;
            std::shared_ptr<PrintTempDocument> temp;
            {
                std::lock_guard<std::mutex> lock(slot->mutex);
                temp = std::move(slot->pending);
            }
            if (cancel->load()) {
                setStatus_("Print cancelled");
                return;
            }
            if (failure->has_value() || temp == nullptr || temp->document == nullptr) {
                if (failure->has_value() && (*failure)->code == core::ErrorCode::Cancelled) {
                    setStatus_("Print cancelled");
                } else {
                    setStatus_("Print failed: " + (failure->has_value() ? core::describe(**failure)
                                                                         : std::string("no document")));
                }
                return;
            }
            // The aliasing pointer keeps `temp` (and so the file) alive as
            // long as the job renders from the document.
            std::shared_ptr<pdf::PdfDocument> document(temp, temp->document.get());
            printer_.printDocument(std::move(document), title, temp);
        });
    });
}

bool MarkdownExportController::requestCancelPrint() {
    if (!preparingPrint_ || printCancel_ == nullptr) return false;
    printCancel_->store(true);
    setStatus_("Cancelling print…");
    return true;
}

} // namespace rivet::app
