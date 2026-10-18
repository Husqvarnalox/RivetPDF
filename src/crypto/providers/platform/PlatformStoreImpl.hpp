// SPDX-License-Identifier: MPL-2.0
#pragma once

// PRIVATE to src/crypto/providers/platform: per-OS factories selected by
// PlatformIdentityStore.cpp. Not part of the public API.

#include "crypto/IdentityStore.hpp"

#include <memory>

namespace rivet::crypto::platform_detail {

#if defined(RIVET_CRYPTO_PLATFORM_MACOS)
std::shared_ptr<IdentityStore> makeMacKeychainStore();
#endif
#if defined(RIVET_CRYPTO_PLATFORM_WINDOWS)
std::shared_ptr<IdentityStore> makeCngStore();
#endif

} // namespace rivet::crypto::platform_detail
