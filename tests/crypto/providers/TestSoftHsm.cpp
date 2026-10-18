// SPDX-License-Identifier: MPL-2.0
//
// Real-module PKCS#11 test against SoftHSM2 (Linux CI; the Ubuntu package
// `softhsm2`). The test fabricates everything at run time: a throw-away token
// directory, a random PIN, an RSA-2048 key and a self-signed certificate that
// are written to the token through the module's own C_CreateObject. Nothing
// is committed and nothing outside a private temporary directory is touched.
// It then enumerates identities and signs through the Rivet PKCS#11 provider,
// and verifies the resulting CAdES-BES / PAdES-B-B style detached CMS with the
// existing verifier.
//
// Module discovery: RIVET_SOFTHSM_MODULE (path to libsofthsm2.so), else the
// usual Debian / Ubuntu / Fedora paths. When no module is found the test
// prints "SKIP: ..." and passes, so it never fails a machine without SoftHSM.
// Whatever the module does once found is a real failure.

#include "RivetTest.h"

#include "TestSupport.hpp"
#include "crypto/Cms.hpp"
#include "crypto/providers/pkcs11/Pkcs11Abi.hpp"
#include "crypto/providers/pkcs11/Pkcs11IdentityStore.hpp"

#include <dlfcn.h>
#include <openssl/bn.h>
#include <openssl/rand.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

using namespace rivet;
using namespace rivet::crypto;
using namespace rivet::crypto::testing;
namespace p11 = rivet::crypto::p11;

namespace {

namespace fs = std::filesystem;

// Attribute / key constants the Rivet ABI subset does not need.
constexpr p11::CK_ATTRIBUTE_TYPE kSensitive = 0x00000103;
constexpr p11::CK_ATTRIBUTE_TYPE kEncrypt = 0x00000104;
constexpr p11::CK_ATTRIBUTE_TYPE kVerify = 0x0000010A;
constexpr p11::CK_ATTRIBUTE_TYPE kPrivateExponent = 0x00000123;
constexpr p11::CK_ATTRIBUTE_TYPE kPrime1 = 0x00000124;
constexpr p11::CK_ATTRIBUTE_TYPE kPrime2 = 0x00000125;
constexpr p11::CK_ATTRIBUTE_TYPE kExponent1 = 0x00000126;
constexpr p11::CK_ATTRIBUTE_TYPE kExponent2 = 0x00000127;
constexpr p11::CK_ATTRIBUTE_TYPE kCoefficient = 0x00000128;
constexpr p11::CK_ATTRIBUTE_TYPE kExtractable = 0x00000162;

using InitTokenFn = p11::CK_RV (*)(p11::CK_SLOT_ID, p11::CK_UTF8CHAR*, p11::CK_ULONG, p11::CK_UTF8CHAR*);
using InitPinFn = p11::CK_RV (*)(p11::CK_SESSION_HANDLE, p11::CK_UTF8CHAR*, p11::CK_ULONG);
using CreateObjectFn = p11::CK_RV (*)(p11::CK_SESSION_HANDLE, p11::CK_ATTRIBUTE*, p11::CK_ULONG, p11::CK_OBJECT_HANDLE*);
using GetFunctionListFn = p11::CK_RV (*)(p11::CK_FUNCTION_LIST**);

constexpr const char* kTokenLabel = "RivetSoftHsmTest";
constexpr const char* kSubject = "Rivet SoftHSM Test Signer";

std::optional<fs::path> findModule() {
    std::error_code ec;
    if (const char* env = std::getenv("RIVET_SOFTHSM_MODULE"); env && *env) {
        const fs::path p = env;
        if (fs::is_regular_file(p, ec)) return p;
        std::fprintf(stderr, "RIVET_SOFTHSM_MODULE=%s is not a file\n", env);
        return std::nullopt;
    }
    for (const char* candidate : {"/usr/lib/softhsm/libsofthsm2.so", "/usr/lib/x86_64-linux-gnu/softhsm/libsofthsm2.so",
                                  "/usr/lib/aarch64-linux-gnu/softhsm/libsofthsm2.so", "/usr/lib64/softhsm/libsofthsm2.so",
                                  "/usr/local/lib/softhsm/libsofthsm2.so"}) {
        if (fs::is_regular_file(candidate, ec)) return fs::path(candidate);
    }
    return std::nullopt;
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> raw;
    raw.resize(bytes);
    if (RAND_bytes(raw.data(), static_cast<int>(bytes)) != 1) return "00000000";
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char b : raw) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

// Removes the throw-away token directory however the test ends.
struct TempTokenDir {
    fs::path path;
    ~TempTokenDir() {
        std::error_code ec;
        if (!path.empty()) fs::remove_all(path, ec);
    }
};

Bytes bnParam(EVP_PKEY* key, const char* name) {
    BIGNUM* bn = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &bn) != 1 || !bn) return {};
    Bytes out;
    out.resize(static_cast<std::size_t>(BN_num_bytes(bn)));
    if (!out.empty()) BN_bn2bin(bn, out.data());
    BN_free(bn);
    return out;
}

template <typename Encoder, typename Object>
Bytes derOf(Encoder encode, Object* object) {
    unsigned char* buf = nullptr;
    const int n = encode(object, &buf);
    if (n <= 0 || !buf) return {};
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

p11::CK_ATTRIBUTE attr(p11::CK_ATTRIBUTE_TYPE type, const void* value, std::size_t size) {
    return p11::CK_ATTRIBUTE{type, const_cast<void*>(value), static_cast<p11::CK_ULONG>(size)};
}

p11::CK_ATTRIBUTE attr(p11::CK_ATTRIBUTE_TYPE type, const Bytes& value) { return attr(type, value.data(), value.size()); }

class FixedPin final : public PinPrompt {
public:
    explicit FixedPin(std::string pin) : pin_(std::move(pin)) {}
    std::optional<SecretBuffer> requestPin(const PinRequest&, const CancelToken&) override {
        ++requests;
        return SecretBuffer(std::string_view(pin_));
    }
    int requests = 0;

private:
    std::string pin_;
};

// Raw Cryptoki access used only to provision the token. The Rivet provider
// loads the module again afterwards, as it would in production.
struct Provisioner {
    void* lib = nullptr;
    p11::CK_FUNCTION_LIST* fn = nullptr;
    bool initialized = false;

    ~Provisioner() {
        if (initialized && fn && fn->C_Finalize) fn->C_Finalize(nullptr);
        if (lib) dlclose(lib);
    }

    bool load(const fs::path& module) {
        lib = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            std::fprintf(stderr, "dlopen(%s): %s\n", module.c_str(), dlerror());
            return false;
        }
        auto getList = reinterpret_cast<GetFunctionListFn>(dlsym(lib, "C_GetFunctionList"));
        if (!getList || getList(&fn) != p11::CKR_OK || !fn) return false;
        if (fn->C_Initialize(nullptr) != p11::CKR_OK) return false;
        initialized = true;
        return true;
    }

    // Slot whose token has `label`, or nullopt.
    std::optional<p11::CK_SLOT_ID> slotWithLabel(const std::string& label) const {
        p11::CK_ULONG count = 0;
        if (fn->C_GetSlotList(p11::CK_TRUE, nullptr, &count) != p11::CKR_OK) return std::nullopt;
        std::vector<p11::CK_SLOT_ID> slots;
        slots.resize(count);
        if (count == 0 || fn->C_GetSlotList(p11::CK_TRUE, slots.data(), &count) != p11::CKR_OK) return std::nullopt;
        for (p11::CK_ULONG i = 0; i < count; ++i) {
            p11::CK_TOKEN_INFO info{};
            if (fn->C_GetTokenInfo(slots[i], &info) != p11::CKR_OK) continue;
            if (std::string(reinterpret_cast<const char*>(info.label), label.size()) == label) return slots[i];
        }
        return std::nullopt;
    }

    // First slot whose token is not initialised yet (SoftHSM2 offers one).
    std::optional<p11::CK_SLOT_ID> freeSlot() const {
        p11::CK_ULONG count = 0;
        if (fn->C_GetSlotList(p11::CK_FALSE, nullptr, &count) != p11::CKR_OK || count == 0) return std::nullopt;
        std::vector<p11::CK_SLOT_ID> slots;
        slots.resize(count);
        if (fn->C_GetSlotList(p11::CK_FALSE, slots.data(), &count) != p11::CKR_OK) return std::nullopt;
        for (p11::CK_ULONG i = 0; i < count; ++i) {
            p11::CK_TOKEN_INFO info{};
            if (fn->C_GetTokenInfo(slots[i], &info) != p11::CKR_OK) continue;
            if ((info.flags & p11::CKF_TOKEN_INITIALIZED) == 0) return slots[i];
        }
        return std::nullopt;
    }
};

bool rvOk(const char* what, p11::CK_RV rv) {
    if (rv == p11::CKR_OK) return true;
    std::fprintf(stderr, "%s failed: CK_RV=0x%lx\n", what, static_cast<unsigned long>(rv));
    return false;
}

// Creates the token, sets the user PIN and stores the key pair + certificate.
bool provision(const fs::path& module, const std::string& soPin, const std::string& userPin, const TestKey& key) {
    Provisioner hsm;
    if (!hsm.load(module)) return false;

    const auto initToken = reinterpret_cast<InitTokenFn>(hsm.fn->C_InitToken);
    const auto initPin = reinterpret_cast<InitPinFn>(hsm.fn->C_InitPIN);
    const auto createObject = reinterpret_cast<CreateObjectFn>(hsm.fn->C_CreateObject);
    if (!initToken || !initPin || !createObject) return false;

    const auto slot = hsm.freeSlot();
    if (!slot) {
        std::fputs("no uninitialised SoftHSM slot\n", stderr);
        return false;
    }
    p11::CK_UTF8CHAR label[32];
    std::memset(label, ' ', sizeof label);
    std::memcpy(label, kTokenLabel, std::strlen(kTokenLabel));
    if (!rvOk("C_InitToken", initToken(*slot, reinterpret_cast<p11::CK_UTF8CHAR*>(const_cast<char*>(soPin.data())),
                                       soPin.size(), label)))
        return false;

    // SoftHSM2 re-derives the slot id from the token serial: look it up again.
    const auto tokenSlot = hsm.slotWithLabel(kTokenLabel);
    if (!tokenSlot) {
        std::fputs("initialised token not found\n", stderr);
        return false;
    }
    p11::CK_SESSION_HANDLE session = 0;
    if (!rvOk("C_OpenSession(SO)", hsm.fn->C_OpenSession(*tokenSlot, p11::CKF_SERIAL_SESSION | p11::CKF_RW_SESSION,
                                                         nullptr, nullptr, &session)))
        return false;
    if (!rvOk("C_Login(SO)", hsm.fn->C_Login(session, p11::CKU_SO,
                                              reinterpret_cast<p11::CK_UTF8CHAR*>(const_cast<char*>(soPin.data())),
                                              soPin.size())))
        return false;
    if (!rvOk("C_InitPIN", initPin(session, reinterpret_cast<p11::CK_UTF8CHAR*>(const_cast<char*>(userPin.data())),
                                   userPin.size())))
        return false;
    hsm.fn->C_CloseSession(session); // also logs the SO out

    session = 0;
    if (!rvOk("C_OpenSession(user)", hsm.fn->C_OpenSession(*tokenSlot, p11::CKF_SERIAL_SESSION | p11::CKF_RW_SESSION,
                                                           nullptr, nullptr, &session)))
        return false;
    if (!rvOk("C_Login(user)", hsm.fn->C_Login(session, p11::CKU_USER,
                                                reinterpret_cast<p11::CK_UTF8CHAR*>(const_cast<char*>(userPin.data())),
                                                userPin.size())))
        return false;

    EVP_PKEY* pkey = key.pkey.get();
    const Bytes modulus = bnParam(pkey, "n");
    const Bytes exponent = bnParam(pkey, "e");
    const Bytes d = bnParam(pkey, "d");
    const Bytes p = bnParam(pkey, "rsa-factor1");
    const Bytes q = bnParam(pkey, "rsa-factor2");
    const Bytes dp = bnParam(pkey, "rsa-exponent1");
    const Bytes dq = bnParam(pkey, "rsa-exponent2");
    const Bytes qinv = bnParam(pkey, "rsa-coefficient1");
    if (modulus.empty() || exponent.empty() || d.empty() || p.empty() || q.empty() || dp.empty() || dq.empty() ||
        qinv.empty())
        return false;

    const Bytes id = {0x52, 0x56, 0x01};
    const std::string keyLabel = "rivet-test-key";
    const Bytes subject = derOf(i2d_X509_NAME, X509_get_subject_name(key.cert.get()));
    const Bytes issuer = derOf(i2d_X509_NAME, X509_get_issuer_name(key.cert.get()));
    const Bytes serial = derOf(i2d_ASN1_INTEGER, X509_get_serialNumber(key.cert.get()));
    const Bytes certDerBytes = certDer(key);

    const p11::CK_BBOOL yes = p11::CK_TRUE;
    const p11::CK_BBOOL no = p11::CK_FALSE;
    const p11::CK_OBJECT_CLASS privateClass = p11::CKO_PRIVATE_KEY;
    const p11::CK_OBJECT_CLASS publicClass = p11::CKO_PUBLIC_KEY;
    const p11::CK_OBJECT_CLASS certClass = p11::CKO_CERTIFICATE;
    const p11::CK_KEY_TYPE rsa = p11::CKK_RSA;
    const p11::CK_CERTIFICATE_TYPE x509 = p11::CKC_X_509;

    p11::CK_OBJECT_HANDLE handle = 0;

    std::vector<p11::CK_ATTRIBUTE> privateKey;
    privateKey.push_back(attr(p11::CKA_CLASS, &privateClass, sizeof privateClass));
    privateKey.push_back(attr(p11::CKA_KEY_TYPE, &rsa, sizeof rsa));
    privateKey.push_back(attr(p11::CKA_TOKEN, &yes, 1));
    privateKey.push_back(attr(p11::CKA_PRIVATE, &yes, 1));
    privateKey.push_back(attr(kSensitive, &yes, 1));
    privateKey.push_back(attr(kExtractable, &no, 1));
    privateKey.push_back(attr(p11::CKA_SIGN, &yes, 1));
    privateKey.push_back(attr(p11::CKA_LABEL, keyLabel.data(), keyLabel.size()));
    privateKey.push_back(attr(p11::CKA_ID, id));
    privateKey.push_back(attr(p11::CKA_SUBJECT, subject));
    privateKey.push_back(attr(p11::CKA_MODULUS, modulus));
    privateKey.push_back(attr(p11::CKA_PUBLIC_EXPONENT, exponent));
    privateKey.push_back(attr(kPrivateExponent, d));
    privateKey.push_back(attr(kPrime1, p));
    privateKey.push_back(attr(kPrime2, q));
    privateKey.push_back(attr(kExponent1, dp));
    privateKey.push_back(attr(kExponent2, dq));
    privateKey.push_back(attr(kCoefficient, qinv));
    if (!rvOk("C_CreateObject(private key)",
              createObject(session, privateKey.data(), static_cast<p11::CK_ULONG>(privateKey.size()), &handle)))
        return false;

    // The public key object is what lets the provider see the pair before login.
    std::vector<p11::CK_ATTRIBUTE> publicKey;
    publicKey.push_back(attr(p11::CKA_CLASS, &publicClass, sizeof publicClass));
    publicKey.push_back(attr(p11::CKA_KEY_TYPE, &rsa, sizeof rsa));
    publicKey.push_back(attr(p11::CKA_TOKEN, &yes, 1));
    publicKey.push_back(attr(p11::CKA_PRIVATE, &no, 1));
    publicKey.push_back(attr(kVerify, &yes, 1));
    publicKey.push_back(attr(kEncrypt, &yes, 1));
    publicKey.push_back(attr(p11::CKA_LABEL, keyLabel.data(), keyLabel.size()));
    publicKey.push_back(attr(p11::CKA_ID, id));
    publicKey.push_back(attr(p11::CKA_MODULUS, modulus));
    publicKey.push_back(attr(p11::CKA_PUBLIC_EXPONENT, exponent));
    if (!rvOk("C_CreateObject(public key)",
              createObject(session, publicKey.data(), static_cast<p11::CK_ULONG>(publicKey.size()), &handle)))
        return false;

    std::vector<p11::CK_ATTRIBUTE> cert;
    cert.push_back(attr(p11::CKA_CLASS, &certClass, sizeof certClass));
    cert.push_back(attr(p11::CKA_CERTIFICATE_TYPE, &x509, sizeof x509));
    cert.push_back(attr(p11::CKA_TOKEN, &yes, 1));
    cert.push_back(attr(p11::CKA_PRIVATE, &no, 1));
    cert.push_back(attr(p11::CKA_LABEL, keyLabel.data(), keyLabel.size()));
    cert.push_back(attr(p11::CKA_ID, id));
    cert.push_back(attr(p11::CKA_SUBJECT, subject));
    cert.push_back(attr(p11::CKA_ISSUER, issuer));
    cert.push_back(attr(p11::CKA_SERIAL_NUMBER, serial));
    cert.push_back(attr(p11::CKA_VALUE, certDerBytes));
    if (!rvOk("C_CreateObject(certificate)",
              createObject(session, cert.data(), static_cast<p11::CK_ULONG>(cert.size()), &handle)))
        return false;

    hsm.fn->C_CloseSession(session);
    return true;
}

} // namespace

RIVET_TEST(softHsmSignAndVerify) {
#if !defined(RIVET_CRYPTO_PKCS11)
    std::puts("SKIP: PKCS#11 provider not built (RIVET_ENABLE_PKCS11=OFF)");
    return;
#else
    const auto module = findModule();
    if (!module) {
        std::puts("SKIP: SoftHSM2 module not found (set RIVET_SOFTHSM_MODULE=/path/to/libsofthsm2.so)");
        return;
    }
    std::fprintf(stderr, "SoftHSM2 module: %s\n", module->c_str());

    // Private token directory + config, selected through SOFTHSM2_CONF before
    // any module code runs.
    std::string pattern = (fs::temp_directory_path() / "rivet-softhsm-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
        std::puts("SKIP: cannot create a temporary directory");
        return;
    }
    TempTokenDir dir{fs::path(pattern)};
    fs::create_directories(dir.path / "tokens");
    {
        std::ofstream conf(dir.path / "softhsm2.conf");
        conf << "directories.tokendir = " << (dir.path / "tokens").string() << "\n"
             << "objectstore.backend = file\n"
             << "log.level = ERROR\n"
             << "slots.removable = false\n";
    }
    const std::string confPath = (dir.path / "softhsm2.conf").string();
    setenv("SOFTHSM2_CONF", confPath.c_str(), 1);

    const std::string soPin = randomHex(6);
    const std::string userPin = randomHex(6);
    const TestKey key = makeCert({.cn = kSubject, .keyKind = "rsa2048"});
    CHECK(key.pkey != nullptr);
    CHECK(provision(*module, soPin, userPin, key));

    Pkcs11Options options;
    options.modulePath = *module;
    options.displayName = "SoftHSM2";
    auto store = makePkcs11IdentityStore(options);
    if (!store->isAvailable()) std::fprintf(stderr, "store unavailable: %s\n", store->unavailableReason().c_str());
    CHECK(store->isAvailable());
    CHECK(store->assurance() == ProviderAssurance::HardwareToken);

    auto all = store->list(CancelToken{});
    CHECK(all.has_value());
    if (!all) return;
    CHECK_EQ(all->size(), std::size_t{1});
    if (all->empty()) return;
    const IdentityDescriptor& descriptor = all->front();
    CHECK(descriptor.certificate.subject.find(kSubject) != std::string::npos);
    CHECK(descriptor.privateKeyAvailable);
    CHECK(descriptor.location.kind == KeyLocationKind::Token);
    CHECK(descriptor.certificate.der == certDer(key));

    auto pin = std::make_shared<FixedPin>(userPin);
    auto opened = store->open(descriptor.id, pin, CancelToken{});
    if (!opened) std::fprintf(stderr, "open failed: %s\n", opened.error().message.c_str());
    CHECK(opened.has_value());
    if (!opened) return;
    SigningIdentity& identity = **opened;
    CHECK(identity.keyType() == PublicKeyType::Rsa);
    CHECK_EQ(identity.keyBits(), 2048);
    CHECK(!identity.isExportable());

    auto provider = makeDefaultProvider();
    const std::string text = "Rivet SoftHSM2 signing test content";
    const Bytes content(text.begin(), text.end());
    const auto source = ContentSource::fromBytes(content);

    // PAdES-B-B shaped CMS: detached, signing-certificate-v2, no signing-time
    // attribute (the PDF /M entry carries the time).
    CmsSignOptions sign;
    sign.cadesBes = true;
    sign.includeSigningTime = false;
    auto cms = signCms(*provider, identity, source, true, sign);
    if (!cms) std::fprintf(stderr, "signCms failed: %s\n", cms.error().message.c_str());
    CHECK(cms.has_value());
    if (!cms) return;
    CHECK(pin->requests >= 1);

    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (!der) return;
    auto reparsed = parseCms(*der);
    CHECK(reparsed.has_value());
    if (!reparsed) return;
    CHECK(reparsed->isDetached());

    auto verdict = verifyCms(*reparsed, *provider, &source);
    CHECK(verdict.has_value());
    if (!verdict) return;
    CHECK_EQ(verdict->size(), std::size_t{1});
    CHECK(verdict->at(0).validation.crypto == CryptoCheck::Valid);
    CHECK(verdict->at(0).validation.state == SignatureState::ValidUntrusted); // self-signed: no trust anchor
    CHECK(verdict->at(0).signer.has_value());

    // A changed byte of the content must be detected.
    Bytes tampered = content;
    tampered.front() ^= 0x01;
    const auto tamperedSource = ContentSource::fromBytes(tampered);
    auto bad = verifyCms(*reparsed, *provider, &tamperedSource);
    CHECK(bad.has_value());
    if (bad && !bad->empty()) CHECK(bad->at(0).validation.state == SignatureState::DocumentModified);

    // A wrong PIN is refused by the token and never signs.
    auto wrong = std::make_shared<FixedPin>(userPin + "x");
    auto wrongOpened = store->open(descriptor.id, wrong, CancelToken{});
    if (wrongOpened) {
        auto refused = (*wrongOpened)->sign(SignatureAlgorithm::RsaPkcs1Sha256, content, CancelToken{});
        CHECK(!refused.has_value());
    }
#endif
}
