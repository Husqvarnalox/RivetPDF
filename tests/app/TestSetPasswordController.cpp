// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "SecurityTestKit.hpp"
#include "app/SetPasswordController.hpp"
#include "core/async/TaskScheduler.hpp"

#include <cstdio>

using namespace rivet;
using namespace rivet::app;
using namespace rivet::app::sectest;

namespace {

class FakeView final : public ISetPasswordView {
public:
    int dialogs = 0, blocked = 0, progress = 0, errors = 0, done = 0, closed = 0;
    std::string lastBlocked, lastError, lastDone;
    void showDialog(const SetPasswordDialogState&) override { ++dialogs; }
    void showBlocked(const std::string& m) override { ++blocked; lastBlocked = m; }
    void showProgress(const std::string&) override { ++progress; }
    void showError(const std::string& m) override { ++errors; lastError = m; }
    void showDone(const std::string& m) override { ++done; lastDone = m; }
    void close() override { ++closed; }
};

SetPasswordForm makeForm(const char* user, const char* userConfirm, const char* owner = "", const char* ownerConfirm = "") {
    SetPasswordForm f;
    f.userPassword = crypto::SecretBuffer(std::string_view(user));
    f.userPasswordConfirm = crypto::SecretBuffer(std::string_view(userConfirm));
    f.ownerPassword = crypto::SecretBuffer(std::string_view(owner));
    f.ownerPasswordConfirm = crypto::SecretBuffer(std::string_view(ownerConfirm));
    return f;
}

struct Rig {
    TempDir dir{"setpw"};
    QueueDispatcher dispatcher;
    core::TaskScheduler scheduler{2};
    FakeHost host;
    FakeView view;
    std::vector<std::string> statuses;
    std::unique_ptr<SetPasswordController> controller;
    std::string seenUser, seenOwner;
    int encryptCalls = 0;

    Rig() {
        host.info = readyDocument(dir.path / "source.pdf");
        host.destination = dir.path / "out.pdf";
        host.bytes = {'%', 'P', 'D', 'F', '-', '1', '.', '7'};
        controller = std::make_unique<SetPasswordController>(host, view, scheduler, dispatcher,
                                                             [this](std::string s) { statuses.push_back(std::move(s)); });
        controller->setEncryptFunction(
            [this](std::span<const std::uint8_t> in, const pdf::encrypt::EncryptOptions& o) -> core::Result<PdfBytes> {
                ++encryptCalls;
                seenUser = o.userPassword;
                seenOwner = o.ownerPassword;
                PdfBytes out(in.begin(), in.end());
                out.push_back('E');
                return out;
            });
    }
    bool settle() { return dispatcher.pumpUntil([&] { return !controller->busy(); }); }
};

} // namespace

RIVET_TEST(setPasswordRefusesSignedDocumentBeforeAskingAnything) {
    Rig r;
    r.host.info = readyDocument(r.dir.path / "source.pdf", true);
    CHECK(!r.controller->begin());
    CHECK_EQ(r.view.blocked, 1);
    CHECK_EQ(r.view.dialogs, 0);
    CHECK(r.view.lastBlocked.find("signatures") != std::string::npos);
    r.controller->submit(makeForm("pw", "pw"));
    CHECK_EQ(r.view.blocked, 2);
    CHECK_EQ(r.host.destinationCalls, 0);
    CHECK_EQ(r.encryptCalls, 0);
}

RIVET_TEST(setPasswordRefusesEncryptedAndUnknownDocuments) {
    Rig r;
    r.host.info.encrypted = true;
    CHECK(!r.controller->begin());
    CHECK(r.view.lastBlocked.find("already password-protected") != std::string::npos);
    r.host.info.encrypted = false;
    r.host.info.signatures = SecurityDocumentInfo::Signatures::Unknown;
    CHECK(!r.controller->begin());
    CHECK(r.view.lastBlocked.find("still checking") != std::string::npos);
    r.host.info.available = false;
    CHECK(!r.controller->begin());
    CHECK_EQ(r.view.blocked, 3);
    CHECK_EQ(r.view.dialogs, 0);
}

RIVET_TEST(setPasswordValidateCatchesMismatchesAndEmpty) {
    CHECK(SetPasswordController::validate(makeForm("a", "b")).find("do not match") != std::string::npos);
    CHECK(SetPasswordController::validate(makeForm("a", "a", "o", "p")).find("owner") != std::string::npos);
    CHECK(!SetPasswordController::validate(makeForm("", "")).empty());
    CHECK(!SetPasswordController::validate(makeForm("same", "same", "same", "same")).empty());
    CHECK(SetPasswordController::validate(makeForm("a", "a")).empty());
    SetPasswordForm restricted = makeForm("", "");
    restricted.permissions.print = false;
    CHECK(SetPasswordController::validate(restricted).empty());
}

RIVET_TEST(setPasswordWritesAtomicallyAndClearsSecrets) {
    Rig r;
    CHECK(r.controller->begin());
    r.controller->submit(makeForm("open-me", "open-me", "owner-pw", "owner-pw"));
    CHECK(r.controller->busy());
    CHECK(r.settle());
    CHECK_EQ(r.view.done, 1);
    CHECK_EQ(r.view.errors, 0);
    CHECK(!r.controller->holdsSecrets());
    CHECK_EQ(r.seenUser, std::string("open-me"));
    CHECK_EQ(r.seenOwner, std::string("owner-pw"));
    CHECK(fileExists(r.host.destination.value()));
    const auto written = readAll(*r.host.destination);
    CHECK_EQ(written.size(), r.host.bytes.size() + 1);
    CHECK(written.back() == 'E');
    // The open document's file is never touched.
    CHECK(!fileExists(r.dir.path / "source.pdf"));
    CHECK_GE(r.host.commits, 1);
}

RIVET_TEST(setPasswordEncryptFailureShowsErrorAndWritesNothing) {
    Rig r;
    r.controller->setEncryptFunction([](std::span<const std::uint8_t>, const pdf::encrypt::EncryptOptions&)
                                         -> core::Result<PdfBytes> {
        return std::unexpected(core::makeError(core::ErrorCode::Unsupported, "no", "t"));
    });
    r.controller->submit(makeForm("pw", "pw"));
    CHECK(r.settle());
    CHECK_EQ(r.view.errors, 1);
    CHECK(!fileExists(*r.host.destination));
    CHECK(!r.controller->holdsSecrets());
}

RIVET_TEST(setPasswordDestinationCancelledLeavesDialogAndDoesNothing) {
    Rig r;
    r.host.destination.reset();
    r.controller->submit(makeForm("pw", "pw"));
    CHECK(!r.controller->busy());
    CHECK_EQ(r.host.captureCalls, 0);
    CHECK_EQ(r.view.done + r.view.errors + r.view.closed, 0);
}

RIVET_TEST(setPasswordRefusesDestinationEqualToSource) {
    Rig r;
    const auto source = r.dir.path / "source.pdf";
    {
        std::FILE* f = std::fopen(source.string().c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("x", f);
        std::fclose(f);
    }
    r.host.destination = source;
    r.controller->submit(makeForm("pw", "pw"));
    CHECK(!r.controller->busy());
    CHECK_EQ(r.view.errors, 1);
    CHECK_EQ(r.host.captureCalls, 0);
}

RIVET_TEST(setPasswordCancelMidJobWritesNoFile) {
    Rig r;
    std::atomic<bool> release{false};
    r.host.producerGate = [&release] { return release.load(); };
    r.controller->submit(makeForm("pw", "pw"));
    CHECK(r.controller->busy());
    r.controller->cancel();
    CHECK(r.settle());
    CHECK_EQ(r.view.closed, 1);
    CHECK_EQ(r.view.done, 0);
    CHECK(!fileExists(*r.host.destination));
    CHECK_EQ(r.encryptCalls, 0);
    CHECK(!r.controller->holdsSecrets());
    release = true;
}

RIVET_TEST(setPasswordCancelWhenIdleJustCloses) {
    Rig r;
    r.controller->cancel();
    CHECK_EQ(r.view.closed, 1);
}
