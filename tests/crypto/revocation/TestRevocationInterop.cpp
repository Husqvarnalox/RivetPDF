// SPDX-License-Identifier: MPL-2.0
// Interoperability with the OpenSSL command line: Rivet's OCSP request is read
// by `openssl ocsp`, responses produced by `openssl ocsp -respout` and a CRL
// produced by `openssl ca -gencrl` are verified by Rivet, and a CRL fetched by
// Rivet is checked by `openssl crl` / `openssl verify -crl_check`. Skipped
// (reported, not failed) when no OpenSSL 3 command line tool is found.
#include "RivetTest.h"

#include "TestRevocationSupport.hpp"
#include "crypto/revocation/RevocationChecker.hpp"

#include <ctime>
#include <fstream>
#include <sstream>

using namespace rivet;
using namespace rivet::crypto;
using namespace rivet::crypto::testing;
using namespace rivet::crypto::testing::rev;
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Cli {
    std::string exe;
    fs::path dir;
    int run(const std::string& args, const std::string& outName, std::string& output) const {
        const fs::path out = dir / outName;
        const std::string cmd = "\"" + exe + "\" " + args + " > \"" + out.string() + "\" 2>&1";
        const int rc = std::system(cmd.c_str());
        output = slurp(out);
        std::fprintf(stderr, "    INTEROP run: openssl %s  -> exit %d\n", args.c_str(), rc);
        return rc;
    }
    std::string path(const char* name) const { return (dir / name).string(); }
};

std::optional<Cli> findCli() {
#if defined(_WIN32)
    return std::nullopt;
#else
    const char* env = std::getenv("RIVET_OPENSSL_CLI");
    std::vector<std::string> candidates;
    if (env) candidates.emplace_back(env);
    candidates.emplace_back("/opt/homebrew/opt/openssl@3/bin/openssl");
    candidates.emplace_back("/usr/local/opt/openssl@3/bin/openssl");
    candidates.emplace_back("openssl");
    for (const std::string& c : candidates) {
        const fs::path probe = tempDir() / "openssl-version.txt";
        const std::string cmd = "\"" + c + "\" version > \"" + probe.string() + "\" 2>&1";
        if (std::system(cmd.c_str()) != 0) continue;
        if (slurp(probe).find("OpenSSL 3") == std::string::npos) continue;
        Cli cli;
        cli.exe = c;
        cli.dir = tempDir() / "revocation-interop";
        std::error_code ec;
        fs::remove_all(cli.dir, ec);
        fs::create_directories(cli.dir);
        return cli;
    }
    return std::nullopt;
#endif
}

void writePemCert(const fs::path& p, const TestKey& k) {
    BioPtr bio(BIO_new_file(p.string().c_str(), "w"));
    PEM_write_bio_X509(bio.get(), k.cert.get());
}
void writePemKey(const fs::path& p, const TestKey& k) {
    BioPtr bio(BIO_new_file(p.string().c_str(), "w"));
    PEM_write_bio_PrivateKey(bio.get(), k.pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
}

std::string stamp(Clock::time_point t) {
    const std::time_t tt = Clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%y%m%d%H%M%SZ", &tm);
    return buf;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

} // namespace

RIVET_TEST(interopOpenSslCommandLine) {
    auto cli = findCli();
    if (!cli) {
        std::fprintf(stderr, "    SKIP: no OpenSSL 3 command line tool found\n");
        return;
    }
    std::fprintf(stderr, "    INTEROP using %s\n", cli->exe.c_str());
    auto pki = std::make_shared<Pki>();
    writePemCert(cli->dir / "ca.pem", pki->ca);
    writePemKey(cli->dir / "ca.key", pki->ca);
    writePemCert(cli->dir / "leaf.pem", pki->leaf);
    writePemCert(cli->dir / "leaf2.pem", pki->leaf2);
    writePemCert(cli->dir / "resp.pem", pki->responder);
    writePemKey(cli->dir / "resp.key", pki->responder);
    std::string out;

    // 1. Rivet's request, read by `openssl ocsp -reqin -text`.
    auto req = buildOcspRequest(*pki->provider, pki->leafInfo, pki->caInfo);
    CHECK(req.has_value());
    writeFile(cli->dir / "req.der", req->der);
    const int rc1 = cli->run("ocsp -reqin \"" + cli->path("req.der") + "\" -text", "req.txt", out);
    if (rc1 != 0 || !contains(out, "Hash Algorithm: sha256") || !contains(out, "Serial Number: " + serialHex(pki->leaf)) || !contains(out, "Nonce"))
        std::fprintf(stderr, "    request dump:\n%s\n", out.c_str());
    CHECK_EQ(rc1, 0);
    CHECK(contains(out, "Hash Algorithm: sha256"));
    CHECK(contains(out, "Serial Number: " + serialHex(pki->leaf)));
    CHECK(contains(out, "Nonce"));

    // 2. Responses produced by `openssl ocsp`, verified by Rivet.
    const auto now = Clock::now();
    {
        std::ofstream idx(cli->dir / "index.txt");
        idx << "R\t" << stamp(now + std::chrono::hours(24 * 365)) << "\t" << stamp(now - std::chrono::hours(24)) << ",keyCompromise\t"
            << serialHex(pki->leaf) << "\tunknown\t/CN=Revocation Test Leaf\n";
        idx << "V\t" << stamp(now + std::chrono::hours(24 * 365)) << "\t\t" << serialHex(pki->leaf2) << "\tunknown\t/CN=Revocation Test Leaf 2\n";
    }
    EvidenceClock clock;
    struct Case {
        const char* label;
        const CertificateInfo* cert;
        const char* signerPem;
        const char* signerKey;
        EvidenceCertStatus want;
    };
    for (const Case& k : {Case{"leaf, delegated responder", &pki->leafInfo, "resp.pem", "resp.key", EvidenceCertStatus::Revoked},
                          Case{"leaf2, delegated responder", &pki->leaf2Info, "resp.pem", "resp.key", EvidenceCertStatus::Good},
                          Case{"leaf, CA signs", &pki->leafInfo, "ca.pem", "ca.key", EvidenceCertStatus::Revoked},
                          Case{"leaf2, CA signs", &pki->leaf2Info, "ca.pem", "ca.key", EvidenceCertStatus::Good}}) {
        auto rq = buildOcspRequest(*pki->provider, *k.cert, pki->caInfo);
        CHECK(rq.has_value());
        writeFile(cli->dir / "rq.der", rq->der);
        const int rc = cli->run("ocsp -index \"" + cli->path("index.txt") + "\" -CA \"" + cli->path("ca.pem") + "\" -rsigner \"" +
                                    cli->path(k.signerPem) + "\" -rkey \"" + cli->path(k.signerKey) + "\" -reqin \"" +
                                    cli->path("rq.der") + "\" -respout \"" + cli->path("rs.der") + "\" -ndays 7",
                                "rs.txt", out);
        if (rc != 0) std::fprintf(stderr, "    CLI output: %s\n", out.c_str());
        CHECK_EQ(rc, 0);
        const Bytes resp = readFile(cli->dir / "rs.der");
        OcspEvidence e = verifyOcspResponse(resp, *pki->provider, *k.cert, pki->caInfo, rq->nonce, clock);
        std::fprintf(stderr, "    INTEROP [%s]: verified=%d status=%d nonceChecked=%d failure=%s\n", k.label, e.verified ? 1 : 0,
                     static_cast<int>(e.status), e.nonceChecked ? 1 : 0, std::string(toString(e.failure)).c_str());
        for (const std::string& r : e.reasons) std::fprintf(stderr, "      reason: %s\n", r.c_str());
        CHECK(e.verified);
        CHECK(e.status == k.want);
        CHECK(e.nonceChecked);
        if (k.want == EvidenceCertStatus::Revoked) CHECK(e.revocationReason.has_value() && *e.revocationReason == 1);
    }
    // A serial the CLI responder does not know: signed "unknown", never good.
    {
        std::ofstream idx(cli->dir / "index-empty.txt");
    }
    {
        auto rq = buildOcspRequest(*pki->provider, pki->leaf2Info, pki->caInfo);
        writeFile(cli->dir / "rq.der", rq->der);
        CHECK_EQ(cli->run("ocsp -index \"" + cli->path("index-empty.txt") + "\" -CA \"" + cli->path("ca.pem") + "\" -rsigner \"" +
                              cli->path("ca.pem") + "\" -rkey \"" + cli->path("ca.key") + "\" -reqin \"" + cli->path("rq.der") +
                              "\" -respout \"" + cli->path("rs.der") + "\" -ndays 7",
                          "rs.txt", out),
                 0);
        OcspEvidence e = verifyOcspResponse(readFile(cli->dir / "rs.der"), *pki->provider, pki->leaf2Info, pki->caInfo, rq->nonce, clock);
        std::fprintf(stderr, "    INTEROP [unknown serial]: verified=%d status=%d\n", e.verified ? 1 : 0, static_cast<int>(e.status));
        CHECK(e.status != EvidenceCertStatus::Good);
    }

    // 3. A CRL produced by `openssl ca -gencrl`, verified by Rivet.
    {
        std::ofstream cfg(cli->dir / "ca.cnf");
        cfg << "[ca]\ndefault_ca = CA_default\n[CA_default]\ndatabase = " << cli->path("index.txt") << "\ncrlnumber = " << cli->path("crlnumber")
            << "\ndefault_md = sha256\ndefault_crl_days = 7\ncertificate = " << cli->path("ca.pem") << "\nprivate_key = " << cli->path("ca.key") << "\n";
        std::ofstream num(cli->dir / "crlnumber");
        num << "0A\n";
    }
    {
        const int rc = cli->run("ca -config \"" + cli->path("ca.cnf") + "\" -gencrl -out \"" + cli->path("cli-crl.pem") + "\"", "gencrl.txt", out);
        if (rc != 0) std::fprintf(stderr, "    CLI output: %s\n", out.c_str());
        CHECK_EQ(rc, 0);
        const Bytes pem = readFile(cli->dir / "cli-crl.pem");
        auto ext = parseRevocationExtensions(pki->leafInfo.der);
        CrlEvidence revoked = verifyCrl(pem, *pki->provider, pki->leafInfo, pki->caInfo, *ext, clock);
        CrlEvidence good = verifyCrl(pem, *pki->provider, pki->leaf2Info, pki->caInfo, *ext, clock);
        std::fprintf(stderr, "    INTEROP [CLI CRL]: leaf verified=%d status=%d reason=%d; leaf2 verified=%d status=%d; crlNumber bytes=%zu\n",
                     revoked.verified ? 1 : 0, static_cast<int>(revoked.status), revoked.revocationReason.value_or(-1), good.verified ? 1 : 0,
                     static_cast<int>(good.status), revoked.crlNumber.size());
        for (const std::string& r : revoked.reasons) std::fprintf(stderr, "      reason: %s\n", r.c_str());
        CHECK(revoked.verified && revoked.status == EvidenceCertStatus::Revoked);
        CHECK(revoked.revocationReason.has_value() && *revoked.revocationReason == 1);
        CHECK(good.verified && good.status == EvidenceCertStatus::Good);
    }

    // 4. A CRL fetched by Rivet, checked by `openssl crl` and `openssl verify -crl_check`.
    {
        Env env(pki);
        CrlCfg c;
        c.revoked = {&pki->leaf};
        env.transport->setCrl(makeCrl(*pki, c));
        CrlClient client(env.network, pki->provider);
        auto ext = parseRevocationExtensions(pki->leafInfo.der);
        auto fetched = client.fetch(kCrlUrl, pki->leafInfo, pki->caInfo, *ext, {}, CancelToken{});
        CHECK(fetched.has_value() && fetched->verified);
        writeFile(cli->dir / "fetched.crl", fetched->rawCrlDer);
        const int rcText = cli->run("crl -inform DER -in \"" + cli->path("fetched.crl") + "\" -noout -text", "crl.txt", out);
        CHECK_EQ(rcText, 0);
        CHECK(contains(out, serialHex(pki->leaf)));
        CHECK(contains(out, "Key Compromise"));
        const int rcVerify = cli->run("crl -inform DER -in \"" + cli->path("fetched.crl") + "\" -CAfile \"" + cli->path("ca.pem") + "\" -noout -verify", "crlv.txt", out);
        CHECK_EQ(rcVerify, 0);
        CHECK_EQ(cli->run("crl -inform DER -in \"" + cli->path("fetched.crl") + "\" -outform PEM -out \"" + cli->path("fetched.pem") + "\"", "crlc.txt", out), 0);
        const int rcRevoked = cli->run("verify -crl_check -CAfile \"" + cli->path("ca.pem") + "\" -CRLfile \"" + cli->path("fetched.pem") + "\" \"" +
                                           cli->path("leaf.pem") + "\"",
                                       "v1.txt", out);
        std::fprintf(stderr, "    INTEROP verify -crl_check (revoked leaf): %s\n", out.c_str());
        CHECK(rcRevoked != 0);
        CHECK(contains(out, "revoked"));
        const int rcGood = cli->run("verify -crl_check -CAfile \"" + cli->path("ca.pem") + "\" -CRLfile \"" + cli->path("fetched.pem") + "\" \"" +
                                        cli->path("leaf2.pem") + "\"",
                                    "v2.txt", out);
        std::fprintf(stderr, "    INTEROP verify -crl_check (good leaf2): %s\n", out.c_str());
        CHECK_EQ(rcGood, 0);
    }

    // 5. End to end: the checker agrees with the CLI's CRL for both certificates.
    {
        auto store = std::make_shared<VectorEvidenceStore>();
        store->addCrl(readFile(cli->dir / "cli-crl.pem"));
        Env env(pki);
        RevocationChecker checker(pki->provider, env.network);
        RevocationPolicy p;
        p.mode = RevocationMode::CrlOnly;
        p.allowNetwork = false;
        p.store = store;
        CHECK(checker.check(pki->chain(), Clock::now(), p, CancelToken{}).status == RevocationStatus::Revoked);
        CHECK(checker.check(pki->chain2(), Clock::now(), p, CancelToken{}).status == RevocationStatus::Good);
        CHECK_EQ(env.transport->calls.load(), 0);
    }
}
