// SPDX-License-Identifier: MPL-2.0
// PAdES B-LT / B-LTA: evidence collection, /DSS + /VRI embedding, archive
// time-stamps, offline verification and hostile /DSS input (ADR-0045).

#include "RivetTest.h"

#include "LtvTestSupport.hpp"

#include <algorithm>
#include <random>


using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;

namespace {

Bytes signAt(LtvEnv& env, PadesLevel level, Bytes pdf = makeFixture(), bool allowPartial = false) {
    SignRequest r = env.request(level);
    r.ltv.allowPartial = allowPartial;
    auto out = signPdf(std::move(pdf), r, *env.provider, *env.identity);
    if (!out) {
        std::printf("    sign(%s) failed: %s\n", std::string(toString(level)).c_str(), errText(out.error()).c_str());
        return {};
    }
    return std::move(out->data);
}

} // namespace

RIVET_TEST(ltvPadesLevelNames) {
    CHECK(toString(PadesLevel::BaselineLT) == "B-LT");
    CHECK(toString(PadesLevel::BaselineLTA) == "B-LTA");
}

RIVET_TEST(signToBaselineLtEmbedsDssAndVerifiesOffline) {
    LtvEnv env;
    SignRequest r = env.request(PadesLevel::BaselineLT);
    auto out = signPdf(makeFixture(), r, *env.provider, *env.identity);
    if (!out) std::printf("    sign failed: %s\n", errText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK(out->level == PadesLevel::BaselineLT);
    CHECK(out->levelDowngradeReason.empty());
    CHECK(out->ltvOperation);
    CHECK_GE(out->dssOcspResponses, std::size_t{3}); // leaf, intermediate and TSA certificate
    CHECK(!out->archiveTimestamped);
    const Bytes base = makeFixture();
    CHECK(std::equal(base.begin(), base.end(), out->data.begin())); // incremental only

    const int before = env.transport->totalCalls();
    const auto sigs = env.verify(out->data);
    printLtv("B-LT", sigs);
    CHECK_EQ(env.transport->totalCalls(), before); // zero transport calls while verifying
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (sigs.empty()) return;
    const PdfSignatureInfo& s = sigs[0];
    CHECK(s.level == PadesLevel::BaselineLT);
    CHECK(s.ltv.dssPresent);
    CHECK(s.ltv.vriPresent);
    CHECK_EQ(s.ltv.vriKey.size(), std::size_t{40});
    CHECK(s.ltv.evaluated);
    CHECK(s.ltv.signerEvidenceComplete);
    CHECK(s.ltv.timestampEvidenceComplete);
    CHECK(!s.ltv.dssCoveredByTimestamp);
    CHECK(s.ltv.validationTime.has_value());
    CHECK_GE(s.ltv.vriOcspResponses, std::size_t{3});
    CHECK(s.validation.crypto == crypto::CryptoCheck::Valid);

    // The VRI key is the uppercase SHA-1 of /Contents.
    auto key = vriKey(*env.provider, s.material.contentsDer);
    CHECK(key.has_value());
    if (key) CHECK(*key == s.ltv.vriKey);

    // Without trust anchors nothing is claimed.
    VerifyOptions none;
    const auto weak = env.verify(out->data, &none);
    CHECK_EQ(weak.size(), std::size_t{1});
    if (!weak.empty()) CHECK(weak[0].level != PadesLevel::BaselineLT);
}

RIVET_TEST(signToBaselineLtaAddsArchiveTimestamp) {
    LtvEnv env;
    auto out = signPdf(makeFixture(), env.request(PadesLevel::BaselineLTA), *env.provider, *env.identity);
    if (!out) std::printf("    sign failed: %s\n", errText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK(out->level == PadesLevel::BaselineLTA);
    CHECK(out->archiveTimestamped);
    const int before = env.transport->totalCalls();
    const auto sigs = env.verify(out->data);
    printLtv("B-LTA", sigs);
    CHECK_EQ(env.transport->totalCalls(), before);
    CHECK_EQ(countKind(sigs, SignatureKind::DocumentTimestamp), std::size_t{1});
    const PdfSignatureInfo* s = firstSignature(sigs);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(s->level == PadesLevel::BaselineLTA);
    CHECK(s->ltv.dssCoveredByTimestamp);
    CHECK(s->ltv.coveringTimestampTime.has_value());
    CHECK(s->validation.modification != crypto::ModificationStatus::Disallowed);
}

RIVET_TEST(missingIntermediateOcspProducesNoFile) {
    LtvEnv env;
    env.transport->fail(kOcspRoot);
    env.transport->fail(kCrlHost);
    auto out = signPdf(makeFixture(), env.request(PadesLevel::BaselineLT), *env.provider, *env.identity);
    CHECK(!out.has_value());
    if (out) return;
    std::printf("    error: %s\n", errText(out.error()).c_str());
    CHECK(out.error().code == SignErrorCode::EvidenceUnavailable);
    CHECK(!out.error().missingEvidence.empty());
    bool names = false;
    for (const std::string& m : out.error().missingEvidence)
        if (m.find("LTV Intermediate CA") != std::string::npos) names = true;
    CHECK(names);

    const auto dir = scratchDir("ltv-missing");
    ctest::writeFile(dir / "in.pdf", makeFixture());
    auto f = signPdfFile(dir / "in.pdf", dir / "out.pdf", env.request(PadesLevel::BaselineLT), *env.provider, *env.identity);
    CHECK(!f.has_value());
    CHECK(!std::filesystem::exists(dir / "out.pdf"));
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++files;
    }
    CHECK_EQ(files, std::size_t{1});
}

RIVET_TEST(allowPartialReportsTheAchievedLevel) {
    LtvEnv env;
    env.transport->fail(kOcspRoot);
    env.transport->fail(kCrlHost);
    SignRequest r = env.request(PadesLevel::BaselineLTA);
    r.ltv.allowPartial = true;
    auto out = signPdf(makeFixture(), r, *env.provider, *env.identity);
    if (!out) std::printf("    sign failed: %s\n", errText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    std::printf("    level=%s downgrade=%s\n", std::string(toString(out->level)).c_str(), out->levelDowngradeReason.c_str());
    CHECK(out->level == PadesLevel::BaselineT);
    CHECK(!out->levelDowngradeReason.empty());
    CHECK(!out->archiveTimestamped);
    const auto sigs = env.verify(out->data);
    const PdfSignatureInfo* s = firstSignature(sigs);
    CHECK(s != nullptr);
    if (s) {
        CHECK(s->level == PadesLevel::BaselineT);
        CHECK(!s->ltv.reasons.empty());
        CHECK(!s->ltv.missing.empty());
    }
}

RIVET_TEST(upgradeFromBaselineBIsRefused) {
    LtvEnv env;
    auto b = signPdf(makeFixture(), env.request(PadesLevel::BaselineB), *env.provider, *env.identity);
    CHECK(b.has_value());
    if (!b) return;
    CHECK(b->level == PadesLevel::BaselineB);
    for (PadesLevel target : {PadesLevel::BaselineT, PadesLevel::BaselineLT, PadesLevel::BaselineLTA}) {
        auto up = upgradePdf(b->data, env.upgrade(target), *env.provider);
        CHECK(!up.has_value());
        if (up) continue;
        std::printf("    refusal: %s\n", errText(up.error()).c_str());
        CHECK(up.error().code == SignErrorCode::UnsupportedUpgrade);
    }
}

RIVET_TEST(upgradeBaselineTToLtThenLta) {
    LtvEnv env;
    const Bytes t = signAt(env, PadesLevel::BaselineT);
    CHECK(!t.empty());
    if (t.empty()) return;
    CHECK(firstSignature(env.verify(t))->level == PadesLevel::BaselineT);

    auto lt = upgradePdf(t, env.upgrade(PadesLevel::BaselineLT), *env.provider);
    if (!lt) std::printf("    upgrade failed: %s\n", errText(lt.error()).c_str());
    CHECK(lt.has_value());
    if (!lt) return;
    CHECK(lt->level == PadesLevel::BaselineLT);
    CHECK(std::equal(t.begin(), t.end(), lt->data.begin()));
    CHECK_GT(lt->data.size(), t.size());

    auto lta = upgradePdf(lt->data, env.upgrade(PadesLevel::BaselineLTA), *env.provider);
    if (!lta) std::printf("    upgrade failed: %s\n", errText(lta.error()).c_str());
    CHECK(lta.has_value());
    if (!lta) return;
    CHECK(lta->level == PadesLevel::BaselineLTA);
    CHECK(lta->archiveTimestamped);
    CHECK(std::equal(lt->data.begin(), lt->data.end(), lta->data.begin()));
    printLtv("T->LT->LTA", env.verify(lta->data));
}

RIVET_TEST(secondUpgradeIsANoOpAndDoesNotBloat) {
    LtvEnv env;
    const Bytes t = signAt(env, PadesLevel::BaselineT);
    if (t.empty()) return;
    auto lt = upgradePdf(t, env.upgrade(PadesLevel::BaselineLT), *env.provider);
    CHECK(lt.has_value());
    if (!lt) return;
    const int before = env.transport->totalCalls();
    auto again = upgradePdf(lt->data, env.upgrade(PadesLevel::BaselineLT), *env.provider);
    CHECK(again.has_value());
    if (!again) return;
    CHECK_EQ(again->data.size(), lt->data.size()); // nothing appended
    CHECK_EQ(env.transport->totalCalls(), before); // nothing fetched
    CHECK(again->level == PadesLevel::BaselineLT);
}

RIVET_TEST(dssMergeKeepsOldEntriesAndDeduplicates) {
    LtvEnv env;
    // Signature A: B-LT. Signature B (same signer, so the same chain): B-T, then upgraded.
    const Bytes a = signAt(env, PadesLevel::BaselineLT);
    if (a.empty()) return;
    const DssData dssA = dssOf(a);
    CHECK_EQ(dssA.vri.size(), std::size_t{1});
    auto bSigned = signPdf(a, env.request(PadesLevel::BaselineT), *env.provider, *env.identity);
    CHECK(bSigned.has_value());
    if (!bSigned) return;
    UpgradeRequest up = env.upgrade(PadesLevel::BaselineLT);
    up.onlySignature = bSigned->signatureRef;
    auto lt = upgradePdf(bSigned->data, up, *env.provider);
    if (!lt) std::printf("    upgrade failed: %s\n", errText(lt.error()).c_str());
    CHECK(lt.has_value());
    if (!lt) return;
    CHECK_GT(lt->dssReusedStreams, std::size_t{0}); // chain certificates and OCSPs already in the /DSS
    const DssData merged = dssOf(lt->data);
    CHECK_EQ(merged.vri.size(), std::size_t{2});
    for (const auto& [k, v] : dssA.vri) CHECK(merged.vri.count(k) == 1);
    // Every old global item is still referenced.
    for (const DssItem& c : dssA.global.certs) {
        const bool kept = std::any_of(merged.global.certs.begin(), merged.global.certs.end(),
                                      [&](const DssItem& m) { return m.data == c.data; });
        CHECK(kept);
    }
    for (const DssItem& o : dssA.global.ocsps) {
        const bool kept = std::any_of(merged.global.ocsps.begin(), merged.global.ocsps.end(),
                                      [&](const DssItem& m) { return m.data == o.data; });
        CHECK(kept);
    }
    // No duplicate certificate streams.
    for (std::size_t i = 0; i < merged.global.certs.size(); ++i)
        for (std::size_t j = i + 1; j < merged.global.certs.size(); ++j)
            CHECK(merged.global.certs[i].data != merged.global.certs[j].data);
    const auto sigs = env.verify(lt->data);
    printLtv("merge", sigs);
    CHECK_EQ(countKind(sigs, SignatureKind::Signature), std::size_t{2});
    for (const PdfSignatureInfo& s : sigs)
        if (s.kind == SignatureKind::Signature) CHECK(s.level == PadesLevel::BaselineLT);
}

RIVET_TEST(secondSignatureAfterDssKeepsTheFirstValid) {
    LtvEnv env;
    const Bytes a = signAt(env, PadesLevel::BaselineLT);
    if (a.empty()) return;
    auto second = signPdf(a, basicRequest(), *env.provider, *env.identity); // plain B-B after the DSS
    CHECK(second.has_value());
    if (!second) return;
    const auto sigs = env.verify(second->data);
    printLtv("second-signature", sigs);
    CHECK_EQ(sigs.size(), std::size_t{2});
    const PdfSignatureInfo* first = firstSignature(sigs);
    CHECK(first != nullptr);
    if (!first) return;
    CHECK(first->validation.crypto == crypto::CryptoCheck::Valid);
    CHECK(first->validation.integrity == crypto::IntegrityStatus::Intact);
    CHECK(first->validation.modification != crypto::ModificationStatus::Disallowed);
    CHECK(first->validation.modification != crypto::ModificationStatus::Unknown);
    CHECK(first->level == PadesLevel::BaselineLT);
}

RIVET_TEST(archiveRenewalAddsAnotherTimestampOverTheDss) {
    LtvEnv env;
    const Bytes a = signAt(env, PadesLevel::BaselineLTA);
    if (a.empty()) return;
    ArchiveRenewalRequest rr;
    rr.ltv = env.ltv();
    rr.fetch = env.fetcher();
    auto renewed = addArchiveTimestamp(a, rr, *env.provider);
    if (!renewed) std::printf("    renewal failed: %s\n", errText(renewed.error()).c_str());
    CHECK(renewed.has_value());
    if (!renewed) return;
    CHECK(std::equal(a.begin(), a.end(), renewed->data.begin()));
    CHECK(renewed->archiveTimestamped);
    CHECK(renewed->level == PadesLevel::BaselineLTA);
    const auto sigs = env.verify(renewed->data);
    printLtv("renewal", sigs);
    CHECK_EQ(countKind(sigs, SignatureKind::DocumentTimestamp), std::size_t{2});
    const PdfSignatureInfo* s = firstSignature(sigs);
    CHECK(s && s->level == PadesLevel::BaselineLTA && s->ltv.dssCoveredByTimestamp);
    // A second timestamp's token has its own VRI entry in the (merged) /DSS.
    const DssData d = dssOf(renewed->data);
    CHECK_GE(d.vri.size(), std::size_t{2});
    for (const PdfSignatureInfo& x : sigs)
        if (x.kind == SignatureKind::DocumentTimestamp) CHECK(x.validation.crypto == crypto::CryptoCheck::Valid);

    // Without any archive time-stamp there is nothing to renew.
    const Bytes lt = signAt(env, PadesLevel::BaselineLT);
    auto none = addArchiveTimestamp(lt, rr, *env.provider);
    CHECK(!none.has_value());
    if (!none) CHECK(none.error().code == SignErrorCode::UnsupportedUpgrade);
}

RIVET_TEST(crlFallbackWhenTheResponderIsDown) {
    LtvEnv env;
    env.transport->fail(kOcspRoot); // the intermediate is then proven by a CRL of the root
    auto out = signPdf(makeFixture(), env.request(PadesLevel::BaselineLT), *env.provider, *env.identity);
    if (!out) std::printf("    sign failed: %s\n", errText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK(out->level == PadesLevel::BaselineLT);
    CHECK_GE(out->dssCrls, std::size_t{1});
    CHECK(env.verify(out->data)[0].level == PadesLevel::BaselineLT);
}

RIVET_TEST(chainFromDssAndAiaWithoutEmbeddedCertificates) {
    LtvEnv env(/*leafHasChain=*/false);
    // The CMS carries only the leaf; the intermediate is fetched through AIA caIssuers.
    SignRequest r = env.request(PadesLevel::BaselineLT);
    r.includeChain = false;
    auto nofetch = signPdf(makeFixture(), r, *env.provider, *env.identity);
    CHECK(!nofetch.has_value()); // no issuer anywhere: no file
    if (!nofetch) CHECK(nofetch.error().code == SignErrorCode::EvidenceUnavailable);
    CHECK_EQ(env.transport->hostCalls(kCaHost), 0);

    r.ltv.fetchMissingIssuers = true;
    auto out = signPdf(makeFixture(), r, *env.provider, *env.identity);
    if (!out) std::printf("    sign failed: %s\n", errText(out.error()).c_str());
    CHECK(out.has_value());
    if (!out) return;
    CHECK_GE(env.transport->hostCalls(kCaHost), 1);
    CHECK(out->level == PadesLevel::BaselineLT);
    // The verifier finds the intermediate in the /DSS only; no network.
    const int before = env.transport->totalCalls();
    const auto sigs = env.verify(out->data);
    CHECK_EQ(env.transport->totalCalls(), before);
    const PdfSignatureInfo* first = firstSignature(sigs);
    CHECK(first != nullptr);
    CHECK(first->level == PadesLevel::BaselineLT);
}

RIVET_TEST(networkOffYieldsNoEvidence) {
    LtvEnv env;
    SignRequest r = env.request(PadesLevel::BaselineLT);
    r.ltv.network = nullptr;
    const int before = env.transport->hostCalls(kOcspInter);
    auto out = signPdf(makeFixture(), r, *env.provider, *env.identity);
    CHECK(!out.has_value());
    if (!out) CHECK(out.error().code == SignErrorCode::EvidenceUnavailable);
    CHECK_EQ(env.transport->hostCalls(kOcspInter), before);

    // A closed gate (Off) behaves the same through the shared network client.
    env.gate->setMode(net::OnlineValidationMode::Off);
    auto gated = signPdf(makeFixture(), env.request(PadesLevel::BaselineLT), *env.provider, *env.identity);
    CHECK(!gated.has_value());
}

RIVET_TEST(tamperedEvidenceIsNeverLtv) {
    LtvEnv env;
    const Bytes lt = signAt(env, PadesLevel::BaselineLT);
    if (lt.empty()) return;
    const DssData d = dssOf(lt);
    CHECK(!d.global.ocsps.empty());
    if (d.global.ocsps.empty()) return;
    // Flip one byte inside an embedded OCSP response (the signature's own bytes are untouched).
    Bytes bad = lt;
    const Bytes& ocsp = d.global.ocsps[0].data;
    auto it = std::search(bad.begin(), bad.end(), ocsp.begin(), ocsp.end());
    CHECK(it != bad.end());
    if (it == bad.end()) return;
    *(it + static_cast<std::ptrdiff_t>(ocsp.size() / 2)) ^= 0x40;
    const auto sigs = env.verify(bad);
    printLtv("tampered-ocsp", sigs);
    const PdfSignatureInfo* s = firstSignature(sigs);
    CHECK(s != nullptr);
    if (s) {
        CHECK(s->level != PadesLevel::BaselineLT);
        CHECK(s->level != PadesLevel::BaselineLTA);
        CHECK(!s->ltv.reasons.empty());
        CHECK(s->validation.crypto == crypto::CryptoCheck::Valid); // the signature itself is fine
    }

    // Tampering with the evidence of a B-LTA file also breaks the archive time-stamp.
    const Bytes lta = signAt(env, PadesLevel::BaselineLTA);
    const DssData dl = dssOf(lta);
    Bytes bad2 = lta;
    const Bytes& o2 = dl.global.ocsps[0].data;
    auto it2 = std::search(bad2.begin(), bad2.end(), o2.begin(), o2.end());
    CHECK(it2 != bad2.end());
    if (it2 == bad2.end()) return;
    *(it2 + static_cast<std::ptrdiff_t>(o2.size() / 2)) ^= 0x40;
    const auto sigs2 = env.verify(bad2);
    const PdfSignatureInfo* s2 = firstSignature(sigs2);
    CHECK(s2 && s2->level < PadesLevel::BaselineLT);
    for (const PdfSignatureInfo& x : sigs2)
        if (x.kind == SignatureKind::DocumentTimestamp) CHECK(x.validation.integrity != crypto::IntegrityStatus::Intact);
}

RIVET_TEST(missingVriAndIncompleteChainAreNotLtv) {
    LtvEnv env;
    const Bytes t = signAt(env, PadesLevel::BaselineT);
    if (t.empty()) return;
    const auto sigT = env.verify(t);
    CHECK(firstSignature(sigT) != nullptr);
    if (!firstSignature(sigT)) return;

    // Global evidence for a real B-LT, but no /VRI entry.
    auto lt = upgradePdf(t, env.upgrade(PadesLevel::BaselineLT), *env.provider);
    CHECK(lt.has_value());
    if (!lt) return;
    const DssData full = dssOf(lt->data);
    DssAdditions onlyGlobal;
    for (const DssItem& c : full.global.certs) onlyGlobal.global.certs.push_back(c.data);
    for (const DssItem& o : full.global.ocsps) onlyGlobal.global.ocsps.push_back(o.data);
    for (const DssItem& c : full.global.crls) onlyGlobal.global.crls.push_back(c.data);
    auto noVri = appendDss(t, onlyGlobal, *env.provider);
    CHECK(noVri.has_value());
    if (!noVri) return;
    const auto strict = env.verify(noVri->data);
    const PdfSignatureInfo* strictFirst = firstSignature(strict);
    CHECK(strictFirst != nullptr);
    CHECK(strictFirst->level == PadesLevel::BaselineT);
    CHECK(!strictFirst->ltv.vriPresent);
    VerifyOptions lax = env.verifyOpts();
    lax.requireVri = false;
    const auto relaxed = env.verify(noVri->data, &lax);
    const PdfSignatureInfo* relaxedFirst = firstSignature(relaxed);
    CHECK(relaxedFirst != nullptr);
    CHECK(relaxedFirst->level == PadesLevel::BaselineLT);

    // Evidence without the CA certificates and without CMS-embedded intermediates: incomplete chain.
    LtvEnv bare(/*leafHasChain=*/false);
    SignRequest r = bare.request(PadesLevel::BaselineT);
    r.includeChain = false;
    auto bt = signPdf(makeFixture(), r, *bare.provider, *bare.identity);
    CHECK(bt.has_value());
    if (!bt) return;
    DssAdditions ocspOnly;
    VriAddition v;
    const auto btSigs = bare.verify(bt->data);
    auto sig = firstSignature(btSigs);
    CHECK(sig != nullptr);
    if (!sig) return;
    v.signatureContents = sig->material.contentsDer;
    for (const DssItem& o : full.global.ocsps) v.evidence.ocsps.push_back(o.data);
    ocspOnly.vri.push_back(v);
    auto inc = appendDss(bt->data, ocspOnly, *bare.provider);
    CHECK(inc.has_value());
    if (!inc) return;
    const auto incSigs = bare.verify(inc->data);
    printLtv("incomplete-chain", incSigs);
    const PdfSignatureInfo* incFirst = firstSignature(incSigs);
    CHECK(incFirst != nullptr);
    CHECK(incFirst->level == PadesLevel::BaselineT);
    CHECK(!incFirst->ltv.missing.empty());
}

namespace {

// Appends a revision that replaces the catalog's /DSS with `dssValue` and adds `extra` objects.
Bytes withRawDss(const Bytes& pdf, const std::string& dssValue,
                 const std::vector<std::pair<std::uint32_t, std::string>>& extra) {
    auto f = PdfFile::open(pdf);
    if (!f) return pdf;
    auto cat = f->catalog();
    if (!cat) return pdf;
    const std::string needle = std::to_string(cat->first.num) + " 0 obj";
    const std::string text(pdf.begin(), pdf.end());
    const std::size_t at = text.rfind(needle);
    const std::size_t end = text.find("endobj", at);
    std::string body = text.substr(at + needle.size(), end - at - needle.size());
    const std::size_t close = body.rfind(">>");
    body = body.substr(0, close) + " /DSS " + dssValue + " >>";
    std::vector<std::pair<std::uint32_t, std::string>> objs{{cat->first.num, body}};
    for (const auto& e : extra) objs.push_back(e);
    return appendRevision(pdf, objs);
}

} // namespace

static PadesLevel levelOf(const std::vector<PdfSignatureInfo>& v) {
    return v.empty() ? PadesLevel::NotPades : v.front().level;
}

RIVET_TEST(malformedDssNeverCrashes) {
    LtvEnv env;
    const Bytes t = signAt(env, PadesLevel::BaselineT);
    if (t.empty()) return;
    std::string huge;
    for (int i = 0; i < 6000; ++i) huge += "900 0 R ";
    const std::vector<std::pair<std::string, std::vector<std::pair<std::uint32_t, std::string>>>> cases = {
        {"5", {}},
        {"(text)", {}},
        {"[1 2 3]", {}},
        {"<< /Certs 7 /OCSPs (x) /CRLs << >> /VRI 5 >>", {}},
        {"<< /Certs [900 0 R 901 0 R] >>", {}},                                     // missing streams
        {"<< /Certs [900 0 R] >>", {{900, "<< /Length 4 >>"}}},                      // a dictionary, not a stream
        {"950 0 R", {{950, "<< /Certs [950 0 R] /OCSPs [950 0 R] /VRI 950 0 R >>"}}}, // cyclic
        {"<< /Certs [" + huge + "] >>", {{900, "<< /Length 0 >>\nstream\n\nendstream"}}},
        {"<< /VRI << /ABCDEF << /Cert 5 /OCSP [null] /TU 7 >> >> >>", {}},
        {"<< /Certs [900 0 R] >>", {{900, "<< /Length 1000000000 >>\nstream\nabc\nendstream"}}},
    };
    for (const auto& [value, extra] : cases) {
        const Bytes bad = withRawDss(t, value, extra);
        auto r = verifyPdfSignatures(bad, *env.provider, env.verifyOpts());
        std::printf("    DSS %.40s -> %s\n", value.c_str(), r ? std::string(toString(levelOf(*r))).c_str() : "unreadable");
        if (r)
            if (const PdfSignatureInfo* s = firstSignature(*r)) CHECK(s->level < PadesLevel::BaselineLT);
        // A writer meeting such a catalog refuses or merges, never crashes.
        auto up = upgradePdf(bad, env.upgrade(PadesLevel::BaselineLT), *env.provider);
        (void)up;
    }
}

RIVET_TEST(mutatedLtvFilesNeverCrashTheVerifier) {
    LtvEnv env;
    const Bytes lta = signAt(env, PadesLevel::BaselineLTA);
    if (lta.empty()) return;
    const std::size_t floor = makeFixture().size();
    std::mt19937 rng(0xD55);
    std::size_t levelDrops = 0;
    for (int i = 0; i < 100; ++i) {
        Bytes m = lta;
        const int edits = 1 + static_cast<int>(rng() % 3);
        for (int e = 0; e < edits; ++e) {
            const std::size_t pos = floor + rng() % (m.size() - floor);
            switch (rng() % 3) {
            case 0: m[pos] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
            case 1: m[pos] = static_cast<std::uint8_t>(rng()); break;
            default: m.erase(m.begin() + static_cast<std::ptrdiff_t>(pos), m.begin() + static_cast<std::ptrdiff_t>(std::min(m.size(), pos + 1 + rng() % 16))); break;
            }
        }
        auto r = verifyPdfSignatures(m, *env.provider, env.verifyOpts());
        if (!r) continue;
        const PdfSignatureInfo* s = firstSignature(*r);
        if (!s || s->level < PadesLevel::BaselineLTA) ++levelDrops;
    }
    std::printf("    100 mutations, %zu lowered or removed the level\n", levelDrops);
    CHECK(true);
}

RIVET_TEST(fileVariantsOfUpgradeAndRenewal) {
    LtvEnv env;
    const Bytes t = signAt(env, PadesLevel::BaselineT);
    if (t.empty()) return;
    const auto dir = scratchDir("ltv-file");
    ctest::writeFile(dir / "t.pdf", t);
    auto lt = upgradePdfFile(dir / "t.pdf", dir / "lt.pdf", env.upgrade(PadesLevel::BaselineLT), *env.provider);
    if (!lt) std::printf("    upgrade failed: %s\n", errText(lt.error()).c_str());
    CHECK(lt.has_value());
    CHECK(std::filesystem::exists(dir / "lt.pdf"));
    auto lta = upgradePdfFile(dir / "lt.pdf", dir / "lta.pdf", env.upgrade(PadesLevel::BaselineLTA), *env.provider);
    CHECK(lta.has_value());
    ArchiveRenewalRequest rr;
    rr.ltv = env.ltv();
    rr.fetch = env.fetcher();
    auto renewed = addArchiveTimestampFile(dir / "lta.pdf", dir / "renewed.pdf", rr, *env.provider);
    CHECK(renewed.has_value());
    CHECK(std::filesystem::exists(dir / "renewed.pdf"));
    // A refusal leaves no output.
    auto b = signPdf(makeFixture(), env.request(PadesLevel::BaselineB), *env.provider, *env.identity);
    if (b) {
        ctest::writeFile(dir / "b.pdf", b->data);
        auto bad = upgradePdfFile(dir / "b.pdf", dir / "b-up.pdf", env.upgrade(PadesLevel::BaselineLT), *env.provider);
        CHECK(!bad.has_value());
        CHECK(!std::filesystem::exists(dir / "b-up.pdf"));
    }
}

// ---- Real interop with independent tools (each skipped, with a message, when absent) ----

RIVET_TEST(ltvInteropWithIndependentTools) {
    LtvEnv env;
    auto out = signPdf(makeFixture(), env.request(PadesLevel::BaselineLTA), *env.provider, *env.identity);
    CHECK(out.has_value());
    if (!out) return;
    const auto dir = scratchDir("ltv-interop");
    ctest::writeFile(dir / "lta.pdf", out->data);
    std::printf("    tools: openssl=%s pdfsig=%s qpdf=%s mutool=%s pdftk=%s\n", toolAvailable("openssl") ? "yes" : "no",
                toolAvailable("pdfsig") ? "yes" : "no", toolAvailable("qpdf") ? "yes" : "no",
                toolAvailable("mutool") ? "yes" : "no", toolAvailable("pdftk") ? "yes" : "no");
    const DssData d = dssOf(out->data);
    CHECK(!d.global.ocsps.empty());
    CHECK(!d.global.certs.empty());
    const auto sigs = env.verify(out->data);
    const PdfSignatureInfo* sig = firstSignature(sigs);
    CHECK(sig != nullptr);
    std::string o;

    if (toolAvailable("openssl")) {
        // VRI key == openssl dgst -sha1 of /Contents (uppercase hex).
        if (sig) {
            ctest::writeFile(dir / "contents.der", sig->material.contentsDer);
            const int rc = runCommand("openssl dgst -sha1 -r '" + (dir / "contents.der").string() + "'", o);
            std::string digest = o.substr(0, 40);
            for (char& c : digest) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::printf("    openssl dgst -sha1 -> %s (vri key %s)\n", digest.c_str(), sig->ltv.vriKey.c_str());
            CHECK_EQ(rc, 0);
            CHECK(digest == sig->ltv.vriKey);
        }
        std::size_t i = 0;
        for (const DssItem& c : d.global.certs) {
            ctest::writeFile(dir / ("c" + std::to_string(i) + ".der"), c.data);
            const int rc = runCommand("openssl x509 -inform DER -noout -subject -in '" + (dir / ("c" + std::to_string(i) + ".der")).string() + "'", o);
            std::printf("    openssl x509 #%zu -> rc=%d %s", i, rc, o.c_str());
            CHECK_EQ(rc, 0);
            ++i;
        }
        i = 0;
        for (const DssItem& r : d.global.ocsps) {
            const auto f = dir / ("r" + std::to_string(i) + ".der");
            ctest::writeFile(f, r.data);
            const int rc = runCommand("openssl ocsp -respin '" + f.string() + "' -resp_text -noverify", o);
            std::printf("    openssl ocsp #%zu -> rc=%d %s\n", i, rc, o.substr(0, o.find("Responses by")).c_str());
            CHECK_EQ(rc, 0);
            CHECK(o.find("OCSP Response Status: successful") != std::string::npos);
            CHECK(o.find("Cert Status: good") != std::string::npos);
            ++i;
        }
        // The CRL path: provoke a CRL and let openssl read it.
        LtvEnv crlEnv;
        crlEnv.transport->fail(kOcspRoot);
        auto c = signPdf(makeFixture(), crlEnv.request(PadesLevel::BaselineLT), *crlEnv.provider, *crlEnv.identity);
        CHECK(c.has_value());
        if (c) {
            const DssData dc = dssOf(c->data);
            CHECK(!dc.global.crls.empty());
            if (!dc.global.crls.empty()) {
                ctest::writeFile(dir / "x.crl", dc.global.crls[0].data);
                const int rc = runCommand("openssl crl -inform DER -noout -text -in '" + (dir / "x.crl").string() + "'", o);
                std::printf("    openssl crl -> rc=%d %s\n", rc, o.substr(0, 120).c_str());
                CHECK_EQ(rc, 0);
                CHECK(o.find("Certificate Revocation List") != std::string::npos);
            }
        }
    } else {
        std::printf("    SKIP openssl not installed\n");
    }

    if (toolAvailable("pdfsig")) {
        const int rc = runCommand("pdfsig '" + (dir / "lta.pdf").string() + "'", o);
        std::printf("    pdfsig rc=%d\n%s\n", rc, o.c_str());
        CHECK(o.find("Signature is Valid") != std::string::npos);
        CHECK(o.find("Signature #1") != std::string::npos);
        CHECK(o.find("Signature #2") != std::string::npos); // the archive time-stamp is listed too
    } else {
        std::printf("    SKIP pdfsig not installed\n");
    }
    if (toolAvailable("qpdf")) {
        const int rc = runCommand("qpdf --check '" + (dir / "lta.pdf").string() + "'", o);
        std::printf("    qpdf --check rc=%d %s\n", rc, o.c_str());
        CHECK(rc == 0 || rc == 768); // 0 = clean, 768 = warnings only (exit code 3)
    } else {
        std::printf("    SKIP qpdf not installed\n");
    }
    if (toolAvailable("mutool")) {
        const int rc = runCommand("mutool info '" + (dir / "lta.pdf").string() + "'", o);
        std::printf("    mutool info rc=%d\n", rc);
        CHECK_EQ(rc, 0);
    } else {
        std::printf("    SKIP mutool not installed\n");
    }
    if (toolAvailable("pdftk")) {
        const int rc = runCommand("pdftk '" + (dir / "lta.pdf").string() + "' dump_data", o);
        std::printf("    pdftk dump_data rc=%d\n", rc);
        CHECK_EQ(rc, 0);
    } else {
        std::printf("    SKIP pdftk not installed\n");
    }
}

RIVET_TEST(ltvBudgetsFailClosedWithClearReasons) {
    LtvEnv env;
    auto out = signPdf(makeFixture(), env.request(PadesLevel::BaselineLT), *env.provider, *env.identity);
    CHECK(out.has_value());
    if (!out) return;

    // Baseline: the document is B-LT under the default budgets.
    const auto base = env.verify(out->data);
    CHECK_EQ(base.size(), std::size_t{1});
    if (base.empty()) return;
    CHECK(base[0].level == PadesLevel::BaselineLT);
    CHECK(!base[0].ltv.limitExceeded);

    // DSS larger than the OCSP budget: not evaluated, flagged, never B-LT.
    VerifyOptions tight = env.verifyOpts();
    tight.limits.maxDssOcsps = 1;
    const auto tooMany = env.verify(out->data, &tight);
    CHECK_EQ(tooMany.size(), std::size_t{1});
    if (!tooMany.empty()) {
        CHECK(tooMany[0].ltv.limitExceeded);
        CHECK(!tooMany[0].ltv.evaluated);
        CHECK(tooMany[0].level != PadesLevel::BaselineLT);
        bool named = false;
        for (const auto& r : tooMany[0].ltv.reasons) named = named || r.find("OCSP responses") != std::string::npos;
        CHECK(named);
    }
    // An exhausted chain-building budget makes the chains incomplete, with the reason recorded.
    VerifyOptions noSteps = env.verifyOpts();
    noSteps.limits.maxChainSearchSteps = 0;
    const auto steps = env.verify(out->data, &noSteps);
    CHECK_EQ(steps.size(), std::size_t{1});
    if (!steps.empty()) {
        CHECK(steps[0].level != PadesLevel::BaselineLT);
        CHECK(!steps[0].ltv.signerEvidenceComplete);
        bool named = false;
        for (const auto& m : steps[0].ltv.missing) named = named || m.find("step budget") != std::string::npos;
        CHECK(named);
    }
}

RIVET_TEST(esicDeclarationIsCheckedWhenADssIsPresent) {
    LtvEnv env;
    SignRequest r = env.request(PadesLevel::BaselineLT);
    auto out = signPdf(makeFixture(), r, *env.provider, *env.identity);
    CHECK(out.has_value());
    if (!out) return;
    auto esicNote = [](const std::vector<PdfSignatureInfo>& sigs) {
        for (const PdfSignatureInfo& s : sigs)
            for (const std::string& l : s.ltv.limitations)
                if (l.find("ESIC") != std::string::npos) return true;
        return false;
    };
    CHECK(!esicNote(env.verify(out->data))); // written by the signer: declared

    // The same document with the extension name disturbed: the /DSS is there, the declaration is not.
    Bytes broken = out->data;
    const std::string key = "/ESIC";
    std::size_t hits = 0;
    for (auto it = broken.begin(); (it = std::search(it, broken.end(), key.begin(), key.end())) != broken.end(); it += 5) {
        *(it + 4) = 'X';
        ++hits;
    }
    CHECK_GE(hits, std::size_t{1});
    const auto sigs = env.verify(broken);
    CHECK(esicNote(sigs));
    for (const PdfSignatureInfo& s : sigs) CHECK(s.ltv.dssPresent); // reported, never fatal on its own
}
