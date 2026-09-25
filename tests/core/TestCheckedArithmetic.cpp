#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/CheckedArithmetic.hpp"
#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>

using namespace rivet::core;

namespace {

constexpr std::size_t kMax = SIZE_MAX;

// Sentinel proving an out parameter is untouched when overflow is reported.
constexpr std::size_t kSentinel = 42;

} // namespace

RIVET_TEST(checkedMultiplyBasic) {
    std::size_t result = 0;
    CHECK(checkedMultiply(3, 4, result));
    CHECK_EQ(result, std::size_t{12});

    // Zero annihilates any product and must not read as overflow.
    result = 0;
    CHECK(checkedMultiply(0, kMax, result));
    CHECK_EQ(result, std::size_t{0});
    CHECK(checkedMultiply(kMax, 0, result));
    CHECK_EQ(result, std::size_t{0});

    CHECK(checkedMultiply(1, kMax, result));
    CHECK_EQ(result, kMax);
}

RIVET_TEST(checkedMultiplyOverflows) {
    std::size_t result = kSentinel;
    // SIZE_MAX * 2 (and the symmetric case) cannot fit.
    CHECK(!checkedMultiply(kMax, 2, result));
    CHECK_EQ(result, kSentinel); // out parameter untouched on failure
    CHECK(!checkedMultiply(2, kMax, result));
    CHECK_EQ(result, kSentinel);

    // Any multiplier above 1 overflows for the largest operand.
    CHECK(!checkedMultiply(kMax, kMax, result));
    CHECK_EQ(result, kSentinel);
}

RIVET_TEST(checkedMultiplyBoundary) {
    std::size_t result = 0;
    // Largest product that still fits: (SIZE_MAX / 2) * 2 == SIZE_MAX - 1.
    constexpr std::size_t halfPlusOne = kMax / 2 + 1;
    CHECK(checkedMultiply(kMax / 2, 2, result));
    CHECK_EQ(result, kMax - 1);
    CHECK(checkedMultiply(2, kMax / 2, result));
    CHECK_EQ(result, kMax - 1);
    // One unit more on either operand overflows.
    result = kSentinel;
    CHECK(!checkedMultiply(halfPlusOne, 2, result));
    CHECK_EQ(result, kSentinel);
    CHECK(!checkedMultiply(2, halfPlusOne, result));
    CHECK_EQ(result, kSentinel);
}

RIVET_TEST(checkedAddBasic) {
    std::size_t result = 0;
    CHECK(checkedAdd(0, 7, result));
    CHECK_EQ(result, std::size_t{7});
    CHECK(checkedAdd(7, 0, result));
    CHECK_EQ(result, std::size_t{7});
    CHECK(checkedAdd(kMax, 0, result));
    CHECK_EQ(result, kMax);
    CHECK(checkedAdd(0, kMax, result));
    CHECK_EQ(result, kMax);
}

RIVET_TEST(checkedAddBoundary) {
    std::size_t result = 0;
    // Largest sum that still fits.
    CHECK(checkedAdd(kMax - 1, 1, result));
    CHECK_EQ(result, kMax);
    // One unit more overflows.
    result = kSentinel;
    CHECK(!checkedAdd(kMax, 1, result));
    CHECK_EQ(result, kSentinel); // out parameter untouched on failure
    CHECK(!checkedAdd(1, kMax, result));
    CHECK_EQ(result, kSentinel);
    CHECK(!checkedAdd(kMax - 1, 2, result));
    CHECK_EQ(result, kSentinel);
}

RIVET_TEST(roundUpToAlignmentPowersOfTwo) {
    CHECK_EQ(roundUpToAlignment(0, 1), std::size_t{0});
    CHECK_EQ(roundUpToAlignment(7, 1), std::size_t{7});
    CHECK_EQ(roundUpToAlignment(0, 64), std::size_t{0});
    CHECK_EQ(roundUpToAlignment(1, 2), std::size_t{2});
    CHECK_EQ(roundUpToAlignment(63, 64), std::size_t{64});
    CHECK_EQ(roundUpToAlignment(65, 64), std::size_t{128});
    CHECK_EQ(roundUpToAlignment(4095, 4096), std::size_t{4096});
    // Already-aligned values pass through unchanged.
    CHECK_EQ(roundUpToAlignment(64, 64), std::size_t{64});
    CHECK_EQ(roundUpToAlignment(4096, 4096), std::size_t{4096});
}

RIVET_TEST(roundUpToAlignmentRejectsBadAlignment) {
    // Zero and non-power-of-two alignments pass the value through unchanged.
    CHECK_EQ(roundUpToAlignment(100, 0), std::size_t{100});
    CHECK_EQ(roundUpToAlignment(100, 3), std::size_t{100});
    CHECK_EQ(roundUpToAlignment(100, 5), std::size_t{100});
    CHECK_EQ(roundUpToAlignment(100, 96), std::size_t{100}); // 96 = 32 * 3
}

RIVET_TEST(roundUpToAlignmentOverflowSentinel) {
    // Largest already-aligned value: rounding is a no-op.
    constexpr std::size_t alignedMax = kMax & ~std::size_t{63};
    CHECK_EQ(roundUpToAlignment(alignedMax, 64), alignedMax);
    // Anything above it cannot round up without overflow -> SIZE_MAX.
    CHECK_EQ(roundUpToAlignment(alignedMax + 1, 64), kMax);
    CHECK_EQ(roundUpToAlignment(kMax, 64), kMax);
    CHECK_EQ(roundUpToAlignment(kMax, 2), kMax);
}

RIVET_TEST(bitmapCreateCleanErrorOnStrideOverflow) {
    // stride * height cannot be computed even in theory; Bitmap::create must
    // return a clean error through the checkedMultiply path instead of
    // wrapping or crashing.
    auto result = Bitmap::create(1, 2, kMax);
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::InvalidArgument);

    // stride * height fits (height 1) but the total exceeds the byte bound.
    auto overBound = Bitmap::create(1, 1, kMax);
    CHECK(!overBound.has_value());
    CHECK_EQ(overBound.error().code, ErrorCode::InvalidArgument);
}

RIVET_TEST(bitmapCreateLargestAllowedStride) {
    // A large-but-bounded explicit stride still works end to end.
    constexpr std::size_t stride = std::size_t{64};
    auto result = Bitmap::create(1, 4, stride);
    CHECK(result.has_value());
    CHECK_EQ(result->stride(), stride);
    CHECK_EQ(result->sizeBytes(), stride * 4);
}
