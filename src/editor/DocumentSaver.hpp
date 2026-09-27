// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/DocumentSession.hpp"
#include "editor/PageModel.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/io/AtomicFileWriter.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace rivet::editor {

// Writing a document to disk (Save / Save As / Extract), split into a
// main-thread capture and a worker-thread run (ADR-0009):
//
//   main:   job = makeSaveJob(session, path)     // snapshot + request
//   worker: result = runDocumentWrite(engine, job, control)
//             assembleDocument -> buffered sink -> AtomicFileWriter
//             (temp file in the destination directory -> fsync -> rename)
//             [Save] reopen the written file + read its page metadata
//   main:   session.rebaseOnto(*result.rebase) + markSaved()
//
// The job owns the page-model snapshot: its entries keep the base and every
// imported document alive until the worker is done, and the request's raw
// document pointers point into it. The worker never reads session state.
//
// Failure anywhere before the rename (assembly error, I/O error, disk full,
// cancellation) removes the temp file and leaves the destination untouched.
struct DocumentWriteJob {
    enum class Kind : std::uint8_t { Save, Extract };

    Kind kind = Kind::Save;
    PageSnapshotPtr snapshot;
    pdf::PdfAssemblyRequest request;
    std::filesystem::path destination;
    // Save: after the write, reopen the file (same credentials as the
    // snapshot's base) and read what DocumentSession::rebaseOnto needs.
    bool prepareRebase = false;
    // The session state the job captured (for the completion's checks).
    core::DocumentId document;
    std::uint64_t documentRevision = 0;
};

// Save / Save As: PreserveBase over every page of the current model.
core::Result<DocumentWriteJob> makeSaveJob(const DocumentSession& session, std::filesystem::path destination);

// Extract: Fresh document of `pages` (model order; duplicates ignored).
core::Result<DocumentWriteJob> makeExtractJob(const DocumentSession& session,
                                              std::span<const core::PageId> pages,
                                              std::filesystem::path destination);

// Cancellation and test hooks for runDocumentWrite. Every member is
// optional. `cancelled` is polled between output chunks and before the
// commit; `faultInjector` is forwarded to AtomicWriteOptions (tests only).
struct DocumentWriteControl {
    std::function<bool()> cancelled;
    std::function<int(core::io::AtomicWriteFault)> faultInjector;
};

struct DocumentWriteResult {
    // Assembly + atomic replacement. On failure the destination is intact.
    core::Status written = core::ok();
    // Save with prepareRebase and a successful write: the reopened file (or
    // why it could not be reopened - the save itself still succeeded).
    std::optional<core::Result<DocumentSession::RebaseTarget>> rebase;
    std::uint64_t bytesWritten = 0;
    std::chrono::steady_clock::duration elapsed{};
};

// Worker thread. Blocking; touches only the engine and the job.
DocumentWriteResult runDocumentWrite(pdf::PdfEngine& engine, const DocumentWriteJob& job,
                                     const DocumentWriteControl& control = {});

} // namespace rivet::editor
