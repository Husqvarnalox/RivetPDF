#include "core/Error.hpp"

namespace rivet::core {

std::string_view toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::None: return "None";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::NotAvailable: return "NotAvailable";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::Io: return "Io";
        case ErrorCode::InvalidDocument: return "InvalidDocument";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

std::string describe(const Error& error) {
    std::string result;
    if (!error.subsystem.empty()) {
        result += error.subsystem;
        result += ": ";
    }
    result += toString(error.code);
    if (!error.message.empty()) {
        result += ": ";
        result += error.message;
    }
    return result;
}

} // namespace rivet::core
