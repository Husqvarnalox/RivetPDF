// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/SelectionModel.hpp"
#include "editor/TextSearchController.hpp"
#include "pdf/PdfEngine.hpp"
#include "render/ViewerState.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rivet::app {

// Normalized path identity for duplicate detection: absolute +
// lexically-normalized. Symlinks are deliberately NOT resolved so
// user-visible path semantics stay untouched.
std::filesystem::path dedupKeyForPath(const std::filesystem::path& path);

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

    DocumentTab(std::filesystem::path path, std::string title);

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
    void markNeedsPassword(std::string message) {
        session_.reset();
        state_ = State::NeedsPassword;
        errorText_ = std::move(message);
    }
    // Begin the password retry (state -> Loading). Workspace only.
    void beginPasswordRetry() { state_ = State::Loading; }

private:
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
// Cancellation and lifetime: closing a Loading tab removes it; the in-flight
// background task cannot be interrupted (the open runs to completion inside
// the PDF backend), but its completion handler re-checks tab existence on
// the main thread and drops the result when the tab is gone. Completions
// arriving after the workspace itself died are dropped via a shared alive
// flag (the app-level open flow must never resurrect or leak a session into
// a dead workspace).
//
// Threading: everything is main-thread-only except the background open task,
// which touches only shell-owned references (engine, scheduler, dispatcher)
// and produces the session. DocumentSession::create performs its PDFium work
// on the calling (worker) thread under the adapter's global call gate.
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
    // Shared between the background task and the main-thread completion so a
    // completed open can be dropped safely after workspace death.
    struct OpenContinuation {
        std::shared_ptr<std::atomic<bool>> workspaceAlive;
        DocumentTab* tab = nullptr; // raw; re-verified against tabs_ on delivery
        core::Result<std::unique_ptr<editor::DocumentSession>> session;
        bool isRetry = false; // retry failures re-enter NeedsPassword
    };

    void handleOpenCompleted(std::shared_ptr<OpenContinuation> continuation);
    void startOpen(std::size_t tabIndex, DocumentTab* tab, std::string password, bool isRetry);
    void activate(std::size_t index);
    void fireTabsChanged();

    pdf::PdfEngine& engine_;
    core::TaskScheduler& scheduler_;
    core::IMainThreadDispatcher* mainDispatcher_;

    std::vector<std::unique_ptr<DocumentTab>> tabs_;
    std::size_t activeIndex_ = kNoTab;
    std::function<void()> onTabsChanged_;
    std::function<void()> onActiveTabChanged_;
    // Set false by the destructor; completions arriving afterwards drop their
    // session instead of touching the dead workspace.
    std::shared_ptr<std::atomic<bool>> workspaceAlive_;
};

} // namespace rivet::app
