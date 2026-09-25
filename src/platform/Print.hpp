// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rivet::platform {

// Printing is a two-step flow so that NO PDF rasterization ever happens on
// the main thread:
//
//   1. choosePrintSettings(): the native print panel (modal, main thread,
//      no rendering) -> the user's page range + an opaque platform handle.
//   2. The application spools the chosen pages on a worker (Rivet render
//      pipeline, banded, written to raw files; see editor::PrintSpooler).
//   3. printSpool(): runs the platform print job over the spool files. The
//      platform side only COMPOSITES the pre-rendered bands.
//
// Page content always comes from the Rivet render pipeline - printing never
// uses an alternate PDF engine (no PDFKit).

// What the print panel needs to know about the document.
struct PrintSetup {
    std::string jobTitle;
    std::size_t pageCount = 0;
    // Display size of the first page; selects the panel's initial paper
    // orientation (landscape documents default to landscape).
    core::Size firstPageSizePoints;
};

// The user's choices from the print panel.
struct PrintSettings {
    std::string jobTitle;
    // Selected pages, 0-based inclusive, clamped to the document.
    std::size_t firstPage = 0;
    std::size_t lastPage = 0;
    // Platform print state (printer, paper, copies, ...). Opaque to shared
    // code; only the backend that produced it may interpret it.
    std::shared_ptr<void> platformHandle;
};

// One pre-rendered horizontal strip of a page, stored as a raw file of
// pixelHeight rows of `stride` bytes, pixels in core::PixelFormat
// BGRA8888Straight order (B, G, R, A; straight alpha).
struct PrintSpoolBand {
    std::filesystem::path file;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::size_t stride = 0;
    // Where the band goes, in page display space (points, top-left origin,
    // y-down). The bands of a page tile it exactly, top to bottom.
    core::Rect rectPoints;
};

struct PrintSpoolPage {
    std::size_t pageIndex = 0; // document page index (informational)
    core::Size displaySizePoints;
    std::vector<PrintSpoolBand> bands;
};

// Engine- and editor-independent description of a finished spool. The
// files it names are owned by the spool's producer and must stay alive
// until printSpool() returns.
struct PrintSpoolDescription {
    std::vector<PrintSpoolPage> pages; // in print order
};

// Platform print service. Main thread only.
class IPrintService {
public:
    virtual ~IPrintService() = default;

    // Shows the native print panel. Returns ErrorCode::Cancelled when the
    // user dismisses it. Performs no rendering.
    virtual core::Result<PrintSettings> choosePrintSettings(const PrintSetup& setup) = 0;

    // Prints the spooled pages with settings from choosePrintSettings().
    // Fails (never prints blank sheets) when a band cannot be read; returns
    // ErrorCode::Cancelled when the user cancels the running job.
    virtual core::Status printSpool(const PrintSettings& settings,
                                    const PrintSpoolDescription& spool) = 0;
};

} // namespace rivet::platform
