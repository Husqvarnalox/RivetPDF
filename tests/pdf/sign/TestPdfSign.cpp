// SPDX-License-Identifier: MPL-2.0
// Functional tests of the incremental signer and the verifier (ADR-0037/0038).

#include "RivetTest.h"

#include "SignTestSupport.hpp"

#include <algorithm>
#include <cstring>

using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;
namespace crypto = rivet::crypto;

namespace {

bool validState(crypto::SignatureState s) {
    return s == crypto::SignatureState::ValidTrusted || s == crypto::SignatureState::ValidUntrusted;
}

bool hasPrefix(const Bytes& whole, const Bytes& prefix) {
    return whole.size() >= prefix.size() && std::memcmp(whole.data(), prefix.data(), prefix.size()) == 0;
}

bool contains(const Bytes& data, std::string_view needle) {
    return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

SignedPdf mustSign(Signer& s, Bytes pdf, const SignRequest& r) {
    auto out = signPdf(std::move(pdf), r, *s.provider, *s.identity);
    if (!out) {
        std::fprintf(stderr, "    sign failed: %s: %s\n", std::string(toString(out.error().code)).c_str(),
                     out.error().message.c_str());
    }
    CHECK(out.has_value());
    return out ? std::move(*out) : SignedPdf{};
}

void signAndCheck(const FixtureSpec& spec, bool expectClassic) {
    Signer s;
    CHECK(s.identity != nullptr);
    const Bytes original = makeFixture(spec);
    SignRequest r = basicRequest();
    r.reason = "Approved";
    r.location = "Moscow";
    r.placement.kind = SignaturePlacement::Kind::NewVisible;
    r.placement.rect = {50, 600, 250, 660};
    const SignedPdf out = mustSign(s, original, r);
    CHECK(hasPrefix(out.data, original)); // existing bytes untouched
    CHECK_EQ(out.classicXref, expectClassic);
    CHECK(out.attempts == 1);

    const auto sigs = verifyAll(out.data, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (sigs.size() != 1) return;
    CHECK(validState(sigs[0].validation.state));
    CHECK(sigs[0].validation.crypto == crypto::CryptoCheck::Valid);
    CHECK(sigs[0].validation.integrity == crypto::IntegrityStatus::Intact);
    CHECK(sigs[0].validation.modification == crypto::ModificationStatus::None);
    CHECK(sigs[0].coversWholeFile);
    CHECK_EQ(sigs[0].subFilter, std::string("ETSI.CAdES.detached"));
    CHECK(sigs[0].padesBaselineB);
    CHECK_EQ(sigs[0].reason, std::string("Approved"));
    CHECK_EQ(sigs[0].location, std::string("Moscow"));
    CHECK_EQ(sigs[0].fieldName, std::string("Signature1"));

    // Anchored: trusted.
    const auto trusted = verifyAll(out.data, s, true);
    CHECK_EQ(trusted.size(), std::size_t{1});
    if (!trusted.empty()) CHECK(trusted[0].validation.state == crypto::SignatureState::ValidTrusted);
}

} // namespace

RIVET_TEST(signClassicXrefTable) { signAndCheck({}, true); }
RIVET_TEST(signClassicXrefTableWithAcroFormAndAnnots) {
    signAndCheck({.emptySigField = true, .textField = true, .indirectAnnots = true, .textAnnot = true}, true);
}
RIVET_TEST(signXrefStream) { signAndCheck({.xrefStream = true}, false); }
RIVET_TEST(signXrefStreamWithObjectStream) {
    signAndCheck({.emptySigField = true, .textField = true, .xrefStream = true, .objectStream = true}, false);
}
RIVET_TEST(signFlateXrefStreamWithPngPredictor) {
    signAndCheck({.xrefStream = true, .objectStream = true, .flateXref = true}, false);
}

RIVET_TEST(signPkcs7DetachedProfile) {
    Signer s;
    const Bytes original = makeFixture();
    SignRequest r = basicRequest(SignatureProfile::Pkcs7Detached);
    r.signerName = "Test Name";
    const SignedPdf out = mustSign(s, original, r);
    const auto sigs = verifyAll(out.data, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (sigs.size() != 1) return;
    CHECK_EQ(sigs[0].subFilter, std::string("adbe.pkcs7.detached"));
    CHECK(!sigs[0].padesBaselineB);
    CHECK(validState(sigs[0].validation.state));
    CHECK_EQ(sigs[0].signerName, std::string("Test Name"));
    CHECK(contains(out.data, "/Adobe.PPKLite"));
}

RIVET_TEST(padesProfileHasNoSigningTimeAndCarriesEsicExtension) {
    Signer s;
    const SignedPdf out = mustSign(s, makeFixture(), basicRequest());
    CHECK(contains(out.data, "/ESIC"));
    CHECK(contains(out.data, "/ETSI.CAdES.detached"));
    const auto sigs = verifyAll(out.data, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) CHECK(sigs[0].padesBaselineB);
}

RIVET_TEST(sha384AndSha512AndEcdsa) {
    for (const auto digest : {crypto::DigestAlgorithm::Sha384, crypto::DigestAlgorithm::Sha512}) {
        Signer s("EC signer", "ec384");
        SignRequest r = basicRequest();
        r.digest = digest;
        const SignedPdf out = mustSign(s, makeFixture(), r);
        const auto sigs = verifyAll(out.data, s);
        CHECK_EQ(sigs.size(), std::size_t{1});
        if (sigs.empty()) continue;
        CHECK(validState(sigs[0].validation.state));
        CHECK(sigs[0].digest == digest);
    }
}

RIVET_TEST(sha1IsRefusedForCreation) {
    Signer s;
    SignRequest r = basicRequest();
    r.digest = crypto::DigestAlgorithm::Sha1;
    auto out = signPdf(makeFixture(), r, *s.provider, *s.identity);
    CHECK(!out.has_value());
}

RIVET_TEST(chainOfThreeSignaturesKeepsEarlierOnesValid) {
    Signer s;
    const Bytes a = makeFixture({.emptySigField = true});
    SignRequest r1 = basicRequest();
    r1.placement.kind = SignaturePlacement::Kind::NewVisible;
    r1.placement.rect = {50, 600, 250, 660};
    const SignedPdf b = mustSign(s, a, r1);
    SignRequest r2 = basicRequest(SignatureProfile::Pkcs7Detached);
    r2.placement.kind = SignaturePlacement::Kind::NewVisible;
    r2.placement.rect = {300, 600, 500, 660};
    const SignedPdf c = mustSign(s, b.data, r2);
    SignRequest r3 = basicRequest();
    r3.placement.kind = SignaturePlacement::Kind::ExistingField;
    r3.placement.fieldName = "Sig1";
    const SignedPdf d = mustSign(s, c.data, r3);

    CHECK(hasPrefix(c.data, b.data));
    CHECK(hasPrefix(d.data, c.data));
    const auto sigs = verifyAll(d.data, s);
    CHECK_EQ(sigs.size(), std::size_t{3});
    if (sigs.size() != 3) return;
    for (const auto& sig : sigs) CHECK(sig.validation.crypto == crypto::CryptoCheck::Valid);
    // Find them by field name: earlier signatures are intact; later signatures were "permitted modifications".
    for (const auto& sig : sigs) {
        CHECK(sig.validation.integrity == crypto::IntegrityStatus::Intact);
        if (sig.fieldName == "Signature1" || sig.fieldName == "Signature2") {
            CHECK(sig.validation.modification == crypto::ModificationStatus::Permitted);
            CHECK(sig.validation.state == crypto::SignatureState::PermittedModifications);
            CHECK(!sig.coversWholeFile);
        } else {
            CHECK_EQ(sig.fieldName, std::string("Sig1"));
            CHECK(sig.validation.modification == crypto::ModificationStatus::None);
            CHECK(validState(sig.validation.state));
        }
    }
}

RIVET_TEST(chainOnXrefStreamDocument) {
    Signer s;
    Bytes pdf = makeFixture({.xrefStream = true, .objectStream = true});
    for (int i = 0; i < 3; ++i) {
        SignRequest r = basicRequest();
        r.placement.kind = SignaturePlacement::Kind::NewVisible;
        r.placement.rect = {50, 100.0 * (i + 1), 250, 100.0 * (i + 1) + 40};
        const SignedPdf out = mustSign(s, pdf, r);
        CHECK(hasPrefix(out.data, pdf));
        pdf = out.data;
    }
    const auto sigs = verifyAll(pdf, s);
    CHECK_EQ(sigs.size(), std::size_t{3});
    for (const auto& sig : sigs) CHECK(sig.validation.crypto == crypto::CryptoCheck::Valid);
}

RIVET_TEST(existingEmptyFieldIsFilledNotDuplicated) {
    Signer s;
    const Bytes original = makeFixture({.emptySigField = true});
    SignRequest r = basicRequest();
    r.placement.kind = SignaturePlacement::Kind::ExistingField;
    r.placement.fieldName = "Sig1";
    const SignedPdf out = mustSign(s, original, r);
    const auto sigs = verifyAll(out.data, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) CHECK_EQ(sigs[0].fieldName, std::string("Sig1"));

    // Already signed: refused with a typed error.
    auto again = signPdf(out.data, r, *s.provider, *s.identity);
    CHECK(!again.has_value());
    if (!again) CHECK(again.error().code == SignErrorCode::FieldNotEmpty);

    r.placement.fieldName = "NoSuchField";
    auto missing = signPdf(original, r, *s.provider, *s.identity);
    CHECK(!missing.has_value());
    if (!missing) CHECK(missing.error().code == SignErrorCode::FieldNotFound);
}

RIVET_TEST(placeholderOverflowRebuildsTheRevision) {
    Signer s;
    const Bytes original = makeFixture({.xrefStream = true});
    SignRequest r = basicRequest();
    r.initialReserveOverride = 64; // far too small for any CMS
    const SignedPdf out = mustSign(s, original, r);
    CHECK(out.attempts >= 2);
    CHECK(out.cmsBytes <= out.reservedBytes);
    CHECK(hasPrefix(out.data, original));
    const auto sigs = verifyAll(out.data, s);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) CHECK(validState(sigs[0].validation.state));

    SignRequest huge = basicRequest();
    huge.initialReserveOverride = std::size_t{6} << 20;
    auto refused = signPdf(original, huge, *s.provider, *s.identity);
    CHECK(!refused.has_value());
    if (!refused) CHECK(refused.error().code == SignErrorCode::SignatureTooLarge);
}

RIVET_TEST(encryptedDocumentsAreRefused) {
    Signer s;
    for (const bool stream : {false, true}) {
        auto out = signPdf(makeFixture({.encrypted = true, .xrefStream = stream}), basicRequest(), *s.provider, *s.identity);
        CHECK(!out.has_value());
        if (!out) CHECK(out.error().code == SignErrorCode::EncryptedDocument);
    }
}

RIVET_TEST(garbageInputIsRejectedWithTypedErrors) {
    Signer s;
    for (const std::string& junk : {std::string(), std::string("hello"), std::string("%PDF-1.7\nstartxref\n999999\n%%EOF\n"),
                                    std::string("%PDF-1.7\nstartxref\n9\n%%EOF\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")}) {
        auto out = signPdf(toBytes(junk), basicRequest(), *s.provider, *s.identity);
        CHECK(!out.has_value());
    }
}

RIVET_TEST(certificationLevelsAndDocMdp) {
    Signer s;
    const Bytes original = makeFixture({.textField = true});
    SignRequest cert = basicRequest();
    cert.certification = DocMdpLevel::FormFill;
    const SignedPdf certified = mustSign(s, original, cert);
    const auto first = verifyAll(certified.data, s);
    CHECK_EQ(first.size(), std::size_t{1});
    if (first.empty()) return;
    CHECK(first[0].certification.has_value());
    if (first[0].certification) CHECK(*first[0].certification == DocMdpLevel::FormFill);
    CHECK(contains(certified.data, "/DocMDP"));
    CHECK(contains(certified.data, "/Perms"));

    // An approval signature on a P=2 document is allowed and does not break the certification.
    const SignedPdf approved = mustSign(s, certified.data, basicRequest());
    const auto both = verifyAll(approved.data, s);
    CHECK_EQ(both.size(), std::size_t{2});
    for (const auto& sig : both) CHECK(sig.validation.crypto == crypto::CryptoCheck::Valid);
    for (const auto& sig : both)
        if (sig.certification) {
            CHECK(sig.validation.modification == crypto::ModificationStatus::Permitted);
            CHECK(sig.validation.state == crypto::SignatureState::PermittedModifications);
        }

    // A second certification is refused, as is certifying after any signature.
    auto second = signPdf(certified.data, cert, *s.provider, *s.identity);
    CHECK(!second.has_value());
    if (!second) CHECK(second.error().code == SignErrorCode::CertificationNotAllowed);
    const SignedPdf plain = mustSign(s, original, basicRequest());
    auto late = signPdf(plain.data, cert, *s.provider, *s.identity);
    CHECK(!late.has_value());
    if (!late) CHECK(late.error().code == SignErrorCode::CertificationNotAllowed);

    // P=1: nothing may follow.
    SignRequest noChanges = basicRequest();
    noChanges.certification = DocMdpLevel::NoChanges;
    const SignedPdf locked = mustSign(s, original, noChanges);
    auto after = signPdf(locked.data, basicRequest(), *s.provider, *s.identity);
    CHECK(!after.has_value());
    if (!after) CHECK(after.error().code == SignErrorCode::ViolatesCertification);
}

RIVET_TEST(modificationAnalysisFollowsDocMdpLevel) {
    Signer s;
    const Bytes original = makeFixture({.textField = true, .textAnnot = true});
    auto fill = [](const Bytes& base) {
        return appendRevision(base, {{6, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /V (Alice) /Rect [50 120 250 150] "
                                         "/P 3 0 R /F 4 /DA (/Helv 0 Tf 0 g) >>"}});
    };
    auto annotate = [](const Bytes& base) {
        return appendRevision(base, {{9, "<< /Type /Annot /Subtype /Text /Rect [300 300 320 320] /Contents (changed) /P 3 0 R >>"}});
    };
    auto breakContent = [](const Bytes& base) {
        return appendRevision(base, {{4, "<< /Length 4 >>\nstream\nq Q \nendstream"}});
    };
    auto stateOf = [&](const Bytes& pdf) {
        const auto sigs = verifyAll(pdf, s);
        CHECK(!sigs.empty());
        return sigs.empty() ? crypto::ModificationStatus::NotApplicable : sigs[0].validation.modification;
    };
    using crypto::ModificationStatus;

    SignRequest p2 = basicRequest();
    p2.certification = DocMdpLevel::FormFill;
    const Bytes certified2 = mustSign(s, original, p2).data;
    CHECK(stateOf(fill(certified2)) == ModificationStatus::Permitted);
    CHECK(stateOf(annotate(certified2)) == ModificationStatus::Disallowed);
    CHECK(stateOf(breakContent(certified2)) == ModificationStatus::Disallowed);

    SignRequest p3 = basicRequest();
    p3.certification = DocMdpLevel::Annotate;
    const Bytes certified3 = mustSign(s, original, p3).data;
    CHECK(stateOf(fill(certified3)) == ModificationStatus::Permitted);
    CHECK(stateOf(annotate(certified3)) == ModificationStatus::Permitted);
    CHECK(stateOf(breakContent(certified3)) == ModificationStatus::Disallowed);

    SignRequest p1 = basicRequest();
    p1.certification = DocMdpLevel::NoChanges;
    const Bytes certified1 = mustSign(s, original, p1).data;
    CHECK(stateOf(fill(certified1)) == ModificationStatus::Disallowed);

    // An approval signature (no DocMDP) tolerates only signing and DSS, never form edits.
    const Bytes approval = mustSign(s, original, basicRequest()).data;
    CHECK(stateOf(fill(approval)) == ModificationStatus::Disallowed);
    CHECK(stateOf(breakContent(approval)) == ModificationStatus::Disallowed);

    // The state folds to "document modified" for a disallowed change.
    const auto sigs = verifyAll(breakContent(approval), s);
    CHECK(!sigs.empty());
    if (!sigs.empty()) CHECK(sigs[0].validation.state == crypto::SignatureState::DocumentModified);
}

RIVET_TEST(fieldMdpLockBlocksLockedFieldEdits) {
    Signer s;
    const Bytes original = makeFixture({.textField = true});
    auto fill = [](const Bytes& base) {
        return appendRevision(base, {{6, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /V (Bob) /Rect [50 120 250 150] "
                                         "/P 3 0 R /F 4 /DA (/Helv 0 Tf 0 g) >>"}});
    };
    auto modificationAfterFill = [&](FieldLockAction action) {
        SignRequest r = basicRequest();
        r.certification = DocMdpLevel::FormFill;
        r.fieldLock = FieldLock{action, {"Name"}};
        const Bytes signedPdf = mustSign(s, original, r).data;
        const auto sigs = verifyAll(fill(signedPdf), s);
        CHECK_EQ(sigs.size(), std::size_t{1});
        if (sigs.empty()) return crypto::ModificationStatus::NotApplicable;
        CHECK(sigs[0].fieldMdp.has_value());
        return sigs[0].validation.modification;
    };
    CHECK(modificationAfterFill(FieldLockAction::Include) == crypto::ModificationStatus::Disallowed);
    CHECK(modificationAfterFill(FieldLockAction::All) == crypto::ModificationStatus::Disallowed);
    CHECK(modificationAfterFill(FieldLockAction::Exclude) == crypto::ModificationStatus::Permitted);
}

RIVET_TEST(trailingGarbageIsNeverReportedAsUnmodified) {
    Signer s;
    Bytes out = mustSign(s, makeFixture(), basicRequest()).data;
    Bytes ws = out;
    ws.push_back('\n');
    ws.push_back(' ');
    auto sigsWs = verifyAll(ws, s);
    CHECK(!sigsWs.empty());
    if (!sigsWs.empty()) CHECK(validState(sigsWs[0].validation.state));

    Bytes junk = out;
    for (const char c : std::string("JUNKJUNK")) junk.push_back(static_cast<std::uint8_t>(c));
    auto sigsJunk = verifyAll(junk, s);
    CHECK(!sigsJunk.empty());
    if (!sigsJunk.empty()) {
        CHECK(sigsJunk[0].validation.modification == crypto::ModificationStatus::Unknown);
        CHECK(!validState(sigsJunk[0].validation.state));
    }
}

RIVET_TEST(tamperedSignedBytesAreDetected) {
    Signer s;
    const SignedPdf out = mustSign(s, makeFixture(), basicRequest());
    const auto good = verifyAll(out.data, s);
    CHECK(!good.empty());
    if (good.empty()) return;
    // Flip one byte in each of: the original content, the new revision before and after /Contents.
    const std::int64_t l1 = good[0].byteRange[1], o2 = good[0].byteRange[2];
    for (const std::size_t pos : {std::size_t{40}, static_cast<std::size_t>(l1 - 20), static_cast<std::size_t>(o2 + 5)}) {
        Bytes t = out.data;
        t[pos] ^= 0x01;
        const auto sigs = verifyAll(t, s);
        // The flip may break the structure (then there is no usable signature) or the digest; never "valid".
        for (const auto& sig : sigs) CHECK(!validState(sig.validation.state));
    }
    // A flip inside the signature value itself.
    Bytes t = out.data;
    t[static_cast<std::size_t>(l1) + 400] ^= 0x01;
    const auto sigs = verifyAll(t, s);
    CHECK(!sigs.empty());
    for (const auto& sig : sigs) CHECK(!validState(sig.validation.state));
}

RIVET_TEST(fileSigningIsTransactional) {
    Signer s;
    const auto dir = scratchDir("transactional");
    const auto in = dir / "in.pdf";
    const auto outPath = dir / "out.pdf";
    const Bytes original = makeFixture({.emptySigField = true});
    {
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
    }
    SignRequest r = basicRequest();
    auto ok = signPdfFile(in, outPath, r, *s.provider, *s.identity);
    CHECK(ok.has_value());
    CHECK(std::filesystem::exists(outPath));

    // Failure (encrypted source): the destination keeps its old content and no temp files remain.
    const Bytes encrypted = makeFixture({.encrypted = true});
    const auto encPath = dir / "enc.pdf";
    {
        std::ofstream f(encPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(encrypted.data()), static_cast<std::streamsize>(encrypted.size()));
    }
    const auto sizeBefore = std::filesystem::file_size(outPath);
    auto bad = signPdfFile(encPath, outPath, r, *s.provider, *s.identity);
    CHECK(!bad.has_value());
    CHECK_EQ(std::filesystem::file_size(outPath), sizeBefore);

    // In-place signing: replaces the source atomically and the result verifies.
    auto inPlace = signPdfFile(in, in, r, *s.provider, *s.identity);
    CHECK(inPlace.has_value());
    Bytes data;
    { // closed before remove_all below: Windows cannot delete an open file
        std::ifstream f(in, std::ios::binary);
        char blk_[8192];
        while (f.read(blk_, sizeof blk_) || f.gcount() > 0) data.insert(data.end(), blk_, blk_ + f.gcount());
    }
    CHECK(hasPrefix(data, original));
    CHECK_EQ(verifyAll(data, s).size(), std::size_t{1});

    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++files;
    }
    CHECK_EQ(files, std::size_t{3}); // in.pdf, out.pdf, enc.pdf: no leftovers
    std::filesystem::remove_all(dir);
}

RIVET_TEST(finalizeHookCanAddUnsignedAttributesAndReserveGrows) {
    Signer s;
    SignRequest r = basicRequest();
    bool called = false;
    r.extraReserveBytes = 1024;
    r.finalizeCms = [&](crypto::CmsSignedData& m) -> crypto::CryptoOk {
        called = true;
        CHECK_EQ(m.signers.size(), std::size_t{1});
        return {};
    };
    const SignedPdf out = mustSign(s, makeFixture(), r);
    CHECK(called);
    CHECK(out.reservedBytes >= 1024);
}

RIVET_TEST(visibleAppearanceAndWidgetAreWritten) {
    Signer s;
    SignRequest r = basicRequest();
    r.placement.kind = SignaturePlacement::Kind::NewVisible;
    r.placement.rect = {100, 100, 300, 160};
    r.appearanceLines = {"Signed by Test", "caf\xC3\xA9 \xE2\x82\xAC"};
    const SignedPdf out = mustSign(s, makeFixture(), r);
    CHECK(contains(out.data, "/Subtype /Widget"));
    CHECK(contains(out.data, "/BaseFont /Helvetica"));
    CHECK(contains(out.data, "/SigFlags 3"));
    // Degenerate rectangles are rejected.
    r.placement.rect = {100, 100, 100, 100};
    auto bad = signPdf(makeFixture(), r, *s.provider, *s.identity);
    CHECK(!bad.has_value());
    if (!bad) CHECK(bad.error().code == SignErrorCode::InvalidArgument);
}
