// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/BackgroundRunner.hpp"
#include "app/DocumentSecurityHost.hpp"
#include "crypto/SecretBuffer.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace rivet::app {

// What the dialog hands over on "Protect". Move-only: every password is a
// SecretBuffer that is zeroed when the controller is done with it.
struct SetPasswordForm {
    crypto::SecretBuffer userPassword;
    crypto::SecretBuffer userPasswordConfirm;
    crypto::SecretBuffer ownerPassword;        // optional
    crypto::SecretBuffer ownerPasswordConfirm;
    pdf::encrypt::Cipher cipher = pdf::encrypt::Cipher::Aes256;
    pdf::encrypt::Permissions permissions;
    bool encryptMetadata = true;
};

struct SetPasswordDialogState {
    std::string documentTitle;
    pdf::encrypt::Cipher defaultCipher = pdf::encrypt::Cipher::Aes256;
};

// The dialog. Every method runs on the main thread.
class ISetPasswordView {
public:
    virtual ~ISetPasswordView() = default;
    // Opens the dialog with empty fields.
    virtual void showDialog(const SetPasswordDialogState& state) = 0;
    // A refusal that replaces the dialog (signed / already protected / ...).
    virtual void showBlocked(const std::string& message) = 0;
    virtual void showProgress(const std::string& message) = 0;
    // An error: the dialog stays and its password fields are empty again.
    virtual void showError(const std::string& message) = 0;
    // Success: closes the dialog.
    virtual void showDone(const std::string& message) = 0;
    virtual void close() = 0;
};

// Set Password (ADR-0048): writes a PASSWORD-PROTECTED COPY of the active
// document. Design choice: never in place. PDFium opens encrypted files but
// cannot write them, so the protected file is produced from the SAVED BYTES
// (ADR-0046): the existing save pipeline writes the current document state to
// a private temp file, encryptPdf() encrypts those bytes, and the result is
// written atomically to the chosen destination. The open tab keeps pointing at
// its unprotected file; the original is never modified.
//
// Refusals (stated before any password is asked for): the document is signed
// (encrypting rewrites every object and would invalidate the signatures), it
// is already password-protected, or its signature state is still loading.
//
// Threading: validation and dialogs on the main thread; the save, the
// encryption and the file write run on one worker (BackgroundRunner). The
// passwords live only in SecretBuffers; the std::string copies the encryptor
// API needs are zeroed right after the call and the buffers are cleared as
// soon as the worker finishes, on success, failure and cancellation alike.
//
// Main thread only.
class SetPasswordController {
public:
    using StatusSink = std::function<void(std::string)>;
    // Seam for tests; production uses pdf::encrypt::encryptPdf.
    using EncryptFn = std::function<core::Result<PdfBytes>(std::span<const std::uint8_t>,
                                                           const pdf::encrypt::EncryptOptions&)>;

    SetPasswordController(IDocumentSecurityHost& host, ISetPasswordView& view, core::TaskScheduler& scheduler,
                          core::IMainThreadDispatcher& dispatcher, StatusSink setStatus);
    ~SetPasswordController();

    SetPasswordController(const SetPasswordController&) = delete;
    SetPasswordController& operator=(const SetPasswordController&) = delete;

    void setEncryptFunction(EncryptFn fn) { encrypt_ = std::move(fn); }

    // Menu command: checks the document and opens the dialog (or says why not).
    // Returns true when the dialog was shown.
    bool begin();
    // "Protect": validates the form, asks for the destination and starts the work.
    void submit(SetPasswordForm form);
    // Cancel button / Esc. While the job runs it cancels the job (no file is
    // written); otherwise it just closes the dialog.
    void cancel();

    bool busy() const { return busy_; }
    // True while any password is held (tests: must be false once idle).
    bool holdsSecrets() const;

    // Static checks shared with the dialog: empty when the form is acceptable.
    static std::string validate(const SetPasswordForm& form);

private:
    struct Job;
    // The refusal text for the active document, empty when allowed.
    std::string refusalFor(const SecurityDocumentInfo& info) const;
    void finish(const std::shared_ptr<Job>& job);

    IDocumentSecurityHost& host_;
    ISetPasswordView& view_;
    StatusSink setStatus_;
    EncryptFn encrypt_;
    bool busy_ = false;
    std::shared_ptr<Job> job_;
    // Declared last: workers drain before the members above die.
    BackgroundRunner runner_;
};

} // namespace rivet::app
