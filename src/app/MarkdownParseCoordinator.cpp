// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownParseCoordinator.hpp"

#include "markdown/MarkdownParser.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace rivet::app {

struct MarkdownParseCoordinator::Shared {
    core::AsyncScope scope;
};

MarkdownParseCoordinator::MarkdownParseCoordinator(MarkdownParseEnvironment environment, MarkdownParseTuning tuning)
    : env_(std::move(environment)), tuning_(tuning), shared_(std::make_shared<Shared>()) {}

MarkdownParseCoordinator::~MarkdownParseCoordinator() {
    onParsed_ = {};
    snapshot_ = {};
    alive_.reset();
    shared_->scope.closeAndWait();
}

void MarkdownParseCoordinator::run(std::function<void()> job) {
    if (env_.runBackground) {
        env_.runBackground(std::move(job));
    } else if (env_.scheduler != nullptr) {
        env_.scheduler->post(std::move(job));
    }
}

void MarkdownParseCoordinator::bind(SnapshotFn snapshot) {
    unbind();
    snapshot_ = std::move(snapshot);
}

void MarkdownParseCoordinator::unbind() {
    snapshot_ = {};
    ++epoch_; // in-flight results and debounce wake-ups of the old binding are stale
    requestedRevision_ = 0;
    appliedRevision_ = 0;
    anyApplied_ = false;
    debouncePending_ = false;
    parsesInFlight_ = 0;
}

void MarkdownParseCoordinator::requestParse(std::uint64_t revision, Urgency urgency) {
    if (!snapshot_) return;
    requestedRevision_ = std::max(requestedRevision_, revision);
    const auto now = std::chrono::steady_clock::now();
    lastRequest_ = now;

    if (urgency == Urgency::Immediate) {
        debouncePending_ = false; // a pending wake-up becomes a no-op; older results fail the revision test
        Snapshot snap = snapshot_();
        if (snap.source.size() <= tuning_.synchronousParseBytes || !canRunAsync()) {
            parseNow(std::move(snap));
            return;
        }
        startParse(std::move(snap));
        return;
    }
    if (!canRunAsync()) {
        parseNow(snapshot_());
        return;
    }

    if (debouncePending_) return; // the armed wake-up reads the newest state when it fires
    debouncePending_ = true;
    firstPending_ = now;
    armDebounce(tuning_.debounce);
}

void MarkdownParseCoordinator::parseNow(Snapshot snap) {
    ++parsesStarted_;
    requestedRevision_ = std::max(requestedRevision_, snap.revision);
    auto document = std::make_shared<const markdown::MarkdownDocument>(markdown::makeMarkdownParser()->parse(snap.source));
    deliver(epoch_, MarkdownParseResult{std::move(document), snap.revision, std::move(snap.source)});
}

void MarkdownParseCoordinator::armDebounce(std::chrono::milliseconds delay) {
    std::shared_ptr<Shared> shared = shared_;
    core::IMainThreadDispatcher* dispatcher = env_.dispatcher;
    const std::weak_ptr<int> alive = alive_;
    const std::uint64_t epoch = epoch_;
    run([shared, dispatcher, alive, epoch, delay, this] {
        std::optional<core::AsyncScope::Token> token = shared->scope.enter();
        if (!token) return; // coordinator gone: touch nothing
        const auto until = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < until && !token->cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (token->cancelled() || dispatcher == nullptr) return;
        dispatcher->post([alive, epoch, this] {
            if (!alive.lock()) return; // destroyed meanwhile
            if (epoch != epoch_ || !debouncePending_) return;
            onDebounceFired();
        });
    });
}

void MarkdownParseCoordinator::onDebounceFired() {
    const auto now = std::chrono::steady_clock::now();
    // Trailing debounce: more edits arrived during the wait -> wait again for
    // the rest of the quiet period, unless the first unparsed edit is old.
    if (now - firstPending_ < tuning_.maxDebounce) {
        const auto quiet = now - lastRequest_;
        if (quiet < tuning_.debounce) {
            armDebounce(std::chrono::duration_cast<std::chrono::milliseconds>(tuning_.debounce - quiet) +
                        std::chrono::milliseconds(1));
            return;
        }
    }
    debouncePending_ = false;
    if (!snapshot_) return;
    startParse(snapshot_());
}

void MarkdownParseCoordinator::startParse(Snapshot snapshot) {
    requestedRevision_ = std::max(requestedRevision_, snapshot.revision);
    ++parsesStarted_;
    ++parsesInFlight_;
    std::shared_ptr<Shared> shared = shared_;
    core::IMainThreadDispatcher* dispatcher = env_.dispatcher;
    const std::weak_ptr<int> alive = alive_;
    const std::uint64_t epoch = epoch_;
    const std::uint64_t revision = snapshot.revision;
    run([shared, dispatcher, alive, epoch, revision, source = std::move(snapshot.source), this]() mutable {
        std::optional<core::AsyncScope::Token> token = shared->scope.enter();
        if (!token || token->cancelled()) return;
        auto document = std::make_shared<const markdown::MarkdownDocument>(
            markdown::makeMarkdownParser()->parse(source));
        if (token->cancelled() || dispatcher == nullptr) return;
        dispatcher->post([alive, epoch, revision, document = std::move(document), source = std::move(source),
                          this]() mutable {
            if (!alive.lock()) return;
            if (epoch == epoch_ && parsesInFlight_ > 0) --parsesInFlight_;
            deliver(epoch, MarkdownParseResult{std::move(document), revision, std::move(source)});
        });
    });
}

void MarkdownParseCoordinator::deliver(std::uint64_t epoch, MarkdownParseResult result) {
    const std::uint64_t revision = result.revision;
    // Immediate requests bump the epoch themselves, so their own epoch is current.
    const bool stale = epoch != epoch_ || revision != requestedRevision_ || (anyApplied_ && revision < appliedRevision_);
    if (stale) {
        ++resultsDropped_;
        return;
    }
    appliedRevision_ = revision;
    anyApplied_ = true;
    if (onParsed_) onParsed_(std::move(result));
}

} // namespace rivet::app
