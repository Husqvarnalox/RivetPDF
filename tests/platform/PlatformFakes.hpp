// SPDX-License-Identifier: MPL-2.0
#pragma once

// Scriptable fakes of the platform shell services for headless tests of the
// app layer (link rivet::platform_fakes). Main-thread/single-thread use only.

#include "platform/PlatformKit.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::platform::testing {

// Open + save dialogs. Queue the answers; an empty queue means "cancelled".
class FakeFileDialog final : public IFileDialog, public ISaveDialog {
public:
    std::deque<std::vector<std::filesystem::path>> openAnswers;
    std::deque<std::optional<std::filesystem::path>> saveAnswers;
    std::vector<IFileDialog::OpenOptions> openRequests;
    std::vector<ISaveDialog::Options> saveRequests;

    core::Result<std::filesystem::path> openPdf() override {
        auto chosen = openPdfs(OpenOptions{});
        if (!chosen) return std::unexpected(chosen.error());
        return chosen->front();
    }

    core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) override {
        openRequests.push_back(options);
        if (openAnswers.empty() || openAnswers.front().empty()) {
            if (!openAnswers.empty()) openAnswers.pop_front();
            return std::unexpected(
                core::makeError(core::ErrorCode::Cancelled, "cancelled", "platform.fake"));
        }
        auto answer = std::move(openAnswers.front());
        openAnswers.pop_front();
        if (!options.allowMultiple && answer.size() > 1) answer.resize(1);
        return answer;
    }

    std::optional<std::filesystem::path> runSavePanel(const ISaveDialog::Options& options) override {
        saveRequests.push_back(options);
        if (saveAnswers.empty()) return std::nullopt;
        auto answer = std::move(saveAnswers.front());
        saveAnswers.pop_front();
        return answer;
    }
};

// Alerts. Queue the answers; an empty queue answers Cancel (the safe choice).
class FakeAlertService final : public IAlertService {
public:
    std::deque<SaveChangesChoice> saveChangesAnswers;
    std::deque<ReviewChangesChoice> reviewAnswers;
    std::vector<std::string> askedDocumentTitles;
    std::vector<std::size_t> askedReviewCounts;
    std::vector<std::pair<std::string, std::string>> errors; // (title, message)

    SaveChangesChoice askSaveChanges(std::string_view documentTitle) override {
        askedDocumentTitles.emplace_back(documentTitle);
        if (saveChangesAnswers.empty()) return SaveChangesChoice::Cancel;
        const SaveChangesChoice answer = saveChangesAnswers.front();
        saveChangesAnswers.pop_front();
        return answer;
    }

    ReviewChangesChoice askReviewUnsavedChanges(std::size_t unsavedDocumentCount) override {
        askedReviewCounts.push_back(unsavedDocumentCount);
        if (reviewAnswers.empty()) return ReviewChangesChoice::Cancel;
        const ReviewChangesChoice answer = reviewAnswers.front();
        reviewAnswers.pop_front();
        return answer;
    }

    void showError(std::string_view title, std::string_view message) override {
        errors.emplace_back(std::string(title), std::string(message));
    }
};

// Plays the platform side of the lifecycle: the test calls the simulate*()
// methods where AppKit would deliver windowShouldClose: /
// applicationShouldTerminate:, and inspects what the "platform" was told.
class FakeLifecycleHost {
public:
    LifecycleHooks hooks;

    // Result of the most recent quit request as the platform sees it.
    std::optional<bool> quitAnswer; // set for sync answers and async replies
    int asyncReplies = 0;
    bool edited = false;
    std::string title;

    bool simulateWindowClose() { return hooks.dispatchCloseRequest(); }

    LifecycleHooks::QuitDecision simulateQuit() {
        quitAnswer.reset();
        const auto decision = hooks.dispatchQuitRequest([this](bool proceed) {
            ++asyncReplies;
            quitAnswer = proceed;
        });
        if (decision != LifecycleHooks::QuitDecision::Pending) {
            quitAnswer = decision == LifecycleHooks::QuitDecision::Proceed;
        }
        return decision;
    }

    // Services wired to this host (plus optional dialogs/alerts).
    void wire(ShellServices& services) {
        services.lifecycle = &hooks;
        services.setDocumentEdited = [this](bool value) { edited = value; };
        services.setWindowTitle = [this](const std::string& value) { title = value; };
    }
};

} // namespace rivet::platform::testing
