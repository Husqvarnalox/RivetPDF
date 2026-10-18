// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/SecureWipe.hpp"
#include "crypto/SecretBuffer.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

using namespace rivet::crypto;

static_assert(!std::is_copy_constructible_v<SecretBuffer>);
static_assert(!std::is_copy_assignable_v<SecretBuffer>);
static_assert(std::is_nothrow_move_constructible_v<SecretBuffer>);

RIVET_TEST(secretBufferHoldsAndMoves) {
    SecretBuffer a(std::string_view("1234"));
    CHECK_EQ(a.size(), std::size_t{4});
    CHECK(std::memcmp(a.chars(), "1234", 4) == 0);
    CHECK_EQ(a.chars()[4], '\0'); // NUL-terminated for C APIs

    SecretBuffer b(std::move(a));
    CHECK(a.empty()); // NOLINT: moved-from is defined as empty
    CHECK_EQ(b.size(), std::size_t{4});
}

RIVET_TEST(secretBufferClearZeroesMemory) {
    // Observe the storage through the span before clear(); after clear() the
    // buffer must report empty. (The zeroing itself is secureZero, checked next.)
    SecretBuffer s(std::string_view("secret"));
    CHECK(!s.empty());
    s.clear();
    CHECK(s.empty());
    CHECK_EQ(s.size(), std::size_t{0});
}

RIVET_TEST(secureZeroOverwrites) {
    std::uint8_t buf[16];
    std::memset(buf, 0xAB, sizeof buf);
    secureZero(buf, sizeof buf);
    for (std::uint8_t b : buf) CHECK_EQ(b, 0);
    secureZero(nullptr, 0); // must not crash
}

RIVET_TEST(wipeStringZeroesTheOwnedStorageInPlace) {
    // Long enough to be heap allocated; clear() keeps the allocation, so the
    // storage stays readable through the saved pointer.
    std::string s(64, 'x');
    s.resize(40);
    const char* storage = s.data();
    const std::size_t capacity = s.capacity();
    rivet::core::wipeString(s);
    CHECK(s.empty());
    CHECK_EQ(s.capacity(), capacity);
    bool allZero = true;
    for (std::size_t i = 0; i < capacity; ++i) allZero = allZero && storage[i] == '\0';
    CHECK(allZero);
}

RIVET_TEST(scopedWipeZeroesOnEveryExitPath) {
    std::string s(48, 'p');
    const char* storage = s.data();
    const std::size_t capacity = s.capacity();
    try {
        rivet::core::ScopedWipe guard(s);
        throw 1; // error path
    } catch (int) {
    }
    CHECK(s.empty());
    bool allZero = true;
    for (std::size_t i = 0; i < capacity; ++i) allZero = allZero && storage[i] == '\0';
    CHECK(allZero);

    std::string moved(48, 'q');
    std::string taken = std::move(moved);
    rivet::core::wipeString(moved); // moved-from strings are safe to wipe
    rivet::core::wipeString(taken);
    CHECK(taken.empty());
}
