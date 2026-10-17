// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/PlatformKit.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct HWND__;

namespace rivet::platform {

class WindowsAlertService;
class WindowsMainThreadDispatcher;

// Open/save dialogs through the shell's common item dialogs
// (IFileOpenDialog / IFileSaveDialog), app-modal over the owner window, with
// main-thread tasks deferred while they are up (like NSOpenPanel).
//   - Filters: PDF; PDF + Markdown (.md/.markdown/.mdown); PNG/JPEG.
//   - Only file-system items (FOS_FORCEFILESYSTEM); the user dismissing the
//     dialog is ErrorCode::Cancelled / nullopt.
//   - Save: FOS_OVERWRITEPROMPT confirms replacing a chosen existing file;
//     when Rivet appends the default extension to a name typed without an
//     allowed one, it asks through the alert service before returning an
//     existing path (same rule as the Linux backend).
// Requires a COM apartment on the main thread. Main thread only.
class WindowsFileDialog final : public IFileDialog, public ISaveDialog {
public:
    WindowsFileDialog(HWND__* owner, WindowsMainThreadDispatcher* dispatcher, WindowsAlertService* alerts)
        : owner_(owner), dispatcher_(dispatcher), alerts_(alerts) {}

    void setOwner(HWND__* owner) { owner_ = owner; }

    core::Result<std::filesystem::path> openPdf() override;
    core::Result<std::filesystem::path> openDocument() override;
    core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) override;
    core::Result<std::filesystem::path> openImage() override;

    std::optional<std::filesystem::path> runSavePanel(const ISaveDialog::Options& options) override;

private:
    struct Filter {
        std::wstring name;                   // "PDF documents"
        std::vector<std::wstring> patterns; // L"*.pdf"
    };

    core::Result<std::vector<std::filesystem::path>> runOpen(const std::string& title, const std::string& prompt,
                                                             const std::vector<Filter>& filters, bool multiple);

    HWND__* owner_ = nullptr;
    WindowsMainThreadDispatcher* dispatcher_ = nullptr;
    WindowsAlertService* alerts_ = nullptr;
};

} // namespace rivet::platform
