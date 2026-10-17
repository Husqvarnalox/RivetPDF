// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "app/DigitalSignatureController.hpp"
#include "app/SecurityDialogs.hpp"
#include "app/SecurityText.hpp"

#include <algorithm>
#include <cstdio>

using namespace rivet;
using namespace rivet::app;
using rivet::crypto::ProviderAssurance;
using rivet::crypto::SignatureClass;

namespace {
bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

RIVET_TEST(securityTextLevelsNeedTimestampOnlyAboveB) {
    using pdf::sign::PadesLevel;
    CHECK(!levelNeedsTimestamp(PadesLevel::BaselineB));
    CHECK(levelNeedsTimestamp(PadesLevel::BaselineT));
    CHECK(levelNeedsTimestamp(PadesLevel::BaselineLT));
    CHECK(levelNeedsTimestamp(PadesLevel::BaselineLTA));
    CHECK(levelShortName(PadesLevel::BaselineLTA) != levelShortName(PadesLevel::BaselineT));
}

RIVET_TEST(securityTextQualificationNeverQualifiedWithoutCertifiedCsp) {
    crypto::SignatureClassification c;
    c.level = SignatureClass::QualifiedEvidence;
    const std::string software = qualificationLabel(c, ProviderAssurance::Software);
    const std::string remote = qualificationLabel(c, ProviderAssurance::RemoteService);
    const std::string token = qualificationLabel(c, ProviderAssurance::HardwareToken);
    const std::string certified = qualificationLabel(c, ProviderAssurance::CertifiedCspDeclared);
    for (const std::string* label : {&software, &remote, &token}) {
        const std::string low = lower(*label);
        if (low.find("all technical indicators") != std::string::npos) {
            std::fprintf(stderr, "software/remote/mock label claims qualified: %s\n", label->c_str());
        }
        CHECK(low.find("all technical indicators") == std::string::npos);
        CHECK(contains(*label, "not shown to be a qualified"));
    }
    CHECK(contains(certified, "evidence only"));
    // Simple / unknown classes never read as qualified or advanced.
    c.level = SignatureClass::SimpleCrypto;
    CHECK(contains(qualificationLabel(c, ProviderAssurance::CertifiedCspDeclared), "Simple"));
    c.level = SignatureClass::Unknown;
    CHECK(contains(qualificationLabel(c, ProviderAssurance::Software), "could not be determined"));
}

RIVET_TEST(securityTextFormatUtcIsStable) {
    const auto epoch = std::chrono::system_clock::time_point{};
    const std::string text = formatUtc(epoch);
    CHECK(contains(text, "1970-01-01"));
    CHECK(contains(text, "UTC"));
}

RIVET_TEST(securityTextSignErrorsAreReadable) {
    pdf::sign::SignError e;
    e.code = pdf::sign::SignErrorCode::Cancelled;
    CHECK(contains(describeSignError(e), "original file was not changed"));
    e.code = pdf::sign::SignErrorCode::EncryptedDocument;
    CHECK(contains(describeSignError(e), "password-protected"));
    e.code = pdf::sign::SignErrorCode::Internal;
    e.message = "boom";
    CHECK(!describeSignError(e).empty());
}

RIVET_TEST(securityTextEncryptErrorsAreReadable) {
    CHECK(contains(describeEncryptError(core::makeError(core::ErrorCode::Cancelled, "x", "t")), "No file was written"));
    CHECK(contains(describeEncryptError(core::makeError(core::ErrorCode::DiskFull, "x", "t")), "disk space"));
    CHECK(contains(describeEncryptError(core::makeError(core::ErrorCode::NotAvailable, "x", "t")), "cryptography"));
}

RIVET_TEST(securityTextTsaUrlMustBeHttps) {
    CHECK(DigitalSignatureController::isAcceptableTsaUrl("https://tsa.example.com/ts"));
    CHECK(!DigitalSignatureController::isAcceptableTsaUrl("http://tsa.example.com/ts"));
    CHECK(!DigitalSignatureController::isAcceptableTsaUrl("ftp://tsa.example.com/ts"));
    CHECK(!DigitalSignatureController::isAcceptableTsaUrl(""));
    CHECK(!DigitalSignatureController::isAcceptableTsaUrl("https://"));
    CHECK(!DigitalSignatureController::isAcceptableTsaUrl("tsa.example.com"));
}

RIVET_TEST(securityTextWrapSplitsAtWordsAndNeverExceedsWidth) {
    const auto lines = WrappedText::wrap("alpha beta gamma delta epsilon", 12);
    CHECK_GT(lines.size(), 1u);
    for (const std::string& line : lines) CHECK_LE(line.size(), 12u);
    // A single over-long word is still emitted (split), nothing is dropped.
    const auto longWord = WrappedText::wrap("abcdefghijklmnopqrstuvwxyz", 8);
    std::size_t total = 0;
    for (const std::string& line : longWord) total += line.size();
    CHECK_EQ(total, std::size_t{26});
    CHECK(WrappedText::wrap("", 10).size() <= 1u);
}
