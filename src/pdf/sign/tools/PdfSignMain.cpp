// SPDX-License-Identifier: MPL-2.0
// Dev/test tool: signs a PDF with a PKCS#12 identity, or creates a throwaway
// self-signed TEST identity. Not shipped in the app. The PKCS#12 password is
// read from the RIVET_P12_PASSWORD environment variable, or from the first line
// of stdin when the variable is unset; it is never taken from argv.
//
//   rivet-pdf-sign --in a.pdf --out b.pdf --p12 id.p12 [--profile basic|pades-bb]
//       [--digest sha256|sha384|sha512] [--alg rsa|rsa-pss|ecdsa]
//       [--visible page,x,y,w,h | --invisible] [--field name] [--certify 1|2|3]
//       [--reason text] [--location text]
//   rivet-pdf-sign --make-test-identity out.p12 --alg rsa2048|rsa-pss|ecp256|ecp384 --cn name
//   rivet-pdf-sign timestamp --in a.pdf --out b.pdf --timestamp-url URL --timestamp-anchor tsa-ca.pem
//
// --timestamp-url (with --timestamp-anchor) adds an RFC 3161 signature time-stamp (PAdES B-T);
// `timestamp` appends a document time-stamp (/DocTimeStamp) revision. The request goes through
// rivet::net with the online-validation gate set to On: running this command with a TSA URL is
// the user's consent to contact that host (and only that host). Without a network transport
// (RIVET_ENABLE_NETWORK=OFF) the command fails with a BackendUnavailable error. The TSA chain is
// checked against --timestamp-anchor only (PEM or DER file); the OS store is never used.
//
// Exit codes: 0 ok, 1 operation failed, 2 usage error.

#include "crypto/Providers.hpp"
#include "crypto/SecretBuffer.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"
#include "net/Client.hpp"
#include "net/Consent.hpp"
#include "net/Transport.hpp"
#include "pdf/sign/PdfSigner.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace rivet;
using namespace rivet::pdf::sign;

constexpr const char* kUsage =
    "usage:\n"
    "  rivet-pdf-sign --in a.pdf --out b.pdf --p12 id.p12 [--profile basic|pades-bb]\n"
    "      [--digest sha256|sha384|sha512] [--alg rsa|rsa-pss|ecdsa]\n"
    "      [--visible page,x,y,w,h | --invisible] [--field name] [--certify 1|2|3]\n"
    "      [--reason text] [--location text]\n"
    "  rivet-pdf-sign --make-test-identity out.p12 --alg rsa2048|rsa-pss|ecp256|ecp384 --cn name\n"
    "  rivet-pdf-sign --in a.pdf --out b.pdf --p12 id.p12 --timestamp-url URL --timestamp-anchor tsa-ca.pem\n"
    "      [--allow-no-timestamp]   (PAdES B-T; the TSA is contacted only because you passed the URL)\n"
    "  rivet-pdf-sign timestamp --in a.pdf --out b.pdf --timestamp-url URL --timestamp-anchor tsa-ca.pem\n"
    "password: env RIVET_P12_PASSWORD, else first line of stdin (never argv)\n";

int usage() {
    std::fputs(kUsage, stderr);
    return 2;
}

crypto::SecretBuffer readPassword() {
    if (const char* env = std::getenv("RIVET_P12_PASSWORD")) return crypto::SecretBuffer(std::string_view(env));
    std::string line;
    std::getline(std::cin, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    crypto::SecretBuffer buf{std::string_view(line)};
    crypto::secureZero(line.data(), line.size());
    return buf;
}

bool writeFile(const std::string& path, const crypto::Bytes& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

int makeIdentity(const std::string& outPath, const std::string& alg, const std::string& cn) {
    crypto::SelfSignedRequest req;
    req.commonName = "RIVET TEST " + cn;
    req.organization = "Rivet TEST ONLY (throwaway, not for real use)";
    if (alg == "rsa2048" || alg == "rsa-pss") req.key = crypto::KeyAlgorithm::Rsa2048;
    else if (alg == "ecp256") req.key = crypto::KeyAlgorithm::EcdsaP256;
    else if (alg == "ecp384") req.key = crypto::KeyAlgorithm::EcdsaP384;
    else return usage();
    auto provider = crypto::makeDefaultProvider();
    auto id = provider->createSelfSignedIdentity(req);
    if (!id) {
        std::fprintf(stderr, "identity creation failed: %s\n", id.error().message.c_str());
        return 1;
    }
    const auto password = readPassword();
    auto p12 = provider->exportPkcs12(**id, password);
    if (!p12) {
        std::fprintf(stderr, "PKCS#12 export failed: %s\n", p12.error().message.c_str());
        return 1;
    }
    if (!writeFile(outPath, *p12)) {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote TEST identity %s (CN=%s)\n", outPath.c_str(), req.commonName.c_str());
    return 0;
}

bool parseVisible(const std::string& s, SignaturePlacement& p) {
    double v[5];
    if (std::sscanf(s.c_str(), "%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) return false;
    if (v[0] < 1 || v[3] <= 0 || v[4] <= 0) return false;
    p.kind = SignaturePlacement::Kind::NewVisible;
    p.pageIndex = static_cast<std::size_t>(v[0]) - 1; // 1-based on the command line
    p.rect = {v[1], v[2], v[1] + v[3], v[2] + v[4]};
    return true;
}

crypto::SignatureAlgorithm pick(crypto::DigestAlgorithm d, crypto::SignatureAlgorithm s256,
                                crypto::SignatureAlgorithm s384, crypto::SignatureAlgorithm s512) {
    return d == crypto::DigestAlgorithm::Sha256 ? s256 : d == crypto::DigestAlgorithm::Sha384 ? s384 : s512;
}

// Whole file as bytes. (Not an istreambuf_iterator range: GCC 13 -O2 reports a false
// -Wnull-dereference inside libstdc++ for that construction.)
std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string data;
    if (!in) return data;
    char buffer[8192];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) data.append(buffer, static_cast<std::size_t>(in.gcount()));
    return data;
}

// Certificates from a PEM file (several allowed) or a single DER file.
std::vector<crypto::Bytes> readCertificates(const std::string& path) {
    const std::string raw = readWholeFile(path);
    std::vector<crypto::Bytes> out;
    if (raw.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
        if (!raw.empty()) out.emplace_back(raw.begin(), raw.end());
        return out;
    }
    std::size_t pos = 0;
    while ((pos = raw.find("-----BEGIN CERTIFICATE-----", pos)) != std::string::npos) {
        pos += 27;
        const auto end = raw.find("-----END CERTIFICATE-----", pos);
        if (end == std::string::npos) break;
        crypto::Bytes der;
        std::uint32_t acc = 0;
        int bits = 0;
        for (std::size_t i = pos; i < end; ++i) {
            const char c = raw[i];
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else continue;
            acc = (acc << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
            }
        }
        out.push_back(std::move(der));
        pos = end + 25;
    }
    return out;
}

// The TSA client for the CLI. The gate is On: passing --timestamp-url is the consent.
struct TimestampSetup {
    std::shared_ptr<crypto::CryptoProvider> provider;
    std::shared_ptr<crypto::TimeStampClient> client;
    crypto::TrustAnchorSet anchors;
    std::string url;

    TimestampFetcher fetcher() {
        return [this](crypto::DigestAlgorithm alg, crypto::ByteView digest,
                      const crypto::CancelToken& cancel) -> crypto::CryptoResult<crypto::TimeStampVerification> {
            crypto::TimeStampClientOptions o;
            o.verify.anchors = &anchors;
            auto r = client->request(url, alg, digest, o, cancel);
            if (!r) return std::unexpected(r.error());
            return std::move(r->verification);
        };
    }
};

// Returns 0 and fills `out`, or an exit code (message already printed).
int setUpTimestamp(std::map<std::string, std::string>& opt, const std::shared_ptr<crypto::CryptoProvider>& provider,
                   TimestampSetup& out) {
    if (!opt.count("--timestamp-url") || !opt.count("--timestamp-anchor")) {
        std::fputs("a time-stamp needs both --timestamp-url and --timestamp-anchor (the TSA chain is never "
                   "validated against the OS store)\n",
                   stderr);
        return 2;
    }
    if (!net::networkBackendAvailable()) {
        std::fputs("time-stamping failed: BackendUnavailable: this build has no network transport "
                   "(configure with -DRIVET_ENABLE_NETWORK=ON)\n",
                   stderr);
        return 1;
    }
    for (const auto& der : readCertificates(opt["--timestamp-anchor"])) {
        auto c = provider->parseCertificate(der);
        if (!c) {
            std::fprintf(stderr, "cannot parse a certificate in %s: %s\n", opt["--timestamp-anchor"].c_str(),
                         c.error().message.c_str());
            return 1;
        }
        out.anchors.add(*c);
    }
    if (out.anchors.size() == 0) {
        std::fprintf(stderr, "no certificate found in %s\n", opt["--timestamp-anchor"].c_str());
        return 1;
    }
    auto gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    auto network = std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, net::makeDefaultTransport(), gate);
    out.provider = provider;
    out.client = std::make_shared<crypto::TimeStampClient>(network, provider);
    out.url = opt["--timestamp-url"];
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt;
    bool invisible = false;
    bool timestampCommand = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i == 1 && a == "timestamp") {
            timestampCommand = true;
        } else if (a == "--allow-no-timestamp") {
            opt[a] = "1";
        } else if (a == "--invisible") {
            invisible = true;
        } else if (a.rfind("--", 0) == 0 && i + 1 < argc) {
            opt[a] = argv[++i];
        } else {
            return usage();
        }
    }
    if (opt.count("--make-test-identity")) {
        if (!opt.count("--alg") || !opt.count("--cn")) return usage();
        return makeIdentity(opt["--make-test-identity"], opt["--alg"], opt["--cn"]);
    }
    if (timestampCommand) {
        if (!opt.count("--in") || !opt.count("--out")) return usage();
        auto provider = crypto::makeDefaultProvider();
        if (!provider->isAvailable()) {
            std::fputs("crypto backend unavailable (build with -DRIVET_ENABLE_CRYPTO=ON)\n", stderr);
            return 1;
        }
        TimestampSetup ts;
        if (const int rc = setUpTimestamp(opt, provider, ts)) return rc;
        DocTimestampRequest dreq;
        dreq.fetch = ts.fetcher();
        auto r = timestampPdfFile(opt["--in"], opt["--out"], dreq, *provider);
        if (!r) {
            std::fprintf(stderr, "time-stamping failed: %s: %s\n", std::string(toString(r.error().code)).c_str(),
                         r.error().message.c_str());
            return 1;
        }
        std::printf("time-stamped %s -> %s token=%zu bytes tsa=%s\n", opt["--in"].c_str(), opt["--out"].c_str(),
                    r->timestampTokenBytes, r->timestampTsaSubject.c_str());
        return 0;
    }
    if (!opt.count("--in") || !opt.count("--out") || !opt.count("--p12")) return usage();

    SignRequest req;
    const std::string profile = opt.count("--profile") ? opt["--profile"] : "pades-bb";
    if (profile == "basic") req.profile = SignatureProfile::Pkcs7Detached;
    else if (profile == "pades-bb") req.profile = SignatureProfile::PadesBaselineB;
    else return usage();

    const std::string digest = opt.count("--digest") ? opt["--digest"] : "sha256";
    if (digest == "sha256") req.digest = crypto::DigestAlgorithm::Sha256;
    else if (digest == "sha384") req.digest = crypto::DigestAlgorithm::Sha384;
    else if (digest == "sha512") req.digest = crypto::DigestAlgorithm::Sha512;
    else return usage();

    if (opt.count("--visible") && invisible) return usage();
    if (opt.count("--visible") && !parseVisible(opt["--visible"], req.placement)) return usage();
    if (opt.count("--field")) {
        if (opt.count("--visible") || invisible) return usage();
        req.placement.kind = SignaturePlacement::Kind::ExistingField;
        req.placement.fieldName = opt["--field"];
    }
    if (opt.count("--certify")) {
        const std::string c = opt["--certify"];
        if (c == "1") req.certification = DocMdpLevel::NoChanges;
        else if (c == "2") req.certification = DocMdpLevel::FormFill;
        else if (c == "3") req.certification = DocMdpLevel::Annotate;
        else return usage();
    }
    if (opt.count("--reason")) req.reason = opt["--reason"];
    if (opt.count("--location")) req.location = opt["--location"];

    auto provider = crypto::makeDefaultProvider();
    if (!provider->isAvailable()) {
        std::fputs("crypto backend unavailable (build with -DRIVET_ENABLE_CRYPTO=ON)\n", stderr);
        return 1;
    }
    std::ifstream in(opt["--p12"], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", opt["--p12"].c_str());
        return 1;
    }
    in.close();
    const std::string p12Text = readWholeFile(opt["--p12"]);
    const crypto::Bytes p12(p12Text.begin(), p12Text.end());
    const auto password = readPassword();
    auto contents = provider->importPkcs12(p12, password);
    if (!contents || contents->identities.empty()) {
        std::fprintf(stderr, "PKCS#12 import failed: %s\n",
                     contents ? "no identity" : contents.error().message.c_str());
        return 1;
    }
    auto& identity = *contents->identities.front();

    if (opt.count("--alg")) {
        using SA = crypto::SignatureAlgorithm;
        const std::string alg = opt["--alg"];
        const auto kt = identity.keyType();
        if (alg == "rsa" && kt == crypto::PublicKeyType::Rsa) {
            req.signature = pick(req.digest, SA::RsaPkcs1Sha256, SA::RsaPkcs1Sha384, SA::RsaPkcs1Sha512);
        } else if (alg == "rsa-pss" && kt == crypto::PublicKeyType::Rsa) {
            req.signature = pick(req.digest, SA::RsaPssSha256, SA::RsaPssSha384, SA::RsaPssSha512);
        } else if (alg == "ecdsa" && kt == crypto::PublicKeyType::Ec) {
            req.signature = pick(req.digest, SA::EcdsaSha256, SA::EcdsaSha384, SA::EcdsaSha512);
        } else {
            std::fputs("--alg does not match the key type of the identity\n", stderr);
            return 2;
        }
    }

    TimestampSetup tsSetup;
    if (opt.count("--timestamp-url") || opt.count("--timestamp-anchor")) {
        if (const int rc = setUpTimestamp(opt, provider, tsSetup)) return rc;
        req.timestamp.fetch = tsSetup.fetcher();
        req.timestamp.allowSignWithoutTimestamp = opt.count("--allow-no-timestamp") != 0;
    }

    auto result = signPdfFile(opt["--in"], opt["--out"], req, *provider, identity);
    if (!result) {
        std::fprintf(stderr, "signing failed: %s: %s\n", std::string(toString(result.error().code)).c_str(),
                     result.error().message.c_str());
        return 1;
    }
    std::printf("signed %s -> %s field=%s cms=%zu level=%s\n", opt["--in"].c_str(), opt["--out"].c_str(),
                result->fieldName.c_str(), result->cmsBytes, std::string(toString(result->level)).c_str());
    if (!result->timestampSkippedReason.empty())
        std::printf("warning: no time-stamp (B-B only): %s\n", result->timestampSkippedReason.c_str());
    return 0;
}
