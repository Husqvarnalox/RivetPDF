// SPDX-License-Identifier: MPL-2.0
#include "editor/DocumentSaver.hpp"

#include "core/Log.hpp"

#include <utility>
#include <vector>

namespace rivet::editor {
namespace {

core::Error cancelledError() {
    return core::makeError(core::ErrorCode::Cancelled, "the save was cancelled", "editor");
}

// Buffers the assembly's (often small) chunks into large writes and polls
// cancellation between them. Never calls back into the engine (see
// IPdfByteSink).
class BufferedAtomicSink final : public pdf::IPdfByteSink {
public:
    static constexpr std::size_t kBufferSize = 256 * 1024;

    BufferedAtomicSink(core::io::AtomicFileWriter& writer, const DocumentWriteControl& control)
        : writer_(writer), control_(control) {
        buffer_.reserve(kBufferSize);
    }

    core::Status write(const void* data, std::size_t size) override {
        if (size == 0) return core::ok();
        if (data == nullptr) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "null output chunk", "editor"));
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
        if (control_.cancelled && control_.cancelled()) return std::unexpected(cancelledError());
        return writer_.write(data, size);
    }

    core::io::AtomicFileWriter& writer_;
    const DocumentWriteControl& control_;
    std::vector<char> buffer_;
};

core::Result<DocumentWriteJob> makeJob(const DocumentSession& session, DocumentWriteJob::Kind kind,
                                       std::span<const core::PageId> pages, std::filesystem::path destination) {
    if (destination.empty()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "no destination file", "editor"));
    }
    DocumentWriteJob job;
    job.kind = kind;
    job.snapshot = session.pageSnapshot();
    auto request = job.snapshot->toAssemblyRequest(
        kind == DocumentWriteJob::Kind::Save ? PageModelSnapshot::AssemblyMode::Save
                                             : PageModelSnapshot::AssemblyMode::Extract,
        pages);
    if (!request.has_value()) return std::unexpected(std::move(request).error());
    job.request = std::move(*request);
    job.destination = std::move(destination);
    job.prepareRebase = kind == DocumentWriteJob::Kind::Save;
    job.document = session.id();
    job.documentRevision = session.documentRevision();
    return job;
}

} // namespace

core::Result<DocumentWriteJob> makeSaveJob(const DocumentSession& session, std::filesystem::path destination) {
    return makeJob(session, DocumentWriteJob::Kind::Save, {}, std::move(destination));
}

core::Result<DocumentWriteJob> makeExtractJob(const DocumentSession& session, std::span<const core::PageId> pages,
                                              std::filesystem::path destination) {
    return makeJob(session, DocumentWriteJob::Kind::Extract, pages, std::move(destination));
}

DocumentWriteResult runDocumentWrite(pdf::PdfEngine& engine, const DocumentWriteJob& job,
                                     const DocumentWriteControl& control) {
    DocumentWriteResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&]() -> DocumentWriteResult {
        result.elapsed = std::chrono::steady_clock::now() - started;
        return std::move(result);
    };
    if (job.snapshot == nullptr || job.request.pages.empty()) {
        result.written = std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "empty write job", "editor"));
        return finish();
    }
    if (control.cancelled && control.cancelled()) {
        result.written = std::unexpected(cancelledError());
        return finish();
    }

    core::io::AtomicWriteOptions options;
    options.overwriteExisting = job.overwriteExisting; // default: the save panel confirmed it
    options.faultInjector = control.faultInjector;
    auto writer = core::io::AtomicFileWriter::begin(job.destination, std::move(options));
    if (!writer.has_value()) {
        result.written = std::unexpected(std::move(writer).error());
        return finish();
    }

    BufferedAtomicSink sink(**writer, control);
    std::vector<pdf::PdfAssembledPageAnnotations> annotationReport;
    std::vector<pdf::PdfAssembledPageContent> contentReport;
    core::Status status = engine.assembleDocument(job.request, sink, &annotationReport, &contentReport);
    if (status.has_value()) status = sink.flush();
    if (status.has_value() && control.cancelled && control.cancelled()) status = std::unexpected(cancelledError());
    if (status.has_value()) status = (*writer)->commit();
    result.bytesWritten = (*writer)->bytesWritten();
    if (!status.has_value()) {
        (*writer)->abort(); // idempotent; the destination stays untouched
        result.written = std::move(status);
        return finish();
    }
    writer->reset();

    if (job.prepareRebase && job.snapshot->base() != nullptr) {
        // The save succeeded; a failed reopen only means the session keeps
        // its previous base (still valid: it reads its own open descriptor).
        result.rebase = DocumentSession::prepareRebase(engine, job.destination, *job.snapshot->base());
        if (!result.rebase->has_value()) {
            core::log::warning("save: reopening the saved file failed: " + core::describe(result.rebase->error()));
        } else {
            (**result.rebase).annotationReport = std::move(annotationReport);
            (**result.rebase).contentReport = std::move(contentReport);
        }
    }
    return finish();
}

} // namespace rivet::editor
