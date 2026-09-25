#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace rivet::core {

// Subsystem error codes. One shared enum keeps Result<T> uniform across
// subsystem boundaries; the subsystem string inside Error identifies origin.
enum class ErrorCode : std::uint8_t {
    None = 0,
    InvalidArgument,
    NotFound,
    NotAvailable,
    Unsupported,
    Io,
    InvalidDocument,
    OutOfMemory,
    Cancelled,
    Internal,
    // An encrypted document needs a password (or the provided one was
    // rejected). Distinct from InvalidDocument so the open flow can prompt
    // instead of failing.
    PasswordRequired,
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::string subsystem;

    Error() = default;
    Error(ErrorCode c, std::string msg, std::string sub = {})
        : code(c), message(std::move(msg)), subsystem(std::move(sub)) {}

    explicit operator bool() const { return code != ErrorCode::None; }

    bool operator==(const Error&) const = default;
};

inline Error makeError(ErrorCode code, std::string message, std::string subsystem = {}) {
    return Error{code, std::move(message), std::move(subsystem)};
}

std::string_view toString(ErrorCode code);

// Human-readable one-line description: "<subsystem>: <code>: <message>".
std::string describe(const Error& error);

// Result<T> is the standard Rivet return type for fallible operations at
// subsystem boundaries. C++23 std::expected based.
template <typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

inline Status ok() { return Status{}; }

} // namespace rivet::core
