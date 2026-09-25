// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/SelectionModel.hpp"
#include "editor/TextSearchController.hpp"
#include "pdf/PdfEngine.hpp"
#include "render/ViewerState.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

// Normalized path identity for duplicate detection: absolute +
// lexically-normalized. Symlinks are deliberately NOT resolved so
// user-visible path semantics stay untouched.
std::filesystem::path dedupKeyForPath(const std::filesystem::path& path);

// Stable identity of one tab for its whole life. Minted by the workspace
// from a monotonic counter and never reused, so asynchronous completions
// can address "their" tab without relying on object addresses (a freed
// tab's address may be reused by a later tab).
struct TabIdTag;
using TabId = core::StrongId<TabIdTag>;

// One open document tab. A tab exists in Loading state while its document
// opens on a background task, then becomes Ready (session attached) or Error
// (message shown in the tab's view).
//
// View state (zoom/scroll) is deliberately separate from DocumentSession:
// one session may serve several views later, and per-tab view state must
// survive tab switches. Main-thread only.
class DocumentTab {
public:
    enum class State : std::uint8_t { Loading, Ready, Error, NeedsPassword };

    DocumentTab(TabId id, std::filesystem::path path, std::string title);
    ~DocumentTab();

    DocumentTab(const DocumentTab&) = delete;
    DocumentTab& operator=(const DocumentTab&) = delete;

    TabId id() const { return id_; }
    const std::filesystem::path& path() const { return path_; }
    // Normalized identity used for duplicate detection (never displayed).
    const std::filesystem::path& dedupKey() const { return dedupKey_; }
    const std::string& title() const { return title_; }
    State state() const { return state_; }
    const std::string& errorText() const { return errorText_; }
    editor::DocumentSession* session() { return session_.get(); }
    const editor::DocumentSession* session() const { return session_.get(); }
    render::ViewerState& viewState() { return viewState_; }
    const render::ViewerState& viewState() const { return viewState_; }

    // Per-tab text-interaction state. The search controller exists only in
    // the Ready state (it references the session and its text service).
    editor::SelectionModel& selection() { return selection_; }
    const editor::SelectionModel& selection() const { return selection_; }
    editor::TextSearchController* search() { return search_.get(); }
    const editor::TextSearchController* search() const { return search_.get(); }

    std::size_t currentPage() const { return currentPage_; }
    void setCurrentPage(std::size_t page) { currentPage_ = page; }

    // False for a freshly created tab: the shell applies the initial fit
    // mode (fit width) on the first bind of a Ready tab. View-state values
    // restored from an already-initialized tab are used as they are.
    bool viewStateInitialized() const { return viewStateInitialized_; }
    void markViewStateInitialized() { viewStateInitialized_ = true; }

    // Called by the workspace on the main thread when the open completes.
    void attachSession(std::unique_ptr<editor::DocumentSession> session);
    void setError(std::string text);
    // Password-required outcome: the tab stays open and asks for a password.
    void markNeedsPassword(std::string message);
    // Begin the password retry (state -> Loading). Workspace only.
    void beginPasswordRetry() { state_ = State::Loading; }

    // Identity of the latest open request issued for this tab. A completion
    // is applied only when it carries the tab's CURRENT request (a retry
    // supersedes an earlier attempt). Workspace only.
    std::uint64_t openRequest() const { return openRequest_; }
    std::uint64_t beginOpenRequest() { return ++openRequest_; }

private:
    // Drops the session-dependent state in dependency order (search first).
    void releaseSession();

    TabId id_;
    std::uint64_t openRequest_ = 0;
    std::filesystem::path path_;
    std::filesystem::path dedupKey_;
    std::string title_;
    State state_ = State::Loading;
    std::string errorText_;
    std::unique_ptr<editor::DocumentSession> session_;
    render::ViewerState viewState_;
    editor::SelectionModel selection_;
    std::unique_ptr<editor::TextSearchController> search_;
    std::size_t currentPage_ = 0;
    bool viewStateInitialized_ = false;
};

// The set of open document tabs plus the active one. Owns tab lifetime and
// the asynchronous open pipeline:
//
//   openDocument(path) -> Loading tab appears and becomes active
//                      -> background task: DocumentSession::create
//                      -> main-thread completion: attach session | error
//
// Opening a file that is already open activates its existing tab instead of
// duplicating it. The dedup key is the absolute, lexically-normalized path;
// symlinks are deliberately NOT resolved so user-visible path semantics stay
// untouched.
//
// Async identity: every open is addressed by (TabId, request token). TabIds
// are never reused and each open/retry mints a new request token, so a stale
// completion (tab closed meanwhile, or superseded by a password retry) can
// never land in a different or newer tab - no address-based identity.
//
// Lifetime: background opens borrow the engine and the scheduler. They run
// inside an AsyncScope; the destructor (and shutdown()) closes the scope and
// WAITS until no open task is inside the engine anymore, so the borrowed
// engine/scheduler (declared before the workspace by the shell) are
// guaranteed to outlive every use. A task that finishes after cancellation
// destroys its freshly created session on the worker, before releasing its
// token. Results still queued on the dispatcher are owned by the workspace's
// in-flight list and destroyed deterministically at shutdown; the posted
// delivery itself only holds a liveness flag.
//
// Threading: everything is main-thread-only except the background open task,
// which touches only the borrowed engine/scheduler/dispatcher and produces
// the session. DocumentSession::create performs its PDFium work on the
// calling (worker) thread under the adapter's global call gate.
class DocumentWorkspace {
public:
    static constexpr std::size_t kNoTab = static_cast<std::size_t>(-1);

    // The engine and scheduler are shell-owned and must outlive the
    // workspace (the shell declares them before it).
    DocumentWorkspace(pdf::PdfEngine& engine, core::TaskScheduler& scheduler,
                      core::IMainThreadDispatcher* mainDispatcher);
    ~DocumentWorkspace();

    DocumentWorkspace(const DocumentWorkspace&) = delete;
    DocumentWorkspace& operator=(const DocumentWorkspace&) = delete;

    // Refuses new opens, waits for in-flight open tasks to leave the engine
    // and drops undelivered results. Idempotent; the destructor calls it.
    // The owner calls it explicitly when the borrowed engine/scheduler are
    // about to go away.
    void shutdown();

    // Opens `path` in a new tab (or activates the existing tab for the same
    // file) and makes it active. Asynchronous: the Loading tab exists before
    // this returns; the session arrives via a later main-thread completion.
    void openDocument(const std::filesystem::path& path);

    // Retries the open of a NeedsPassword tab with a password. The password
    // is forwarded to the background open and NOT stored anywhere: the tab
    // drops it as soon as the completion runs. Never log it.
    void retryWithPassword(std::size_t tabIndex, std::string password);

    // Closes the tab (cancelling a pending open for it). Closing the active
    // tab activates the nearest remaining tab; closing the last tab leaves
    // the empty state.
    void closeTab(std::size_t index);
    void closeActiveTab();

    void activateTab(std::size_t index);

    std::size_t tabCount() const { return tabs_.size(); }
    DocumentTab* tab(std::size_t index);
    DocumentTab* activeTab();
    // Tab by stable identity (nullptr when closed) and its current index.
    DocumentTab* tabById(TabId id);
    std::size_t indexOfTab(TabId id) const;
    // Const reads for text assembly/highlight painting (shallow const: the
    // tabs are owned through unique_ptrs, the pointees stay mutable).
    DocumentTab* tab(std::size_t index) const { return const_cast<DocumentWorkspace*>(this)->tab(index); }
    DocumentTab* activeTab() const { return const_cast<DocumentWorkspace*>(this)->activeTab(); }
    std::size_t activeIndex() const { return activeIndex_; }
    bool isEmpty() const { return tabs_.empty(); }

    // Index of the tab whose dedup key matches, or kNoTab.
    std::size_t indexOfPath(const std::filesystem::path& path) const;

    // Host hooks (all fired on the main thread).
    // Fired whenever any tab was added, changed state, or was removed.
    void setOnTabsChanged(std::function<void()> onTabsChanged);
    // Fired when the active tab changed (including to none). The host
    // rebinds its viewer widgets to the new active tab.
    void setOnActiveTabChanged(std::function<void()> onActiveTabChanged);

private:
    // One background open. Filled by the worker, consumed on the main
    // thread. Owned by inFlight_ until delivered or shut down.
    struct OpenOperation {
        TabId tab;
        std::uint64_t request = 0;
        bool isRetry = false; // retry failures re-enter NeedsPassword
        std::optional<core::Result<std::unique_ptr<editor::DocumentSession>>> result;
    };

    void handleOpenCompleted(const std::shared_ptr<OpenOperation>& operation);
    void startOpen(DocumentTab& tab, std::string password, bool isRetry);
    void activate(std::size_t index);
    void fireTabsChanged();

    pdf::PdfEngine& engine_;
    core::TaskScheduler& scheduler_;
    core::IMainThreadDispatcher* mainDispatcher_;

    std::vector<std::unique_ptr<DocumentTab>> tabs_;
    std::size_t activeIndex_ = kNoTab;
    core::IdGenerator<TabIdTag> tabIds_;
    std::function<void()> onTabsChanged_;
    std::function<void()> onActiveTabChanged_;

    // Opens whose result has not been applied yet (main thread).
    std::vector<std::shared_ptr<OpenOperation>> inFlight_;
    // Set false by shutdown(); posted deliveries check it on the main thread.
    std::shared_ptr<std::atomic<bool>> workspaceAlive_;
    // Fence for the worker side of every open (see class comment).
    core::AsyncScope openScope_;
};

} // namespace rivet::app
