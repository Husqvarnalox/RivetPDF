// SPDX-License-Identifier: MPL-2.0
#include "app/SecurityServices.hpp"

#include "crypto/Qualification.hpp"
#include "crypto/providers/platform/PlatformIdentityStore.hpp"
#include "net/Transport.hpp"
#include "platform/AppDataLocation.hpp"

#include <fstream>
#include <system_error>

namespace rivet::app {

namespace {

constexpr std::size_t kMaxBundleBytes = std::size_t{4} << 20;

std::string readSmallFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxBundleBytes) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) return {};
    return text;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

crypto::TrustAnchorSet parseAnchorBundle(crypto::CryptoProvider& provider, const std::string& text) {
    crypto::TrustAnchorSet set;
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    const auto add = [&](crypto::ByteView der) {
        if (auto cert = provider.parseCertificate(der)) set.add(*cert);
    };
    if (text.find(begin) == std::string::npos) {
        if (!text.empty()) add(crypto::viewOf(text)); // a single DER certificate
        return set;
    }
    std::size_t pos = 0;
    while ((pos = text.find(begin, pos)) != std::string::npos) {
        pos += begin.size();
        const std::size_t stop = text.find(end, pos);
        if (stop == std::string::npos) break;
        crypto::Bytes der;
        std::uint32_t acc = 0;
        int bits = 0;
        for (std::size_t i = pos; i < stop; ++i) {
            const int v = base64Value(text[i]);
            if (v < 0) continue;
            acc = (acc << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFU));
            }
        }
        add(der);
        pos = stop + end.size();
    }
    return set;
}

SecurityServices makeSecurityServices(platform::IAppDataLocation* appData) {
    SecurityServices services;
    services.provider = crypto::makeDefaultProvider();
    services.stores.push_back(crypto::makePlatformIdentityStore());
    services.networkAvailable = net::networkBackendAvailable();
    services.gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::Ask);
    if (services.networkAvailable) {
        services.network = std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, net::makeDefaultTransport(),
                                                                services.gate);
    }
    auto signerAnchors = std::make_shared<crypto::TrustAnchorSet>();
    auto timestampAnchors = std::make_shared<crypto::TrustAnchorSet>();
    if (appData != nullptr && services.provider && services.provider->isAvailable()) {
        if (auto directory = appData->directory(); directory.has_value()) {
            const std::filesystem::path trust = *directory / "trust";
            *signerAnchors = parseAnchorBundle(*services.provider, readSmallFile(trust / "signer-anchors.pem"));
            *timestampAnchors = parseAnchorBundle(*services.provider, readSmallFile(trust / "timestamp-anchors.pem"));
            const std::string accredited = readSmallFile(trust / "accredited-cas.txt");
            if (!accredited.empty()) {
                if (auto list = crypto::AccreditedCaList::parse(accredited)) {
                    services.accredited = std::make_shared<const crypto::AccreditedCaList>(std::move(*list));
                }
            }
        }
    }
    services.anchors = std::move(signerAnchors);
    services.timestampAnchors = std::move(timestampAnchors);
    return services;
}

} // namespace rivet::app
