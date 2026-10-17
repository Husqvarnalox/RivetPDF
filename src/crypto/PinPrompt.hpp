// SPDX-License-Identifier: MPL-2.0
#pragma once

// PIN / password prompting for hardware and CSP-backed identities. This layer
// contains no UI: the application injects a PinPrompt implementation.
//
// Contract:
//  - requestPin() may block (it normally waits for the user). Providers call
//    it from the thread that runs sign() (a runCryptoAsync worker), never on
//    the main thread, and never while holding a lock.
//  - Returning std::nullopt means the user cancelled; the operation then
//    fails with CryptoErrorCode::Cancelled.
//  - The PIN travels in a SecretBuffer only; it is never logged and the
//    provider zeroes its own copies as soon as the backend call returns.

#include "crypto/SecretBuffer.hpp"
#include "crypto/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace rivet::crypto {

enum class PinPromptReason : std::uint8_t {
    Required,    // first request for this key / token
    Incorrect,   // previous PIN was rejected
    LastAttempt, // previous PIN was rejected and the next failure blocks the PIN
};

struct PinRequest {
    std::string providerName;       // "cryptopro", "pkcs11", "macos-keychain", ...
    std::string label;              // container / token / key label, safe to display
    PinPromptReason reason = PinPromptReason::Required;
    int attempt = 1;                // 1-based
    std::optional<int> retriesLeft; // when the backend reports it
};

class PinPrompt {
public:
    virtual ~PinPrompt() = default;
    virtual std::optional<SecretBuffer> requestPin(const PinRequest& request, const CancelToken& cancel) = 0;
};

// Prompt that always cancels (used when no prompt is injected: a PIN-protected
// key then fails with Cancelled instead of hanging or guessing).
class NoPinPrompt final : public PinPrompt {
public:
    std::optional<SecretBuffer> requestPin(const PinRequest&, const CancelToken&) override { return std::nullopt; }
};

} // namespace rivet::crypto
