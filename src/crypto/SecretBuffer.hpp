// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace rivet::crypto {

// Fixed-capacity buffer for PIN / password bytes. Zeroed on destruction,
// move-only, never copied, never formatted or logged. Deliberately has no
// stream operator and no conversion to std::string.
class SecretBuffer {
public:
    SecretBuffer() = default;
    explicit SecretBuffer(std::string_view bytes);
    explicit SecretBuffer(std::span<const std::uint8_t> bytes);
    ~SecretBuffer();

    SecretBuffer(SecretBuffer&& other) noexcept;
    SecretBuffer& operator=(SecretBuffer&& other) noexcept;
    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::span<const std::uint8_t> bytes() const { return {data_.get(), size_}; }
    // NUL-terminated view for C APIs (valid while the buffer is alive).
    const char* chars() const { return reinterpret_cast<const char*>(data_.get()); }

    // Zeroes and releases the contents.
    void clear();

private:
    void assign(const std::uint8_t* src, std::size_t n);

    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_ = 0;
};

// Overwrites memory in a way the optimizer may not elide.
void secureZero(void* data, std::size_t size);

} // namespace rivet::crypto
