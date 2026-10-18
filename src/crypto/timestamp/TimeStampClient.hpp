// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "crypto/timestamp/TimeStamp.hpp"
#include "net/Client.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace rivet::crypto {

inline constexpr std::string_view kTimeStampQueryContentType = "application/timestamp-query";
inline constexpr std::string_view kTimeStampReplyContentType = "application/timestamp-reply";

struct TimeStampClientOptions {
    TimeStampRequestOptions request;
    // Trust anchors for the TSA chain (the OS store is never used implicitly),
    // optional extra chain material.
    TimeStampVerifyOptions verify;
};

struct TimeStampResult {
    TimeStampRequest request;
    Bytes responseDer;            // the reply exactly as received
    TimeStampResponse response;   // parsed PKIStatusInfo (+ token); default when the reply did not parse
    TimeStampVerification verification;

    bool ok() const { return verification.ok(); }
    // The raw token to embed unchanged (attachSignatureTimestamp); empty unless ok().
    const Bytes& tokenDer() const { return verification.tokenDer; }
};

// RFC 3161 client over HTTP(S). It reaches the network only through
// net::NetworkClient, so the online-validation gate (Off / Ask / On), the SSRF
// policy, redirect handling and the size / time limits of NetworkPolicy always
// apply. Thread-safe. There is no built-in TSA URL: the caller supplies it.
class TimeStampClient {
public:
    TimeStampClient(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider);

    // Blocking: run through requestAsync, never on the UI thread.
    //
    // Errors (CryptoResult): InvalidArgument / WeakAlgorithm (bad URL or digest,
    // SHA-1), BackendUnavailable, Network (gate, policy, transport, size cap,
    // timeout), BadResponse (HTTP status != 200, wrong Content-Type), Cancelled.
    // A reply that arrives but is rejected (bad status, mismatching imprint or
    // nonce, untrusted TSA, ...) is a successful TimeStampResult whose
    // verification.failure says why.
    CryptoResult<TimeStampResult> request(std::string_view url, DigestAlgorithm algorithm, ByteView digest,
                                          const TimeStampClientOptions& options, const CancelToken& cancel) const;

    // Same on a scheduler; `done` runs once on the main thread (runCryptoAsync
    // contract). Returns false (no callback) when the scope is closed.
    bool requestAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                      core::AsyncScope& scope, CancelToken cancel, std::string url, DigestAlgorithm algorithm,
                      Bytes digest, TimeStampClientOptions options,
                      std::function<void(CryptoResult<TimeStampResult>)> done) const;

private:
    std::shared_ptr<const net::NetworkClient> network_;
    std::shared_ptr<CryptoProvider> provider_;
};

} // namespace rivet::crypto
