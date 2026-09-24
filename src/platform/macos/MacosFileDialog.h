#pragma once

#include "platform/PlatformKit.hpp"

namespace rivet::platform {

// NSOpenPanel-backed PDF chooser. Must be used on the main thread (runModal).
class MacosFileDialog final : public IFileDialog {
public:
    core::Result<std::filesystem::path> openPdf() override;
};

} // namespace rivet::platform
