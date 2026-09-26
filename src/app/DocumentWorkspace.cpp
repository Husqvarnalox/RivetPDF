// SPDX-License-Identifier: MPL-2.0
#include "app/DocumentWorkspace.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace rivet::app {

std::filesystem::path dedupKeyForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    // absolute() only fails on exotic allocation errors; fall back to the
    // input path so a probe never crashes the open flow.
    return (ec ? path : absolute).lexically_normal();
}

DocumentTab::DocumentTab(TabId id, std::filesystem::path path, std::string title)
    : id_(id), path_(std::move(path)), title_(std::move(title)) {
    dedupKey_ = dedupKeyForPath(path_);
}

DocumentTab::~DocumentTab() { releaseSession(); }

void DocumentTab::releaseSession() {
    // The search controller references the session (and its text service),
    // so it dies first; its destructor drains its walker stream.
    search_.reset();
    session_.reset();
}

void DocumentTab::attachSession(std::unique_ptr<editor::DocumentSession> session) {
    releaseSession();
    session_ = std::move(session);
    search_ = std::make_unique<editor::TextSearchController>(*session_, session_->textService());
    state_ = State::Ready;
    errorText_.clear();
}

void DocumentTab::setError(std::string text) {
    releaseSession();
    state_ = State::Error;
    errorText_ = std::move(text);
}

void DocumentTab::markNeedsPassword(std::string message) {
    releaseSession();
    state_ = State::NeedsPassword;
    errorText_ = std::move(message);
}

DocumentWorkspace::DocumentWorkspace(pdf::PdfEngine& engine, core::TaskScheduler& scheduler,
                                     core::IMainThreadDispatcher* mainDispatcher)
    : engine_(engine),
      scheduler_(scheduler),
      mainDispatcher_(mainDispatcher),
      workspaceAlive_(std::make_shared<std::atomic<bool>>(true)) {}

DocumentWorkspace::~DocumentWorkspace() {
    shutdown();
    // Tabs (and their sessions) die with tabs_ below; destroying a session
    // waits for its executor streams while the shared scheduler is alive.
}

void DocumentWorkspace::shutdown() {
    // 1. Deliveries still queued on the dispatcher become no-ops.
    workspaceAlive_->store(false, std::memory_order_release);
    // 2. No new opens; wait until every open task left the engine. A task
    //    that completes after this point destroys its session on the worker
    //    before releasing its token (engine and scheduler still alive).
    openScope_.closeAndWait();
    // 3. Results that were produced but not yet delivered: destroy their
    //    sessions now, deterministically, while everything they reference is
    //    alive (the worker wrote them before releasing its token, which the
    //    wait above synchronizes with).
    for (const std::shared_ptr<OpenOperation>& operation : inFlight_) operation->result.reset();
    inFlight_.clear();
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

    auto tab = std::make_unique<DocumentTab>(tabIds_.next(), path, path.filename().string());
    DocumentTab& tabRef = *tab;
    tabs_.push_back(std::move(tab));
    activate(tabs_.size() - 1);
    startOpen(tabRef, {}, false);
    fireTabsChanged();
}

void DocumentWorkspace::retryWithPassword(std::size_t tabIndex, std::string password) {
    DocumentTab* retryTab = tab(tabIndex);
    if (retryTab == nullptr || retryTab->state() != DocumentTab::State::NeedsPassword) return;
    retryTab->beginPasswordRetry(); // -> Loading
    startOpen(*retryTab, std::move(password), /*isRetry=*/true);
    fireTabsChanged();
}

void DocumentWorkspace::startOpen(DocumentTab& tab, std::string password, bool isRetry) {
    auto operation = std::make_shared<OpenOperation>();
    operation->tab = tab.id();
    operation->request = tab.beginOpenRequest();
    operation->isRetry = isRetry;

    std::optional<core::AsyncScope::Token> entered = openScope_.enter();
    if (!entered.has_value() || mainDispatcher_ == nullptr) {
        // Shut down (or misconfigured: completions need the main thread).
        tab.setError(mainDispatcher_ == nullptr ? "no main-thread dispatcher configured"
                                                : "the workspace is shutting down");
        return;
    }
    // std::function needs a copyable callable: share the move-only token.
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));
    inFlight_.push_back(operation);

    // The task borrows engine_/scheduler_ by reference: safe because the
    // workspace's shutdown() waits for this task's token before the shell
    // destroys them. The password lives only in this task (never stored,
    // never logged) and is wiped before delivery.
    scheduler_.post([this, operation, token, openPath = tab.path(), alive = workspaceAlive_,
                     dispatcher = mainDispatcher_, password = std::move(password)]() mutable {
        if (!token->cancelled()) {
            auto created = editor::DocumentSession::create(engine_, scheduler_, dispatcher,
                                                           openPath, password);
            if (token->cancelled()) {
                // Nobody will take the session: destroy it here, while the
                // engine and scheduler are guaranteed alive (token held).
                created = std::unexpected(core::Error{core::ErrorCode::Cancelled,
                                                      "open cancelled", "app"});
            }
            operation->result = std::move(created);
        }
        std::fill(password.begin(), password.end(), '\0');
        std::string().swap(password);
        // Posted delivery holds the operation and a liveness flag only; the
        // workspace itself is touched after the flag check on the main
        // thread, where shutdown() also runs.
        dispatcher->post([this, operation, alive] {
            if (!alive->load(std::memory_order_acquire)) return;
            handleOpenCompleted(operation);
        });
        token.reset(); // leave the scope last: everything above is done
    });
}

void DocumentWorkspace::handleOpenCompleted(const std::shared_ptr<OpenOperation>& operation) {
    // Take ownership of the result out of the in-flight list first; whatever
    // happens next, this operation is finished.
    std::erase(inFlight_, operation);
    if (!operation->result.has_value()) return; // cancelled before running

    DocumentTab* tab = tabById(operation->tab);
    if (tab == nullptr || tab->openRequest() != operation->request ||
        tab->state() != DocumentTab::State::Loading) {
        // The tab was closed, or a newer request superseded this one: drop
        // the result (the session dies here, on the main thread).
        return;
    }

    core::Result<std::unique_ptr<editor::DocumentSession>>& session = *operation->result;
    if (session.has_value()) {
        tab->attachSession(std::move(*session));
        core::log::info(std::string("document opened: ") + std::to_string(tab->session()->pageCount()) +
                        " pages");
    } else if (session.error().code == core::ErrorCode::PasswordRequired) {
        // Prompt instead of failing. A retry that fails again re-enters this
        // state (the UI keeps the field focused). No password is stored or
        // logged.
        core::log::warning("document open requires a password");
        tab->markNeedsPassword(session.error().message);
    } else {
        core::log::warning("document open failed: " + core::describe(session.error()));
        // Never include document contents in user text; describe() is a
        // sanitized error string.
        tab->setError(core::describe(session.error()));
    }
    // The shell must rebind its viewer widgets when the active tab's content
    // changed (Loading -> Ready/Error), not just on tab switches.
    const bool isActive = activeIndex_ != kNoTab && tabs_[activeIndex_]->id() == operation->tab;
    fireTabsChanged();
    if (isActive && onActiveTabChanged_) onActiveTabChanged_();
}

void DocumentWorkspace::closeTab(std::size_t index) {
    if (index >= tabs_.size()) return;
    // The closed tab (and its session) stays alive until the host hooks
    // below have run: the host still has views bound to it (viewport,
    // thumbnails) and unbinds them from onActiveTabChanged. Destroying the
    // session first would leave those views cancelling work on a freed
    // renderer.
    const std::unique_ptr<DocumentTab> closed = std::move(tabs_[index]);
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

DocumentTab* DocumentWorkspace::tabById(TabId id) {
    const std::size_t index = indexOfTab(id);
    return index == kNoTab ? nullptr : tabs_[index].get();
}

std::size_t DocumentWorkspace::indexOfTab(TabId id) const {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->id() == id) return i;
    }
    return kNoTab;
}

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
