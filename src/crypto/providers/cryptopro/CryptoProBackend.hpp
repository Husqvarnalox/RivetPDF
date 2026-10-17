// SPDX-License-Identifier: MPL-2.0
#pragma once

// Private interface between the always-compiled factory
// (CryptoProIdentityStore.cpp) and the CSP backend (CryptoProBackend.cpp,
// built only with RIVET_ENABLE_CRYPTOPRO). Not for use outside this directory.

#include "crypto/providers/cryptopro/CryptoProIdentityStore.hpp"

#include <memory>
#include <string>

namespace rivet::crypto::cryptopro_detail {

// Loads the CSP libraries; on failure returns null and fills `reason`.
std::shared_ptr<IdentityStore> makeBackendStore(const CryptoProOptions& options, std::string& reason);
std::shared_ptr<CryptoProvider> makeBackendProvider(const CryptoProOptions& options, std::string& reason);

} // namespace rivet::crypto::cryptopro_detail
