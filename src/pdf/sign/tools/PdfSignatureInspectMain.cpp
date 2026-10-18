// SPDX-License-Identifier: MPL-2.0
// Diagnostic tool: lists the signatures of a PDF with their rich validation
// status. Usage: rivet-pdf-signature-inspect --pdf file.pdf [--trust cert.(pem|der)]...
// Exit codes: 0 = file read (signature states are in the output), 1 = file
// unreadable or structurally unusable, 2 = usage error. Never prints secrets
// or document contents.

#include "pdf/sign/PdfSignatureVerifier.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

rivet::pdf::sign::Bytes readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    rivet::pdf::sign::Bytes data;
    { char blk_[8192]; while (in.read(blk_, sizeof blk_) || in.gcount() > 0) data.insert(data.end(), blk_, blk_ + in.gcount()); }
    return data;
}

constexpr const char* kUsage = "usage: rivet-pdf-signature-inspect --pdf file.pdf [--trust cert.pem|cert.der]...\n";

} // namespace

int main(int argc, char** argv) {
    std::string pdfPath;
    std::vector<std::string> trust;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pdf" && i + 1 < argc) {
            pdfPath = argv[++i];
        } else if (a == "--trust" && i + 1 < argc) {
            trust.emplace_back(argv[++i]);
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (pdfPath.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    auto provider = rivet::crypto::makeDefaultProvider();
    if (!provider || !provider->isAvailable()) {
        std::fputs("no crypto backend available (build with RIVET_ENABLE_CRYPTO=ON)\n", stderr);
        return 1;
    }
    rivet::crypto::TrustAnchorSet anchors;
    for (const std::string& t : trust) {
        bool ok = false;
        auto data = readFile(t, ok);
        if (!ok) {
            std::fprintf(stderr, "cannot read %s\n", t.c_str());
            return 1;
        }
        auto der = rivet::crypto::decodePem(data);
        auto cert = provider->parseCertificate(der ? *der : data);
        if (!cert) {
            std::fprintf(stderr, "not a certificate: %s\n", t.c_str());
            return 1;
        }
        anchors.add(*cert);
    }
    bool ok = false;
    const auto pdf = readFile(pdfPath, ok);
    if (!ok) {
        std::fprintf(stderr, "cannot read %s\n", pdfPath.c_str());
        return 1;
    }
    rivet::pdf::sign::VerifyOptions options;
    if (anchors.size() > 0) options.anchors = &anchors;
    auto result = rivet::pdf::sign::verifyPdfSignatures(pdf, *provider, options);
    if (!result) {
        std::fprintf(stderr, "%s: %s\n", std::string(rivet::pdf::sign::toString(result.error().code)).c_str(),
                     result.error().message.c_str());
        return 1;
    }
    std::printf("signatures: %zu\n", result->size());
    for (const auto& s : *result) std::fputs(rivet::pdf::sign::describeSignature(s).c_str(), stdout);
    return 0;
}
