// SPDX-License-Identifier: MPL-2.0
#include "crypto/CertificateDisplay.hpp"

#include <string_view>

namespace rivet::crypto {

namespace {

std::optional<std::string> firstAttribute(const std::vector<NameAttribute>& attrs, std::string_view oid) {
    for (const NameAttribute& a : attrs)
        if (a.oid == oid) return a.value;
    return std::nullopt;
}

std::string keyLabel(const CertificateInfo& c) {
    switch (c.publicKeyType) {
    case PublicKeyType::Gost3410_2012_256: return "GOST R 34.10-2012 256";
    case PublicKeyType::Gost3410_2012_512: return "GOST R 34.10-2012 512";
    case PublicKeyType::Rsa: return c.keyBits > 0 ? "RSA " + std::to_string(c.keyBits) : "RSA";
    case PublicKeyType::Ec: return c.keyBits > 0 ? "EC P-" + std::to_string(c.keyBits) : "EC";
    case PublicKeyType::Unknown: break;
    }
    return {};
}

} // namespace

CertificateDisplay makeCertificateDisplay(const CertificateInfo& cert, const X509Extras* extras, const KeyLocation* location,
                                          bool privateKeyAvailable, std::chrono::system_clock::time_point now) {
    X509Extras local;
    if (!extras) {
        if (parseX509(cert.der, {}, &local)) extras = &local;
    }

    CertificateDisplay d;
    d.subjectName = cert.commonName;
    d.organization = cert.organization;
    d.email = cert.email;
    d.inn = cert.russian.inn;
    d.snils = cert.russian.snils;
    d.ogrn = cert.russian.ogrn;
    d.ogrnip = cert.russian.ogrnip;
    d.innLe = cert.russian.innLe;
    d.validFrom = cert.notBefore;
    d.validTo = cert.notAfter;
    d.expired = now > cert.notAfter;
    d.notYetValid = now < cert.notBefore;
    if (!cert.serialHex.empty()) d.serialHex = cert.serialHex;
    d.sha256Fingerprint = cert.sha256Fingerprint;
    d.keyAlgorithmLabel = keyLabel(cert);
    if (location) d.location = *location;
    d.privateKeyAvailable = privateKeyAvailable;

    if (extras) {
        const auto surname = firstAttribute(extras->subjectAttributes, "2.5.4.4");
        const auto given = firstAttribute(extras->subjectAttributes, "2.5.4.42");
        if (surname && given) d.personName = *surname + " " + *given;
        else if (surname) d.personName = surname;
        else if (given) d.personName = given;
        d.title = firstAttribute(extras->subjectAttributes, "2.5.4.12");
        d.country = firstAttribute(extras->subjectAttributes, "2.5.4.6");
        d.locality = firstAttribute(extras->subjectAttributes, "2.5.4.7");
        d.region = firstAttribute(extras->subjectAttributes, "2.5.4.8");
        d.issuerOrganization = firstAttribute(extras->issuerAttributes, "2.5.4.10");
        d.issuerName = firstAttribute(extras->issuerAttributes, "2.5.4.3");
        if (!d.issuerName) d.issuerName = d.issuerOrganization;
        d.certificatePolicyOids = extras->certificatePolicyOids;
    }
    return d;
}

CertificateDisplay makeCertificateDisplay(const IdentityDescriptor& identity, const X509Extras* extras,
                                          std::chrono::system_clock::time_point now) {
    return makeCertificateDisplay(identity.certificate, extras, &identity.location, identity.privateKeyAvailable, now);
}

} // namespace rivet::crypto
