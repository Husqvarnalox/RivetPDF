// SPDX-License-Identifier: MPL-2.0
// Cloud / remote signing (ADR-0047): the CryptoPro DSS adapter against an
// in-process MOCK service. Nothing here talks to a real DSS; the CA and the
// service's key pair are generated at run time.

#include "RivetTest.h"

#include "MockDss.hpp"
#include "SignTestSupport.hpp"
#include "crypto/Qualification.hpp"
#include "crypto/RemoteIdentity.hpp"
#include "crypto/remote/CryptoProDss.hpp"
#include "net/Client.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>

using namespace rivet::crypto;
using namespace rivet::crypto::testing;
namespace net = rivet::net;
namespace psign = rivet::pdf::sign;

namespace {

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

template <typename T>
void expectCode(const CryptoResult<T>& r, CryptoErrorCode code, const char* what) {
    const bool ok = !r.has_value() && r.error().code == code;
    if (!ok) {
        std::fprintf(stderr, "    %s: expected %s, got %s\n", what, std::string(toString(code)).c_str(),
                     r ? "success" : (std::string(toString(r.error().code)) + ": " + r.error().message).c_str());
    }
    CHECK(ok);
}

struct ScriptedPrompt final : PinPrompt {
    std::vector<std::optional<std::string>> answers;
    std::size_t next = 0;
    std::vector<PinRequest> requests;
    std::function<void()> onCall;
    std::optional<SecretBuffer> requestPin(const PinRequest& r, const CancelToken&) override {
        requests.push_back(r);
        if (onCall) onCall();
        if (next >= answers.size() || !answers[next]) {
            ++next;
            return std::nullopt;
        }
        return SecretBuffer(std::string_view(*answers[next++]));
    }
};

struct Env {
    std::shared_ptr<CryptoProvider> crypto = makeDefaultProvider();
    TestKey ca, leaf;
    std::vector<Bytes> chain;
    TrustAnchorSet anchors;
    std::shared_ptr<MockDssTransport> dss;
    std::shared_ptr<net::OnlineValidationGate> gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    std::shared_ptr<net::NetworkClient> network;
    CryptoProDssOptions opts;
    std::shared_ptr<CryptoProDssProvider> provider;
    std::optional<CertificateInfo> leafInfo, caInfo;

    explicit Env(DssScript script = {}, std::function<void(CryptoProDssOptions&)> tweak = {},
                 net::NetworkPolicy policy = {}) {
        CertSpec caSpec;
        caSpec.cn = "Test Cloud CA (generated at run time)";
        caSpec.ca = true;
        ca = makeCert(caSpec);
        CertSpec leafSpec;
        leafSpec.cn = "Mock Cloud Signer (test)";
        leafSpec.issuer = &ca;
        leaf = makeCert(leafSpec);
        chain = {certDer(leaf), certDer(ca)};
        if (auto c = crypto->parseCertificate(chain[1])) {
            caInfo = *c;
            anchors.add(*c);
        }
        if (auto c = crypto->parseCertificate(chain[0])) leafInfo = *c;
        dss = std::make_shared<MockDssTransport>(std::move(leaf), chain, std::move(script));
        // `leaf` was moved into the mock; re-read the certificate only.
        network = std::make_shared<net::NetworkClient>(policy, dss, gate);
        opts.serviceUrl = "https://dss.example.com/dss";
        opts.clientId = "rivet-test-client";
        opts.retryBackoff = std::chrono::milliseconds(0);
        opts.pollInterval = std::chrono::milliseconds(1);
        opts.confirmationTimeout = std::chrono::milliseconds(2000);
        if (tweak) tweak(opts);
        rebuild();
    }

    void rebuild() {
        auto p = CryptoProDssProvider::create(network, crypto, opts);
        if (!p) std::fprintf(stderr, "    create failed: %s\n", p.error().message.c_str());
        CHECK(p.has_value());
        if (p) provider = *p;
    }
    CryptoOk login() {
        RemoteCredentials c;
        c.userId = "alice";
        c.secret = SecretBuffer(std::string_view(dss->script().password));
        return provider->authenticate(c, CancelToken{});
    }
    RemoteSignRequest request() const {
        RemoteSignRequest r;
        r.identityId = "id-1";
        r.algorithm = SignatureAlgorithm::RsaPkcs1Sha256;
        r.digest = *crypto->digest(DigestAlgorithm::Sha256, viewOf("signed attributes stand-in"));
        return r;
    }
    std::unique_ptr<RemoteSigningIdentity> identity(std::shared_ptr<PinPrompt> prompt = nullptr) {
        RemoteIdentityStore store(provider, crypto);
        auto opened = store.openRemote("id-1", std::move(prompt), CancelToken{});
        if (!opened) std::fprintf(stderr, "    open failed: %s\n", opened.error().message.c_str());
        CHECK(opened.has_value());
        return opened ? std::move(*opened) : nullptr;
    }
    bool validTrusted(const psign::Bytes& pdf, std::size_t expectSigs = 1) {
        psign::VerifyOptions vo;
        vo.anchors = &anchors;
        auto sigs = psign::verifyPdfSignatures(pdf, *crypto, vo);
        CHECK(sigs.has_value());
        if (!sigs || sigs->size() != expectSigs) return false;
        const auto& s = sigs->front();
        return s.validation.state == SignatureState::ValidTrusted && s.padesBaselineB &&
               s.validation.crypto == CryptoCheck::Valid && s.validation.integrity == IntegrityStatus::Intact &&
               s.subFilter == "ETSI.CAdES.detached";
    }
};

bool pdfLeaked(const std::string& body) {
    // "%PDF" raw, or the base64 of "%PDF-1." at any of the three alignments.
    return contains(body, "%PDF") || contains(body, "JVBERi0xL") || contains(body, "UERGLTEu") || contains(body, "QREYtMS");
}

} // namespace

// ---- configuration guards ------------------------------------------------------

RIVET_TEST(remote_dss_refuses_http_even_when_policy_allows_it) {
    net::NetworkPolicy lax;
    lax.allowHttp = true;
    auto crypto = makeDefaultProvider();
    auto client = std::make_shared<net::NetworkClient>(lax, std::make_shared<MockDssTransport>(TestKey{}, std::vector<Bytes>{}, DssScript{}),
                                                      std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On));
    CryptoProDssOptions o;
    o.serviceUrl = "http://dss.example.com/dss";
    expectCode(CryptoProDssProvider::create(client, crypto, o), CryptoErrorCode::InvalidArgument, "http URL");
    o.serviceUrl = "https://dss.example.com/dss?x=1";
    expectCode(CryptoProDssProvider::create(client, crypto, o), CryptoErrorCode::InvalidArgument, "query in URL");
    o.serviceUrl = "https://user:pw@dss.example.com/dss";
    expectCode(CryptoProDssProvider::create(client, crypto, o), CryptoErrorCode::InvalidArgument, "userinfo in URL");
    o.serviceUrl = "https://dss.example.com/dss";
    CHECK(CryptoProDssProvider::create(client, crypto, o).has_value());
}

RIVET_TEST(remote_dss_whole_document_needs_confirmed_consent_text) {
    Env env;
    auto o = env.opts;
    o.privacy = RemotePrivacyMode::WholeDocument;
    expectCode(CryptoProDssProvider::create(env.network, env.crypto, o), CryptoErrorCode::InvalidArgument, "no consent");
    o.wholeDocumentConsent = "yes";
    expectCode(CryptoProDssProvider::create(env.network, env.crypto, o), CryptoErrorCode::InvalidArgument, "wrong consent");
    o.wholeDocumentConsent = std::string(kDssWholeDocumentConsentText);
    auto p = CryptoProDssProvider::create(env.network, env.crypto, o);
    CHECK(p.has_value());
    if (p) CHECK(p.value()->privacyMode() == RemotePrivacyMode::WholeDocument);
    CHECK(kDssWholeDocumentConsentText.find("COMPLETE document") != std::string_view::npos);
    CHECK(env.opts.privacy == RemotePrivacyMode::HashOnly); // default
}

// ---- authentication, token hygiene --------------------------------------------------

RIVET_TEST(remote_dss_authentication_and_secrets_stay_out_of_urls_and_messages) {
    Env env;
    CHECK(!env.provider->hasAccessToken());
    CHECK(env.login().has_value());
    CHECK(env.provider->hasAccessToken());
    auto ids = env.provider->listIdentities(CancelToken{});
    CHECK(ids.has_value());
    const auto token = env.dss->script().token;
    const auto password = env.dss->script().password;
    for (const auto& s : env.dss->seen()) {
        CHECK(s.url.rfind("https://", 0) == 0);
        CHECK(!contains(s.url, token));
        CHECK(!contains(s.url, password));
        CHECK(!contains(s.url, "access_token"));
        for (const auto& [name, value] : s.headers) {
            CHECK(!contains(value, token));
            CHECK(!contains(value, password));
            CHECK(name != "Authorization"); // the net layer forbids it; the token is in the https body (ADR assumption)
        }
        if (s.path != "/dss/oauth/token") CHECK(!contains(s.body, password));
    }
    // Wrong password -> AuthFailed, no token stored, messages carry no secret.
    Env bad;
    bad.dss->script().password = "other";
    RemoteCredentials c;
    c.userId = "alice";
    c.secret = SecretBuffer(std::string_view("wrong-password-xyz"));
    auto r = bad.provider->authenticate(c, CancelToken{});
    expectCode(r, CryptoErrorCode::AuthFailed, "wrong password");
    if (!r) CHECK(!contains(r.error().message, "wrong-password-xyz"));
    CHECK(!bad.provider->hasAccessToken());
    // Unauthenticated use fails locally, without a request.
    Env none;
    expectCode(none.provider->listIdentities(CancelToken{}), CryptoErrorCode::AuthFailed, "no token");
    expectCode(none.provider->signHash(none.request(), nullptr, CancelToken{}), CryptoErrorCode::AuthFailed, "no token sign");
    CHECK_EQ(none.dss->seen().size(), std::size_t{0});
    env.provider->forgetAccessToken();
    CHECK(!env.provider->hasAccessToken());
}

RIVET_TEST(remote_dss_token_rejected_by_service_is_auth_failed_without_leaking_it) {
    Env env({.sign = DssScript::Sign::Unauthorized});
    CHECK(env.login().has_value());
    auto r = env.provider->signHash(env.request(), nullptr, CancelToken{});
    expectCode(r, CryptoErrorCode::AuthFailed, "401");
    if (!r) CHECK(!contains(r.error().message, env.dss->script().token));
    env.provider->setAccessToken(SecretBuffer(std::string_view("some-other-token")));
    expectCode(env.provider->listIdentities(CancelToken{}), CryptoErrorCode::AuthFailed, "bad token");
}

RIVET_TEST(remote_dss_network_gate_and_policy_apply) {
    {
        Env env;
        env.gate->setMode(net::OnlineValidationMode::Off);
        expectCode(env.login(), CryptoErrorCode::Network, "gate off");
        CHECK_EQ(env.dss->seen().size(), std::size_t{0});
    }
    {
        Env env;
        env.gate->setMode(net::OnlineValidationMode::Ask); // default prompt denies
        expectCode(env.login(), CryptoErrorCode::Network, "consent denied");
        CHECK_EQ(env.dss->seen().size(), std::size_t{0});
    }
    {
        // A redirect is never followed: it could carry the token to another host.
        Env env({.redirectSignTo = "http://evil.example.com/steal"});
        CHECK(env.login().has_value());
        auto r = env.provider->signHash(env.request(), nullptr, CancelToken{});
        CHECK(!r.has_value());
        CHECK_EQ(env.dss->signCalls(), std::size_t{1});
        for (const auto& s : env.dss->seen()) CHECK(contains(s.url, "dss.example.com"));
    }
}

// ---- identities, classification ----------------------------------------------------

RIVET_TEST(remote_identity_store_lists_cloud_identities_and_never_claims_qes) {
    Env env;
    CHECK(env.login().has_value());
    RemoteIdentityStore store(env.provider, env.crypto);
    CHECK(store.isAvailable());
    CHECK(store.assurance() == ProviderAssurance::RemoteService);
    auto list = store.list(CancelToken{});
    CHECK(list.has_value());
    if (!list || list->size() != 1) return;
    const auto& d = list->front();
    CHECK(d.location.kind == KeyLocationKind::Cloud);
    CHECK_EQ(d.providerName, std::string("cryptopro-dss"));
    CHECK(d.certificate.sha256Fingerprint == env.leafInfo->sha256Fingerprint);
    expectCode(store.openRemote("nope", nullptr, CancelToken{}), CryptoErrorCode::CertificateNotFound, "unknown id");

    // Evidence-based classification: even with the mock CA on the accredited
    // list, a qualified policy listed and the provider id listed as a CSP, a
    // mock chain behind a remote service is NOT QualifiedEvidence.
    const std::string text = "rivet-accredited-ca-list 1\nca sha256=" + env.caInfo->sha256Fingerprint +
                             " name=Test Cloud CA\npolicy 1.2.3.4.5\ncsp cryptopro-dss\n";
    auto accredited = AccreditedCaList::parse(text);
    CHECK(accredited.has_value());
    if (!accredited) return;
    ClassificationInput in;
    in.signer = &*env.leafInfo;
    in.chain = {*env.caInfo};
    in.provider = ProviderAssurance::RemoteService;
    in.providerId = "cryptopro-dss";
    in.list = &*accredited;
    in.signature = SignatureAlgorithm::RsaPkcs1Sha256;
    const auto c = classify(in);
    CHECK(c.level != SignatureClass::QualifiedEvidence);
    CHECK(!c.evidence.certifiedCspDeclared);
    in.list = nullptr;
    CHECK(classify(in).level != SignatureClass::QualifiedEvidence);
}

// ---- sign-hash flow and privacy modes ----------------------------------------------

RIVET_TEST(remote_pades_hash_only_sends_only_the_digest_and_verifies) {
    Env env;
    CHECK(env.login().has_value());
    auto identity = env.identity();
    CHECK(identity != nullptr);
    if (!identity) return;
    const psign::Bytes original = psign::testing::makeFixture();
    auto signedPdf = psign::signPdf(original, psign::testing::basicRequest(), *env.crypto, *identity);
    if (!signedPdf) std::fprintf(stderr, "    sign failed: %s\n", signedPdf.error().message.c_str());
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    CHECK(env.validTrusted(signedPdf->data)); // PAdES-B-B, verified by the existing verifier
    CHECK_EQ(env.dss->signCalls(), std::size_t{1});

    for (const auto& s : env.dss->seen()) {
        CHECK(!pdfLeaked(s.body));
        CHECK(!contains(s.body, "\"document\""));
        if (s.path == "/dss/api/v1/sign") {
            auto j = remote::parseJson(s.body);
            CHECK(j.has_value());
            if (!j) continue;
            CHECK_EQ(j->stringAt("privacy").value_or(""), std::string("hash_only"));
            CHECK(j->find("document") == nullptr);
            auto digest = remote::base64Decode(j->stringAt("digest").value_or(""));
            CHECK(digest.has_value() && digest->size() == 32);
            CHECK(s.body.size() < 600); // a digest, not a document
        }
    }
    // The classification of the result is simple crypto at best, never QES.
    ClassificationInput in;
    in.signer = &*env.leafInfo;
    in.provider = ProviderAssurance::RemoteService;
    CHECK(classify(in).level != SignatureClass::QualifiedEvidence);
}

RIVET_TEST(remote_hash_only_never_serialises_a_supplied_document) {
    Env env;
    CHECK(env.login().has_value());
    auto req = env.request();
    req.document = std::make_shared<const Bytes>(psign::testing::makeFixture());
    auto sig = env.provider->signHash(req, nullptr, CancelToken{});
    CHECK(sig.has_value());
    for (const auto& s : env.dss->seen()) {
        CHECK(!pdfLeaked(s.body));
        CHECK(!contains(s.body, "\"document\""));
    }
}

RIVET_TEST(remote_pades_whole_document_uploads_the_document_after_consent) {
    Env env({}, [](CryptoProDssOptions& o) {
        o.privacy = RemotePrivacyMode::WholeDocument;
        o.wholeDocumentConsent = std::string(kDssWholeDocumentConsentText);
    });
    CHECK(env.login().has_value());
    auto identity = env.identity();
    CHECK(identity != nullptr);
    if (!identity) return;
    const psign::Bytes original = psign::testing::makeFixture();
    // Without the document the mode refuses to run (nothing is sent).
    expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::InvalidArgument, "no document");
    CHECK_EQ(env.dss->signCalls(), std::size_t{0});
    identity->setDocument(std::make_shared<const Bytes>(original));
    auto signedPdf = psign::signPdf(original, psign::testing::basicRequest(), *env.crypto, *identity);
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    CHECK(env.validTrusted(signedPdf->data));
    bool found = false;
    for (const auto& s : env.dss->seen()) {
        if (s.path != "/dss/api/v1/sign") continue;
        auto j = remote::parseJson(s.body);
        CHECK(j.has_value());
        if (!j) continue;
        CHECK_EQ(j->stringAt("privacy").value_or(""), std::string("whole_document"));
        auto doc = remote::base64Decode(j->stringAt("document").value_or(""));
        CHECK(doc.has_value() && *doc == original);
        found = true;
    }
    CHECK(found);
}

RIVET_TEST(remote_pades_signing_asks_the_service_exactly_once) {
    // The default /Contents reservation accounts for the service's whole chain,
    // so the PDF signer does not need a second attempt (a retry would change the
    // /ByteRange, hence the digest, and cost the user another confirmation).
    Env env;
    CHECK(env.login().has_value());
    auto identity = env.identity();
    CHECK(identity != nullptr);
    if (!identity) return;
    auto out = psign::signPdf(psign::testing::makeFixture(), psign::testing::basicRequest(), *env.crypto, *identity);
    CHECK(out.has_value());
    if (!out) return;
    CHECK_EQ(out->attempts, std::size_t{1});
    CHECK_EQ(identity->remoteCalls(), std::size_t{1});
    CHECK_EQ(env.dss->signCalls(), std::size_t{1});
}

RIVET_TEST(remote_bad_signature_from_service_is_rejected_before_it_reaches_a_document) {
    Env env({.sign = DssScript::Sign::BadSignature});
    CHECK(env.login().has_value());
    auto identity = env.identity();
    CHECK(identity != nullptr);
    if (!identity) return;
    auto out = psign::signPdf(psign::testing::makeFixture(), psign::testing::basicRequest(), *env.crypto, *identity);
    CHECK(!out.has_value());
    if (!out) CHECK(out.error().cryptoCode == CryptoErrorCode::BadResponse);
}

// ---- PIN / OTP and asynchronous confirmation ---------------------------------------

RIVET_TEST(remote_pin_flow_prompts_retries_and_reports_reasons) {
    Env env({.sign = DssScript::Sign::Pin});
    CHECK(env.login().has_value());
    ScriptedPrompt prompt;
    prompt.answers = {std::string("000000"), std::string("246810")}; // wrong, then right
    auto sig = env.provider->signHash(env.request(), &prompt, CancelToken{});
    CHECK(sig.has_value());
    CHECK_EQ(prompt.requests.size(), std::size_t{2});
    if (prompt.requests.size() == 2) {
        CHECK(prompt.requests[0].reason == PinPromptReason::Required);
        CHECK(prompt.requests[1].reason == PinPromptReason::Incorrect);
        CHECK_EQ(prompt.requests[1].attempt, 2);
        CHECK_EQ(prompt.requests[0].providerName, std::string("cryptopro-dss"));
    }
    for (const auto& s : env.dss->seen()) CHECK(!contains(s.url, "246810"));
}

RIVET_TEST(remote_pin_failures_map_to_the_taxonomy) {
    {
        Env env({.sign = DssScript::Sign::Pin});
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::ConfirmationRequired, "no prompt");
        CHECK_EQ(env.dss->cancelCalls(), std::size_t{1}); // the pending transaction is released
    }
    {
        Env env({.sign = DssScript::Sign::Pin, .pinRetries = 3}, [](CryptoProDssOptions& o) { o.maxPinAttempts = 2; });
        CHECK(env.login().has_value());
        ScriptedPrompt prompt;
        prompt.answers = {std::string("1"), std::string("2"), std::string("3")};
        auto r = env.provider->signHash(env.request(), &prompt, CancelToken{});
        expectCode(r, CryptoErrorCode::ConfirmationRejected, "attempts exhausted");
        CHECK_EQ(prompt.requests.size(), std::size_t{2});
    }
    {
        Env env({.sign = DssScript::Sign::Pin, .pinRetries = 2});
        CHECK(env.login().has_value());
        ScriptedPrompt prompt;
        prompt.answers = {std::string("1"), std::string("2"), std::string("3")};
        auto r = env.provider->signHash(env.request(), &prompt, CancelToken{});
        expectCode(r, CryptoErrorCode::PinLocked, "locked by service");
    }
    {
        Env env({.sign = DssScript::Sign::Pin});
        CHECK(env.login().has_value());
        ScriptedPrompt prompt; // no answers: the user cancels
        expectCode(env.provider->signHash(env.request(), &prompt, CancelToken{}), CryptoErrorCode::Cancelled, "user cancel in prompt");
        CHECK_EQ(env.dss->cancelCalls(), std::size_t{1});
    }
}

RIVET_TEST(remote_async_confirmation_is_polled_until_signed) {
    Env env({.sign = DssScript::Sign::Pending, .pendingPolls = 3});
    CHECK(env.login().has_value());
    auto sig = env.provider->signHash(env.request(), nullptr, CancelToken{});
    CHECK(sig.has_value());
    CHECK_EQ(env.dss->statusCalls(), std::size_t{4});
    CHECK_EQ(env.dss->signCalls(), std::size_t{1});
}

RIVET_TEST(remote_device_rejection_and_confirmation_timeout) {
    {
        Env env({.sign = DssScript::Sign::DeviceRejects, .pendingPolls = 1});
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::ConfirmationRejected, "declined");
    }
    {
        Env env({.sign = DssScript::Sign::Pending, .pendingPolls = 1000000}, [](CryptoProDssOptions& o) {
            o.confirmationTimeout = std::chrono::milliseconds(60);
        });
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::Timeout, "confirmation timeout");
        CHECK_EQ(env.dss->cancelCalls(), std::size_t{1});
    }
}

// ---- service and transport errors --------------------------------------------------

RIVET_TEST(remote_error_taxonomy_for_service_refusals) {
    {
        Env env({.sign = DssScript::Sign::NotQualified});
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::NotQualified, "not qualified");
        CHECK_EQ(env.dss->signCalls(), std::size_t{1}); // refusals are not retried
    }
    {
        Env env({.sign = DssScript::Sign::RateLimit});
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::RateLimited, "429");
        CHECK_EQ(env.dss->signCalls(), std::size_t{1});
    }
    {
        Env env({.timeoutOnSign = true});
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::Timeout, "timeout");
        CHECK_EQ(env.dss->signCalls(), std::size_t{1});
    }
}

RIVET_TEST(remote_retries_5xx_a_bounded_number_of_times_with_one_request_id) {
    {
        Env env({.fail5xxFirst = 2}); // maxRetries default 2: third attempt succeeds
        CHECK(env.login().has_value());
        CHECK(env.provider->signHash(env.request(), nullptr, CancelToken{}).has_value());
        CHECK_EQ(env.dss->signCalls(), std::size_t{3});
        std::vector<std::string> bodies;
        for (const auto& s : env.dss->seen())
            if (s.path == "/dss/api/v1/sign") bodies.push_back(s.body);
        CHECK_EQ(bodies.size(), std::size_t{3});
        CHECK(std::all_of(bodies.begin(), bodies.end(), [&](const std::string& b) { return b == bodies.front(); })); // same idempotency key
        CHECK_EQ(env.dss->requestIds().size(), std::size_t{1});
    }
    {
        Env env({.fail5xxFirst = 1000});
        CHECK(env.login().has_value());
        auto r = env.provider->signHash(env.request(), nullptr, CancelToken{});
        expectCode(r, CryptoErrorCode::ServerError, "always 503");
        CHECK_EQ(env.dss->signCalls(), std::size_t{3}); // 1 + maxRetries, then stop
    }
    {
        Env env({.fail5xxFirst = 1000}, [](CryptoProDssOptions& o) { o.maxRetries = 0; });
        CHECK(env.login().has_value());
        expectCode(env.provider->signHash(env.request(), nullptr, CancelToken{}), CryptoErrorCode::ServerError, "no retries");
        CHECK_EQ(env.dss->signCalls(), std::size_t{1});
    }
}

// ---- cancellation ------------------------------------------------------------------

RIVET_TEST(remote_cancel_during_confirmation_leaves_no_partial_output) {
    namespace fs = std::filesystem;
    CancelToken cancel;
    Env env({.sign = DssScript::Sign::Pending, .pendingPolls = 1000000});
    env.dss->script().onStatusPoll = [&] { cancel.cancel(); };
    CHECK(env.login().has_value());
    auto identity = env.identity();
    CHECK(identity != nullptr);
    if (!identity) return;

    const fs::path dir = psign::testing::scratchDir("remote-cancel");
    const fs::path input = dir / "in.pdf";
    const fs::path output = dir / "out.pdf";
    const psign::Bytes original = psign::testing::makeFixture();
    {
        std::ofstream f(input, std::ios::binary);
        f.write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
    }
    psign::SignRequest r = psign::testing::basicRequest();
    r.cancel = cancel;
    auto out = psign::signPdfFile(input, output, r, *env.crypto, *identity);
    CHECK(!out.has_value());
    if (!out) {
        if (out.error().code != psign::SignErrorCode::Cancelled)
            std::fprintf(stderr, "    expected Cancelled, got %s\n", out.error().message.c_str());
        CHECK(out.error().code == psign::SignErrorCode::Cancelled);
    }
    CHECK(!fs::exists(output));
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++files;
    }
    CHECK_EQ(files, std::size_t{1}); // only the untouched input: no temp file, no partial output
    CHECK_EQ(env.dss->cancelCalls(), std::size_t{1}); // the service was told
    fs::remove_all(dir);
}

RIVET_TEST(remote_cancel_before_start_sends_nothing) {
    Env env;
    CHECK(env.login().has_value());
    const std::size_t before = env.dss->seen().size();
    CancelToken cancel;
    cancel.cancel();
    expectCode(env.provider->signHash(env.request(), nullptr, cancel), CryptoErrorCode::Cancelled, "pre-cancelled");
    CHECK_EQ(env.dss->seen().size(), before);
}
