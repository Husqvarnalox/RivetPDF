// SPDX-License-Identifier: MPL-2.0
#include "crypto/SecretBuffer.hpp"

#include "core/SecureWipe.hpp"

#include <cstring>
#include <utility>


namespace rivet::crypto {

void secureZero(void* data, std::size_t size) { core::secureZero(data, size); }

SecretBuffer::SecretBuffer(std::string_view bytes) {
    assign(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

SecretBuffer::SecretBuffer(std::span<const std::uint8_t> bytes) { assign(bytes.data(), bytes.size()); }

SecretBuffer::~SecretBuffer() { clear(); }

SecretBuffer::SecretBuffer(SecretBuffer&& other) noexcept
    : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

SecretBuffer& SecretBuffer::operator=(SecretBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void SecretBuffer::assign(const std::uint8_t* src, std::size_t n) {
    clear();
    if (n == 0) return;
    data_ = std::make_unique<std::uint8_t[]>(n + 1); // +1: always NUL-terminated for C APIs
    std::memcpy(data_.get(), src, n);
    size_ = n;
}

void SecretBuffer::clear() {
    if (data_) secureZero(data_.get(), size_ + 1);
    data_.reset();
    size_ = 0;
}

} // namespace rivet::crypto
