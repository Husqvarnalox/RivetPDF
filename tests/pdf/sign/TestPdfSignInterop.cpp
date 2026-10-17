// SPDX-License-Identifier: MPL-2.0
// Interop checks against independent tools. Each check is skipped (with a
// message naming the missing tool) when the tool is not installed.

#include "RivetTest.h"

#include "SignTestSupport.hpp"

#include <openssl/cms.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <fstream>

using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;
namespace crypto = rivet::crypto;

namespace {

void writeFile(const std::filesystem::path& p, const Bytes& b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// openssl cms -verify equivalent, in process: detached content = the two ByteRange spans.
bool opensslVerifiesPdfSignature(const Bytes& pdf, const PdfSignatureInfo& info, const Bytes& certDerBytes) {
    if (info.byteRange.size() != 4) return false;
    Bytes content(pdf.begin() + info.byteRange[0], pdf.begin() + info.byteRange[0] + info.byteRange[1]);
    content.insert(content.end(), pdf.begin() + info.byteRange[2], pdf.begin() + info.byteRange[2] + info.byteRange[3]);
    // Contents hex sits in the gap: "<hex>".
    const std::size_t gapBegin = static_cast<std::size_t>(info.byteRange[1]);
    const std::size_t gapEnd = static_cast<std::size_t>(info.byteRange[2]);
    Bytes der;
    auto nib = [](std::uint8_t c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = gapBegin + 1; i + 2 <= gapEnd - 1; i += 2)
        der.push_back(static_cast<std::uint8_t>(nib(pdf[i]) << 4 | nib(pdf[i + 1])));
    const unsigned char* p = der.data();
    CMS_ContentInfo* cms = d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(der.size())); // trailing zero padding tolerated
    if (!cms) return false;
    BIO* data = BIO_new_mem_buf(content.data(), static_cast<int>(content.size()));
    const unsigned char* cp = certDerBytes.data();
    X509* cert = d2i_X509(nullptr, &cp, static_cast<long>(certDerBytes.size()));
    STACK_OF(X509)* certs = sk_X509_new_null();
    sk_X509_push(certs, cert);
    const int ok = CMS_verify(cms, certs, nullptr, data, nullptr, CMS_BINARY | CMS_NO_SIGNER_CERT_VERIFY);
    sk_X509_free(certs);
    X509_free(cert);
    BIO_free(data);
    CMS_ContentInfo_free(cms);
    return ok == 1;
}

} // namespace

RIVET_TEST(opensslCmsVerifiesTheByteRangeContent) {
    Signer s;
    for (auto profile : {SignatureProfile::PadesBaselineB, SignatureProfile::Pkcs7Detached}) {
        auto out = signPdf(makeFixture(), basicRequest(profile), *s.provider, *s.identity);
        CHECK(out.has_value());
        if (!out) continue;
        const auto sigs = verifyAll(out->data, s);
        CHECK_EQ(sigs.size(), std::size_t{1});
        if (sigs.empty()) continue;
        CHECK(opensslVerifiesPdfSignature(out->data, sigs[0], crypto::testing::certDer(s.key)));
        Bytes tampered = out->data;
        tampered[20] ^= 1; // inside the first covered span
        CHECK(!opensslVerifiesPdfSignature(tampered, sigs[0], crypto::testing::certDer(s.key)));
    }
}

RIVET_TEST(pdfsigAgreesWithRivet) {
    if (!toolAvailable("pdfsig")) {
        std::printf("    SKIP pdfsig not installed\n");
        return;
    }
    Signer s;
    const auto dir = scratchDir("pdfsig");
    auto out = signPdf(makeFixture(), basicRequest(), *s.provider, *s.identity);
    CHECK(out.has_value());
    if (!out) return;
    writeFile(dir / "ok.pdf", out->data);
    Bytes tampered = out->data;
    const std::string needle = "(hello)";
    auto it = std::search(tampered.begin(), tampered.end(), needle.begin(), needle.end());
    CHECK(it != tampered.end());
    if (it != tampered.end()) *(it + 1) = 'J';
    writeFile(dir / "bad.pdf", tampered);

    std::string log;
    runCommand("pdfsig '" + (dir / "ok.pdf").string() + "'", log);
    std::printf("    pdfsig(valid): %s\n", log.find("Signature is Valid") != std::string::npos ? "Signature is Valid" : "(see below)");
    CHECK(log.find("Signature Validation: Signature is Valid") != std::string::npos ||
          log.find("Signature is Valid") != std::string::npos);
    CHECK(log.find("Signature is Invalid") == std::string::npos);

    runCommand("pdfsig '" + (dir / "bad.pdf").string() + "'", log);
    CHECK(log.find("Signature is Invalid") != std::string::npos || log.find("Digest Mismatch") != std::string::npos ||
          log.find("Signature Validation: Signature is Valid") == std::string::npos);
}

RIVET_TEST(openssl_cli_available_report) {
    std::printf("    interop tools: openssl=%s pdfsig=%s qpdf=%s\n", toolAvailable("openssl") ? "yes" : "no",
                toolAvailable("pdfsig") ? "yes" : "no", toolAvailable("qpdf") ? "yes" : "no");
}

RIVET_TEST(qpdfCheckAcceptsSignedFiles) {
    if (!toolAvailable("qpdf")) {
        std::printf("    SKIP qpdf not installed\n");
        return;
    }
    Signer s;
    const auto dir = scratchDir("qpdf");
    int n = 0;
    for (const FixtureSpec& spec : {FixtureSpec{}, FixtureSpec{.xrefStream = true}, FixtureSpec{.xrefStream = true, .objectStream = true}}) {
        auto first = signPdf(makeFixture(spec), basicRequest(), *s.provider, *s.identity);
        CHECK(first.has_value());
        if (!first) continue;
        auto second = signPdf(first->data, basicRequest(), *s.provider, *s.identity);
        CHECK(second.has_value());
        if (!second) continue;
        const auto path = dir / ("f" + std::to_string(n++) + ".pdf");
        writeFile(path, second->data);
        std::string log;
        const int rc = runCommand("qpdf --check '" + path.string() + "'", log);
        if (rc != 0) std::printf("    qpdf: %s\n", log.c_str());
        CHECK(rc == 0 || rc == 768); // 3 (warnings) is tolerated, 2 (errors) is not
    }
}
