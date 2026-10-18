// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace rivet::crypto {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

inline ByteView viewOf(std::string_view text) {
    return ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// Cooperative cancellation shared between the caller and a running operation.
// Copies share one flag; cancel() is thread-safe and idempotent.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { flag_->store(true, std::memory_order_release); }
    bool cancelled() const { return flag_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace rivet::crypto
