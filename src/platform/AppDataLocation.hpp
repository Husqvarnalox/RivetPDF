// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace rivet::platform {

// The per-user application-data directory of Rivet (ADR-0025: the saved
// signatures live there). Implemented per platform:
//   - macOS:   ~/Library/Application Support/Rivet (NSFileManager)
//   - Linux:   $XDG_DATA_HOME/rivet, else ~/.local/share/rivet
//   - Windows: %APPDATA%\Rivet (SHGetKnownFolderPath(FOLDERID_RoamingAppData))
//
// The directory is NOT created here: writers create it (owner-only where
// the platform allows) before the first write. An unavailable location
// (no home directory, API failure) is ErrorCode::NotAvailable. Thread-safe.
//
// Every implementation first honours appDataOverride() (below), so tests and
// scripted QA runs never touch the user's real preferences, Recent list or
// signatures.
class IAppDataLocation {
public:
    virtual ~IAppDataLocation() = default;

    virtual core::Result<std::filesystem::path> directory() const = 0;
};

// The override rule shared by the three platform implementations, for given
// variable values (nullopt = unset):
//   - RIVET_APP_DATA=<absolute path> wins;
//   - else, while RIVET_QA_SCRIPT is set (scripted QA run), a throwaway
//     "rivet-qa-appdata" directory under `tempDir`;
//   - else nullopt (use the platform location).
inline std::optional<std::filesystem::path> resolveAppDataOverride(std::optional<std::string_view> appData,
                                                                   std::optional<std::string_view> qaScript,
                                                                   const std::filesystem::path& tempDir) {
    if (appData.has_value() && !appData->empty()) {
        std::filesystem::path path{std::string(*appData)};
        if (path.is_absolute()) return path;
    }
    if (qaScript.has_value() && !qaScript->empty() && !tempDir.empty()) return tempDir / "rivet-qa-appdata";
    return std::nullopt;
}

// resolveAppDataOverride() over the process environment.
inline std::optional<std::filesystem::path> appDataOverride() {
    const auto env = [](const char* name) -> std::optional<std::string_view> {
        const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read-only after startup
        if (value == nullptr) return std::nullopt;
        return std::string_view(value);
    };
    const std::optional<std::string_view> qaScript = env("RIVET_QA_SCRIPT");
    std::filesystem::path tempDir;
    if (qaScript.has_value()) {
        std::error_code error;
        tempDir = std::filesystem::temp_directory_path(error);
        if (error) tempDir.clear();
    }
    return resolveAppDataOverride(env("RIVET_APP_DATA"), qaScript, tempDir);
}

} // namespace rivet::platform
