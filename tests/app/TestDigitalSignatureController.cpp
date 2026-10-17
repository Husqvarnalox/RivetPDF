// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "SecurityTestKit.hpp"
#include "TestSupport.hpp"
#include "app/DigitalSignatureController.hpp"
#include "app/SecurityText.hpp"
#include "core/async/TaskScheduler.hpp"
#include "crypto/Validation.hpp"
#include "net/MockTransport.hpp"

#include <atomic>
#include <cstdio>

using namespace rivet;
using namespace rivet::app;
using namespace rivet::app::sectest;
using rivet::pdf::sign::PadesLevel;

namespace {

crypto::testing::TestKey makeKey(const char* cn) {
    crypto::testing::CertSpec spec;
    spec.cn = cn;
    return crypto::testing::makeCert(spec);
}

crypto::testing::TestKey& sharedKey() {
    static crypto::testing::TestKey key = makeKey("Sign Test");
    return key;
}

// A software-like store backed by one runtime-generated key. `pin` non-empty:
// open() asks for it (3 attempts).
class FakeStore final : public crypto::IdentityStore {
public:
    FakeStore(std::shared_ptr<crypto::CryptoProvider> p, crypto::ProviderAssurance a) : provider(std::move(p)), assur(a) {}
    std::shared_ptr<crypto::CryptoProvider> provider;
    crypto::ProviderAssurance assur;
    std::string pin;
    bool keyAvailable = true;
    std::atomic<int> pinAsks{0};

    std::string_view name() const override { return "fake"; }
    bool isAvailable() const override { return true; }
    crypto::ProviderAssurance assurance() const override { return assur; }
    crypto::CryptoResult<std::vector<crypto::IdentityDescriptor>> list(const crypto::CancelToken&) override {
        crypto::IdentityDescriptor d;
        d.id = "id-1";
        d.providerName = "fake";
        auto cert = provider->parseCertificate(crypto::testing::certDer(sharedKey()));
        if (!cert) return std::unexpected(cert.error());
        d.certificate = *cert;
        d.privateKeyAvailable = keyAvailable;
        return std::vector<crypto::IdentityDescriptor>{d};
    }
    crypto::CryptoResult<std::unique_ptr<crypto::SigningIdentity>> open(std::string_view, std::shared_ptr<crypto::PinPrompt> prompt,
                                                                       const crypto::CancelToken& cancel) override {
        if (!pin.empty()) {
            for (int attempt = 1; attempt <= 3; ++attempt) {
                crypto::PinRequest req;
                req.providerName = "fake";
                req.label = "token";
                req.attempt = attempt;
                req.reason = attempt == 1 ? crypto::PinPromptReason::Required : crypto::PinPromptReason::Incorrect;
                ++pinAsks;
                auto answer = prompt->requestPin(req, cancel);
                if (!answer.has_value()) {
                    return std::unexpected(crypto::CryptoError{crypto::CryptoErrorCode::Cancelled, "pin cancelled"});
                }
                const auto bytes = answer->bytes();
                if (std::string(bytes.begin(), bytes.end()) == pin) goto ok;
            }
            return std::unexpected(crypto::CryptoError{crypto::CryptoErrorCode::PinLocked, "locked"});
        }
    ok:
        auto id = crypto::testing::makeIdentity(*provider, sharedKey());
        if (!id) return std::unexpected(crypto::CryptoError{crypto::CryptoErrorCode::Internal, "no identity"});
        return id;
    }
};

class FakeView final : public ISigningView {
public:
    int dialogs = 0, blocked = 0, progress = 0, errors = 0, done = 0, closed = 0, consents = 0;
    std::string lastError, lastDone, lastBlocked;
    SigningDialogState state;
    std::vector<std::string> pins;       // scripted answers; "<cancel>" cancels
    std::vector<std::string> pinsSeen;
    bool consentAnswer = true;
    std::string consentHost;

    void showDialog(const SigningDialogState& s) override { ++dialogs; state = s; }
    void showBlocked(const std::string& m) override { ++blocked; lastBlocked = m; }
    void showProgress(const std::string&) override { ++progress; }
    void showError(const std::string& m) override { ++errors; lastError = m; }
    void showDone(const std::string& m) override { ++done; lastDone = m; }
    void close() override { ++closed; }
    void askPin(const crypto::PinRequest& r, std::function<void(std::optional<crypto::SecretBuffer>)> answer) override {
        pinsSeen.push_back(std::to_string(r.attempt));
        if (pins.empty()) { answer(std::nullopt); return; }
        const std::string next = pins.front();
        pins.erase(pins.begin());
        if (next == "<cancel>") answer(std::nullopt);
        else answer(crypto::SecretBuffer(std::string_view(next)));
    }
    void askConsent(const net::ConsentRequest& r, std::function<void(bool)> answer) override {
        ++consents;
        consentHost = r.host;
        answer(consentAnswer);
    }
};

struct Rig {
    TempDir dir{"sign"};
    QueueDispatcher dispatcher;
    core::TaskScheduler scheduler{2};
    FakeHost host;
    FakeView view;
    std::shared_ptr<FakeStore> store;
    std::shared_ptr<net::OnlineValidationGate> gate;
    std::shared_ptr<ConsentPromptAdapter> adapter;
    std::vector<std::string> statuses;
    std::unique_ptr<DigitalSignatureController> controller;
    std::atomic<int> signCalls{0};
    pdf::sign::SignRequest lastRequest;
    std::mutex requestMutex;
    std::function<void(const pdf::sign::SignRequest&)> signHook;

    explicit Rig(crypto::ProviderAssurance assurance = crypto::ProviderAssurance::Software,
                 net::OnlineValidationMode mode = net::OnlineValidationMode::Ask, bool timestampAnchors = true) {
        host.info = readyDocument(dir.path / "source.pdf");
        host.destination = dir.path / "out-signed.pdf";
        host.bytes = {'%', 'P', 'D', 'F'};
        SecurityServices services;
        services.provider = crypto::makeDefaultProvider();
        store = std::make_shared<FakeStore>(services.provider, assurance);
        services.stores = {store};
        gate = std::make_shared<net::OnlineValidationGate>(mode);
        services.gate = gate;
        services.network = std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, std::make_shared<net::MockTransport>(), gate);
        services.networkAvailable = true;
        auto tsa = std::make_shared<crypto::TrustAnchorSet>();
        if (timestampAnchors) {
            if (auto c = services.provider->parseCertificate(crypto::testing::certDer(sharedKey()))) tsa->add(*c);
        }
        services.timestampAnchors = tsa;
        auto anchors = std::make_shared<crypto::TrustAnchorSet>();
        if (auto c = services.provider->parseCertificate(crypto::testing::certDer(sharedKey()))) anchors->add(*c);
        services.anchors = anchors;
        adapter = std::make_shared<ConsentPromptAdapter>(
            dispatcher, [this](const net::ConsentRequest& r, std::function<void(bool)> a) { view.askConsent(r, std::move(a)); });
        gate->setPrompt(adapter);
        controller = std::make_unique<DigitalSignatureController>(host, view, std::move(services), scheduler, dispatcher,
                                                                  [this](std::string s) { statuses.push_back(std::move(s)); });
        controller->setSignFunction([this](pdf::sign::Bytes pdf, const pdf::sign::SignRequest& request, crypto::CryptoProvider&,
                                           crypto::SigningIdentity&) -> pdf::sign::SignResult<pdf::sign::SignedPdf> {
            ++signCalls;
            if (signHook) signHook(request);
            pdf::sign::SignedPdf out;
            out.inputSize = pdf.size();
            out.data = std::move(pdf);
            out.data.push_back('S');
            out.level = PadesLevel::BaselineB;
            return out;
        });
    }
    ~Rig() { adapter->detach(); }

    bool ready() {
        controller->begin();
        return dispatcher.pumpUntil([&] { return view.dialogs > 0 || view.blocked > 0; }) && view.dialogs > 0;
    }
    bool settle() { return dispatcher.pumpUntil([&] { return !controller->busy(); }); }
    SigningForm form(PadesLevel level = PadesLevel::BaselineB) {
        SigningForm f;
        f.level = level;
        if (level != PadesLevel::BaselineB) f.tsaUrl = "https://tsa.example.com/ts";
        return f;
    }
};

} // namespace

RIVET_TEST(signingRefusesProtectedAndUnknownAndDirtySigned) {
    Rig r;
    r.host.info.encrypted = true;
    r.controller->begin();
    CHECK_EQ(r.view.blocked, 1);
    CHECK(r.view.lastBlocked.find("password-protected") != std::string::npos);
    r.host.info.encrypted = false;
    r.host.info.signatures = SecurityDocumentInfo::Signatures::Unknown;
    r.controller->begin();
    CHECK_EQ(r.view.blocked, 2);
    r.host.info.signatures = SecurityDocumentInfo::Signatures::Present;
    r.host.info.dirty = true;
    r.controller->begin();
    CHECK_EQ(r.view.blocked, 3);
    CHECK(r.view.lastBlocked.find("unsaved") != std::string::npos);
    CHECK_EQ(r.view.dialogs, 0);
}

RIVET_TEST(signingListsIdentitiesAndMarksUnusableOnes) {
    Rig r;
    r.store->keyAvailable = false;
    CHECK(r.ready());
    CHECK_EQ(r.view.state.identities.size(), std::size_t{1});
    CHECK(!r.view.state.identities[0].usable);
    r.controller->submit(r.form());
    CHECK_EQ(r.view.errors, 1);
    CHECK_EQ(r.signCalls.load(), 0);
}

RIVET_TEST(signingBasicLevelWritesFileAtomically) {
    Rig r;
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK(r.controller->busy());
    CHECK(r.settle());
    CHECK_EQ(r.view.done, 1);
    CHECK_EQ(r.signCalls.load(), 1);
    CHECK(fileExists(*r.host.destination));
    CHECK(readAll(*r.host.destination).back() == 'S');
    CHECK(r.view.lastDone.find("Signed copy saved") != std::string::npos);
    CHECK(!fileExists(r.dir.path / "source.pdf"));
}

RIVET_TEST(signingTsaUrlMustBeHttpsAndOnlyChecksWhenLevelNeedsIt) {
    Rig r;
    CHECK(r.ready());
    SigningForm f = r.form(PadesLevel::BaselineT);
    f.tsaUrl = "http://tsa.example.com/ts";
    r.controller->submit(f);
    CHECK_EQ(r.view.errors, 1);
    CHECK(r.view.lastError.find("https://") != std::string::npos);
    CHECK_EQ(r.signCalls.load(), 0);
    // B-B ignores the field entirely.
    SigningForm b = r.form();
    b.tsaUrl = "garbage";
    r.controller->submit(b);
    CHECK(r.settle());
    CHECK_EQ(r.view.done, 1);
}

RIVET_TEST(signingConsentOffBlocksTimestampedLevelBeforeAnythingIsSigned) {
    Rig r(crypto::ProviderAssurance::Software, net::OnlineValidationMode::Off);
    CHECK(r.ready());
    r.controller->submit(r.form(PadesLevel::BaselineT));
    CHECK_EQ(r.view.errors, 1);
    CHECK(r.view.lastError.find("turned off") != std::string::npos);
    CHECK_EQ(r.host.destinationCalls, 0);
    CHECK_EQ(r.signCalls.load(), 0);
    CHECK_EQ(r.view.consents, 0);
}

RIVET_TEST(signingConsentAskPromptsOnMainThreadAndHonoursDeny) {
    Rig r(crypto::ProviderAssurance::Software, net::OnlineValidationMode::Ask);
    CHECK(r.ready());
    // The fake signer behaves like the time-stamp client: it asks the gate.
    std::atomic<int> authorizeOk{-1};
    r.signHook = [&](const pdf::sign::SignRequest&) {
        const auto st = r.gate->authorize("tsa.example.com", "time-stamp", net::CancelToken{});
        authorizeOk = st.has_value() ? 1 : 0;
    };
    r.view.consentAnswer = false;
    r.controller->submit(r.form(PadesLevel::BaselineT));
    CHECK(r.settle());
    CHECK_EQ(r.view.consents, 1);
    CHECK(r.view.consentHost == "tsa.example.com");
    CHECK_EQ(authorizeOk.load(), 0);
}

RIVET_TEST(signingConsentAskAllowAndOnProceed) {
    {
        Rig r(crypto::ProviderAssurance::Software, net::OnlineValidationMode::Ask);
        CHECK(r.ready());
        std::atomic<int> ok{-1};
        r.signHook = [&](const pdf::sign::SignRequest&) {
            ok = r.gate->authorize("tsa.example.com", "time-stamp", net::CancelToken{}).has_value() ? 1 : 0;
        };
        r.view.consentAnswer = true;
        r.controller->submit(r.form(PadesLevel::BaselineT));
        CHECK(r.settle());
        CHECK_EQ(r.view.consents, 1);
        CHECK_EQ(ok.load(), 1);
        CHECK_EQ(r.view.done, 1);
    }
    {
        Rig r(crypto::ProviderAssurance::Software, net::OnlineValidationMode::On);
        CHECK(r.ready());
        std::atomic<int> ok{-1};
        r.signHook = [&](const pdf::sign::SignRequest&) {
            ok = r.gate->authorize("tsa.example.com", "time-stamp", net::CancelToken{}).has_value() ? 1 : 0;
        };
        r.controller->submit(r.form(PadesLevel::BaselineT));
        CHECK(r.settle());
        CHECK_EQ(r.view.consents, 0);
        CHECK_EQ(ok.load(), 1);
    }
}

RIVET_TEST(signingTimestampLevelsNeedTimestampAnchors) {
    Rig r(crypto::ProviderAssurance::Software, net::OnlineValidationMode::On, /*timestampAnchors=*/false);
    CHECK(r.ready());
    r.controller->submit(r.form(PadesLevel::BaselineT));
    CHECK_EQ(r.view.errors, 1);
    CHECK(r.view.lastError.find("time-stamp authority") != std::string::npos);
    CHECK_EQ(r.signCalls.load(), 0);
}

RIVET_TEST(signingPinWrongThenRightSigns) {
    Rig r;
    r.store->pin = "1234";
    r.view.pins = {"0000", "1234"};
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK(r.settle());
    CHECK_EQ(r.view.done, 1);
    CHECK_EQ(r.store->pinAsks.load(), 2);
    CHECK_EQ(r.signCalls.load(), 1);
}

RIVET_TEST(signingPinWrongThreeTimesShowsErrorAndWritesNothing) {
    Rig r;
    r.store->pin = "1234";
    r.view.pins = {"1", "2", "3"};
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK(r.settle());
    CHECK_EQ(r.view.errors, 1);
    CHECK_EQ(r.signCalls.load(), 0);
    CHECK(!fileExists(*r.host.destination));
}

RIVET_TEST(signingPinCancelledClosesWithoutFile) {
    Rig r;
    r.store->pin = "1234";
    r.view.pins = {"<cancel>"};
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK(r.settle());
    CHECK_EQ(r.view.closed, 1);
    CHECK_EQ(r.view.done, 0);
    CHECK_EQ(r.signCalls.load(), 0);
    CHECK(!fileExists(*r.host.destination));
}

RIVET_TEST(signingCancelWhileCapturingWritesNothing) {
    Rig r;
    std::atomic<bool> release{false};
    r.host.producerGate = [&release] { return release.load(); };
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK(r.controller->busy());
    r.controller->cancel();
    CHECK(r.settle());
    CHECK_EQ(r.view.closed, 1);
    CHECK_EQ(r.signCalls.load(), 0);
    CHECK(!fileExists(*r.host.destination));
    release = true;
}

RIVET_TEST(signingCancelDuringSignKeepsOriginalAndWritesNothing) {
    Rig r;
    CHECK(r.ready());
    std::atomic<bool> inSign{false};
    r.signHook = [&](const pdf::sign::SignRequest& request) {
        inSign = true;
        for (int i = 0; i < 1000 && !request.cancel.cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    r.controller->submit(r.form());
    CHECK(r.dispatcher.pumpUntil([&] { return inSign.load(); }));
    r.controller->cancel();
    CHECK(r.settle());
    // The fake returned a "signed" result, but cancellation is honoured before the write.
    CHECK(!fileExists(*r.host.destination));
    CHECK_EQ(r.view.closed, 1);
}

RIVET_TEST(signingNeverReplacesTheSourceFile) {
    Rig r;
    {
        std::FILE* f = std::fopen((r.dir.path / "source.pdf").string().c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("x", f);
        std::fclose(f);
    }
    r.host.destination = r.dir.path / "source.pdf";
    CHECK(r.ready());
    r.controller->submit(r.form());
    CHECK_EQ(r.view.errors, 1);
    CHECK_EQ(r.signCalls.load(), 0);
}

RIVET_TEST(signingSignedDocumentIsSignedFromSavedFileNotResaved) {
    Rig r;
    r.host.info = readyDocument(r.dir.path / "source.pdf", /*signedDoc=*/true);
    {
        std::FILE* f = std::fopen((r.dir.path / "source.pdf").string().c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("ORIGINALBYTES", f);
        std::fclose(f);
    }
    CHECK(r.ready());
    std::atomic<std::size_t> seen{0};
    r.controller->setReadFileFunction([&](const std::filesystem::path& p) -> core::Result<PdfBytes> {
        seen = 1;
        return readAll(p);
    });
    r.controller->submit(r.form());
    CHECK(r.settle());
    CHECK_EQ(seen.load(), std::size_t{1});
    CHECK_EQ(r.host.captureCalls, 0);
    CHECK_EQ(r.view.done, 1);
}

RIVET_TEST(signingVisibleFormNeedsPositiveRectangleAndValidPage) {
    Rig r;
    CHECK(r.ready());
    SigningForm f = r.form();
    f.visible = true;
    f.rect = {10, 10, 5, 50};
    CHECK(!DigitalSignatureController::validate(f, r.view.state).empty());
    f.rect = {10, 10, 100, 50};
    f.page = 9;
    CHECK(!DigitalSignatureController::validate(f, r.view.state).empty());
    f.page = 1;
    CHECK(DigitalSignatureController::validate(f, r.view.state).empty());
}
