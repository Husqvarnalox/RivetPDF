// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/Internal.hpp"
#include "crypto/revocation/Revocation.hpp"

namespace rivet::crypto {

namespace dr = rivet::crypto::der;
using namespace revocation_detail;

namespace {

constexpr std::uint8_t kUriTag = 0x86; // [6] IMPLICIT IA5String

std::string uriText(const dr::Element& e) {
    return std::string(reinterpret_cast<const char*>(e.content.data()), e.content.size());
}

// CRLDistributionPoints / FreshestCRL value.
CryptoOk parseDistributionPoints(ByteView value, std::vector<DistributionPointInfo>& out) {
    auto seq = dr::readSingle(value);
    if (!seq || seq->tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "malformed CRL distribution points");
    auto points = dr::children(*seq);
    if (!points) return std::unexpected(points.error());
    for (const dr::Element& dp : *points) {
        if (dp.tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "malformed distribution point");
        auto fields = dr::children(dp);
        if (!fields) return std::unexpected(fields.error());
        DistributionPointInfo info;
        for (const dr::Element& f : *fields) {
            if (f.tag == dr::contextTag(0, true)) { // distributionPoint
                auto names = dr::children(f);
                if (!names || names->empty()) return fail(CryptoErrorCode::CorruptData, "malformed distributionPoint name");
                const dr::Element& choice = (*names)[0];
                if (choice.tag == dr::contextTag(0, true)) { // fullName
                    auto gns = dr::children(choice);
                    if (!gns) return std::unexpected(gns.error());
                    for (const dr::Element& gn : *gns) {
                        info.generalNames.emplace_back(gn.full.begin(), gn.full.end());
                        if (gn.tag == kUriTag) info.uris.push_back(uriText(gn));
                    }
                } else if (choice.tag == dr::contextTag(1, true)) {
                    info.hasRelativeName = true;
                }
            } else if (f.tag == dr::contextTag(1, false)) {
                info.hasReasons = true;
            } else if (f.tag == dr::contextTag(2, true)) {
                info.hasCrlIssuer = true;
            }
        }
        out.push_back(std::move(info));
    }
    return {};
}

} // namespace

CryptoResult<RevocationExtensions> parseRevocationExtensions(ByteView certDer) {
    auto cert = dr::readSingle(certDer);
    if (!cert || cert->tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "certificate is not a SEQUENCE");
    auto top = dr::children(*cert);
    if (!top || top->empty() || (*top)[0].tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "malformed certificate");
    auto tbs = dr::children((*top)[0]);
    if (!tbs) return std::unexpected(tbs.error());

    RevocationExtensions out;
    for (const dr::Element& e : *tbs) {
        if (e.tag != dr::contextTag(3, true)) continue;
        auto inner = dr::children(e);
        if (!inner || inner->empty()) return fail(CryptoErrorCode::CorruptData, "malformed extensions");
        auto exts = readExtensions((*inner)[0]);
        if (!exts) return std::unexpected(exts.error());
        for (const RawExtension& ext : *exts) {
            if (ext.oid == kOidAia) {
                auto seq = dr::readSingle(ext.value);
                if (!seq || seq->tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "malformed authorityInfoAccess");
                auto descs = dr::children(*seq);
                if (!descs) return std::unexpected(descs.error());
                for (const dr::Element& d : *descs) {
                    auto parts = dr::children(d);
                    if (!parts || parts->size() != 2 || (*parts)[0].tag != dr::kOid)
                        return fail(CryptoErrorCode::CorruptData, "malformed AccessDescription");
                    auto method = dr::decodeOid((*parts)[0].content);
                    if (!method) return std::unexpected(method.error());
                    if ((*parts)[1].tag != kUriTag) continue; // only URI locations are of interest
                    if (*method == kOidAdOcsp) out.ocspUrls.push_back(uriText((*parts)[1]));
                    else if (*method == kOidAdCaIssuers) out.caIssuerUrls.push_back(uriText((*parts)[1]));
                }
            } else if (ext.oid == kOidCrlDp) {
                if (auto r = parseDistributionPoints(ext.value, out.crlDistributionPoints); !r) return std::unexpected(r.error());
            } else if (ext.oid == kOidFreshestCrl) {
                out.freshestCrlPresent = true;
                std::vector<DistributionPointInfo> dps;
                if (auto r = parseDistributionPoints(ext.value, dps); !r) return std::unexpected(r.error());
                for (const DistributionPointInfo& dp : dps)
                    for (const std::string& u : dp.uris) out.freshestCrlUrls.push_back(u);
            } else if (ext.oid == kOidOcspNoCheck) {
                out.ocspNoCheck = true;
            }
        }
    }
    return out;
}

std::optional<std::string> classifyRevocationUrl(std::string_view url) {
    constexpr std::size_t kMaxUrl = 2048;
    if (url.empty()) return "empty URL";
    if (url.size() > kMaxUrl) return "URL too long";
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) return "URL contains control, space or non-ASCII characters";
    auto startsWith = [&](std::string_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i]) return false;
        }
        return true;
    };
    if (startsWith("http://") || startsWith("https://")) return std::nullopt;
    const std::size_t colon = url.find(':');
    const std::string scheme = colon == std::string_view::npos ? std::string("(none)") : std::string(url.substr(0, colon));
    return "unsupported URL scheme '" + scheme + "' (only http and https are used)";
}

} // namespace rivet::crypto
