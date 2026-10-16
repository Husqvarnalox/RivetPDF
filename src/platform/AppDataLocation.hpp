// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <filesystem>

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
class IAppDataLocation {
public:
    virtual ~IAppDataLocation() = default;

    virtual core::Result<std::filesystem::path> directory() const = 0;
};

} // namespace rivet::platform
