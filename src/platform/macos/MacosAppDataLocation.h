// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/AppDataLocation.hpp"

namespace rivet::platform {

// ~/Library/Application Support/Rivet, resolved through NSFileManager
// (NSApplicationSupportDirectory in the user domain). Thread-safe.
class MacosAppDataLocation final : public IAppDataLocation {
public:
    core::Result<std::filesystem::path> directory() const override;
};

} // namespace rivet::platform
