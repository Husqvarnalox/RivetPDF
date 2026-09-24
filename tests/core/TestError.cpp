#include "RivetTest.h"

#include "core/Error.hpp"

#include <expected>
#include <string>

using namespace rivet::core;

RIVET_TEST(errorBasics) {
    const Error e = makeError(ErrorCode::InvalidDocument, "bad xref", "pdf");
    CHECK(e.operator bool());
    CHECK_EQ(e.code, ErrorCode::InvalidDocument);
    CHECK_EQ(e.subsystem, "pdf");
    CHECK_EQ(e.message, "bad xref");

    const Error none{};
    CHECK(!none.operator bool());
    CHECK_EQ(none.code, ErrorCode::None); // default Error is the "no error" state
}

RIVET_TEST(errorCodeToString) {
    CHECK_EQ(toString(ErrorCode::NotFound), "NotFound");
    CHECK_EQ(toString(ErrorCode::OutOfMemory), "OutOfMemory");
}

RIVET_TEST(describeIncludesSubsystemCodeAndMessage) {
    const std::string text = describe(makeError(ErrorCode::Io, "read failed", "pdf"));
    CHECK_EQ(text, "pdf: Io: read failed");

    const std::string noSubsystem = describe(makeError(ErrorCode::Internal, "boom"));
    CHECK_EQ(noSubsystem, "Internal: boom");
}

RIVET_TEST(resultContracts) {
    Result<int> good = 42;
    CHECK(good.has_value());
    CHECK_EQ(*good, 42);

    Result<int> bad = std::unexpected(makeError(ErrorCode::NotFound, "missing"));
    CHECK(!bad.has_value());
    CHECK_EQ(bad.error().code, ErrorCode::NotFound);

    const Status fine = ok();
    CHECK(fine.has_value());
}

RIVET_TEST(resultMonadicUse) {
    auto failing = []() -> Result<int> {
        return std::unexpected(makeError(ErrorCode::InvalidArgument, "nope"));
    };

    const Result<int> mapped = failing().and_then([](int v) -> Result<int> { return v * 2; });
    CHECK(!mapped.has_value());
    CHECK_EQ(mapped.error().code, ErrorCode::InvalidArgument);

    const int recovered = failing().value_or(-1);
    CHECK_EQ(recovered, -1);
}
