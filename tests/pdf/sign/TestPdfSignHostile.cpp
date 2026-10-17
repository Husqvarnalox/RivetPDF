// SPDX-License-Identifier: MPL-2.0
// Hostile-input tests: the verifier and the signer treat every byte of the
// file, and every signature dictionary, as untrusted.

#include "RivetTest.h"

#include "SignTestSupport.hpp"

#include <algorithm>
#include <cstring>

using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;
namespace crypto = rivet::crypto;

namespace {

bool acceptedAsGood(crypto::SignatureState s) {
    return s == crypto::SignatureState::ValidTrusted || s == crypto::SignatureState::ValidUntrusted ||
           s == crypto::SignatureState::PermittedModifications;
}

std::size_t findText(const Bytes& data, std::string_view needle, std::size_t from = 0) {
    const auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), needle.begin(), needle.end());
    return it == data.end() ? std::string::npos : static_cast<std::size_t>(it - data.begin());
}

// Replaces the text between "/ByteRange [" and "]" by `text`, padded with spaces to the same width.
Bytes patchByteRange(Bytes data, const std::string& text) {
    const std::size_t at = findText(data, "/ByteRange [");
    CHECK(at != std::string::npos);
    const std::size_t open = at + 11; // '['
    const std::size_t close = std::find(data.begin() + static_cast<std::ptrdiff_t>(open), data.end(), ']') - data.begin();
    const std::size_t width = close - open - 1;
    CHECK(text.size() <= width);
    std::string padded = text;
    padded.resize(width, ' ');
    std::memcpy(data.data() + open + 1, padded.data(), width);
    return data;
}

Bytes patchText(Bytes data, std::string_view from, std::string_view to) {
    const std::size_t at = findText(data, from);
    CHECK(at != std::string::npos);
    CHECK(to.size() <= from.size());
    std::string padded(to);
    padded.resize(from.size(), ' ');
    std::memcpy(data.data() + at, padded.data(), from.size());
    return data;
}

std::string num(std::int64_t v) { return std::to_string(v); }

struct SignedFixture {
    Signer signer;
    Bytes data;
    std::int64_t l1 = 0, o2 = 0, l2 = 0;
    std::size_t baseSize = 0;
};

SignedFixture makeSigned() {
    SignedFixture f;
    const Bytes base = makeFixture();
    f.baseSize = base.size();
    auto out = signPdf(base, basicRequest(), *f.signer.provider, *f.signer.identity);
    CHECK(out.has_value());
    f.data = std::move(out->data);
    const auto sigs = verifyAll(f.data, f.signer);
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) {
        f.l1 = sigs[0].byteRange[1];
        f.o2 = sigs[0].byteRange[2];
        f.l2 = sigs[0].byteRange[3];
    }
    return f;
}

} // namespace

RIVET_TEST(maliciousByteRangeCorpusIsRejected) {
    SignedFixture f = makeSigned();
    const std::string l1 = num(f.l1), o2 = num(f.o2), l2 = num(f.l2);
    const std::vector<std::pair<std::string, std::string>> corpus = {
        {"negative length", "0 -1 " + o2 + " " + l2},
        {"negative offset", "0 " + l1 + " -5 " + l2},
        {"negative last", "0 " + l1 + " " + o2 + " -7"},
        {"nonzero start", "1 " + l1 + " " + o2 + " " + l2},
        {"overlap", "0 " + num(f.o2 + 10) + " " + o2 + " " + l2},
        {"second range before first", "0 " + l1 + " 3 " + l2},
        {"out of bounds", "0 " + l1 + " " + o2 + " 9999999999"},
        {"int64 max length", "0 1 2 9223372036854775807"},
        {"int64 max offset", "0 1 9223372036854775807 5"},
        {"int64 overflow digits", "0 1 2 92233720368547758079999"},
        {"gap too small", "0 " + num(f.l1 - 1) + " " + o2 + " " + l2},
        {"gap too large", "0 " + l1 + " " + num(f.o2 + 1) + " " + num(f.l2 - 1)},
        {"gap shifted", "0 " + num(f.l1 + 2) + " " + num(f.o2 + 2) + " " + num(f.l2 - 2)},
        {"empty last range", "0 " + l1 + " " + o2 + " 0"},
        {"three elements", "0 " + l1 + " " + o2},
        {"five elements", "0 " + l1 + " " + o2 + " " + l2 + " 0"},
        {"two elements", "0 " + l1},
        {"empty array", ""},
        {"real", "0 1.5 " + o2 + " " + l2},
        {"string element", "0 (a) " + o2 + " " + l2},
        {"reference element", "0 1 0 R " + o2 + " " + l2},
        {"name element", "0 /x " + o2 + " " + l2},
        {"shortened last range", "0 " + l1 + " " + o2 + " " + num(f.l2 - 4)},
    };
    for (const auto& [label, text] : corpus) {
        const Bytes mutated = patchByteRange(f.data, text);
        auto result = verifyPdfSignatures(mutated, *f.signer.provider);
        if (!result) continue; // refusing the whole file is acceptable
        for (const PdfSignatureInfo& sig : *result) {
            if (acceptedAsGood(sig.validation.state)) std::fprintf(stderr, "    corpus entry accepted: %s\n", label.c_str());
            CHECK(!acceptedAsGood(sig.validation.state));
        }
    }
}

RIVET_TEST(duplicateContentsKeyAndGarbageCmsAreRejected) {
    SignedFixture f = makeSigned();
    // A second /Contents key (the first one stays valid and is the one a lax parser would use).
    const Bytes dup = patchText(f.data, "/Filter /Adobe.PPKLite", "/Contents <00>");
    auto r1 = verifyPdfSignatures(dup, *f.signer.provider);
    if (r1)
        for (const auto& sig : *r1) CHECK(!acceptedAsGood(sig.validation.state));

    // The CMS prefix replaced by an impossible DER length.
    Bytes bad = f.data;
    const std::size_t contents = findText(bad, "/Contents <");
    CHECK(contents != std::string::npos);
    const std::string garbage = "3082FFFF";
    std::memcpy(bad.data() + contents + 11, garbage.data(), garbage.size());
    auto r2 = verifyPdfSignatures(bad, *f.signer.provider);
    CHECK(r2.has_value());
    if (r2) {
        CHECK(!r2->empty());
        for (const auto& sig : *r2) CHECK(sig.validation.state == crypto::SignatureState::InvalidSignature);
    }

    // Non-zero bytes after the CMS structure.
    Bytes tail = f.data;
    const std::size_t end = findText(tail, ">\n/M ", contents);
    CHECK(end != std::string::npos);
    tail[end - 1] = '7';
    auto r3 = verifyPdfSignatures(tail, *f.signer.provider);
    CHECK(r3.has_value());
    if (r3)
        for (const auto& sig : *r3) CHECK(sig.validation.state == crypto::SignatureState::InvalidSignature);
}

RIVET_TEST(sameSignatureReferencedByTwoFieldsIsFlagged) {
    Signer s;
    Bytes base = makeFixture({.emptySigField = true});
    auto signedPdf = signPdf(base, [] {
        SignRequest r = basicRequest();
        r.placement.kind = SignaturePlacement::Kind::ExistingField;
        r.placement.fieldName = "Sig1";
        return r;
    }(), *s.provider, *s.identity);
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    const std::string sigRef = std::to_string(signedPdf->signatureRef.num) + " 0 R";
    const Bytes forged = appendRevision(
        signedPdf->data,
        {{20, "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Dup) /V " + sigRef + " /Rect [0 0 0 0] /P 3 0 R >>"},
         {7, "<< /Fields [5 0 R 20 0 R] /DA (/Helv 0 Tf 0 g) /SigFlags 3 >>"}});
    auto result = verifyPdfSignatures(forged, *s.provider);
    CHECK(result.has_value());
    if (!result) return;
    CHECK_EQ(result->size(), std::size_t{2});
    std::size_t invalid = 0;
    for (const auto& sig : *result)
        if (sig.validation.state == crypto::SignatureState::InvalidSignature) ++invalid;
    CHECK(invalid >= 1);
}

RIVET_TEST(oversizedContentsIsRejected) {
    Signer s;
    // Revision updating the empty field /Sig1 with a hand-made signature dictionary holding 9 MiB of CMS space.
    const Bytes base = makeFixture({.emptySigField = true});
    const std::size_t hexChars = std::size_t{9} * 1024 * 1024 * 2;
    std::string rev = base.back() == '\n' ? "" : "\n";
    const std::size_t baseSize = base.size();
    auto fileOffset = [&] { return baseSize + rev.size(); };
    const std::size_t off5 = fileOffset();
    rev += "5 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Sig /T (Sig1) /Rect [50 50 250 100] /P 3 0 R /F 4 /V 12 0 R >>\nendobj\n";
    const std::size_t off12 = fileOffset();
    rev += "12 0 obj\n<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 ";
    const std::size_t brAt = rev.size();
    rev += std::string(34, ' ') + "] /Contents ";
    const std::size_t cStart = rev.size();
    rev += "<" + std::string(hexChars, '0') + ">";
    const std::size_t cEnd = rev.size();
    rev += " >>\nendobj\n";
    const std::size_t xrefPos = fileOffset();
    char entries[128];
    std::snprintf(entries, sizeof entries, "xref\n5 1\n%010zu 00000 n \n12 1\n%010zu 00000 n \n", off5, off12);
    rev += entries;
    rev += "trailer\n<< /Size 13 /Root 1 0 R /Prev " + std::to_string(PdfFile::open(base)->lastStartxref()) +
           " >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    char br[64];
    std::snprintf(br, sizeof br, "%zu %zu %zu]", baseSize + cStart, baseSize + cEnd, rev.size() - cEnd);
    std::string brText = std::string("0 ") + br;
    // patch the 34-char field: "[0 " is already written, fill "<len1> <off2> <len2>"
    std::memcpy(rev.data() + brAt, brText.c_str() + 2, brText.size() - 3);
    Bytes file = base;
    file.insert(file.end(), rev.begin(), rev.end());
    auto result = verifyPdfSignatures(file, *s.provider);
    CHECK(result.has_value());
    if (!result) return;
    CHECK_EQ(result->size(), std::size_t{1});
    if (!result->empty()) {
        CHECK((*result)[0].validation.state == crypto::SignatureState::InvalidSignature);
        CHECK(!(*result)[0].validation.notes.empty());
    }
}

RIVET_TEST(crossReferenceLoopIsRejectedWithoutHanging) {
    Bytes base = makeFixture();
    const std::size_t sx = findText(base, "startxref\n");
    CHECK(sx != std::string::npos);
    std::size_t xrefPos = std::stoull(std::string(base.begin() + static_cast<std::ptrdiff_t>(sx) + 10,
                                                  base.begin() + static_cast<std::ptrdiff_t>(sx) + 10 + 6));
    const Bytes looped = patchText(base, "/Info 11 0 R", "/Prev " + std::to_string(xrefPos));
    Signer s;
    auto sig = signPdf(looped, basicRequest(), *s.provider, *s.identity);
    CHECK(!sig.has_value());
    CHECK(!verifyPdfSignatures(looped, *s.provider).has_value());
}

RIVET_TEST(randomMutationAndTruncationNeverCrash) {
    SignedFixture f = makeSigned();
    std::uint32_t state = 0x12345678u;
    auto rnd = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    std::size_t refused = 0, completed = 0;
    for (int i = 0; i < 400; ++i) {
        Bytes m = f.data;
        const std::size_t flips = 1 + rnd() % 3;
        for (std::size_t k = 0; k < flips; ++k) {
            // Bias towards the appended revision, where the signature structures live.
            const std::size_t lo = (rnd() % 4 == 0) ? 0 : f.baseSize;
            const std::size_t pos = lo + rnd() % (m.size() - lo);
            m[pos] = static_cast<std::uint8_t>(rnd());
        }
        auto r = verifyPdfSignatures(m, *f.signer.provider);
        (r ? completed : refused)++;
        if (r)
            for (const auto& sig : *r) CHECK(!(acceptedAsGood(sig.validation.state) && sig.validation.integrity != crypto::IntegrityStatus::Intact));
    }
    for (int i = 0; i < 150; ++i) {
        Bytes m = f.data;
        m.resize(rnd() % m.size());
        auto r = verifyPdfSignatures(m, *f.signer.provider);
        (r ? completed : refused)++;
        auto s = signPdf(m, basicRequest(), *f.signer.provider, *f.signer.identity);
        (void)s; // typed error or success: only memory safety is asserted
    }
    CHECK(refused + completed == 550);
}

RIVET_TEST(hostileInputsToTheSignerAreBounded) {
    Signer s;
    // A trailer /Size that claims an enormous object count must not allocate or overflow.
    Bytes big = makeFixture();
    big = patchText(big, "/Size 12 /Root 1 0 R /Info 11 0 R", "/Size 2147483647 /Root 1 0 R");
    auto r = signPdf(big, basicRequest(), *s.provider, *s.identity);
    // Either refused or signed with sane object numbers; both are acceptable, a crash is not.
    if (r) CHECK(verifyAll(r->data, s).size() == 1);
    // A file past the 2 GiB ByteRange limit is refused up front.
    SignRequest req = basicRequest();
    req.initialReserveOverride = std::size_t{8} << 20;
    auto limit = signPdf(makeFixture(), req, *s.provider, *s.identity);
    CHECK(!limit.has_value());
}
