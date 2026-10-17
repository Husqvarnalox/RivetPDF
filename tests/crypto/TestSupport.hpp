// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test-only helpers that use OpenSSL directly to fabricate certificates, keys
// and PKCS#12 files independently of the code under test.

#include "crypto/Providers.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rivet::crypto::testing {

struct PkeyDel { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct X509Del { void operator()(X509* p) const { X509_free(p); } };
struct BioDel { void operator()(BIO* p) const { BIO_free_all(p); } };
struct P12Del { void operator()(PKCS12* p) const { PKCS12_free(p); } };
struct StackDel { void operator()(STACK_OF(X509)* p) const { sk_X509_pop_free(p, X509_free); } };
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDel>;
using X509Ptr = std::unique_ptr<X509, X509Del>;
using BioPtr = std::unique_ptr<BIO, BioDel>;
using P12Ptr = std::unique_ptr<PKCS12, P12Del>;

struct TestKey {
    PkeyPtr pkey;
    X509Ptr cert;
};

inline PkeyPtr makeKey(const std::string& kind) {
    if (kind == "ec256") return PkeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (kind == "ec384") return PkeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-384"));
    const unsigned bits = kind == "rsa1024" ? 1024U : kind == "rsa3072" ? 3072U : 2048U;
    return PkeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(bits)));
}

inline void addExt(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer ? issuer : cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
}

struct CertSpec {
    std::string cn = "Test Signer";
    std::string keyKind = "rsa2048";
    bool ca = false;
    long notBeforeOffsetSec = -3600;
    long notAfterOffsetSec = 3600L * 24 * 365;
    std::string sigDigest = "SHA256";
    std::string inn;   // adds 1.2.643.3.131.1.1 to the subject when non-empty
    std::string snils; // adds 1.2.643.100.3
    std::string email;
    const TestKey* issuer = nullptr; // null: self-signed
    std::string extKeyUsage;         // X509V3 conf value for extendedKeyUsage, e.g. "critical,timeStamping"; empty: none
    std::string basicConstraints;    // X509V3 conf value replacing the default basicConstraints (e.g. "critical,CA:TRUE,pathlen:0")
    std::string keyUsage;            // X509V3 conf value replacing the default keyUsage
    std::vector<std::pair<int, std::string>> extraExt; // additional extensions: (NID, X509V3 conf value)
};

inline TestKey makeCert(const CertSpec& spec) {
    static long serial = 1000;
    TestKey out;
    out.pkey = makeKey(spec.keyKind);
    out.cert.reset(X509_new());
    X509* c = out.cert.get();
    X509_set_version(c, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(c), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(c), spec.notBeforeOffsetSec);
    X509_gmtime_adj(X509_getm_notAfter(c), spec.notAfterOffsetSec);
    X509_set_pubkey(c, out.pkey.get());
    X509_NAME* name = X509_NAME_new(); // owned here; copied into the certificate below
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(spec.cn.c_str()), -1,
                               -1, 0);
    auto addOid = [&](const char* oid, const std::string& v) {
        if (v.empty()) return;
        ASN1_OBJECT* obj = OBJ_txt2obj(oid, 1);
        X509_NAME_add_entry_by_OBJ(name, obj, V_ASN1_UTF8STRING, reinterpret_cast<const unsigned char*>(v.c_str()),
                                   static_cast<int>(v.size()), -1, 0);
        ASN1_OBJECT_free(obj);
    };
    addOid("1.2.643.3.131.1.1", spec.inn);
    addOid("1.2.643.100.3", spec.snils);
    if (!spec.email.empty()) {
        X509_NAME_add_entry_by_txt(name, "emailAddress", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(spec.email.c_str()), -1, -1, 0);
    }
    X509_set_subject_name(c, name);
    X509* issuerCert = spec.issuer ? spec.issuer->cert.get() : c;
    X509_set_issuer_name(c, X509_get_subject_name(issuerCert));
    X509_NAME_free(name);
    addExt(c, issuerCert == c ? nullptr : issuerCert, NID_subject_key_identifier, "hash");
    const std::string bc = !spec.basicConstraints.empty() ? spec.basicConstraints
                           : spec.ca                      ? std::string("critical,CA:TRUE")
                                                          : std::string("critical,CA:FALSE");
    const std::string ku = !spec.keyUsage.empty() ? spec.keyUsage
                           : spec.ca              ? std::string("critical,keyCertSign,cRLSign")
                                                  : std::string("critical,digitalSignature,nonRepudiation");
    addExt(c, issuerCert == c ? nullptr : issuerCert, NID_basic_constraints, bc.c_str());
    addExt(c, issuerCert == c ? nullptr : issuerCert, NID_key_usage, ku.c_str());
    if (!spec.extKeyUsage.empty())
        addExt(c, issuerCert == c ? nullptr : issuerCert, NID_ext_key_usage, spec.extKeyUsage.c_str());
    for (const auto& [nid, value] : spec.extraExt) addExt(c, issuerCert == c ? nullptr : issuerCert, nid, value.c_str());
    EVP_PKEY* signKey = spec.issuer ? spec.issuer->pkey.get() : out.pkey.get();
    X509_sign(c, signKey, EVP_get_digestbyname(spec.sigDigest.c_str()));
    return out;
}

inline Bytes certDer(const TestKey& k) {
    unsigned char* buf = nullptr;
    const int n = i2d_X509(k.cert.get(), &buf);
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

// Builds a PKCS#12 holding `leaf` (with key) and the given CA certificates.
inline Bytes makeP12(const TestKey& leaf, const std::vector<const TestKey*>& cas, const std::string& password,
                     bool withKey = true) {
    STACK_OF(X509)* stack = sk_X509_new_null();
    for (const TestKey* ca : cas) {
        X509_up_ref(ca->cert.get());
        sk_X509_push(stack, ca->cert.get());
    }
    P12Ptr p12(PKCS12_create(password.c_str(), "test", withKey ? leaf.pkey.get() : nullptr, leaf.cert.get(), stack, 0,
                             0, 0, 0, 0));
    sk_X509_pop_free(stack, X509_free);
    unsigned char* buf = nullptr;
    const int n = i2d_PKCS12(p12.get(), &buf);
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

// Imports a fabricated identity through the provider under test.
inline std::unique_ptr<SigningIdentity> makeIdentity(CryptoProvider& provider, const TestKey& leaf,
                                                     const std::vector<const TestKey*>& cas = {}) {
    const Bytes p12 = makeP12(leaf, cas, "pw");
    auto contents = provider.importPkcs12(p12, SecretBuffer(std::string_view("pw")));
    if (!contents || contents->identities.empty()) return nullptr;
    return std::move(contents->identities.front());
}

inline bool cliAvailable() {
#if defined(_WIN32)
    return false;
#else
    return std::system("openssl version > /dev/null 2>&1") == 0;
#endif
}

inline std::filesystem::path tempDir() {
    const auto base = std::filesystem::temp_directory_path() / "rivet-crypto-tests";
    std::filesystem::create_directories(base);
    return base;
}

inline void writeFile(const std::filesystem::path& p, ByteView data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

inline Bytes readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace rivet::crypto::testing
