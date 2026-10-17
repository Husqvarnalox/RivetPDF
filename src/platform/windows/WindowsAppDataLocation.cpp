// SPDX-License-Identifier: MPL-2.0
#include "WindowsAppDataLocation.hpp"

#include "WinCommon.hpp"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

namespace rivet::platform {

core::Result<std::filesystem::path> WindowsAppDataLocation::directory() const {
    if (std::optional<std::filesystem::path> overridden = appDataOverride()) return *std::move(overridden);
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) ::CoTaskMemFree(raw);
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "the roaming application-data folder is not available", "platform"));
    }
    std::filesystem::path base(raw);
    ::CoTaskMemFree(raw);
    return base / L"Rivet";
}

} // namespace rivet::platform
