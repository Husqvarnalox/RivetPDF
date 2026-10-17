// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/PlatformKit.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rivet::platform {

class LinuxAlertService;
class X11Connection;
class X11Window;

// Open/save dialogs for the X11 shell through the desktop's own chooser,
// run as a subprocess: `zenity` (GTK/GNOME) when found on $PATH, otherwise
// `kdialog` (KDE). xdg-desktop-portal is out of scope for now.
//
//   - posix_spawn, no shell; titles are stripped of control characters and
//     path arguments never start with '-'.
//   - App-modal like NSOpenPanel: while the chooser runs, Rivet's windows
//     repaint but take no input and main-thread tasks wait (modal pump).
//   - Exit code 1 (the user dismissed the chooser) -> ErrorCode::Cancelled /
//     nullopt. With neither tool installed the open calls report
//     ErrorCode::Unsupported ("install zenity or kdialog") and the
//     composition root leaves ShellServices::saveDialog null.
//   - Save: zenity confirms overwriting itself (--confirm-overwrite); for
//     kdialog, and whenever Rivet appends the default extension to the typed
//     name, Rivet asks through LinuxAlertService before returning an
//     existing path.
class LinuxFileDialog final : public IFileDialog, public ISaveDialog {
public:
    LinuxFileDialog(X11Connection& connection, LinuxAlertService* alerts);

    // A chooser tool was found at construction.
    bool available() const { return tool_ != Tool::None; }
    void setOwner(X11Window* owner) { owner_ = owner; }

    core::Result<std::filesystem::path> openPdf() override;
    core::Result<std::filesystem::path> openDocument() override;
    core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) override;
    core::Result<std::filesystem::path> openImage() override;

    std::optional<std::filesystem::path> runSavePanel(const ISaveDialog::Options& options) override;

private:
    enum class Tool : unsigned char { None, Zenity, Kdialog };

    struct Filter {
        std::string name;                    // "PDF documents"
        std::vector<std::string> extensions; // no dot, lower case
    };

    core::Result<std::vector<std::filesystem::path>> runOpen(const std::string& title, const Filter& filter,
                                                             bool multiple);

    X11Connection& connection_;
    LinuxAlertService* alerts_ = nullptr;
    X11Window* owner_ = nullptr;
    Tool tool_ = Tool::None;
    std::filesystem::path toolPath_;
};

} // namespace rivet::platform
