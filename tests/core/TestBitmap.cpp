#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>

using namespace rivet::core;

RIVET_TEST(bitmapCreateDefaults) {
    auto result = Bitmap::create(10, 4);
    CHECK(result.has_value());
    Bitmap& b = *result;
    CHECK(b.isValid());
    CHECK_EQ(b.width(), 10u);
    CHECK_EQ(b.height(), 4u);
    CHECK_EQ(b.bytesPerPixel(), std::size_t{4});
    CHECK_EQ(b.sizeBytes(), b.stride() * 4);
    // Default stride is width*4 rounded up to 64.
    CHECK_EQ(b.stride(), roundUpToAlignment(40, 64));
    CHECK(b.data() != nullptr);
}

RIVET_TEST(bitmapZeroFilled) {
    auto result = Bitmap::create(8, 8);
    CHECK(result.has_value());
    const Bitmap& b = *result;
    for (std::size_t i = 0; i < b.sizeBytes(); ++i) {
        if (b.data()[i] != std::byte{0}) {
            CHECK(false);
            return;
        }
    }
    CHECK(true);
}

RIVET_TEST(bitmapExplicitStride) {
    auto result = Bitmap::create(4, 2, 32);
    CHECK(result.has_value());
    CHECK_EQ(result->stride(), std::size_t{32});
    CHECK_EQ(result->sizeBytes(), std::size_t{64});
}

RIVET_TEST(bitmapRejectsZeroDimensions) {
    auto r1 = Bitmap::create(0, 10);
    CHECK(!r1.has_value());
    CHECK_EQ(r1.error().code, ErrorCode::InvalidArgument);

    auto r2 = Bitmap::create(10, 0);
    CHECK(!r2.has_value());
}

RIVET_TEST(bitmapRejectsAbsurdDimensions) {
    auto result = Bitmap::create(kMaxBitmapDimension + 1, 1);
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::InvalidArgument);
}

RIVET_TEST(bitmapRejectsOverflowingAllocation) {
    // kMaxBitmapDimension x kMaxBitmapDimension x 4 bytes would far exceed
    // the byte bound; the allocation must be refused without attempting it.
    auto result = Bitmap::create(kMaxBitmapDimension, kMaxBitmapDimension);
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::InvalidArgument);
}

RIVET_TEST(bitmapMoveSemantics) {
    auto created = Bitmap::create(4, 4);
    CHECK(created.has_value());
    Bitmap moved = std::move(*created);
    CHECK(moved.isValid());
    CHECK_EQ(moved.width(), 4u);
}

RIVET_TEST(bitmapClear) {
    auto result = Bitmap::create(4, 4);
    CHECK(result.has_value());
    for (std::size_t i = 0; i < result->sizeBytes(); ++i) {
        result->data()[i] = std::byte{0xFF};
    }
    result->clear();
    for (std::size_t i = 0; i < result->sizeBytes(); ++i) {
        if (result->data()[i] != std::byte{0}) {
            CHECK(false);
            return;
        }
    }
    CHECK(true);
}

RIVET_TEST(roundUpToAlignment) {
    CHECK_EQ(roundUpToAlignment(0, 64), std::size_t{0});
    CHECK_EQ(roundUpToAlignment(1, 64), std::size_t{64});
    CHECK_EQ(roundUpToAlignment(64, 64), std::size_t{64});
    CHECK_EQ(roundUpToAlignment(65, 64), std::size_t{128});
    CHECK_EQ(roundUpToAlignment(63, 64), std::size_t{64});
}
