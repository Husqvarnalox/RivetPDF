// SPDX-License-Identifier: MPL-2.0
#include "crypto/X509Parse.hpp"

#include "crypto/Der.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Backend-free X.509 reader.
//
// Name text (CertificateInfo::subject / issuer) follows the same convention as
// the OpenSSL provider (X509_NAME_print_ex with XN_FLAG_RFC2253):
//   * RDNs in reverse encoding order, separated by ',' ; the members of a
//     multi-valued RDN are separated by '+';
//   * attribute types with a well-known short name use it (CN, O, OU, C, L,
//     ST, street, DC, UID, serialNumber, GN, SN, title, emailAddress, ... and
//     the Russian INN / SNILS / OGRN / OGRNIP); every other OID, including
//     1.2.643.100.4 (INN of a legal entity), is rendered as the dotted OID;
//   * values of a known string type are converted to UTF-8 and escaped
//     (',' '+' '"' '\' '<' '>' ';' anywhere, a leading '#' or ' ', a trailing
//     ' ', control characters and every non-ASCII byte as \XX with upper-case
//     hex); a value that is not a string, or an attribute of unknown type,
//     is rendered as '#' + upper-case hex of the value's complete DER.
// X509Extras attribute lists carry the decoded UTF-8 values (unescaped);
// attributes whose value is not a decodable string are left out of them.

namespace rivet::crypto {

namespace {

using der::Element;

constexpr std::size_t kMaxCertificateSize = std::size_t{16} << 20;

// ---- small helpers --------------------------------------------------------

std::string hexLower(ByteView data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

std::string hexUpper(ByteView data) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(ByteView s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint8_t b = s[i];
        std::size_t extra = 0;
        char32_t cp = 0;
        if (b < 0x80) {
            ++i;
            continue;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            extra = 1;
            cp = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            extra = 2;
            cp = b & 0x0F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            extra = 3;
            cp = b & 0x07;
        } else {
            return false;
        }
        if (i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

// Decodes a directory-string element to UTF-8. nullopt for non-string types
// and malformed content.
std::optional<std::string> decodeText(const Element& v) {
    if (v.constructed || v.indefinite) return std::nullopt;
    const ByteView c = v.content;
    std::string out;
    switch (v.tag) {
    case 0x0C: // UTF8String
        if (!validUtf8(c)) return std::nullopt;
        out.assign(c.begin(), c.end());
        return out;
    case 0x12: // NumericString
    case 0x13: // PrintableString
    case 0x14: // TeletexString (T61, read as ISO 8859-1)
    case 0x16: // IA5String
    case 0x1A: // VisibleString
        for (const std::uint8_t b : c) appendUtf8(out, b);
        return out;
    case 0x1E: { // BMPString (UCS-2 big endian, surrogate pairs combined)
        if (c.size() % 2 != 0) return std::nullopt;
        for (std::size_t i = 0; i < c.size(); i += 2) {
            char32_t u = static_cast<char32_t>((c[i] << 8) | c[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < c.size()) {
                const char32_t lo = static_cast<char32_t>((c[i + 2] << 8) | c[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (u >= 0xD800 && u <= 0xDFFF) return std::nullopt;
            appendUtf8(out, u);
        }
        return out;
    }
    case 0x1C: { // UniversalString (UCS-4 big endian)
        if (c.size() % 4 != 0) return std::nullopt;
        for (std::size_t i = 0; i < c.size(); i += 4) {
            const char32_t u = (static_cast<char32_t>(c[i]) << 24) | (static_cast<char32_t>(c[i + 1]) << 16) |
                               (static_cast<char32_t>(c[i + 2]) << 8) | c[i + 3];
            if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return std::nullopt;
            appendUtf8(out, u);
        }
        return out;
    }
    default: return std::nullopt;
    }
}

struct AttrName {
    std::string_view oid;
    std::string_view name;
};

constexpr AttrName kAttrNames[] = {
    {"2.5.4.3", "CN"},
    {"2.5.4.4", "SN"},
    {"2.5.4.5", "serialNumber"},
    {"2.5.4.6", "C"},
    {"2.5.4.7", "L"},
    {"2.5.4.8", "ST"},
    {"2.5.4.9", "street"},
    {"2.5.4.10", "O"},
    {"2.5.4.11", "OU"},
    {"2.5.4.12", "title"},
    {"2.5.4.13", "description"},
    {"2.5.4.15", "businessCategory"},
    {"2.5.4.17", "postalCode"},
    {"2.5.4.41", "name"},
    {"2.5.4.42", "GN"},
    {"2.5.4.43", "initials"},
    {"2.5.4.44", "generationQualifier"},
    {"2.5.4.46", "dnQualifier"},
    {"2.5.4.65", "pseudonym"},
    {"0.9.2342.19200300.100.1.1", "UID"},
    {"0.9.2342.19200300.100.1.25", "DC"},
    {"1.2.840.113549.1.9.1", "emailAddress"},
    {"1.2.643.3.131.1.1", "INN"},
    {"1.2.643.100.1", "OGRN"},
    {"1.2.643.100.3", "SNILS"},
    {"1.2.643.100.5", "OGRNIP"},
};

std::string_view attrName(std::string_view oid) {
    for (const AttrName& a : kAttrNames)
        if (a.oid == oid) return a.name;
    return {};
}

std::string escapeRfc2253(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b >= 0x80 || b < 0x20 || b == 0x7F) {
            out += '\\';
            out += kHex[b >> 4];
            out += kHex[b & 0xF];
        } else if (b == ',' || b == '+' || b == '"' || b == '\\' || b == '<' || b == '>' || b == ';' ||
                   (i == 0 && (b == '#' || b == ' ')) || (i + 1 == n && b == ' ')) {
            out += '\\';
            out += static_cast<char>(b);
        } else {
            out += static_cast<char>(b);
        }
    }
    return out;
}

struct ParsedName {
    std::string text;
    std::vector<NameAttribute> attributes;
};

CryptoResult<ParsedName> parseName(const Element& name) {
    if (name.tag != der::kSequence) return fail(CryptoErrorCode::CorruptData, "Name is not a SEQUENCE");
    auto rdns = der::children(name);
    if (!rdns) return fail(CryptoErrorCode::CorruptData, "malformed Name");

    struct Flat {
        std::size_t set;
        std::string typeText; // dotted or short name
        std::string valueText;
    };
    std::vector<Flat> flat;
    ParsedName out;
    std::size_t setIndex = 0;
    for (const Element& rdn : *rdns) {
        if (rdn.tag != der::kSet) return fail(CryptoErrorCode::CorruptData, "RDN is not a SET");
        auto atvs = der::children(rdn);
        if (!atvs || atvs->empty()) return fail(CryptoErrorCode::CorruptData, "malformed RDN");
        for (const Element& atv : *atvs) {
            if (atv.tag != der::kSequence) return fail(CryptoErrorCode::CorruptData, "AttributeTypeAndValue is not a SEQUENCE");
            auto parts = der::children(atv);
            if (!parts || parts->size() != 2 || (*parts)[0].tag != der::kOid)
                return fail(CryptoErrorCode::CorruptData, "malformed AttributeTypeAndValue");
            auto oid = der::decodeOid((*parts)[0].content);
            if (!oid) return fail(CryptoErrorCode::CorruptData, "malformed attribute OID");
            const Element& value = (*parts)[1];
            const std::string_view shortName = attrName(*oid);
            Flat f;
            f.set = setIndex;
            f.typeText = shortName.empty() ? *oid : std::string(shortName);
            const auto text = decodeText(value);
            // Unknown attribute types and non-string values: dump the value's DER.
            f.valueText = (text && !shortName.empty()) ? escapeRfc2253(*text) : "#" + hexUpper(value.full);
            if (text) out.attributes.push_back(NameAttribute{*oid, *text});
            flat.push_back(std::move(f));
        }
        ++setIndex;
    }
    for (std::size_t i = flat.size(); i-- > 0;) {
        out.text += flat[i].typeText;
        out.text += '=';
        out.text += flat[i].valueText;
        if (i > 0) out.text += flat[i].set == flat[i - 1].set ? '+' : ',';
    }
    return out;
}

// ---- SubjectPublicKeyInfo -----------------------------------------------------

struct KeyInfo {
    PublicKeyType type = PublicKeyType::Unknown;
    int bits = 0;
    std::string algorithmOid;
    std::vector<std::string> parameterOids;
    Bytes keyBits;
};

int bitLength(ByteView magnitude) {
    std::size_t i = 0;
    while (i < magnitude.size() && magnitude[i] == 0) ++i;
    if (i == magnitude.size()) return 0;
    int bits = static_cast<int>((magnitude.size() - i) * 8);
    std::uint8_t top = magnitude[i];
    while ((top & 0x80) == 0) {
        top = static_cast<std::uint8_t>(top << 1);
        --bits;
    }
    return bits;
}

CryptoResult<KeyInfo> parseSpki(const Element& spki) {
    auto parts = der::children(spki);
    if (!parts || parts->size() != 2 || (*parts)[0].tag != der::kSequence || (*parts)[1].tag != der::kBitString)
        return fail(CryptoErrorCode::CorruptData, "malformed SubjectPublicKeyInfo");
    auto algParts = der::children((*parts)[0]);
    if (!algParts || algParts->empty() || (*algParts)[0].tag != der::kOid)
        return fail(CryptoErrorCode::CorruptData, "malformed public key algorithm");
    auto algOid = der::decodeOid((*algParts)[0].content);
    if (!algOid) return fail(CryptoErrorCode::CorruptData, "malformed public key algorithm OID");

    KeyInfo k;
    k.algorithmOid = *algOid;
    const ByteView bitContent = (*parts)[1].content;
    if (bitContent.empty() || bitContent[0] > 7) return fail(CryptoErrorCode::CorruptData, "malformed public key BIT STRING");
    k.keyBits.assign(bitContent.begin() + 1, bitContent.end());

    const Element* params = algParts->size() > 1 ? &(*algParts)[1] : nullptr;
    if (*algOid == oid::kRsaEncryption || *algOid == oid::kRsaPss) {
        k.type = PublicKeyType::Rsa;
        if (auto rsa = der::readElement(k.keyBits); rsa && rsa->tag == der::kSequence) {
            if (auto nums = der::children(*rsa); nums && nums->size() >= 2 && (*nums)[0].tag == der::kInteger)
                k.bits = bitLength((*nums)[0].content);
        }
    } else if (*algOid == oid::kEcPublicKey) {
        k.type = PublicKeyType::Ec;
        if (params && params->tag == der::kOid) {
            if (auto curve = der::decodeOid(params->content)) {
                k.parameterOids.push_back(*curve);
                if (*curve == "1.2.840.10045.3.1.7") k.bits = 256;
                else if (*curve == "1.3.132.0.34") k.bits = 384;
                else if (*curve == "1.3.132.0.35") k.bits = 521;
            }
        }
    } else if (*algOid == oid::kGost3410_256Key || *algOid == oid::kGost3410_512Key) {
        k.type = *algOid == oid::kGost3410_256Key ? PublicKeyType::Gost3410_2012_256 : PublicKeyType::Gost3410_2012_512;
        k.bits = k.type == PublicKeyType::Gost3410_2012_256 ? 256 : 512;
        if (params && params->tag == der::kSequence) {
            if (auto ps = der::children(*params)) {
                for (const Element& p : *ps) {
                    if (p.tag != der::kOid) continue;
                    if (auto o = der::decodeOid(p.content)) k.parameterOids.push_back(*o);
                }
            }
        }
    }
    return k;
}

// ---- extensions ------------------------------------------------------------------

std::uint16_t mapKeyUsage(ByteView bits) {
    // bits: BIT STRING content after the unused-bits octet; bit 0 is the MSB of byte 0.
    std::uint16_t ku = 0;
    for (unsigned i = 0; i < 9; ++i) {
        const std::size_t byte = i / 8;
        if (byte >= bits.size()) break;
        if (bits[byte] & (0x80u >> (i % 8))) ku = static_cast<std::uint16_t>(ku | (1u << i));
    }
    return ku;
}

CryptoOk parseExtensions(const Element& wrapper, CertificateInfo& c, X509Extras* extras) {
    auto inner = der::children(wrapper);
    if (!inner || inner->size() != 1 || (*inner)[0].tag != der::kSequence)
        return fail(CryptoErrorCode::CorruptData, "malformed extensions");
    auto exts = der::children((*inner)[0]);
    if (!exts) return fail(CryptoErrorCode::CorruptData, "malformed extensions");

    for (const Element& ext : *exts) {
        if (ext.tag != der::kSequence) return fail(CryptoErrorCode::CorruptData, "extension is not a SEQUENCE");
        auto parts = der::children(ext);
        if (!parts || parts->size() < 2 || parts->size() > 3 || (*parts)[0].tag != der::kOid)
            return fail(CryptoErrorCode::CorruptData, "malformed extension");
        auto extOid = der::decodeOid((*parts)[0].content);
        if (!extOid) return fail(CryptoErrorCode::CorruptData, "malformed extension OID");
        const Element& valueOctets = parts->back();
        if (valueOctets.tag != der::kOctetString || (parts->size() == 3 && (*parts)[1].tag != 0x01))
            return fail(CryptoErrorCode::CorruptData, "malformed extension value");
        auto raw = der::octetStringValue(valueOctets);
        if (!raw) return fail(CryptoErrorCode::CorruptData, "malformed extension value");
        auto value = der::readElement(*raw);
        if (!value) {
            // Unknown extensions may hold anything; known ones must parse.
            if (*extOid == "2.5.29.15" || *extOid == "2.5.29.37" || *extOid == "2.5.29.19" || *extOid == "2.5.29.14" ||
                *extOid == "2.5.29.35" || *extOid == "2.5.29.32")
                return fail(CryptoErrorCode::CorruptData, "malformed extension content");
            continue;
        }
        if (*extOid == "2.5.29.15") { // keyUsage
            if (value->tag != der::kBitString || value->content.empty() || value->content[0] > 7)
                return fail(CryptoErrorCode::CorruptData, "malformed keyUsage");
            c.hasKeyUsage = true;
            c.keyUsage = mapKeyUsage(value->content.subspan(1));
        } else if (*extOid == "2.5.29.37") { // extKeyUsage
            auto ids = der::children(*value);
            if (value->tag != der::kSequence || !ids) return fail(CryptoErrorCode::CorruptData, "malformed extKeyUsage");
            for (const Element& id : *ids) {
                if (id.tag != der::kOid) return fail(CryptoErrorCode::CorruptData, "malformed extKeyUsage OID");
                auto o = der::decodeOid(id.content);
                if (!o) return fail(CryptoErrorCode::CorruptData, "malformed extKeyUsage OID");
                c.extendedKeyUsageOids.push_back(*o);
            }
        } else if (*extOid == "2.5.29.19") { // basicConstraints
            auto items = der::children(*value);
            if (value->tag != der::kSequence || !items) return fail(CryptoErrorCode::CorruptData, "malformed basicConstraints");
            std::size_t i = 0;
            if (i < items->size() && (*items)[i].tag == 0x01) {
                c.isCa = !(*items)[i].content.empty() && (*items)[i].content[0] != 0;
                ++i;
            }
            if (i < items->size() && (*items)[i].tag == der::kInteger) {
                const Bytes n = der::normalizeInteger((*items)[i].content);
                if (n.size() > 3) return fail(CryptoErrorCode::CorruptData, "pathLenConstraint out of range");
                int len = 0;
                for (const std::uint8_t b : n) len = (len << 8) | b;
                c.pathLength = len;
            }
        } else if (*extOid == "2.5.29.14") { // subjectKeyIdentifier
            if (value->tag != der::kOctetString) return fail(CryptoErrorCode::CorruptData, "malformed subjectKeyIdentifier");
            c.subjectKeyId.assign(value->content.begin(), value->content.end());
        } else if (*extOid == "2.5.29.35") { // authorityKeyIdentifier
            auto items = der::children(*value);
            if (value->tag != der::kSequence || !items) return fail(CryptoErrorCode::CorruptData, "malformed authorityKeyIdentifier");
            for (const Element& item : *items)
                if (item.tag == 0x80) c.authorityKeyId.assign(item.content.begin(), item.content.end());
        } else if (*extOid == "2.5.29.32" && extras) { // certificatePolicies
            auto infos = der::children(*value);
            if (value->tag != der::kSequence || !infos) return fail(CryptoErrorCode::CorruptData, "malformed certificatePolicies");
            for (const Element& info : *infos) {
                auto fields = der::children(info);
                if (info.tag != der::kSequence || !fields || fields->empty() || (*fields)[0].tag != der::kOid)
                    return fail(CryptoErrorCode::CorruptData, "malformed PolicyInformation");
                auto o = der::decodeOid((*fields)[0].content);
                if (!o) return fail(CryptoErrorCode::CorruptData, "malformed policy OID");
                extras->certificatePolicyOids.push_back(*o);
            }
        }
    }
    return {};
}

void assignRussianAndNames(const std::vector<NameAttribute>& attrs, CertificateInfo& c) {
    for (const NameAttribute& a : attrs) {
        auto assign = [&](std::optional<std::string>& slot) {
            if (!slot) slot = a.value;
        };
        if (a.oid == "2.5.4.3") assign(c.commonName);
        else if (a.oid == "2.5.4.10") assign(c.organization);
        else if (a.oid == "1.2.840.113549.1.9.1") assign(c.email);
        else if (a.oid == "1.2.643.3.131.1.1") assign(c.russian.inn);
        else if (a.oid == "1.2.643.100.3") assign(c.russian.snils);
        else if (a.oid == "1.2.643.100.1") assign(c.russian.ogrn);
        else if (a.oid == "1.2.643.100.5") assign(c.russian.ogrnip);
        else if (a.oid == "1.2.643.100.4") assign(c.russian.innLe);
    }
}

CryptoResult<CertificateInfo> parseImpl(ByteView in, const DigestFn& digest, X509Extras* extras) {
    if (in.empty() || in.size() > kMaxCertificateSize) return fail(CryptoErrorCode::CorruptData, "certificate size is out of range");
    auto cert = der::readElement(in);
    if (!cert || cert->tag != der::kSequence) return fail(CryptoErrorCode::CorruptData, "not a valid X.509 certificate");

    CertificateInfo c;
    c.der.assign(cert->full.begin(), cert->full.end());

    auto top = der::children(*cert);
    if (!top || top->size() != 3 || (*top)[0].tag != der::kSequence || (*top)[1].tag != der::kSequence ||
        (*top)[2].tag != der::kBitString)
        return fail(CryptoErrorCode::CorruptData, "malformed Certificate structure");

    {
        auto sigParts = der::children((*top)[1]);
        if (!sigParts || sigParts->empty() || (*sigParts)[0].tag != der::kOid)
            return fail(CryptoErrorCode::CorruptData, "malformed signatureAlgorithm");
        auto o = der::decodeOid((*sigParts)[0].content);
        if (!o) return fail(CryptoErrorCode::CorruptData, "malformed signatureAlgorithm OID");
        c.signatureAlgorithmOid = *o;
    }

    auto tbs = der::children((*top)[0]);
    if (!tbs) return fail(CryptoErrorCode::CorruptData, "malformed TBSCertificate");
    std::size_t i = 0;
    auto need = [&](std::uint8_t tag, const char* what) -> const Element* {
        if (i >= tbs->size() || (*tbs)[i].tag != tag) {
            (void)what;
            return nullptr;
        }
        return &(*tbs)[i++];
    };
    if (i < tbs->size() && (*tbs)[i].tag == 0xA0) ++i; // version [0] EXPLICIT
    const Element* serial = need(der::kInteger, "serial");
    const Element* tbsSig = need(der::kSequence, "signature");
    const Element* issuer = need(der::kSequence, "issuer");
    const Element* validity = need(der::kSequence, "validity");
    const Element* subject = need(der::kSequence, "subject");
    const Element* spki = need(der::kSequence, "subjectPublicKeyInfo");
    if (!serial || !tbsSig || !issuer || !validity || !subject || !spki)
        return fail(CryptoErrorCode::CorruptData, "malformed TBSCertificate fields");

    c.serial = der::normalizeInteger(serial->content);
    c.serialHex = c.serial.empty() ? "00" : hexLower(c.serial);

    {
        auto times = der::children(*validity);
        if (!times || times->size() != 2) return fail(CryptoErrorCode::CorruptData, "malformed validity");
        auto nb = der::decodeTime((*times)[0]);
        auto na = der::decodeTime((*times)[1]);
        if (!nb || !na) return fail(CryptoErrorCode::CorruptData, "malformed validity time");
        c.notBefore = *nb;
        c.notAfter = *na;
    }

    auto issuerName = parseName(*issuer);
    if (!issuerName) return std::unexpected(issuerName.error());
    auto subjectName = parseName(*subject);
    if (!subjectName) return std::unexpected(subjectName.error());
    c.issuer = issuerName->text;
    c.subject = subjectName->text;
    c.issuerDer.assign(issuer->full.begin(), issuer->full.end());
    c.subjectDer.assign(subject->full.begin(), subject->full.end());
    assignRussianAndNames(subjectName->attributes, c);

    auto key = parseSpki(*spki);
    if (!key) return std::unexpected(key.error());
    c.publicKeyType = key->type;
    c.keyBits = key->bits;

    if (extras) *extras = X509Extras{};
    for (; i < tbs->size(); ++i) {
        const Element& e = (*tbs)[i];
        if (e.tag == 0xA3) {
            if (auto r = parseExtensions(e, c, extras); !r) return std::unexpected(r.error());
        } else if (e.tag != 0x81 && e.tag != 0x82) {
            return fail(CryptoErrorCode::CorruptData, "unexpected field in TBSCertificate");
        }
    }

    if (digest) {
        if (auto d = digest(DigestAlgorithm::Sha256, c.der); d) c.sha256Fingerprint = hexLower(*d);
        if (auto d = digest(DigestAlgorithm::Sha1, c.der); d) c.sha1Fingerprint = hexLower(*d);
    }

    if (extras) {
        extras->subjectAttributes = std::move(subjectName->attributes);
        extras->issuerAttributes = std::move(issuerName->attributes);
        extras->spkiDer.assign(spki->full.begin(), spki->full.end());
        extras->spkiAlgorithmOid = std::move(key->algorithmOid);
        extras->spkiParameterOids = std::move(key->parameterOids);
        extras->publicKeyBits = std::move(key->keyBits);
    }
    return c;
}

} // namespace

CryptoResult<CertificateInfo> parseX509(ByteView der, const DigestFn& digest, X509Extras* extras) {
    // Nothing below throws except allocation failure; keep the contract anyway.
    try {
        return parseImpl(der, digest, extras);
    } catch (const std::exception&) {
        return fail(CryptoErrorCode::Internal, "certificate parsing failed unexpectedly");
    }
}

} // namespace rivet::crypto
