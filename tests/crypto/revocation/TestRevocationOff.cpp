// SPDX-License-Identifier: MPL-2.0
// Revocation behaviour that does not need a crypto backend: without one every
// check reports Unknown (never good) and the network is never touched.
#include "RivetTest.h"

#include "crypto/revocation/RevocationChecker.hpp"
#include "net/MockTransport.hpp"

using namespace rivet;
using namespace rivet::crypto;

RIVET_TEST(revocationNullProviderReportsUnknownAndStaysOffline) {
    auto provider = makeNullProvider();
    auto mock = std::make_shared<net::MockTransport>();
    auto gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    auto network = std::make_shared<net::NetworkClient>(net::NetworkPolicy{}, mock, gate);
    RevocationChecker checker(provider, network);
    CertificateInfo a, b;
    a.subject = "leaf";
    b.subject = "ca";
    auto out = checker.check({a, b}, std::chrono::system_clock::now(), {}, CancelToken{});
    CHECK(out.status == RevocationStatus::Unknown);
    CHECK(out.certs.size() == 1 && !out.certs[0].unknownReason.empty());
    CHECK(out.certs[0].status != RevocationStatus::Good);
    CHECK_EQ(mock->callCount(), std::size_t{0});

    RevocationPolicy off;
    off.mode = RevocationMode::Off;
    auto skipped = checker.check({a, b}, std::chrono::system_clock::now(), off, CancelToken{});
    CHECK(skipped.status == RevocationStatus::NotChecked && skipped.acceptable());

    auto request = buildOcspRequest(*provider, a, b);
    CHECK(!request.has_value() && request.error().code == CryptoErrorCode::BackendUnavailable);
    CrlClient crl(network, provider);
    auto ext = RevocationExtensions{};
    auto fetched = crl.fetch("http://crl.example.com/ca.crl", a, b, ext, {}, CancelToken{});
    CHECK(!fetched.has_value() && fetched.error().code == CryptoErrorCode::BackendUnavailable);
    CHECK_EQ(mock->callCount(), std::size_t{0});
}

RIVET_TEST(revocationBackendFreeHelpers) {
    CHECK(classifyRevocationUrl("ldap://x/y").has_value());
    CHECK(classifyRevocationUrl("file:///etc/passwd").has_value());
    CHECK(!classifyRevocationUrl("http://crl.example.com/ca.crl").has_value());
    CHECK(!parseRevocationExtensions(Bytes{}).has_value());
    CHECK(!parseRevocationExtensions(Bytes{0x30, 0x00}).has_value());

    EvidenceClock clock;
    clock.now = std::chrono::system_clock::now();
    clock.validationTime = clock.now;
    const auto hour = std::chrono::hours(1);
    CHECK(!evidenceTimeProblem(clock.now - hour, clock.now + hour, clock.now, clock).has_value());
    CHECK(evidenceTimeProblem(clock.now - 3 * hour, clock.now - hour, clock.now - 2 * hour, clock) == EvidenceFailure::Stale);
    CHECK(evidenceTimeProblem(clock.now + hour, clock.now + 2 * hour, clock.now, clock) == EvidenceFailure::NotYetValid);
    // No nextUpdate: bounded by maxAge.
    CHECK(!evidenceTimeProblem(clock.now - hour, std::nullopt, clock.now, clock).has_value());
    CHECK(evidenceTimeProblem(clock.now - 48 * hour, std::nullopt, clock.now - 48 * hour, clock) == EvidenceFailure::Stale);
    // Historical: evidence that ended before the validation time does not cover it.
    EvidenceClock past = clock;
    past.validationTime = clock.now - 100 * hour;
    CHECK(evidenceTimeProblem(clock.now - 3 * hour, clock.now - hour, clock.now - 2 * hour, past).has_value() == false);
    CHECK(evidenceTimeProblem(past.validationTime - 10 * hour, past.validationTime - 5 * hour, past.validationTime - 8 * hour, past) ==
          EvidenceFailure::DoesNotCoverTime);

    CrlCache cache;
    cache.insert("http://x/", Bytes(10, 1), clock.now + hour);
    CHECK(cache.find("http://x/", clock.now).has_value());
    CHECK(!cache.find("http://x/", clock.now + 2 * hour).has_value());
}
