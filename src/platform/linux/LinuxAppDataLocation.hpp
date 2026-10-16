// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/AppDataLocation.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace rivet::platform {

// $XDG_DATA_HOME/rivet when XDG_DATA_HOME is set to an ABSOLUTE path (the
// XDG Base Directory spec says relative values are invalid and must be
// ignored), else $HOME/.local/share/rivet, else the passwd home directory.
// Thread-safe (environment reads only; nothing is cached).
class LinuxAppDataLocation final : public IAppDataLocation {
public:
    core::Result<std::filesystem::path> directory() const override;

    // The rule above for given variable values (nullopt = unset); exposed for
    // tests. NotAvailable when no usable directory remains.
    static core::Result<std::filesystem::path> resolve(std::optional<std::string_view> xdgDataHome,
                                                       std::optional<std::string_view> home);
};

} // namespace rivet::platform
