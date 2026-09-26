// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/PlatformKit.hpp"

namespace rivet::platform {

// NSOpenPanel / NSSavePanel-backed PDF chooser. App-modal (runModal) rather
// than window sheets: the callers are synchronous and main-thread only.
class MacosFileDialog final : public IFileDialog, public ISaveDialog {
public:
    core::Result<std::filesystem::path> openPdf() override;
    core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) override;
    std::optional<std::filesystem::path> runSavePanel(const ISaveDialog::Options& options) override;
};

} // namespace rivet::platform
