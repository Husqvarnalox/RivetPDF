// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test-only X.509 fabrication with the Rivet der:: writer (no OpenSSL). The
// signature is a dummy value: these certificates exercise parsing and
// classification only, never signature verification.

#include "crypto/Der.hpp"
#include "crypto/Algorithms.hpp"
#include "crypto/Certificate.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::crypto::testing {

struct AttrSpec {
    std::string oid;
    std::string value;
    std::uint8_t tag = der::kUtf8String;
    bool sameRdnAsPrevious = false; // multi-valued RDN
};

struct CertBuild {
    std::vector<AttrSpec> subject;
    std::vector<AttrSpec> issuer;
    Bytes serial{0x01, 0x02, 0x03};
    std::chrono::system_clock::time_point notBefore = std::chrono::sys_days{std::chrono::year{2024} / 1 / 1};
    std::chrono::system_clock::time_point notAfter = std::chrono::sys_days{std::chrono::year{2034} / 1 / 1};

    std::string spkiAlgOid = "1.2.840.113549.1.1.1";
    Bytes spkiParams = der::nullValue(); // full DER of the parameters element; empty = absent
    Bytes keyBits;                       // BIT STRING content without the unused-bits octet
    std::string sigAlgOid = "1.2.840.113549.1.1.11";

    std::optional<std::uint16_t> keyUsage; // keyusage::k* bits; absent = no extension
    std::vector<std::string> eku;
    std::vector<std::string> policies;
    std::optional<bool> ca; // basicConstraints
    std::optional<int> pathLen;
    Bytes ski;
    Bytes aki;
};

inline Bytes encodeName(const std::vector<AttrSpec>& attrs) {
    std::vector<Bytes> rdns;
    std::vector<Bytes> current;
    auto flush = [&] {
        if (!current.empty()) rdns.push_back(der::setOf(current));
        current.clear();
    };
    for (const AttrSpec& a : attrs) {
        if (!a.sameRdnAsPrevious) flush();
        current.push_back(der::sequence({der::oidElement(a.oid), der::tlv(a.tag, viewOf(a.value))}));
    }
    flush();
    return der::sequence(rdns);
}

inline Bytes extension(std::string_view oid, const Bytes& value, bool critical = false) {
    std::vector<Bytes> parts{der::oidElement(oid)};
    if (critical) parts.push_back(der::tlv(0x01, Bytes{0xFF}));
    parts.push_back(der::octetString(value));
    return der::sequence(parts);
}

inline Bytes bitString(ByteView bits, std::uint8_t unused = 0) {
    Bytes content{unused};
    content.insert(content.end(), bits.begin(), bits.end());
    return der::tlv(der::kBitString, content);
}

inline Bytes buildCertificate(const CertBuild& b) {
    std::vector<Bytes> exts;
    if (b.keyUsage) {
        Bytes bits(2, 0);
        for (unsigned i = 0; i < 9; ++i)
            if (*b.keyUsage & (1u << i)) bits[i / 8] = static_cast<std::uint8_t>(bits[i / 8] | (0x80u >> (i % 8)));
        const std::size_t n = (*b.keyUsage & 0x100) ? 2 : 1;
        exts.push_back(extension("2.5.29.15", bitString(ByteView(bits.data(), n)), true));
    }
    if (b.ca) {
        std::vector<Bytes> bc;
        if (*b.ca) bc.push_back(der::tlv(0x01, Bytes{0xFF}));
        if (b.pathLen) bc.push_back(der::integerFromInt(*b.pathLen));
        exts.push_back(extension("2.5.29.19", der::sequence(bc), true));
    }
    if (!b.eku.empty()) {
        std::vector<Bytes> ids;
        for (const auto& o : b.eku) ids.push_back(der::oidElement(o));
        exts.push_back(extension("2.5.29.37", der::sequence(ids)));
    }
    if (!b.ski.empty()) exts.push_back(extension("2.5.29.14", der::octetString(b.ski)));
    if (!b.aki.empty()) exts.push_back(extension("2.5.29.35", der::sequence({der::tlv(0x80, b.aki)})));
    if (!b.policies.empty()) {
        std::vector<Bytes> infos;
        for (const auto& o : b.policies) infos.push_back(der::sequence({der::oidElement(o)}));
        exts.push_back(extension("2.5.29.32", der::sequence(infos)));
    }

    Bytes algId = b.spkiParams.empty() ? der::sequence({der::oidElement(b.spkiAlgOid)})
                                       : der::sequence({der::oidElement(b.spkiAlgOid), b.spkiParams});
    const Bytes spki = der::sequence({algId, bitString(b.keyBits)});
    const Bytes sigAlg = der::sequence({der::oidElement(b.sigAlgOid)});

    std::vector<Bytes> tbs{der::tlv(0xA0, der::integerFromInt(2)), der::integer(b.serial), sigAlg, encodeName(b.issuer),
                           der::sequence({der::timeElement(b.notBefore), der::timeElement(b.notAfter)}),
                           encodeName(b.subject), spki};
    if (!exts.empty()) tbs.push_back(der::tlv(0xA3, der::sequence(exts)));
    return der::sequence({der::sequence(tbs), sigAlg, bitString(Bytes(8, 0x5A))});
}

} // namespace rivet::crypto::testing
