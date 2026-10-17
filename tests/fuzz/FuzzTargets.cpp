// SPDX-License-Identifier: MPL-2.0
#include "FuzzTargets.hpp"

#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"
#include "crypto/Providers.hpp"
#include "crypto/X509Parse.hpp"
#include "crypto/remote/JsonLite.hpp"
#include "crypto/revocation/Crl.hpp"
#include "crypto/revocation/Ocsp.hpp"
#include "crypto/revocation/Revocation.hpp"
#include "crypto/timestamp/TimeStamp.hpp"
#include "net/Client.hpp"
#include "net/Ip.hpp"
#include "net/MockTransport.hpp"
#include "net/Policy.hpp"
#include "net/Url.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"
#include "pdf/sign/PdfDss.hpp"
#include "pdf/sign/PdfFile.hpp"
#include "pdf/sign/PdfSignatureVerifier.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

namespace rivet::fuzz {

namespace {

namespace cr = rivet::crypto;
namespace dr = rivet::crypto::der;
namespace ps = rivet::pdf::sign;

cr::CryptoProvider& provider() {
    static const std::shared_ptr<cr::CryptoProvider> p = cr::makeDefaultProvider();
    return *p;
}

cr::ByteView view(const std::uint8_t* data, std::size_t size) { return cr::ByteView(data, size); }

std::string_view text(const std::uint8_t* data, std::size_t size) {
    return std::string_view(reinterpret_cast<const char*>(data), size);
}

// Anything in the input that is a self-issued certificate becomes a trust
// anchor, so the chain / trust code paths past "untrusted" are reachable.
void addSelfIssued(cr::TrustAnchorSet& set, const std::vector<cr::Bytes>& certs) {
    std::size_t taken = 0;
    for (const cr::Bytes& der : certs) {
        if (++taken > 8) break;
        auto info = provider().parseCertificate(der);
        if (info && info->isSelfIssued()) set.add(*info);
    }
}

// Consumes a u16be length-prefixed chunk.
bool takeChunk(const std::uint8_t*& p, std::size_t& left, cr::ByteView& out) {
    if (left < 2) return false;
    const std::size_t n = (std::size_t{p[0]} << 8) | p[1];
    p += 2;
    left -= 2;
    if (n > left) return false;
    out = cr::ByteView(p, n);
    p += n;
    left -= n;
    return true;
}

// ---- DER ------------------------------------------------------------------------------------

void walk(const dr::Element& e, int depth, std::size_t& budget) {
    if (budget == 0 || depth > dr::kMaxDepth + 2) return;
    --budget;
    switch (e.tag) {
    case dr::kOid:
        if (auto s = dr::decodeOid(e.content)) {
            if (auto enc = dr::encodeOid(*s)) (void)dr::decodeOid(*enc);
        }
        break;
    case dr::kUtcTime:
    case dr::kGeneralizedTime: (void)dr::decodeTime(e); break;
    case dr::kOctetString: (void)dr::octetStringValue(e); break;
    case dr::kInteger: (void)dr::normalizeInteger(e.content); break;
    default: break;
    }
    if (e.constructed) {
        auto kids = dr::children(e);
        if (!kids) return;
        for (const dr::Element& k : *kids) {
            if (k.full.empty()) invariantViolation("der", "child element with empty encoding");
            walk(k, depth + 1, budget);
        }
    }
}

// Writer round trip: what the Rivet writer emits, the reader must accept.
void writerRoundTrip(cr::ByteView content) {
    const cr::Bytes seq = dr::sequence({dr::octetString(content), dr::integer(content), dr::setOf({dr::octetString(content), dr::nullValue()})});
    auto e = dr::readSingle(seq);
    if (!e) invariantViolation("der", "writer output rejected by the reader");
}

} // namespace

void invariantViolation(const char* target, const char* what) {
    std::fprintf(stderr, "FUZZ INVARIANT VIOLATION [%s]: %s\n", target, what);
    std::fflush(stderr);
    std::abort();
}

int fuzzDer(const std::uint8_t* data, std::size_t size) {
    const cr::ByteView in = view(data, size);
    std::size_t budget = 4096;
    if (auto e = dr::readElement(in)) {
        if (e->full.size() > in.size()) invariantViolation("der", "element extends past the input");
        walk(*e, 0, budget);
    }
    if (auto e = dr::readSingle(in)) walk(*e, 0, budget);
    (void)dr::decodeOid(in);
    (void)dr::encodeOid(text(data, std::min<std::size_t>(size, 128)));
    if (size > 0) {
        dr::Element synthetic;
        synthetic.tag = data[0] & 0x1F ? dr::kGeneralizedTime : dr::kUtcTime;
        synthetic.content = in.subspan(1);
        (void)dr::decodeTime(synthetic);
    }
    writerRoundTrip(in.subspan(0, std::min<std::size_t>(size, 256)));
    return 0;
}

// ---- X.509 --------------------------------------------------------------------------------------

int fuzzX509(const std::uint8_t* data, std::size_t size) {
    const cr::ByteView in = view(data, size);
    cr::X509Extras extras;
    auto digest = [](cr::DigestAlgorithm a, cr::ByteView d) { return provider().digest(a, d); };
    auto info = cr::parseX509(in, digest, &extras);
    if (info) {
        if (info->der.size() > size) invariantViolation("x509", "certificate larger than its input");
        cr::TrustAnchorSet set;
        set.add(*info);
        if (!set.contains(*info)) invariantViolation("x509", "anchor set lost a certificate");
        (void)info->validAt(std::chrono::system_clock::now());
        (void)info->isSelfIssued();
    }
    // The OpenSSL-backed parse and the revocation-extension reader see the same bytes.
    (void)provider().parseCertificate(in);
    (void)cr::parseRevocationExtensions(in);
    (void)cr::classifyRevocationUrl(text(data, std::min<std::size_t>(size, 512)));
    return 0;
}

// ---- CMS ----------------------------------------------------------------------------------------------

int fuzzCms(const std::uint8_t* data, std::size_t size) {
    const cr::ByteView in = view(data, size);
    (void)cr::decodePem(in);
    auto model = cr::parseCmsAny(in);
    if (!model) return 0;
    (void)cr::encodeCms(*model);
    (void)cr::describeCms(*model, &provider());

    cr::TrustAnchorSet anchors;
    addSelfIssued(anchors, model->certificates);
    cr::CmsVerifyOptions options;
    options.anchors = &anchors;
    options.timestampAnchors = &anchors;
    const cr::ContentSource content = cr::ContentSource::fromBytes(cr::Bytes(kCmsMessage.begin(), kCmsMessage.end()));
    (void)cr::verifyCms(*model, provider(), model->isDetached() ? &content : nullptr, options);
    // Untrusted verification takes a different branch (no anchors at all).
    (void)cr::verifyCms(*model, provider(), model->isDetached() ? &content : nullptr, cr::CmsVerifyOptions{});
    return 0;
}

// ---- RFC 3161 -------------------------------------------------------------------------------------------

int fuzzTimestamp(const std::uint8_t* data, std::size_t size) {
    const cr::ByteView in = view(data, size);
    (void)cr::parseTstInfo(in);
    cr::Bytes token(in.begin(), in.end());
    if (auto response = cr::parseTimeStampResponse(in)) {
        (void)cr::failInfoNames(response->status.failInfo);
        if (response->tokenDer) token = *response->tokenDer;
    }
    auto cms = cr::parseCms(token);
    cr::TrustAnchorSet anchors;
    if (cms) addSelfIssued(anchors, cms->certificates);
    cr::TimeStampExpectation expected;
    expected.algorithm = cr::DigestAlgorithm::Sha256;
    if (auto d = provider().digest(cr::DigestAlgorithm::Sha256, cr::viewOf(kTimestampMessage))) expected.digest = *d;
    cr::TimeStampVerifyOptions options;
    options.anchors = &anchors;
    (void)cr::verifyTimeStampToken(token, provider(), expected, options);
    (void)cr::verifyTimeStampToken(token, provider(), expected, cr::TimeStampVerifyOptions{});
    if (cms) {
        for (const cr::CmsSignerInfo& s : cms->signers) {
            (void)cr::signatureTimestampImprint(provider(), s, cr::DigestAlgorithm::Sha256);
            if (s.findUnsigned("1.2.840.113549.1.9.16.2.14"))
                (void)cr::verifySignatureTimestamp(s, provider(), &anchors, cms->certificates, cr::CancelToken{});
        }
    }
    return 0;
}

// ---- OCSP / CRL --------------------------------------------------------------------------------------------

Bytes frameRevocation(std::span<const std::uint8_t> cert, std::span<const std::uint8_t> issuer,
                      std::span<const std::uint8_t> evidence) {
    Bytes out;
    for (const auto part : {cert, issuer}) {
        out.push_back(static_cast<std::uint8_t>(part.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(part.size() & 0xFF));
        out.insert(out.end(), part.begin(), part.end());
    }
    out.insert(out.end(), evidence.begin(), evidence.end());
    return out;
}

int fuzzRevocation(const std::uint8_t* data, std::size_t size) {
    const std::uint8_t* p = data;
    std::size_t left = size;
    cr::ByteView certDer, issuerDer;
    if (!takeChunk(p, left, certDer) || !takeChunk(p, left, issuerDer)) return 0;
    const cr::ByteView evidence(p, left);
    auto cert = provider().parseCertificate(certDer);
    auto issuer = provider().parseCertificate(issuerDer);
    if (!cert || !issuer) return 0;
    auto ext = cr::parseRevocationExtensions(certDer);
    (void)cr::parseRevocationExtensions(issuerDer);

    cr::EvidenceClock now;
    cr::EvidenceClock historical;
    historical.validationTime = now.now - std::chrono::hours(24 * 90);
    const std::optional<cr::Bytes> nonce = cr::Bytes(16, 0x5A);
    for (const cr::EvidenceClock& clock : {now, historical}) {
        const cr::OcspEvidence o = cr::verifyOcspResponse(evidence, provider(), *cert, *issuer, std::nullopt, clock);
        if (o.verified && o.failure != cr::EvidenceFailure::None)
            invariantViolation("revocation", "OCSP evidence verified with a failure code");
        const cr::OcspEvidence on = cr::verifyOcspResponse(evidence, provider(), *cert, *issuer, nonce, clock);
        // A response must never verify against a nonce it cannot contain.
        if (on.verified && on.nonceSent && !on.nonceChecked)
            invariantViolation("revocation", "OCSP verified with an unchecked nonce");
        const cr::CrlEvidence c = cr::verifyCrl(evidence, provider(), *cert, *issuer,
                                                ext ? *ext : cr::RevocationExtensions{}, clock);
        if (c.verified && c.failure != cr::EvidenceFailure::None)
            invariantViolation("revocation", "CRL evidence verified with a failure code");
    }
    return 0;
}

// ---- PDF signatures / DSS -----------------------------------------------------------------------------------

int fuzzPdfSignature(const std::uint8_t* data, std::size_t size) {
    const cr::ByteView in = view(data, size);
    ps::VerifyOptions plain;
    auto first = ps::verifyPdfSignatures(in, provider(), plain);
    if (first) {
        std::vector<cr::Bytes> certs;
        for (const ps::PdfSignatureInfo& s : *first) {
            (void)ps::describeSignature(s);
            certs.insert(certs.end(), s.material.cmsCertificates.begin(), s.material.cmsCertificates.end());
            certs.insert(certs.end(), s.material.tokenCertificates.begin(), s.material.tokenCertificates.end());
            if (s.byteRangeValid && s.coversWholeFile) {
                // A fully covering ByteRange may be followed by whitespace only.
                bool bad = s.byteRange.size() != 4 || s.byteRange[0] != 0 || s.byteRange[2] < 0;
                if (!bad) {
                    const auto end = static_cast<std::uint64_t>(s.byteRange[2]) + static_cast<std::uint64_t>(s.byteRange[3]);
                    bad = end > size;
                    for (std::uint64_t p = end; !bad && p < size; ++p) {
                        const std::uint8_t c = data[p];
                        if (c != 0 && c != 9 && c != 10 && c != 12 && c != 13 && c != 32) bad = true;
                    }
                }
                if (bad) invariantViolation("pdfsig", "coversWholeFile with non-whitespace data after the ByteRange");
            }
        }
        if (!certs.empty()) {
            cr::TrustAnchorSet anchors;
            addSelfIssued(anchors, certs);
            ps::VerifyOptions trusting;
            trusting.anchors = &anchors;
            trusting.timestampAnchors = &anchors;
            (void)ps::verifyPdfSignatures(in, provider(), trusting);
        }
    }
    if (auto file = ps::PdfFile::open(in)) {
        (void)file->isEncrypted();
        (void)ps::readDss(*file);
        for (std::size_t view = 0; view < std::min<std::size_t>(file->sectionCount(), 3); ++view) (void)ps::readDss(*file, view);
        (void)file->fields();
    }
    return 0;
}

// ---- PDF encryption ---------------------------------------------------------------------------------------------

int fuzzPdfEncrypt(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    namespace pe = rivet::pdf::encrypt;
    pe::EncryptOptions options;
    options.cipher = (data[0] & 1) ? pe::Cipher::Aes128 : pe::Cipher::Aes256;
    options.userPassword = (data[0] & 2) ? "user-pw" : "";
    options.ownerPassword = (data[0] & 4) ? "owner-pw" : "";
    options.encryptMetadata = (data[0] & 8) != 0;
    options.permissions.print = (data[0] & 16) != 0;
    options.permissions.copy = (data[0] & 32) != 0;
    auto out = pe::encryptPdf(std::span<const std::uint8_t>(data + 1, size - 1), options);
    if (out && (out->size() < 8 || std::string_view(reinterpret_cast<const char*>(out->data()), 5) != "%PDF-"))
        invariantViolation("pdfencrypt", "encrypted output does not start with a PDF header");
    return 0;
}

// ---- network layer: URL, IP, host policy, redirects ---------------------------------------------------------------

namespace {

void checkUrlInvariants(const net::Url& u, std::string_view input) {
    (void)input;
    const net::NetworkPolicy defaults;
    net::NetworkPolicy lax;
    lax.allowHttp = true;
    lax.allowPrivateNetworks = true;
    const std::string s = u.toString();
    auto again = net::parseUrl(s);
    if (!again) invariantViolation("net", "Url::toString() does not parse again");
    if (!(*again == u)) invariantViolation("net", "parse(toString(url)) != url");
    if (u.host.empty() || u.port == 0) invariantViolation("net", "URL without host or port accepted");
    // SSRF: an IP literal in ANY notation must be classified by its address.
    if (u.ip) {
        const bool forbidden = net::isForbiddenAddress(*u.ip);
        const bool blocked = !net::checkUrl(u, [&] { auto p = defaults; p.allowHttp = true; return p; }());
        if (forbidden && !blocked) invariantViolation("net", "forbidden literal address passed checkUrl");
    } else {
        std::string host = u.host;
        if (net::parseIpv4Strict(host) || net::parseIpv4Legacy(host) || net::parseIpv6(host))
            invariantViolation("net", "IP literal host was not recognised as an address (SSRF classification bypass)");
    }
    (void)net::checkUrl(u, defaults);
    (void)net::checkUrl(u, lax);
    (void)u.origin();
}

void splitAt(std::string_view s, char sep, std::string_view& a, std::string_view& b) {
    const std::size_t i = s.find(sep);
    a = s.substr(0, i);
    b = i == std::string_view::npos ? std::string_view{} : s.substr(i + 1);
}

void fuzzRedirects(const std::uint8_t* data, std::size_t size) {
    // data[0]: policy bits; then records: status byte, u8 location length, location.
    static const char* const kUrls[] = {"https://example.com/a", "https://example.com/b", "https://example.com/c",
                                        "http://example.com/a",  "http://example.com/b",  "https://other.example.org/x",
                                        "http://127.0.0.1/",     "http://localhost/",     "https://[::1]/",
                                        "http://169.254.169.254/latest/meta-data"};
    static constexpr int kStatuses[] = {200, 301, 302, 303, 307, 308, 404, 500, 204, 399};
    net::NetworkPolicy policy;
    policy.allowHttp = (data[0] & 1) != 0;
    policy.allowPrivateNetworks = (data[0] & 2) != 0;
    policy.maxRedirects = data[0] >> 4 & 7;
    if (data[0] & 4) policy.allowedHosts = {"example.com", "*.example.org"};
    auto transport = std::make_shared<net::MockTransport>();
    std::size_t pos = 1;
    for (const char* url : kUrls) {
        if (pos + 2 > size) break;
        net::MockReply reply;
        reply.status = kStatuses[data[pos] % 10];
        const std::size_t len = std::min<std::size_t>(data[pos + 1], size - pos - 2);
        reply.headers.emplace_back("Location", std::string(reinterpret_cast<const char*>(data + pos + 2), len));
        reply.body = "body";
        pos += 2 + len;
        transport->script(url, std::move(reply));
    }
    net::NetworkClient client(policy, transport, std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On));
    net::HttpRequest request;
    auto start = net::parseUrl(kUrls[(data[0] >> 3) & 1 ? 3 : 0]);
    request.url = *start;
    auto result = client.fetch(request);
    const auto sent = transport->requests();
    if (static_cast<int>(sent.size()) > policy.maxRedirects + 1)
        invariantViolation("net", "more transport calls than maxRedirects allows");
    for (const net::HttpRequest& r : sent) {
        if (net::checkUrl(r.url, policy)) continue;
        invariantViolation("net", "transport was called for a URL the policy forbids");
    }
    for (std::size_t i = 1; i < sent.size(); ++i)
        if (sent[i - 1].url.isHttps() && !sent[i].url.isHttps())
            invariantViolation("net", "https -> http downgrade was followed");
    if (result && (result->status >= 300 && result->status < 400) && result->headers.empty())
        invariantViolation("net", "redirect response lost its headers");
}

} // namespace

int fuzzNet(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::string_view body = text(data + 1, size - 1);
    switch (data[0] % 6) {
    case 0: {
        if (auto u = net::parseUrl(body)) checkUrlInvariants(*u, body);
        break;
    }
    case 1: {
        std::string_view baseText, ref;
        splitAt(body, '\n', baseText, ref);
        auto base = net::parseUrl(baseText);
        if (!base) break;
        if (auto r = net::resolveReference(*base, ref)) checkUrlInvariants(*r, ref);
        break;
    }
    case 2: {
        const std::string s(body);
        for (auto addr : {net::parseIpv4Strict(s), net::parseIpv4Legacy(s), net::parseIpv6(s)}) {
            if (!addr) continue;
            (void)net::isForbiddenAddress(*addr);
            const std::string f = net::formatIp(*addr);
            auto back = addr->isV6 ? net::parseIpv6(f) : net::parseIpv4Strict(f);
            if (!back || !(*back == *addr)) invariantViolation("net", "formatIp / parseIp round trip changed the address");
        }
        break;
    }
    case 3: {
        std::string_view pattern, host;
        splitAt(body, '\n', pattern, host);
        (void)net::hostMatchesPattern(pattern, host);
        (void)net::isPrivateHostName(host);
        net::NetworkPolicy p;
        p.allowedHosts = {std::string(pattern)};
        (void)net::hostAllowed(p, host);
        break;
    }
    case 4: {
        net::HttpHeaders headers;
        std::string_view rest = body;
        while (!rest.empty() && headers.size() < 64) {
            std::string_view line;
            splitAt(rest, '\n', line, rest);
            std::string_view name, value;
            splitAt(line, ':', name, value);
            headers.emplace_back(std::string(name), std::string(value));
        }
        (void)net::validateHeaders(headers);
        (void)net::findHeader(headers, "location");
        break;
    }
    default:
        if (size >= 2) fuzzRedirects(data + 1, size - 1);
        break;
    }
    return 0;
}

// ---- JsonLite -----------------------------------------------------------------------------------------------------------

namespace {

void walkJson(const crypto::remote::JsonValue& v, int depth) {
    if (depth > crypto::remote::kMaxJsonDepth + 1) invariantViolation("json", "value nested deeper than kMaxJsonDepth");
    if (v.items.size() > 0 && v.kind == crypto::remote::JsonValue::Kind::Object && v.keys.size() != v.items.size())
        invariantViolation("json", "object keys and values differ in count");
    for (std::size_t i = 0; i < v.items.size(); ++i) {
        if (v.isObject()) {
            (void)v.stringAt(v.keys[i]);
            (void)v.intAt(v.keys[i]);
            (void)v.boolAt(v.keys[i]);
            (void)v.find(v.keys[i]);
        }
        walkJson(v.items[i], depth + 1);
    }
}

} // namespace

int fuzzJson(const std::uint8_t* data, std::size_t size) {
    using namespace crypto::remote;
    const std::string_view in = text(data, size);
    if (auto v = parseJson(in)) walkJson(*v, 0);
    if (auto bytes = base64Decode(in)) {
        auto back = base64Decode(base64Encode(*bytes));
        if (!back || *back != *bytes) invariantViolation("json", "base64 round trip changed the data");
    }
    (void)base64Encode(view(data, size));
    (void)formEncode(in);
    // The writer's output is always parseable and keeps the string.
    JsonObjectWriter w;
    w.add("k", in).addInt("n", static_cast<long long>(size)).addBool("b", size % 2 == 0);
    auto parsed = parseJson(w.str());
    if (!parsed) invariantViolation("json", "JsonObjectWriter output is not parseable");
    if (auto s = parsed->stringAt("k"); !s || *s != in) invariantViolation("json", "string did not survive quote + parse");
    return 0;
}

} // namespace rivet::fuzz
