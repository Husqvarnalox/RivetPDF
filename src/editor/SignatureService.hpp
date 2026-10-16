// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfSignatures.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace rivet::editor {

class DocumentSession;

// The digital signatures of a session's BASE document (ADR-0025), for
// display and for the save warning:
//
//   - loaded lazily, once per base, on a dedicated SerialExecutor stream
//     (PDFium calls are gated in the backend) and delivered on the main
//     thread; a backend without signature support (or a load error) yields
//     an empty list;
//   - a rebase (save) drops the list; it reloads when someone used it.
//
// Signature strings are untrusted and never logged.
//
// Threading: main thread only, except the load job (which only reads the
// captured document). Completions are dropped when the service died
// (liveness flag) or the base changed meanwhile (generation token).
class SignatureService {
public:
    enum class State : std::uint8_t { NotLoaded, Loading, Ready };

    explicit SignatureService(DocumentSession& session);
    ~SignatureService();

    SignatureService(const SignatureService&) = delete;
    SignatureService& operator=(const SignatureService&) = delete;

    // Fired (main thread) when the list finished loading.
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }

    // Starts the load when it has not started. Main thread.
    void ensureLoaded();
    State state() const;
    // The loaded list (null until Ready; empty without signature support).
    pdf::PdfSignatureListPtr list() const;
    // True once Ready with at least one signature.
    bool hasSignatures() const;

    // The session switched its base document (save + rebase).
    void rebased();

    // Tests: blocks until the load stream is idle (the completion may still
    // be queued on the main dispatcher).
    void waitForLoads();

private:
    void complete(std::uint64_t generation, pdf::PdfSignatureListPtr list);

    DocumentSession& session_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    mutable std::mutex mutex_; // guards state_/list_ (inline completions in tests)
    State state_ = State::NotLoaded;
    pdf::PdfSignatureListPtr list_;
    std::uint64_t generation_ = 1;
    std::function<void()> onChanged_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    core::SerialExecutor executor_; // declared last: destroyed (drained) first
};

} // namespace rivet::editor
