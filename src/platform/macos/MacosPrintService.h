// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Print.hpp"

#include <filesystem>

namespace rivet::platform {

// AppKit-backed print service. Main thread only.
//
//   choosePrintSettings(): NSPrintPanel over a copy of the shared
//     NSPrintInfo (no preview: the panel never triggers rendering).
//   printSpool(): NSPrintOperation without a panel over a flipped view that
//     composites the pre-rendered band files (file-backed CGImages). No PDF
//     rasterization happens on the main thread.
//
// A band that cannot be read at draw time cancels the job through the
// operation's printInfo.jobDisposition = NSPrintCancelJob (verified by
// tests/platform/TestPrintService.mm: -runOperation then returns NO and no
// output is produced) and printSpool() returns that error.
class MacosPrintService final : public IPrintService {
public:
    core::Result<PrintSettings> choosePrintSettings(const PrintSetup& setup) override;
    core::Status printSpool(const PrintSettings& settings, const PrintSpoolDescription& spool) override;

    // Non-interactive settings that save the job as a PDF file at `output`
    // (NSPrintSaveJob). Used by tests; the page range is informational.
    static PrintSettings settingsForSavingPdf(const std::filesystem::path& output,
                                              std::string jobTitle);

    // The progress panel is shown by default; tests turn it off.
    void setShowsProgressPanel(bool shows) { showsProgressPanel_ = shows; }

private:
    bool showsProgressPanel_ = true;
};

} // namespace rivet::platform
