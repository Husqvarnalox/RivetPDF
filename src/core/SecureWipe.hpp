// SPDX-License-Identifier: MPL-2.0
#pragma once

// Non-elidable wiping of secret bytes (passwords, PINs). Header-only so that
// every layer (core, pdf, ui, app, crypto) can use it without a new link
// dependency.

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rivet::core {

// Overwrites memory in a way the optimizer may not elide.
inline void secureZero(void* data, std::size_t size) {
    if (data == nullptr || size == 0) return;
#if defined(_WIN32)
    SecureZeroMemory(data, size);
#else
    volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) p[i] = 0;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(data) : "memory");
#endif
#endif
}

// Zeroes the whole owned storage of `text` (its full capacity, which also
// covers a shrunk-but-not-reallocated tail), then empties it. Safe on a
// moved-from string.
inline void wipeString(std::string& text) {
    text.resize(text.capacity());
    secureZero(text.data(), text.size());
    text.clear();
}

// Wipes the referenced string when the scope ends, on every path.
class ScopedWipe {
public:
    explicit ScopedWipe(std::string& text) : text_(text) {}
    ~ScopedWipe() { wipeString(text_); }
    ScopedWipe(const ScopedWipe&) = delete;
    ScopedWipe& operator=(const ScopedWipe&) = delete;

private:
    std::string& text_;
};

} // namespace rivet::core
