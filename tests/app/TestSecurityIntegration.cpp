// SPDX-License-Identifier: MPL-2.0
// Real signer + real encryptor behind the Phase 8 controllers (ADR-0048).
// The identity is generated at run time; nothing is read from disk or a keystore.
#include "RivetTest.h"

#include "EncryptTestSupport.hpp"
#include "SecurityTestKit.hpp"
#include "SignTestSupport.hpp"
#include "app/DigitalSignatureController.hpp"
#include "app/SetPasswordController.hpp"
#include "app/SignatureValidationController.hpp"
#include "core/async/TaskScheduler.hpp"

#include <cstdio>

using namespace rivet;
using namespace rivet::app;
using namespace rivet::app::sectest;

namespace {

crypto::testing::TestKey makeKey(const char* cn) {
    crypto::testing::CertSpec spec;
    spec.cn = cn;
    return crypto::testing::makeCert(spec);
}

class RealStore final : public crypto::IdentityStore {
public:
    RealStore(std::shared_ptr<crypto::CryptoProvider> p, const crypto::testing::TestKey& k) : provider(std::move(p)), key(k) {}
    std::shared_ptr<crypto::CryptoProvider> provider;
    const crypto::testing::TestKey& key;
    std::string_view name() const override { return "runtime-key"; }
    bool isAvailable() const override { return true; }
    crypto::ProviderAssurance assurance() const override { return crypto::ProviderAssurance::Software; }
    crypto::CryptoResult<std::vector<crypto::IdentityDescriptor>> list(const crypto::CancelToken&) override {
        crypto::IdentityDescriptor d;
        d.id = "rt";
        d.providerName = "runtime-key";
        auto cert = provider->parseCertificate(crypto::testing::certDer(key));
        if (!cert) return std::unexpected(cert.error());
        d.certificate = *cert;
        d.privateKeyAvailable = true;
        return std::vector<crypto::IdentityDescriptor>{d};
    }
    crypto::CryptoResult<std::unique_ptr<crypto::SigningIdentity>> open(std::string_view, std::shared_ptr<crypto::PinPrompt>,
                                                                       const crypto::CancelToken&) override {
        return crypto::testing::makeIdentity(*provider, key);
    }
};

struct SignView final : ISigningView {
    int dialogs = 0, blocked = 0, errors = 0, done = 0, closed = 0;
    std::string lastError, lastDone;
    void showDialog(const SigningDialogState&) override { ++dialogs; }
    void showBlocked(const std::string&) override { ++blocked; }
    void showProgress(const std::string&) override {}
    void showError(const std::string& m) override { ++errors; lastError = m; }
    void showDone(const std::string& m) override { ++done; lastDone = m; }
    void close() override { ++closed; }
    void askPin(const crypto::PinRequest&, std::function<void(std::optional<crypto::SecretBuffer>)> a) override { a(std::nullopt); }
    void askConsent(const net::ConsentRequest&, std::function<void(bool)> a) override { a(false); }
};

struct PwView final : ISetPasswordView {
    int blocked = 0, errors = 0, done = 0;
    std::string lastError;
    void showDialog(const SetPasswordDialogState&) override {}
    void showBlocked(const std::string&) override { ++blocked; }
    void showProgress(const std::string&) override {}
    void showError(const std::string& m) override { ++errors; lastError = m; }
    void showDone(const std::string&) override { ++done; }
    void close() override {}
};

struct ValView final : ISignatureValidationView {
    int results = 0;
    ValidationPanelModel model;
    void showLoading(const std::string&) override {}
    void showResult(const ValidationPanelModel& m) override { ++results; model = m; }
    void showMessage(const std::string&) override {}
    void close() override {}
};

std::string asText(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }

} // namespace

RIVET_TEST(securityIntegrationSignThenValidateThenRefusePassword) {
    TempDir dir("integration");
    QueueDispatcher dispatcher;
    core::TaskScheduler scheduler(2);
    crypto::testing::TestKey key = makeKey("Integration Signer");

    SecurityServices services;
    services.provider = crypto::makeDefaultProvider();
    services.stores = {std::make_shared<RealStore>(services.provider, key)};
    auto anchors = std::make_shared<crypto::TrustAnchorSet>();
    if (auto c = services.provider->parseCertificate(crypto::testing::certDer(key))) anchors->add(*c);
    services.anchors = anchors;

    // 1. Sign (B-B, invisible) the unsigned fixture through the controller.
    FakeHost host;
    host.info = readyDocument(dir.path / "source.pdf");
    host.destination = dir.path / "source-signed.pdf";
    host.bytes = pdf::sign::testing::makeFixture();
    SignView signView;
    std::vector<std::string> statuses;
    {
        DigitalSignatureController signing(host, signView, services, scheduler, dispatcher,
                                           [&](std::string s) { statuses.push_back(std::move(s)); });
        signing.begin();
        CHECK(dispatcher.pumpUntil([&] { return signView.dialogs > 0 || signView.blocked > 0; }));
        CHECK_EQ(signView.dialogs, 1);
        SigningForm form;
        form.reason = "Integration";
        signing.submit(form);
        CHECK(dispatcher.pumpUntil([&] { return !signing.busy(); }));
        if (signView.errors != 0) std::fprintf(stderr, "sign error: %s\n", signView.lastError.c_str());
        CHECK_EQ(signView.done, 1);
    }
    CHECK(fileExists(*host.destination));
    const auto signedBytes = readAll(*host.destination);
    CHECK(asText(signedBytes).find("/ByteRange") != std::string::npos);
    CHECK_GT(signedBytes.size(), host.bytes.size());

    // 2. Validate the saved copy: valid, never a qualified claim for a software key.
    FakeHost valHost;
    valHost.info = readyDocument(*host.destination, true);
    ValView valView;
    SignatureValidationController validation(valHost, valView, services, scheduler, dispatcher, [](std::string) {});
    validation.begin();
    CHECK(dispatcher.pumpUntil([&] { return !validation.busy(); }));
    CHECK_EQ(valView.results, 1);
    CHECK_EQ(valView.model.rows.size(), std::size_t{1});
    const SignatureRowModel& row = valView.model.rows[0];
    CHECK(row.severity == Severity::Good);
    CHECK(row.qualification.find("All technical indicators") == std::string::npos);
    CHECK(row.signer.find("Integration Signer") != std::string::npos);

    // 3. Set Password on the signed copy is refused before any prompt; the file is untouched.
    FakeHost pwHost;
    pwHost.info = readyDocument(*host.destination, true);
    pwHost.destination = dir.path / "never.pdf";
    PwView pwView;
    SetPasswordController setPassword(pwHost, pwView, scheduler, dispatcher, [](std::string) {});
    CHECK(!setPassword.begin());
    CHECK_EQ(pwView.blocked, 1);
    CHECK_EQ(readAll(*host.destination).size(), signedBytes.size());
    // ...and the encryptor itself refuses signed input (defence in depth).
    pdf::encrypt::EncryptOptions options;
    options.userPassword = "pw";
    auto sealed = pdf::encrypt::encryptPdf(signedBytes, options);
    CHECK(!sealed.has_value());
}

RIVET_TEST(securityIntegrationSetPasswordWritesEncryptedCopy) {
    TempDir dir("integration-pw");
    QueueDispatcher dispatcher;
    core::TaskScheduler scheduler(2);
    FakeHost host;
    host.info = readyDocument(dir.path / "plain.pdf");
    host.destination = dir.path / "plain-protected.pdf";
    const std::string plain = rivet::test::enc::samplePdf();
    host.bytes.assign(plain.begin(), plain.end());
    PwView view;
    SetPasswordController controller(host, view, scheduler, dispatcher, [](std::string) {});
    SetPasswordForm form;
    form.userPassword = crypto::SecretBuffer(std::string_view("correct horse"));
    form.userPasswordConfirm = crypto::SecretBuffer(std::string_view("correct horse"));
    form.cipher = pdf::encrypt::Cipher::Aes256;
    CHECK(controller.begin());
    controller.submit(std::move(form));
    CHECK(dispatcher.pumpUntil([&] { return !controller.busy(); }));
    if (view.errors != 0) std::fprintf(stderr, "encrypt error: %s\n", view.lastError.c_str());
    CHECK_EQ(view.done, 1);
    CHECK(!controller.holdsSecrets());
    const std::string written = asText(readAll(*host.destination));
    CHECK(written.find("/Encrypt") != std::string::npos);
    CHECK(written.find("Secret body text") == std::string::npos);
    CHECK(written.find("SECRET-XMP-MARKER") == std::string::npos);
    CHECK(!fileExists(dir.path / "plain.pdf"));

    // An already-encrypted document is refused for both features.
    FakeHost encHost;
    encHost.info = readyDocument(*host.destination);
    encHost.info.encrypted = true;
    PwView again;
    SetPasswordController second(encHost, again, scheduler, dispatcher, [](std::string) {});
    CHECK(!second.begin());
    CHECK_EQ(again.blocked, 1);
}
