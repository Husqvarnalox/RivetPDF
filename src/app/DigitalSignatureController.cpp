// SPDX-License-Identifier: MPL-2.0
#include "app/DigitalSignatureController.hpp"

#include "app/SecurityText.hpp"
#include "core/io/AtomicFileWriter.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>

namespace rivet::app {

namespace {

std::string custodyText(crypto::ProviderAssurance assurance) {
    switch (assurance) {
    case crypto::ProviderAssurance::Software: return "Software key";
    case crypto::ProviderAssurance::PlatformKeystore: return "System keystore";
    case crypto::ProviderAssurance::HardwareToken: return "Hardware token";
    case crypto::ProviderAssurance::CertifiedCspDeclared: return "Cryptographic provider (CSP)";
    case crypto::ProviderAssurance::RemoteService: return "Remote signing service";
    }
    return "Unknown";
}

core::Result<PdfBytes> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot open the document", "app"));
    const std::streamoff size = in.tellg();
    if (size < 0) return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot size the document", "app"));
    in.seekg(0);
    PdfBytes bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot read the document", "app"));
    }
    return bytes;
}

} // namespace

// ---------------------------------------------------------------- consent

ConsentPromptAdapter::ConsentPromptAdapter(core::IMainThreadDispatcher& dispatcher, Ask ask)
    : dispatcher_(dispatcher), ask_(std::make_shared<Ask>(std::move(ask))) {}

void ConsentPromptAdapter::detach() { alive_->store(false, std::memory_order_release); }

void ConsentPromptAdapter::ask(const net::ConsentRequest& request, std::function<void(bool allow)> answer) {
    auto alive = alive_;
    auto ask = ask_;
    dispatcher_.post([alive, ask, request, answer = std::move(answer)]() mutable {
        if (!alive->load(std::memory_order_acquire) || !*ask) {
            answer(false);
            return;
        }
        (*ask)(request, std::move(answer));
    });
}

// ---------------------------------------------------------------- bridge

// The PIN prompt handed to identity stores: asks the view on the main thread
// and blocks the worker until the answer (or cancellation) arrives.
struct DigitalSignatureController::Bridge final : crypto::PinPrompt {
    struct Wait {
        std::mutex mutex;
        std::condition_variable cv;
        bool answered = false;
        std::optional<crypto::SecretBuffer> pin;
    };

    Bridge(ISigningView& v, core::IMainThreadDispatcher& d) : view(&v), dispatcher(d) {}

    std::optional<crypto::SecretBuffer> requestPin(const crypto::PinRequest& request,
                                                   const crypto::CancelToken& cancel) override {
        auto wait = std::make_shared<Wait>();
        auto aliveFlag = alive;
        dispatcher.post([this, wait, aliveFlag, request] {
            const auto reply = [wait](std::optional<crypto::SecretBuffer> pin) {
                std::lock_guard lock(wait->mutex);
                if (wait->answered) return;
                wait->pin = std::move(pin);
                wait->answered = true;
                wait->cv.notify_all();
            };
            if (!aliveFlag->load(std::memory_order_acquire)) {
                reply(std::nullopt);
                return;
            }
            view->askPin(request, reply); // never under a lock
        });
        std::unique_lock lock(wait->mutex);
        while (!wait->answered) {
            if (cancel.cancelled() || !alive->load(std::memory_order_acquire)) return std::nullopt;
            wait->cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return std::move(wait->pin);
    }

    ISigningView* view;
    core::IMainThreadDispatcher& dispatcher;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

struct DigitalSignatureController::Listing {
    std::vector<SigningIdentityChoice> choices;
    std::string problem;
};

struct DigitalSignatureController::Job {
    SigningIdentityChoice identity;
    SigningForm form;
    std::filesystem::path destination;
    std::string documentName;
    std::optional<PlainPdfProducer> producer; // set when the document must be captured (not signed yet)
    std::filesystem::path sourceFile;         // otherwise the unmodified original is signed
    crypto::CancelToken token;
    std::atomic<bool> cancel{false};
    std::optional<core::Status> ioResult;
    std::optional<pdf::sign::SignError> signError;
    pdf::sign::PadesLevel reached = pdf::sign::PadesLevel::NotPades;
    std::string downgrade;
    std::vector<std::string> notes;
    void requestCancel() {
        cancel.store(true);
        token.cancel();
    }
};

// ---------------------------------------------------------------- controller

DigitalSignatureController::DigitalSignatureController(IDocumentSecurityHost& host, ISigningView& view,
                                                       SecurityServices services, core::TaskScheduler& scheduler,
                                                       core::IMainThreadDispatcher& dispatcher, StatusSink setStatus)
    : host_(host), view_(view), services_(std::move(services)),
      setStatus_(std::move(setStatus)),
      sign_([](pdf::sign::Bytes pdf, const pdf::sign::SignRequest& request, crypto::CryptoProvider& provider,
               crypto::SigningIdentity& identity) { return pdf::sign::signPdf(std::move(pdf), request, provider, identity); }),
      readFile_(&readWholeFile), bridge_(std::make_shared<Bridge>(view, dispatcher)), runner_(scheduler, dispatcher) {}

DigitalSignatureController::~DigitalSignatureController() {
    bridge_->alive->store(false, std::memory_order_release);
    if (job_ != nullptr) job_->requestCancel();
}

bool DigitalSignatureController::isAcceptableTsaUrl(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() + 1 || url.size() > 2048) return false;
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        const char c = url[i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != scheme[i]) return false;
    }
    const std::string rest = url.substr(scheme.size());
    const std::size_t end = rest.find_first_of("/?#");
    const std::string authority = rest.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string::npos) return false; // no credentials
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return false;
    }
    return authority.front() != ':' && authority.front() != '.';
}

std::string DigitalSignatureController::validate(const SigningForm& form, const SigningDialogState& state) {
    if (form.identityIndex >= state.identities.size()) return "Choose the certificate to sign with.";
    const SigningIdentityChoice& id = state.identities[form.identityIndex];
    if (!id.usable) {
        return id.unusableReason.empty() ? "This certificate cannot be used for signing." : id.unusableReason;
    }
    if (levelNeedsTimestamp(form.level)) {
        if (!isAcceptableTsaUrl(form.tsaUrl)) {
            return "Enter the time-stamp server address. It must start with https:// (plain http is never used).";
        }
    }
    if (form.reason.size() > 500 || form.location.size() > 500) return "Reason and location can have at most 500 characters.";
    if (form.visible) {
        if (state.pageCount != 0 && form.page >= state.pageCount) return "Choose a page of the document for the visible signature.";
        const auto& r = form.rect;
        if (!(r[2] > r[0]) || !(r[3] > r[1])) return "The visible signature needs a rectangle with a positive size.";
    }
    return {};
}

std::string DigitalSignatureController::refusalFor(const SecurityDocumentInfo& info) const {
    if (!info.available) return "Open a PDF document first.";
    if (info.encrypted) {
        return "This document is password-protected. Rivet cannot sign a protected document yet, because saving "
               "a copy would drop the protection. Sign an unprotected version instead.";
    }
    if (info.signatures == SecurityDocumentInfo::Signatures::Unknown) {
        return "Rivet is still checking this document for digital signatures. Try again in a moment.";
    }
    if (info.signatures == SecurityDocumentInfo::Signatures::Present && (info.dirty || info.path.empty())) {
        return "This document is already signed and has unsaved changes. Rewriting it would invalidate the existing "
               "signatures. Save or discard the changes first, then sign again.";
    }
    if (!services_.provider || !services_.provider->isAvailable()) {
        return "This build of Rivet has no cryptographic backend, so it cannot create digital signatures.";
    }
    if (services_.stores.empty()) return "No certificate store is available on this system.";
    return {};
}

void DigitalSignatureController::begin() {
    if (busy()) {
        setStatus_("Signing is already in progress");
        return;
    }
    host_.commitPendingEdits();
    const SecurityDocumentInfo info = host_.activeDocument();
    if (const std::string refusal = refusalFor(info); !refusal.empty()) {
        view_.showBlocked(refusal);
        return;
    }
    listing_ = true;
    auto listing = std::make_shared<Listing>();
    auto services = services_;
    auto token = std::make_shared<crypto::CancelToken>();
    const bool started = runner_.post(
        [listing, services, token] {
            for (std::size_t s = 0; s < services.stores.size(); ++s) {
                crypto::IdentityStore& store = *services.stores[s];
                if (!store.isAvailable()) continue;
                auto listed = store.list(*token);
                if (!listed) {
                    if (listing->problem.empty()) {
                        listing->problem = std::string(store.name()) + ": " + listed.error().message;
                    }
                    continue;
                }
                for (const crypto::IdentityDescriptor& d : *listed) {
                    SigningIdentityChoice c;
                    c.storeIndex = s;
                    c.id = d.id;
                    c.label = d.certificate.commonName.value_or(d.certificate.subject);
                    if (d.certificate.organization) c.label += " (" + *d.certificate.organization + ")";
                    c.assurance = store.assurance();
                    c.custody = custodyText(c.assurance);
                    c.detail = certificateSummary(d.certificate, std::chrono::system_clock::now());
                    c.usable = d.privateKeyAvailable && !d.certificateExpired;
                    if (!d.privateKeyAvailable) c.unusableReason = "The private key for this certificate is not available.";
                    else if (d.certificateExpired) c.unusableReason = "This certificate has expired.";
                    listing->choices.push_back(std::move(c));
                }
            }
        },
        [this, listing, info] { presentDialog(info, listing); });
    if (!started) listing_ = false;
}

void DigitalSignatureController::presentDialog(const SecurityDocumentInfo& info,
                                               const std::shared_ptr<Listing>& listing) {
    listing_ = false;
    choices_ = listing->choices;
    state_ = SigningDialogState{};
    state_.documentTitle = info.title;
    state_.identities = choices_;
    state_.identityProblem = listing->problem;
    if (choices_.empty() && state_.identityProblem.empty()) {
        state_.identityProblem = "No signing certificate was found. Import a certificate into the system keystore "
                                 "or connect a token.";
    }
    state_.onlineMode = services_.gate ? services_.gate->mode() : net::OnlineValidationMode::Off;
    state_.networkAvailable = services_.networkAvailable && services_.network != nullptr;
    state_.pageCount = info.pageCount;
    state_.currentPage = info.currentPage;
    view_.showDialog(state_);
}

void DigitalSignatureController::submit(const SigningForm& form) {
    if (busy_ || listing_) return;
    if (const std::string problem = validate(form, state_); !problem.empty()) {
        view_.showError(problem);
        return;
    }
    if (levelNeedsNetwork(form.level)) {
        if (!state_.networkAvailable) {
            view_.showError("This build of Rivet has no network access, so levels that need a time-stamp server "
                            "are not available. Choose B-B.");
            return;
        }
        if (!services_.gate || services_.gate->mode() == net::OnlineValidationMode::Off) {
            view_.showError("Online validation is turned off in the settings. Turn it on (or set it to ask) to "
                            "use a time-stamp server, or choose B-B.");
            return;
        }
        if (!services_.timestampAnchors || services_.timestampAnchors->size() == 0) {
            view_.showError("No trusted time-stamp authority certificates are configured, so a time-stamp could not "
                            "be verified. Choose B-B or add the authority's certificate in the settings.");
            return;
        }
        if (form.level != pdf::sign::PadesLevel::BaselineT &&
            (!services_.anchors || services_.anchors->size() == 0)) {
            view_.showError("Long-term validation needs the trusted root certificates of your signing certificate. "
                            "None are configured. Choose B-T or B-B.");
            return;
        }
    }
    host_.commitPendingEdits();
    const SecurityDocumentInfo info = host_.activeDocument();
    if (const std::string refusal = refusalFor(info); !refusal.empty()) {
        view_.showBlocked(refusal);
        return;
    }
    std::string stem = info.path.stem().string();
    if (stem.empty()) stem = info.title.empty() ? "Untitled" : info.title;
    const std::optional<std::filesystem::path> chosen = host_.chooseDestination(stem + "-signed.pdf", "Save Signed Copy");
    if (!chosen.has_value()) return; // the dialog stays, nothing changed
    std::filesystem::path destination = *chosen;
    if (destination.extension().empty()) destination.replace_extension(".pdf");
    std::error_code ec;
    if (!info.path.empty() && std::filesystem::exists(destination, ec) &&
        std::filesystem::equivalent(destination, info.path, ec)) {
        view_.showError("Choose a different file name: the signed copy never replaces the open file.");
        return;
    }
    const SecurityDocumentInfo again = host_.activeDocument();
    if (!again.available || again.tab != info.tab) {
        view_.showBlocked("The document was closed or changed while choosing the file. Nothing was written.");
        return;
    }
    if (const std::string refusal = refusalFor(again); !refusal.empty()) {
        view_.showBlocked(refusal);
        return;
    }

    auto job = std::make_shared<Job>();
    job->identity = state_.identities[form.identityIndex];
    job->form = form;
    job->destination = destination;
    job->documentName = destination.filename().string();
    if (info.signatures == SecurityDocumentInfo::Signatures::Present) {
        job->sourceFile = info.path; // never re-saved: that would break the earlier signatures
    } else {
        core::Result<PlainPdfProducer> producer = host_.captureSave(info.tab);
        if (!producer.has_value()) {
            view_.showError(describeEncryptError(producer.error()));
            return;
        }
        job->producer = std::move(*producer);
    }
    job_ = job;
    busy_ = true;
    view_.showProgress("Signing…");
    setStatus_("Signing…");

    auto services = services_;
    auto bridge = bridge_;
    auto sign = sign_;
    auto readFile = readFile_;
    const bool started = runner_.post(
        [job, services, bridge, sign, readFile] {
            const auto cancelled = [&job] { return job->cancel.load(); };
            const auto fail = [&job](pdf::sign::SignError e) { job->signError = std::move(e); };
            const auto cancelErr = [&] {
                fail(pdf::sign::SignError{pdf::sign::SignErrorCode::Cancelled, "cancelled", std::nullopt});
            };
            // 1. the bytes to sign
            PdfBytes plain;
            if (job->producer.has_value()) {
                core::Result<PdfBytes> r = (*job->producer)(cancelled);
                if (!r.has_value()) {
                    if (r.error().code == core::ErrorCode::Cancelled) cancelErr();
                    else fail(pdf::sign::SignError{pdf::sign::SignErrorCode::Io, r.error().message, std::nullopt});
                    return;
                }
                plain = std::move(*r);
            } else {
                core::Result<PdfBytes> r = readFile(job->sourceFile);
                if (!r.has_value()) {
                    fail(pdf::sign::SignError{pdf::sign::SignErrorCode::Io, r.error().message, std::nullopt});
                    return;
                }
                plain = std::move(*r);
            }
            if (cancelled()) return cancelErr();

            // 2. the identity (may prompt for a PIN)
            auto opened = services.stores[job->identity.storeIndex]->open(job->identity.id, bridge, job->token);
            if (!opened) {
                fail(pdf::sign::failCrypto(opened.error()).error());
                return;
            }
            if (cancelled()) return cancelErr();

            // 3. the request
            const SigningForm& jobForm = job->form;
            pdf::sign::SignRequest request;
            request.profile = pdf::sign::SignatureProfile::PadesBaselineB;
            request.reason = jobForm.reason;
            request.location = jobForm.location;
            request.cancel = job->token;
            if (jobForm.visible) {
                request.placement.kind = pdf::sign::SignaturePlacement::Kind::NewVisible;
                request.placement.pageIndex = jobForm.page;
                request.placement.rect = jobForm.rect;
            } else {
                request.placement.kind = pdf::sign::SignaturePlacement::Kind::Invisible;
            }
            std::shared_ptr<crypto::TimeStampClient> tsClient;
            if (levelNeedsTimestamp(jobForm.level)) {
                tsClient = std::make_shared<crypto::TimeStampClient>(services.network, services.provider);
                const std::string url = jobForm.tsaUrl;
                const auto anchors = services.timestampAnchors;
                const auto fetcher = [tsClient, url, anchors](
                                         crypto::DigestAlgorithm alg, crypto::ByteView digest,
                                         const crypto::CancelToken& cancel)
                    -> crypto::CryptoResult<crypto::TimeStampVerification> {
                    crypto::TimeStampClientOptions o;
                    o.verify.anchors = anchors.get();
                    auto r = tsClient->request(url, alg, digest, o, cancel);
                    if (!r) return std::unexpected(r.error());
                    return std::move(r->verification);
                };
                request.timestamp.fetch = fetcher;
                request.timestamp.allowSignWithoutTimestamp = false; // never silently downgrade
                if (jobForm.level != pdf::sign::PadesLevel::BaselineB) request.targetLevel = jobForm.level;
                if (jobForm.level >= pdf::sign::PadesLevel::BaselineLT) {
                    request.ltv.anchors = services.anchors.get();
                    request.ltv.timestampAnchors = services.timestampAnchors.get();
                    request.ltv.network = services.network;
                    request.ltv.revocation.mode = crypto::RevocationMode::PreferOcspThenCrl;
                    request.ltv.cancel = job->token;
                }
                if (jobForm.level == pdf::sign::PadesLevel::BaselineLTA) request.archiveTimestamp = fetcher;
            }

            // 4. sign (in memory), then write atomically
            auto signed_ = sign(std::move(plain), request, *services.provider, **opened);
            if (!signed_) {
                fail(signed_.error());
                return;
            }
            job->reached = signed_->level;
            job->downgrade = signed_->levelDowngradeReason;
            job->notes = signed_->ltvNotes;
            if (cancelled()) return cancelErr();
            core::io::AtomicWriteOptions write;
            write.cancelFlag = &job->cancel;
            job->ioResult = core::io::writeFileAtomically(
                job->destination,
                [&](core::io::IByteSink& out) { return out.write(signed_->data.data(), signed_->data.size()); }, write);
        },
        [this, job] { finish(job); });
    if (!started) {
        job_.reset();
        busy_ = false;
    }
}

void DigitalSignatureController::finish(const std::shared_ptr<Job>& job) {
    busy_ = false;
    job_.reset();
    if (job->signError.has_value()) {
        if (job->signError->code == pdf::sign::SignErrorCode::Cancelled) {
            setStatus_("Signing was cancelled; no file was written");
            view_.close();
            return;
        }
        const std::string message = describeSignError(*job->signError);
        setStatus_(message);
        view_.showError(message);
        return;
    }
    if (!job->ioResult.has_value() || !job->ioResult->has_value()) {
        if (!job->ioResult.has_value() || job->ioResult->error().code == core::ErrorCode::Cancelled) {
            setStatus_("Signing was cancelled; no file was written");
            view_.close();
            return;
        }
        const std::string message = describeEncryptError(job->ioResult->error());
        setStatus_(message);
        view_.showError(message);
        return;
    }
    std::string text = "Signed copy saved: " + job->documentName;
    const std::string level = levelShortName(job->reached);
    if (job->reached != pdf::sign::PadesLevel::NotPades) text += " (PAdES " + level + ")";
    if (!job->downgrade.empty()) text += ". The requested level was not reached: " + job->downgrade;
    setStatus_(text);
    view_.showDone(text);
}

void DigitalSignatureController::cancel() {
    if (busy_ && job_ != nullptr) {
        job_->requestCancel();
        view_.showProgress("Cancelling…");
        return;
    }
    view_.close();
}

} // namespace rivet::app
