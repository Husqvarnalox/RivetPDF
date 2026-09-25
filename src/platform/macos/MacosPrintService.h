// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Print.hpp"

#import <AppKit/AppKit.h>

namespace rivet::platform {

// AppKit-backed print service. Main thread only (NSPrintOperation).
//
// Density policy: page rasters are produced by the Rivet callback at a
// CAPPED density (kMaxPrintDensity px/pt) so a full page never exceeds a
// bounded memory footprint (~11 MB for US Letter at the cap). Banded/tiled
// print rendering for very high printer DPI is a documented future
// optimization; the dialog, page range and paper handling are all AppKit's.
class MacosPrintService final : public IPrintService {
public:
    core::Status printDocument(const PrintRequest& request) override;

private:
    // The print view needs the request alive for the whole operation; it is
    // owned by the print operation's view during printDocument().
    PrintRequest request_;
};

} // namespace rivet::platform
