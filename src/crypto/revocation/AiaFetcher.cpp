// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/AiaFetcher.hpp"

#include "crypto/Cms.hpp"
#include "crypto/revocation/Internal.hpp"

namespace rivet::crypto {

namespace {
constexpr std::size_t kMaxNotes = 16;
constexpr std::size_t kMaxBundleCerts = 16;
} // namespace

std::vector<Bytes> certificatesFromAiaBody(ByteView body, CryptoProvider& provider) {
    std::vector<Bytes> out;
    if (body.empty()) return out;
    auto keepIfCert = [&](const Bytes& der) {
        if (out.size() < kMaxBundleCerts && provider.parseCertificate(der)) out.push_back(der);
    };
    Bytes der(body.begin(), body.end());
    const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
    if (text.find("-----BEGIN") != std::string_view::npos) {
        if (text.find("CERTIFICATE") != std::string_view::npos && text.find("PKCS7") == std::string_view::npos) {
            if (auto d = decodePem(body)) keepIfCert(*d);
            return out;
        }
        if (auto cms = parseCmsAny(body))
            for (const Bytes& c : cms->certificates) keepIfCert(c);
        return out;
    }
    if (provider.parseCertificate(der)) {
        out.push_back(std::move(der));
        return out;
    }
    if (auto cms = parseCms(body))
        for (const Bytes& c : cms->certificates) keepIfCert(c);
    return out;
}

IssuerFetcher makeAiaFetcher(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider,
                             AiaFetchOptions options, std::shared_ptr<AiaFetchLog> log) {
    return [network = std::move(network), provider = std::move(provider), options = std::move(options),
            log = std::move(log)](const CertificateInfo& cert) -> std::vector<Bytes> {
        std::vector<Bytes> out;
        auto note = [&](std::string s) {
            if (log && log->notes.size() < kMaxNotes) log->notes.push_back(std::move(s));
        };
        if (!network || !provider) return out;
        auto ext = parseRevocationExtensions(cert.der);
        if (!ext) return out;
        std::size_t tried = 0;
        for (const std::string& url : ext->caIssuerUrls) {
            if (options.cancel.cancelled() || tried >= options.maxUrlsPerCertificate) break;
            if (const auto skip = classifyRevocationUrl(url)) {
                note("caIssuers URL skipped: " + *skip);
                continue;
            }
            ++tried;
            if (log) ++log->requests;
            auto response = revocation_detail::httpExchange(
                *network, net::HttpMethod::Get, url, {},
                {{"Accept", "application/pkix-cert, application/x-x509-ca-cert, application/pkcs7-mime, */*"}},
                options.maxResponseBytes, options.cancel);
            if (!response) {
                note("caIssuers fetch failed: " + response.error().message);
                if (revocation_detail::isGateRefusal(response.error())) break; // no repeated prompts
                continue;
            }
            if (response->status != 200) {
                note("caIssuers server answered HTTP " + std::to_string(response->status));
                continue;
            }
            const ByteView body(reinterpret_cast<const std::uint8_t*>(response->body.data()), response->body.size());
            std::vector<Bytes> certs = certificatesFromAiaBody(body, *provider);
            if (certs.empty()) {
                note("caIssuers response is not a certificate");
                continue;
            }
            if (log) log->certificatesAccepted += certs.size();
            for (Bytes& c : certs) out.push_back(std::move(c));
            break; // the first URL that yields certificates is enough
        }
        return out;
    };
}

} // namespace rivet::crypto
