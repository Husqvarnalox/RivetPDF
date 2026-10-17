// SPDX-License-Identifier: MPL-2.0
#include "crypto/Qualification.hpp"

#include "crypto/X509Parse.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>

namespace rivet::crypto {

std::string_view toString(SignatureClass c) {
    switch (c) {
    case SignatureClass::Unknown: return "Unknown";
    case SignatureClass::SimpleCrypto: return "Cryptographic signature";
    case SignatureClass::GostDigitalSignature: return "GOST digital signature";
    case SignatureClass::AdvancedUnqualified: return "Advanced (unqualified) signature";
    case SignatureClass::QualifiedEvidence: return "Qualified signature evidence";
    }
    return "Unknown";
}

std::string_view machineKey(SignatureClass c) {
    switch (c) {
    case SignatureClass::Unknown: return "unknown";
    case SignatureClass::SimpleCrypto: return "simple_crypto";
    case SignatureClass::GostDigitalSignature: return "gost_digital_signature";
    case SignatureClass::AdvancedUnqualified: return "advanced_unqualified";
    case SignatureClass::QualifiedEvidence: return "qualified_evidence";
    }
    return "unknown";
}

// ---- AccreditedCaList -------------------------------------------------------

namespace {

constexpr std::string_view kMagic = "rivet-accredited-ca-list";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::optional<Bytes> parseHex(std::string_view hex) {
    if (hex.empty() || hex.size() % 2 != 0) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

std::string lowerHex(const Bytes& b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t v : b) {
        out += kDigits[v >> 4];
        out += kDigits[v & 0xF];
    }
    return out;
}

bool validOid(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    std::size_t arcs = 1;
    bool digitSeen = false;
    for (const char c : s) {
        if (c == '.') {
            if (!digitSeen) return false;
            digitSeen = false;
            ++arcs;
        } else if (c >= '0' && c <= '9') {
            digitSeen = true;
        } else {
            return false;
        }
    }
    return digitSeen && arcs >= 2;
}

bool validProviderId(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    return std::ranges::all_of(s, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool validDate(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}

CryptoError lineError(std::size_t line, std::string_view what) {
    return CryptoError{CryptoErrorCode::CorruptData, "line " + std::to_string(line) + ": " + std::string(what)};
}

} // namespace

CryptoResult<AccreditedCaList> AccreditedCaList::parse(std::string_view text) {
    if (text.size() > kMaxBytes) return fail(CryptoErrorCode::LimitExceeded, "accredited CA list is larger than 1 MiB");

    AccreditedCaList list;
    bool haveMagic = false;
    bool haveUpdated = false;
    bool haveSource = false;
    std::size_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view raw = text.substr(pos, end - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (nl == std::string_view::npos && raw.empty()) break;
        ++lineNo;
        if (lineNo > kMaxLines) return fail(CryptoErrorCode::LimitExceeded, "accredited CA list has more than 100000 lines");

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        for (const char c : line)
            if (c == '\0') return std::unexpected(lineError(lineNo, "NUL character"));

        const std::size_t sp = line.find_first_of(" \t");
        const std::string_view directive = sp == std::string_view::npos ? line : line.substr(0, sp);
        const std::string_view rest = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));

        if (!haveMagic) {
            if (directive != kMagic) return std::unexpected(lineError(lineNo, "missing 'rivet-accredited-ca-list 1' header"));
            if (rest != "1") return std::unexpected(lineError(lineNo, "unsupported format version"));
            list.formatVersion = 1;
            haveMagic = true;
            continue;
        }

        if (directive == "updated") {
            if (haveUpdated) return std::unexpected(lineError(lineNo, "duplicate 'updated'"));
            if (!validDate(rest)) return std::unexpected(lineError(lineNo, "'updated' needs a YYYY-MM-DD date"));
            list.updated = std::string(rest);
            haveUpdated = true;
        } else if (directive == "source") {
            if (haveSource) return std::unexpected(lineError(lineNo, "duplicate 'source'"));
            if (rest.empty()) return std::unexpected(lineError(lineNo, "'source' needs text"));
            list.source = std::string(rest);
            haveSource = true;
        } else if (directive == "policy") {
            if (!validOid(rest)) return std::unexpected(lineError(lineNo, "'policy' needs a dotted OID"));
            list.qualifiedPolicyOids.insert(std::string(rest));
        } else if (directive == "csp") {
            if (!validProviderId(rest)) return std::unexpected(lineError(lineNo, "'csp' needs a provider id"));
            list.certifiedCspProviderIds.insert(std::string(rest));
        } else if (directive == "ca") {
            std::string_view r = rest;
            auto takeToken = [&](std::string_view key) -> std::optional<std::string_view> {
                if (!r.starts_with(key)) return std::nullopt;
                r.remove_prefix(key.size());
                const std::size_t e = r.find_first_of(" \t");
                const std::string_view v = e == std::string_view::npos ? r : r.substr(0, e);
                r = trim(e == std::string_view::npos ? std::string_view{} : r.substr(e));
                return v;
            };
            const auto fp = takeToken("sha256=");
            if (!fp) return std::unexpected(lineError(lineNo, "'ca' needs sha256=<hex> first"));
            const auto fpBytes = parseHex(*fp);
            if (!fpBytes || fpBytes->size() != 32) return std::unexpected(lineError(lineNo, "sha256 must be 64 hex digits"));
            AccreditedCaEntry entry;
            entry.sha256Fingerprint = lowerHex(*fpBytes);
            if (const auto ski = takeToken("ski=")) {
                auto skiBytes = parseHex(*ski);
                if (!skiBytes || skiBytes->size() > 64) return std::unexpected(lineError(lineNo, "ski must be 2..128 hex digits"));
                entry.subjectKeyId = std::move(*skiBytes);
            }
            if (r.starts_with("name=")) {
                entry.name = std::string(trim(r.substr(5)));
            } else if (!r.empty()) {
                return std::unexpected(lineError(lineNo, "unexpected text after sha256/ski (use name=...)"));
            }
            auto it = std::ranges::find_if(list.entries, [&](const AccreditedCaEntry& e) {
                return e.sha256Fingerprint == entry.sha256Fingerprint;
            });
            if (it == list.entries.end()) {
                list.entries.push_back(std::move(entry));
            } else {
                if (!entry.subjectKeyId.empty()) {
                    if (!it->subjectKeyId.empty() && it->subjectKeyId != entry.subjectKeyId)
                        return std::unexpected(lineError(lineNo, "conflicting ski for a repeated fingerprint"));
                    it->subjectKeyId = std::move(entry.subjectKeyId);
                }
                if (it->name.empty()) it->name = std::move(entry.name);
            }
        } else {
            return std::unexpected(lineError(lineNo, "unknown directive '" + std::string(directive.substr(0, 32)) + "'"));
        }
    }
    if (!haveMagic) return fail(CryptoErrorCode::CorruptData, "line 1: missing 'rivet-accredited-ca-list 1' header");
    return list;
}

CryptoResult<AccreditedCaList> AccreditedCaList::loadFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return fail(CryptoErrorCode::Io, "cannot read the accredited CA list: " + ec.message());
    if (size > kMaxBytes) return fail(CryptoErrorCode::LimitExceeded, "accredited CA list is larger than 1 MiB");
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(CryptoErrorCode::Io, "cannot open the accredited CA list");
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (in.gcount() != static_cast<std::streamsize>(text.size())) return fail(CryptoErrorCode::Io, "short read of the accredited CA list");
    return parse(text);
}

bool AccreditedCaList::matchesIssuer(const CertificateInfo& issuerCert) const {
    return std::ranges::any_of(entries, [&](const AccreditedCaEntry& e) {
        if (!issuerCert.sha256Fingerprint.empty() && e.sha256Fingerprint == issuerCert.sha256Fingerprint) return true;
        return !issuerCert.subjectKeyId.empty() && !e.subjectKeyId.empty() && e.subjectKeyId == issuerCert.subjectKeyId;
    });
}

// ---- classification -----------------------------------------------------------

namespace {

bool isGostKey(PublicKeyType t) { return t == PublicKeyType::Gost3410_2012_256 || t == PublicKeyType::Gost3410_2012_512; }

// The immediate issuer: same subject as the signer's issuer name, linked by
// AKI == SKI (a name match alone is never enough), not the signer itself.
bool issuerListed(const CertificateInfo& signer, const std::vector<CertificateInfo>& chain, const AccreditedCaList& list,
                  std::vector<std::string>& reasons) {
    if (signer.authorityKeyId.empty()) {
        reasons.emplace_back("signer certificate has no authority key identifier: issuer link not established");
        return false;
    }
    bool nameMatch = false;
    bool linked = false;
    for (const CertificateInfo& cand : chain) {
        if (cand.der == signer.der || cand.subjectDer != signer.issuerDer) continue;
        nameMatch = true;
        if (cand.subjectKeyId.empty() || cand.subjectKeyId != signer.authorityKeyId) continue;
        linked = true;
        if (list.matchesIssuer(cand)) return true;
    }
    if (!nameMatch) reasons.emplace_back("issuer certificate is not part of the chain");
    else if (!linked) reasons.emplace_back("issuer found by name only: authority/subject key identifiers do not link");
    else reasons.emplace_back("issuer is not in the accredited CA list");
    return false;
}

} // namespace

SignatureClassification classify(const ClassificationInput& in) {
    SignatureClassification out;
    auto& ev = out.evidence;

    PublicKeyType key = in.keyType.value_or(PublicKeyType::Unknown);
    if (key == PublicKeyType::Unknown && in.signer) key = in.signer->publicKeyType;
    if (key == PublicKeyType::Unknown && in.signature) key = keyTypeOf(*in.signature);
    const bool algorithmKnown = key != PublicKeyType::Unknown || in.signature.has_value();
    if (!algorithmKnown && !in.signer) {
        out.level = SignatureClass::Unknown;
        out.reasons.emplace_back("no key type, signature algorithm or certificate is known");
        return out;
    }

    ev.gostAlgorithm = isGostKey(key) || (in.signature && isGost(*in.signature));
    out.reasons.emplace_back(ev.gostAlgorithm ? "GOST R 34.10-2012 algorithm"
                                              : "non-GOST algorithm (RSA / ECDSA or unknown)");

    if (in.signer) {
        const CertificateInfo& s = *in.signer;
        ev.identityAttributes = s.commonName && (s.russian.inn || s.russian.snils || s.russian.ogrn || s.russian.ogrnip);
        ev.signatureKeyUsage =
            s.hasKeyUsage && (s.keyUsage & (keyusage::kDigitalSignature | keyusage::kNonRepudiation)) != 0;
        ev.certificateValidAtTime = s.validAt(in.validAt);
        out.reasons.emplace_back(ev.identityAttributes ? "subject carries a name and a Russian identity attribute"
                                                       : "no name plus INN/SNILS/OGRN/OGRNIP in the subject");
        out.reasons.emplace_back(ev.signatureKeyUsage ? "key usage allows signing" : "key usage does not allow signing (or is absent)");
        out.reasons.emplace_back(ev.certificateValidAtTime ? "certificate is within its validity period"
                                                           : "certificate is expired or not yet valid at the given time");
    } else {
        out.reasons.emplace_back("no signing certificate: identity, key usage and validity unknown");
    }
    ev.nonExportableKeyCustody = in.provider != ProviderAssurance::Software;
    out.reasons.emplace_back(ev.nonExportableKeyCustody ? "key is held by a non-exportable provider"
                                                        : "key is held by a software store (exportable)");

    if (in.list && in.signer) {
        ev.issuerInAccreditedList = issuerListed(*in.signer, in.chain, *in.list, out.reasons);
        if (ev.issuerInAccreditedList) out.reasons.emplace_back("issuer is linked by key identifier and listed as accredited");

        X509Extras extras;
        if (auto parsed = parseX509(in.signer->der, {}, &extras)) {
            ev.qualifiedPolicyPresent = std::ranges::any_of(
                extras.certificatePolicyOids, [&](const std::string& oid) { return in.list->qualifiedPolicy(oid); });
        }
        out.reasons.emplace_back(ev.qualifiedPolicyPresent ? "certificate policy marks a qualified certificate"
                                                           : "no listed qualified-certificate policy in the certificate");
        ev.certifiedCspDeclared = in.provider == ProviderAssurance::CertifiedCspDeclared && !in.providerId.empty() &&
                                  in.list->certifiedCspProviderIds.contains(in.providerId);
        out.reasons.emplace_back(ev.certifiedCspDeclared ? "provider is declared as a certified cryptographic means (not verified)"
                                                         : "provider is not declared as a certified cryptographic means");
    } else {
        out.reasons.emplace_back(in.list ? "no signing certificate to check against the accredited list"
                                         : "no accredited CA list loaded: qualified evidence cannot be established");
    }

    if (ev.gostAlgorithm && ev.identityAttributes && ev.signatureKeyUsage && ev.certificateValidAtTime &&
        ev.issuerInAccreditedList && ev.qualifiedPolicyPresent && ev.certifiedCspDeclared) {
        out.level = SignatureClass::QualifiedEvidence;
    } else if (ev.identityAttributes && ev.signatureKeyUsage && ev.nonExportableKeyCustody && ev.certificateValidAtTime) {
        out.level = SignatureClass::AdvancedUnqualified;
    } else {
        out.level = ev.gostAlgorithm ? SignatureClass::GostDigitalSignature : SignatureClass::SimpleCrypto;
    }
    return out;
}

} // namespace rivet::crypto
