// SPDX-License-Identifier: MPL-2.0
// Time-stamp behaviour that does not need a crypto backend: without one,
// every operation reports BackendUnavailable and the network is never touched.
#include "RivetTest.h"

#include "crypto/timestamp/TimeStamp.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"
#include "net/MockTransport.hpp"

using namespace rivet::crypto;

RIVET_TEST(timestampNullProviderReportsBackendUnavailable) {
    auto provider = makeNullProvider();
    auto built = buildTimeStampRequest(*provider, DigestAlgorithm::Sha256, Bytes(32, 1));
    CHECK(!built.has_value() && built.error().code == CryptoErrorCode::BackendUnavailable);

    auto mock = std::make_shared<rivet::net::MockTransport>();
    auto gate = std::make_shared<rivet::net::OnlineValidationGate>(rivet::net::OnlineValidationMode::On);
    auto network = std::make_shared<rivet::net::NetworkClient>(rivet::net::NetworkPolicy{}, mock, gate);
    TimeStampClient client(network, provider);
    auto r = client.request("https://tsa.example.com/tsa", DigestAlgorithm::Sha256, Bytes(32, 1), {}, CancelToken{});
    CHECK(!r.has_value() && r.error().code == CryptoErrorCode::BackendUnavailable);
    CHECK_EQ(mock->callCount(), std::size_t{0});

    auto v = verifyTimeStampToken(Bytes{0x30, 0x00}, *provider, TimeStampExpectation{});
    CHECK(!v.has_value() && v.error().code == CryptoErrorCode::BackendUnavailable);
}

RIVET_TEST(timestampParsersAreBackendFree) {
    CHECK(!parseTimeStampResponse(Bytes{}).has_value());
    CHECK(!parseTimeStampResponse(Bytes{0x30, 0x00}).has_value());
    // granted, no token: a minimal well-formed response parses.
    auto granted = parseTimeStampResponse(Bytes{0x30, 0x05, 0x30, 0x03, 0x02, 0x01, 0x00});
    CHECK(granted.has_value() && granted->status.accepted() && !granted->tokenDer.has_value());
    // rejection with failInfo badAlg (named bit 0): BIT STRING 03 02 07 80.
    auto rejected = parseTimeStampResponse(Bytes{0x30, 0x09, 0x30, 0x07, 0x02, 0x01, 0x02, 0x03, 0x02, 0x07, 0x80});
    CHECK(rejected.has_value() && !rejected->status.accepted() && (rejected->status.failInfo & pkifailure::kBadAlg) != 0);
    CHECK(!parseTstInfo(Bytes{0x30, 0x00}).has_value());
    CHECK(std::string(toString(TimestampFailure::ImprintMismatch)) == "imprint mismatch");
}
