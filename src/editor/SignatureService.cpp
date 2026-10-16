// SPDX-License-Identifier: MPL-2.0
#include "editor/SignatureService.hpp"

#include "editor/DocumentSession.hpp"

#include <exception>

namespace rivet::editor {

SignatureService::SignatureService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

SignatureService::~SignatureService() {
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

SignatureService::State SignatureService::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

pdf::PdfSignatureListPtr SignatureService::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_;
}

bool SignatureService::hasSignatures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_ != nullptr && !list_->empty();
}

void SignatureService::ensureLoaded() {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::NotLoaded) return;
        state_ = State::Loading;
        generation = generation_;
    }
    executor_.post([this, alive = alive_, document = session_.documentPtr(), generation]() mutable {
        pdf::PdfSignatureListPtr list;
        try {
            auto loaded = document->signatures();
            if (loaded.has_value()) list = std::move(*loaded);
        } catch (const std::exception&) {
        } catch (...) {
        }
        // NotAvailable / load error: no signatures to show.
        if (list == nullptr) list = std::make_shared<const pdf::PdfSignatureList>();
        document.reset();
        if (dispatcher_ == nullptr) {
            complete(generation, std::move(list));
            return;
        }
        dispatcher_->post([this, alive, generation, list = std::move(list)]() mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            complete(generation, std::move(list));
        });
    });
}

void SignatureService::complete(std::uint64_t generation, pdf::PdfSignatureListPtr list) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || state_ != State::Loading) return; // rebased meanwhile
        list_ = std::move(list);
        state_ = State::Ready;
    }
    // Never called under the lock.
    if (onChanged_) {
        const auto callback = onChanged_; // may replace itself
        callback();
    }
}

void SignatureService::rebased() {
    bool reload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        reload = state_ != State::NotLoaded;
        state_ = State::NotLoaded;
        list_ = nullptr;
    }
    if (reload) ensureLoaded();
}

void SignatureService::waitForLoads() {
    executor_.waitUntilIdle();
}

} // namespace rivet::editor
