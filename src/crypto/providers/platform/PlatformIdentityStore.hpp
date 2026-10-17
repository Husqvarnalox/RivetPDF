// SPDX-License-Identifier: MPL-2.0
#pragma once

// Platform keystore: the macOS keychain (Security.framework) and the Windows
// certificate store with CNG key storage providers. Keys never leave the OS
// store; Rivet only asks it to sign. On Linux there is no platform keystore:
// the returned store reports isAvailable() == false (use PKCS#11 or files).

#include "crypto/IdentityStore.hpp"

#include <memory>

namespace rivet::crypto {

// Never null. assurance() is ProviderAssurance::PlatformKeystore.
std::shared_ptr<IdentityStore> makePlatformIdentityStore();

// True when this build has a platform keystore implementation (macOS, Windows).
bool platformStoreSupported();

} // namespace rivet::crypto
