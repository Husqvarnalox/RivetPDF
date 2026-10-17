// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DigitalSignatureController.hpp"

#include <filesystem>
#include <string>

namespace rivet::platform {
class IAppDataLocation;
}

namespace rivet::app {

// Anchors parsed from PEM/DER text (several PEM certificates allowed).
// Unparseable certificates are skipped. Pure and unit-tested.
crypto::TrustAnchorSet parseAnchorBundle(crypto::CryptoProvider& provider, const std::string& fileContents);

// The production wiring: default crypto provider, the platform keystore,
// a consent-gated network client (mode Ask, no network at all when the build
// has no transport) and trust anchors read from `<app data>/trust/`:
//   signer-anchors.pem, timestamp-anchors.pem, accredited-cas.txt
// The operating-system trust store is never consulted. Missing files give
// empty sets; the dialogs then explain what is unavailable.
SecurityServices makeSecurityServices(platform::IAppDataLocation* appData);

} // namespace rivet::app
