// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/Error.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rivet::net {

// User setting for online validation (TSA / OCSP / CRL fetches).
enum class OnlineValidationMode : std::uint8_t {
    Off,  // never touch the network
    Ask,  // ask once per (host, session)
    On,   // proceed
};

struct ConsentRequest {
    std::string host;
    std::string purpose; // optional human-readable reason, e.g. "certificate revocation check"
};

// UI-facing prompt. `answer` may be invoked from any thread, at most once,
// now or later; never invoking it is treated as a refusal only when the
// request is cancelled.
class IConsentPrompt {
public:
    virtual ~IConsentPrompt() = default;
    virtual void ask(const ConsentRequest& request, std::function<void(bool allow)> answer) = 0;
};

// Default prompt: denies immediately (nothing is contacted without a UI that
// can ask the user).
class DenyConsentPrompt final : public IConsentPrompt {
public:
    void ask(const ConsentRequest&, std::function<void(bool)> answer) override { answer(false); }
};

// Every caller of the network layer consults this gate (NetworkClient does
// so for every host it is about to contact, redirect hops included).
// Thread-safe; the prompt is never called under a lock.
class OnlineValidationGate {
public:
    explicit OnlineValidationGate(OnlineValidationMode mode = OnlineValidationMode::Ask,
                                  std::shared_ptr<IConsentPrompt> prompt = nullptr);

    OnlineValidationMode mode() const { return mode_.load(std::memory_order_acquire); }
    void setMode(OnlineValidationMode mode) { mode_.store(mode, std::memory_order_release); }
    void setPrompt(std::shared_ptr<IConsentPrompt> prompt);

    // Off -> OnlineValidationOff; Ask -> (cached) consent decision, blocking
    // the calling worker thread until answered or cancelled; On -> ok.
    core::Status authorize(const std::string& host, const std::string& purpose, const CancelToken& cancel);

    // Forgets remembered per-host decisions (new session).
    void resetSession();

private:
    struct Decision {
        bool pending = true;
        bool allow = false;
    };
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::string, Decision> decisions;
    };

    std::atomic<OnlineValidationMode> mode_;
    std::mutex promptMutex_;
    std::shared_ptr<IConsentPrompt> prompt_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

} // namespace rivet::net
