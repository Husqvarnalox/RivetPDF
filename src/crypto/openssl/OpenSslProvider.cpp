// SPDX-License-Identifier: MPL-2.0
//
// OpenSSL 3.x/4.x backed CryptoProvider. Only documented public APIs are used.
// All OpenSSL types stay inside this translation unit.

#include "crypto/Providers.hpp"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace rivet::crypto {

namespace {

template <typename T, void (*Free)(T*)>
struct Deleter {
    void operator()(T* p) const { Free(p); }
};

using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using Pkcs12Ptr = std::unique_ptr<PKCS12, Deleter<PKCS12, PKCS12_free>>;
using NamePtr = std::unique_ptr<X509_NAME, Deleter<X509_NAME, X509_NAME_free>>;
using BnPtr = std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_free>>;

void freeSafes(STACK_OF(PKCS7) * s) { sk_PKCS7_pop_free(s, PKCS7_free); }
void freeBags(STACK_OF(PKCS12_SAFEBAG) * s) { sk_PKCS12_SAFEBAG_pop_free(s, PKCS12_SAFEBAG_free); }
void freeX509Stack(STACK_OF(X509) * s) { sk_X509_pop_free(s, X509_free); }
void freeEku(EXTENDED_KEY_USAGE* e) { EXTENDED_KEY_USAGE_free(e); }
void freeBc(BASIC_CONSTRAINTS* b) { BASIC_CONSTRAINTS_free(b); }
void freeP8(PKCS8_PRIV_KEY_INFO* p) { PKCS8_PRIV_KEY_INFO_free(p); }
void freeGens(GENERAL_NAMES* g) { GENERAL_NAMES_free(g); }

using SafesPtr = std::unique_ptr<STACK_OF(PKCS7), Deleter<STACK_OF(PKCS7), freeSafes>>;
using BagsPtr = std::unique_ptr<STACK_OF(PKCS12_SAFEBAG), Deleter<STACK_OF(PKCS12_SAFEBAG), freeBags>>;
using X509StackPtr = std::unique_ptr<STACK_OF(X509), Deleter<STACK_OF(X509), freeX509Stack>>;
using EkuPtr = std::unique_ptr<EXTENDED_KEY_USAGE, Deleter<EXTENDED_KEY_USAGE, freeEku>>;
using BcPtr = std::unique_ptr<BASIC_CONSTRAINTS, Deleter<BASIC_CONSTRAINTS, freeBc>>;
using P8Ptr = std::unique_ptr<PKCS8_PRIV_KEY_INFO, Deleter<PKCS8_PRIV_KEY_INFO, freeP8>>;
using GensPtr = std::unique_ptr<GENERAL_NAMES, Deleter<GENERAL_NAMES, freeGens>>;

// Drains the OpenSSL error queue into a lowercase text used only to classify
// failures; it is never surfaced to callers (it could mention file contents).
std::string drainErrorText() {
    std::string text;
    unsigned long code;
    while ((code = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof buf);
        text += buf;
        text += '\n';
    }
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const EVP_MD* mdFor(DigestAlgorithm d) {
    switch (d) {
    case DigestAlgorithm::Sha256: return EVP_sha256();
    case DigestAlgorithm::Sha384: return EVP_sha384();
    case DigestAlgorithm::Sha512: return EVP_sha512();
    case DigestAlgorithm::Sha1: return EVP_sha1();
    default: return nullptr; // GOST digests are never provided by this backend
    }
}

std::string toHex(const unsigned char* data, std::size_t n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 0xF];
    }
    return out;
}

std::chrono::system_clock::time_point asn1Time(const ASN1_TIME* t) {
    ASN1_TIME* epoch = ASN1_TIME_set(nullptr, 0);
    int days = 0;
    int secs = 0;
    if (!epoch || !t || !ASN1_TIME_diff(&days, &secs, epoch, t)) {
        if (epoch) ASN1_STRING_free(epoch);
        return {};
    }
    ASN1_STRING_free(epoch);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(days * 86400LL + secs)));
}

std::string oidText(const ASN1_OBJECT* obj) {
    char buf[128];
    const int n = OBJ_obj2txt(buf, sizeof buf, obj, 1);
    return n > 0 && n < static_cast<int>(sizeof buf) ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

std::string nameText(const X509_NAME* name) {
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio.get(), &data);
    return n > 0 ? std::string(data, static_cast<std::size_t>(n)) : std::string();
}

Bytes nameDer(const X509_NAME* name) {
    const unsigned char* der = nullptr;
    std::size_t len = 0;
    if (!X509_NAME_get0_der(name, &der, &len) || !der) return {};
    return Bytes(der, der + len);
}

CryptoResult<CertificateInfo> parseCertificateImpl(ByteView der) {
    if (der.empty() || der.size() > (std::size_t{16} << 20))
        return fail(CryptoErrorCode::CorruptData, "certificate size is out of range");
    const unsigned char* p = der.data();
    X509Ptr x(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
    if (!x) {
        ERR_clear_error();
        return fail(CryptoErrorCode::CorruptData, "not a valid X.509 certificate");
    }
    CertificateInfo c;
    c.der.assign(der.begin(), der.begin() + (p - der.data()));

    const X509_NAME* subject = X509_get_subject_name(x.get());
    const X509_NAME* issuer = X509_get_issuer_name(x.get());
    c.subject = nameText(subject);
    c.issuer = nameText(issuer);
    c.subjectDer = nameDer(subject);
    c.issuerDer = nameDer(issuer);

    const ASN1_INTEGER* serial = X509_get0_serialNumber(x.get());
    if (serial) {
        const unsigned char* data = ASN1_STRING_get0_data(serial);
        std::size_t i = 0;
        const std::size_t n = static_cast<std::size_t>(ASN1_STRING_length(serial));
        while (i < n && data[i] == 0) ++i;
        c.serial.assign(data + i, data + n);
        c.serialHex = c.serial.empty() ? "00" : toHex(c.serial.data(), c.serial.size());
    }
    c.notBefore = asn1Time(X509_get0_notBefore(x.get()));
    c.notAfter = asn1Time(X509_get0_notAfter(x.get()));

    const std::uint32_t ku = X509_get_key_usage(x.get());
    if (ku != UINT32_MAX) {
        c.hasKeyUsage = true;
        auto map = [&](std::uint32_t bit, std::uint16_t flag) {
            if (ku & bit) c.keyUsage = static_cast<std::uint16_t>(c.keyUsage | flag);
        };
        map(KU_DIGITAL_SIGNATURE, keyusage::kDigitalSignature);
        map(KU_NON_REPUDIATION, keyusage::kNonRepudiation);
        map(KU_KEY_ENCIPHERMENT, keyusage::kKeyEncipherment);
        map(KU_DATA_ENCIPHERMENT, keyusage::kDataEncipherment);
        map(KU_KEY_AGREEMENT, keyusage::kKeyAgreement);
        map(KU_KEY_CERT_SIGN, keyusage::kKeyCertSign);
        map(KU_CRL_SIGN, keyusage::kCrlSign);
        map(KU_ENCIPHER_ONLY, keyusage::kEncipherOnly);
        map(KU_DECIPHER_ONLY, keyusage::kDecipherOnly);
    }
    if (EkuPtr eku{static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(x.get(), NID_ext_key_usage, nullptr, nullptr))}) {
        for (int i = 0; i < sk_ASN1_OBJECT_num(eku.get()); ++i)
            c.extendedKeyUsageOids.push_back(oidText(sk_ASN1_OBJECT_value(eku.get(), i)));
    }
    if (BcPtr bc{static_cast<BASIC_CONSTRAINTS*>(X509_get_ext_d2i(x.get(), NID_basic_constraints, nullptr, nullptr))}) {
        c.isCa = bc->ca != 0;
        if (bc->pathlen) c.pathLength = static_cast<int>(ASN1_INTEGER_get(bc->pathlen));
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = 0;
    if (X509_digest(x.get(), EVP_sha256(), md, &mdLen)) c.sha256Fingerprint = toHex(md, mdLen);
    if (X509_digest(x.get(), EVP_sha1(), md, &mdLen)) c.sha1Fingerprint = toHex(md, mdLen);
    if (const ASN1_OCTET_STRING* ski = X509_get0_subject_key_id(x.get()))
        c.subjectKeyId.assign(ASN1_STRING_get0_data(ski), ASN1_STRING_get0_data(ski) + ASN1_STRING_length(ski));
    if (const ASN1_OCTET_STRING* aki = X509_get0_authority_key_id(x.get()))
        c.authorityKeyId.assign(ASN1_STRING_get0_data(aki), ASN1_STRING_get0_data(aki) + ASN1_STRING_length(aki));

    if (const EVP_PKEY* key = X509_get0_pubkey(x.get())) {
        if (EVP_PKEY_is_a(key, "RSA") || EVP_PKEY_is_a(key, "RSA-PSS")) c.publicKeyType = PublicKeyType::Rsa;
        else if (EVP_PKEY_is_a(key, "EC")) c.publicKeyType = PublicKeyType::Ec;
        c.keyBits = EVP_PKEY_get_bits(key);
    }
    {
        const X509_ALGOR* alg = nullptr;
        X509_get0_signature(nullptr, &alg, x.get());
        const ASN1_OBJECT* obj = nullptr;
        if (alg) X509_ALGOR_get0(&obj, nullptr, nullptr, alg);
        if (obj) c.signatureAlgorithmOid = oidText(obj);
    }

    for (int i = 0; i < X509_NAME_entry_count(subject); ++i) {
        const X509_NAME_ENTRY* e = X509_NAME_get_entry(subject, i);
        const std::string o = oidText(X509_NAME_ENTRY_get_object(e));
        unsigned char* utf8 = nullptr;
        const int n = ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(e));
        if (n < 0 || !utf8) continue;
        std::string value(reinterpret_cast<char*>(utf8), static_cast<std::size_t>(n));
        OPENSSL_free(utf8);
        auto assign = [&](std::optional<std::string>& slot) {
            if (!slot) slot = value;
        };
        if (o == "2.5.4.3") assign(c.commonName);
        else if (o == "2.5.4.10") assign(c.organization);
        else if (o == "1.2.840.113549.1.9.1") assign(c.email);
        else if (o == "1.2.643.3.131.1.1") assign(c.russian.inn);
        else if (o == "1.2.643.100.3") assign(c.russian.snils);
        else if (o == "1.2.643.100.1") assign(c.russian.ogrn);
        else if (o == "1.2.643.100.5") assign(c.russian.ogrnip);
        else if (o == "1.2.643.100.4") assign(c.russian.innLe);
    }
    return c;
}

CryptoResult<X509Ptr> loadX509(ByteView der) {
    const unsigned char* p = der.data();
    X509Ptr x(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
    if (!x) {
        ERR_clear_error();
        return fail(CryptoErrorCode::CorruptData, "not a valid X.509 certificate");
    }
    return x;
}

bool keyMatches(const EVP_PKEY* key, const SignatureScheme& s) {
    if (s.keyType == PublicKeyType::Rsa)
        return s.padding == SignaturePadding::Pss ? (EVP_PKEY_is_a(key, "RSA") || EVP_PKEY_is_a(key, "RSA-PSS"))
                                                  : EVP_PKEY_is_a(key, "RSA");
    if (s.keyType == PublicKeyType::Ec) return EVP_PKEY_is_a(key, "EC");
    return false;
}

// Configures padding for an initialised sign/verify context. 1 = ok.
bool configureContext(EVP_PKEY_CTX* pctx, const SignatureScheme& s, const EVP_MD* md) {
    if (s.keyType != PublicKeyType::Rsa) return true;
    if (s.padding == SignaturePadding::Pss) {
        return EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) > 0 &&
               EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, s.pssSaltLength == -1 ? RSA_PSS_SALTLEN_DIGEST : s.pssSaltLength) > 0 &&
               EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) > 0;
    }
    return EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) > 0;
}

class OsslHasher final : public Hasher {
public:
    explicit OsslHasher(MdCtxPtr ctx) : ctx_(std::move(ctx)) {}
    void update(ByteView data) override {
        if (!data.empty()) EVP_DigestUpdate(ctx_.get(), data.data(), data.size());
    }
    Bytes finish() override {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx_.get(), md, &len);
        return Bytes(md, md + len);
    }

private:
    MdCtxPtr ctx_;
};

class OpenSslProvider;

class SoftIdentity final : public SigningIdentity {
public:
    SoftIdentity(PKeyPtr key, CertificateInfo cert, std::vector<Bytes> chain)
        : key_(std::move(key)), cert_(std::move(cert)), chain_(std::move(chain)) {
        bits_ = EVP_PKEY_get_bits(key_.get());
        type_ = cert_.publicKeyType;
    }

    std::string_view providerName() const override { return "openssl"; }
    const CertificateInfo& certificate() const override { return cert_; }
    std::vector<Bytes> chainDer() const override { return chain_; }
    PublicKeyType keyType() const override { return type_; }
    int keyBits() const override { return bits_; }
    bool isExportable() const override { return true; }
    EVP_PKEY* key() const { return key_.get(); }

    std::vector<SignatureAlgorithm> supportedSignatures() const override {
        using S = SignatureAlgorithm;
        if (type_ == PublicKeyType::Rsa)
            return {S::RsaPkcs1Sha256, S::RsaPkcs1Sha384, S::RsaPkcs1Sha512,
                    S::RsaPssSha256,   S::RsaPssSha384,   S::RsaPssSha512};
        if (type_ == PublicKeyType::Ec) return {S::EcdsaSha256, S::EcdsaSha384, S::EcdsaSha512};
        return {};
    }

    CryptoResult<Bytes> sign(SignatureAlgorithm alg, ByteView message, const CancelToken& cancel) override {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        if (auto ok = checkCreationAllowed(alg, type_, bits_); !ok) return std::unexpected(ok.error());
        const SignatureScheme scheme = schemeOf(alg);
        const EVP_MD* md = mdFor(scheme.digest);
        if (!md || !keyMatches(key_.get(), scheme))
            return fail(CryptoErrorCode::UnsupportedAlgorithm, "algorithm not available for this key");

        MdCtxPtr ctx(EVP_MD_CTX_new());
        EVP_PKEY_CTX* pctx = nullptr;
        if (!ctx || EVP_DigestSignInit(ctx.get(), &pctx, md, nullptr, key_.get()) <= 0 || !configureContext(pctx, scheme, md)) {
            ERR_clear_error();
            return fail(CryptoErrorCode::Internal, "cannot initialise signing");
        }
        std::size_t len = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &len, message.data(), message.size()) <= 0) {
            ERR_clear_error();
            return fail(CryptoErrorCode::Internal, "signing failed");
        }
        Bytes sig(len);
        if (EVP_DigestSign(ctx.get(), sig.data(), &len, message.data(), message.size()) <= 0) {
            ERR_clear_error();
            return fail(CryptoErrorCode::Internal, "signing failed");
        }
        sig.resize(len);
        return sig;
    }

private:
    PKeyPtr key_;
    CertificateInfo cert_;
    std::vector<Bytes> chain_;
    int bits_ = 0;
    PublicKeyType type_ = PublicKeyType::Unknown;
};

class OpenSslProvider final : public CryptoProvider {
public:
    std::string_view name() const override { return "openssl"; }
    bool isAvailable() const override { return true; }
    bool supports(DigestAlgorithm d) const override { return mdFor(d) != nullptr; }
    bool supports(SignatureAlgorithm a) const override { return !isGost(a); }

    CryptoResult<std::unique_ptr<Hasher>> createHasher(DigestAlgorithm d) override {
        const EVP_MD* md = mdFor(d);
        if (!md) return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not provided by the OpenSSL backend");
        MdCtxPtr ctx(EVP_MD_CTX_new());
        if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) <= 0) {
            ERR_clear_error();
            return fail(CryptoErrorCode::Internal, "cannot initialise the digest");
        }
        return std::unique_ptr<Hasher>(std::make_unique<OsslHasher>(std::move(ctx)));
    }

    CryptoResult<Bytes> randomBytes(std::size_t count) override {
        Bytes out(count);
        if (count && RAND_bytes(out.data(), static_cast<int>(count)) != 1) {
            ERR_clear_error();
            return fail(CryptoErrorCode::Internal, "random generator failure");
        }
        return out;
    }

    CryptoResult<CertificateInfo> parseCertificate(ByteView der) override { return parseCertificateImpl(der); }

    bool isIssuedBy(const CertificateInfo& cert, const CertificateInfo& issuer) override {
        auto c = loadX509(cert.der);
        auto i = loadX509(issuer.der);
        if (!c || !i) return false;
        const EVP_PKEY* key = X509_get0_pubkey(i->get());
        const bool ok = key && X509_verify(c->get(), const_cast<EVP_PKEY*>(key)) == 1;
        ERR_clear_error();
        return ok;
    }

    CryptoOk verifySignature(const CertificateInfo& signer, const SignatureScheme& scheme, ByteView message,
                             ByteView signature) override {
        const EVP_MD* md = mdFor(scheme.digest);
        if (!md || scheme.padding == SignaturePadding::Gost)
            return fail(CryptoErrorCode::UnsupportedAlgorithm, "algorithm not provided by the OpenSSL backend");
        auto x = loadX509(signer.der);
        if (!x) return std::unexpected(x.error());
        EVP_PKEY* key = const_cast<EVP_PKEY*>(X509_get0_pubkey(x->get()));
        if (!key || !keyMatches(key, scheme)) return fail(CryptoErrorCode::KeyMismatch, "key does not fit the signature scheme");

        MdCtxPtr ctx(EVP_MD_CTX_new());
        EVP_PKEY_CTX* pctx = nullptr;
        if (!ctx || EVP_DigestVerifyInit(ctx.get(), &pctx, md, nullptr, key) <= 0 || !configureContext(pctx, scheme, md)) {
            ERR_clear_error();
            return fail(CryptoErrorCode::CorruptData, "cannot initialise verification");
        }
        const int rc = EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), message.data(), message.size());
        ERR_clear_error();
        if (rc != 1) return fail(CryptoErrorCode::CorruptData, "signature does not verify");
        return {};
    }

    CryptoResult<Pkcs12Contents> importPkcs12(ByteView data, const SecretBuffer& password) override;
    CryptoResult<std::unique_ptr<SigningIdentity>> createSelfSignedIdentity(const SelfSignedRequest& req) override;
    CryptoResult<Bytes> exportPkcs12(const SigningIdentity& identity, const SecretBuffer& password) override;
};

const char* passwordChars(const SecretBuffer& pw) { return pw.empty() ? "" : pw.chars(); }

CryptoResult<Pkcs12Contents> OpenSslProvider::importPkcs12(ByteView data, const SecretBuffer& password) {
    if (data.empty() || data.size() > (std::size_t{64} << 20))
        return fail(CryptoErrorCode::CorruptData, "PKCS#12 size is out of range");
    ERR_clear_error();
    const unsigned char* p = data.data();
    Pkcs12Ptr p12(d2i_PKCS12(nullptr, &p, static_cast<long>(data.size())));
    if (!p12) {
        ERR_clear_error();
        return fail(CryptoErrorCode::CorruptData, "not a valid PKCS#12 container");
    }
    const char* pw = passwordChars(password);

    bool macChecked = false;
    if (PKCS12_mac_present(p12.get())) {
        int ok = PKCS12_verify_mac(p12.get(), pw, -1);
        if (!ok && password.empty()) ok = PKCS12_verify_mac(p12.get(), nullptr, 0); // NULL vs "" empty password
        ERR_clear_error();
        if (!ok) return fail(CryptoErrorCode::WrongPassword, "the PKCS#12 password is wrong (or the file is damaged)");
        macChecked = true;
    }

    SafesPtr safes(PKCS12_unpack_authsafes(p12.get()));
    if (!safes) {
        ERR_clear_error();
        return fail(CryptoErrorCode::CorruptData, "damaged PKCS#12 structure");
    }

    std::vector<X509Ptr> certs;
    std::vector<PKeyPtr> keys;
    auto classify = [&](const char* what) -> std::unexpected<CryptoError> {
        const std::string text = drainErrorText();
        if (text.find("unsupported") != std::string::npos || text.find("fetch") != std::string::npos ||
            text.find("unknown cipher") != std::string::npos || text.find("unknown digest") != std::string::npos)
            return fail(CryptoErrorCode::UnsupportedAlgorithm, std::string("PKCS#12 uses an unsupported algorithm (") + what + ")");
        if (!macChecked) return fail(CryptoErrorCode::WrongPassword, "the PKCS#12 password is wrong (or the file is damaged)");
        return fail(CryptoErrorCode::CorruptData, std::string("damaged PKCS#12 ") + what);
    };

    for (int i = 0; i < sk_PKCS7_num(safes.get()); ++i) {
        PKCS7* p7 = sk_PKCS7_value(safes.get(), i);
        BagsPtr bags;
        if (PKCS7_type_is_data(p7)) bags.reset(PKCS12_unpack_p7data(p7));
        else if (PKCS7_type_is_encrypted(p7)) bags.reset(PKCS12_unpack_p7encdata(p7, pw, -1));
        else continue;
        if (!bags) return classify("safe");

        for (int b = 0; b < sk_PKCS12_SAFEBAG_num(bags.get()); ++b) {
            const PKCS12_SAFEBAG* bag = sk_PKCS12_SAFEBAG_value(bags.get(), b);
            switch (PKCS12_SAFEBAG_get_nid(bag)) {
            case NID_certBag:
                if (X509* x = PKCS12_SAFEBAG_get1_cert(bag)) certs.emplace_back(x);
                break;
            case NID_keyBag: {
                const PKCS8_PRIV_KEY_INFO* p8 = PKCS12_SAFEBAG_get0_p8inf(bag);
                if (EVP_PKEY* k = p8 ? EVP_PKCS82PKEY(p8) : nullptr) keys.emplace_back(k);
                break;
            }
            case NID_pkcs8ShroudedKeyBag: {
                P8Ptr p8(PKCS12_decrypt_skey(bag, pw, -1));
                if (!p8) return classify("key");
                if (EVP_PKEY* k = EVP_PKCS82PKEY(p8.get())) keys.emplace_back(k);
                else return classify("key");
                break;
            }
            default: break;
            }
        }
    }
    ERR_clear_error();

    if (keys.empty()) return fail(CryptoErrorCode::NoPrivateKey, "the PKCS#12 container holds no private key");

    std::vector<CertificateInfo> infos;
    infos.reserve(certs.size());
    for (const X509Ptr& x : certs) {
        unsigned char* der = nullptr;
        const int n = i2d_X509(x.get(), &der);
        if (n <= 0) return fail(CryptoErrorCode::CorruptData, "cannot encode a certificate");
        auto info = parseCertificateImpl(ByteView(der, static_cast<std::size_t>(n)));
        OPENSSL_free(der);
        if (!info) return std::unexpected(info.error());
        infos.push_back(std::move(*info));
    }

    Pkcs12Contents out;
    std::vector<std::string> used;
    TrustAnchorSet noAnchors;
    for (PKeyPtr& key : keys) {
        std::size_t leaf = certs.size();
        for (std::size_t i = 0; i < certs.size(); ++i)
            if (X509_check_private_key(certs[i].get(), key.get()) == 1) {
                leaf = i;
                break;
            }
        ERR_clear_error();
        if (leaf == certs.size()) continue; // a key without a certificate is not an identity
        std::vector<CertificateInfo> pool;
        for (std::size_t i = 0; i < infos.size(); ++i)
            if (i != leaf) pool.push_back(infos[i]);
        const ChainResult chain = buildChain(infos[leaf], pool, noAnchors, *this);
        std::vector<Bytes> chainDer;
        for (const CertificateInfo& c : chain.chain) {
            chainDer.push_back(c.der);
            used.push_back(c.sha256Fingerprint);
        }
        out.identities.push_back(std::make_unique<SoftIdentity>(std::move(key), infos[leaf], std::move(chainDer)));
    }
    if (out.identities.empty())
        return fail(CryptoErrorCode::NoPrivateKey, "no private key in the container matches a certificate");
    for (const CertificateInfo& c : infos)
        if (std::find(used.begin(), used.end(), c.sha256Fingerprint) == used.end()) out.otherCertificates.push_back(c.der);
    return out;
}

CryptoResult<std::unique_ptr<SigningIdentity>> OpenSslProvider::createSelfSignedIdentity(const SelfSignedRequest& req) {
    if (req.commonName.empty() || req.commonName.size() > 128)
        return fail(CryptoErrorCode::InvalidArgument, "the name must be 1 to 128 characters");
    if (req.validity <= std::chrono::hours(0))
        return fail(CryptoErrorCode::InvalidArgument, "the validity period must be positive");
    if (req.email && (req.email->empty() || req.email->size() > 128 || req.email->find('@') == std::string::npos))
        return fail(CryptoErrorCode::InvalidArgument, "invalid e-mail address");

    ERR_clear_error();
    PKeyPtr key;
    switch (req.key) {
    case KeyAlgorithm::Rsa2048: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048})); break;
    case KeyAlgorithm::Rsa3072: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{3072})); break;
    case KeyAlgorithm::Rsa4096: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{4096})); break;
    case KeyAlgorithm::EcdsaP256: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256")); break;
    case KeyAlgorithm::EcdsaP384: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-384")); break;
    case KeyAlgorithm::EcdsaP521: key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-521")); break;
    }
    if (!key) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "key generation failed");
    }

    X509Ptr x(X509_new());
    if (!x) return fail(CryptoErrorCode::Internal, "out of memory");
    X509_set_version(x.get(), X509_VERSION_3);

    unsigned char serialBytes[16];
    if (RAND_bytes(serialBytes, sizeof serialBytes) != 1) return fail(CryptoErrorCode::Internal, "random generator failure");
    serialBytes[0] &= 0x7F; // positive
    serialBytes[0] |= 0x40; // fixed length
    BnPtr bn(BN_bin2bn(serialBytes, sizeof serialBytes, nullptr));
    if (!bn || !BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(x.get())))
        return fail(CryptoErrorCode::Internal, "cannot set the serial number");

    const auto start = req.notBefore.value_or(std::chrono::system_clock::now());
    const std::time_t t0 = std::chrono::system_clock::to_time_t(start);
    const std::time_t t1 = std::chrono::system_clock::to_time_t(
        start + std::chrono::duration_cast<std::chrono::system_clock::duration>(req.validity));
    if (!ASN1_TIME_set(X509_getm_notBefore(x.get()), t0) || !ASN1_TIME_set(X509_getm_notAfter(x.get()), t1))
        return fail(CryptoErrorCode::Internal, "cannot set the validity period");

    NamePtr name(X509_NAME_new());
    auto addName = [&](int nid, const std::string& value) {
        return X509_NAME_add_entry_by_NID(name.get(), nid, MBSTRING_UTF8,
                                          reinterpret_cast<const unsigned char*>(value.data()),
                                          static_cast<int>(value.size()), -1, 0) == 1;
    };
    bool ok = name != nullptr;
    if (ok && req.organization && !req.organization->empty()) ok = addName(NID_organizationName, *req.organization);
    if (ok) ok = addName(NID_commonName, req.commonName);
    if (ok && req.email) ok = addName(NID_pkcs9_emailAddress, *req.email);
    if (!ok || !X509_set_subject_name(x.get(), name.get()) || !X509_set_issuer_name(x.get(), name.get()) ||
        !X509_set_pubkey(x.get(), key.get()))
        return fail(CryptoErrorCode::Internal, "cannot set the certificate subject");

    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, x.get(), x.get(), nullptr, nullptr, 0);
    auto addExt = [&](int nid, const char* value) {
        X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (!e) return false;
        const int rc = X509_add_ext(x.get(), e, -1);
        X509_EXTENSION_free(e);
        return rc == 1;
    };
    ok = addExt(NID_basic_constraints, "critical,CA:FALSE") &&
         addExt(NID_key_usage, "critical,digitalSignature,nonRepudiation") &&
         addExt(NID_ext_key_usage, "emailProtection") && addExt(NID_subject_key_identifier, "hash");
    if (ok && req.email) {
        GensPtr gens(GENERAL_NAMES_new());
        GENERAL_NAME* gn = GENERAL_NAME_new();
        ASN1_IA5STRING* ia5 = ASN1_IA5STRING_new();
        if (!gens || !gn || !ia5 ||
            !ASN1_STRING_set(ia5, req.email->data(), static_cast<int>(req.email->size()))) {
            if (gn) GENERAL_NAME_free(gn);
            if (ia5) ASN1_IA5STRING_free(ia5);
            return fail(CryptoErrorCode::Internal, "cannot encode the e-mail address");
        }
        GENERAL_NAME_set0_value(gn, GEN_EMAIL, ia5);
        sk_GENERAL_NAME_push(gens.get(), gn);
        ok = X509_add1_ext_i2d(x.get(), NID_subject_alt_name, gens.get(), 0, 0) == 1;
    }
    if (!ok || X509_sign(x.get(), key.get(), EVP_sha256()) <= 0) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "cannot build the certificate");
    }

    unsigned char* der = nullptr;
    const int n = i2d_X509(x.get(), &der);
    if (n <= 0) return fail(CryptoErrorCode::Internal, "cannot encode the certificate");
    auto info = parseCertificateImpl(ByteView(der, static_cast<std::size_t>(n)));
    OPENSSL_free(der);
    if (!info) return std::unexpected(info.error());
    std::vector<Bytes> chain{info->der};
    return std::unique_ptr<SigningIdentity>(std::make_unique<SoftIdentity>(std::move(key), std::move(*info), std::move(chain)));
}

CryptoResult<Bytes> OpenSslProvider::exportPkcs12(const SigningIdentity& identity, const SecretBuffer& password) {
    const auto* soft = dynamic_cast<const SoftIdentity*>(&identity);
    if (!soft || !identity.isExportable())
        return fail(CryptoErrorCode::NotExportable, "this identity cannot be exported");
    if (password.empty()) return fail(CryptoErrorCode::InvalidArgument, "a non-empty password is required for export");

    const std::vector<Bytes> chain = identity.chainDer();
    if (chain.empty()) return fail(CryptoErrorCode::Internal, "identity without a certificate");
    auto leaf = loadX509(chain[0]);
    if (!leaf) return std::unexpected(leaf.error());
    X509StackPtr ca(sk_X509_new_null());
    for (std::size_t i = 1; i < chain.size(); ++i) {
        auto c = loadX509(chain[i]);
        if (!c) return std::unexpected(c.error());
        sk_X509_push(ca.get(), c->release());
    }
    Pkcs12Ptr p12(PKCS12_create(password.chars(), nullptr, soft->key(), leaf->get(), ca.get(), 0, 0, 0, 0, 0));
    if (!p12) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "PKCS#12 export failed");
    }
    unsigned char* der = nullptr;
    const int n = i2d_PKCS12(p12.get(), &der);
    if (n <= 0) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "PKCS#12 encoding failed");
    }
    Bytes out(der, der + n);
    OPENSSL_free(der);
    return out;
}

} // namespace

std::shared_ptr<CryptoProvider> makeDefaultProvider() { return std::make_shared<OpenSslProvider>(); }

} // namespace rivet::crypto
