// SPDX-License-Identifier: MPL-2.0
#include "app/SetPasswordController.hpp"

#include "app/SecurityText.hpp"
#include "core/SecureWipe.hpp"
#include "core/io/AtomicFileWriter.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace rivet::app {

namespace {

constexpr std::size_t kMaxPasswordBytes = 1024;

bool sameBytes(const crypto::SecretBuffer& a, const crypto::SecretBuffer& b) {
    const auto x = a.bytes();
    const auto y = b.bytes();
    return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size()) == 0);
}

// std::string copy of a secret for the encryptor API; zeroed by the caller.
std::string toString(const crypto::SecretBuffer& secret) {
    return std::string(reinterpret_cast<const char*>(secret.bytes().data()), secret.size());
}

core::Error cancelledError() { return core::makeError(core::ErrorCode::Cancelled, "cancelled", "app"); }

} // namespace

struct SetPasswordController::Job {
    crypto::SecretBuffer user;
    crypto::SecretBuffer owner;
    pdf::encrypt::Cipher cipher = pdf::encrypt::Cipher::Aes256;
    pdf::encrypt::Permissions permissions;
    bool encryptMetadata = true;
    std::filesystem::path destination;
    PlainPdfProducer producer;
    std::atomic<bool> cancel{false};
    std::optional<core::Status> result; // written by the worker, read after delivery
    // Tracks whether the buffers still hold anything; only the worker (and the
    // main thread once the worker is done) touch the buffers, so the main
    // thread reads this flag instead of the buffers.
    std::atomic<bool> secretsHeld{true};
    bool hasSecrets() const { return secretsHeld.load(std::memory_order_acquire); }
    void clearSecrets() {
        user.clear();
        owner.clear();
        secretsHeld.store(false, std::memory_order_release);
    }
};

SetPasswordController::SetPasswordController(IDocumentSecurityHost& host, ISetPasswordView& view,
                                             core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                                             StatusSink setStatus)
    : host_(host), view_(view), setStatus_(std::move(setStatus)),
      encrypt_([](std::span<const std::uint8_t> pdf, const pdf::encrypt::EncryptOptions& options) {
          return pdf::encrypt::encryptPdf(pdf, options);
      }),
      runner_(scheduler, dispatcher) {}

SetPasswordController::~SetPasswordController() {
    if (job_ != nullptr) job_->cancel.store(true);
    // runner_ (declared last) drains the worker first; the job's buffers are
    // zeroed by their destructors.
}

bool SetPasswordController::holdsSecrets() const { return job_ != nullptr && job_->hasSecrets(); }

std::string SetPasswordController::refusalFor(const SecurityDocumentInfo& info) const {
    if (!info.available) return "Open a PDF document first.";
    if (info.encrypted) {
        return "This document is already password-protected. Use “Remove Password and Save As” first if you "
               "want to set a different password.";
    }
    switch (info.signatures) {
    case SecurityDocumentInfo::Signatures::Present:
        return "This document contains digital signatures. Setting a password rewrites the whole file and would "
               "invalidate them, so it is not allowed. To protect a signed document, set the password first and sign "
               "afterwards.";
    case SecurityDocumentInfo::Signatures::Unknown:
        return "Rivet is still checking this document for digital signatures. Try again in a moment.";
    case SecurityDocumentInfo::Signatures::None: break;
    }
    return {};
}

bool SetPasswordController::begin() {
    if (busy_) {
        setStatus_("A password-protected copy is already being written");
        return false;
    }
    host_.commitPendingEdits();
    const SecurityDocumentInfo info = host_.activeDocument();
    if (const std::string refusal = refusalFor(info); !refusal.empty()) {
        view_.showBlocked(refusal);
        return false;
    }
    SetPasswordDialogState state;
    state.documentTitle = info.title;
    view_.showDialog(state);
    return true;
}

std::string SetPasswordController::validate(const SetPasswordForm& form) {
    if (form.userPassword.size() > kMaxPasswordBytes || form.ownerPassword.size() > kMaxPasswordBytes) {
        return "A password can be at most 1024 bytes long.";
    }
    if (!sameBytes(form.userPassword, form.userPasswordConfirm)) return "The two passwords to open the file do not match.";
    if (!sameBytes(form.ownerPassword, form.ownerPasswordConfirm)) return "The two owner passwords do not match.";
    if (form.userPassword.empty() && form.ownerPassword.empty() && form.permissions.allAllowed()) {
        return "Enter a password, or restrict at least one permission.";
    }
    if (!form.ownerPassword.empty() && sameBytes(form.ownerPassword, form.userPassword)) {
        return "The owner password must differ from the password to open the file.";
    }
    return {};
}

void SetPasswordController::submit(SetPasswordForm form) {
    if (busy_) return;
    if (const std::string problem = validate(form); !problem.empty()) {
        view_.showError(problem);
        return;
    }
    host_.commitPendingEdits();
    SecurityDocumentInfo info = host_.activeDocument();
    if (const std::string refusal = refusalFor(info); !refusal.empty()) {
        view_.showBlocked(refusal);
        return;
    }
    std::string stem = info.path.stem().string();
    if (stem.empty()) stem = info.title.empty() ? "Untitled" : info.title;
    const std::optional<std::filesystem::path> chosen =
        host_.chooseDestination(stem + "-protected.pdf", "Save Password-Protected Copy");
    if (!chosen.has_value()) return; // cancelled: the dialog stays, nothing changed
    std::filesystem::path destination = *chosen;
    if (destination.extension().empty()) destination.replace_extension(".pdf");
    std::error_code ec;
    if (!info.path.empty() && std::filesystem::exists(destination, ec) &&
        std::filesystem::equivalent(destination, info.path, ec)) {
        view_.showError("Choose a different file name: the protected copy never replaces the open file.");
        return;
    }
    // The save panel is modal and may have run completions: re-check.
    const SecurityDocumentInfo again = host_.activeDocument();
    if (!again.available || again.tab != info.tab) {
        view_.showBlocked("The document was closed or changed while choosing the file. Nothing was written.");
        return;
    }
    if (const std::string refusal = refusalFor(again); !refusal.empty()) {
        view_.showBlocked(refusal);
        return;
    }
    core::Result<PlainPdfProducer> producer = host_.captureSave(info.tab);
    if (!producer.has_value()) {
        view_.showError(describeEncryptError(producer.error()));
        return;
    }

    auto job = std::make_shared<Job>();
    job->user = std::move(form.userPassword);
    job->owner = std::move(form.ownerPassword);
    form.userPasswordConfirm.clear();
    form.ownerPasswordConfirm.clear();
    job->cipher = form.cipher;
    job->permissions = form.permissions;
    job->encryptMetadata = form.encryptMetadata;
    job->destination = destination;
    job->producer = std::move(*producer);
    job_ = job;
    busy_ = true;
    view_.showProgress("Writing the password-protected copy…");
    setStatus_("Setting password…");

    const bool started = runner_.post(
        [job, this] {
            const auto cancelled = [&job] { return job->cancel.load(); };
            const auto run = [&]() -> core::Status {
                if (cancelled()) return std::unexpected(cancelledError());
                core::Result<PdfBytes> plain = job->producer(cancelled);
                if (!plain.has_value()) return std::unexpected(plain.error());
                if (cancelled()) return std::unexpected(cancelledError());
                core::Result<PdfBytes> sealed = [&] {
                    pdf::encrypt::EncryptOptions options;
                    // Zeroed when this block ends, also if the encryptor throws.
                    core::ScopedWipe wipeUser(options.userPassword);
                    core::ScopedWipe wipeOwner(options.ownerPassword);
                    options.cipher = job->cipher;
                    options.userPassword = toString(job->user);
                    options.ownerPassword = toString(job->owner);
                    options.permissions = job->permissions;
                    options.encryptMetadata = job->encryptMetadata;
                    return encrypt_(*plain, options);
                }();
                job->clearSecrets(); // no longer needed: do not keep them through the file write
                if (!sealed.has_value()) return std::unexpected(sealed.error());
                if (cancelled()) return std::unexpected(cancelledError());
                core::io::AtomicWriteOptions write;
                write.cancelFlag = &job->cancel;
                return core::io::writeFileAtomically(
                    job->destination,
                    [&](core::io::IByteSink& out) { return out.write(sealed->data(), sealed->size()); }, write);
            };
            job->result = run();
            job->clearSecrets();
        },
        [job, this] { finish(job); });
    if (!started) {
        job->clearSecrets();
        job_.reset();
        busy_ = false;
    }
}

void SetPasswordController::finish(const std::shared_ptr<Job>& job) {
    job->clearSecrets();
    busy_ = false;
    const core::Status status = job->result.value_or(std::unexpected(cancelledError()));
    const std::string name = job->destination.filename().string();
    job_.reset();
    if (status.has_value()) {
        const std::string text = "Saved a password-protected copy: " + name;
        setStatus_(text);
        view_.showDone(text);
        return;
    }
    if (status.error().code == core::ErrorCode::Cancelled) {
        setStatus_("Setting the password was cancelled");
        view_.close();
        return;
    }
    const std::string message = describeEncryptError(status.error());
    setStatus_(message);
    view_.showError(message);
}

void SetPasswordController::cancel() {
    if (busy_ && job_ != nullptr) {
        job_->cancel.store(true);
        view_.showProgress("Cancelling…");
        return;
    }
    view_.close();
}

} // namespace rivet::app
