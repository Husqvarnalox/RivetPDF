// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Print.hpp"

namespace rivet::platform {

class LinuxAlertService;

// CUPS-backed print service (built only when libcups is found). Main thread
// only.
//
//   choosePrintSettings(): there is no native print panel on plain X11, so
//     the "panel" is minimal: the CUPS default destination, every page, and
//     a Cancel/Print confirmation through LinuxAlertService. No default
//     printer -> ErrorCode::NotAvailable. No rendering happens here.
//   printSpool(): composites the pre-rendered bands into a temporary PDF
//     (cairo PDF surface, one page per spooled page at its display size),
//     submits it with cupsPrintFile (fit-to-page) and deletes it. A band
//     that cannot be read fails the job before anything is submitted (never
//     prints blank sheets).
class LinuxPrintService final : public IPrintService {
public:
    explicit LinuxPrintService(LinuxAlertService* alerts) : alerts_(alerts) {}

    core::Result<PrintSettings> choosePrintSettings(const PrintSetup& setup) override;
    core::Status printSpool(const PrintSettings& settings, const PrintSpoolDescription& spool) override;

private:
    LinuxAlertService* alerts_ = nullptr;
};

} // namespace rivet::platform
