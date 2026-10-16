// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Print.hpp"

struct HWND__;

namespace rivet::platform {

class WindowsMainThreadDispatcher;

// Win32 printing. choosePrintSettings() shows the common print dialog
// (PrintDlgExW: printer, page range, copies, preferences) with the initial
// orientation taken from the first page, and keeps the printer DC in the
// opaque PrintSettings::platformHandle. printSpool() runs a GDI job
// (StartDoc/StartPage) that only composites the pre-rendered bands with
// StretchDIBits, each page scaled to fit the printable area and centered
// (like the CUPS "fit-to-page" option on Linux). Page content always comes
// from the Rivet render pipeline. Copies/collation are left to the driver
// (PD_USEDEVMODECOPIESANDCOLLATE). A cancelled StartDoc (e.g. the
// "Microsoft Print to PDF" save prompt) is ErrorCode::Cancelled.
// Main thread only; main-thread tasks are deferred while dialogs are up.
class WindowsPrintService final : public IPrintService {
public:
    WindowsPrintService(HWND__* owner, WindowsMainThreadDispatcher* dispatcher)
        : owner_(owner), dispatcher_(dispatcher) {}

    core::Result<PrintSettings> choosePrintSettings(const PrintSetup& setup) override;
    core::Status printSpool(const PrintSettings& settings, const PrintSpoolDescription& spool) override;

private:
    HWND__* owner_ = nullptr;
    WindowsMainThreadDispatcher* dispatcher_ = nullptr;
};

} // namespace rivet::platform
