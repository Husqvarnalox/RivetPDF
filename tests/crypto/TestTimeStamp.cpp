// SPDX-License-Identifier: MPL-2.0
// RFC 3161 client, token verification and the CMS signature time-stamp.
//
// The test TSA is built here: a throwaway key + self-signed (or CA-issued) TSA
// certificate, and a transport that parses the incoming TimeStampReq with
// OpenSSL's own parser and answers with a TimeStampResp assembled from
// Rivet's DER writer and signed with the test key (so every field can be
// bent for the negative tests). One happy-path response is additionally
// produced by OpenSSL's TS_RESP_create_response, and the `openssl ts` CLI is
// used for interop when it is installed.
#include "RivetTest.h"

#include "TestSupport.hpp"
#include "TestTimeStampSupport.hpp"
#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"
#include "crypto/timestamp/TimeStamp.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"
#include "net/MockTransport.hpp"

#include <openssl/bn.h>
#include <openssl/cms.h>
#include <openssl/pem.h>
#include <openssl/ts.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

using namespace rivet;
using namespace rivet::crypto;
using namespace rivet::crypto::testing;
namespace dr = rivet::crypto::der;
using Clock = std::chrono::system_clock;

namespace {

struct Env {
    std::shared_ptr<CryptoProvider> provider = makeDefaultProvider();
    std::shared_ptr<Tsa> tsa;
    std::shared_ptr<TsaTransport> transport;
    std::shared_ptr<net::OnlineValidationGate> gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    net::NetworkPolicy policy;
    std::shared_ptr<net::NetworkClient> network;
    std::shared_ptr<TimeStampClient> client;

    explicit Env(std::shared_ptr<Tsa> t = nullptr) {
        tsa = t ? std::move(t) : makeTsa(*provider, tsaSpec());
        transport = std::make_shared<TsaTransport>(provider, tsa);
        rebuild();
    }
    void rebuild() {
        network = std::make_shared<net::NetworkClient>(policy, transport, gate);
        client = std::make_shared<TimeStampClient>(network, provider);
    }
    TimeStampClientOptions options() const {
        TimeStampClientOptions o;
        o.verify.anchors = &tsa->anchors;
        return o;
    }
    Bytes documentDigest() const { return *provider->digest(DigestAlgorithm::Sha256, bytesOf("the document to be time-stamped")); }
    CryptoResult<TimeStampResult> stamp(DigestAlgorithm alg = DigestAlgorithm::Sha256) {
        const Bytes d = alg == DigestAlgorithm::Sha256 ? documentDigest() : *provider->digest(alg, bytesOf("the document"));
        return client->request(kTsaUrl, alg, d, options(), CancelToken{});
    }
};

// Prints what went wrong (the CHECK that follows throws) and reports whether
// the exchange ended in `want`.
bool expectFailure(const CryptoResult<TimeStampResult>& r, TimestampFailure want, const char* what) {
    if (!r) {
        std::fprintf(stderr, "    [%s] transport-level error %s: %s\n", what, std::string(toString(r.error().code)).c_str(),
                     r.error().message.c_str());
        return false;
    }
    if (r->verification.failure != want) {
        std::fprintf(stderr, "    [%s] failure=%s, wanted %s\n", what, std::string(toString(r->verification.failure)).c_str(),
                     std::string(toString(want)).c_str());
        for (const std::string& n : r->verification.validation.notes) std::fprintf(stderr, "      note: %s\n", n.c_str());
        return false;
    }
    return true;
}

bool expectOk(const CryptoResult<TimeStampResult>& r, const char* what) { return expectFailure(r, TimestampFailure::None, what); }

bool expectError(const CryptoResult<TimeStampResult>& r, CryptoErrorCode want, const char* what, std::string_view messagePart = {}) {
    if (r) {
        std::fprintf(stderr, "    [%s] unexpectedly succeeded (failure=%s)\n", what, std::string(toString(r->verification.failure)).c_str());
        return false;
    }
    if (r.error().code != want || r.error().message.find(messagePart) == std::string::npos) {
        std::fprintf(stderr, "    [%s] error %s: %s (wanted %s containing '%s')\n", what,
                     std::string(toString(r.error().code)).c_str(), r.error().message.c_str(),
                     std::string(toString(want)).c_str(), std::string(messagePart).c_str());
        return false;
    }
    return true;
}

bool timesClose(Clock::time_point a, Clock::time_point b, std::chrono::seconds tol) {
    return a > b ? a - b <= tol : b - a <= tol;
}

// ---- OpenSSL CLI ---------------------------------------------------------------------

std::string opensslCli() {
#if defined(_WIN32)
    return {};
#else
    static const std::string path = [] {
        for (const char* c : {"/opt/homebrew/opt/openssl@3/bin/openssl", "openssl"}) {
            const std::string probe = std::string("\"") + c + "\" ts -help > /dev/null 2>&1";
            if (std::system(probe.c_str()) == 0) return std::string(c);
        }
        return std::string();
    }();
    return path;
#endif
}

struct CliRun {
    int status = -1;
    std::string output;
};

CliRun runCli(const std::string& args, const std::filesystem::path& scratch) {
    const std::filesystem::path out = scratch / "cli-out.txt";
    const std::string cmd = "\"" + opensslCli() + "\" " + args + " > \"" + out.string() + "\" 2>&1";
    CliRun r;
    r.status = std::system(cmd.c_str());
    const Bytes text = readFile(out);
    r.output.assign(text.begin(), text.end());
    return r;
}

std::string hexString(ByteView v) {
    std::string out;
    char b[3];
    for (std::uint8_t x : v) {
        std::snprintf(b, sizeof b, "%02x", x);
        out += b;
    }
    return out;
}

void writePem(const std::filesystem::path& p, const Bytes& certDerBytes) {
    const unsigned char* d = certDerBytes.data();
    X509Ptr x(d2i_X509(nullptr, &d, static_cast<long>(certDerBytes.size())));
    BioPtr bio(BIO_new_file(p.string().c_str(), "w"));
    PEM_write_bio_X509(bio.get(), x.get());
}

// Independent producer: OpenSSL's own TS responder.
ASN1_INTEGER* serialCb(TS_RESP_CTX*, void*) {
    ASN1_INTEGER* s = ASN1_INTEGER_new();
    ASN1_INTEGER_set(s, 4242);
    return s;
}

Bytes opensslTsResponse(const Tsa& tsa, ByteView reqDer, bool sha256Ess) {
    std::unique_ptr<TS_RESP_CTX, decltype(&TS_RESP_CTX_free)> ctx(TS_RESP_CTX_new(), &TS_RESP_CTX_free);
    TS_RESP_CTX_set_signer_cert(ctx.get(), tsa.key.cert.get());
    TS_RESP_CTX_set_signer_key(ctx.get(), tsa.key.pkey.get());
    TS_RESP_CTX_set_signer_digest(ctx.get(), EVP_sha256());
    if (sha256Ess) TS_RESP_CTX_set_ess_cert_id_digest(ctx.get(), EVP_sha256());
    ASN1_OBJECT* pol = OBJ_txt2obj("1.3.6.1.4.1.99999.7", 1);
    TS_RESP_CTX_set_def_policy(ctx.get(), pol);
    ASN1_OBJECT_free(pol);
    TS_RESP_CTX_add_md(ctx.get(), EVP_sha256());
    TS_RESP_CTX_add_md(ctx.get(), EVP_sha512());
    TS_RESP_CTX_set_serial_cb(ctx.get(), serialCb, nullptr);
    TS_RESP_CTX_set_accuracy(ctx.get(), 1, 0, 0);
    TS_RESP_CTX_set_clock_precision_digits(ctx.get(), 0);
    BioPtr in(BIO_new_mem_buf(reqDer.data(), static_cast<int>(reqDer.size())));
    std::unique_ptr<TS_RESP, decltype(&TS_RESP_free)> resp(TS_RESP_create_response(ctx.get(), in.get()), &TS_RESP_free);
    if (!resp) return {};
    unsigned char* buf = nullptr;
    const int n = i2d_TS_RESP(resp.get(), &buf);
    if (n <= 0) return {};
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    bool drain(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
        std::size_t ran = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ran < count) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return false;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
            ++ran;
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

} // namespace

// ---- request ------------------------------------------------------------------------

RIVET_TEST(timestampRequestEncoding) {
    auto provider = makeDefaultProvider();
    const Bytes digest = *provider->digest(DigestAlgorithm::Sha256, bytesOf("x"));
    TimeStampRequestOptions o;
    o.policyOid = "1.2.3.4.5";
    auto a = buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, digest, o);
    auto b = buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, digest, o);
    if (!a || !b) std::fprintf(stderr, "    build failed: %s\n", a ? b.error().message.c_str() : a.error().message.c_str());
    CHECK(a.has_value() && b.has_value());
    CHECK(a->nonce.has_value() && b->nonce.has_value());
    CHECK(*a->nonce != *b->nonce); // random per request
    CHECK(a->der != b->der);

    const ParsedReq pr = parseReqWithOpenSsl(a->der); // OpenSSL's parser accepts our encoding
    CHECK(pr.ok);
    CHECK_EQ(pr.version, 1);
    CHECK_EQ(pr.algOid, std::string(oid::kSha256));
    CHECK(pr.digest == digest);
    CHECK(pr.certReq);
    CHECK(pr.nonce.has_value() && *pr.nonce == *a->nonce);
    CHECK(pr.policy.has_value() && *pr.policy == "1.2.3.4.5");

    TimeStampRequestOptions bare;
    bare.includeNonce = false;
    bare.certReq = false;
    auto c = buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, digest, bare);
    CHECK(c.has_value());
    const ParsedReq pc = parseReqWithOpenSsl(c->der);
    CHECK(pc.ok && !pc.nonce && !pc.certReq && !pc.policy);

    for (DigestAlgorithm alg : {DigestAlgorithm::Sha384, DigestAlgorithm::Sha512}) {
        const Bytes d = *provider->digest(alg, bytesOf("x"));
        auto r = buildTimeStampRequest(*provider, alg, d);
        CHECK(r.has_value());
        if (r) CHECK(parseReqWithOpenSsl(r->der).algOid == std::string(digestOid(alg)));
    }
    // GOST R 34.11-2012: typed algorithm, imprint encoded without NULL parameters.
    auto gost = buildTimeStampRequest(*provider, DigestAlgorithm::Gost3411_2012_256, Bytes(32, 0x5A));
    CHECK(gost.has_value());
    if (gost) CHECK(parseReqWithOpenSsl(gost->der).algOid == std::string(oid::kGost3411_256));

    // SHA-1 is refused for creation; wrong imprint length is an argument error.
    auto sha1 = buildTimeStampRequest(*provider, DigestAlgorithm::Sha1, Bytes(20, 1));
    CHECK(!sha1.has_value() && sha1.error().code == CryptoErrorCode::WeakAlgorithm);
    auto shortDigest = buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, Bytes(31, 1));
    CHECK(!shortDigest.has_value() && shortDigest.error().code == CryptoErrorCode::InvalidArgument);
    TimeStampRequestOptions badPolicy;
    badPolicy.policyOid = "not.an.oid";
    CHECK(!buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, digest, badPolicy).has_value());
}

// ---- happy paths ----------------------------------------------------------------------

RIVET_TEST(timestampHappyPathEc) {
    Env env;
    env.transport->cfg.fractionMicros = 123456;
    env.transport->cfg.accuracy = true;
    env.transport->cfg.tsaName = true;
    auto r = env.stamp();
    CHECK(expectOk(r, "happy ec"));
    if (!r || !r->ok()) return;
    CHECK_EQ(env.transport->calls.load(), 1);

    // Wire format of the request: POST, application/timestamp-query, valid TimeStampReq.
    const net::HttpRequest& sent = env.transport->lastRequest;
    CHECK(sent.method == net::HttpMethod::Post);
    CHECK(net::findHeader(sent.headers, "content-type").value_or("") == "application/timestamp-query");
    CHECK(parseReqWithOpenSsl(viewOf(sent.body)).ok);

    const TimeStampVerification& v = r->verification;
    CHECK(v.info.has_value());
    CHECK(v.info->policy == "1.3.6.1.4.1.99999.1");
    CHECK(timesClose(v.info->genTime, Clock::now(), std::chrono::seconds(10)));
    const auto frac = std::chrono::duration_cast<std::chrono::microseconds>(
        v.info->genTime - std::chrono::time_point_cast<std::chrono::seconds>(v.info->genTime));
    CHECK_EQ(frac.count(), 123456);
    CHECK(v.info->accuracy.has_value() && v.info->accuracy->seconds == 1 && v.info->accuracy->millis == 500 &&
          v.info->accuracy->micros == 42);
    CHECK(v.info->nonce == r->request.nonce);
    CHECK(v.info->tsaDirectoryNameDer.has_value());
    CHECK(v.tsaCertificate.has_value() && v.tsaCertificate->subject == env.tsa->info.subject);
    CHECK(v.validation.timestamp == TimestampStatus::Valid);
    CHECK(v.validation.crypto == CryptoCheck::Valid);
    CHECK(v.validation.trust == TrustStatus::Trusted);
    CHECK(!r->tokenDer().empty());

    // The raw token is exactly what the TSA sent.
    auto parsed = parseTimeStampResponse(r->responseDer);
    CHECK(parsed.has_value() && parsed->tokenDer.has_value() && *parsed->tokenDer == r->tokenDer());
    CHECK(r->response.status.status == PkiStatus::Granted);
}

RIVET_TEST(timestampHappyPathRsaAndEssVariants) {
    auto provider = makeDefaultProvider();
    CertSpec spec = tsaSpec("RSA TSA");
    spec.keyKind = "rsa2048";
    Env env(makeTsa(*provider, spec));
    for (int ess : {1, 2, 3}) {
        env.transport->cfg.ess = ess;
        auto r = env.stamp();
        if (!expectOk(r, ("rsa ess " + std::to_string(ess)).c_str())) CHECK(false);
    }
    for (DigestAlgorithm alg : {DigestAlgorithm::Sha384, DigestAlgorithm::Sha512}) {
        auto r = env.stamp(alg);
        CHECK(expectOk(r, "sha2 imprint"));
    }
    env.transport->cfg.contentType = "Application/Timestamp-Reply; charset=binary"; // parameters / case ignored
    CHECK(expectOk(env.stamp(), "content type with parameters"));
    env.transport->cfg.status = 1; // grantedWithMods is accepted when a token follows... here without one: malformed
    env.transport->cfg.status.reset();
    env.transport->cfg.ess = 0; // no ESS attribute at all
    CHECK(expectFailure(env.stamp(), TimestampFailure::MalformedToken, "no ESS attribute"));
}

RIVET_TEST(timestampChainedTsa) {
    auto provider = makeDefaultProvider();
    CertSpec caSpec;
    caSpec.cn = "TSA Root";
    caSpec.keyKind = "ec256";
    caSpec.ca = true;
    caSpec.notBeforeOffsetSec = -3600L * 24 * 90;
    const TestKey ca = makeCert(caSpec);
    Env env(makeTsa(*provider, tsaSpec("Issued TSA"), &ca));
    CHECK(expectOk(env.stamp(), "chain via embedded CA"));

    // certReq honoured: without embedded certificates the caller supplies the chain.
    env.transport->cfg.embedCerts = false;
    CHECK(expectFailure(env.stamp(), TimestampFailure::SignatureInvalid, "no certs in token"));
    TimeStampClientOptions o = env.options();
    o.verify.extraCertificates = env.tsa->chainDer;
    CHECK(expectOk(env.client->request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), o, CancelToken{}), "extra certs"));
}

RIVET_TEST(timestampAsync) {
    Env env;
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    int delivered = 0;
    bool ok = false;
    const bool started = env.client->requestAsync(scheduler, dispatcher, scope, CancelToken{}, kTsaUrl, DigestAlgorithm::Sha256,
                                                  env.documentDigest(), env.options(),
                                                  [&](CryptoResult<TimeStampResult> r) {
                                                      ++delivered;
                                                      ok = r.has_value() && r->ok();
                                                  });
    CHECK(started);
    CHECK(dispatcher.drain(1));
    CHECK_EQ(delivered, 1);
    CHECK(ok);
    scope.closeAndWait();
    CHECK(!env.client->requestAsync(scheduler, dispatcher, scope, CancelToken{}, kTsaUrl, DigestAlgorithm::Sha256,
                                    env.documentDigest(), env.options(), [&](CryptoResult<TimeStampResult>) { ++delivered; }));
    CHECK_EQ(delivered, 1);
}

// ---- negative: token content ---------------------------------------------------------------

RIVET_TEST(timestampNonceAndImprintMismatch) {
    Env env;
    env.transport->cfg.nonce = Bytes{0x01, 0x02, 0x03};
    CHECK(expectFailure(env.stamp(), TimestampFailure::NonceMismatch, "wrong nonce"));
    env.transport->cfg.nonce.reset();
    env.transport->cfg.dropNonce = true;
    CHECK(expectFailure(env.stamp(), TimestampFailure::NonceMismatch, "missing nonce"));
    env.transport->cfg.dropNonce = false;

    env.transport->cfg.imprint = std::make_pair(std::string(oid::kSha256), Bytes(32, 0x11));
    CHECK(expectFailure(env.stamp(), TimestampFailure::ImprintMismatch, "wrong imprint digest"));
    env.transport->cfg.imprint = std::make_pair(std::string(oid::kSha384), Bytes(48, 0x11));
    CHECK(expectFailure(env.stamp(), TimestampFailure::ImprintMismatch, "wrong imprint algorithm"));

    // Policy: requested policy must come back.
    env.transport->cfg.imprint.reset();
    TimeStampClientOptions o = env.options();
    o.request.policyOid = "1.3.6.1.4.1.99999.2";
    CHECK(expectOk(env.client->request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), o, CancelToken{}), "policy echoed"));
}

RIVET_TEST(timestampStatusRejection) {
    Env env;
    env.transport->cfg.status = 2; // rejection
    env.transport->cfg.failInfo = pkifailure::kBadAlg;
    env.transport->cfg.statusText = "unsupported hash algorithm";
    auto r = env.stamp();
    CHECK(expectFailure(r, TimestampFailure::BadStatus, "badAlg"));
    if (!r) return;
    CHECK(r->response.status.status == PkiStatus::Rejection);
    CHECK(r->response.status.hasFailInfo && (r->response.status.failInfo & pkifailure::kBadAlg) != 0);
    CHECK(r->response.status.text.size() == 1 && r->response.status.text[0] == "unsupported hash algorithm");
    const auto& notes = r->verification.validation.notes;
    CHECK(std::find(notes.begin(), notes.end(), "failInfo: badAlg") != notes.end());
    CHECK(std::find(notes.begin(), notes.end(), "TSA status text: unsupported hash algorithm") != notes.end());
    CHECK(r->tokenDer().empty());

    env.transport->cfg.failInfo = pkifailure::kSystemFailure | pkifailure::kTimeNotAvailable;
    env.transport->cfg.statusText.clear();
    auto s = env.stamp();
    CHECK(expectFailure(s, TimestampFailure::BadStatus, "system failure"));
    if (s) CHECK_EQ(s->response.status.failInfo, pkifailure::kSystemFailure | pkifailure::kTimeNotAvailable);

    for (int status : {3, 4, 5, 9}) { // waiting, revocation warning / notification, unknown
        env.transport->cfg.status = status;
        env.transport->cfg.failInfo = 0;
        CHECK(expectFailure(env.stamp(), TimestampFailure::BadStatus, "other status"));
    }
    env.transport->cfg.status.reset();
    env.transport->cfg.grantedNoToken = true;
    CHECK(expectFailure(env.stamp(), TimestampFailure::MalformedToken, "granted without token"));
}

RIVET_TEST(timestampTsaCertificateRules) {
    auto provider = makeDefaultProvider();
    struct Case {
        const char* name;
        const char* eku;
    };
    const Case cases[] = {
        {"no EKU", ""},
        {"EKU not critical", "timeStamping"},
        {"other EKU", "critical,serverAuth"},
        {"extra EKU", "critical,timeStamping,emailProtection"},
    };
    for (const Case& c : cases) {
        CertSpec spec = tsaSpec(c.name);
        spec.extKeyUsage = c.eku;
        Env env(makeTsa(*provider, spec));
        CHECK(expectFailure(env.stamp(), TimestampFailure::NotATimestampCert, c.name));
    }
    {
        CertSpec spec = tsaSpec("Wrong key usage");
        spec.extKeyUsage = "critical,timeStamping";
        spec.ca = true; // CA certificates carry keyCertSign only: not a signing key usage
        Env env(makeTsa(*provider, spec));
        CHECK(expectFailure(env.stamp(), TimestampFailure::NotATimestampCert, "key usage"));
    }
}

RIVET_TEST(timestampGenTimeAndTrust) {
    Env env;
    env.transport->cfg.genTime = Clock::now() + std::chrono::hours(24 * 365 * 3);
    CHECK(expectFailure(env.stamp(), TimestampFailure::GenTimeOutsideCertValidity, "genTime after notAfter"));
    env.transport->cfg.genTime = Clock::now() - std::chrono::hours(24 * 365);
    CHECK(expectFailure(env.stamp(), TimestampFailure::GenTimeOutsideCertValidity, "genTime before notBefore"));
    env.transport->cfg.genTime = Clock::now() - std::chrono::hours(24);
    CHECK(expectOk(env.stamp(), "genTime inside validity (past)"));

    env.transport->cfg.genTime.reset();
    TimeStampClientOptions none; // no trust anchors supplied: the OS store is never used
    CHECK(expectFailure(env.client->request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), none, CancelToken{}),
                        TimestampFailure::UntrustedTsa, "no anchors"));
    auto provider = makeDefaultProvider();
    auto other = makeTsa(*provider, tsaSpec("Another TSA"));
    TimeStampClientOptions wrong;
    wrong.verify.anchors = &other->anchors;
    auto r = env.client->request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), wrong, CancelToken{});
    CHECK(expectFailure(r, TimestampFailure::UntrustedTsa, "unrelated anchor"));
    if (r) CHECK(r->verification.validation.timestamp == TimestampStatus::Untrusted);
}

RIVET_TEST(timestampTamperedToken) {
    Env env;
    auto good = env.stamp();
    CHECK(expectOk(good, "baseline"));
    if (!good || !good->ok()) return;
    const TimeStampRequest& req = good->request;
    TimeStampExpectation expect;
    expect.algorithm = req.algorithm;
    expect.digest = req.digest;
    expect.nonce = req.nonce;
    TimeStampVerifyOptions vo;
    vo.anchors = &env.tsa->anchors;

    auto verify = [&](const CmsSignedData& m) {
        auto der = encodeCms(m);
        return verifyTimeStampToken(*der, *env.provider, expect, vo);
    };
    auto model = parseCms(good->tokenDer());
    CHECK(model.has_value());
    if (!model) return;
    {
        CmsSignedData m = *model; // signature value
        m.signers[0].signature[4] ^= 0x40;
        auto v = verify(m);
        CHECK(v.has_value() && v->failure == TimestampFailure::SignatureInvalid);
    }
    {
        CmsSignedData m = *model; // TSTInfo serial number (covered by message-digest)
        (*m.content)[m.content->size() - 1] ^= 0x01;
        auto v = verify(m);
        if (v && v->failure != TimestampFailure::SignatureInvalid && v->failure != TimestampFailure::NonceMismatch && v->failure != TimestampFailure::MalformedToken)
            std::fprintf(stderr, "    tampered TSTInfo gave %s\n", std::string(toString(v->failure)).c_str());
        CHECK(v.has_value() && !v->ok());
    }
    {
        CmsSignedData m = *model; // signed attribute (message-digest value) changed
        m.signers[0].signedAttrsDer[m.signers[0].signedAttrsDer.size() - 3] ^= 0x01;
        auto v = verify(m);
        CHECK(v.has_value() && !v->ok());
    }
    {
        CmsSignedData m = *model; // swapped TSA certificate: ESS hash / signature binding fails
        auto other = makeTsa(*env.provider, tsaSpec("Swapped"));
        m.certificates = {other->certDer};
        auto v = verify(m);
        CHECK(v.has_value() && v->failure == TimestampFailure::SignatureInvalid);
    }
    {
        CmsSignedData m = *model; // not a TSTInfo
        m.contentType = std::string(oid::kData);
        auto v = verify(m);
        CHECK(v.has_value() && v->failure == TimestampFailure::MalformedToken);
    }
    {
        CmsSignedData m = *model; // two signers are forbidden
        m.signers.push_back(m.signers[0]);
        auto v = verify(m);
        CHECK(v.has_value() && v->failure == TimestampFailure::MalformedToken);
    }
}

// ---- negative: transport / hostile input --------------------------------------------------------

RIVET_TEST(timestampMalformedAndMutatedInput) {
    Env env;
    auto good = env.stamp();
    CHECK(expectOk(good, "baseline"));
    if (!good || !good->ok()) return;
    const Bytes response = good->responseDer;
    const TimeStampRequest request = good->request;
    TimeStampVerifyOptions vo;
    vo.anchors = &env.tsa->anchors;

    // Truncations, empty and garbage input never succeed and never crash.
    for (std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{5}, response.size() / 4, response.size() / 2, response.size() - 1}) {
        auto v = verifyTimeStampResponse(ByteView(response.data(), len), *env.provider, request, vo);
        CHECK(v.has_value() && !v->ok() && v->failure == TimestampFailure::MalformedToken);
    }
    const Bytes garbage(200, 0xFF);
    CHECK(!parseTimeStampResponse(garbage).has_value());
    CHECK(!parseTstInfo(garbage).has_value());
    CHECK(!parseTimeStampResponse(Bytes{0x30, 0x80, 0x00, 0x00}).has_value());
    Bytes deep;
    for (int i = 0; i < 200; ++i) deep.insert(deep.begin(), {0x30, 0x80});
    CHECK(!parseTimeStampResponse(deep).has_value());

    // ~200 random mutations: bit flips, truncations, insertions, deletions.
    std::mt19937 rng(0xC0FFEE);
    int rejected = 0;
    for (int i = 0; i < 200; ++i) {
        Bytes m = response;
        switch (rng() % 4) {
        case 0: m[rng() % m.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
        case 1: m.resize(rng() % m.size()); break;
        case 2: m.insert(m.begin() + static_cast<std::ptrdiff_t>(rng() % m.size()), static_cast<std::uint8_t>(rng())); break;
        default: m.erase(m.begin() + static_cast<std::ptrdiff_t>(rng() % m.size())); break;
        }
        auto v = verifyTimeStampResponse(m, *env.provider, request, vo);
        (void)parseTimeStampResponse(m);
        if (!v || !v->ok()) ++rejected;
    }
    CHECK_GE(rejected, 150); // the vast majority must be rejected; some bytes (e.g. padding in unsigned parts) are inert
}

RIVET_TEST(timestampHttpFailures) {
    Env env;
    env.transport->cfg.httpStatus = 500;
    CHECK(expectError(env.stamp(), CryptoErrorCode::BadResponse, "HTTP 500", "500"));
    env.transport->cfg.httpStatus = 200;
    env.transport->cfg.contentType = "text/html";
    CHECK(expectError(env.stamp(), CryptoErrorCode::BadResponse, "wrong content type", "Content-Type"));
    env.transport->cfg.contentType = "application/timestamp-query";
    CHECK(expectError(env.stamp(), CryptoErrorCode::BadResponse, "query content type", "Content-Type"));

    auto mock = std::make_shared<net::MockTransport>();
    net::MockReply noType;
    noType.body = "abc";
    mock->script(kTsaUrl, noType);
    TimeStampClient noTypeClient(std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, mock, env.gate), env.provider);
    CHECK(expectError(noTypeClient.request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), env.options(), CancelToken{}),
                      CryptoErrorCode::BadResponse, "no content type", "Content-Type"));

    // Response size cap comes from NetworkPolicy.
    net::MockReply big;
    big.headers.emplace_back("Content-Type", "application/timestamp-reply");
    big.body.assign(4096, 'x');
    mock->script(kTsaUrl, big);
    net::NetworkPolicy small;
    small.maxResponseBytes = 1024;
    TimeStampClient smallClient(std::make_shared<net::NetworkClient>(small, mock, env.gate), env.provider);
    CHECK(expectError(smallClient.request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), env.options(), CancelToken{}),
                      CryptoErrorCode::Network, "too large", "ResponseTooLarge"));

    // A reply of garbage under the right headers is a malformed reply, not a crash.
    net::MockReply junk;
    junk.headers.emplace_back("Content-Type", "application/timestamp-reply");
    junk.body = "this is not DER";
    mock->script(kTsaUrl, junk);
    TimeStampClient junkClient(std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, mock, env.gate), env.provider);
    CHECK(expectFailure(junkClient.request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), env.options(), CancelToken{}),
                        TimestampFailure::MalformedToken, "junk body"));
}

RIVET_TEST(timestampNetworkGateAndPolicy) {
    Env env;
    env.gate->setMode(net::OnlineValidationMode::Off);
    CHECK(expectError(env.stamp(), CryptoErrorCode::Network, "gate off", "OnlineValidationOff"));
    CHECK_EQ(env.transport->calls.load(), 0);

    env.gate->setMode(net::OnlineValidationMode::Ask); // default prompt denies
    CHECK(expectError(env.stamp(), CryptoErrorCode::Network, "gate ask, denied", "ConsentDenied"));
    CHECK_EQ(env.transport->calls.load(), 0);
    env.gate->setMode(net::OnlineValidationMode::On);

    // SSRF policy applies: no private / loopback / plain-http TSA.
    for (const char* url : {"https://127.0.0.1/tsa", "https://localhost/tsa", "http://tsa.example.com/tsa", "https://10.0.0.5/tsa"}) {
        auto r = env.client->request(url, DigestAlgorithm::Sha256, env.documentDigest(), env.options(), CancelToken{});
        CHECK(expectError(r, CryptoErrorCode::Network, url));
    }
    CHECK(expectError(env.client->request("not a url", DigestAlgorithm::Sha256, env.documentDigest(), env.options(), CancelToken{}),
                      CryptoErrorCode::InvalidArgument, "bad url"));
    CHECK_EQ(env.transport->calls.load(), 0);

    // SHA-1 never reaches the wire.
    auto sha1 = env.client->request(kTsaUrl, DigestAlgorithm::Sha1, Bytes(20, 7), env.options(), CancelToken{});
    CHECK(expectError(sha1, CryptoErrorCode::WeakAlgorithm, "sha1"));
    CHECK_EQ(env.transport->calls.load(), 0);

    CancelToken cancelled;
    cancelled.cancel();
    CHECK(expectError(env.client->request(kTsaUrl, DigestAlgorithm::Sha256, env.documentDigest(), env.options(), cancelled),
                      CryptoErrorCode::Cancelled, "cancelled"));
    CHECK_EQ(env.transport->calls.load(), 0);

    // A normal request still works afterwards (exactly one transport call).
    CHECK(expectOk(env.stamp(), "after refusals"));
    CHECK_EQ(env.transport->calls.load(), 1);
}

// ---- CMS signature time-stamp ---------------------------------------------------------------------------

namespace {

struct Signed {
    TestKey key;
    std::unique_ptr<SigningIdentity> identity;
    CmsSignedData model;
    Bytes der;
    Bytes content;
};

Signed makeSigned(CryptoProvider& provider, const CertSpec& spec) {
    Signed s;
    s.key = makeCert(spec);
    s.identity = makeIdentity(provider, s.key);
    s.content = bytesOf("signed payload for a CAdES-T signature");
    CmsSignOptions o;
    auto m = signCms(provider, *s.identity, ContentSource::fromBytes(s.content), true, o);
    if (m) {
        s.model = std::move(*m);
        s.der = *encodeCms(s.model);
    }
    return s;
}

CertificateInfo infoOf(CryptoProvider& provider, const Bytes& der) { return *provider.parseCertificate(der); }

// Stamps the signature value of signer 0 through the client and returns the token.
std::optional<Bytes> stampSignature(Env& env, const CmsSignedData& model) {
    auto imprint = signatureTimestampImprint(*env.provider, model.signers[0], DigestAlgorithm::Sha256);
    if (!imprint) return std::nullopt;
    auto r = env.client->request(kTsaUrl, DigestAlgorithm::Sha256, *imprint, env.options(), CancelToken{});
    if (!r || !r->ok()) {
        std::fprintf(stderr, "    stamping the signature failed\n");
        return std::nullopt;
    }
    return r->tokenDer();
}

bool opensslCmsVerifies(ByteView cmsDer, const Bytes& detached, ByteView signerCert) {
    BioPtr in(BIO_new_mem_buf(cmsDer.data(), static_cast<int>(cmsDer.size())));
    CMS_ContentInfo* raw = d2i_CMS_bio(in.get(), nullptr);
    if (!raw) return false;
    std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> cms(raw, &CMS_ContentInfo_free);
    const unsigned char* p = signerCert.data();
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(signerCert.size())));
    STACK_OF(X509)* certs = sk_X509_new_null();
    sk_X509_push(certs, cert.get());
    X509_STORE* store = X509_STORE_new();
    BioPtr content(BIO_new_mem_buf(detached.data(), static_cast<int>(detached.size())));
    BioPtr out(BIO_new(BIO_s_mem()));
    const int ok = CMS_verify(cms.get(), certs, store, content.get(), out.get(), CMS_NO_SIGNER_CERT_VERIFY);
    sk_X509_free(certs);
    X509_STORE_free(store);
    return ok == 1;
}

} // namespace

RIVET_TEST(timestampAttachToCmsKeepsSignedBytes) {
    Env env;
    CertSpec spec;
    spec.cn = "Signer";
    spec.keyKind = "ec256";
    Signed s = makeSigned(*env.provider, spec);
    CHECK(!s.der.empty());
    if (s.der.empty()) return;
    const CertificateInfo signerInfo = infoOf(*env.provider, certDer(s.key));

    auto token = stampSignature(env, s.model);
    CHECK(token.has_value());
    if (!token) return;
    auto stamped = attachSignatureTimestampDer(s.der, 0, *token, *env.provider);
    if (!stamped) std::fprintf(stderr, "    attach failed: %s\n", stamped.error().message.c_str());
    CHECK(stamped.has_value());
    if (!stamped) return;
    CHECK(stamped->size() > s.der.size());

    auto before = parseCms(s.der);
    auto after = parseCms(*stamped);
    CHECK(before.has_value() && after.has_value());
    if (!before || !after) return;
    const CmsSignerInfo& b = before->signers[0];
    const CmsSignerInfo& a = after->signers[0];
    // Signed bytes are untouched: signed attributes, signature value, algorithms, certificates.
    CHECK(a.signedAttrsDer == b.signedAttrsDer);
    CHECK(a.signature == b.signature);
    CHECK(a.digestAlgDer == b.digestAlgDer && a.signatureAlgDer == b.signatureAlgDer && a.sidDer == b.sidDer);
    CHECK(after->certificates == before->certificates);
    CHECK(b.unsignedAttrs.empty());
    CHECK(a.unsignedAttrs.size() == 1 && a.unsignedAttrs[0].oid == oid::kSignatureTimeStampToken);
    CHECK(a.unsignedAttrs[0].values.size() == 1 && a.unsignedAttrs[0].values[0] == *token); // the token verbatim
    // The signed attributes appear byte-for-byte inside the new encoding.
    CHECK(std::search(stamped->begin(), stamped->end(), b.signedAttrsDer.begin(), b.signedAttrsDer.end()) != stamped->end());

    // The signature still verifies, with the timestamp attribute present, by OpenSSL and by Rivet.
    CHECK(opensslCmsVerifies(*stamped, s.content, certDer(s.key)));
    CmsVerifyOptions plain;
    auto pv = verifyCms(*after, *env.provider, nullptr, plain); // detached content missing -> error expected
    CHECK(!pv.has_value());
    const ContentSource content = ContentSource::fromBytes(s.content);
    auto base = verifyCms(*after, *env.provider, &content, plain);
    CHECK(base.has_value());
    if (!base) return;
    CHECK(base->at(0).hasTimestampToken);
    CHECK(base->at(0).validation.crypto == CryptoCheck::Valid);
    // No anchors: the signer is untrusted and so is the TSA.
    CHECK(base->at(0).validation.timestamp == TimestampStatus::Untrusted);
    CHECK(base->at(0).validation.state == SignatureState::ValidUntrusted);

    // Trusted signer + trusted TSA -> "valid, timestamp verified".
    TrustAnchorSet both = env.tsa->anchors;
    both.add(signerInfo);
    CmsVerifyOptions trusted;
    trusted.anchors = &both;
    auto good = verifyCms(*after, *env.provider, &content, trusted);
    CHECK(good.has_value());
    if (good) {
        const SignerVerification& sv = good->at(0);
        CHECK(sv.timestamp.has_value() && sv.timestamp->verified);
        if (sv.timestamp) {
            CHECK(timesClose(*sv.timestamp->genTime, Clock::now(), std::chrono::seconds(10)));
            CHECK(sv.timestamp->tsaSubject == env.tsa->info.subject);
            CHECK(sv.timestamp->status == TimestampStatus::Valid);
        }
        CHECK(sv.validation.timestamp == TimestampStatus::Valid);
        if (sv.validation.state != SignatureState::ValidTimestamped)
            std::fprintf(stderr, "    state: %s\n", std::string(toString(sv.validation.state)).c_str());
        CHECK(sv.validation.state == SignatureState::ValidTimestamped);
    }
    // Separate anchor sets for signer and TSA.
    TrustAnchorSet signerOnly;
    signerOnly.add(signerInfo);
    CmsVerifyOptions split;
    split.anchors = &signerOnly;
    auto notTsa = verifyCms(*after, *env.provider, &content, split);
    CHECK(notTsa.has_value() && notTsa->at(0).validation.state == SignatureState::ValidTimestampInvalid);
    split.timestampAnchors = &env.tsa->anchors;
    auto withTsa = verifyCms(*after, *env.provider, &content, split);
    CHECK(withTsa.has_value() && withTsa->at(0).validation.state == SignatureState::ValidTimestamped);

    // A token for a different signature value is refused at attach time.
    Signed other = makeSigned(*env.provider, spec);
    CHECK(!attachSignatureTimestampDer(other.der, 0, *token, *env.provider).has_value());
    // Second attach is refused.
    CHECK(!attachSignatureTimestampDer(*stamped, 0, *token, *env.provider).has_value());
    CHECK(!attachSignatureTimestampDer(s.der, 3, *token, *env.provider).has_value());
    CHECK(!attachSignatureTimestampDer(s.der, 0, Bytes{1, 2, 3}, *env.provider).has_value());

    // Tampering with the signature after stamping: the signature fails, the stamp's imprint no longer matches.
    {
        CmsSignedData m = *after;
        m.signers[0].signature[3] ^= 0x10;
        auto v = verifyCms(m, *env.provider, &content, trusted);
        CHECK(v.has_value());
        if (v) {
            CHECK(v->at(0).validation.state == SignatureState::InvalidSignature);
            CHECK(v->at(0).validation.timestamp == TimestampStatus::Invalid);
            CHECK(v->at(0).timestamp.has_value() && v->at(0).timestamp->failure == TimestampFailure::ImprintMismatch);
        }
    }
    // Garbage in the attribute: present but invalid, no crash.
    {
        CmsSignedData m = *before;
        CmsAttribute junk;
        junk.oid = std::string(oid::kSignatureTimeStampToken);
        junk.values.push_back(Bytes{0x30, 0x03, 0x02, 0x01, 0x05});
        junk.der = dr::sequence({dr::oidElement(oid::kSignatureTimeStampToken), dr::tlv(dr::kSet, junk.values[0])});
        m.signers[0].unsignedAttrs.push_back(junk);
        auto v = verifyCms(m, *env.provider, &content, trusted);
        CHECK(v.has_value() && v->at(0).validation.timestamp == TimestampStatus::Invalid &&
              v->at(0).validation.state == SignatureState::ValidTimestampInvalid);
    }
    // ~50 mutations of the stamped CMS: never crash or hang.
    std::mt19937 rng(0xFEEDBEEF);
    for (int i = 0; i < 50; ++i) {
        Bytes m = *stamped;
        if (i % 3 == 0) m.resize(rng() % m.size());
        else m[rng() % m.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        auto parsed = parseCms(m);
        if (parsed) (void)verifyCms(*parsed, *env.provider, &content, trusted);
    }

    // describeCms notes the time-stamp token; CMS without one is unchanged.
    CHECK(describeCms(*after, env.provider.get()).find("timestamp token: present") != std::string::npos);
    auto unstamped = verifyCms(*before, *env.provider, &content, trusted);
    CHECK(unstamped.has_value() && unstamped->at(0).validation.state == SignatureState::ValidTrusted &&
          !unstamped->at(0).timestamp.has_value() && unstamped->at(0).validation.timestamp == TimestampStatus::None);
}

RIVET_TEST(timestampRescuesExpiredSignerCertificate) {
    Env env;
    CertSpec spec;
    spec.cn = "Expired Signer";
    spec.keyKind = "ec256";
    spec.notBeforeOffsetSec = -3600L * 24 * 30;
    spec.notAfterOffsetSec = -3600; // expired an hour ago
    Signed s = makeSigned(*env.provider, spec);
    CHECK(!s.der.empty());
    if (s.der.empty()) return;
    TrustAnchorSet both = env.tsa->anchors;
    both.add(infoOf(*env.provider, certDer(s.key)));
    CmsVerifyOptions o;
    o.anchors = &both;
    const ContentSource content = ContentSource::fromBytes(s.content);

    // Stamped while the signer certificate was still valid (two days ago).
    env.transport->cfg.genTime = Clock::now() - std::chrono::hours(48);
    auto token = stampSignature(env, s.model);
    CHECK(token.has_value());
    if (!token) return;
    auto stamped = attachSignatureTimestampDer(s.der, 0, *token, *env.provider);
    CHECK(stamped.has_value());
    if (!stamped) return;
    auto model = parseCms(*stamped);
    auto v = verifyCms(*model, *env.provider, &content, o);
    CHECK(v.has_value());
    if (v) {
        if (v->at(0).validation.state != SignatureState::ValidAtTrustedTimestamp)
            std::fprintf(stderr, "    state: %s\n", std::string(toString(v->at(0).validation.state)).c_str());
        CHECK(v->at(0).validation.state == SignatureState::ValidAtTrustedTimestamp);
    }

    // Stamped after the signer certificate expired: the stamp cannot vouch for it.
    env.transport->cfg.genTime = Clock::now() - std::chrono::minutes(10);
    auto late = stampSignature(env, s.model);
    CHECK(late.has_value());
    if (!late) return;
    auto lateStamped = attachSignatureTimestampDer(s.der, 0, *late, *env.provider);
    CHECK(lateStamped.has_value());
    if (!lateStamped) return;
    auto lateModel = parseCms(*lateStamped);
    auto lv = verifyCms(*lateModel, *env.provider, &content, o);
    CHECK(lv.has_value() && lv->at(0).validation.state == SignatureState::ValidExpiredNow);
}

// ---- OpenSSL interop ------------------------------------------------------------------------------

RIVET_TEST(timestampOpenSslInterop) {
    if (opensslCli().empty()) {
        std::fprintf(stderr, "    openssl CLI not found: CLI interop skipped\n");
    }
    Env env;
    const std::filesystem::path dir = tempDir() / "timestamp";
    std::filesystem::create_directories(dir);
    writePem(dir / "tsa.pem", env.tsa->certDer);

    // OpenSSL's own TS responder -> our verifier (independent producer; ESS v1 and v2).
    for (bool sha256Ess : {false, true}) {
        auto req = buildTimeStampRequest(*env.provider, DigestAlgorithm::Sha256, env.documentDigest());
        CHECK(req.has_value());
        if (!req) return;
        const Bytes resp = opensslTsResponse(*env.tsa, req->der, sha256Ess);
        CHECK(!resp.empty());
        if (resp.empty()) return;
        auto v = verifyTimeStampResponse(resp, *env.provider, *req, [&] {
            TimeStampVerifyOptions o;
            o.anchors = &env.tsa->anchors;
            return o;
        }());
        if (v && !v->ok()) {
            std::fprintf(stderr, "    OpenSSL-produced response rejected: %s\n", std::string(toString(v->failure)).c_str());
            for (const std::string& n : v->validation.notes) std::fprintf(stderr, "      note: %s\n", n.c_str());
        }
        CHECK(v.has_value() && v->ok());
        if (v && v->ok()) {
            CHECK(v->info->policy == "1.3.6.1.4.1.99999.7");
            CHECK(v->info->serialHex == "1092"); // 4242
            CHECK(v->info->accuracy.has_value() && v->info->accuracy->seconds == 1);
            CHECK(v->info->nonce == req->nonce);
        }
        // Wrong request: the imprint check catches a response made for another document.
        auto other = buildTimeStampRequest(*env.provider, DigestAlgorithm::Sha256, *env.provider->digest(DigestAlgorithm::Sha256, bytesOf("other")));
        auto mism = verifyTimeStampResponse(resp, *env.provider, *other, [&] {
            TimeStampVerifyOptions o;
            o.anchors = &env.tsa->anchors;
            return o;
        }());
        CHECK(mism.has_value() && mism->failure == TimestampFailure::ImprintMismatch);
    }
    if (opensslCli().empty()) return;

    // Our request and the OpenSSL-produced / Rivet-test-TSA-produced responses through `openssl ts`.
    const Bytes data = bytesOf("interop data to be time-stamped");
    writeFile(dir / "data.bin", data);
    const Bytes digest = *env.provider->digest(DigestAlgorithm::Sha256, data);
    auto req = buildTimeStampRequest(*env.provider, DigestAlgorithm::Sha256, digest);
    CHECK(req.has_value());
    if (!req) return;
    writeFile(dir / "req.tsq", req->der);

    CliRun q = runCli("ts -query -in \"" + (dir / "req.tsq").string() + "\" -text", dir);
    if (q.status != 0) std::fprintf(stderr, "    openssl ts -query:\n%s\n", q.output.c_str());
    CHECK_EQ(q.status, 0);
    CHECK(q.output.find("Hash Algorithm: sha256") != std::string::npos);
    CHECK(q.output.find("Nonce:") != std::string::npos);
    CHECK(q.output.find("Certificate required: yes") != std::string::npos);

    struct Producer {
        const char* name;
        Bytes response;
    };
    std::vector<Producer> producers;
    producers.push_back({"openssl-ts-responder", opensslTsResponse(*env.tsa, req->der, true)});
    producers.push_back({"rivet-test-tsa", buildResponseFor(*env.provider, *env.tsa, req->der, TsaCfg{})});
    for (const Producer& p : producers) {
        const std::filesystem::path tsr = dir / (std::string(p.name) + ".tsr");
        writeFile(tsr, p.response);
        CliRun text = runCli("ts -reply -in \"" + tsr.string() + "\" -text", dir);
        if (text.status != 0) std::fprintf(stderr, "    [%s] openssl ts -reply:\n%s\n", p.name, text.output.c_str());
        CHECK_EQ(text.status, 0);
        CHECK(text.output.find("Status: Granted") != std::string::npos);
        CliRun ver = runCli("ts -verify -queryfile \"" + (dir / "req.tsq").string() + "\" -in \"" + tsr.string() +
                                "\" -CAfile \"" + (dir / "tsa.pem").string() + "\"",
                            dir);
        if (ver.status != 0) std::fprintf(stderr, "    [%s] openssl ts -verify:\n%s\n", p.name, ver.output.c_str());
        CHECK_EQ(ver.status, 0);
        CHECK(ver.output.find("Verification: OK") != std::string::npos);
        CliRun verData = runCli("ts -verify -data \"" + (dir / "data.bin").string() + "\" -in \"" + tsr.string() +
                                    "\" -CAfile \"" + (dir / "tsa.pem").string() + "\"",
                                dir);
        // -data re-hashes with the response's algorithm and ignores our nonce: also OK for a nonce-less check.
        CHECK(verData.status == 0 || verData.output.find("nonce") != std::string::npos);

        // Our verifier agrees with OpenSSL.
        TimeStampVerifyOptions o;
        o.anchors = &env.tsa->anchors;
        auto ours = verifyTimeStampResponse(p.response, *env.provider, *req, o);
        CHECK(ours.has_value() && ours->ok());

        // Disagreement check: wrong data must fail in both.
        writeFile(dir / "wrong.bin", bytesOf("different data"));
        CliRun bad = runCli("ts -verify -data \"" + (dir / "wrong.bin").string() + "\" -in \"" + tsr.string() +
                                "\" -CAfile \"" + (dir / "tsa.pem").string() + "\"",
                            dir);
        CHECK(bad.status != 0);
    }

    // A rejection built by our test TSA is parsed by OpenSSL the way we parse it.
    TsaCfg reject;
    reject.status = 2;
    reject.failInfo = pkifailure::kBadAlg;
    reject.statusText = "bad algorithm";
    const Bytes rej = buildResponseFor(*env.provider, *env.tsa, req->der, reject);
    writeFile(dir / "reject.tsr", rej);
    CliRun rejText = runCli("ts -reply -in \"" + (dir / "reject.tsr").string() + "\" -text", dir);
    CHECK_EQ(rejText.status, 0);
    CHECK(rejText.output.find("Status: Rejected") != std::string::npos);
    CHECK(rejText.output.find("unrecognized or unsupported algorithm identifier") != std::string::npos);

    // A CMS signature carrying a stamped signature value.
    CertSpec spec;
    spec.cn = "Interop Signer";
    spec.keyKind = "ec256";
    Signed s = makeSigned(*env.provider, spec);
    CHECK(!s.der.empty());
    if (s.der.empty()) return;
    auto token = stampSignature(env, s.model);
    CHECK(token.has_value());
    if (!token) return;
    auto stamped = attachSignatureTimestampDer(s.der, 0, *token, *env.provider);
    CHECK(stamped.has_value());
    if (!stamped) return;
    writeFile(dir / "stamped.p7s", *stamped);
    writeFile(dir / "stamp.tst", *token);
    CliRun asn1 = runCli("asn1parse -inform DER -in \"" + (dir / "stamped.p7s").string() + "\"", dir);
    CHECK_EQ(asn1.status, 0);
    CHECK(asn1.output.find("id-smime-aa-timeStampToken") != std::string::npos);
    // The token itself verifies in OpenSSL against the SHA-256 of the signature value.
    const Bytes imprint = *signatureTimestampImprint(*env.provider, s.model.signers[0], DigestAlgorithm::Sha256);
    CliRun tokenVerify = runCli("ts -verify -token_in -in \"" + (dir / "stamp.tst").string() + "\" -digest " + hexString(imprint) +
                                    " -CAfile \"" + (dir / "tsa.pem").string() + "\"",
                                dir);
    if (tokenVerify.status != 0) std::fprintf(stderr, "    openssl ts -verify -token_in:\n%s\n", tokenVerify.output.c_str());
    CHECK_EQ(tokenVerify.status, 0);
    // And OpenSSL's CMS verification of the stamped message still passes.
    writeFile(dir / "payload.bin", s.content);
    CliRun cms = runCli("cms -verify -inform DER -in \"" + (dir / "stamped.p7s").string() + "\" -content \"" + (dir / "payload.bin").string() +
                            "\" -binary -noverify -out \"" + (dir / "cms-out.bin").string() + "\"",
                        dir);
    if (cms.status != 0) std::fprintf(stderr, "    openssl cms -verify:\n%s\n", cms.output.c_str());
    CHECK_EQ(cms.status, 0);
}
