// SPDX-License-Identifier: MPL-2.0
//
// rivet-signature-inspect: diagnostic dump of a CMS/PKCS#7 SignedData file
// (DER, BER or PEM). With --content <file> a detached signature is verified
// as well. Never prints secrets or document contents.

#include "crypto/Cms.hpp"
#include "crypto/Providers.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace rivet::crypto;

namespace {

bool readFile(const char* path, Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* signaturePath = nullptr;
    const char* contentPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--content" && i + 1 < argc) contentPath = argv[++i];
        else if (!signaturePath && arg.rfind("--", 0) != 0) signaturePath = argv[i];
        else signaturePath = nullptr;
        if (!signaturePath) break;
    }
    if (!signaturePath) {
        std::fputs("usage: rivet-signature-inspect <signature.p7s|.p7m|.sig> [--content <file>]\n", stderr);
        return 2;
    }

    Bytes data;
    if (!readFile(signaturePath, data)) {
        std::fputs("error: cannot read the signature file\n", stderr);
        return 1;
    }
    auto model = parseCmsAny(data);
    if (!model) {
        std::fprintf(stderr, "error: %s: %s\n", std::string(toString(model.error().code)).c_str(),
                     model.error().message.c_str());
        return 1;
    }

    auto provider = makeDefaultProvider();
    std::fputs(describeCms(*model, provider.get()).c_str(), stdout);

    if (provider->isAvailable() && (contentPath || !model->isDetached())) {
        const ContentSource source = contentPath ? ContentSource::fromFile(contentPath) : ContentSource::fromBytes({});
        auto result = verifyCms(*model, *provider, contentPath ? &source : nullptr);
        if (!result) {
            std::fprintf(stderr, "verification error: %s\n", result.error().message.c_str());
            return 1;
        }
        for (const SignerVerification& sv : *result) {
            const ValidationResult& v = sv.validation;
            std::printf("verification signer %zu: %s\n", sv.index, std::string(toString(v.state)).c_str());
            std::printf("    crypto=%s certificate=%s trust=%s integrity=%s timestamp=%s revocation=%s policy=%s\n",
                        std::string(toString(v.crypto)).c_str(), std::string(toString(v.certificate)).c_str(),
                        std::string(toString(v.trust)).c_str(), std::string(toString(v.integrity)).c_str(),
                        std::string(toString(v.timestamp)).c_str(), std::string(toString(v.revocation)).c_str(),
                        std::string(toString(v.policy)).c_str());
            for (const std::string& note : v.notes) std::printf("    note: %s\n", note.c_str());
            for (const CountersignatureVerification& cs : sv.countersignatures)
                std::printf("    countersignature: %s\n", std::string(toString(cs.validation.state)).c_str());
        }
    }
    return 0;
}
