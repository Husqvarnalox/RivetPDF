// SPDX-License-Identifier: MPL-2.0
#include "pdf/encrypt/StandardHandler.hpp"

#include "crypto/Providers.hpp"
#include "crypto/Symmetric.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rivet::pdf::encrypt {

namespace {

constexpr const char* kSubsystem = "pdf.encrypt";

core::Error fromCrypto(const crypto::CryptoError& e) {
    if (e.code == crypto::CryptoErrorCode::BackendUnavailable)
        return core::makeError(core::ErrorCode::NotAvailable, e.message, kSubsystem);
    return crypto::toCoreError(e);
}

core::Error invalidArgument(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), kSubsystem);
}

// The 32-byte password padding string of ISO 32000-1 Algorithm 2.
constexpr std::array<std::uint8_t, 32> kPad = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                               0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                               0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

Bytes padPassword(ByteView password) {
    Bytes out(password.begin(), password.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(password.size(), 32)));
    out.insert(out.end(), kPad.begin(), kPad.begin() + static_cast<std::ptrdiff_t>(32 - out.size()));
    return out;
}

void append(Bytes& to, ByteView from) { to.insert(to.end(), from.begin(), from.end()); }

core::Result<Bytes> randomBytes(std::size_t n) {
    auto provider = crypto::makeDefaultProvider();
    auto r = provider->randomBytes(n);
    if (!r) return std::unexpected(fromCrypto(r.error()));
    return std::move(*r);
}

// RC4 (the standard handler's R2-R4 password entries are defined with it; it
// is NEVER used for document content here).
Bytes rc4(ByteView key, ByteView data) {
    std::array<std::uint8_t, 256> s{};
    for (std::size_t i = 0; i < 256; ++i) s[i] = static_cast<std::uint8_t>(i);
    std::uint8_t j = 0;
    for (std::size_t i = 0; i < 256; ++i) {
        j = static_cast<std::uint8_t>(j + s[i] + key[i % key.size()]);
        std::swap(s[i], s[j]);
    }
    Bytes out(data.size());
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    for (std::size_t k = 0; k < data.size(); ++k) {
        a = static_cast<std::uint8_t>(a + 1);
        b = static_cast<std::uint8_t>(b + s[a]);
        std::swap(s[a], s[b]);
        out[k] = data[k] ^ s[static_cast<std::uint8_t>(s[a] + s[b])];
    }
    return out;
}

Bytes xorKey(ByteView key, std::uint8_t v) {
    Bytes out(key.begin(), key.end());
    for (auto& c : out) c ^= v;
    return out;
}

void putLe32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// ---- UTF-8 ----------------------------------------------------------------

bool decodeUtf8(std::string_view s, std::vector<char32_t>& out) {
    for (std::size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        char32_t cp = len == 1 ? lead : lead & (0xFFu >> (len + 1));
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        static constexpr char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        out.push_back(cp);
        i += len;
    }
    return true;
}

std::size_t encodedLength(char32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

void encodeUtf8(char32_t cp, Bytes& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<std::uint8_t>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<std::uint8_t>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<std::uint8_t>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
    }
}

bool isNonAsciiSpace(char32_t c) {
    return c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool isMappedToNothing(char32_t c) {
    return c == 0x00AD || c == 0x034F || c == 0x1806 || (c >= 0x180B && c <= 0x180D) || (c >= 0x200B && c <= 0x200D) ||
           c == 0x2060 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF;
}

bool isControl(char32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029; }

// ---- hex / serialization ---------------------------------------------------

std::string hexString(ByteView data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "<";
    for (const std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    out += '>';
    return out;
}

core::Result<Bytes> cbcNoPad(ByteView key, ByteView iv, ByteView data, bool encrypt) {
    auto r = encrypt ? crypto::aesEncrypt(crypto::AesMode::Cbc, key, iv, data, false)
                     : crypto::aesDecrypt(crypto::AesMode::Cbc, key, iv, data, false);
    if (!r) return std::unexpected(fromCrypto(r.error()));
    return std::move(*r);
}

} // namespace

core::Result<Bytes> prepareR6Password(std::string_view utf8) {
    std::vector<char32_t> cps;
    if (!decodeUtf8(utf8, cps)) return std::unexpected(invalidArgument("password is not valid UTF-8"));
    Bytes out;
    for (char32_t cp : cps) {
        if (isMappedToNothing(cp)) continue;
        if (isNonAsciiSpace(cp)) cp = 0x20;
        if (isControl(cp)) return std::unexpected(invalidArgument("password contains a control character"));
        if (out.size() + encodedLength(cp) > 127) break;
        encodeUtf8(cp, out);
    }
    return out;
}

core::Result<Bytes> prepareR4Password(std::string_view utf8) {
    std::vector<char32_t> cps;
    if (!decodeUtf8(utf8, cps)) return std::unexpected(invalidArgument("password is not valid UTF-8"));
    Bytes out;
    for (const char32_t cp : cps) {
        if (cp > 0xFF)
            return std::unexpected(invalidArgument("AES-128 (revision 4) passwords are limited to Latin-1 characters; use AES-256"));
        if (out.size() < 32) out.push_back(static_cast<std::uint8_t>(cp));
    }
    return out;
}

core::Result<Bytes> hashR6(ByteView password, ByteView salt, ByteView udata) {
    auto provider = crypto::makeDefaultProvider();
    auto digestOf = [&](crypto::DigestAlgorithm alg, ByteView data) -> core::Result<Bytes> {
        auto r = provider->digest(alg, data);
        if (!r) return std::unexpected(fromCrypto(r.error()));
        return std::move(*r);
    };

    Bytes seed;
    append(seed, password);
    append(seed, salt);
    append(seed, udata);
    auto k = digestOf(crypto::DigestAlgorithm::Sha256, seed);
    if (!k) return k;

    Bytes e;
    for (int round = 0; round < 64 || static_cast<int>(e.back()) > round - 32; ++round) {
        Bytes unit;
        append(unit, password);
        append(unit, *k);
        append(unit, udata);
        Bytes k1;
        k1.reserve(unit.size() * 64);
        for (int i = 0; i < 64; ++i) append(k1, unit);

        auto enc = cbcNoPad(ByteView(k->data(), 16), ByteView(k->data() + 16, 16), k1, true);
        if (!enc) return enc;
        e = std::move(*enc);

        unsigned sum = 0;
        for (std::size_t i = 0; i < 16; ++i) sum += e[i];
        const unsigned mod = sum % 3;
        k = digestOf(mod == 0 ? crypto::DigestAlgorithm::Sha256
                     : mod == 1 ? crypto::DigestAlgorithm::Sha384
                                : crypto::DigestAlgorithm::Sha512,
                     e);
        if (!k) return k;
    }
    k->resize(32);
    return k;
}

std::int32_t permissionBits(const Permissions& p) {
    std::uint32_t bits = 0xFFFFF0C0u; // bits 7-8 and 13-32 are reserved and must be 1
    if (p.print) bits |= 1u << 2;
    if (p.modify) bits |= 1u << 3;
    if (p.copy) bits |= 1u << 4;
    if (p.annotate) bits |= 1u << 5;
    if (p.fillForms) bits |= 1u << 8;
    if (p.accessibility) bits |= 1u << 9;
    if (p.assemble) bits |= 1u << 10;
    if (p.print && p.printHighQuality) bits |= 1u << 11;
    return static_cast<std::int32_t>(bits);
}

namespace {

core::Result<EncryptionSetup> buildR6(const EncryptOptions& options) {
    EncryptionSetup s;
    s.cipher = Cipher::Aes256;
    s.encryptMetadata = options.encryptMetadata;
    s.p = permissionBits(options.permissions);

    auto userPw = prepareR6Password(options.userPassword);
    if (!userPw) return std::unexpected(userPw.error());
    Bytes ownerPw;
    if (options.ownerPassword.empty()) {
        auto rnd = randomBytes(32);
        if (!rnd) return std::unexpected(rnd.error());
        ownerPw = std::move(*rnd);
    } else {
        auto o = prepareR6Password(options.ownerPassword);
        if (!o) return std::unexpected(o.error());
        ownerPw = std::move(*o);
    }

    // salts: user validation, user key, owner validation, owner key; then the key and the Perms filler.
    auto rnd = randomBytes(32 + 32 + 4);
    if (!rnd) return std::unexpected(rnd.error());
    const std::uint8_t* r = rnd->data();
    s.fileKey.assign(r, r + 32);
    const ByteView uvs(r + 32, 8);
    const ByteView uks(r + 40, 8);
    const ByteView ovs(r + 48, 8);
    const ByteView oks(r + 56, 8);
    const std::uint8_t* filler = r + 64;
    const Bytes zeroIv(16, 0);

    // U / UE (empty "udata" for the user hash).
    auto uHash = hashR6(*userPw, uvs, {});
    if (!uHash) return std::unexpected(uHash.error());
    s.u = *uHash;
    append(s.u, uvs);
    append(s.u, uks);
    auto uKey = hashR6(*userPw, uks, {});
    if (!uKey) return std::unexpected(uKey.error());
    auto ue = cbcNoPad(*uKey, zeroIv, s.fileKey, true);
    if (!ue) return std::unexpected(ue.error());
    s.ue = std::move(*ue);

    // O / OE (the 48-byte U is the owner hash's extra input).
    auto oHash = hashR6(ownerPw, ovs, s.u);
    if (!oHash) return std::unexpected(oHash.error());
    s.o = *oHash;
    append(s.o, ovs);
    append(s.o, oks);
    auto oKey = hashR6(ownerPw, oks, s.u);
    if (!oKey) return std::unexpected(oKey.error());
    auto oe = cbcNoPad(*oKey, zeroIv, s.fileKey, true);
    if (!oe) return std::unexpected(oe.error());
    s.oe = std::move(*oe);

    // Perms: P (LE), 0xFFFFFFFF, 'T'/'F', "adb", 4 random bytes; AES-256-ECB under the file key.
    Bytes block;
    putLe32(block, static_cast<std::uint32_t>(s.p));
    putLe32(block, 0xFFFFFFFFu);
    block.push_back(s.encryptMetadata ? 'T' : 'F');
    block.push_back('a');
    block.push_back('d');
    block.push_back('b');
    block.insert(block.end(), filler, filler + 4);
    auto perms = crypto::aesEncrypt(crypto::AesMode::Ecb, s.fileKey, {}, block, false);
    if (!perms) return std::unexpected(fromCrypto(perms.error()));
    s.perms = std::move(*perms);
    return s;
}

core::Result<EncryptionSetup> buildR4(const EncryptOptions& options, ByteView id0) {
    EncryptionSetup s;
    s.cipher = Cipher::Aes128;
    s.encryptMetadata = options.encryptMetadata;
    s.p = permissionBits(options.permissions);

    auto userPw = prepareR4Password(options.userPassword);
    if (!userPw) return std::unexpected(userPw.error());
    Bytes ownerPw;
    if (options.ownerPassword.empty()) {
        auto rnd = randomBytes(32);
        if (!rnd) return std::unexpected(rnd.error());
        ownerPw = std::move(*rnd);
    } else {
        auto o = prepareR4Password(options.ownerPassword);
        if (!o) return std::unexpected(o.error());
        ownerPw = std::move(*o);
    }

    auto md5 = [](ByteView d) -> core::Result<Bytes> {
        auto r = crypto::md5(d);
        if (!r) return std::unexpected(fromCrypto(r.error()));
        return std::move(*r);
    };

    // Algorithm 3: /O.
    auto h = md5(padPassword(ownerPw));
    if (!h) return std::unexpected(h.error());
    for (int i = 0; i < 50; ++i) {
        h = md5(*h);
        if (!h) return std::unexpected(h.error());
    }
    const Bytes rc4Key(h->begin(), h->begin() + 16);
    Bytes o = rc4(rc4Key, padPassword(*userPw));
    for (std::uint8_t i = 1; i <= 19; ++i) o = rc4(xorKey(rc4Key, i), o);
    s.o = std::move(o);

    // Algorithm 2: the file key.
    Bytes in = padPassword(*userPw);
    append(in, s.o);
    putLe32(in, static_cast<std::uint32_t>(s.p));
    append(in, id0);
    if (!s.encryptMetadata) putLe32(in, 0xFFFFFFFFu);
    auto key = md5(in);
    if (!key) return std::unexpected(key.error());
    for (int i = 0; i < 50; ++i) {
        key = md5(ByteView(key->data(), 16));
        if (!key) return std::unexpected(key.error());
    }
    key->resize(16);
    s.fileKey = std::move(*key);

    // Algorithm 5: /U.
    Bytes uIn(kPad.begin(), kPad.end());
    append(uIn, id0);
    auto uh = md5(uIn);
    if (!uh) return std::unexpected(uh.error());
    Bytes u = rc4(s.fileKey, *uh);
    for (std::uint8_t i = 1; i <= 19; ++i) u = rc4(xorKey(s.fileKey, i), u);
    auto filler = randomBytes(16);
    if (!filler) return std::unexpected(filler.error());
    append(u, *filler);
    s.u = std::move(u);
    return s;
}

} // namespace

core::Result<EncryptionSetup> buildSetup(const EncryptOptions& options, ByteView id0) {
    return options.cipher == Cipher::Aes256 ? buildR6(options) : buildR4(options, id0);
}

std::string encryptDictionary(const EncryptionSetup& s) {
    const bool r6 = s.cipher == Cipher::Aes256;
    std::string d = "<< /Filter /Standard";
    d += r6 ? " /V 5 /R 6 /Length 256" : " /V 4 /R 4 /Length 128";
    d += " /CF << /StdCF << /AuthEvent /DocOpen /CFM ";
    d += r6 ? "/AESV3 /Length 32" : "/AESV2 /Length 16";
    d += " >> >> /StmF /StdCF /StrF /StdCF";
    d += " /O " + hexString(s.o) + " /U " + hexString(s.u);
    if (r6) d += " /OE " + hexString(s.oe) + " /UE " + hexString(s.ue);
    d += " /P " + std::to_string(s.p);
    if (r6) d += " /Perms " + hexString(s.perms);
    if (!s.encryptMetadata) d += " /EncryptMetadata false";
    d += " >>";
    return d;
}

core::Result<Bytes> objectKey(const EncryptionSetup& s, std::uint32_t num, std::uint32_t gen) {
    if (s.cipher == Cipher::Aes256) return s.fileKey;
    Bytes in = s.fileKey;
    in.push_back(static_cast<std::uint8_t>(num));
    in.push_back(static_cast<std::uint8_t>(num >> 8));
    in.push_back(static_cast<std::uint8_t>(num >> 16));
    in.push_back(static_cast<std::uint8_t>(gen));
    in.push_back(static_cast<std::uint8_t>(gen >> 8));
    for (const char c : std::string_view("sAlT")) in.push_back(static_cast<std::uint8_t>(c));
    auto h = crypto::md5(in);
    if (!h) return std::unexpected(fromCrypto(h.error()));
    h->resize(16); // min(n + 5, 16) with n = 16
    return std::move(*h);
}

core::Result<Bytes> encryptObjectData(const EncryptionSetup& s, std::uint32_t num, std::uint32_t gen, ByteView plain) {
    auto key = objectKey(s, num, gen);
    if (!key) return std::unexpected(key.error());
    auto iv = randomBytes(16);
    if (!iv) return std::unexpected(iv.error());
    auto ct = crypto::aesEncrypt(crypto::AesMode::Cbc, *key, *iv, plain, true);
    if (!ct) return std::unexpected(fromCrypto(ct.error()));
    Bytes out = std::move(*iv);
    append(out, *ct);
    return out;
}

} // namespace rivet::pdf::encrypt
