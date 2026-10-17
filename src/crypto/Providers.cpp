// SPDX-License-Identifier: MPL-2.0
#include "crypto/Providers.hpp"

#include <algorithm>

namespace rivet::crypto {

void InMemoryCertificateStore::add(const CertificateInfo& certificate) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool known = std::any_of(certificates_.begin(), certificates_.end(), [&](const CertificateInfo& c) {
        return c.sha256Fingerprint == certificate.sha256Fingerprint;
    });
    if (!known) certificates_.push_back(certificate);
}

std::vector<CertificateInfo> InMemoryCertificateStore::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return certificates_;
}

std::optional<CertificateInfo> InMemoryCertificateStore::findBySha256(std::string_view fingerprint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const CertificateInfo& c : certificates_)
        if (c.sha256Fingerprint == fingerprint) return c;
    return std::nullopt;
}

CertificateValidation AnchorSetValidator::validate(const CertificateValidationRequest& request) {
    return validateCertificate(request.certificate, request.intermediates, anchors_, *provider_, request.at);
}

} // namespace rivet::crypto
