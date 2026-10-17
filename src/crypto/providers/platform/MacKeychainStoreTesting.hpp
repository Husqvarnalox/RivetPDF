// SPDX-License-Identifier: MPL-2.0
#pragma once

// PRIVATE test seam of the macOS keychain store (not installed, not used by
// the application). Restricts enumeration to ONE keychain file so that tests
// never read or touch the user's login keychain. No Apple type appears here.

#include "crypto/IdentityStore.hpp"

#include <filesystem>
#include <memory>

namespace rivet::crypto {

#if defined(RIVET_CRYPTO_PLATFORM_MACOS)
// The keychain file at `keychainPath` must already exist and be unlocked by
// the caller. Search list = exactly that keychain.
std::shared_ptr<IdentityStore> makeMacKeychainStoreForTesting(const std::filesystem::path& keychainPath);
#endif

} // namespace rivet::crypto
