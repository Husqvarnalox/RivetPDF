// SPDX-License-Identifier: MPL-2.0
#include "net/Consent.hpp"

#include <chrono>

namespace rivet::net {

OnlineValidationGate::OnlineValidationGate(OnlineValidationMode mode, std::shared_ptr<IConsentPrompt> prompt)
    : mode_(mode), prompt_(prompt ? std::move(prompt) : std::make_shared<DenyConsentPrompt>()) {}

void OnlineValidationGate::setPrompt(std::shared_ptr<IConsentPrompt> prompt) {
    std::lock_guard lock(promptMutex_);
    prompt_ = prompt ? std::move(prompt) : std::make_shared<DenyConsentPrompt>();
}

void OnlineValidationGate::resetSession() {
    std::lock_guard lock(shared_->mutex);
    for (auto it = shared_->decisions.begin(); it != shared_->decisions.end();) {
        // Keep in-flight prompts; their answer still has to land somewhere.
        it = it->second.pending ? std::next(it) : shared_->decisions.erase(it);
    }
}

core::Status OnlineValidationGate::authorize(const std::string& host, const std::string& purpose,
                                             const CancelToken& cancel) {
    switch (mode()) {
        case OnlineValidationMode::Off:
            return std::unexpected(
                makeNetError(NetFailure::OnlineValidationOff, "online validation is turned off in the settings"));
        case OnlineValidationMode::On: return core::ok();
        case OnlineValidationMode::Ask: break;
    }

    bool mustAsk = false;
    {
        std::lock_guard lock(shared_->mutex);
        auto [it, inserted] = shared_->decisions.try_emplace(host);
        mustAsk = inserted;
        if (!inserted && !it->second.pending) {
            return it->second.allow ? core::ok()
                                    : core::Status(std::unexpected(makeNetError(
                                          NetFailure::ConsentDenied, "connection to '" + host + "' was declined")));
        }
    }

    if (mustAsk) {
        std::shared_ptr<IConsentPrompt> prompt;
        {
            std::lock_guard lock(promptMutex_);
            prompt = prompt_;
        }
        std::shared_ptr<Shared> shared = shared_;
        std::string key = host;
        // The answer callback may outlive this call; it only touches `shared`.
        prompt->ask(ConsentRequest{host, purpose}, [shared, key](bool allow) {
            {
                std::lock_guard lock(shared->mutex);
                auto& d = shared->decisions[key];
                if (!d.pending) return; // answered twice: first wins
                d.pending = false;
                d.allow = allow;
            }
            shared->cv.notify_all();
        });
    }

    std::unique_lock lock(shared_->mutex);
    while (true) {
        const auto it = shared_->decisions.find(host);
        if (it != shared_->decisions.end() && !it->second.pending) {
            const bool allow = it->second.allow;
            lock.unlock();
            return allow ? core::ok()
                         : core::Status(std::unexpected(makeNetError(NetFailure::ConsentDenied,
                                                                     "connection to '" + host + "' was declined")));
        }
        if (it == shared_->decisions.end()) {
            // The request that asked was cancelled and withdrew the prompt.
            lock.unlock();
            return std::unexpected(makeNetError(NetFailure::ConsentDenied, "consent prompt was dismissed"));
        }
        if (cancel.cancelled()) {
            // An unanswered prompt of a cancelled request must not wedge later
            // requests: drop the pending marker (a late answer re-creates it).
            if (mustAsk && it->second.pending) shared_->decisions.erase(it);
            lock.unlock();
            return std::unexpected(makeNetError(NetFailure::Cancelled, "operation cancelled"));
        }
        shared_->cv.wait_for(lock, std::chrono::milliseconds(20));
    }
}

} // namespace rivet::net
