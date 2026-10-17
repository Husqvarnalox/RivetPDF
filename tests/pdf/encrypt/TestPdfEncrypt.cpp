// SPDX-License-Identifier: MPL-2.0
// PDF encryption (ADR-0046): key derivation, permission bits, structure of the
// output, decryption by an independent implementation of the standard's
// algorithms (this file) and refusal of unsupported input.

#include "RivetTest.h"

#include "EncryptTestSupport.hpp"

#include <algorithm>
#include <cstring>

using namespace rivet::test::enc;
namespace pe = rivet::pdf::encrypt;
namespace core = rivet::core;
namespace crypto = rivet::crypto;

namespace {

pe::EncryptOptions opts(pe::Cipher c, std::string user, std::string owner = {}, bool meta = true) {
    pe::EncryptOptions o;
    o.cipher = c;
    o.userPassword = std::move(user);
    o.ownerPassword = std::move(owner);
    o.encryptMetadata = meta;
    return o;
}

std::string encryptOk(const std::string& pdf, const pe::EncryptOptions& o) {
    auto r = pe::encryptPdf(toBytes(pdf), o);
    if (!r) std::fprintf(stderr, "    encryptPdf failed: %s\n", core::describe(r.error()).c_str());
    CHECK(r.has_value());
    return r ? toStr(*r) : std::string();
}

core::ErrorCode encryptFails(const std::string& pdf, const pe::EncryptOptions& o) {
    auto r = pe::encryptPdf(toBytes(pdf), o);
    if (r) std::fprintf(stderr, "    expected a refusal but encryptPdf succeeded\n");
    CHECK(!r.has_value());
    return r ? core::ErrorCode::None : r.error().code;
}

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

} // namespace

RIVET_TEST(permissionBitsFollowTheStandard) {
    pe::Permissions all;
    CHECK_EQ(static_cast<std::uint32_t>(pe::permissionBits(all)), 0xFFFFFFFCu);
    pe::Permissions none{false, false, false, false, false, false, false, false};
    CHECK_EQ(static_cast<std::uint32_t>(pe::permissionBits(none)), 0xFFFFF0C0u);

    struct Case {
        bool pe::Permissions::*member;
        std::uint32_t bit;
    } cases[] = {{&pe::Permissions::print, 1u << 2},       {&pe::Permissions::modify, 1u << 3},
                 {&pe::Permissions::copy, 1u << 4},        {&pe::Permissions::annotate, 1u << 5},
                 {&pe::Permissions::fillForms, 1u << 8},   {&pe::Permissions::accessibility, 1u << 9},
                 {&pe::Permissions::assemble, 1u << 10}};
    for (const auto& c : cases) {
        pe::Permissions p;
        p.*(c.member) = false;
        const auto bits = static_cast<std::uint32_t>(pe::permissionBits(p));
        std::fprintf(stderr, "    deny bit 0x%x -> P=0x%08x\n", c.bit, bits);
        // Printing off also drops the high-quality bit, which is meaningless without print.
        const std::uint32_t expected = 0xFFFFFFFCu & ~c.bit & (c.bit == (1u << 2) ? ~(1u << 11) : ~0u);
        CHECK_EQ(bits, expected);
    }
    pe::Permissions hq;
    hq.printHighQuality = false;
    CHECK_EQ(static_cast<std::uint32_t>(pe::permissionBits(hq)), 0xFFFFFFFCu & ~(1u << 11));
}

RIVET_TEST(r6PasswordPreparation) {
    auto plain = pe::prepareR6Password("hunter2");
    CHECK(plain.has_value());
    CHECK_EQ(toStr(*plain), std::string("hunter2"));

    // NBSP becomes a space, soft hyphen and zero-width space vanish.
    auto mapped = pe::prepareR6Password("a\xC2\xA0" "b\xC2\xAD" "c\xE2\x80\x8B" "d");
    CHECK(mapped.has_value());
    CHECK_EQ(toStr(*mapped), std::string("a bcd"));

    // 127-byte cap never splits a code point: 63 x 2-byte chars + 1 ASCII = 127, the next 2-byte char is dropped.
    std::string longPw;
    for (int i = 0; i < 70; ++i) longPw += "\xC3\xA9";
    auto cut = pe::prepareR6Password(longPw);
    CHECK(cut.has_value());
    CHECK_EQ(cut->size(), std::size_t{126});
    auto ascii = pe::prepareR6Password(std::string(300, 'x'));
    CHECK(ascii.has_value());
    CHECK_EQ(ascii->size(), std::size_t{127});

    CHECK(!pe::prepareR6Password(std::string("a\x01" "b")).has_value());
    CHECK(!pe::prepareR6Password(std::string("\xFF\xFE")).has_value());

    auto latin = pe::prepareR4Password("caf\xC3\xA9");
    CHECK(latin.has_value());
    CHECK_EQ(toStr(*latin), std::string("caf\xE9"));
    CHECK(!pe::prepareR4Password("\xE6\xBC\xA2").has_value());
}

RIVET_TEST(r6HashIsDeterministicAndInputSensitive) {
    const std::string pw = "password";
    const std::string salt = "saltsalt";
    auto a = pe::hashR6(view(pw), view(salt), {});
    auto b = pe::hashR6(view(pw), view(salt), {});
    auto c = pe::hashR6(view(pw), view("saltsalu"), {});
    auto d = pe::hashR6(view("passwore"), view(salt), {});
    auto e = pe::hashR6(view(pw), view(salt), view(std::string(48, 'u')));
    CHECK(a.has_value() && b.has_value() && c.has_value() && d.has_value() && e.has_value());
    CHECK_EQ(a->size(), std::size_t{32});
    CHECK(*a == *b);
    CHECK(*a != *c);
    CHECK(*a != *d);
    CHECK(*a != *e);
}

RIVET_TEST(r6SetupValidatesBothPasswordsAndPerms) {
    pe::EncryptOptions o = opts(pe::Cipher::Aes256, "user-pw", "owner-pw", false);
    o.permissions.copy = false;
    auto setup = pe::buildSetup(o, {});
    CHECK(setup.has_value());
    if (!setup) return;
    CHECK_EQ(setup->u.size(), std::size_t{48});
    CHECK_EQ(setup->o.size(), std::size_t{48});
    CHECK_EQ(setup->fileKey.size(), std::size_t{32});
    CHECK_EQ(setup->perms.size(), std::size_t{16});

    const std::vector<std::uint8_t> zeroIv(16, 0);
    auto check = [&](const std::string& pw, const pe::Bytes& entry, const pe::Bytes& wrapped, const pe::Bytes& extra) {
        auto prepared = pe::prepareR6Password(pw);
        CHECK(prepared.has_value());
        auto h = pe::hashR6(*prepared, pe::ByteView(entry.data() + 32, 8), extra);
        CHECK(h.has_value());
        if (!h || !std::equal(h->begin(), h->end(), entry.begin())) return false;
        auto k = pe::hashR6(*prepared, pe::ByteView(entry.data() + 40, 8), extra);
        CHECK(k.has_value());
        auto key = crypto::aesDecrypt(crypto::AesMode::Cbc, *k, zeroIv, wrapped, false);
        CHECK(key.has_value());
        return key && *key == setup->fileKey;
    };
    CHECK(check("user-pw", setup->u, setup->ue, {}));
    CHECK(!check("owner-pw", setup->u, setup->ue, {}));
    CHECK(!check("User-pw", setup->u, setup->ue, {}));
    CHECK(check("owner-pw", setup->o, setup->oe, setup->u));
    CHECK(!check("user-pw", setup->o, setup->oe, setup->u));

    auto perms = crypto::aesDecrypt(crypto::AesMode::Ecb, setup->fileKey, {}, setup->perms, false);
    CHECK(perms.has_value());
    if (!perms) return;
    std::uint32_t p = 0;
    for (int i = 0; i < 4; ++i) p |= static_cast<std::uint32_t>((*perms)[static_cast<std::size_t>(i)]) << (8 * i);
    CHECK_EQ(p, static_cast<std::uint32_t>(setup->p));
    CHECK_EQ(static_cast<std::uint32_t>(setup->p) & (1u << 4), 0u);
    CHECK_EQ((*perms)[4], std::uint8_t{0xFF});
    CHECK_EQ((*perms)[8], std::uint8_t{'F'}); // metadata not encrypted
    CHECK_EQ((*perms)[9], std::uint8_t{'a'});
    CHECK_EQ((*perms)[10], std::uint8_t{'d'});
    CHECK_EQ((*perms)[11], std::uint8_t{'b'});
}

RIVET_TEST(r6OutputDecryptsWithUserAndOwnerPassword) {
    const std::string src = samplePdf();
    const std::string out = encryptOk(src, opts(pe::Cipher::Aes256, "u-pass", "o-pass"));
    CHECK(out.rfind("%PDF-2.0", 0) == 0);
    CHECK(contains(out, "/AESV3"));
    CHECK(contains(out, "/R 6"));
    CHECK(!contains(out, "Secret body text"));
    CHECK(!contains(out, "SECRET-XMP-MARKER"));

    const std::string userKey = r6FileKey(out, "u-pass", false);
    const std::string ownerKey = r6FileKey(out, "o-pass", true);
    CHECK_EQ(userKey.size(), std::size_t{32});
    CHECK(userKey == ownerKey);
    CHECK(r6FileKey(out, "wrong", false).empty());
    CHECK(r6FileKey(out, "u-pass", true).empty());
    CHECK(r6FileKey(out, "o-pass", false).empty());

    // Content stream (indirect /Length in the source) and metadata stream.
    const std::string content = decryptWith(userKey, rawStream(out, 4));
    std::fprintf(stderr, "    content: %s\n", content.c_str());
    CHECK_EQ(content, std::string(kBody));
    CHECK_EQ(decryptWith(userKey, rawStream(out, 6)), std::string(kXmp));
    CHECK(contains(out, "/Length 7 0 R") == false);

    // Strings: literal with escapes (\351, \( \), octal, line continuation), UTF-16 hex, odd hex.
    const std::string title = decryptWith(userKey, hexAfter(out, "/Title"));
    std::fprintf(stderr, "    title: %s\n", title.c_str());
    CHECK_EQ(title, std::string("Caf\xE9 (draft) AB"));
    CHECK_EQ(decryptWith(userKey, hexAfter(out, "/Author")), std::string("\xFE\xFF\x00\x41\x00\x4C", 6));
    CHECK_EQ(decryptWith(userKey, hexAfter(out, "/Odd")), std::string("\x40", 1));

    // The new /ID was minted (the source had none) and /Info survived.
    CHECK(contains(out, "/ID ["));
    CHECK(contains(out, "/Info 5 0 R"));
}

RIVET_TEST(structureOffsetsAreExact) {
    for (const auto cipher : {pe::Cipher::Aes256, pe::Cipher::Aes128}) {
        const std::string out = encryptOk(samplePdf(), opts(cipher, "p"));
        const std::size_t sx = out.rfind("startxref\n");
        CHECK(sx != std::string::npos);
        const std::size_t xrefOff = std::stoul(out.substr(sx + 10));
        CHECK_EQ(out.compare(xrefOff, 5, "xref\n"), 0);
        const std::size_t nEntries = 9; // 0..7 + /Encrypt
        CHECK(contains(out, "xref\n0 " + std::to_string(nEntries) + "\n"));
        std::size_t p = xrefOff + std::string("xref\n0 9\n").size();
        for (std::size_t n = 0; n < nEntries; ++n) {
            const std::string entry = out.substr(p + n * 20, 20);
            CHECK_EQ(entry.size(), std::size_t{20});
            CHECK_EQ(entry[18], ' ');
            CHECK_EQ(entry[19], '\n');
            if (n == 0) continue;
            CHECK_EQ(entry[17], 'n');
            const std::size_t off = std::stoul(entry.substr(0, 10));
            const std::string expect = std::to_string(n) + " 0 obj";
            std::fprintf(stderr, "    obj %zu at %zu\n", n, off);
            CHECK_EQ(out.compare(off, expect.size(), expect), 0);
        }
        // Stream /Length equals the real data size.
        const std::string raw = rawStream(out, 4);
        const std::size_t o4 = out.find("\n4 0 obj");
        const std::size_t lenAt = out.find("/Length ", o4);
        CHECK_EQ(std::stoul(out.substr(lenAt + 8)), raw.size());
    }
}

RIVET_TEST(encryptMetadataFalseLeavesXmpReadable) {
    const std::string src = samplePdf();
    const std::string open = encryptOk(src, opts(pe::Cipher::Aes256, "u", "o", false));
    CHECK(contains(open, "SECRET-XMP-MARKER"));
    CHECK(contains(open, "/EncryptMetadata false"));
    CHECK(!contains(open, "Secret body text"));
    const std::string key = r6FileKey(open, "u", false);
    CHECK_EQ(decryptWith(key, rawStream(open, 4)), std::string(kBody));

    const std::string closed = encryptOk(src, opts(pe::Cipher::Aes256, "u", "o", true));
    CHECK(!contains(closed, "SECRET-XMP-MARKER"));
    CHECK(!contains(closed, "/EncryptMetadata"));

    const std::string open128 = encryptOk(src, opts(pe::Cipher::Aes128, "u", "o", false));
    CHECK(contains(open128, "SECRET-XMP-MARKER"));
    CHECK(contains(open128, "/EncryptMetadata false"));
}

RIVET_TEST(aes128UsesVersion16AndV4Dictionary) {
    const std::string out = encryptOk(samplePdf(), opts(pe::Cipher::Aes128, "u", "o"));
    CHECK(out.rfind("%PDF-1.7", 0) == 0); // never lowered
    CHECK(contains(out, "/AESV2"));
    CHECK(contains(out, "/V 4 /R 4 /Length 128"));
    CHECK(!contains(out, "Secret body text"));
    CHECK(!contains(out, "/OE"));
    const std::string old = encryptOk(buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] /Count 0 >>"}, "", "%PDF-1.4\n"),
                                      opts(pe::Cipher::Aes128, "u"));
    CHECK(old.rfind("%PDF-1.6", 0) == 0);
}

RIVET_TEST(randomIvsMakeEveryRunDifferent) {
    const std::string src = samplePdf();
    const auto o = opts(pe::Cipher::Aes256, "u", "o");
    const std::string a = encryptOk(src, o);
    const std::string b = encryptOk(src, o);
    CHECK(a != b);
    CHECK(rawStream(a, 4).substr(0, 16) != rawStream(b, 4).substr(0, 16));
    CHECK_EQ(rawStream(a, 4).size() % 16, std::size_t{0});
}

RIVET_TEST(existingIdIsKept) {
    const std::string src = samplePdf("/Info 5 0 R /ID [<00112233445566778899AABBCCDDEEFF> <0123456789ABCDEF0123456789ABCDEF>]");
    const std::string out = encryptOk(src, opts(pe::Cipher::Aes128, "u"));
    CHECK(contains(out, "<00112233445566778899aabbccddeeff><0123456789abcdef0123456789abcdef>"));
}

RIVET_TEST(refusesEncryptedSignedAndStructuredInput) {
    const std::string src = samplePdf();
    const auto o = opts(pe::Cipher::Aes256, "u");

    // Already encrypted (our own output).
    CHECK_EQ(encryptFails(encryptOk(src, o), o), core::ErrorCode::Unsupported);

    // A signed document: /ByteRange anywhere.
    const std::string signedPdf =
        buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] /Count 0 >>",
                  "<< /Type /Sig /Filter /Adobe.PPKLite /ByteRange [0 10 20 30] /Contents <00> >>"});
    CHECK_EQ(encryptFails(signedPdf, o), core::ErrorCode::Unsupported);

    // Object stream, xref stream, incremental update.
    const std::string objstm = buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] /Count 0 >>",
                                         stream("/Type /ObjStm /N 0 /First 0", "")});
    CHECK_EQ(encryptFails(objstm, o), core::ErrorCode::Unsupported);

    std::string xrefStream = "%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n";
    const std::size_t at = xrefStream.size();
    xrefStream += "2 0 obj\n<< /Type /XRef /Size 3 /W [1 2 1] /Root 1 0 R /Length 0 >>\nstream\n\nendstream\nendobj\nstartxref\n" +
                  std::to_string(at) + "\n%%EOF\n";
    CHECK_EQ(encryptFails(xrefStream, o), core::ErrorCode::Unsupported);

    CHECK_EQ(encryptFails(samplePdf("/Info 5 0 R /Prev 100"), o), core::ErrorCode::Unsupported);
    CHECK_EQ(encryptFails(buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< >>", "<< /Filter [/Crypt] /Length 0 >>\nstream\n\nendstream"}), o),
             core::ErrorCode::Unsupported);
}

RIVET_TEST(refusesMalformedInputAndBadOptions) {
    const auto o = opts(pe::Cipher::Aes256, "u");
    CHECK_EQ(encryptFails("", o), core::ErrorCode::InvalidDocument);
    CHECK_EQ(encryptFails("not a pdf at all, just some text", o), core::ErrorCode::InvalidDocument);

    std::string src = samplePdf();
    // Corrupt an xref offset: point object 3 into the middle of object 2.
    std::string bad = src;
    const std::size_t xr = bad.rfind("\nxref\n") + 1;
    const std::size_t e3 = xr + std::string("xref\n0 8\n").size() + 3 * 20;
    bad.replace(e3, 10, "0000000015");
    CHECK_EQ(encryptFails(bad, o), core::ErrorCode::InvalidDocument);

    // Wrong stream /Length.
    std::string lie = buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] /Count 0 >>",
                                "<< /Length 3 >>\nstream\nabcdefgh\nendstream"});
    CHECK_EQ(encryptFails(lie, o), core::ErrorCode::InvalidDocument);

    // Truncated file.
    CHECK_EQ(encryptFails(src.substr(0, src.size() / 2), o), core::ErrorCode::InvalidDocument);

    // Options.
    pe::EncryptOptions nothing;
    CHECK_EQ(encryptFails(src, nothing), core::ErrorCode::InvalidArgument);
    // Permission-only protection (empty user password, restricted rights) is allowed.
    pe::EncryptOptions permOnly;
    permOnly.permissions.print = false;
    CHECK(!encryptOk(src, permOnly).empty());
    CHECK_EQ(encryptFails(src, opts(pe::Cipher::Aes128, "\xE6\xBC\xA2")), core::ErrorCode::InvalidArgument);
    CHECK(!encryptOk(src, opts(pe::Cipher::Aes256, "\xE6\xBC\xA2\xE5\xAD\x97")).empty());
}

RIVET_TEST(hostileTokenInputsNeverCrash) {
    const auto o = opts(pe::Cipher::Aes256, "u");
    const std::string base = samplePdf();
    // Flip each byte region to junk; every outcome is either success or a typed error.
    std::size_t refused = 0;
    for (std::size_t i = 8; i < base.size(); i += 7) {
        std::string m = base;
        m[i] = static_cast<char>(m[i] ^ 0x5A);
        auto r = pe::encryptPdf(toBytes(m), o);
        if (!r) {
            ++refused;
            CHECK(r.error().code == core::ErrorCode::InvalidDocument || r.error().code == core::ErrorCode::Unsupported);
        }
    }
    std::fprintf(stderr, "    %zu of %zu mutations refused\n", refused, base.size() / 7);
    CHECK(refused > 0);
    // Deep nesting and unbalanced brackets.
    CHECK(!pe::encryptPdf(toBytes(buildPdf({"<< /Type /Catalog /Pages 2 0 R /X " + std::string(500, '[') + " >>", "<< >>"})), o).has_value());
    CHECK(!pe::encryptPdf(toBytes(buildPdf({"<< /Type /Catalog /Pages 2 0 R /X (unterminated >>", "<< >>"})), o).has_value());
}
