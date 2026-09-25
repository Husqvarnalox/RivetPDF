// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace rivet::core {

// Lifetime fence for background operations that borrow their owner's
// resources (engine, scheduler, ...). An operation enters the scope before it
// is posted and holds the returned Token until its worker-side part is done;
// the owner calls closeAndWait() before it (or the borrowed resources) dies:
//
//   - closeAndWait() refuses new entries, flags cancellation, and blocks
//     until every outstanding Token is released. After it returns no worker
//     is inside an operation of this scope.
//   - Tokens are RAII: a task that is dropped without running (e.g. a
//     discarded scheduler queue) releases its token when the task object is
//     destroyed, so the wait cannot hang on work that will never run.
//   - cancelled() lets a worker skip or abandon work cheaply once the owner
//     is going away; it is advisory (work already inside a backend call runs
//     to completion, which the wait covers).
//
// The shared state is reference counted, so a Token stays valid even if it
// outlives the AsyncScope object itself.
//
// Thread-safe. closeAndWait() must not be called from inside an operation of
// the same scope (it would wait for itself).
class AsyncScope {
    struct State {
        std::mutex mutex;
        std::condition_variable idle;
        std::size_t active = 0;
        bool closed = false;
        bool cancelled = false;
    };

public:
    class Token {
    public:
        Token(Token&& other) noexcept : state_(std::move(other.state_)) {}
        Token& operator=(Token&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        ~Token() { release(); }

        bool cancelled() const {
            if (!state_) return true;
            std::lock_guard<std::mutex> lock(state_->mutex);
            return state_->cancelled;
        }

    private:
        friend class AsyncScope;
        explicit Token(std::shared_ptr<State> state) : state_(std::move(state)) {}

        void release() {
            if (!state_) return;
            std::shared_ptr<State> state = std::move(state_);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (--state->active == 0) state->idle.notify_all();
        }

        std::shared_ptr<State> state_;
    };

    AsyncScope() : state_(std::make_shared<State>()) {}
    ~AsyncScope() { closeAndWait(); }

    AsyncScope(const AsyncScope&) = delete;
    AsyncScope& operator=(const AsyncScope&) = delete;

    // A token for one operation, or nullopt once the scope is closed.
    std::optional<Token> enter() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) return std::nullopt;
        ++state_->active;
        return Token(state_);
    }

    void closeAndWait() {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->closed = true;
        state_->cancelled = true;
        state_->idle.wait(lock, [this] { return state_->active == 0; });
    }

    std::size_t activeCount() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->active;
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace rivet::core
