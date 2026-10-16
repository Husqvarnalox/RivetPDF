// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ContentTool.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace rivet::app {

// Recognize Text (ADR-0027) over the ACTIVE Ready PDF tab: renders the target
// pages at 300 dpi on a worker, runs the platform ITextRecognizer there and
// applies one undoable invisible-text edit per page on the main thread as
// each page completes. One job at a time. Progress and the outcome go to the
// status bar; Esc cancels (pages already applied stay, each its own undo
// step); closing the tab cancels. Recognized text is never logged. Main
// thread only; the destructor cancels and waits for the worker.
class OcrController {
public:
    OcrController(ShellContext& context, core::TaskScheduler& scheduler);
    ~OcrController();

    OcrController(const OcrController&) = delete;
    OcrController& operator=(const OcrController&) = delete;

    // RecognizeTextMissing / RecognizeTextAll.
    bool canPerform(ContentCommand command) const;
    void perform(ContentCommand command);
    // Why the commands are unavailable right now ("" = they are available).
    std::string unavailableReason() const;

    bool running() const { return job_ != nullptr; }
    // Esc: stops a running job. False when there is none.
    bool requestCancel();
    // Cancels the job when it belongs to the tab (tab close).
    void cancelForTab(TabId tab);

    // Languages requested from the engine (priority order).
    static const std::vector<std::string>& defaultLanguages();
    // 300 dpi.
    static constexpr double kPixelsPerPoint = 300.0 / 72.0;
    // Applying a page waits (20 ms steps) for its content extraction.
    static constexpr std::size_t kMaxApplyAttempts = 500;

private:
    struct Job;
    struct PageResult;
    struct PageTask;

    static bool isOcrCommand(ContentCommand command);
    void start(bool allPages);
    void onPage(const std::shared_ptr<Job>& job, PageResult result);
    void apply(const std::shared_ptr<Job>& job, const std::shared_ptr<PageResult>& result, std::size_t attempt);
    void pageDone(const std::shared_ptr<Job>& job);
    void maybeFinish(const std::shared_ptr<Job>& job);
    void workerDone(const std::shared_ptr<Job>& job);
    void finish(const std::shared_ptr<Job>& job);

    ShellContext& context_;
    core::TaskScheduler& scheduler_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<Job> job_;
    // Declared last: destroyed first (waits for the worker).
    core::AsyncScope scope_;
};

} // namespace rivet::app
