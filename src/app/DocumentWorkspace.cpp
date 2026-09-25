// SPDX-License-Identifier: MPL-2.0
#include "app/DocumentWorkspace.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <utility>

namespace rivet::app {

std::filesystem::path dedupKeyForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    // absolute() only fails on exotic allocation errors; fall back to the
    // input path so a probe never crashes the open flow.
    return (ec ? path : absolute).lexically_normal();
}

DocumentTab::DocumentTab(std::filesystem::path path, std::string title)
    : path_(std::move(path)), title_(std::move(title)) {
    dedupKey_ = dedupKeyForPath(path_);
}

void DocumentTab::attachSession(std::unique_ptr<editor::DocumentSession> session) {
    session_ = std::move(session);
    search_ = std::make_unique<editor::TextSearchController>(*session_, session_->textService());
    state_ = State::Ready;
    errorText_.clear();
}

void DocumentTab::setError(std::string text) {
    // The search controller references the session, so it dies first.
    search_.reset();
    session_.reset();
    state_ = State::Error;
    errorText_ = std::move(text);
}

DocumentWorkspace::DocumentWorkspace(pdf::PdfEngine& engine, core::TaskScheduler& scheduler,
                                     core::IMainThreadDispatcher* mainDispatcher)
    : engine_(engine),
      scheduler_(scheduler),
      mainDispatcher_(mainDispatcher),
      workspaceAlive_(std::make_shared<std::atomic<bool>>(true)) {}

DocumentWorkspace::~DocumentWorkspace() {
    // Completions dispatched after this point see the flag and drop their
    // session instead of touching the dead workspace. The sessions themselves
    // die with the tabs below (destroying a session waits for its executor
    // stream, which is fine: the shared scheduler is still alive here).
    workspaceAlive_->store(false, std::memory_order_release);
}

std::size_t DocumentWorkspace::indexOfPath(const std::filesystem::path& path) const {
    const std::filesystem::path key = dedupKeyForPath(path);
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->dedupKey() == key) return i;
    }
    return kNoTab;
}

void DocumentWorkspace::openDocument(const std::filesystem::path& path) {
    // Already open (including a tab still loading): activate, do not dup.
    const std::size_t existing = indexOfPath(path);
    if (existing != kNoTab) {
        if (existing != activeIndex_) activate(existing);
        return;
    }

    auto tab = std::make_unique<DocumentTab>(path, path.filename().string());
    DocumentTab* tabPtr = tab.get();
    tabs_.push_back(std::move(tab));
    activate(tabs_.size() - 1);
    startOpen(tabs_.size() - 1, tabPtr, {}, false);
    fireTabsChanged();
}

void DocumentWorkspace::retryWithPassword(std::size_t tabIndex, std::string password) {
    DocumentTab* retryTab = tab(tabIndex);
    if (retryTab == nullptr || retryTab->state() != DocumentTab::State::NeedsPassword) return;
    retryTab->beginPasswordRetry(); // -> Loading
    startOpen(tabIndex, retryTab, std::move(password), /*isRetry=*/true);
    fireTabsChanged();
}

void DocumentWorkspace::startOpen(std::size_t tabIndex, DocumentTab* tab, std::string password,
                                  bool isRetry) {
    (void)tabIndex;
    // Background open. The continuation carries the outcome across the
    // thread boundary; the main-thread completion re-validates everything
    // (workspace alive, tab still open) before touching it. The engine and
    // scheduler references are shell-owned and outlive this task (the shell
    // destroys the workspace before the engine and scheduler). The
    // completion is dispatched by the WORKER after create() finishes, so a
    // completion never runs ahead of its result. The password lives only in
    // this task and the continuation - never stored, never logged.
    auto continuation = std::make_shared<OpenContinuation>();
    continuation->workspaceAlive = workspaceAlive_;
    continuation->tab = tab;
    continuation->isRetry = isRetry;
    pdf::PdfEngine& engine = engine_;
    core::TaskScheduler& scheduler = scheduler_;
    core::IMainThreadDispatcher* dispatcher = mainDispatcher_;
    const std::filesystem::path openPath = tab->path();
    DocumentWorkspace* self = this;
    scheduler.post([&engine, &scheduler, dispatcher, openPath, continuation, self,
                    password = std::move(password)]() mutable {
        continuation->session =
            editor::DocumentSession::create(engine, scheduler, dispatcher, openPath, password);
        // Drop the password before the completion lambda captures anything.
        password.clear();
        password.shrink_to_fit();
        std::string().swap(password);
        dispatcher->post([self, continuation] {
            if (!continuation->workspaceAlive->load(std::memory_order_acquire)) return;
            self->handleOpenCompleted(continuation);
        });
    });
}

void DocumentWorkspace::handleOpenCompleted(std::shared_ptr<OpenContinuation> continuation) {
    DocumentTab* tab = continuation->tab;
    const bool stillOpen =
        std::any_of(tabs_.begin(), tabs_.end(), [tab](const auto& owned) { return owned.get() == tab; });
    if (!stillOpen) {
        // Tab was closed while loading: drop the session (its destructor
        // waits for the executor stream; safe on the main thread).
        return;
    }

    if (continuation->session.has_value()) {
        tab->attachSession(std::move(*continuation->session));
        core::log::info(std::string("document opened: ") + std::to_string(tab->session()->pageCount()) +
                        " pages");
    } else if (continuation->session.error().code == core::ErrorCode::PasswordRequired) {
        // Prompt instead of failing. A retry that fails again re-enters this
        // state (the UI keeps the field focused). No password is stored or
        // logged.
        core::log::warning("document open requires a password");
        tab->markNeedsPassword(continuation->session.error().message);
    } else {
        core::log::warning("document open failed: " + core::describe(continuation->session.error()));
        // Never include document contents in user text; describe() is a
        // sanitized error string.
        tab->setError(core::describe(continuation->session.error()));
    }
    // The shell must rebind its viewer widgets when the active tab's content
    // changed (Loading -> Ready/Error), not just on tab switches.
    const std::size_t activeNow =
        (activeIndex_ != kNoTab && tabs_[activeIndex_].get() == tab) ? activeIndex_ : kNoTab;
    fireTabsChanged();
    if (activeNow != kNoTab && onActiveTabChanged_) onActiveTabChanged_();
}

void DocumentWorkspace::closeTab(std::size_t index) {
    if (index >= tabs_.size()) return;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));

    if (tabs_.empty()) {
        const bool wasActive = activeIndex_ != kNoTab;
        activeIndex_ = kNoTab;
        if (wasActive && onActiveTabChanged_) onActiveTabChanged_();
        fireTabsChanged();
        return;
    }

    if (activeIndex_ != kNoTab) {
        if (index < activeIndex_) {
            // Shifted down; active tab identity preserved.
            --activeIndex_;
        } else if (index == activeIndex_) {
            // Active tab closed: activate the tab now at the same index
            // (the next one), or the last one.
            activeIndex_ = std::min(activeIndex_, tabs_.size() - 1);
            if (onActiveTabChanged_) onActiveTabChanged_();
        }
    }
    fireTabsChanged();
}

void DocumentWorkspace::closeActiveTab() {
    if (activeIndex_ != kNoTab) closeTab(activeIndex_);
}

void DocumentWorkspace::activateTab(std::size_t index) {
    if (index >= tabs_.size() || index == activeIndex_) return;
    activate(index);
}

void DocumentWorkspace::activate(std::size_t index) {
    activeIndex_ = index;
    if (onActiveTabChanged_) onActiveTabChanged_();
}

DocumentTab* DocumentWorkspace::tab(std::size_t index) {
    return index < tabs_.size() ? tabs_[index].get() : nullptr;
}

DocumentTab* DocumentWorkspace::activeTab() { return tab(activeIndex_); }

void DocumentWorkspace::setOnTabsChanged(std::function<void()> onTabsChanged) {
    onTabsChanged_ = std::move(onTabsChanged);
}

void DocumentWorkspace::setOnActiveTabChanged(std::function<void()> onActiveTabChanged) {
    onActiveTabChanged_ = std::move(onActiveTabChanged);
}

void DocumentWorkspace::fireTabsChanged() {
    if (onTabsChanged_) onTabsChanged_();
}

} // namespace rivet::app
