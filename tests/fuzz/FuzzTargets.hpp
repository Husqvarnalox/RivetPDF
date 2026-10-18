// SPDX-License-Identifier: MPL-2.0
#pragma once

// Fuzz targets for the Phase 8 attack surface. Every target is a plain
// function over a byte buffer, usable from libFuzzer (FuzzEntry.cpp) and from
// the deterministic replay/mutation runner (FuzzReplay.cpp) that works in any
// build on any compiler. A target must never crash, hang, throw, or violate
// its documented invariants for ANY input; invariant violations call abort()
// after printing the reason (libFuzzer records that as a crash).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rivet::fuzz {

using Bytes = std::vector<std::uint8_t>;
using FuzzFn = int (*)(const std::uint8_t*, std::size_t);
using SeedListFn = std::vector<Bytes> (*)();

struct Target {
    std::string_view name;
    FuzzFn run;
    SeedListFn seeds; // null in libFuzzer-only builds
};

[[noreturn]] void invariantViolation(const char* target, const char* what);

// ---- targets (FuzzTargets.cpp) ---------------------------------------------------------
int fuzzDer(const std::uint8_t* data, std::size_t size);
int fuzzX509(const std::uint8_t* data, std::size_t size);
int fuzzCms(const std::uint8_t* data, std::size_t size);
int fuzzTimestamp(const std::uint8_t* data, std::size_t size);
// Input: u16be len + certificate DER, u16be len + issuer DER, then the evidence
// (an OCSP response or a CRL; both parsers see every input).
int fuzzRevocation(const std::uint8_t* data, std::size_t size);
int fuzzPdfSignature(const std::uint8_t* data, std::size_t size);
// Input: one option byte, then a PDF in PDFium output shape.
int fuzzPdfEncrypt(const std::uint8_t* data, std::size_t size);
int fuzzNet(const std::uint8_t* data, std::size_t size);
int fuzzJson(const std::uint8_t* data, std::size_t size);

// Message the timestamp seeds are built over (the verifier expects its SHA-256).
inline constexpr std::string_view kTimestampMessage = "rivet fuzz timestamp message";
// Message detached CMS seeds sign.
inline constexpr std::string_view kCmsMessage = "rivet fuzz cms content";

// Frames a revocation input (see fuzzRevocation).
Bytes frameRevocation(std::span<const std::uint8_t> cert, std::span<const std::uint8_t> issuer,
                      std::span<const std::uint8_t> evidence);

// ---- seeds (FuzzSeeds.cpp, needs the OpenSSL test support) ----------------------------------
std::vector<Bytes> seedsDer();
std::vector<Bytes> seedsX509();
std::vector<Bytes> seedsCms();
std::vector<Bytes> seedsTimestamp();
std::vector<Bytes> seedsRevocation();
std::vector<Bytes> seedsPdfSignature();
std::vector<Bytes> seedsPdfEncrypt();
std::vector<Bytes> seedsNet();
std::vector<Bytes> seedsJson();

std::span<const Target> allTargets();

} // namespace rivet::fuzz
