// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "PlatformFakes.hpp"

#include <functional>
#include <utility>

using rivet::platform::IAppLifecycle;
using rivet::platform::ISaveDialog;
using rivet::platform::LifecycleHooks;
using rivet::platform::ReviewChangesChoice;
using rivet::platform::SaveChangesChoice;
using rivet::platform::ShellServices;
using rivet::platform::testing::FakeAlertService;
using rivet::platform::testing::FakeFileDialog;
using rivet::platform::testing::FakeLifecycleHost;
using Decision = LifecycleHooks::QuitDecision;

RIVET_TEST(shellServicesNewMembersDefaultToNull) {
    const ShellServices services;
    CHECK(services.saveDialog == nullptr);
    CHECK(services.alerts == nullptr);
    CHECK(services.lifecycle == nullptr);
    CHECK(!services.setDocumentEdited);
}

RIVET_TEST(closeAndQuitProceedWithoutHandlers) {
    FakeLifecycleHost host;
    CHECK(host.simulateWindowClose());
    CHECK(host.simulateQuit() == Decision::Proceed);
    CHECK(!host.hooks.quitPending());
}

RIVET_TEST(closeHandlerDecides) {
    FakeLifecycleHost host;
    bool allow = false;
    int calls = 0;
    host.hooks.setCloseRequestHandler([&] {
        ++calls;
        return allow;
    });
    CHECK(!host.simulateWindowClose());
    allow = true;
    CHECK(host.simulateWindowClose());
    CHECK_EQ(calls, 2);
    host.hooks.clearHandlers();
    CHECK(host.simulateWindowClose());
    CHECK_EQ(calls, 2);
}

RIVET_TEST(quitSynchronousAnswers) {
    FakeLifecycleHost host;
    bool answer = false;
    host.hooks.setQuitRequestHandler([&](IAppLifecycle::QuitReply reply) { reply(answer); });
    CHECK(host.simulateQuit() == Decision::Cancel);
    answer = true;
    CHECK(host.simulateQuit() == Decision::Proceed);
    CHECK_EQ(host.asyncReplies, 0); // sync answers never use the async path
    CHECK(!host.hooks.quitPending());
}

RIVET_TEST(quitAsynchronousReplyIsDeliveredOnce) {
    FakeLifecycleHost host;
    IAppLifecycle::QuitReply stored;
    host.hooks.setQuitRequestHandler([&](IAppLifecycle::QuitReply reply) { stored = std::move(reply); });
    CHECK(host.simulateQuit() == Decision::Pending);
    CHECK(host.hooks.quitPending());
    CHECK(!host.quitAnswer.has_value());

    // A second request while the first is pending is declined immediately.
    CHECK(host.simulateQuit() == Decision::Cancel);
    CHECK(host.hooks.quitPending());

    // Re-arm to observe the first request's async reply.
    host.quitAnswer.reset();
    stored(true);
    CHECK_EQ(host.asyncReplies, 1);
    CHECK(host.quitAnswer.has_value() && *host.quitAnswer);
    CHECK(!host.hooks.quitPending());
    stored(false); // ignored
    CHECK_EQ(host.asyncReplies, 1);
}

RIVET_TEST(quitDroppedReplyCountsAsCancel) {
    FakeLifecycleHost host;
    // Handler that ignores the reply entirely: declined synchronously.
    host.hooks.setQuitRequestHandler([](IAppLifecycle::QuitReply) {});
    CHECK(host.simulateQuit() == Decision::Cancel);
    CHECK(!host.hooks.quitPending());

    // Handler that keeps the reply, then drops it unanswered later.
    IAppLifecycle::QuitReply stored;
    host.hooks.setQuitRequestHandler([&](IAppLifecycle::QuitReply reply) { stored = std::move(reply); });
    CHECK(host.simulateQuit() == Decision::Pending);
    stored = nullptr;
    CHECK_EQ(host.asyncReplies, 1);
    CHECK(host.quitAnswer.has_value() && !*host.quitAnswer);
    CHECK(!host.hooks.quitPending());

    // The hooks are usable again afterwards.
    host.hooks.setQuitRequestHandler([](IAppLifecycle::QuitReply reply) { reply(true); });
    CHECK(host.simulateQuit() == Decision::Proceed);
}

RIVET_TEST(quitHandlerMayReplaceItself) {
    FakeLifecycleHost host;
    host.hooks.setQuitRequestHandler([&host](IAppLifecycle::QuitReply reply) {
        host.hooks.setQuitRequestHandler(nullptr);
        reply(false);
    });
    CHECK(host.simulateQuit() == Decision::Cancel);
    CHECK(host.simulateQuit() == Decision::Proceed);
}

RIVET_TEST(fakesDriveAnUnsavedChangesFlow) {
    // Sketch of the app-layer quit flow against the fakes: ask, then save as.
    FakeLifecycleHost host;
    FakeAlertService alerts;
    FakeFileDialog dialogs;
    ShellServices services;
    host.wire(services);
    services.alerts = &alerts;
    services.saveDialog = &dialogs;

    services.setDocumentEdited(true);
    CHECK(host.edited);

    alerts.saveChangesAnswers.push_back(SaveChangesChoice::Save);
    dialogs.saveAnswers.emplace_back(std::filesystem::path("/tmp/out.pdf"));
    services.lifecycle->setQuitRequestHandler([&](IAppLifecycle::QuitReply reply) {
        switch (services.alerts->askSaveChanges("Report.pdf")) {
            case SaveChangesChoice::Cancel: reply(false); return;
            case SaveChangesChoice::DontSave: reply(true); return;
            case SaveChangesChoice::Save: break;
        }
        ISaveDialog::Options options;
        options.suggestedName = "Report.pdf";
        const auto chosen = services.saveDialog->runSavePanel(options);
        reply(chosen.has_value());
    });
    CHECK(host.simulateQuit() == Decision::Proceed);
    CHECK_EQ(alerts.askedDocumentTitles.size(), std::size_t{1});
    CHECK_EQ(alerts.askedDocumentTitles[0], std::string("Report.pdf"));
    CHECK_EQ(dialogs.saveRequests.size(), std::size_t{1});
    CHECK_EQ(dialogs.saveRequests[0].suggestedName, std::string("Report.pdf"));

    // Empty queues: alert answers Cancel, so quitting is refused.
    CHECK(host.simulateQuit() == Decision::Cancel);
    CHECK(alerts.askReviewUnsavedChanges(3) == ReviewChangesChoice::Cancel);
    alerts.showError("Save failed", "disk full");
    CHECK_EQ(alerts.errors.size(), std::size_t{1});
}

RIVET_TEST(defaultOpenPdfsFallsBackToSingleOpen) {
    struct SingleOnly final : rivet::platform::IFileDialog {
        rivet::core::Result<std::filesystem::path> openPdf() override {
            return std::filesystem::path("/tmp/a.pdf");
        }
    } single;
    rivet::platform::IFileDialog::OpenOptions options;
    options.allowMultiple = true;
    const auto chosen = single.openPdfs(options);
    CHECK(chosen.has_value());
    CHECK_EQ(chosen->size(), std::size_t{1});

    FakeFileDialog fake;
    fake.openAnswers.push_back({"/a.pdf", "/b.pdf"});
    const auto multi = fake.openPdfs(options);
    CHECK(multi.has_value() && multi->size() == 2);
    CHECK(!fake.openPdf().has_value()); // queue empty -> cancelled
}
