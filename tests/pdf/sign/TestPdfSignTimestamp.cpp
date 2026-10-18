// SPDX-License-Identifier: MPL-2.0
// PAdES B-T signature time-stamps and PDF document time-stamps (ADR-0038).
// The TSA is the in-process test TSA behind a mock transport; the real network
// is never touched.

#include "RivetTest.h"

#include "SignTestSupport.hpp"
#include "TestTimeStampSupport.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"
#include "net/MockTransport.hpp"

#include <algorithm>
#include <fstream>
#include <random>

using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;
namespace crypto = rivet::crypto;
namespace net = rivet::net;
namespace ctest = rivet::crypto::testing;

namespace {

struct TsEnv {
    std::shared_ptr<crypto::CryptoProvider> provider = crypto::makeDefaultProvider();
    std::shared_ptr<ctest::Tsa> tsa;
    std::shared_ptr<ctest::TsaTransport> transport;
    std::shared_ptr<net::OnlineValidationGate> gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    std::shared_ptr<net::NetworkClient> network;
    std::shared_ptr<crypto::TimeStampClient> client;
    int fetches = 0;

    TsEnv() {
        tsa = ctest::makeTsa(*provider, ctest::tsaSpec());
        transport = std::make_shared<ctest::TsaTransport>(provider, tsa);
        useTransport(transport);
    }
    void useTransport(std::shared_ptr<net::INetworkTransport> t) {
        network = std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, std::move(t), gate);
        client = std::make_shared<crypto::TimeStampClient>(network, provider);
    }
    TimestampFetcher fetcher() {
        return [this](crypto::DigestAlgorithm alg, crypto::ByteView digest,
                      const crypto::CancelToken& cancel) -> crypto::CryptoResult<crypto::TimeStampVerification> {
            ++fetches;
            crypto::TimeStampClientOptions o;
            o.verify.anchors = &tsa->anchors;
            auto r = client->request(ctest::kTsaUrl, alg, digest, o, cancel);
            if (!r) return std::unexpected(r.error());
            return std::move(r->verification);
        };
    }
    VerifyOptions verifyOpts(const Signer& s) const {
        VerifyOptions o;
        o.anchors = &s.anchors;
        o.timestampAnchors = &tsa->anchors;
        return o;
    }
};

std::vector<PdfSignatureInfo> verifyWith(const Bytes& pdf, const TsEnv& env, const Signer& s) {
    auto r = verifyPdfSignatures(pdf, *env.provider, env.verifyOpts(s));
    if (!r) {
        std::printf("    verify error: %s\n", r.error().message.c_str());
        return {};
    }
    return std::move(*r);
}

SignRequest timestampedRequest(TsEnv& env) {
    SignRequest r = basicRequest();
    r.timestamp.fetch = env.fetcher();
    return r;
}

DocTimestampRequest docRequest(TsEnv& env) {
    DocTimestampRequest r;
    r.fetch = env.fetcher();
    return r;
}

std::string failText(const SignError& e) { return std::string(toString(e.code)) + ": " + e.message; }

// The raw /Contents DER of a signature (hex between the ByteRange spans), padding trimmed by the caller.
Bytes contentsOf(const Bytes& pdf, const PdfSignatureInfo& info) {
    Bytes der;
    if (info.byteRange.size() != 4) return der;
    const std::size_t gb = static_cast<std::size_t>(info.byteRange[1]);
    const std::size_t ge = static_cast<std::size_t>(info.byteRange[2]);
    auto nib = [](std::uint8_t c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = gb + 1; i + 2 <= ge - 1; i += 2)
        der.push_back(static_cast<std::uint8_t>(nib(pdf[i]) << 4 | nib(pdf[i + 1])));
    // trim to the outer DER SEQUENCE length
    if (der.size() > 4 && der[0] == 0x30) {
        std::size_t len = 0, hdr = 2;
        if (der[1] < 0x80) len = der[1];
        else {
            const int n = der[1] & 0x7F;
            for (int k = 0; k < n; ++k) len = (len << 8) | der[2 + k];
            hdr = 2 + static_cast<std::size_t>(n);
        }
        der.resize(std::min(der.size(), hdr + len));
    }
    return der;
}

void writeFile(const std::filesystem::path& p, const Bytes& b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

const PdfSignatureInfo* firstOfKind(const std::vector<PdfSignatureInfo>& v, SignatureKind k) {
    for (const auto& i : v)
        if (i.kind == k) return &i;
    return nullptr;
}

} // namespace

RIVET_TEST(signWithTimestampIsBaselineT) {
    TsEnv env;
    Signer s;
    auto out = signPdf(makeFixture(), timestampedRequest(env), *s.provider, *s.identity);
    if (!out) std::printf("    sign failed: %s\n", failText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK(out->level == PadesLevel::BaselineT);
    CHECK(out->timestamped);
    CHECK(out->timestampSkippedReason.empty());
    CHECK(out->timestampGenTime.has_value());
    CHECK(!out->timestampTsaSubject.empty());
    CHECK_EQ(env.fetches, 1);
    CHECK(out->cmsBytes <= out->reservedBytes);
    const auto sigs = verifyWith(out->data, env, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (sigs.empty()) return;
    std::printf("    B-T state: %s\n", std::string(crypto::toString(sigs[0].validation.state)).c_str());
    CHECK(sigs[0].kind == SignatureKind::Signature);
    CHECK(sigs[0].hasTimestampToken);
    CHECK(sigs[0].padesBaselineB);
    CHECK(sigs[0].padesBaselineT);
    CHECK(sigs[0].timestamp.has_value());
    if (sigs[0].timestamp) CHECK(sigs[0].timestamp->verified);
    CHECK(sigs[0].validation.state == crypto::SignatureState::ValidTimestamped);
    // The original bytes are an exact prefix.
    const Bytes base = makeFixture();
    CHECK(std::equal(base.begin(), base.end(), out->data.begin()));

    // Without the TSA anchors the timestamp is untrusted: the signature stays valid but not ValidTimestamped.
    const auto noTsa = verifyAll(out->data, s, true);
    CHECK_EQ(noTsa.size(), std::size_t{1});
    if (!noTsa.empty()) {
        CHECK(noTsa[0].validation.state != crypto::SignatureState::ValidTimestamped);
        CHECK(noTsa[0].validation.crypto == crypto::CryptoCheck::Valid);
    }
}

RIVET_TEST(tsaFailureFailsSigningAndWritesNoFile) {
    TsEnv env;
    Signer s;
    auto mock = std::make_shared<net::MockTransport>();
    net::MockReply bad;
    bad.status = 500;
    bad.body = "boom";
    mock->script(ctest::kTsaUrl, bad);
    env.useTransport(mock);

    auto out = signPdf(makeFixture(), timestampedRequest(env), *s.provider, *s.identity);
    CHECK(!out.has_value());
    if (!out) {
        std::printf("    error: %s\n", failText(out.error()).c_str());
        CHECK(out.error().code == SignErrorCode::TimestampFailed);
        CHECK(!out.error().message.empty());
    }

    const auto dir = scratchDir("ts-fail");
    writeFile(dir / "in.pdf", makeFixture());
    auto f = signPdfFile(dir / "in.pdf", dir / "out.pdf", timestampedRequest(env), *s.provider, *s.identity);
    CHECK(!f.has_value());
    CHECK(!std::filesystem::exists(dir / "out.pdf"));
    for (const auto& e : std::filesystem::directory_iterator(dir)) CHECK(e.path().filename() == "in.pdf");

    auto d = timestampPdfFile(dir / "in.pdf", dir / "out2.pdf", docRequest(env), *s.provider);
    CHECK(!d.has_value());
    CHECK(!std::filesystem::exists(dir / "out2.pdf"));
}

RIVET_TEST(allowSignWithoutTimestampFallsBackToBaselineB) {
    TsEnv env;
    Signer s;
    auto mock = std::make_shared<net::MockTransport>();
    net::MockReply bad;
    bad.status = 503;
    bad.body = "down";
    mock->script(ctest::kTsaUrl, bad);
    env.useTransport(mock);
    SignRequest req = timestampedRequest(env);
    req.timestamp.allowSignWithoutTimestamp = true;
    auto out = signPdf(makeFixture(), req, *s.provider, *s.identity);
    if (!out) std::printf("    sign failed: %s\n", failText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK(out->level == PadesLevel::BaselineB);
    CHECK(!out->timestamped);
    CHECK(!out->timestampSkippedReason.empty());
    std::printf("    skipped: %s\n", out->timestampSkippedReason.c_str());
    const auto sigs = verifyWith(out->data, env, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) {
        CHECK(sigs[0].padesBaselineB);
        CHECK(!sigs[0].padesBaselineT);
        CHECK(!sigs[0].hasTimestampToken);
        CHECK(sigs[0].validation.crypto == crypto::CryptoCheck::Valid);
    }
}

RIVET_TEST(tokenLargerThanReservationGivesTypedErrorThenRetrySucceeds) {
    TsEnv env;
    Signer s;
    // Learn the size of the CMS without a time-stamp.
    auto plain = signPdf(makeFixture(), basicRequest(), *s.provider, *s.identity);
    CHECK(plain.has_value());
    if (!plain) return;

    SignRequest req = timestampedRequest(env);
    req.initialReserveOverride = plain->cmsBytes + 200; // far less than a token
    req.timestamp.retryWithLargerReservation = false;
    auto small = signPdf(makeFixture(), req, *s.provider, *s.identity);
    CHECK(!small.has_value());
    if (!small) {
        std::printf("    error: %s (required %zu)\n", failText(small.error()).c_str(), small.error().requiredBytes);
        CHECK(small.error().code == SignErrorCode::ContentsPlaceholderTooSmall);
        CHECK(small.error().requiredBytes > req.initialReserveOverride);
    }

    req.timestamp.retryWithLargerReservation = true;
    auto retried = signPdf(makeFixture(), req, *s.provider, *s.identity);
    if (!retried) std::printf("    retry failed: %s\n", failText(retried.error()).c_str());
    CHECK(retried.has_value());
    if (!retried) return;
    CHECK(retried->timestamped);
    CHECK(retried->attempts >= 2);
    CHECK(retried->reservedBytes > req.initialReserveOverride);
    const auto sigs = verifyWith(retried->data, env, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) CHECK(sigs[0].validation.state == crypto::SignatureState::ValidTimestamped);

    // The same for a document time-stamp.
    DocTimestampRequest dr = docRequest(env);
    dr.initialReserveOverride = 300;
    dr.retryWithLargerReservation = false;
    auto dsmall = timestampPdf(makeFixture(), dr, *s.provider);
    CHECK(!dsmall.has_value());
    if (!dsmall) CHECK(dsmall.error().code == SignErrorCode::ContentsPlaceholderTooSmall);
    dr.retryWithLargerReservation = true;
    auto dok = timestampPdf(makeFixture(), dr, *s.provider);
    if (!dok) std::printf("    doc retry failed: %s\n", failText(dok.error()).c_str());
    CHECK(dok.has_value());
}

RIVET_TEST(docTimestampAfterSignatureKeepsEarlierSignatureValid) {
    TsEnv env;
    Signer s;
    auto signedPdf = signPdf(makeFixture(), basicRequest(), *s.provider, *s.identity);
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    auto stamped = timestampPdf(signedPdf->data, docRequest(env), *s.provider);
    if (!stamped) std::printf("    timestamp failed: %s\n", failText(stamped.error()).c_str());
    CHECK(stamped.has_value());
    if (!stamped) return;
    CHECK(std::equal(signedPdf->data.begin(), signedPdf->data.end(), stamped->data.begin()));
    CHECK(stamped->timestamped);

    const auto sigs = verifyWith(stamped->data, env, s);
    CHECK_EQ(sigs.size(), std::size_t{2});
    if (sigs.size() != 2) return;
    const auto* sig = firstOfKind(sigs, SignatureKind::Signature);
    const auto* ts = firstOfKind(sigs, SignatureKind::DocumentTimestamp);
    CHECK(sig != nullptr);
    CHECK(ts != nullptr);
    if (!sig || !ts) return;
    std::printf("    signature: %s, modification=%d\n", std::string(crypto::toString(sig->validation.state)).c_str(),
                static_cast<int>(sig->validation.modification));
    CHECK(sig->validation.crypto == crypto::CryptoCheck::Valid);
    CHECK(sig->validation.integrity == crypto::IntegrityStatus::Intact);
    CHECK(sig->validation.modification == crypto::ModificationStatus::None);
    CHECK(sig->validation.state == crypto::SignatureState::ValidTrusted);
    CHECK(ts->type == "DocTimeStamp");
    CHECK(ts->subFilter == "ETSI.RFC3161");
    CHECK(ts->coversWholeFile);
    CHECK(ts->byteRangeValid);
    CHECK(ts->timestamp.has_value());
    if (ts->timestamp) {
        CHECK(ts->timestamp->verified);
        CHECK(ts->timestamp->genTime.has_value());
        CHECK(!ts->timestamp->tsaSubject.empty());
    }
    CHECK(ts->validation.crypto == crypto::CryptoCheck::Valid);
    CHECK(ts->validation.integrity == crypto::IntegrityStatus::Intact);
    std::printf("    DocTimeStamp state: %s\n%s\n", std::string(crypto::toString(ts->validation.state)).c_str(),
                describeSignature(*ts).c_str());

    // Without TSA anchors the time-stamp is reported untrusted, not crashed or hidden.
    auto noAnchors = verifyPdfSignatures(stamped->data, *s.provider, VerifyOptions{});
    CHECK(noAnchors.has_value());

    // A second DocTimeStamp chained after the first is accepted as well.
    auto again = timestampPdf(stamped->data, docRequest(env), *s.provider);
    CHECK(again.has_value());
    if (again) {
        const auto three = verifyWith(again->data, env, s);
        CHECK_EQ(three.size(), std::size_t{3});
        const auto* first = firstOfKind(three, SignatureKind::Signature);
        if (first) CHECK(first->validation.integrity == crypto::IntegrityStatus::Intact);
        for (const auto& i : three) CHECK(i.validation.crypto == crypto::CryptoCheck::Valid);
    }

    // Xref-stream fixtures work the same way.
    auto xs = signPdf(makeFixture({.xrefStream = true}), basicRequest(), *s.provider, *s.identity);
    CHECK(xs.has_value());
    if (xs) {
        auto xt = timestampPdf(xs->data, docRequest(env), *s.provider);
        if (!xt) std::printf("    xref-stream timestamp failed: %s\n", failText(xt.error()).c_str());
        CHECK(xt.has_value());
        if (xt) CHECK_EQ(verifyWith(xt->data, env, s).size(), std::size_t{2});
    }
}

RIVET_TEST(docTimestampOnUnsignedAndRefusals) {
    TsEnv env;
    Signer s;
    auto only = timestampPdf(makeFixture(), docRequest(env), *s.provider);
    CHECK(only.has_value());
    if (only) {
        const auto sigs = verifyWith(only->data, env, s);
        CHECK_EQ(sigs.size(), std::size_t{1});
        if (!sigs.empty()) {
            CHECK(sigs[0].kind == SignatureKind::DocumentTimestamp);
            CHECK(sigs[0].validation.crypto == crypto::CryptoCheck::Valid);
        }
    }
    auto enc = timestampPdf(makeFixture({.encrypted = true}), docRequest(env), *s.provider);
    CHECK(!enc.has_value());
    CHECK_EQ(env.fetches, 1); // refused before any network traffic

    // Never before a certification signature: a DocTimeStamp then a certification signature is the
    // wrong order, but a certification signature then a DocTimeStamp is fine; and the reverse is refused.
    SignRequest cert = basicRequest();
    cert.certification = DocMdpLevel::NoChanges;
    auto certified = signPdf(makeFixture(), cert, *s.provider, *s.identity);
    CHECK(certified.has_value());
    if (certified) {
        auto ts = timestampPdf(certified->data, docRequest(env), *s.provider);
        CHECK(ts.has_value());
        if (ts) {
            const auto sigs = verifyWith(ts->data, env, s);
            const auto* sig = firstOfKind(sigs, SignatureKind::Signature);
            CHECK(sig != nullptr);
            if (sig) CHECK(sig->validation.modification != crypto::ModificationStatus::Disallowed);
        }
    }
    if (only) {
        auto late = signPdf(only->data, cert, *s.provider, *s.identity);
        CHECK(!late.has_value());
        if (!late) std::printf("    certification after DocTimeStamp: %s\n", failText(late.error()).c_str());
    }
}

RIVET_TEST(docTimestampTamperIsInvalid) {
    TsEnv env;
    Signer s;
    auto signedPdf = signPdf(makeFixture(), basicRequest(), *s.provider, *s.identity);
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    auto stamped = timestampPdf(signedPdf->data, docRequest(env), *s.provider);
    CHECK(stamped.has_value());
    if (!stamped) return;
    const auto before = verifyWith(stamped->data, env, s);
    const auto* ts0 = firstOfKind(before, SignatureKind::DocumentTimestamp);
    CHECK(ts0 != nullptr);
    if (!ts0) return;
    // Flip a byte inside the first covered span (the PDF header comment area is covered).
    Bytes bad = stamped->data;
    bad[20] ^= 1;
    const auto sigs = verifyWith(bad, env, s);
    const auto* ts = firstOfKind(sigs, SignatureKind::DocumentTimestamp);
    CHECK(ts != nullptr);
    if (!ts) return;
    std::printf("    tampered DocTimeStamp: %s\n", std::string(crypto::toString(ts->validation.state)).c_str());
    CHECK(ts->validation.integrity != crypto::IntegrityStatus::Intact);
    CHECK(ts->validation.state == crypto::SignatureState::DocumentModified ||
          ts->validation.state == crypto::SignatureState::InvalidSignature);
    CHECK(ts->timestamp.has_value() && !ts->timestamp->verified);
}

RIVET_TEST(wrongNonceOrImprintTokenIsRejected) {
    Signer s;
    for (int mode = 0; mode < 2; ++mode) {
        TsEnv env;
        if (mode == 0) env.transport->cfg.nonce = ctest::bytesOf("\x01\x02\x03\x04\x05\x06\x07\x08");
        else env.transport->cfg.imprint = std::make_pair(std::string("2.16.840.1.101.3.4.2.1"), Bytes(32, 0x42));
        auto out = signPdf(makeFixture(), timestampedRequest(env), *s.provider, *s.identity);
        CHECK(!out.has_value());
        if (!out) {
            std::printf("    mode %d: %s\n", mode, failText(out.error()).c_str());
            CHECK(out.error().code == SignErrorCode::TimestampFailed);
        }
        auto d = timestampPdf(makeFixture(), docRequest(env), *s.provider);
        CHECK(!d.has_value());
        // And with the fallback the signature is B-B only, with the reason reported.
        SignRequest req = timestampedRequest(env);
        req.timestamp.allowSignWithoutTimestamp = true;
        auto bb = signPdf(makeFixture(), req, *s.provider, *s.identity);
        CHECK(bb.has_value());
        if (bb) {
            CHECK(bb->level == PadesLevel::BaselineB);
            CHECK(!bb->timestampSkippedReason.empty());
        }
    }
}

RIVET_TEST(garbageContentsNeverCrashes) {
    TsEnv env;
    Signer s;
    auto signedPdf = signPdf(makeFixture(), timestampedRequest(env), *s.provider, *s.identity);
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    auto stamped = timestampPdf(signedPdf->data, docRequest(env), *s.provider);
    CHECK(stamped.has_value());
    if (!stamped) return;
    const auto sigs = verifyWith(stamped->data, env, s);
    CHECK_EQ(sigs.size(), std::size_t{2});
    if (sigs.size() != 2) return;
    std::mt19937 rng(0xC0FFEE);
    int iterations = 0;
    for (const auto& info : sigs) {
        if (info.byteRange.size() != 4) continue;
        const std::size_t gb = static_cast<std::size_t>(info.byteRange[1]) + 1;
        const std::size_t ge = static_cast<std::size_t>(info.byteRange[2]) - 1;
        const Bytes der = contentsOf(stamped->data, info);
        for (int i = 0; i < 60; ++i, ++iterations) {
            Bytes m = stamped->data;
            const int kind = i % 4;
            const std::size_t used = std::min(ge - gb, der.size() * 2);
            if (kind == 0) { // flip random hex nibbles inside the DER
                for (int k = 0; k < 4; ++k) m[gb + rng() % used] = "0123456789abcdef"[rng() % 16];
            } else if (kind == 1) { // truncate the DER: zero the rest
                const std::size_t cut = rng() % used;
                std::fill(m.begin() + static_cast<std::ptrdiff_t>(gb + cut), m.begin() + static_cast<std::ptrdiff_t>(ge), '0');
            } else if (kind == 2) { // random hex garbage over the whole hole
                for (std::size_t k = gb; k < ge; ++k) m[k] = "0123456789abcdef"[rng() % 16];
            } else { // non-hex bytes
                for (int k = 0; k < 8; ++k) m[gb + rng() % (ge - gb)] = static_cast<std::uint8_t>(rng());
            }
            auto r = verifyPdfSignatures(m, *env.provider, env.verifyOpts(s));
            if (r)
                for (const auto& x : *r) CHECK(x.validation.state != crypto::SignatureState::ValidTimestamped || m == stamped->data || x.kind == SignatureKind::Signature);
        }
    }
    std::printf("    %d mutated /Contents verified without crashing\n", iterations);
    CHECK(iterations >= 100);
}

RIVET_TEST(interopPdfsigAndOpenssl) {
    TsEnv env;
    Signer s;
    auto bt = signPdf(makeFixture(), timestampedRequest(env), *s.provider, *s.identity);
    CHECK(bt.has_value());
    if (!bt) return;
    auto doc = timestampPdf(bt->data, docRequest(env), *s.provider);
    CHECK(doc.has_value());
    if (!doc) return;
    const auto dir = scratchDir("ts-interop");
    writeFile(dir / "bt.pdf", bt->data);
    writeFile(dir / "doc.pdf", doc->data);
    if (const char* keep = std::getenv("RIVET_TS_INTEROP_KEEP")) { // debugging aid: leave the artefacts behind
        std::filesystem::create_directories(keep);
        writeFile(std::filesystem::path(keep) / "bt.pdf", bt->data);
        writeFile(std::filesystem::path(keep) / "doc.pdf", doc->data);
    }
    const auto sigs = verifyWith(doc->data, env, s);
    const auto* ts = firstOfKind(sigs, SignatureKind::DocumentTimestamp);
    if (ts) {
        writeFile(dir / "token.der", contentsOf(doc->data, *ts));
        writeFile(dir / "tsa.pem", {});
    }

    if (toolAvailable("pdfsig")) {
        for (const char* f : {"bt.pdf", "doc.pdf"}) {
            std::string log;
            runCommand("pdfsig '" + (dir / f).string() + "'", log);
            std::printf("    pdfsig %s:\n", f);
            std::size_t pos = 0;
            int lines = 0;
            while (pos < log.size() && lines < 40) {
                const auto e = log.find('\n', pos);
                std::printf("      | %s\n", log.substr(pos, e == std::string::npos ? std::string::npos : e - pos).c_str());
                ++lines;
                if (e == std::string::npos) break;
                pos = e + 1;
            }
        }
    } else {
        std::printf("    SKIP pdfsig not installed\n");
    }

    if (toolAvailable("openssl") && ts) {
        // The TSA certificate as PEM for -CAfile.
        {
            std::ofstream pem(dir / "tsa.pem");
            const auto der = ctest::certDer(env.tsa->key);
            static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string enc;
            for (std::size_t i = 0; i < der.size(); i += 3) {
                const std::uint32_t v = (der[i] << 16) | (i + 1 < der.size() ? der[i + 1] << 8 : 0) | (i + 2 < der.size() ? der[i + 2] : 0);
                enc += b64[(v >> 18) & 63];
                enc += b64[(v >> 12) & 63];
                enc += i + 1 < der.size() ? b64[(v >> 6) & 63] : '=';
                enc += i + 2 < der.size() ? b64[v & 63] : '=';
            }
            pem << "-----BEGIN CERTIFICATE-----\n";
            for (std::size_t i = 0; i < enc.size(); i += 64) pem << enc.substr(i, 64) << "\n";
            pem << "-----END CERTIFICATE-----\n";
        }
        std::string log;
        int rc = runCommand("openssl cms -cmsout -inform DER -in '" + (dir / "token.der").string() + "' -noout -print | head -5", log);
        std::printf("    openssl cms parse rc=%d: %s\n", rc, log.substr(0, 120).c_str());
        CHECK(rc == 0);
        rc = runCommand("openssl ts -reply -token_in -in '" + (dir / "token.der").string() + "' -text", log);
        std::printf("    openssl ts -reply -token_in rc=%d, first lines: %s\n", rc, log.substr(0, 160).c_str());
        CHECK(rc == 0);
    } else {
        std::printf("    SKIP openssl cms/ts (tool absent)\n");
    }
}
