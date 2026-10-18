// SPDX-License-Identifier: MPL-2.0
// RFC 3161 response / TSTInfo parsing and TimeStampToken verification
// (RFC 3161 section 2.4.2 and 2.3, RFC 5816 ESSCertIDv2).
#include "crypto/Der.hpp"
#include "crypto/timestamp/TimeStamp.hpp"

#include <algorithm>
#include <cstdio>

namespace rivet::crypto {

using namespace der;

namespace {

constexpr std::string_view kIdKpTimeStamping = "1.3.6.1.5.5.7.3.8";
constexpr std::string_view kEkuExtension = "2.5.29.37";
constexpr std::size_t kMaxStatusStrings = 16;
constexpr std::size_t kMaxStatusStringLength = 256;
constexpr std::size_t kMaxIntegerBytes = 32;

std::unexpected<CryptoError> bad(std::string message) { return fail(CryptoErrorCode::MalformedCms, std::move(message)); }

Bytes toBytes(ByteView v) { return Bytes(v.begin(), v.end()); }

std::string hexOf(ByteView v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    if (v.empty()) return "00";
    std::string out;
    out.reserve(v.size() * 2);
    for (std::uint8_t b : v) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

std::string sanitize(ByteView text) {
    std::string out;
    const std::size_t n = std::min(text.size(), kMaxStatusStringLength);
    for (std::size_t i = 0; i < n; ++i) out.push_back(text[i] >= 0x20 && text[i] < 0x7F ? static_cast<char>(text[i]) : '?');
    return out;
}

CryptoResult<std::int64_t> smallInteger(const Element& e) {
    if (e.tag != kInteger || e.content.empty() || e.content.size() > 8) return bad("bad INTEGER");
    if (e.content[0] & 0x80) return bad("negative INTEGER where a count is expected");
    std::int64_t v = 0;
    for (std::uint8_t b : e.content) v = (v << 8) | b;
    if (v < 0) return bad("INTEGER out of range");
    return v;
}

CryptoResult<std::string> oidOf(const Element& e) {
    if (e.tag != kOid) return bad("expected an OBJECT IDENTIFIER");
    return decodeOid(e.content);
}

// GeneralizedTime with optional fractional seconds -> time point.
CryptoResult<std::chrono::system_clock::time_point> parseGenTime(const Element& e) {
    if (e.tag != kGeneralizedTime) return bad("genTime is not a GeneralizedTime");
    const std::string_view text(reinterpret_cast<const char*>(e.content.data()), e.content.size());
    if (text.size() < 15 || text.back() != 'Z') return bad("unsupported genTime format");
    std::string whole(text.substr(0, 14));
    std::string_view rest = text.substr(14, text.size() - 15); // between the seconds and 'Z'
    std::int64_t nanos = 0;
    if (!rest.empty()) {
        if (rest.front() != '.' || rest.size() < 2 || rest.size() > 10) return bad("bad genTime fraction");
        std::int64_t scale = 100'000'000;
        for (char c : rest.substr(1)) {
            if (c < '0' || c > '9') return bad("bad genTime fraction");
            nanos += (c - '0') * scale;
            scale /= 10;
        }
    }
    whole.push_back('Z');
    Element synthetic;
    synthetic.tag = kGeneralizedTime;
    synthetic.content = ByteView(reinterpret_cast<const std::uint8_t*>(whole.data()), whole.size());
    auto base = decodeTime(synthetic);
    if (!base) return std::unexpected(base.error());
    return *base + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nanos));
}

// ---- certificate extension scan (criticality is not in CertificateInfo) ----------

struct EkuInfo {
    bool present = false;
    bool critical = false;
    std::vector<std::string> oids;
};

EkuInfo scanEku(ByteView certDer) {
    EkuInfo eku;
    auto cert = readSingle(certDer);
    if (!cert || cert->tag != kSequence) return eku;
    auto top = children(*cert);
    if (!top || top->empty() || (*top)[0].tag != kSequence) return eku;
    auto tbs = children((*top)[0]);
    if (!tbs) return eku;
    for (const Element& field : *tbs) {
        if (field.tag != contextTag(3, true)) continue;
        auto wrap = children(field);
        if (!wrap || wrap->size() != 1 || (*wrap)[0].tag != kSequence) return eku;
        auto exts = children((*wrap)[0]);
        if (!exts) return eku;
        for (const Element& ext : *exts) {
            auto parts = children(ext);
            if (!parts || parts->size() < 2 || parts->size() > 3) continue;
            auto id = oidOf((*parts)[0]);
            if (!id || *id != kEkuExtension) continue;
            eku.present = true;
            std::size_t i = 1;
            if ((*parts)[i].tag == kBoolean) {
                eku.critical = (*parts)[i].content.size() == 1 && (*parts)[i].content[0] != 0;
                ++i;
            }
            if (i >= parts->size() || (*parts)[i].tag != kOctetString) {
                eku.oids.push_back("?malformed");
                return eku;
            }
            auto inner = readSingle((*parts)[i].content);
            auto list = inner && inner->tag == kSequence ? children(*inner) : std::unexpected(CryptoError{});
            if (!list) {
                eku.oids.push_back("?malformed");
                return eku;
            }
            for (const Element& o : *list) {
                auto dotted = oidOf(o);
                eku.oids.push_back(dotted ? *dotted : std::string("?malformed"));
            }
            return eku;
        }
    }
    return eku;
}

// ---- ESS signing-certificate binding ------------------------------------------

enum class Ess { Absent, Match, Mismatch };

Ess checkEss(const CmsSignerInfo& si, CryptoProvider& provider, const CertificateInfo& cert) {
    const CmsAttribute* v2 = si.findSigned(oid::kSigningCertificateV2);
    const CmsAttribute* v1 = si.findSigned(oid::kSigningCertificateV1);
    const CmsAttribute* attr = v2 ? v2 : v1;
    if (!attr) return Ess::Absent;
    if (attr->values.size() != 1) return Ess::Mismatch;
    auto top = readSingle(attr->values[0]);
    if (!top || top->tag != kSequence) return Ess::Mismatch;
    auto kids = children(*top);
    if (!kids || kids->empty() || (*kids)[0].tag != kSequence) return Ess::Mismatch;
    auto ids = children((*kids)[0]);
    if (!ids || ids->empty() || (*ids)[0].tag != kSequence) return Ess::Mismatch;
    auto fields = children((*ids)[0]);
    if (!fields || fields->empty()) return Ess::Mismatch;

    DigestAlgorithm alg = v2 ? DigestAlgorithm::Sha256 : DigestAlgorithm::Sha1; // the DEFAULT of each version
    std::size_t idx = 0;
    if (v2 && (*fields)[0].tag == kSequence) {
        auto algKids = children((*fields)[0]);
        if (!algKids || algKids->empty()) return Ess::Mismatch;
        auto o = oidOf((*algKids)[0]);
        auto d = o ? digestFromOid(*o) : std::nullopt;
        if (!d) return Ess::Mismatch;
        alg = *d;
        idx = 1;
    }
    if (idx >= fields->size() || (*fields)[idx].tag != kOctetString) return Ess::Mismatch;
    auto hash = provider.digest(alg, cert.der);
    return hash && *hash == toBytes((*fields)[idx].content) ? Ess::Match : Ess::Mismatch;
}

// Parses ContentInfo + TSTInfo of a token (no cryptography).
struct ParsedToken {
    CmsSignedData cms;
    TstInfo info;
};

CryptoResult<ParsedToken> parseToken(ByteView tokenDer) {
    auto cms = parseCms(tokenDer);
    if (!cms) return std::unexpected(cms.error());
    if (cms->contentType != oid::kTstInfo) return bad("the token does not encapsulate a TSTInfo");
    if (!cms->content) return bad("the token carries no TSTInfo");
    if (cms->signers.size() != 1) return bad("a TimeStampToken must have exactly one signer");
    auto info = parseTstInfo(*cms->content);
    if (!info) return std::unexpected(info.error());
    return ParsedToken{std::move(*cms), std::move(*info)};
}

} // namespace

std::string_view toString(TimestampFailure f) {
    switch (f) {
    case TimestampFailure::None: return "none";
    case TimestampFailure::MalformedToken: return "malformed token";
    case TimestampFailure::BadStatus: return "bad status";
    case TimestampFailure::ImprintMismatch: return "imprint mismatch";
    case TimestampFailure::NonceMismatch: return "nonce mismatch";
    case TimestampFailure::PolicyMismatch: return "policy mismatch";
    case TimestampFailure::UnsupportedAlgorithm: return "unsupported algorithm";
    case TimestampFailure::SignatureInvalid: return "signature invalid";
    case TimestampFailure::NotATimestampCert: return "not a time-stamping certificate";
    case TimestampFailure::UntrustedTsa: return "untrusted TSA";
    case TimestampFailure::GenTimeOutsideCertValidity: return "genTime outside certificate validity";
    }
    return "?";
}

std::vector<std::string> failInfoNames(std::uint32_t failInfo) {
    struct Named {
        std::uint32_t bit;
        const char* name;
    };
    static constexpr Named kNames[] = {
        {1u << 0, "badAlg"},
        {1u << 2, "badRequest"},
        {1u << 5, "badDataFormat"},
        {1u << 14, "timeNotAvailable"},
        {1u << 15, "unacceptedPolicy"},
        {1u << 16, "unacceptedExtension"},
        {1u << 17, "addInfoNotAvailable"},
        {1u << 25, "systemFailure"},
    };
    std::vector<std::string> out;
    std::uint32_t known = 0;
    for (const Named& n : kNames) {
        known |= n.bit;
        if (failInfo & n.bit) out.emplace_back(n.name);
    }
    if (failInfo & ~known) out.emplace_back("other");
    return out;
}

// ---- parsing ----------------------------------------------------------------------

CryptoResult<TimeStampResponse> parseTimeStampResponse(ByteView der) {
    auto top = readSingle(der);
    if (!top) return std::unexpected(top.error());
    if (top->tag != kSequence) return bad("TimeStampResp is not a SEQUENCE");
    auto kids = children(*top);
    if (!kids) return std::unexpected(kids.error());
    if (kids->empty() || kids->size() > 2) return bad("bad TimeStampResp");

    TimeStampResponse resp;
    const Element& st = (*kids)[0];
    if (st.tag != kSequence) return bad("PKIStatusInfo is not a SEQUENCE");
    auto parts = children(st);
    if (!parts) return std::unexpected(parts.error());
    if (parts->empty()) return bad("empty PKIStatusInfo");
    auto value = smallInteger((*parts)[0]);
    if (!value) return std::unexpected(value.error());
    resp.status.statusValue = static_cast<int>(std::min<std::int64_t>(*value, 1'000'000));
    resp.status.status = *value <= 5 ? static_cast<PkiStatus>(*value) : PkiStatus::Unknown;

    std::size_t i = 1;
    if (i < parts->size() && (*parts)[i].tag == kSequence) {
        auto strings = children((*parts)[i]);
        if (!strings) return std::unexpected(strings.error());
        for (const Element& s : *strings) {
            if (s.tag != kUtf8String && s.tag != 0x13 /*Printable*/ && s.tag != 0x16 /*IA5*/ && s.tag != 0x1E /*BMP*/)
                return bad("bad statusString entry");
            if (resp.status.text.size() < kMaxStatusStrings) resp.status.text.push_back(sanitize(s.content));
        }
        ++i;
    }
    if (i < parts->size() && (*parts)[i].tag == kBitString) {
        const ByteView bits = (*parts)[i].content;
        if (bits.empty() || bits[0] > 7) return bad("bad failInfo BIT STRING");
        std::uint32_t mask = 0;
        for (std::size_t byte = 1; byte < bits.size() && byte <= 4; ++byte)
            for (int b = 0; b < 8; ++b)
                if (bits[byte] & (0x80 >> b)) mask |= 1u << ((byte - 1) * 8 + static_cast<std::size_t>(b));
        resp.status.failInfo = mask;
        resp.status.hasFailInfo = true;
        ++i;
    }
    if (i != parts->size()) return bad("unexpected data in PKIStatusInfo");

    if (kids->size() == 2) {
        if ((*kids)[1].tag != kSequence) return bad("timeStampToken is not a ContentInfo");
        resp.tokenDer = toBytes((*kids)[1].full);
    }
    return resp;
}

CryptoResult<TstInfo> parseTstInfo(ByteView derBytes) {
    auto top = readSingle(derBytes);
    if (!top) return std::unexpected(top.error());
    if (top->tag != kSequence) return bad("TSTInfo is not a SEQUENCE");
    auto kidsR = children(*top);
    if (!kidsR) return std::unexpected(kidsR.error());
    const std::vector<Element>& k = *kidsR;
    if (k.size() < 5) return bad("TSTInfo is too short");

    TstInfo info;
    auto version = smallInteger(k[0]);
    if (!version) return std::unexpected(version.error());
    if (*version != 1) return bad("unsupported TSTInfo version");
    info.version = 1;

    auto policy = oidOf(k[1]);
    if (!policy) return std::unexpected(policy.error());
    info.policy = std::move(*policy);

    if (k[2].tag != kSequence) return bad("bad MessageImprint");
    auto imprint = children(k[2]);
    if (!imprint) return std::unexpected(imprint.error());
    if (imprint->size() != 2 || (*imprint)[0].tag != kSequence || (*imprint)[1].tag != kOctetString)
        return bad("bad MessageImprint");
    auto algKids = children((*imprint)[0]);
    if (!algKids || algKids->empty()) return bad("bad MessageImprint algorithm");
    auto algOid = oidOf((*algKids)[0]);
    if (!algOid) return std::unexpected(algOid.error());
    info.imprintAlgorithmOid = *algOid;
    info.imprintAlgorithm = digestFromOid(*algOid);
    info.imprintDigest = toBytes((*imprint)[1].content);
    if (info.imprintAlgorithm && info.imprintDigest.size() != digestSize(*info.imprintAlgorithm))
        return bad("message imprint length does not match its hash algorithm");

    if (k[3].tag != kInteger || k[3].content.empty() || k[3].content.size() > kMaxIntegerBytes + 1)
        return bad("bad serialNumber");
    info.serial = normalizeInteger(k[3].content);
    info.serialHex = hexOf(info.serial);

    auto gen = parseGenTime(k[4]);
    if (!gen) return std::unexpected(gen.error());
    info.genTime = *gen;

    std::size_t i = 5;
    if (i < k.size() && k[i].tag == kSequence) {
        auto acc = children(k[i]);
        if (!acc) return std::unexpected(acc.error());
        TstAccuracy a;
        for (const Element& e : *acc) {
            Element asInt = e;
            if (e.tag == 0x80 || e.tag == 0x81) asInt.tag = kInteger;
            auto v = smallInteger(asInt);
            if (!v) return std::unexpected(v.error());
            if (e.tag == kInteger) a.seconds = *v;
            else if (e.tag == 0x80) a.millis = *v;
            else if (e.tag == 0x81) a.micros = *v;
            else return bad("bad Accuracy field");
        }
        info.accuracy = a;
        ++i;
    }
    if (i < k.size() && k[i].tag == kBoolean) {
        if (k[i].content.size() != 1) return bad("bad ordering BOOLEAN");
        info.ordering = k[i].content[0] != 0;
        ++i;
    }
    if (i < k.size() && k[i].tag == kInteger) {
        if (k[i].content.empty() || k[i].content.size() > kMaxIntegerBytes + 1) return bad("bad nonce");
        info.nonce = normalizeInteger(k[i].content);
        ++i;
    }
    if (i < k.size() && k[i].tag == contextTag(0, true)) {
        auto gn = children(k[i]);
        if (!gn) return std::unexpected(gn.error());
        if (gn->size() != 1) return bad("bad tsa GeneralName");
        if ((*gn)[0].tag == contextTag(4, true)) {
            auto name = children((*gn)[0]);
            if (!name || name->size() != 1 || (*name)[0].tag != kSequence) return bad("bad tsa directoryName");
            info.tsaDirectoryNameDer = toBytes((*name)[0].full);
        }
        ++i;
    }
    if (i < k.size() && k[i].tag == contextTag(1, true)) ++i; // extensions: not interpreted
    if (i != k.size()) return bad("unexpected data in TSTInfo");
    return info;
}

// ---- verification -------------------------------------------------------------------

CryptoResult<TimeStampVerification> verifyTimeStampToken(ByteView tokenDer, CryptoProvider& provider,
                                                         const TimeStampExpectation& expected,
                                                         const TimeStampVerifyOptions& options) {
    if (!provider.isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");
    if (options.cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");

    TimeStampVerification r;
    r.tokenDer = toBytes(tokenDer);
    auto reject = [&](TimestampFailure f, std::string note) -> CryptoResult<TimeStampVerification> {
        r.failure = f;
        r.validation.timestamp = f == TimestampFailure::UntrustedTsa ? TimestampStatus::Untrusted : TimestampStatus::Invalid;
        r.validation.notes.push_back(std::move(note));
        r.validation.state = foldState(r.validation);
        return std::move(r);
    };

    auto parsed = parseToken(tokenDer);
    if (!parsed) {
        return reject(TimestampFailure::MalformedToken, "malformed time-stamp token: " + parsed.error().message);
    }
    const TstInfo& info = parsed->info;
    r.info = info;

    if (!info.imprintAlgorithm || *info.imprintAlgorithm != expected.algorithm || info.imprintDigest != expected.digest)
        return reject(TimestampFailure::ImprintMismatch, "the token's message imprint is not the one that was requested");
    if (expected.nonce && (!info.nonce || *info.nonce != *expected.nonce))
        return reject(TimestampFailure::NonceMismatch, info.nonce ? "the token's nonce differs from the request's"
                                                                  : "the token carries no nonce although one was sent");
    if (expected.policy && info.policy != *expected.policy)
        return reject(TimestampFailure::PolicyMismatch, "the token's policy is not the requested one");

    CmsVerifyOptions cmsOptions;
    cmsOptions.anchors = options.anchors;
    cmsOptions.timestampAnchors = options.anchors;
    cmsOptions.validationTime = info.genTime; // the chain is judged at the time the TSA says it signed
    cmsOptions.extraCertificates = options.extraCertificates;
    cmsOptions.cancel = options.cancel;
    auto verified = verifyCms(parsed->cms, provider, nullptr, cmsOptions);
    if (!verified) return std::unexpected(verified.error());
    const SignerVerification& sv = verified->front();
    r.tsaCertificate = sv.signer;
    r.validation = sv.validation;
    r.validation.timestamp = TimestampStatus::None;

    if (sv.validation.crypto == CryptoCheck::Unsupported)
        return reject(TimestampFailure::UnsupportedAlgorithm, "the token's signature algorithm is not supported");
    if (sv.validation.crypto != CryptoCheck::Valid || sv.validation.integrity != IntegrityStatus::Intact || !sv.signer)
        return reject(TimestampFailure::SignatureInvalid, "the token's signature does not verify");

    const CertificateInfo& tsa = *sv.signer;
    switch (checkEss(parsed->cms.signers.front(), provider, tsa)) {
    case Ess::Absent:
        return reject(TimestampFailure::MalformedToken, "the token has no ESS signing-certificate attribute");
    case Ess::Mismatch:
        return reject(TimestampFailure::SignatureInvalid, "the ESS signing-certificate hash does not match the TSA certificate");
    case Ess::Match: break;
    }

    // RFC 3161 section 2.3: the TSA certificate must carry id-kp-timeStamping
    // as its only extended key usage, and the extension must be critical.
    const EkuInfo eku = scanEku(tsa.der);
    if (!eku.present || eku.oids.size() != 1 || eku.oids[0] != kIdKpTimeStamping)
        return reject(TimestampFailure::NotATimestampCert, "the TSA certificate's extended key usage is not exactly id-kp-timeStamping");
    if (!eku.critical)
        return reject(TimestampFailure::NotATimestampCert, "the TSA certificate's extended key usage is not marked critical");
    if (sv.validation.certificate == CertificateStatus::UsageInvalid)
        return reject(TimestampFailure::NotATimestampCert, "the TSA certificate's key usage does not allow signing");

    if (!tsa.validAt(info.genTime) || sv.validation.certificate == CertificateStatus::Expired ||
        sv.validation.certificate == CertificateStatus::NotYetValid)
        return reject(TimestampFailure::GenTimeOutsideCertValidity, "genTime lies outside the TSA certificate chain's validity period");

    if (sv.validation.trust != TrustStatus::Trusted)
        return reject(TimestampFailure::UntrustedTsa, "the TSA certificate does not chain to a trusted anchor");

    if (info.tsaDirectoryNameDer && *info.tsaDirectoryNameDer != tsa.subjectDer)
        r.validation.notes.push_back("the TSTInfo tsa name differs from the TSA certificate subject (informational)");
    r.validation.timestamp = TimestampStatus::Valid;
    r.validation.state = foldState(r.validation);
    return r;
}

CryptoResult<TimeStampVerification> verifyTimeStampResponse(ByteView responseDer, CryptoProvider& provider,
                                                            const TimeStampRequest& request,
                                                            const TimeStampVerifyOptions& options) {
    auto resp = parseTimeStampResponse(responseDer);
    if (!resp) {
        TimeStampVerification r;
        r.failure = TimestampFailure::MalformedToken;
        r.validation.timestamp = TimestampStatus::Invalid;
        r.validation.notes.push_back("malformed time-stamp response: " + resp.error().message);
        r.validation.state = foldState(r.validation);
        return r;
    }
    auto statusFailure = [&](TimestampFailure f, std::string note) {
        TimeStampVerification r;
        r.failure = f;
        r.validation.timestamp = TimestampStatus::Invalid;
        r.validation.notes.push_back(std::move(note));
        r.validation.state = foldState(r.validation);
        return r;
    };
    if (!resp->status.accepted()) {
        TimeStampVerification r = statusFailure(TimestampFailure::BadStatus,
                                                "the TSA refused the request (PKIStatus " + std::to_string(resp->status.statusValue) + ")");
        for (const std::string& t : resp->status.text) r.validation.notes.push_back("TSA status text: " + t);
        for (const std::string& n : failInfoNames(resp->status.failInfo)) r.validation.notes.push_back("failInfo: " + n);
        return r;
    }
    if (!resp->tokenDer) return statusFailure(TimestampFailure::MalformedToken, "the TSA granted the request but returned no token");

    TimeStampExpectation expected;
    expected.algorithm = request.algorithm;
    expected.digest = request.digest;
    expected.nonce = request.nonce;
    expected.policy = request.policyOid;
    return verifyTimeStampToken(*resp->tokenDer, provider, expected, options);
}

// ---- CMS signature time-stamp -------------------------------------------------------------

CryptoResult<Bytes> signatureTimestampImprint(CryptoProvider& provider, const CmsSignerInfo& signer,
                                              DigestAlgorithm algorithm) {
    if (auto ok = checkCreationAllowed(algorithm); !ok) return std::unexpected(ok.error());
    return provider.digest(algorithm, signer.signature);
}

CryptoOk attachSignatureTimestamp(CmsSignedData& model, std::size_t signerIndex, ByteView tokenDer,
                                  CryptoProvider& provider) {
    if (signerIndex >= model.signers.size()) return fail(CryptoErrorCode::InvalidArgument, "signer index out of range");
    CmsSignerInfo& signer = model.signers[signerIndex];
    if (signer.findUnsigned(oid::kSignatureTimeStampToken))
        return fail(CryptoErrorCode::InvalidArgument, "the signer already carries a signature time-stamp");
    auto parsed = parseToken(tokenDer);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->info.imprintAlgorithm)
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "the token's imprint algorithm is unknown");
    auto imprint = provider.digest(*parsed->info.imprintAlgorithm, signer.signature);
    if (!imprint) return std::unexpected(imprint.error());
    if (*imprint != parsed->info.imprintDigest)
        return fail(CryptoErrorCode::InvalidArgument, "the token's imprint does not match this signer's signature value");

    CmsAttribute attr;
    attr.oid = std::string(oid::kSignatureTimeStampToken);
    attr.values.push_back(toBytes(tokenDer));
    attr.der = sequence({oidElement(oid::kSignatureTimeStampToken), tlv(kSet, tokenDer)});
    signer.unsignedAttrs.push_back(std::move(attr));
    return {};
}

CryptoResult<Bytes> attachSignatureTimestampDer(ByteView cmsDer, std::size_t signerIndex, ByteView tokenDer,
                                                CryptoProvider& provider) {
    auto model = parseCms(cmsDer);
    if (!model) return std::unexpected(model.error());
    if (auto ok = attachSignatureTimestamp(*model, signerIndex, tokenDer, provider); !ok) return std::unexpected(ok.error());
    return encodeCms(*model);
}

CryptoResult<SignatureTimestampInfo> verifySignatureTimestamp(const CmsSignerInfo& signer, CryptoProvider& provider,
                                                              const TrustAnchorSet* anchors,
                                                              const std::vector<Bytes>& extraCertificates,
                                                              const CancelToken& cancel) {
    const CmsAttribute* attr = signer.findUnsigned(oid::kSignatureTimeStampToken);
    if (!attr) return fail(CryptoErrorCode::InvalidArgument, "the signer has no signature time-stamp");

    SignatureTimestampInfo out;
    out.status = TimestampStatus::PresentUnverified;
    auto invalid = [&](TimestampFailure f, std::string note) {
        out.failure = f;
        out.status = TimestampStatus::Invalid;
        out.notes.push_back(std::move(note));
        return out;
    };
    if (attr->values.size() != 1) return invalid(TimestampFailure::MalformedToken, "the time-stamp attribute must hold exactly one token");
    const Bytes& token = attr->values[0];

    auto parsed = parseToken(token);
    if (!parsed) return invalid(TimestampFailure::MalformedToken, "malformed signature time-stamp: " + parsed.error().message);
    out.genTime = parsed->info.genTime;
    out.serialHex = parsed->info.serialHex;
    out.policy = parsed->info.policy;

    const auto alg = parsed->info.imprintAlgorithm;
    if (!alg || !provider.supports(*alg)) {
        out.failure = TimestampFailure::UnsupportedAlgorithm;
        out.notes.push_back("the time-stamp imprint algorithm cannot be verified");
        return out; // stays PresentUnverified
    }
    auto digest = provider.digest(*alg, signer.signature);
    if (!digest) return std::unexpected(digest.error());

    TimeStampExpectation expected;
    expected.algorithm = *alg;
    expected.digest = std::move(*digest);
    TimeStampVerifyOptions vo;
    vo.anchors = anchors;
    vo.extraCertificates = extraCertificates;
    vo.cancel = cancel;
    auto verification = verifyTimeStampToken(token, provider, expected, vo);
    if (!verification) return std::unexpected(verification.error());

    out.failure = verification->failure;
    if (verification->tsaCertificate) out.tsaSubject = verification->tsaCertificate->subject;
    out.notes = verification->validation.notes;
    if (verification->ok()) {
        out.status = TimestampStatus::Valid;
        out.verified = true;
    } else if (verification->failure == TimestampFailure::UnsupportedAlgorithm) {
        out.status = TimestampStatus::PresentUnverified;
    } else {
        out.status = verification->validation.timestamp == TimestampStatus::Untrusted ? TimestampStatus::Untrusted
                                                                                       : TimestampStatus::Invalid;
    }
    return out;
}

} // namespace rivet::crypto
