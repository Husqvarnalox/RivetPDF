// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/AppDataLocation.hpp"

namespace rivet::platform {

// %APPDATA%\Rivet: SHGetKnownFolderPath(FOLDERID_RoamingAppData) + "Rivet".
// Thread-safe.
class WindowsAppDataLocation final : public IAppDataLocation {
public:
    core::Result<std::filesystem::path> directory() const override;
};

} // namespace rivet::platform
