// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace rivet::platform {

// Window/application lifecycle interception, registered by the app layer.
//
// Threading: main thread only - handlers are invoked on the platform main
// thread, and the QuitReply must be called (or dropped) there as well.
class IAppLifecycle {
public:
    // Return true to let the window close, false to keep it open (e.g. the
    // user chose Cancel in an unsaved-changes prompt). Must decide
    // synchronously (an app-modal prompt is fine).
    using CloseRequestHandler = std::function<bool()>;

    // Answers a pending quit: true = terminate, false = keep running. Call at
    // most once (later calls are ignored). Dropping every copy without
    // calling it counts as false, so a lost reply never wedges termination.
    using QuitReply = std::function<void(bool proceed)>;

    // Invoked when the user/system asks the app to quit. May reply
    // synchronously from inside the handler or later (after async saves).
    using QuitRequestHandler = std::function<void(QuitReply reply)>;

    virtual ~IAppLifecycle() = default;

    // Empty handler = no interception (close/quit proceed immediately).
    virtual void setCloseRequestHandler(CloseRequestHandler handler) = 0;
    virtual void setQuitRequestHandler(QuitRequestHandler handler) = 0;
};

// Portable plumbing behind IAppLifecycle. A platform backend owns one and
// forwards its native events:
//   windowShouldClose:          -> dispatchCloseRequest()
//   applicationShouldTerminate: -> dispatchQuitRequest(asyncReply), mapping
//        Proceed -> NSTerminateNow, Cancel -> NSTerminateCancel,
//        Pending -> NSTerminateLater (asyncReply later calls
//        replyToApplicationShouldTerminate:).
// Header-only and free of platform APIs so it can be unit-tested headlessly.
class LifecycleHooks final : public IAppLifecycle {
public:
    enum class QuitDecision : std::uint8_t { Proceed, Cancel, Pending };

    void setCloseRequestHandler(CloseRequestHandler handler) override {
        closeHandler_ = std::move(handler);
    }
    void setQuitRequestHandler(QuitRequestHandler handler) override {
        quitHandler_ = std::move(handler);
    }

    // Drops both handlers (call before the objects they capture die).
    void clearHandlers() {
        closeHandler_ = nullptr;
        quitHandler_ = nullptr;
    }

    // True = close the window.
    bool dispatchCloseRequest() const { return closeHandler_ ? closeHandler_() : true; }

    // Asks the app whether to quit. A synchronous answer is returned
    // directly and `asyncReply` is never called. Otherwise returns Pending
    // and `asyncReply` is called exactly once later with the decision.
    // A quit request while another is still pending is declined (Cancel):
    // the first prompt is still on screen.
    QuitDecision dispatchQuitRequest(std::function<void(bool proceed)> asyncReply) {
        if (!quitHandler_) return QuitDecision::Proceed;
        if (*pending_) return QuitDecision::Cancel;

        auto state = std::make_shared<QuitState>();
        state->pending = pending_;
        *pending_ = true;
        {
            // Copy the handler: it may replace itself while running.
            const QuitRequestHandler handler = quitHandler_;
            handler([state](bool proceed) { state->deliver(proceed); });
        }
        if (state->answered) {
            *pending_ = false;
            return state->proceed ? QuitDecision::Proceed : QuitDecision::Cancel;
        }
        if (state.use_count() == 1) {
            // The handler kept no reply: nobody can answer, so decline now
            // (answering asynchronously before Pending is returned would
            // violate the platform contract).
            state->answered = true;
            *pending_ = false;
            return QuitDecision::Cancel;
        }
        state->synchronous = false;
        state->asyncReply = std::move(asyncReply);
        return QuitDecision::Pending;
    }

    bool quitPending() const { return *pending_; }

private:
    struct QuitState {
        std::shared_ptr<bool> pending;
        std::function<void(bool)> asyncReply;
        bool synchronous = true;
        bool answered = false;
        bool proceed = false;

        void deliver(bool decision) {
            if (answered) return;
            answered = true;
            proceed = decision;
            if (synchronous) return; // picked up by dispatchQuitRequest
            *pending = false;
            if (asyncReply) {
                auto reply = std::move(asyncReply);
                asyncReply = nullptr;
                reply(decision);
            }
        }

        // Every reply copy was dropped unanswered: treat as "don't quit".
        ~QuitState() {
            if (!answered && !synchronous) deliver(false);
        }
    };

    CloseRequestHandler closeHandler_;
    QuitRequestHandler quitHandler_;
    std::shared_ptr<bool> pending_ = std::make_shared<bool>(false);
};

} // namespace rivet::platform
