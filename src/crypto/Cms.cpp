// SPDX-License-Identifier: MPL-2.0
#include "crypto/Cms.hpp"

#include "crypto/Der.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace rivet::crypto {

using namespace der;

namespace {

constexpr std::size_t kMaxSigners = 1024;
constexpr std::size_t kMaxCertificates = 4096;

std::unexpected<CryptoError> bad(std::string message) { return fail(CryptoErrorCode::MalformedCms, std::move(message)); }

CryptoResult<std::string> oidValue(const Element& e) {
    if (e.tag != kOid) return bad("expected OBJECT IDENTIFIER");
    return decodeOid(e.content);
}

struct AlgId {
    std::string oid;
    std::optional<Element> params;
};

CryptoResult<AlgId> parseAlgId(const Element& e) {
    if (e.tag != kSequence) return bad("AlgorithmIdentifier is not a SEQUENCE");
    auto kids = children(e);
    if (!kids) return std::unexpected(kids.error());
    if (kids->empty() || kids->size() > 2) return bad("malformed AlgorithmIdentifier");
    auto o = oidValue((*kids)[0]);
    if (!o) return std::unexpected(o.error());
    AlgId id;
    id.oid = *o;
    if (kids->size() == 2) id.params = (*kids)[1];
    return id;
}

Bytes toBytes(ByteView v) { return Bytes(v.begin(), v.end()); }

CryptoResult<CmsAttribute> parseAttribute(const Element& e) {
    if (e.tag != kSequence) return bad("Attribute is not a SEQUENCE");
    auto kids = children(e);
    if (!kids) return std::unexpected(kids.error());
    if (kids->size() != 2) return bad("malformed Attribute");
    auto o = oidValue((*kids)[0]);
    if (!o) return std::unexpected(o.error());
    if ((*kids)[1].tag != kSet) return bad("Attribute values are not a SET");
    auto vals = children((*kids)[1]);
    if (!vals) return std::unexpected(vals.error());
    if (vals->empty()) return bad("Attribute without values");
    CmsAttribute attr;
    attr.oid = *o;
    attr.der = toBytes(e.full);
    for (const Element& v : *vals) attr.values.push_back(toBytes(v.full));
    return attr;
}

CryptoResult<std::vector<CmsAttribute>> parseAttributes(const Element& container, bool rejectDuplicates) {
    auto kids = children(container);
    if (!kids) return std::unexpected(kids.error());
    std::vector<CmsAttribute> out;
    std::set<std::string> seen;
    for (const Element& k : *kids) {
        auto a = parseAttribute(k);
        if (!a) return std::unexpected(a.error());
        if (rejectDuplicates && !seen.insert(a->oid).second) return bad("duplicate signed attribute");
        out.push_back(std::move(*a));
    }
    return out;
}

CryptoResult<CmsSignerInfo> parseSignerInfo(const Element& e) {
    if (e.tag != kSequence) return bad("SignerInfo is not a SEQUENCE");
    auto kidsR = children(e);
    if (!kidsR) return std::unexpected(kidsR.error());
    const std::vector<Element>& k = *kidsR;
    if (k.size() < 5) return bad("SignerInfo is too short");

    CmsSignerInfo si;
    std::size_t i = 0;
    if (k[i].tag != kInteger || k[i].content.size() != 1) return bad("bad SignerInfo version");
    si.version = k[i].content[0];
    ++i;

    if (k[i].tag == kSequence) {
        auto parts = children(k[i]);
        if (!parts) return std::unexpected(parts.error());
        if (parts->size() != 2 || (*parts)[0].tag != kSequence || (*parts)[1].tag != kInteger)
            return bad("bad IssuerAndSerialNumber");
        si.issuerDer = toBytes((*parts)[0].full);
        si.serial = normalizeInteger((*parts)[1].content);
    } else if (k[i].tag == contextTag(0, false)) {
        si.subjectKeyId = toBytes(k[i].content);
    } else {
        return bad("unsupported SignerIdentifier");
    }
    si.sidDer = toBytes(k[i].full);
    ++i;

    auto dig = parseAlgId(k[i]);
    if (!dig) return std::unexpected(dig.error());
    si.digestOid = dig->oid;
    si.digestAlgDer = toBytes(k[i].full);
    ++i;

    if (k[i].tag == contextTag(0, true)) {
        if (k[i].indefinite) return bad("signed attributes must use definite DER lengths");
        auto attrs = parseAttributes(k[i], true);
        if (!attrs) return std::unexpected(attrs.error());
        si.signedAttrs = std::move(*attrs);
        si.signedAttrsDer = toBytes(k[i].full);
        ++i;
    }
    if (i + 2 > k.size()) return bad("SignerInfo is missing its signature");

    auto sigAlg = parseAlgId(k[i]);
    if (!sigAlg) return std::unexpected(sigAlg.error());
    si.signatureAlgOid = sigAlg->oid;
    si.signatureAlgDer = toBytes(k[i].full);
    ++i;

    auto sig = octetStringValue(k[i]);
    if (!sig) return std::unexpected(sig.error());
    si.signature = std::move(*sig);
    ++i;

    if (i < k.size() && k[i].tag == contextTag(1, true)) {
        auto attrs = parseAttributes(k[i], false);
        if (!attrs) return std::unexpected(attrs.error());
        si.unsignedAttrs = std::move(*attrs);
        ++i;
    }
    if (i != k.size()) return bad("unexpected data in SignerInfo");
    return si;
}

// ---- encoding -------------------------------------------------------------

Bytes encodeSignerInfo(const CmsSignerInfo& s) {
    const bool ski = !s.subjectKeyId.empty() && s.issuerDer.empty();
    std::vector<Bytes> parts;
    parts.push_back(integerFromInt(ski ? 3 : 1));
    parts.push_back(s.sidDer);
    parts.push_back(s.digestAlgDer);
    if (s.hasSignedAttrs()) parts.push_back(s.signedAttrsDer);
    parts.push_back(s.signatureAlgDer);
    parts.push_back(octetString(s.signature));
    if (!s.unsignedAttrs.empty()) {
        std::vector<Bytes> ders;
        for (const CmsAttribute& a : s.unsignedAttrs) ders.push_back(a.der);
        std::sort(ders.begin(), ders.end());
        parts.push_back(tlv(contextTag(1, true), concat(ders)));
    }
    return sequence(parts);
}

Bytes attribute(std::string_view oidStr, const Bytes& value) {
    return sequence({oidElement(oidStr), tlv(kSet, value)});
}

CmsAttribute makeAttribute(std::string_view oidStr, Bytes value) {
    CmsAttribute a;
    a.oid = std::string(oidStr);
    a.der = attribute(oidStr, value);
    a.values.push_back(std::move(value));
    return a;
}

Bytes digestAlgId(DigestAlgorithm d) { return sequence({oidElement(digestOid(d))}); }

Bytes pssParams(DigestAlgorithm d) {
    const Bytes hashAlg = sequence({oidElement(digestOid(d)), nullValue()});
    const Bytes mgf = sequence({oidElement(oid::kMgf1), hashAlg});
    return sequence({tlv(contextTag(0, true), hashAlg), tlv(contextTag(1, true), mgf),
                     tlv(contextTag(2, true), integerFromInt(static_cast<std::int64_t>(digestSize(d))))});
}

Bytes signatureAlgId(SignatureAlgorithm a) {
    switch (a) {
    case SignatureAlgorithm::RsaPkcs1Sha256: return sequence({oidElement(oid::kSha256WithRsa), nullValue()});
    case SignatureAlgorithm::RsaPkcs1Sha384: return sequence({oidElement(oid::kSha384WithRsa), nullValue()});
    case SignatureAlgorithm::RsaPkcs1Sha512: return sequence({oidElement(oid::kSha512WithRsa), nullValue()});
    case SignatureAlgorithm::RsaPssSha256:
    case SignatureAlgorithm::RsaPssSha384:
    case SignatureAlgorithm::RsaPssSha512:
        return sequence({oidElement(oid::kRsaPss), pssParams(digestOf(a))});
    case SignatureAlgorithm::EcdsaSha256: return sequence({oidElement(oid::kEcdsaSha256)});
    case SignatureAlgorithm::EcdsaSha384: return sequence({oidElement(oid::kEcdsaSha384)});
    case SignatureAlgorithm::EcdsaSha512: return sequence({oidElement(oid::kEcdsaSha512)});
    case SignatureAlgorithm::Gost3410_2012_256: return sequence({oidElement(oid::kGost3410_256)});
    case SignatureAlgorithm::Gost3410_2012_512: return sequence({oidElement(oid::kGost3410_512)});
    }
    return {};
}

// ---- scheme resolution --------------------------------------------------------

CryptoResult<SignatureScheme> resolveScheme(const CmsSignerInfo& si, DigestAlgorithm signerDigest) {
    auto el = readSingle(si.signatureAlgDer);
    if (!el) return std::unexpected(el.error());
    auto alg = parseAlgId(*el);
    if (!alg) return std::unexpected(alg.error());

    SignatureScheme s;
    s.digest = signerDigest;
    const std::string& o = alg->oid;
    auto unsupported = [&]() -> std::unexpected<CryptoError> {
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "unsupported signature algorithm " + o);
    };

    if (o == oid::kRsaEncryption) {
        s.keyType = PublicKeyType::Rsa;
        s.padding = SignaturePadding::Pkcs1;
    } else if (o == oid::kSha1WithRsa || o == oid::kSha256WithRsa || o == oid::kSha384WithRsa ||
               o == oid::kSha512WithRsa) {
        s.keyType = PublicKeyType::Rsa;
        s.padding = SignaturePadding::Pkcs1;
        s.digest = o == oid::kSha1WithRsa     ? DigestAlgorithm::Sha1
                   : o == oid::kSha256WithRsa ? DigestAlgorithm::Sha256
                   : o == oid::kSha384WithRsa ? DigestAlgorithm::Sha384
                                              : DigestAlgorithm::Sha512;
    } else if (o == oid::kRsaPss) {
        s.keyType = PublicKeyType::Rsa;
        s.padding = SignaturePadding::Pss;
        DigestAlgorithm hash = DigestAlgorithm::Sha1; // RFC 4055 defaults
        DigestAlgorithm mgfHash = DigestAlgorithm::Sha1;
        std::int64_t salt = 20;
        if (alg->params && alg->params->tag == kSequence) {
            auto fields = children(*alg->params);
            if (!fields) return std::unexpected(fields.error());
            for (const Element& f : *fields) {
                auto inner = children(f);
                if (!inner || inner->empty()) return bad("malformed RSASSA-PSS parameters");
                if (f.tag == contextTag(0, true)) {
                    auto h = parseAlgId((*inner)[0]);
                    if (!h) return std::unexpected(h.error());
                    auto d = digestFromOid(h->oid);
                    if (!d) return unsupported();
                    hash = *d;
                } else if (f.tag == contextTag(1, true)) {
                    auto m = parseAlgId((*inner)[0]);
                    if (!m || m->oid != oid::kMgf1 || !m->params) return unsupported();
                    auto h = parseAlgId(*m->params);
                    if (!h) return std::unexpected(h.error());
                    auto d = digestFromOid(h->oid);
                    if (!d) return unsupported();
                    mgfHash = *d;
                } else if (f.tag == contextTag(2, true)) {
                    const Element& n = (*inner)[0];
                    if (n.tag != kInteger || n.content.size() > 4) return bad("malformed PSS salt length");
                    salt = 0;
                    for (std::uint8_t b : n.content) salt = (salt << 8) | b;
                } else if (f.tag == contextTag(3, true)) {
                    // trailerField must be 1 (0xBC); anything else is unsupported
                    if ((*inner)[0].tag != kInteger || (*inner)[0].content.size() != 1 || (*inner)[0].content[0] != 1)
                        return unsupported();
                } else {
                    return bad("unknown RSASSA-PSS parameter");
                }
            }
        }
        if (mgfHash != hash) return unsupported();
        s.digest = hash;
        s.pssSaltLength = salt == static_cast<std::int64_t>(digestSize(hash)) ? -1 : static_cast<int>(salt);
    } else if (o == oid::kEcPublicKey || o == oid::kEcdsaSha1 || o == oid::kEcdsaSha256 || o == oid::kEcdsaSha384 ||
               o == oid::kEcdsaSha512) {
        s.keyType = PublicKeyType::Ec;
        s.padding = SignaturePadding::Ecdsa;
        if (o == oid::kEcdsaSha1) s.digest = DigestAlgorithm::Sha1;
        else if (o == oid::kEcdsaSha256) s.digest = DigestAlgorithm::Sha256;
        else if (o == oid::kEcdsaSha384) s.digest = DigestAlgorithm::Sha384;
        else if (o == oid::kEcdsaSha512) s.digest = DigestAlgorithm::Sha512;
    } else if (o == oid::kGost3410_256) {
        s.keyType = PublicKeyType::Gost3410_2012_256;
        s.padding = SignaturePadding::Gost;
        s.digest = DigestAlgorithm::Gost3411_2012_256;
    } else if (o == oid::kGost3410_512) {
        s.keyType = PublicKeyType::Gost3410_2012_512;
        s.padding = SignaturePadding::Gost;
        s.digest = DigestAlgorithm::Gost3411_2012_512;
    } else {
        return unsupported();
    }
    return s;
}

const CmsAttribute* findAttr(const std::vector<CmsAttribute>& attrs, std::string_view o) {
    for (const CmsAttribute& a : attrs)
        if (a.oid == o) return &a;
    return nullptr;
}

std::optional<std::chrono::system_clock::time_point> signingTimeOf(const CmsSignerInfo& si) {
    const CmsAttribute* a = findAttr(si.signedAttrs, oid::kSigningTime);
    if (!a || a->values.size() != 1) return std::nullopt;
    auto el = readSingle(a->values[0]);
    if (!el) return std::nullopt;
    auto t = decodeTime(*el);
    if (!t) return std::nullopt;
    return *t;
}

bool matchesSigner(const CmsSignerInfo& si, const CertificateInfo& cert) {
    if (!si.issuerDer.empty()) return si.issuerDer == cert.issuerDer && si.serial == cert.serial;
    return !si.subjectKeyId.empty() && si.subjectKeyId == cert.subjectKeyId;
}

std::vector<CertificateInfo> parseCertificates(CryptoProvider& provider, const std::vector<Bytes>& ders) {
    std::vector<CertificateInfo> out;
    for (const Bytes& d : ders) {
        auto c = provider.parseCertificate(d);
        if (c) out.push_back(std::move(*c));
    }
    return out;
}

// ---- verification of one SignerInfo -----------------------------------------

struct OneResult {
    std::optional<CertificateInfo> signer;
    std::optional<DigestAlgorithm> digest;
    ValidationResult v;
};

CryptoResult<OneResult> verifyOne(const CmsSignerInfo& si, std::string_view eContentType, const ContentSource& content,
                                  const std::vector<CertificateInfo>& pool, CryptoProvider& provider,
                                  const CmsVerifyOptions& opts) {
    static const TrustAnchorSet kNoAnchors;
    OneResult r;
    ValidationResult& v = r.v;
    auto finish = [&]() -> CryptoResult<OneResult> {
        v.state = foldState(v);
        return r;
    };

    const auto digest = digestFromOid(si.digestOid);
    if (!digest || !provider.supports(*digest)) {
        v.crypto = CryptoCheck::Unsupported;
        v.notes.push_back("unsupported digest algorithm " + oidName(si.digestOid));
        return finish();
    }
    r.digest = *digest;

    for (const CertificateInfo& c : pool)
        if (matchesSigner(si, c)) {
            r.signer = c;
            break;
        }
    if (!r.signer) {
        v.notes.push_back("signer certificate not found in the message");
        return finish();
    }
    const CertificateInfo& cert = *r.signer;

    auto scheme = resolveScheme(si, *digest);
    if (!scheme) {
        v.crypto = CryptoCheck::Unsupported;
        v.notes.push_back(scheme.error().message);
        return finish();
    }
    if (scheme->padding == SignaturePadding::Gost || !provider.supports(scheme->digest)) {
        v.crypto = CryptoCheck::Unsupported;
        v.notes.push_back("unsupported signature scheme");
        return finish();
    }
    if (cert.publicKeyType != PublicKeyType::Unknown && scheme->keyType != cert.publicKeyType) {
        v.crypto = CryptoCheck::Invalid;
        v.notes.push_back("signature algorithm does not match the signer's key type");
        return finish();
    }

    auto contentDigest = content.digest(provider, *digest, opts.cancel);
    if (!contentDigest) return std::unexpected(contentDigest.error());

    Bytes message;
    if (si.hasSignedAttrs()) {
        const CmsAttribute* md = findAttr(si.signedAttrs, oid::kMessageDigest);
        Bytes claimed;
        bool mdOk = false;
        if (md && md->values.size() == 1) {
            auto el = readSingle(md->values[0]);
            if (el && el->tag == kOctetString) {
                claimed = toBytes(el->content);
                mdOk = true;
            }
        }
        if (!mdOk) {
            v.crypto = CryptoCheck::Invalid;
            v.notes.push_back("message-digest attribute missing or malformed");
            return finish();
        }
        v.integrity = claimed == *contentDigest ? IntegrityStatus::Intact : IntegrityStatus::Modified;
        if (v.integrity == IntegrityStatus::Modified) v.notes.push_back("content digest does not match message-digest");

        if (!eContentType.empty()) {
            const CmsAttribute* ct = findAttr(si.signedAttrs, oid::kContentType);
            bool ok = false;
            if (ct && ct->values.size() == 1) {
                auto el = readSingle(ct->values[0]);
                if (el && el->tag == kOid) {
                    auto dotted = decodeOid(el->content);
                    ok = dotted && *dotted == eContentType;
                }
            }
            if (!ok) {
                v.crypto = CryptoCheck::Invalid;
                v.notes.push_back("content-type attribute missing or different from the encapsulated type");
                return finish();
            }
        }
        message = si.signedAttrsDer;
        message[0] = kSet; // the signature covers the DER SET OF encoding
    } else {
        auto all = content.read();
        if (!all) return std::unexpected(all.error());
        message = std::move(*all);
        v.integrity = IntegrityStatus::Unknown;
    }

    auto sig = provider.verifySignature(cert, *scheme, message, si.signature);
    if (sig) {
        v.crypto = CryptoCheck::Valid;
        if (!si.hasSignedAttrs()) v.integrity = IntegrityStatus::Intact;
    } else if (sig.error().code == CryptoErrorCode::BackendUnavailable ||
               sig.error().code == CryptoErrorCode::Cancelled) {
        return std::unexpected(sig.error());
    } else if (sig.error().code == CryptoErrorCode::UnsupportedAlgorithm) {
        v.crypto = CryptoCheck::Unsupported;
        v.notes.push_back("signature algorithm not supported by the provider");
    } else {
        v.crypto = CryptoCheck::Invalid;
        v.notes.push_back("signature value does not verify");
        if (!si.hasSignedAttrs()) v.integrity = IntegrityStatus::Modified;
    }

    // CAdES signing-certificate-v2: the signer certificate hash must match.
    if (const CmsAttribute* ess = findAttr(si.signedAttrs, oid::kSigningCertificateV2);
        ess && ess->values.size() == 1 && v.crypto == CryptoCheck::Valid) {
        bool match = false;
        auto top = readSingle(ess->values[0]);
        if (top && top->tag == kSequence) {
            auto kids = children(*top);
            if (kids && !kids->empty() && (*kids)[0].tag == kSequence) {
                auto certs = children((*kids)[0]);
                if (certs && !certs->empty() && (*certs)[0].tag == kSequence) {
                    auto fields = children((*certs)[0]);
                    DigestAlgorithm hashAlg = DigestAlgorithm::Sha256;
                    std::size_t idx = 0;
                    bool algOk = true;
                    if (fields && !fields->empty() && (*fields)[0].tag == kSequence) {
                        auto a = parseAlgId((*fields)[0]);
                        auto d = a ? digestFromOid(a->oid) : std::nullopt;
                        if (d) hashAlg = *d;
                        else algOk = false;
                        idx = 1;
                    }
                    if (fields && algOk && idx < fields->size() && (*fields)[idx].tag == kOctetString) {
                        auto h = provider.digest(hashAlg, cert.der);
                        match = h && *h == toBytes((*fields)[idx].content);
                    }
                }
            }
        }
        if (!match) {
            v.crypto = CryptoCheck::Invalid;
            v.notes.push_back("signing-certificate-v2 hash does not match the signer certificate");
        }
    }

    if (isWeakForVerification(scheme->digest, cert.publicKeyType, cert.keyBits) ||
        isWeakForVerification(*digest, cert.publicKeyType, cert.keyBits)) {
        v.policy = PolicyStatus::WeakLegacyAlgorithm;
        v.notes.push_back("weak legacy algorithm (SHA-1 or RSA below 2048 bits)");
    }

    if (findAttr(si.unsignedAttrs, oid::kSignatureTimeStampToken)) v.timestamp = TimestampStatus::PresentUnverified;

    const CertificateValidation cv = validateCertificate(
        cert, pool, opts.anchors ? *opts.anchors : kNoAnchors, provider, opts.validationTime);
    v.certificate = cv.certificate;
    v.trust = cv.trust;
    if (v.trust != TrustStatus::Trusted) v.notes.push_back("signer certificate does not chain to a trusted anchor");
    if (v.certificate == CertificateStatus::Expired) v.notes.push_back("certificate expired at validation time");
    return finish();
}

} // namespace

// ---- model accessors ----------------------------------------------------------

const CmsAttribute* CmsSignerInfo::findSigned(std::string_view o) const { return findAttr(signedAttrs, o); }
const CmsAttribute* CmsSignerInfo::findUnsigned(std::string_view o) const { return findAttr(unsignedAttrs, o); }

// ---- parse / encode -----------------------------------------------------------

CryptoResult<CmsSignedData> parseCms(ByteView data) {
    auto top = readSingle(data);
    if (!top) return std::unexpected(top.error());
    if (top->tag != kSequence) return bad("ContentInfo is not a SEQUENCE");
    auto ci = children(*top);
    if (!ci) return std::unexpected(ci.error());
    if (ci->size() != 2) return bad("malformed ContentInfo");
    auto type = oidValue((*ci)[0]);
    if (!type) return std::unexpected(type.error());
    if (*type != oid::kSignedData)
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "CMS content type " + oidName(*type) + " is not SignedData");
    if ((*ci)[1].tag != contextTag(0, true)) return bad("ContentInfo content is not [0] EXPLICIT");
    auto inner = children((*ci)[1]);
    if (!inner) return std::unexpected(inner.error());
    if (inner->size() != 1 || (*inner)[0].tag != kSequence) return bad("malformed SignedData wrapper");
    auto sdKids = children((*inner)[0]);
    if (!sdKids) return std::unexpected(sdKids.error());
    const std::vector<Element>& k = *sdKids;
    if (k.size() < 4) return bad("SignedData is too short");

    std::size_t i = 0;
    if (k[i].tag != kInteger || k[i].content.size() != 1) return bad("bad SignedData version");
    ++i;
    if (k[i].tag != kSet) return bad("digestAlgorithms is not a SET");
    {
        auto algs = children(k[i]);
        if (!algs) return std::unexpected(algs.error());
        for (const Element& a : *algs) {
            auto id = parseAlgId(a);
            if (!id) return std::unexpected(id.error());
        }
    }
    ++i;

    CmsSignedData out;
    // EncapsulatedContentInfo
    if (k[i].tag != kSequence) return bad("EncapsulatedContentInfo is not a SEQUENCE");
    {
        auto enc = children(k[i]);
        if (!enc) return std::unexpected(enc.error());
        if (enc->empty() || enc->size() > 2) return bad("malformed EncapsulatedContentInfo");
        auto ct = oidValue((*enc)[0]);
        if (!ct) return std::unexpected(ct.error());
        out.contentType = *ct;
        if (enc->size() == 2) {
            if ((*enc)[1].tag != contextTag(0, true)) return bad("eContent is not [0] EXPLICIT");
            auto oc = children((*enc)[1]);
            if (!oc) return std::unexpected(oc.error());
            if (oc->size() != 1) return bad("malformed eContent");
            if ((*oc)[0].content.size() > kMaxAttachedContent)
                return fail(CryptoErrorCode::LimitExceeded, "attached content too large");
            auto bytes = octetStringValue((*oc)[0]);
            if (!bytes) return std::unexpected(bytes.error());
            if (bytes->size() > kMaxAttachedContent)
                return fail(CryptoErrorCode::LimitExceeded, "attached content too large");
            out.content = std::move(*bytes);
        }
    }
    ++i;

    if (i < k.size() && k[i].tag == contextTag(0, true)) {
        auto certs = children(k[i]);
        if (!certs) return std::unexpected(certs.error());
        if (certs->size() > kMaxCertificates) return fail(CryptoErrorCode::LimitExceeded, "too many certificates");
        for (const Element& c : *certs)
            if (c.tag == kSequence) out.certificates.push_back(toBytes(c.full));
        ++i;
    }
    if (i < k.size() && k[i].tag == contextTag(1, true)) {
        auto crls = children(k[i]);
        if (!crls) return std::unexpected(crls.error());
        for (const Element& c : *crls) out.crls.push_back(toBytes(c.full));
        ++i;
    }
    if (i >= k.size() || k[i].tag != kSet) return bad("SignerInfos is missing");
    {
        auto sis = children(k[i]);
        if (!sis) return std::unexpected(sis.error());
        if (sis->size() > kMaxSigners) return fail(CryptoErrorCode::LimitExceeded, "too many signers");
        for (const Element& s : *sis) {
            auto si = parseSignerInfo(s);
            if (!si) return std::unexpected(si.error());
            out.signers.push_back(std::move(*si));
        }
    }
    ++i;
    if (i != k.size()) return bad("unexpected data after SignerInfos");
    return out;
}

CryptoResult<Bytes> decodePem(ByteView pem) {
    const std::string_view text(reinterpret_cast<const char*>(pem.data()), pem.size());
    const std::size_t begin = text.find("-----BEGIN");
    if (begin == std::string_view::npos) return fail(CryptoErrorCode::MalformedCms, "no PEM header");
    const std::size_t bodyStart = text.find('\n', begin);
    const std::size_t end = text.find("-----END", begin);
    if (bodyStart == std::string_view::npos || end == std::string_view::npos || end < bodyStart)
        return fail(CryptoErrorCode::MalformedCms, "malformed PEM block");
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t p = bodyStart; p < end; ++p) {
        const char c = text[p];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        else return fail(CryptoErrorCode::MalformedCms, "invalid PEM character");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

CryptoResult<CmsSignedData> parseCmsAny(ByteView data) {
    if (data.size() > 10 && data[0] == '-' && data[1] == '-') {
        auto der = decodePem(data);
        if (!der) return std::unexpected(der.error());
        return parseCms(*der);
    }
    return parseCms(data);
}

CryptoResult<Bytes> encodeCms(const CmsSignedData& m) {
    if (m.signers.empty()) return fail(CryptoErrorCode::InvalidArgument, "SignedData without signers");
    std::set<Bytes> algs;
    bool v3 = m.contentType != oid::kData;
    std::vector<Bytes> infos;
    for (const CmsSignerInfo& s : m.signers) {
        algs.insert(s.digestAlgDer);
        if (s.issuerDer.empty()) v3 = true;
        infos.push_back(encodeSignerInfo(s));
    }
    std::vector<Bytes> encap{oidElement(m.contentType)};
    if (m.content) encap.push_back(tlv(contextTag(0, true), octetString(*m.content)));

    std::vector<Bytes> parts;
    parts.push_back(integerFromInt(v3 ? 3 : 1));
    parts.push_back(setOf(std::vector<Bytes>(algs.begin(), algs.end())));
    parts.push_back(sequence(encap));
    if (!m.certificates.empty()) parts.push_back(tlv(contextTag(0, true), concat(m.certificates)));
    if (!m.crls.empty()) parts.push_back(tlv(contextTag(1, true), concat(m.crls)));
    parts.push_back(tlv(kSet, concat(infos)));

    return sequence({oidElement(oid::kSignedData), tlv(contextTag(0, true), sequence(parts))});
}

// ---- content source -----------------------------------------------------------

ContentSource ContentSource::fromBytes(Bytes bytes) {
    ContentSource s;
    s.bytes_ = std::make_shared<const Bytes>(std::move(bytes));
    return s;
}

ContentSource ContentSource::fromFile(std::filesystem::path path) {
    ContentSource s;
    s.path_ = std::move(path);
    return s;
}

CryptoResult<Bytes> ContentSource::digest(CryptoProvider& provider, DigestAlgorithm algorithm,
                                          const CancelToken& cancel) const {
    auto hasher = provider.createHasher(algorithm);
    if (!hasher) return std::unexpected(hasher.error());
    if (bytes_) {
        (*hasher)->update(*bytes_);
        return (*hasher)->finish();
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) return fail(CryptoErrorCode::Io, "cannot open the content file");
    Bytes buffer(std::size_t{1} << 20);
    while (in) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) (*hasher)->update(ByteView(buffer.data(), static_cast<std::size_t>(got)));
    }
    if (in.bad()) return fail(CryptoErrorCode::Io, "error reading the content file");
    return (*hasher)->finish();
}

CryptoResult<Bytes> ContentSource::read() const {
    if (bytes_) return *bytes_;
    std::ifstream in(path_, std::ios::binary | std::ios::ate);
    if (!in) return fail(CryptoErrorCode::Io, "cannot open the content file");
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxAttachedContent)
        return fail(CryptoErrorCode::LimitExceeded, "content too large to load into memory");
    Bytes out(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (!in) return fail(CryptoErrorCode::Io, "error reading the content file");
    return out;
}

// ---- signing ---------------------------------------------------------------------

namespace {

CryptoResult<SignatureAlgorithm> chooseSignature(const SigningIdentity& id, const CmsSignOptions& o) {
    SignatureAlgorithm sa;
    if (o.signature) {
        sa = *o.signature;
    } else {
        if (auto ok = checkCreationAllowed(o.digest); !ok) return std::unexpected(ok.error());
        auto d = defaultSignatureFor(id.keyType(), o.digest);
        if (!d) return fail(CryptoErrorCode::UnsupportedAlgorithm, "no signature algorithm for this key and digest");
        sa = *d;
    }
    if (auto ok = checkCreationAllowed(sa, id.keyType(), id.keyBits()); !ok) return std::unexpected(ok.error());
    const auto supported = id.supportedSignatures();
    if (std::find(supported.begin(), supported.end(), sa) == supported.end())
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "the signing identity does not support this algorithm");
    return sa;
}

CryptoResult<CmsSignerInfo> buildSignerInfo(CryptoProvider& provider, SigningIdentity& identity,
                                            const CmsSignOptions& o, SignatureAlgorithm sigAlg,
                                            std::string_view contentType, const Bytes& contentDigest) {
    const CertificateInfo& cert = identity.certificate();
    const DigestAlgorithm digest = digestOf(sigAlg);

    std::vector<CmsAttribute> attrs;
    if (!contentType.empty()) attrs.push_back(makeAttribute(oid::kContentType, oidElement(contentType)));
    attrs.push_back(makeAttribute(oid::kMessageDigest, octetString(contentDigest)));
    if (o.includeSigningTime || o.cadesBes)
        attrs.push_back(makeAttribute(oid::kSigningTime, timeElement(o.signingTime)));
    if (o.cadesBes) {
        auto certHash = provider.digest(DigestAlgorithm::Sha256, cert.der);
        if (!certHash) return std::unexpected(certHash.error());
        const Bytes issuerSerial =
            sequence({sequence({tlv(contextTag(4, true), cert.issuerDer)}), integer(cert.serial)});
        const Bytes essId = sequence({octetString(*certHash), issuerSerial}); // SHA-256 is the DEFAULT hashAlgorithm
        attrs.push_back(makeAttribute(oid::kSigningCertificateV2, sequence({sequence({essId})})));
    }

    std::vector<Bytes> ders;
    for (const CmsAttribute& a : attrs) ders.push_back(a.der);
    const Bytes attrSet = setOf(ders); // DER SET OF, sorted
    auto attrSetEl = readSingle(attrSet);
    if (!attrSetEl) return std::unexpected(attrSetEl.error());
    const ByteView body = attrSetEl->content;

    CmsSignerInfo si;
    si.version = 1;
    si.issuerDer = cert.issuerDer;
    si.serial = cert.serial;
    si.sidDer = sequence({cert.issuerDer, integer(cert.serial)});
    si.digestOid = std::string(digestOid(digest));
    si.digestAlgDer = digestAlgId(digest);
    si.signedAttrs = std::move(attrs);
    si.signedAttrsDer = tlv(contextTag(0, true), body);
    const Bytes algDer = signatureAlgId(sigAlg);
    si.signatureAlgDer = algDer;
    auto parsedAlg = readSingle(algDer);
    if (parsedAlg) {
        auto id = parseAlgId(*parsedAlg);
        if (id) si.signatureAlgOid = id->oid;
    }

    auto signature = identity.sign(sigAlg, attrSet, o.cancel);
    if (!signature) return std::unexpected(signature.error());
    si.signature = std::move(*signature);

    // Defence in depth: never emit a signature that does not verify.
    if (auto check = provider.verifySignature(cert, schemeOf(sigAlg), attrSet, si.signature); !check)
        return fail(CryptoErrorCode::Internal, "the produced signature failed self-verification");
    return si;
}

void addCertificates(CmsSignedData& m, const SigningIdentity& identity, bool chain) {
    std::vector<Bytes> chainDer = identity.chainDer();
    if (chainDer.empty()) chainDer.push_back(identity.certificate().der);
    if (!chain) chainDer.resize(1);
    for (const Bytes& c : chainDer)
        if (std::find(m.certificates.begin(), m.certificates.end(), c) == m.certificates.end())
            m.certificates.push_back(c);
}

} // namespace

CryptoResult<CmsSignedData> signCms(CryptoProvider& provider, SigningIdentity& identity, const ContentSource& content,
                                    bool detached, const CmsSignOptions& options) {
    auto sigAlg = chooseSignature(identity, options);
    if (!sigAlg) return std::unexpected(sigAlg.error());
    CmsSignedData model;
    model.contentType = std::string(oid::kData);
    if (!detached) {
        auto bytes = content.read();
        if (!bytes) return std::unexpected(bytes.error());
        model.content = std::move(*bytes);
    }
    auto digest = content.digest(provider, digestOf(*sigAlg), options.cancel);
    if (!digest) return std::unexpected(digest.error());
    auto si = buildSignerInfo(provider, identity, options, *sigAlg, model.contentType, *digest);
    if (!si) return std::unexpected(si.error());
    model.signers.push_back(std::move(*si));
    addCertificates(model, identity, options.includeChain);
    return model;
}

CryptoOk addSigner(CmsSignedData& model, CryptoProvider& provider, SigningIdentity& identity,
                   const ContentSource* content, const CmsSignOptions& options) {
    auto sigAlg = chooseSignature(identity, options);
    if (!sigAlg) return std::unexpected(sigAlg.error());
    ContentSource attached = ContentSource::fromBytes(model.content ? *model.content : Bytes{});
    const ContentSource* source = &attached;
    if (!model.content) {
        if (!content) return fail(CryptoErrorCode::InvalidArgument, "co-signing a detached message needs its content");
        source = content;
    }
    auto digest = source->digest(provider, digestOf(*sigAlg), options.cancel);
    if (!digest) return std::unexpected(digest.error());
    auto si = buildSignerInfo(provider, identity, options, *sigAlg, model.contentType, *digest);
    if (!si) return std::unexpected(si.error());
    model.signers.push_back(std::move(*si));
    addCertificates(model, identity, options.includeChain);
    return {};
}

CryptoOk addCountersignature(CmsSignedData& model, std::size_t signerIndex, CryptoProvider& provider,
                             SigningIdentity& identity, const CmsSignOptions& options) {
    if (signerIndex >= model.signers.size())
        return fail(CryptoErrorCode::InvalidArgument, "signer index out of range");
    auto sigAlg = chooseSignature(identity, options);
    if (!sigAlg) return std::unexpected(sigAlg.error());
    CmsSignerInfo& target = model.signers[signerIndex];
    // RFC 5652 11.4: the countersignature covers the signature value octets
    // of the target and carries no content-type attribute.
    auto digest = provider.digest(digestOf(*sigAlg), target.signature);
    if (!digest) return std::unexpected(digest.error());
    CmsSignOptions cs = options;
    cs.cadesBes = false;
    auto si = buildSignerInfo(provider, identity, cs, *sigAlg, {}, *digest);
    if (!si) return std::unexpected(si.error());
    target.unsignedAttrs.push_back(makeAttribute(oid::kCounterSignature, encodeSignerInfo(*si)));
    addCertificates(model, identity, options.includeChain);
    return {};
}

// ---- verification -----------------------------------------------------------------

CryptoResult<std::vector<SignerVerification>> verifyCms(const CmsSignedData& model, CryptoProvider& provider,
                                                        const ContentSource* detachedContent,
                                                        const CmsVerifyOptions& options) {
    if (!provider.isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");
    ContentSource attached = ContentSource::fromBytes(model.content ? *model.content : Bytes{});
    const ContentSource* source = &attached;
    if (!model.content) {
        if (!detachedContent) return fail(CryptoErrorCode::InvalidArgument, "detached signature needs its content");
        source = detachedContent;
    }

    std::vector<Bytes> ders = model.certificates;
    ders.insert(ders.end(), options.extraCertificates.begin(), options.extraCertificates.end());
    const std::vector<CertificateInfo> pool = parseCertificates(provider, ders);

    std::vector<SignerVerification> out;
    for (std::size_t i = 0; i < model.signers.size(); ++i) {
        const CmsSignerInfo& si = model.signers[i];
        auto one = verifyOne(si, model.contentType, *source, pool, provider, options);
        if (!one) return std::unexpected(one.error());
        SignerVerification sv;
        sv.index = i;
        sv.signer = one->signer;
        sv.digest = one->digest;
        sv.signingTime = signingTimeOf(si);
        sv.hasTimestampToken = si.findUnsigned(oid::kSignatureTimeStampToken) != nullptr;
        sv.validation = one->v;

        for (const CmsAttribute& a : si.unsignedAttrs) {
            if (a.oid != oid::kCounterSignature) continue;
            for (const Bytes& value : a.values) {
                CountersignatureVerification cv;
                auto el = readSingle(value);
                auto parsed = el ? parseSignerInfo(*el) : std::unexpected(el.error());
                if (!parsed) {
                    cv.validation.crypto = CryptoCheck::Invalid;
                    cv.validation.notes.push_back("malformed countersignature");
                    cv.validation.state = foldState(cv.validation);
                } else {
                    auto r = verifyOne(*parsed, {}, ContentSource::fromBytes(si.signature), pool, provider, options);
                    if (!r) return std::unexpected(r.error());
                    cv.signer = r->signer;
                    cv.validation = r->v;
                }
                sv.countersignatures.push_back(std::move(cv));
            }
        }
        out.push_back(std::move(sv));
    }
    return out;
}

// ---- description ---------------------------------------------------------------------

namespace {

std::string timeText(std::chrono::system_clock::time_point t) {
    const auto el = timeElement(t);
    const std::string text(el.begin() + 2, el.end());
    return text; // compact UTCTime / GeneralizedTime text, e.g. 260101120000Z
}

} // namespace

std::string describeCms(const CmsSignedData& m, CryptoProvider* provider) {
    std::ostringstream out;
    out << "type: SignedData\n";
    out << "content type: " << oidName(m.contentType) << "\n";
    if (m.content) out << "content: attached, " << m.content->size() << " bytes\n";
    else out << "content: detached\n";
    out << "certificates: " << m.certificates.size() << "\n";
    out << "crls: " << m.crls.size() << "\n";

    std::size_t n = 0;
    for (const Bytes& der : m.certificates) {
        out << "  certificate " << n++ << ": ";
        bool described = false;
        if (provider && provider->isAvailable()) {
            if (auto c = provider->parseCertificate(der)) {
                out << "subject=" << c->subject << " issuer=" << c->issuer << " serial=" << c->serialHex
                    << " notBefore=" << timeText(c->notBefore) << " notAfter=" << timeText(c->notAfter)
                    << " sha256=" << c->sha256Fingerprint << " key=" << toString(c->publicKeyType) << "/"
                    << c->keyBits;
                if (c->russian.inn) out << " INN=" << *c->russian.inn;
                if (c->russian.snils) out << " SNILS=" << *c->russian.snils;
                if (c->russian.ogrn) out << " OGRN=" << *c->russian.ogrn;
                described = true;
            }
        }
        if (!described) out << der.size() << " bytes (DER)";
        out << "\n";
    }

    out << "signers: " << m.signers.size() << "\n";
    n = 0;
    for (const CmsSignerInfo& s : m.signers) {
        out << "  signer " << n++ << ":\n";
        out << "    digest: " << oidName(s.digestOid) << "\n";
        out << "    signature algorithm: " << oidName(s.signatureAlgOid) << "\n";
        out << "    identifier: " << (s.issuerDer.empty() ? "subjectKeyIdentifier" : "issuerAndSerialNumber") << "\n";
        out << "    signed attributes:";
        if (s.signedAttrs.empty()) out << " none";
        for (const CmsAttribute& a : s.signedAttrs) out << " " << oidName(a.oid);
        out << "\n    unsigned attributes:";
        if (s.unsignedAttrs.empty()) out << " none";
        for (const CmsAttribute& a : s.unsignedAttrs) out << " " << oidName(a.oid);
        out << "\n";
        if (auto t = signingTimeOf(s)) out << "    signing time: " << timeText(*t) << "\n";
        out << "    timestamp token: " << (s.findUnsigned(oid::kSignatureTimeStampToken) ? "present" : "absent") << "\n";
        std::size_t cs = 0;
        for (const CmsAttribute& a : s.unsignedAttrs)
            if (a.oid == oid::kCounterSignature) cs += a.values.size();
        out << "    countersignatures: " << cs << "\n";
        out << "    CAdES signing-certificate-v2: " << (s.findSigned(oid::kSigningCertificateV2) ? "yes" : "no") << "\n";
    }
    return out.str();
}

} // namespace rivet::crypto
