// SPDX-License-Identifier: MPL-2.0
#pragma once

// Digital-signature workflow (ADR-0048): identity and PAdES level choice,
// consent-gated time-stamping, PIN prompting, progress/cancel and an atomic
// "Save As signed copy". Portable: the view is an interface, the work runs on
// the background runner, every callback arrives on the main thread.

#include "app/BackgroundRunner.hpp"
#include "app/DocumentSecurityHost.hpp"
#include "crypto/IdentityStore.hpp"
#include "crypto/PinPrompt.hpp"
#include "crypto/Providers.hpp"
#include "net/Client.hpp"
#include "net/Consent.hpp"
#include "pdf/sign/PdfSigner.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::crypto {
class AccreditedCaList;
}

namespace rivet::app {

// What the signing and validation controllers need from the process.
struct SecurityServices {
    std::shared_ptr<crypto::CryptoProvider> provider;
    std::vector<std::shared_ptr<crypto::IdentityStore>> stores;
    std::shared_ptr<net::OnlineValidationGate> gate;     // may be null: no online access at all
    std::shared_ptr<net::NetworkClient> network;         // shares `gate`
    bool networkAvailable = false;                       // a transport exists in this build
    std::shared_ptr<const crypto::TrustAnchorSet> anchors;           // signer chain anchors (may be empty)
    std::shared_ptr<const crypto::TrustAnchorSet> timestampAnchors;  // TSA chain anchors (may be empty)
    std::shared_ptr<const crypto::AccreditedCaList> accredited;      // may be null
};

struct SigningIdentityChoice {
    std::size_t storeIndex = 0;
    std::string id;
    std::string label;       // "Jane Doe (Example Ltd)"
    std::string detail;      // "issued by CA, valid until ..., stored in <place>"
    std::string custody;     // "Software key", "Hardware token", "Remote service", ...
    bool usable = true;
    std::string unusableReason;
    crypto::ProviderAssurance assurance = crypto::ProviderAssurance::Software;
};

struct SigningDialogState {
    std::string documentTitle;
    std::vector<SigningIdentityChoice> identities;
    std::string identityProblem; // listing failed / no identity, "" when fine
    pdf::sign::PadesLevel defaultLevel = pdf::sign::PadesLevel::BaselineB;
    net::OnlineValidationMode onlineMode = net::OnlineValidationMode::Ask;
    bool networkAvailable = false;
    std::size_t pageCount = 0;
    std::size_t currentPage = 0;
    std::string tsaUrlDefault;
};

struct SigningForm {
    std::size_t identityIndex = 0; // into SigningDialogState::identities
    pdf::sign::PadesLevel level = pdf::sign::PadesLevel::BaselineB;
    std::string tsaUrl;            // https only; used when the level needs a time-stamp
    bool visible = false;
    std::size_t page = 0;
    std::array<double, 4> rect{36, 36, 236, 96};
    std::string reason;
    std::string location;
};

class ISigningView {
public:
    virtual ~ISigningView() = default;
    virtual void showDialog(const SigningDialogState& state) = 0;
    virtual void showBlocked(const std::string& message) = 0;
    virtual void showProgress(const std::string& message) = 0;
    virtual void showError(const std::string& message) = 0; // dialog stays open
    virtual void showDone(const std::string& message) = 0;
    virtual void close() = 0;
    // Both answer exactly once, from the main thread (or never when the job is abandoned).
    virtual void askPin(const crypto::PinRequest& request,
                        std::function<void(std::optional<crypto::SecretBuffer>)> answer) = 0;
    virtual void askConsent(const net::ConsentRequest& request, std::function<void(bool)> answer) = 0;
};

// Gate prompt that forwards to a view on the main thread. Safe against the
// view going away: after detach() every question is answered "no".
class ConsentPromptAdapter final : public net::IConsentPrompt {
public:
    using Ask = std::function<void(const net::ConsentRequest&, std::function<void(bool)>)>;
    ConsentPromptAdapter(core::IMainThreadDispatcher& dispatcher, Ask ask);
    void ask(const net::ConsentRequest& request, std::function<void(bool allow)> answer) override;
    void detach();

private:
    core::IMainThreadDispatcher& dispatcher_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<Ask> ask_;
};

class DigitalSignatureController {
public:
    using StatusSink = std::function<void(std::string)>;
    using SignFn = std::function<pdf::sign::SignResult<pdf::sign::SignedPdf>(
        pdf::sign::Bytes, const pdf::sign::SignRequest&, crypto::CryptoProvider&, crypto::SigningIdentity&)>;
    using ReadFileFn = std::function<core::Result<PdfBytes>(const std::filesystem::path&)>;

    DigitalSignatureController(IDocumentSecurityHost& host, ISigningView& view, SecurityServices services,
                               core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                               StatusSink setStatus);
    ~DigitalSignatureController();

    DigitalSignatureController(const DigitalSignatureController&) = delete;
    DigitalSignatureController& operator=(const DigitalSignatureController&) = delete;

    void setSignFunction(SignFn fn) { sign_ = std::move(fn); }
    void setReadFileFunction(ReadFileFn fn) { readFile_ = std::move(fn); }

    // Menu command: checks the document, lists identities, opens the dialog.
    void begin();
    void submit(const SigningForm& form);
    // Cancel button / Esc: cancels a running job (the original and any chosen
    // destination stay untouched) or closes the dialog.
    void cancel();

    bool busy() const { return busy_ || listing_; }

    // Form checks shared with the dialog: empty when acceptable.
    static std::string validate(const SigningForm& form, const SigningDialogState& state);
    // TSA URL rule: https only, a host, no credentials, bounded length.
    static bool isAcceptableTsaUrl(const std::string& url);

private:
    struct Job;
    struct Listing;
    struct Bridge;
    std::string refusalFor(const SecurityDocumentInfo& info) const;
    void finish(const std::shared_ptr<Job>& job);
    void presentDialog(const SecurityDocumentInfo& info, const std::shared_ptr<Listing>& listing);

    IDocumentSecurityHost& host_;
    ISigningView& view_;
    SecurityServices services_;
    StatusSink setStatus_;
    SignFn sign_;
    ReadFileFn readFile_;
    std::shared_ptr<Bridge> bridge_;
    SigningDialogState state_;
    std::vector<SigningIdentityChoice> choices_;
    bool busy_ = false;
    bool listing_ = false;
    std::shared_ptr<Job> job_;
    // Declared last: workers drain before the members above die.
    BackgroundRunner runner_;
};

} // namespace rivet::app
