// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentComparison.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"
#include "render/ViewerState.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace rivet::app {

// Payload of a comparison tab (ADR-0029): the two documents being compared
// and the result of their text diff.
//
// Both documents are opened by this object, as secondary sessions that are
// not tabs of the workspace (they come from the files on disk). Opening and
// diffing run on the shared worker pool; completions are delivered on the
// main thread and only applied when they still belong to the current
// generation and the object is alive. Destroying the state (closing the tab)
// cancels the work, waits until no worker uses the sessions anymore and then
// releases both documents.
//
// Threading: main thread only, except the private worker bodies. The
// sessions are main-thread objects; workers touch only the page snapshots
// and the sessions' text services (the same worker-side path as search), and
// the AsyncScope fence keeps the sessions alive for them.
class ComparisonTabState {
public:
    enum class Phase : std::uint8_t { Opening, Comparing, Ready, Failed };

    struct Environment {
        pdf::PdfEngine* engine = nullptr;
        core::TaskScheduler* scheduler = nullptr;
        core::IMainThreadDispatcher* dispatcher = nullptr;
    };

    // Starts opening both documents. The engine, scheduler and dispatcher
    // must outlive the state.
    static std::unique_ptr<ComparisonTabState> start(const Environment& environment,
                                                     std::filesystem::path leftPath,
                                                     std::filesystem::path rightPath);
    ~ComparisonTabState();

    ComparisonTabState(const ComparisonTabState&) = delete;
    ComparisonTabState& operator=(const ComparisonTabState&) = delete;

    Phase phase() const { return phase_; }
    // Sanitized message of a Failed state (never document text).
    const std::string& errorText() const { return errorText_; }

    const std::filesystem::path& leftPath() const { return leftPath_; }
    const std::filesystem::path& rightPath() const { return rightPath_; }

    // Null until the documents are open (phase Comparing / Ready).
    editor::DocumentSession* leftSession() { return left_.get(); }
    editor::DocumentSession* rightSession() { return right_.get(); }
    render::ViewerState& leftView() { return leftView_; }
    render::ViewerState& rightView() { return rightView_; }

    // The diff, once the phase is Ready.
    const editor::ComparisonResult* result() const { return result_.has_value() ? &*result_ : nullptr; }

    // False until the host applied the initial fit mode to both views.
    bool viewsInitialized() const { return viewsInitialized_; }
    void markViewsInitialized() { viewsInitialized_ = true; }

    // Fired on the main thread whenever the phase changes.
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }

    // Cancels the running diff (if any) and starts a new one on the open
    // documents: a stale completion is dropped by the generation guard.
    // No-op unless both documents are open.
    void recompare();

private:
    struct Opened;

    ComparisonTabState(const Environment& environment, std::filesystem::path leftPath,
                       std::filesystem::path rightPath);

    void begin();
    void onOpened(std::uint64_t generation);
    void startDiff();
    void onDiffFinished(std::uint64_t generation, editor::ComparisonResult result);
    void fail(std::string text);
    void notify();

    Environment environment_;
    std::filesystem::path leftPath_;
    std::filesystem::path rightPath_;

    Phase phase_ = Phase::Opening;
    bool viewsInitialized_ = false;
    std::string errorText_;
    std::function<void()> onChanged_;

    // Written by the open worker, taken on the main thread.
    std::shared_ptr<Opened> opened_;

    // Declaration order is destruction order in reverse: the fence (waiting
    // for the workers) goes first, then the result, then the sessions.
    std::unique_ptr<editor::DocumentSession> left_;
    std::unique_ptr<editor::DocumentSession> right_;
    render::ViewerState leftView_;
    render::ViewerState rightView_;
    std::optional<editor::ComparisonResult> result_;

    // Generation guard: bumped by every start (open or diff); workers also
    // poll it through `latestGeneration_` to stop superseded work early.
    std::uint64_t generation_ = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> latestGeneration_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    core::AsyncScope scope_;
};

} // namespace rivet::app
