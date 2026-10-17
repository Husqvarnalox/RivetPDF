// SPDX-License-Identifier: MPL-2.0
#include "LinuxAppDataLocation.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace rivet::platform {

namespace {

std::optional<std::string_view> environment(const char* name) {
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read-only after startup
    if (value == nullptr) return std::nullopt;
    return std::string_view(value);
}

std::optional<std::string> passwdHome() {
    long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size <= 0) size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    struct passwd entry {};
    struct passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || result == nullptr ||
        result->pw_dir == nullptr) {
        return std::nullopt;
    }
    return std::string(result->pw_dir);
}

} // namespace

core::Result<std::filesystem::path> LinuxAppDataLocation::resolve(std::optional<std::string_view> xdgDataHome,
                                                                   std::optional<std::string_view> home) {
    if (xdgDataHome.has_value() && !xdgDataHome->empty() && xdgDataHome->front() == '/') {
        return std::filesystem::path(std::string(*xdgDataHome)) / "rivet";
    }
    if (home.has_value() && !home->empty() && home->front() == '/') {
        return std::filesystem::path(std::string(*home)) / ".local" / "share" / "rivet";
    }
    return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                           "no home directory for the application data", "platform"));
}

core::Result<std::filesystem::path> LinuxAppDataLocation::directory() const {
    if (std::optional<std::filesystem::path> overridden = appDataOverride()) return *std::move(overridden);
    auto resolved = resolve(environment("XDG_DATA_HOME"), environment("HOME"));
    if (resolved.has_value()) return resolved;
    const std::optional<std::string> fallback = passwdHome();
    if (!fallback.has_value()) return resolved;
    return resolve(std::nullopt, std::string_view(*fallback));
}

} // namespace rivet::platform
