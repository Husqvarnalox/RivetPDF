// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "markdown/MarkdownModel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// What the coordinator needs from the shell. Both pointers outlive it.
struct MarkdownParseEnvironment {
    core::IMainThreadDispatcher* dispatcher = nullptr; // results are applied on the main thread
    core::TaskScheduler* scheduler = nullptr;          // parse and debounce-timer jobs run here
    // Test seam: where background jobs run (default: scheduler->post). A
    // job must be run at most once; it may be dropped without being run.
    std::function<void(std::function<void()>)> runBackground;
};

struct MarkdownParseTuning {
    // Quiet period after the last edit before a Debounced request parses.
    std::chrono::milliseconds debounce{150};
    // ...but never wait longer than this since the first unparsed edit, so a
    // long typing run still refreshes the preview now and then.
    std::chrono::milliseconds maxDebounce{800};
    // Immediate requests for sources up to this size parse synchronously
    // (no placeholder flash); larger ones parse in the background.
    std::size_t synchronousParseBytes = 64 * 1024;
};

struct MarkdownParseResult {
    std::shared_ptr<const markdown::MarkdownDocument> document;
    std::uint64_t revision = 0;
    std::string source; // exactly the text `document` was parsed from (moved, not copied again)
};

// Parses a tab's Markdown source off the main thread and publishes the newest
// document, with generation semantics:
//   - requestParse(revision) records `revision` as the latest wanted one. A
//     result is applied only if its revision is the latest requested at the
//     moment it completes AND not older than what was applied already; stale
//     results (rev 10 finishing after rev 11 was requested or applied) are
//     dropped silently, so the UI never goes backwards.
//   - Debounced: the parse starts after the quiet period (a worker job waits
//     it out; no timer facility exists), copying the source then. Immediate:
//     parse now - synchronously for small sources, else on a worker.
//   - bind()/unbind() start a new epoch: results of earlier epochs are dropped.
//
// The source is read through a snapshot function set by bind(); it is called
// ONLY on the main thread and the coordinator never holds anything from it
// except the copy it parses. The host must unbind() before the thing the
// function captures dies.
//
// Lifetime: the destructor closes the AsyncScope (waits for RUNNING jobs
// only; queued jobs notice the closed scope and do nothing, and touch no
// member). Nothing is delivered to the callback afterwards. Main thread only.
class MarkdownParseCoordinator {
public:
    struct Snapshot {
        std::string source;
        std::uint64_t revision = 0;
    };
    using SnapshotFn = std::function<Snapshot()>;
    using ResultFn = std::function<void(MarkdownParseResult)>;

    MarkdownParseCoordinator(MarkdownParseEnvironment environment, MarkdownParseTuning tuning = {});
    ~MarkdownParseCoordinator();

    MarkdownParseCoordinator(const MarkdownParseCoordinator&) = delete;
    MarkdownParseCoordinator& operator=(const MarkdownParseCoordinator&) = delete;

    // Called (main thread) whenever a newer document is applied. May run
    // synchronously inside requestParse() for small immediate requests.
    void setOnParsed(ResultFn onParsed) { onParsed_ = std::move(onParsed); }

    void bind(SnapshotFn snapshot);
    void unbind();
    bool bound() const { return static_cast<bool>(snapshot_); }

    enum class Urgency : std::uint8_t { Debounced, Immediate };
    void requestParse(std::uint64_t revision, Urgency urgency);

    // --- State (tests, diagnostics) ---------------------------------------
    std::uint64_t appliedRevision() const { return appliedRevision_; }
    std::uint64_t requestedRevision() const { return requestedRevision_; }
    // A debounce wait or a background parse is outstanding.
    bool busy() const { return debouncePending_ || parsesInFlight_ > 0; }
    std::size_t parsesStarted() const { return parsesStarted_; }
    std::size_t resultsDropped() const { return resultsDropped_; }

private:
    struct Shared; // what worker jobs may touch (outlives the coordinator if needed)

    void armDebounce(std::chrono::milliseconds delay);
    void onDebounceFired();
    void startParse(Snapshot snapshot);
    void parseNow(Snapshot snapshot);
    void deliver(std::uint64_t epoch, MarkdownParseResult result);
    bool canRunAsync() const { return env_.dispatcher != nullptr && (env_.runBackground || env_.scheduler != nullptr); }
    void run(std::function<void()> job);

    MarkdownParseEnvironment env_;
    MarkdownParseTuning tuning_;
    ResultFn onParsed_;
    SnapshotFn snapshot_;

    std::uint64_t epoch_ = 0;
    std::uint64_t requestedRevision_ = 0;
    std::uint64_t appliedRevision_ = 0;
    bool anyApplied_ = false;
    bool debouncePending_ = false;
    std::chrono::steady_clock::time_point firstPending_{};
    std::chrono::steady_clock::time_point lastRequest_{};
    std::size_t parsesInFlight_ = 0;
    std::size_t parsesStarted_ = 0;
    std::size_t resultsDropped_ = 0;

    std::shared_ptr<Shared> shared_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); // guards main-thread deliveries
};

} // namespace rivet::app
