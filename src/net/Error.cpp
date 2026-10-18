// SPDX-License-Identifier: MPL-2.0
#include "net/Error.hpp"

#include <array>

namespace rivet::net {

namespace {

struct Row {
    NetFailure failure;
    std::string_view name;
    std::string_view subsystem;
    core::ErrorCode code;
};

constexpr std::array<Row, 13> kRows{{
    {NetFailure::InvalidRequest, "InvalidRequest", "net.InvalidRequest", core::ErrorCode::InvalidArgument},
    {NetFailure::PolicyViolation, "PolicyViolation", "net.PolicyViolation", core::ErrorCode::PermissionDenied},
    {NetFailure::ForbiddenAddress, "ForbiddenAddress", "net.ForbiddenAddress", core::ErrorCode::PermissionDenied},
    {NetFailure::OnlineValidationOff, "OnlineValidationOff", "net.OnlineValidationOff", core::ErrorCode::PermissionDenied},
    {NetFailure::ConsentDenied, "ConsentDenied", "net.ConsentDenied", core::ErrorCode::PermissionDenied},
    {NetFailure::RedirectLimit, "RedirectLimit", "net.RedirectLimit", core::ErrorCode::Unsupported},
    {NetFailure::RedirectDowngrade, "RedirectDowngrade", "net.RedirectDowngrade", core::ErrorCode::PermissionDenied},
    {NetFailure::BadRedirect, "BadRedirect", "net.BadRedirect", core::ErrorCode::InvalidArgument},
    {NetFailure::ResponseTooLarge, "ResponseTooLarge", "net.ResponseTooLarge", core::ErrorCode::Io},
    {NetFailure::Timeout, "Timeout", "net.Timeout", core::ErrorCode::Io},
    {NetFailure::Cancelled, "Cancelled", "net.Cancelled", core::ErrorCode::Cancelled},
    {NetFailure::BackendUnavailable, "BackendUnavailable", "net.BackendUnavailable", core::ErrorCode::NotAvailable},
    {NetFailure::Transport, "Transport", "net.Transport", core::ErrorCode::Io},
}};

const Row& rowFor(NetFailure failure) {
    for (const auto& r : kRows)
        if (r.failure == failure) return r;
    return kRows.back();
}

} // namespace

std::string_view toString(NetFailure failure) { return rowFor(failure).name; }

core::Error makeNetError(NetFailure failure, std::string message) {
    const Row& row = rowFor(failure);
    return core::Error(row.code, std::move(message), std::string(row.subsystem));
}

NetFailure failureOf(const core::Error& error) {
    for (const auto& r : kRows)
        if (error.subsystem == r.subsystem) return r.failure;
    return NetFailure::Transport;
}

} // namespace rivet::net
