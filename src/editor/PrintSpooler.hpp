// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace rivet::editor {

// One page available for printing. The render function rasterizes a band
// of the page: bandRectPoints is in page display space (points, top-left
// origin, y-down), the returned bitmap must cover it at devicePixelsPerPoint
// (the backend may round the pixel size UP; the spool keeps its planned
// grid). The spooler calls it ONLY on its worker, one call at a time.
struct PrintPageSource {
    core::Size displaySizePoints;
    std::function<core::Result<core::Bitmap>(const core::Rect& bandRectPoints,
                                             double devicePixelsPerPoint)>
        render;
};

// Density and memory policy. Defaults (see docs/ARCHITECTURE.md, printing):
//   - 150 dpi target: good text quality on office printers at 4x fewer
//     pixels than 300 dpi (US Letter = 2.1 MP = 8.4 MiB spooled).
//   - 4 Mi-pixel bands (16 MiB of BGRA): the only raster in memory at once.
//   - 48 Mi-pixel pages (192 MiB spooled): A0 (~35 MP at 150 dpi) still gets
//     the full density; only larger posters are reduced.
//   - 0.5 px/pt floor (36 dpi): a 200-inch (14400 pt) page still prints
//     instead of failing; the floor may exceed the per-page budget, but
//     never the band budget or the bitmap dimension limit.
//   - 1 GiB spool: the job's disk footprint (~120 Letter pages at 150 dpi);
//     longer jobs are scaled down globally (never below the floor) before
//     anything is rendered, and fail when even the floor does not fit.
struct PrintSpoolOptions {
    double targetPixelsPerPoint = 150.0 / 72.0;
    double minPixelsPerPoint = 0.5;
    std::uint64_t maxBandPixels = std::uint64_t{4} << 20;
    std::uint64_t maxPagePixels = std::uint64_t{48} << 20;
    std::uint64_t maxSpoolBytes = std::uint64_t{1} << 30;

    // Pages to print, 0-based inclusive. lastPage is clamped to the page
    // count; firstPage beyond the document is an error.
    std::size_t firstPage = 0;
    std::size_t lastPage = std::numeric_limits<std::size_t>::max();

    // Parent of the per-job spool directory. Empty = the system temp dir.
    std::filesystem::path spoolParentDirectory;
};

// A planned band: pixel rows [firstRow, firstRow + pixelHeight) of the
// page raster, covering rectPoints.
struct PrintBandPlan {
    std::uint32_t firstRow = 0;
    std::uint32_t pixelHeight = 0;
    core::Rect rectPoints;
};

struct PrintPagePlan {
    std::size_t pageIndex = 0;
    core::Size displaySizePoints;
    double pixelsPerPoint = 0.0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::vector<PrintBandPlan> bands;
};

struct PrintSpoolPlan {
    std::vector<PrintPagePlan> pages;
    std::uint64_t totalBytes = 0; // spool files, tightly packed BGRA
};

// A spooled band on disk: pixelHeight rows of `stride` bytes (tightly
// packed, stride == pixelWidth * 4), BGRA8888Straight.
struct SpooledBand {
    std::filesystem::path file;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::size_t stride = 0;
    core::Rect rectPoints; // page display space
};

struct SpooledPage {
    std::size_t pageIndex = 0;
    core::Size displaySizePoints;
    double pixelsPerPoint = 0.0;
    std::vector<SpooledBand> bands;
};

// A finished spool. Owns its directory: destruction (or move-assignment
// over it) removes the directory and every band file. Movable, not
// copyable.
class PrintSpool {
public:
    PrintSpool() = default;
    PrintSpool(std::filesystem::path directory, std::vector<SpooledPage> pages);
    ~PrintSpool();

    PrintSpool(PrintSpool&& other) noexcept;
    PrintSpool& operator=(PrintSpool&& other) noexcept;
    PrintSpool(const PrintSpool&) = delete;
    PrintSpool& operator=(const PrintSpool&) = delete;

    const std::filesystem::path& directory() const { return directory_; }
    const std::vector<SpooledPage>& pages() const { return pages_; }

private:
    void removeDirectory() noexcept;

    std::filesystem::path directory_;
    std::vector<SpooledPage> pages_;
};

// Renders the selected pages of a print job, band by band, into a spool
// directory on a worker so the UI thread never rasterizes:
//
//   start() (main)  -> plan densities/bands (pure arithmetic)
//                   -> worker: for each page, for each band:
//                        render -> write raw file -> drop the bitmap
//                   -> onProgress(pagesDone, pageCount)  [main, via dispatcher]
//                   -> onComplete(Result<PrintSpool>)     [main, via dispatcher]
//
// Memory is bounded by ONE band. The first render or I/O failure aborts the
// job, removes the spool directory and is reported as the job's error (never
// a partial success). cancel() is checked between bands; the job then
// completes with ErrorCode::Cancelled.
//
// Threading: start()/cancel()/isActive()/waitForWorker() and the destructor
// are main-thread only; callbacks are only ever invoked through the
// dispatcher, never on the worker. The destructor cancels, waits for the
// worker and turns every still-queued delivery into a no-op.
class PrintSpooler {
public:
    using ProgressCallback = std::function<void(std::size_t pagesDone, std::size_t pageCount)>;
    using CompletionCallback = std::function<void(core::Result<PrintSpool>)>;

    // The scheduler must outlive the spooler; the dispatcher is required for
    // start() (null disables printing).
    PrintSpooler(core::TaskScheduler& scheduler, core::IMainThreadDispatcher* dispatcher);
    ~PrintSpooler();

    PrintSpooler(const PrintSpooler&) = delete;
    PrintSpooler& operator=(const PrintSpooler&) = delete;

    // Starts a job. Fails synchronously only when the spooler cannot accept
    // a job (one is active, no dispatcher, no completion callback); every
    // job outcome - including planning errors - arrives via onComplete.
    core::Status start(std::vector<PrintPageSource> pages, PrintSpoolOptions options,
                       ProgressCallback onProgress, CompletionCallback onComplete);

    // Requests cancellation of the active job (non-blocking).
    void cancel();

    // Blocks until the worker is idle (at most one band render when the job
    // was cancelled). Completions are still delivered via the dispatcher.
    void waitForWorker();

    // True from start() until the job's completion has been delivered.
    bool isActive() const { return job_ != nullptr; }

    // The active job's cancellation flag (null when idle). It turns true on
    // cancel() or destruction and is notified (std::atomic::wait), so render
    // functions or observers may react without polling.
    std::shared_ptr<const std::atomic<bool>> cancellationToken() const;

    // Density/band planning, exposed for tests and diagnostics. Pure.
    static core::Result<PrintSpoolPlan> plan(const std::vector<PrintPageSource>& pages,
                                             const PrintSpoolOptions& options);

private:
    struct Job;

    static void runJob(const std::shared_ptr<Job>& job);

    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    // Main-thread flag shared with queued deliveries; false once destroyed.
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<Job> job_; // main thread
    // Declared last: destroyed first, after the destructor already waited.
    core::SerialExecutor executor_;
};

} // namespace rivet::editor
