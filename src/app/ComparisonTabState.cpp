// SPDX-License-Identifier: MPL-2.0
#include "app/ComparisonTabState.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <format>
#include <limits>
#include <utility>

namespace rivet::app {

struct ComparisonTabState::Opened {
    std::optional<core::Result<std::unique_ptr<editor::DocumentSession>>> left;
    std::optional<core::Result<std::unique_ptr<editor::DocumentSession>>> right;
};

std::unique_ptr<ComparisonTabState> ComparisonTabState::start(const Environment& environment,
                                                              std::filesystem::path leftPath,
                                                              std::filesystem::path rightPath) {
    std::unique_ptr<ComparisonTabState> state(
        new ComparisonTabState(environment, std::move(leftPath), std::move(rightPath)));
    state->begin();
    return state;
}

ComparisonTabState::ComparisonTabState(const Environment& environment, std::filesystem::path leftPath,
                                       std::filesystem::path rightPath)
    : environment_(environment), leftPath_(std::move(leftPath)), rightPath_(std::move(rightPath)) {}

ComparisonTabState::~ComparisonTabState() {
    // Completions already queued on the dispatcher become no-ops; running
    // workers notice the cancellation and are waited for BEFORE the sessions
    // they read are released (members die in reverse declaration order).
    alive_->store(false, std::memory_order_release);
    latestGeneration_->store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
    scope_.closeAndWait();
}

void ComparisonTabState::notify() {
    if (onChanged_) onChanged_();
}

void ComparisonTabState::fail(std::string text) {
    result_.reset();
    right_.reset();
    left_.reset();
    phase_ = Phase::Failed;
    errorText_ = std::move(text);
    notify();
}

void ComparisonTabState::begin() {
    if (environment_.engine == nullptr || environment_.scheduler == nullptr ||
        environment_.dispatcher == nullptr) {
        fail("comparison is not available");
        return;
    }
    std::optional<core::AsyncScope::Token> entered = scope_.enter();
    if (!entered.has_value()) {
        fail("comparison is shutting down");
        return;
    }
    const std::uint64_t generation = ++generation_;
    latestGeneration_->store(generation, std::memory_order_release);
    opened_ = std::make_shared<Opened>();
    // std::function needs a copyable callable: share the move-only token.
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));

    environment_.scheduler->post([opened = opened_, token, env = environment_, left = leftPath_,
                                  right = rightPath_, alive = alive_, generation, this]() mutable {
        if (!token->cancelled()) {
            opened->left = editor::DocumentSession::create(*env.engine, *env.scheduler, env.dispatcher, left);
            if (opened->left->has_value() && !token->cancelled()) {
                opened->right =
                    editor::DocumentSession::create(*env.engine, *env.scheduler, env.dispatcher, right);
            }
        }
        if (token->cancelled()) {
            // Nobody will take them: release the documents here, while the
            // engine and scheduler are guaranteed alive (token held).
            opened->left.reset();
            opened->right.reset();
        }
        env.dispatcher->post([this, alive, generation] {
            if (!alive->load(std::memory_order_acquire)) return;
            onOpened(generation);
        });
        opened.reset();
        token.reset(); // leave the scope last
    });
}

void ComparisonTabState::onOpened(std::uint64_t generation) {
    if (generation != generation_ || phase_ != Phase::Opening) return;
    const std::shared_ptr<Opened> opened = std::move(opened_);
    if (opened == nullptr || !opened->left.has_value()) {
        fail("comparison was cancelled");
        return;
    }
    const auto describeFailure = [](const core::Error& error, const std::filesystem::path& path) {
        if (error.code == core::ErrorCode::PasswordRequired) {
            return std::format("{} is password protected", path.filename().string());
        }
        return std::format("Could not open {}: {}", path.filename().string(), core::describe(error));
    };
    if (!opened->left->has_value()) {
        fail(describeFailure(opened->left->error(), leftPath_));
        return;
    }
    if (!opened->right.has_value()) {
        fail("comparison was cancelled");
        return;
    }
    if (!opened->right->has_value()) {
        fail(describeFailure(opened->right->error(), rightPath_));
        return;
    }
    left_ = std::move(**opened->left);
    right_ = std::move(**opened->right);
    core::log::info(std::format("comparison: documents open ({} and {} pages)", left_->pageCount(),
                                right_->pageCount()));
    phase_ = Phase::Comparing;
    notify();
    startDiff();
}

void ComparisonTabState::recompare() {
    if (left_ == nullptr || right_ == nullptr) return;
    result_.reset();
    phase_ = Phase::Comparing;
    notify();
    startDiff();
}

void ComparisonTabState::startDiff() {
    std::optional<core::AsyncScope::Token> entered = scope_.enter();
    if (!entered.has_value()) return;
    const std::uint64_t generation = ++generation_;
    latestGeneration_->store(generation, std::memory_order_release);
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));

    environment_.scheduler->post(
        [token, env = environment_, leftSnapshot = left_->pageSnapshot(),
         rightSnapshot = right_->pageSnapshot(), leftText = &left_->textService(),
         rightText = &right_->textService(), alive = alive_, latest = latestGeneration_, generation,
         this]() mutable {
            const auto cancelled = [&token, &latest, generation] {
                return token->cancelled() || latest->load(std::memory_order_acquire) != generation;
            };
            auto result = std::make_shared<editor::ComparisonResult>(
                editor::compareDocuments(leftSnapshot, *leftText, rightSnapshot, *rightText, cancelled));
            leftSnapshot.reset();
            rightSnapshot.reset();
            env.dispatcher->post([this, alive, generation, result] {
                if (!alive->load(std::memory_order_acquire)) return;
                onDiffFinished(generation, std::move(*result));
            });
            result.reset();
            token.reset(); // leave the scope last
        });
}

void ComparisonTabState::onDiffFinished(std::uint64_t generation, editor::ComparisonResult result) {
    if (generation != generation_ || result.cancelled) return; // superseded
    result_ = std::move(result);
    phase_ = Phase::Ready;
    core::log::info(std::format("comparison: {} difference(s)", result_->changes.size()));
    notify();
}

} // namespace rivet::app
