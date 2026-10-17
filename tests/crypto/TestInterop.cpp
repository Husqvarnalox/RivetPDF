// SPDX-License-Identifier: MPL-2.0
// Interoperability with OpenSSL's own CMS implementation (API and CLI).
#include "RivetTest.h"

#include "TestSupport.hpp"
#include "crypto/Cms.hpp"

#include <openssl/cms.h>
#include <openssl/pem.h>

#include <cstdlib>

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

Bytes bytesOf(std::string_view s) { return Bytes(s.begin(), s.end()); }

BioPtr memBio(ByteView data) { return BioPtr(BIO_new_mem_buf(data.data(), static_cast<int>(data.size()))); }

// Verifies a DER CMS with OpenSSL's CMS API (signature only; no chain checks).
bool opensslVerifies(ByteView cmsDer, const Bytes* detached, ByteView certDerBytes) {
    BioPtr in = memBio(cmsDer);
    CMS_ContentInfo* raw = d2i_CMS_bio(in.get(), nullptr);
    if (!raw) return false;
    std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> cms(raw, &CMS_ContentInfo_free);
    const unsigned char* p = certDerBytes.data();
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(certDerBytes.size())));
    STACK_OF(X509)* certs = sk_X509_new_null();
    sk_X509_push(certs, cert.get());
    X509_STORE* store = X509_STORE_new();
    BioPtr content = detached ? memBio(*detached) : BioPtr();
    BioPtr out(BIO_new(BIO_s_mem()));
    const int ok = CMS_verify(cms.get(), certs, store, content.get(), out.get(), CMS_NO_SIGNER_CERT_VERIFY);
    sk_X509_free(certs);
    X509_STORE_free(store);
    return ok == 1;
}

} // namespace

RIVET_TEST(opensslVerifiesRivetSignatures) {
    auto provider = makeDefaultProvider();
    for (const char* kind : {"rsa2048", "ec256"}) {
        const auto key = makeCert({.cn = "Interop", .keyKind = kind});
        auto id = makeIdentity(*provider, key);
        CHECK(id != nullptr);
        if (!id) continue;
        const Bytes content = bytesOf("interop payload");
        for (bool detached : {false, true}) {
            auto cms = signCms(*provider, *id, ContentSource::fromBytes(content), detached);
            CHECK(cms.has_value());
            if (!cms) continue;
            auto der = encodeCms(*cms);
            CHECK(der.has_value());
            if (!der) continue;
            CHECK(opensslVerifies(*der, detached ? &content : nullptr, certDer(key)));
            if (detached) {
                const Bytes other = bytesOf("interop payloaD");
                CHECK(!opensslVerifies(*der, &other, certDer(key)));
            }
        }
    }
}

RIVET_TEST(opensslVerifiesRivetPss) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "PSS"});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    CmsSignOptions o;
    o.signature = SignatureAlgorithm::RsaPssSha256;
    const Bytes content = bytesOf("pss payload");
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(content), true, o);
    CHECK(cms.has_value());
    if (!cms) return;
    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (der) CHECK(opensslVerifies(*der, &content, certDer(key)));
}

RIVET_TEST(rivetVerifiesOpensslSignatures) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "OpenSSL Signer"});
    const Bytes content = bytesOf("made by openssl");
    for (bool detached : {false, true}) {
        BioPtr in = memBio(content);
        std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> cms(
            CMS_sign(key.cert.get(), key.pkey.get(), nullptr, in.get(),
                     CMS_BINARY | (detached ? CMS_DETACHED : 0)),
            &CMS_ContentInfo_free);
        CHECK(cms != nullptr);
        if (!cms) continue;
        BioPtr out(BIO_new(BIO_s_mem()));
        CHECK(i2d_CMS_bio(out.get(), cms.get()) == 1);
        char* data = nullptr;
        const long n = BIO_get_mem_data(out.get(), &data);
        const Bytes der(reinterpret_cast<std::uint8_t*>(data), reinterpret_cast<std::uint8_t*>(data) + n);
        auto parsed = parseCms(der);
        CHECK(parsed.has_value());
        if (!parsed) continue;
        CHECK_EQ(parsed->isDetached(), detached);
        const auto src = ContentSource::fromBytes(content);
        auto v = verifyCms(*parsed, *provider, detached ? &src : nullptr);
        CHECK(v.has_value());
        if (v) CHECK(v->at(0).validation.state == SignatureState::ValidUntrusted);
    }
}

RIVET_TEST(rivetFlagsOpensslSha1SignatureAsWeakLegacy) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "Legacy"});
    const Bytes content = bytesOf("sha1 era");
    BioPtr in = memBio(content);
    // CMS_PARTIAL lets us pick the digest.
    std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> cms(
        CMS_sign(nullptr, nullptr, nullptr, in.get(), CMS_BINARY | CMS_PARTIAL | CMS_DETACHED), &CMS_ContentInfo_free);
    CHECK(cms != nullptr);
    if (!cms) return;
    CMS_SignerInfo* si = CMS_add1_signer(cms.get(), key.cert.get(), key.pkey.get(), EVP_sha1(), CMS_BINARY);
    CHECK(si != nullptr);
    if (!si) return;
    BioPtr in2 = memBio(content);
    CHECK(CMS_final(cms.get(), in2.get(), nullptr, CMS_BINARY | CMS_DETACHED) == 1);
    BioPtr out(BIO_new(BIO_s_mem()));
    CHECK(i2d_CMS_bio(out.get(), cms.get()) == 1);
    char* data = nullptr;
    const long n = BIO_get_mem_data(out.get(), &data);
    const Bytes der(reinterpret_cast<std::uint8_t*>(data), reinterpret_cast<std::uint8_t*>(data) + n);
    auto parsed = parseCms(der);
    CHECK(parsed.has_value());
    if (!parsed) return;
    const auto src = ContentSource::fromBytes(content);
    auto v = verifyCms(*parsed, *provider, &src);
    CHECK(v.has_value());
    if (!v) return;
    CHECK_EQ(v->at(0).validation.state, SignatureState::WeakLegacyAlgorithm);
    CHECK(v->at(0).validation.policy == PolicyStatus::WeakLegacyAlgorithm);
    CHECK(v->at(0).validation.crypto == CryptoCheck::Valid); // still verifies
}

RIVET_TEST(opensslCliVerifiesRivetSignature) {
    if (!cliAvailable()) return; // skipped: no openssl CLI on PATH
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "CLI"});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    const Bytes content = bytesOf("cli cross-check payload");
    const auto dir = tempDir();
    writeFile(dir / "cli-content.txt", content);
    writeFile(dir / "cli-signer.der", certDer(key));
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(content), true);
    CHECK(cms.has_value());
    if (!cms) return;
    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (!der) return;
    writeFile(dir / "cli-sig.der", *der);
    const std::string base = "openssl cms -verify -binary -inform DER -noverify -certfile '" +
                             (dir / "cli-signer.der").string() + "' ";
    // The signer certificate is embedded; -certfile accepts PEM only, so rely on the embedded one.
    const std::string cmd = "openssl cms -verify -binary -inform DER -noverify -in '" + (dir / "cli-sig.der").string() +
                            "' -content '" + (dir / "cli-content.txt").string() + "' -out /dev/null 2>/dev/null";
    (void)base;
    CHECK_EQ(std::system(cmd.c_str()), 0);

    // Negative control: different content must fail.
    writeFile(dir / "cli-content2.txt", bytesOf("cli cross-check payloaD"));
    const std::string bad = "openssl cms -verify -binary -inform DER -noverify -in '" + (dir / "cli-sig.der").string() +
                            "' -content '" + (dir / "cli-content2.txt").string() + "' -out /dev/null 2>/dev/null";
    CHECK(std::system(bad.c_str()) != 0);
}
