// SPDX-License-Identifier: MPL-2.0
// Backend-free hostile-input corpus for the CMS parser. The parser must return
// a typed error (never crash, hang or over-allocate) for every case.
#include "RivetTest.h"

#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"

#include <random>

using namespace rivet::crypto;
using namespace rivet::crypto::der;

namespace {

Bytes algId(std::string_view oidDotted) { return sequence({oidElement(oidDotted), nullValue()}); }

Bytes attribute(std::string_view oidDotted, const Bytes& value) {
    return sequence({oidElement(oidDotted), setOf({value})});
}

// A structurally valid SignedData with one signer; the signature is garbage
// (no backend needed to parse it).
struct Built {
    Bytes attrsBody; // concatenated signed attributes
    Bytes signerInfo;
    Bytes cms;
};

Built build(const std::vector<Bytes>& signedAttrs, bool withAttrs = true) {
    Built b;
    const Bytes issuer = sequence({});
    const Bytes sid = sequence({issuer, integerFromInt(7)});
    std::vector<Bytes> si{integerFromInt(1), sid, algId(oid::kSha256)};
    if (withAttrs) {
        Bytes body = concat(signedAttrs);
        b.attrsBody = body;
        si.push_back(tlv(contextTag(0, true), body));
    }
    si.push_back(algId(oid::kRsaEncryption));
    si.push_back(octetString(Bytes(16, 0xAA)));
    b.signerInfo = sequence(si);
    const Bytes encap = sequence({oidElement(oid::kData), tlv(contextTag(0, true), octetString(viewOf("hi")))});
    const Bytes sd = sequence({integerFromInt(1), tlv(kSet, algId(oid::kSha256)), encap, tlv(kSet, b.signerInfo)});
    b.cms = sequence({oidElement(oid::kSignedData), tlv(contextTag(0, true), sd)});
    return b;
}

std::vector<Bytes> goodAttrs() {
    return {attribute(oid::kContentType, oidElement(oid::kData)),
            attribute(oid::kMessageDigest, octetString(Bytes(32, 1)))};
}

} // namespace

RIVET_TEST(cmsParsesHandBuiltStructure) {
    const auto b = build(goodAttrs());
    const auto parsed = parseCms(b.cms);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK_EQ(parsed->signers.size(), std::size_t{1});
    CHECK(!parsed->isDetached());
    CHECK(parsed->signers[0].hasSignedAttrs());
    CHECK_EQ(parsed->signers[0].digestOid, std::string(oid::kSha256));
    // Re-encoding a parsed model preserves the signed attributes byte for byte.
    const auto again = encodeCms(*parsed);
    CHECK(again.has_value());
    if (again) CHECK(*again == b.cms);
}

RIVET_TEST(cmsRejectsDuplicateSignedAttributes) {
    auto attrs = goodAttrs();
    attrs.push_back(attribute(oid::kMessageDigest, octetString(Bytes(32, 2))));
    const auto parsed = parseCms(build(attrs).cms);
    CHECK(!parsed.has_value());
    if (!parsed) CHECK_EQ(parsed.error().code, CryptoErrorCode::MalformedCms);
}

RIVET_TEST(cmsRejectsEmptyAndGarbage) {
    CHECK(!parseCms(Bytes{}).has_value());
    CHECK(!parseCms(Bytes{0x00}).has_value());
    CHECK(!parseCms(viewOf("not a cms at all")).has_value());
    CHECK(!parseCmsAny(viewOf("-----BEGIN CMS-----\n!!!!\n-----END CMS-----\n")).has_value());
    // A ContentInfo of the wrong type.
    const Bytes wrong = sequence({oidElement(oid::kData), tlv(contextTag(0, true), octetString(viewOf("x")))});
    CHECK(!parseCms(wrong).has_value());
}

RIVET_TEST(cmsRejectsEveryTruncation) {
    const auto b = build(goodAttrs());
    for (std::size_t n = 0; n < b.cms.size(); ++n) {
        const auto parsed = parseCms(ByteView(b.cms.data(), n));
        CHECK(!parsed.has_value());
    }
}

RIVET_TEST(cmsRejectsOversizedLengthClaims) {
    auto b = build(goodAttrs());
    Bytes huge = b.cms;
    // Replace the outer SEQUENCE length with a 4 GiB claim.
    Bytes forged{0x30, 0x84, 0xFF, 0xFF, 0xFF, 0xFF};
    forged.insert(forged.end(), huge.begin() + 2, huge.end());
    CHECK(!parseCms(forged).has_value());
    // A nested element claiming more than its parent holds.
    Bytes forged2 = b.cms;
    // byte 2 is the ContentInfo OID tag; corrupt the [0] length that follows the OID.
    const std::size_t oidLen = 2 + 9;
    const std::size_t pos = 2 + oidLen + 1; // length byte of [0]
    if (pos < forged2.size()) {
        forged2[pos] = 0x84;
        forged2.insert(forged2.begin() + static_cast<std::ptrdiff_t>(pos) + 1, {0x7F, 0xFF, 0xFF, 0xFF});
        CHECK(!parseCms(forged2).has_value());
    }
}

RIVET_TEST(cmsRejectsDeepNesting) {
    Bytes e = sequence({});
    for (int i = 0; i < 500; ++i) e = sequence({e});
    CHECK(!parseCms(e).has_value());
    // Deep nesting hidden inside a signed attribute value.
    Bytes deep = integerFromInt(1);
    for (int i = 0; i < 100; ++i) deep = sequence({deep});
    auto attrs = goodAttrs();
    attrs.push_back(attribute("1.2.3.4", deep));
    CHECK(!parseCms(build(attrs).cms).has_value());
}

RIVET_TEST(cmsRejectsBadSignerStructures) {
    // SignerInfo without signature / algorithm fields.
    const Bytes sid = sequence({sequence({}), integerFromInt(1)});
    const Bytes shortSigner = sequence({integerFromInt(1), sid, algId(oid::kSha256)});
    const Bytes encap = sequence({oidElement(oid::kData)});
    const Bytes sd = sequence({integerFromInt(1), tlv(kSet, algId(oid::kSha256)), encap, tlv(kSet, shortSigner)});
    const Bytes cms = sequence({oidElement(oid::kSignedData), tlv(contextTag(0, true), sd)});
    CHECK(!parseCms(cms).has_value());
}

RIVET_TEST(cmsNeverCrashesOnMutatedInput) {
    const auto b = build(goodAttrs());
    std::mt19937 rng(0xC0FFEE);
    for (int iter = 0; iter < 3000; ++iter) {
        Bytes m = b.cms;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < edits; ++k) m[rng() % m.size()] = static_cast<std::uint8_t>(rng());
        if (rng() % 5 == 0) m.resize(rng() % m.size());
        (void)parseCms(m); // result irrelevant: must simply return
    }
    CHECK(true);
}

RIVET_TEST(cmsAcceptsBerIndefiniteLengthWrapper) {
    // Same structure but with an indefinite-length outer SEQUENCE and [0].
    const auto b = build(goodAttrs());
    const auto top = readSingle(b.cms);
    CHECK(top.has_value());
    if (!top) return;
    const auto kids = children(*top);
    CHECK(kids.has_value());
    if (!kids) return;
    Bytes ber{0x30, 0x80};
    ber.insert(ber.end(), (*kids)[0].full.begin(), (*kids)[0].full.end());
    ber.push_back(0xA0);
    ber.push_back(0x80);
    ber.insert(ber.end(), (*kids)[1].content.begin(), (*kids)[1].content.end());
    ber.insert(ber.end(), {0x00, 0x00, 0x00, 0x00});
    const auto parsed = parseCms(ber);
    CHECK(parsed.has_value());
}

RIVET_TEST(pemDecodeRoundTrip) {
    const auto b = build(goodAttrs());
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (std::size_t i = 0; i < b.cms.size(); i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(b.cms[i]) << 16;
        if (i + 1 < b.cms.size()) v |= static_cast<std::uint32_t>(b.cms[i + 1]) << 8;
        if (i + 2 < b.cms.size()) v |= b.cms[i + 2];
        b64 += tbl[(v >> 18) & 63];
        b64 += tbl[(v >> 12) & 63];
        b64 += i + 1 < b.cms.size() ? tbl[(v >> 6) & 63] : '=';
        b64 += i + 2 < b.cms.size() ? tbl[v & 63] : '=';
    }
    std::string pem = "-----BEGIN CMS-----\n";
    for (std::size_t i = 0; i < b64.size(); i += 64) pem += b64.substr(i, 64) + "\n";
    pem += "-----END CMS-----\n";
    const auto parsed = parseCmsAny(viewOf(pem));
    CHECK(parsed.has_value());
}
