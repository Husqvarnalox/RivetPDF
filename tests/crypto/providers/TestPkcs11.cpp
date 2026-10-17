// SPDX-License-Identifier: MPL-2.0
//
// PKCS#11 provider tests. The functional tests run the real provider against
// the dlopen'ed software token in mock_pkcs11/ (a mock, NOT a vendor module:
// see docs/adr/ADR-0041-pkcs11-and-platform-stores.md for what that proves).
// Tests that need an external module (CryptoPro, SoftHSM2) print SKIP and pass
// when it is absent.

#include "RivetTest.h"

#include "crypto/Async.hpp"
#include "crypto/providers/pkcs11/Pkcs11IdentityStore.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>

using namespace rivet;
using namespace rivet::crypto;

#if defined(RIVET_TEST_PKCS11_MOCK)

#include "TestSupport.hpp"

#include <dlfcn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {

constexpr const char* kPin = "1234";
constexpr const char* kWrongPin = "98761";

// ---- dlopen'ed mock control -------------------------------------------------

struct MockControl {
    void* handle = nullptr;
    void (*setScenario)(const char*) = nullptr;
    unsigned (*loginCalls)() = nullptr;
    unsigned (*signCalls)() = nullptr;
    unsigned (*openSessions)() = nullptr;
    unsigned long (*lastPssSalt)() = nullptr;
    int (*isInitialized)() = nullptr;
};

MockControl& mock() {
    static MockControl m = [] {
        MockControl c;
        c.handle = dlopen(RIVET_TEST_PKCS11_MOCK_PATH, RTLD_NOW | RTLD_LOCAL);
        if (c.handle) {
            c.setScenario = reinterpret_cast<void (*)(const char*)>(dlsym(c.handle, "rivet_mock_set_scenario"));
            c.loginCalls = reinterpret_cast<unsigned (*)()>(dlsym(c.handle, "rivet_mock_login_calls"));
            c.signCalls = reinterpret_cast<unsigned (*)()>(dlsym(c.handle, "rivet_mock_sign_calls"));
            c.openSessions = reinterpret_cast<unsigned (*)()>(dlsym(c.handle, "rivet_mock_open_sessions"));
            c.lastPssSalt = reinterpret_cast<unsigned long (*)()>(dlsym(c.handle, "rivet_mock_last_pss_salt_length"));
            c.isInitialized = reinterpret_cast<int (*)()>(dlsym(c.handle, "rivet_mock_is_initialized"));
        }
        return c;
    }();
    return m;
}

bool mockReady() { return mock().handle && mock().setScenario && mock().loginCalls; }

// ---- helpers ------------------------------------------------------------------

class ScriptedPrompt final : public PinPrompt {
public:
    explicit ScriptedPrompt(std::vector<std::optional<std::string>> answers) : answers_(std::move(answers)) {}
    std::optional<SecretBuffer> requestPin(const PinRequest& request, const CancelToken&) override {
        reasons.push_back(request.reason);
        attempts.push_back(request.attempt);
        if (next_ >= answers_.size()) return std::nullopt;
        const auto& answer = answers_[next_++];
        if (!answer) return std::nullopt;
        return SecretBuffer(std::string_view(*answer));
    }
    std::vector<PinPromptReason> reasons;
    std::vector<int> attempts;

private:
    std::vector<std::optional<std::string>> answers_;
    std::size_t next_ = 0;
};

std::shared_ptr<ScriptedPrompt> prompt(std::vector<std::optional<std::string>> answers) {
    return std::make_shared<ScriptedPrompt>(std::move(answers));
}

std::shared_ptr<IdentityStore> makeStore(const char* scenario) {
    mock().setScenario(scenario);
    Pkcs11Options options;
    options.modulePath = RIVET_TEST_PKCS11_MOCK_PATH;
    options.displayName = "Mock token";
    return makePkcs11IdentityStore(std::move(options));
}

const IdentityDescriptor* findBySubject(const std::vector<IdentityDescriptor>& all, const std::string& needle) {
    for (const auto& d : all)
        if (d.certificate.subject.find(needle) != std::string::npos) return &d;
    return nullptr;
}

struct Opened {
    std::shared_ptr<IdentityStore> store;
    std::shared_ptr<ScriptedPrompt> prompt;
    std::unique_ptr<SigningIdentity> identity;
    std::optional<CryptoError> error;
    IdentityDescriptor descriptor;
};

Opened openIdentity(const char* scenario, const std::string& subject, std::vector<std::optional<std::string>> pins) {
    Opened o;
    o.store = makeStore(scenario);
    o.prompt = prompt(std::move(pins));
    auto all = o.store->list(CancelToken{});
    if (!all) {
        o.error = all.error();
        return o;
    }
    const IdentityDescriptor* d = findBySubject(*all, subject);
    if (!d) {
        o.error = CryptoError{CryptoErrorCode::CertificateNotFound, "identity not listed"};
        return o;
    }
    o.descriptor = *d;
    auto identity = o.store->open(d->id, o.prompt, CancelToken{});
    if (!identity) {
        o.error = identity.error();
        return o;
    }
    o.identity = std::move(*identity);
    return o;
}

// Verifies `signature` over `message` with the public key of the certificate.
bool verifyWithCert(const CertificateInfo& cert, SignatureAlgorithm alg, ByteView message, const Bytes& signature) {
    const unsigned char* p = cert.der.data();
    testing::X509Ptr x(d2i_X509(nullptr, &p, static_cast<long>(cert.der.size())));
    if (!x) return false;
    testing::PkeyPtr key(X509_get_pubkey(x.get()));
    if (!key) return false;
    const EVP_MD* md = nullptr;
    bool pss = false;
    switch (alg) {
    case SignatureAlgorithm::RsaPkcs1Sha256:
    case SignatureAlgorithm::EcdsaSha256: md = EVP_sha256(); break;
    case SignatureAlgorithm::RsaPkcs1Sha384:
    case SignatureAlgorithm::EcdsaSha384: md = EVP_sha384(); break;
    case SignatureAlgorithm::RsaPkcs1Sha512:
    case SignatureAlgorithm::EcdsaSha512: md = EVP_sha512(); break;
    case SignatureAlgorithm::RsaPssSha256: md = EVP_sha256(); pss = true; break;
    case SignatureAlgorithm::RsaPssSha384: md = EVP_sha384(); pss = true; break;
    case SignatureAlgorithm::RsaPssSha512: md = EVP_sha512(); pss = true; break;
    default: return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* pctx = nullptr;
    if (EVP_DigestVerifyInit(ctx.get(), &pctx, md, nullptr, key.get()) <= 0) return false;
    if (pss) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) return false;
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, -1) <= 0) return false; // digest length
    }
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), message.data(), message.size()) == 1;
}

Bytes sampleMessage() {
    const std::string text = "Rivet PKCS#11 provider test message";
    return Bytes(text.begin(), text.end());
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    bool drain(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::size_t ran = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ran < count) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return false;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task(); // never under the lock
            ++ran;
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

} // namespace

#define REQUIRE_MOCK()                                              \
    if (!mockReady()) {                                             \
        std::puts("SKIP: mock PKCS#11 module could not be loaded"); \
        return;                                                     \
    }

RIVET_TEST(mockModuleLoads) {
    CHECK(mockReady());
}

RIVET_TEST(listShowsCertificateKeyPairs) {
    REQUIRE_MOCK();
    auto store = makeStore("ok");
    CHECK(store->isAvailable());
    CHECK(store->assurance() == ProviderAssurance::HardwareToken);
    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
    if (!all) return;
    CHECK_EQ(all->size(), std::size_t{2}); // the CA certificate has no key and is not an identity
    const IdentityDescriptor* rsa = findBySubject(*all, "Mock RSA Signer");
    const IdentityDescriptor* ec = findBySubject(*all, "Mock EC Signer");
    CHECK(rsa != nullptr);
    CHECK(ec != nullptr);
    if (!rsa || !ec) return;
    CHECK(rsa->privateKeyAvailable);
    CHECK(ec->privateKeyAvailable);
    CHECK(!rsa->certificateExpired);
    CHECK(rsa->location.kind == KeyLocationKind::Token);
    CHECK(rsa->id != ec->id);
    CHECK(!contains(rsa->id, kPin));
    // Listing never logs in and never leaves a session behind.
    CHECK_EQ(mock().loginCalls(), 0U);
    CHECK_EQ(mock().openSessions(), 0U);
}

RIVET_TEST(rsaPkcs1SignatureVerifies) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin, kPin, kPin});
    CHECK(!o.error.has_value());
    if (!o.identity) return;
    CHECK(o.identity->keyType() == PublicKeyType::Rsa);
    CHECK_EQ(o.identity->keyBits(), 2048);
    CHECK(!o.identity->isExportable());
    CHECK(o.identity->chainDer().size() >= 2); // leaf + the CA found on the token
    const Bytes message = sampleMessage();
    for (auto alg : {SignatureAlgorithm::RsaPkcs1Sha256, SignatureAlgorithm::RsaPkcs1Sha384, SignatureAlgorithm::RsaPkcs1Sha512}) {
        auto sig = o.identity->sign(alg, message, CancelToken{});
        CHECK(sig.has_value());
        if (sig) CHECK(verifyWithCert(o.identity->certificate(), alg, message, *sig));
    }
    // The identity keeps no PIN and no login between signatures: every sign() opens its own
    // session, prompts once and closes it again.
    CHECK_EQ(mock().loginCalls(), 3U);
    CHECK_EQ(o.prompt->reasons.size(), std::size_t{3});
    CHECK_EQ(mock().openSessions(), 0U);
    CHECK(o.prompt->reasons[0] == PinPromptReason::Required);
}

RIVET_TEST(rsaPssSignatureVerifies) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin, kPin, kPin});
    if (!o.identity) { CHECK(false); return; }
    const Bytes message = sampleMessage();
    for (auto alg : {SignatureAlgorithm::RsaPssSha256, SignatureAlgorithm::RsaPssSha384, SignatureAlgorithm::RsaPssSha512}) {
        auto sig = o.identity->sign(alg, message, CancelToken{});
        CHECK(sig.has_value());
        if (sig) CHECK(verifyWithCert(o.identity->certificate(), alg, message, *sig));
    }
}

RIVET_TEST(rsaRawMechanismsFallBackToSoftwareHash) {
    REQUIRE_MOCK();
    // Token offers only CKM_RSA_PKCS / CKM_RSA_PKCS_PSS / CKM_ECDSA: the provider hashes in software.
    const Bytes message = sampleMessage();
    {
        auto rsa = openIdentity("raw-mechanisms", "Mock RSA Signer", {kPin, kPin});
        if (!rsa.identity) { CHECK(false); return; }
        for (auto alg : {SignatureAlgorithm::RsaPkcs1Sha256, SignatureAlgorithm::RsaPssSha256}) {
            auto sig = rsa.identity->sign(alg, message, CancelToken{});
            CHECK(sig.has_value());
            if (sig) CHECK(verifyWithCert(rsa.identity->certificate(), alg, message, *sig));
        }
        CHECK_EQ(mock().lastPssSalt(), 32UL); // salt length = digest length
    }
    auto ec = openIdentity("raw-mechanisms", "Mock EC Signer", {kPin});
    if (!ec.identity) { CHECK(false); return; }
    auto sig = ec.identity->sign(SignatureAlgorithm::EcdsaSha256, message, CancelToken{});
    CHECK(sig.has_value());
    if (sig) CHECK(verifyWithCert(ec.identity->certificate(), SignatureAlgorithm::EcdsaSha256, message, *sig));
}

RIVET_TEST(ecdsaSignatureIsDerAndVerifies) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock EC Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    CHECK(o.identity->keyType() == PublicKeyType::Ec);
    const Bytes message = sampleMessage();
    auto sig = o.identity->sign(SignatureAlgorithm::EcdsaSha256, message, CancelToken{});
    CHECK(sig.has_value());
    if (!sig) return;
    CHECK(!sig->empty() && (*sig)[0] == 0x30); // DER SEQUENCE, not raw r||s
    CHECK(verifyWithCert(o.identity->certificate(), SignatureAlgorithm::EcdsaSha256, message, *sig));
}

RIVET_TEST(signDigestVerifies) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    const Bytes message = sampleMessage();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = 0;
    EVP_Digest(message.data(), message.size(), md, &mdLen, EVP_sha256(), nullptr);
    auto sig = o.identity->signDigest(SignatureAlgorithm::RsaPkcs1Sha256, ByteView(md, mdLen), CancelToken{});
    CHECK(sig.has_value());
    if (sig) CHECK(verifyWithCert(o.identity->certificate(), SignatureAlgorithm::RsaPkcs1Sha256, message, *sig));
}

RIVET_TEST(wrongPinReprompts) {
    REQUIRE_MOCK();
    auto o = openIdentity("wrong-pin", "Mock RSA Signer", {kWrongPin, kPin});
    CHECK(!o.error.has_value());
    if (!o.identity) return;
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(sig.has_value());
    CHECK_EQ(o.prompt->reasons.size(), std::size_t{2});
    if (o.prompt->reasons.size() == 2) {
        CHECK(o.prompt->reasons[0] == PinPromptReason::Required);
        CHECK(o.prompt->reasons[1] == PinPromptReason::Incorrect || o.prompt->reasons[1] == PinPromptReason::LastAttempt);
        CHECK_EQ(o.prompt->attempts[1], 2);
    }
}

RIVET_TEST(wrongPinThreeTimesGivesWrongPasswordWithoutLeakingPin) {
    REQUIRE_MOCK();
    auto o = openIdentity("wrong-pin", "Mock RSA Signer", {kWrongPin, kWrongPin, kWrongPin, kWrongPin});
    if (!o.identity) { CHECK(false); return; }
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (sig) return;
    CHECK(sig.error().code == CryptoErrorCode::WrongPassword);
    CHECK_EQ(o.prompt->reasons.size(), std::size_t{3}); // exactly three attempts, no fourth prompt
    CHECK_EQ(mock().loginCalls(), 3U);
    CHECK(!contains(sig.error().message, kWrongPin));
    CHECK(!contains(sig.error().message, kPin));
}

RIVET_TEST(promptCancelGivesCancelled) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {std::nullopt});
    if (!o.identity) { CHECK(false); return; }
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::Cancelled);
    CHECK_EQ(mock().loginCalls(), 0U);
}

RIVET_TEST(missingPromptGivesCancelled) {
    REQUIRE_MOCK();
    auto store = makeStore("ok");
    auto all = store->list(CancelToken{});
    if (!all || all->empty()) { CHECK(false); return; }
    auto identity = store->open((*all)[0].id, nullptr, CancelToken{});
    CHECK(identity.has_value());
    if (!identity) return;
    auto sig = (*identity)->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::Cancelled);
}

RIVET_TEST(lockedTokenGivesPinLocked) {
    REQUIRE_MOCK();
    auto o = openIdentity("locked", "Mock RSA Signer", {kPin, kPin});
    if (!o.identity) { CHECK(false); return; }
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::PinLocked);
}

RIVET_TEST(removedTokenGivesDeviceRemoved) {
    REQUIRE_MOCK();
    auto o = openIdentity("token-removed", "Mock RSA Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::DeviceRemoved);
}

RIVET_TEST(sessionExpiryRecovers) {
    REQUIRE_MOCK();
    auto o = openIdentity("session-expired-once", "Mock RSA Signer", {kPin, kPin});
    if (!o.identity) { CHECK(false); return; }
    const Bytes message = sampleMessage();
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, message, CancelToken{});
    CHECK(sig.has_value());
    if (sig) CHECK(verifyWithCert(o.identity->certificate(), SignatureAlgorithm::RsaPkcs1Sha256, message, *sig));
    CHECK_EQ(mock().signCalls(), 2U); // the expired call and the retry
}

RIVET_TEST(persistentSessionExpiryIsReportedNotLooped) {
    REQUIRE_MOCK();
    auto o = openIdentity("session-expired-always", "Mock RSA Signer", {kPin, kPin, kPin, kPin});
    if (!o.identity) { CHECK(false); return; }
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::SessionExpired);
    CHECK(mock().signCalls() <= 2U); // bounded retry
}

RIVET_TEST(missingKeyGivesNoPrivateKey) {
    REQUIRE_MOCK();
    auto store = makeStore("key-missing");
    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
    if (!all) return;
    CHECK(!all->empty());
    for (const auto& d : *all) CHECK(!d.privateKeyAvailable);
    if (all->empty()) return;
    auto identity = store->open((*all)[0].id, prompt({kPin}), CancelToken{});
    if (identity) {
        auto sig = (*identity)->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
        CHECK(!sig.has_value());
        if (!sig) CHECK(sig.error().code == CryptoErrorCode::NoPrivateKey);
    } else {
        CHECK(identity.error().code == CryptoErrorCode::NoPrivateKey);
    }
}

RIVET_TEST(expiredCertificateIsListedAsExpired) {
    REQUIRE_MOCK();
    auto store = makeStore("expired-cert");
    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
    if (!all) return;
    const IdentityDescriptor* rsa = findBySubject(*all, "Mock RSA Signer");
    CHECK(rsa != nullptr);
    if (rsa) {
        CHECK(rsa->certificateExpired);
        CHECK(rsa->privateKeyAvailable);
    }
}

RIVET_TEST(noSlotsListsNothing) {
    REQUIRE_MOCK();
    auto store = makeStore("no-slots");
    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
    if (all) CHECK(all->empty());
}

RIVET_TEST(initFailureMakesStoreUnavailable) {
    REQUIRE_MOCK();
    auto store = makeStore("init-fails");
    CHECK(!store->isAvailable());
    CHECK(!store->unavailableReason().empty());
    auto all = store->list(CancelToken{});
    CHECK(!all.has_value());
    if (!all) CHECK(all.error().code == CryptoErrorCode::BackendUnavailable);
}

RIVET_TEST(alreadyInitializedModuleIsShared) {
    REQUIRE_MOCK();
    auto store = makeStore("already-initialized");
    CHECK(store->isAvailable());
    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
}

RIVET_TEST(missingFunctionEntriesAreToleratedNotCalled) {
    REQUIRE_MOCK();
    // C_GetSlotInfo / C_GetMechanismList / C_GetSessionInfo are NULL in the table.
    auto o = openIdentity("sparse", "Mock RSA Signer", {kPin});
    CHECK(!o.error.has_value());
    if (!o.identity) return;
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
    CHECK(sig.has_value());
}

RIVET_TEST(openTokenSignsWithoutPrompt) {
    REQUIRE_MOCK();
    auto o = openIdentity("open-token", "Mock EC Signer", {});
    CHECK(!o.error.has_value());
    if (!o.identity) return;
    const Bytes message = sampleMessage();
    auto sig = o.identity->sign(SignatureAlgorithm::EcdsaSha256, message, CancelToken{});
    CHECK(sig.has_value());
    CHECK(o.prompt->reasons.empty());
    CHECK_EQ(mock().loginCalls(), 0U);
}

RIVET_TEST(unknownIdentityIdIsRejected) {
    REQUIRE_MOCK();
    auto store = makeStore("ok");
    auto identity = store->open("9999/NOPE/ff", prompt({kPin}), CancelToken{});
    CHECK(!identity.has_value());
    auto garbage = store->open("not an id", prompt({kPin}), CancelToken{});
    CHECK(!garbage.has_value());
}

RIVET_TEST(cancelTokenStopsSigning) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    CancelToken cancel;
    cancel.cancel();
    auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), cancel);
    CHECK(!sig.has_value());
    if (!sig) CHECK(sig.error().code == CryptoErrorCode::Cancelled);
    CHECK_EQ(mock().signCalls(), 0U);
    CancelToken listCancel;
    listCancel.cancel();
    auto all = o.store->list(listCancel);
    CHECK(!all.has_value());
    if (!all) CHECK(all.error().code == CryptoErrorCode::Cancelled);
}

RIVET_TEST(errorMessagesNeverContainThePin) {
    REQUIRE_MOCK();
    const char* scenarios[] = {"ok", "wrong-pin", "locked", "token-removed", "session-expired-always", "key-missing", "init-fails"};
    for (const char* scenario : scenarios) {
        auto o = openIdentity(scenario, "Mock RSA Signer", {kWrongPin, kWrongPin, kWrongPin, kWrongPin});
        if (o.error) {
            CHECK(!contains(o.error->message, kWrongPin));
            CHECK(!contains(o.error->message, kPin));
        }
        if (o.store) CHECK(!contains(o.store->unavailableReason(), kWrongPin));
        if (o.identity) {
            auto sig = o.identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), CancelToken{});
            if (!sig) {
                CHECK(!contains(sig.error().message, kWrongPin));
                CHECK(!contains(sig.error().message, kPin));
            }
        }
    }
}

RIVET_TEST(asyncSignThroughRunCryptoAsync) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    SigningIdentity* identity = o.identity.get();
    const Bytes message = sampleMessage();
    std::optional<CryptoResult<Bytes>> outcome;
    const auto mainThread = std::this_thread::get_id();
    std::thread::id doneThread;
    const bool started = runCryptoAsync<Bytes>(
        scheduler, dispatcher, scope, CancelToken{},
        [identity, message](const CancelToken& cancel) -> CryptoResult<Bytes> {
            return identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, message, cancel);
        },
        [&](CryptoResult<Bytes> r) {
            doneThread = std::this_thread::get_id();
            outcome = std::move(r);
        });
    CHECK(started);
    CHECK(dispatcher.drain(1));
    scope.closeAndWait();
    CHECK(outcome.has_value());
    if (outcome && *outcome) CHECK(verifyWithCert(identity->certificate(), SignatureAlgorithm::RsaPkcs1Sha256, message, **outcome));
    CHECK(doneThread == mainThread);
}

RIVET_TEST(asyncCancellationIsReported) {
    REQUIRE_MOCK();
    auto o = openIdentity("ok", "Mock RSA Signer", {kPin});
    if (!o.identity) { CHECK(false); return; }
    core::TaskScheduler scheduler(1);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    SigningIdentity* identity = o.identity.get();
    CancelToken cancel;
    cancel.cancel();
    std::optional<CryptoResult<Bytes>> outcome;
    const bool started = runCryptoAsync<Bytes>(
        scheduler, dispatcher, scope, cancel,
        [identity](const CancelToken& c) -> CryptoResult<Bytes> {
            return identity->sign(SignatureAlgorithm::RsaPkcs1Sha256, sampleMessage(), c);
        },
        [&](CryptoResult<Bytes> r) { outcome = std::move(r); });
    CHECK(started);
    CHECK(dispatcher.drain(1));
    scope.closeAndWait();
    CHECK(outcome.has_value() && !*outcome && outcome->error().code == CryptoErrorCode::Cancelled);
}

RIVET_TEST(moduleIsFinalizedWhenLastStoreGoes) {
    REQUIRE_MOCK();
    {
        auto store = makeStore("ok");
        CHECK(store->isAvailable());
        CHECK_EQ(mock().isInitialized(), 1);
    }
    CHECK_EQ(mock().isInitialized(), 0); // C_Finalize paired with C_Initialize
    CHECK_EQ(mock().openSessions(), 0U);
}

RIVET_TEST(sharedModuleSurvivesOneOfTwoStores) {
    REQUIRE_MOCK();
    auto a = makeStore("ok");
    CHECK(a->isAvailable());
    Pkcs11Options options;
    options.modulePath = RIVET_TEST_PKCS11_MOCK_PATH;
    auto b = makePkcs11IdentityStore(options);
    CHECK(b->isAvailable());
    b.reset();
    CHECK_EQ(mock().isInitialized(), 1); // a still holds the module
    CHECK(a->list(CancelToken{}).has_value());
}

#else // !RIVET_TEST_PKCS11_MOCK

RIVET_TEST(mockUnavailableInThisBuild) {
    std::puts("SKIP: PKCS#11 mock module not built (RIVET_ENABLE_PKCS11 and RIVET_ENABLE_CRYPTO are required)");
}

#endif // RIVET_TEST_PKCS11_MOCK

// ---- build-mode independent tests ----------------------------------------------

namespace {
#if defined(RIVET_CRYPTO_PKCS11)
constexpr bool kPkcs11Built = true;
#else
constexpr bool kPkcs11Built = false;
#endif
} // namespace

RIVET_TEST(factoryReportsBuildMode) {
    Pkcs11Options options;
    options.modulePath = "/nonexistent/path/libnope.so";
    auto store = makePkcs11IdentityStore(options);
    CHECK(store != nullptr);
    CHECK(!store->isAvailable()); // unavailable either way: not built, or module missing
    CHECK(!store->unavailableReason().empty());
    auto all = store->list(CancelToken{});
    CHECK(!all.has_value());
    if (!all) CHECK(all.error().code == CryptoErrorCode::BackendUnavailable);
    auto opened = store->open("1/x/01", nullptr, CancelToken{});
    CHECK(!opened.has_value());
    if (!opened) CHECK(opened.error().code == CryptoErrorCode::BackendUnavailable);
    CHECK(store->assurance() == ProviderAssurance::HardwareToken);
}

RIVET_TEST(nonModuleFileIsUnavailableNotACrash) {
    const auto dir = std::filesystem::temp_directory_path() / "rivet_pkcs11_nonmodule";
    std::filesystem::create_directories(dir);
    const auto file = dir / "not_a_module.so";
    {
        std::ofstream out(file, std::ios::binary);
        out << "this is not a shared library";
    }
    Pkcs11Options options;
    options.modulePath = file;
    auto store = makePkcs11IdentityStore(options);
    CHECK(!store->isAvailable());
    auto all = store->list(CancelToken{});
    CHECK(!all.has_value());
    if (!all) CHECK(all.error().code == CryptoErrorCode::BackendUnavailable);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

RIVET_TEST(emptyModulePathIsUnavailable) {
    auto store = makePkcs11IdentityStore(Pkcs11Options{});
    CHECK(!store->isAvailable());
    CHECK(!store->list(CancelToken{}).has_value());
}

// ---- external modules (SKIP when absent) ------------------------------------------

RIVET_TEST(cryptoProModuleEnumerationSmoke) {
    const std::filesystem::path module = "/opt/cprocsp/lib/libcppkcs11.dylib";
    if (!kPkcs11Built || !std::filesystem::exists(module)) {
        std::puts("SKIP: CryptoPro PKCS#11 module not installed or PKCS#11 not built");
        return;
    }
    Pkcs11Options options;
    options.modulePath = module;
    auto store = makePkcs11IdentityStore(options);
    // Enumeration only: no signing, no PIN prompt.
    if (!store->isAvailable()) {
        std::printf("SKIP: CryptoPro module did not initialise: %s\n", store->unavailableReason().c_str());
        return;
    }
    auto all = store->list(CancelToken{});
    CHECK(all.has_value()); // zero or more tokens: either is fine
}

RIVET_TEST(softHsm2Roundtrip) {
    std::filesystem::path module;
    if (const char* env = std::getenv("SOFTHSM2_MODULE")) module = env;
    for (const char* candidate : {"/opt/homebrew/lib/softhsm/libsofthsm2.so", "/usr/lib/softhsm/libsofthsm2.so"}) {
        if (module.empty() || !std::filesystem::exists(module)) module = candidate;
    }
    if (!kPkcs11Built || module.empty() || !std::filesystem::exists(module) || std::system("softhsm2-util --version >/dev/null 2>&1") != 0) {
        std::puts("SKIP: SoftHSM2 not installed (set SOFTHSM2_MODULE) or PKCS#11 not built");
        return;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rivet_softhsm2_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "tokens");
    {
        std::ofstream conf(dir / "softhsm2.conf");
        conf << "directories.tokendir = " << (dir / "tokens").string() << "\nobjectstore.backend = file\n";
    }
    const std::string confPath = (dir / "softhsm2.conf").string();
#ifdef _WIN32
    _putenv_s("SOFTHSM2_CONF", confPath.c_str());
#else
    setenv("SOFTHSM2_CONF", confPath.c_str(), 1);
#endif
    const std::string init = "softhsm2-util --init-token --free --label RivetTest --so-pin 5678 --pin 1234 >/dev/null 2>&1";
    if (std::system(init.c_str()) != 0) {
        std::puts("SKIP: softhsm2-util could not initialise a token");
        fs::remove_all(dir, ec);
        return;
    }
    Pkcs11Options options;
    options.modulePath = module;
    auto store = makePkcs11IdentityStore(options);
    CHECK(store->isAvailable());
    auto all = store->list(CancelToken{});
    CHECK(all.has_value()); // a fresh token holds no certificate: zero identities, no error
    if (all) CHECK(all->empty());
    store.reset();
    fs::remove_all(dir, ec);
}
