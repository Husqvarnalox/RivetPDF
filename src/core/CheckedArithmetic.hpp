#pragma once

#include <cstddef>
#include <cstdint>

namespace rivet::core {

// Portable checked arithmetic for std::size_t, replacing compiler-specific
// builtins (__builtin_mul_overflow & co) in shared core code. Standard C++
// only, header-only, constexpr so it can also be used in compile-time
// constants.
//
// Contract shared by all functions:
//   - Overflow is reported by the return value, never by throwing.
//   - On overflow the out parameter is left UNTOUCHED, so a caller may keep
//     a sentinel in it and inspect it after either outcome.

// Computes result = a * b. Returns true and stores the product when it fits
// in std::size_t; returns false (result untouched) when the product would
// overflow. The guard works because unsigned division truncates: any
// b > SIZE_MAX / a makes a * b exceed SIZE_MAX, and any b <= SIZE_MAX / a
// cannot overflow.
constexpr bool checkedMultiply(std::size_t a, std::size_t b, std::size_t& result) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    result = a * b;
    return true;
}

// Computes result = a + b. Returns true and stores the sum when it fits in
// std::size_t; returns false (result untouched) when the sum would overflow.
// The guard works because SIZE_MAX - a never underflows: it is the largest
// addend that keeps the sum in range.
constexpr bool checkedAdd(std::size_t a, std::size_t b, std::size_t& result) {
    if (b > SIZE_MAX - a) {
        return false;
    }
    result = a + b;
    return true;
}

// Rounds value up to the next multiple of alignment (alignment must be a
// power of two). A zero or non-power-of-two alignment passes value through
// unchanged. Returns SIZE_MAX as an overflow sentinel when the rounded value
// would not fit in std::size_t; callers must range-check the result.
constexpr std::size_t roundUpToAlignment(std::size_t value, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return value; // require power-of-two alignment; pass through otherwise
    }
    const std::size_t mask = alignment - 1;
    if (value > (SIZE_MAX & ~mask)) {
        return SIZE_MAX; // overflow indicator; callers must range-check
    }
    return (value + mask) & ~mask;
}

} // namespace rivet::core
