// SPDX-License-Identifier: MPL-2.0
#include "editor/PrintSpooler.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <new>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace rivet::editor {

namespace {

constexpr std::uint64_t kBytesPerPixel = 4; // BGRA8888Straight
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
// Keeps ceil(size * density) at or below a hard pixel limit despite
// floating-point rounding in the product.
constexpr double kLimitShrink = 1.0 - 1e-9;
constexpr double kGridTolerance = 1e-6; // pixels
// Global density rescaling converges in a few rounds; the cap only guards
// against pathological inputs.
constexpr int kMaxScaleIterations = 64;
constexpr int kMaxDirectoryAttempts = 16;

core::Error spoolError(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), "print");
}

std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

// Pixel count covering `extent` device pixels: ceil like the render backend,
// but tolerant of floating-point noise (792 pt * 150/72 is 1650.0000000002,
// not 1651 pixels). The backend may then return one extra row/column, which
// the spool crops.
std::uint32_t gridPixels(double extent) {
    return static_cast<std::uint32_t>(std::max(1.0, std::ceil(extent - kGridTolerance)));
}

// Plans one page at `baseDensity` (px/pt) under the per-page and per-band
// budgets.
PrintPagePlan planPage(std::size_t pageIndex, core::Size size, double baseDensity,
                       double floorDensity, const PrintSpoolOptions& options) {
    const double area = size.width * size.height;
    double density = baseDensity;
    const auto maxPagePixels = static_cast<double>(options.maxPagePixels);
    if ((std::ceil(size.width * density) * std::ceil(size.height * density)) > maxPagePixels) {
        // Largest d with (w*d + 1) * (h*d + 1) <= budget, so the budget
        // holds after rounding the pixel grid up:
        //   w*h*d^2 + (w + h)*d + 1 - budget = 0.
        const double b = size.width + size.height;
        const double c = 1.0 - maxPagePixels;
        density = (-b + std::sqrt(b * b - 4.0 * area * c)) / (2.0 * area) * kLimitShrink;
    }
    density = std::max(density, floorDensity);

    // Hard bounds (override the floor): a 1-px strip of the full width must
    // fit the band budget, and neither axis may exceed the bitmap limit.
    const double widthLimit = std::min(static_cast<double>(options.maxBandPixels),
                                       static_cast<double>(core::kMaxBitmapDimension));
    if (size.width * density > widthLimit) density = widthLimit / size.width * kLimitShrink;
    const auto heightLimit = static_cast<double>(core::kMaxBitmapDimension);
    if (size.height * density > heightLimit) density = heightLimit / size.height * kLimitShrink;

    PrintPagePlan page;
    page.pageIndex = pageIndex;
    page.displaySizePoints = size;
    page.pixelsPerPoint = density;
    page.pixelWidth = gridPixels(size.width * density);
    page.pixelHeight = gridPixels(size.height * density);

    // Horizontal strips: as many full-width rows as the band budget allows.
    const std::uint64_t rowsPerBand = std::clamp<std::uint64_t>(
        options.maxBandPixels / page.pixelWidth, 1, page.pixelHeight);
    for (std::uint64_t row = 0; row < page.pixelHeight; row += rowsPerBand) {
        const std::uint64_t endRow = std::min<std::uint64_t>(row + rowsPerBand, page.pixelHeight);
        const double top = static_cast<double>(row) / density;
        // The last band ends exactly on the page edge (the grid may overhang
        // it by less than one pixel).
        const double bottom =
            endRow == page.pixelHeight ? size.height : static_cast<double>(endRow) / density;
        page.bands.push_back(PrintBandPlan{static_cast<std::uint32_t>(row),
                                           static_cast<std::uint32_t>(endRow - row),
                                           core::Rect{0.0, top, size.width, bottom - top}});
    }
    return page;
}

std::uint64_t pageBytes(const PrintPagePlan& page) {
    return std::uint64_t{page.pixelWidth} * page.pixelHeight * kBytesPerPixel;
}

std::string randomHex() {
    std::random_device device;
    std::mt19937_64 generator((std::uint64_t{device()} << 32) ^ device());
    return std::format("{:016x}", generator());
}

// Creates a fresh, uniquely named spool directory, owner-only where the
// filesystem supports permissions.
core::Result<std::filesystem::path> createSpoolDirectory(const std::filesystem::path& parent) {
    std::error_code error;
    const std::filesystem::path base =
        parent.empty() ? std::filesystem::temp_directory_path(error) : parent;
    if (error) {
        return std::unexpected(spoolError(core::ErrorCode::Io,
                                          "no temporary directory for the print spool: " +
                                              error.message()));
    }
    for (int attempt = 0; attempt < kMaxDirectoryAttempts; ++attempt) {
        std::filesystem::path candidate = base / ("rivet-print-" + randomHex());
        // create_directory reports false (no error) when the name exists.
        if (std::filesystem::create_directory(candidate, error)) {
            std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            return candidate;
        }
        if (error) {
            return std::unexpected(spoolError(core::ErrorCode::Io,
                                              "cannot create the print spool directory: " +
                                                  error.message()));
        }
    }
    return std::unexpected(
        spoolError(core::ErrorCode::Io, "cannot find a unique print spool directory name"));
}

// Removes a spool directory unless released (error paths).
class DirectoryGuard {
public:
    explicit DirectoryGuard(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ~DirectoryGuard() {
        if (directory_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    DirectoryGuard(const DirectoryGuard&) = delete;
    DirectoryGuard& operator=(const DirectoryGuard&) = delete;

    std::filesystem::path release() { return std::exchange(directory_, {}); }

private:
    std::filesystem::path directory_;
};

// Writes rows [0, band.pixelHeight) x [0, pixelWidth) of the bitmap tightly
// packed. The bitmap may be larger (backend ceil rounding); never smaller.
core::Status writeBand(const std::filesystem::path& file, const core::Bitmap& bitmap,
                       std::uint32_t pixelWidth, std::uint32_t pixelHeight) {
    if (bitmap.width() < pixelWidth || bitmap.height() < pixelHeight) {
        return std::unexpected(spoolError(
            core::ErrorCode::Internal,
            std::format("renderer returned {}x{} pixels for a {}x{} print band", bitmap.width(),
                        bitmap.height(), pixelWidth, pixelHeight)));
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(spoolError(core::ErrorCode::Io, "cannot create print spool file"));
    }
    const auto rowBytes = static_cast<std::streamsize>(std::size_t{pixelWidth} * kBytesPerPixel);
    for (std::uint32_t row = 0; row < pixelHeight && out; ++row) {
        const std::byte* source = bitmap.data() + std::size_t{row} * bitmap.stride();
        out.write(reinterpret_cast<const char*>(source), rowBytes);
    }
    out.close();
    if (!out) {
        return std::unexpected(spoolError(core::ErrorCode::Io, "cannot write print spool file"));
    }
    return core::ok();
}

} // namespace

// ---------------------------------------------------------------- PrintSpool

PrintSpool::PrintSpool(std::filesystem::path directory, std::vector<SpooledPage> pages)
    : directory_(std::move(directory)), pages_(std::move(pages)) {}

PrintSpool::~PrintSpool() { removeDirectory(); }

PrintSpool::PrintSpool(PrintSpool&& other) noexcept
    : directory_(std::exchange(other.directory_, {})), pages_(std::move(other.pages_)) {}

PrintSpool& PrintSpool::operator=(PrintSpool&& other) noexcept {
    if (this != &other) {
        removeDirectory();
        directory_ = std::exchange(other.directory_, {});
        pages_ = std::move(other.pages_);
    }
    return *this;
}

void PrintSpool::removeDirectory() noexcept {
    if (directory_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
    directory_.clear();
}

// -------------------------------------------------------------- PrintSpooler

// Shared between the main thread (deliveries) and the worker (runJob).
// Worker-only: pages, plan, directory. Main-only: callbacks, owner deref
// (only while *alive).
struct PrintSpooler::Job {
    std::vector<PrintPageSource> pages;
    PrintSpoolPlan plan;
    std::filesystem::path parentDirectory;
    ProgressCallback onProgress;
    CompletionCallback onComplete;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    core::IMainThreadDispatcher* dispatcher = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
    PrintSpooler* owner = nullptr;

    void requestCancel() {
        cancelled->store(true);
        cancelled->notify_all();
    }

    // Delivery guard (main thread): the owner is alive and this is still
    // its active job.
    bool isCurrent(const std::shared_ptr<Job>& self) const {
        return alive->load() && owner->job_ == self;
    }

    static void postProgress(const std::shared_ptr<Job>& job, std::size_t done, std::size_t total) {
        job->dispatcher->post([job, done, total] {
            if (!job->isCurrent(job) || !job->onProgress) return;
            job->onProgress(done, total);
        });
    }

    static void postCompletion(const std::shared_ptr<Job>& job, core::Result<PrintSpool> result) {
        // std::function needs a copyable closure; the spool is move-only. A
        // dropped delivery destroys the spool (and its directory) with it.
        auto payload = std::make_shared<core::Result<PrintSpool>>(std::move(result));
        job->dispatcher->post([job, payload] {
            if (!job->isCurrent(job)) return;
            job->owner->job_.reset();
            CompletionCallback onComplete = std::move(job->onComplete);
            job->onProgress = nullptr;
            onComplete(std::move(*payload));
        });
    }
};

PrintSpooler::PrintSpooler(core::TaskScheduler& scheduler, core::IMainThreadDispatcher* dispatcher)
    : dispatcher_(dispatcher),
      alive_(std::make_shared<std::atomic<bool>>(true)),
      executor_(scheduler) {}

PrintSpooler::~PrintSpooler() {
    alive_->store(false);
    if (job_ != nullptr) job_->requestCancel();
    // The worker checks the flag between bands: this waits for at most one
    // in-flight band render. Its completion post becomes a no-op.
    executor_.waitUntilIdle();
}

core::Status PrintSpooler::start(std::vector<PrintPageSource> pages, PrintSpoolOptions options,
                                 ProgressCallback onProgress, CompletionCallback onComplete) {
    if (job_ != nullptr) {
        return std::unexpected(spoolError(core::ErrorCode::NotAvailable,
                                          "a print job is already being prepared"));
    }
    if (dispatcher_ == nullptr || !onComplete) {
        return std::unexpected(spoolError(core::ErrorCode::InvalidArgument,
                                          "print spooling needs a dispatcher and a completion"));
    }
    auto job = std::make_shared<Job>();
    job->dispatcher = dispatcher_;
    job->alive = alive_;
    job->owner = this;
    job->onProgress = std::move(onProgress);
    job->onComplete = std::move(onComplete);
    job_ = job;

    auto planned = plan(pages, options);
    if (!planned.has_value()) {
        Job::postCompletion(job, std::unexpected(planned.error()));
        return core::ok();
    }
    job->plan = std::move(*planned);
    job->pages = std::move(pages);
    job->parentDirectory = std::move(options.spoolParentDirectory);
    executor_.post([job] { runJob(job); });
    return core::ok();
}

void PrintSpooler::cancel() {
    if (job_ != nullptr) job_->requestCancel();
}

void PrintSpooler::waitForWorker() { executor_.waitUntilIdle(); }

std::shared_ptr<const std::atomic<bool>> PrintSpooler::cancellationToken() const {
    return job_ != nullptr ? job_->cancelled : nullptr;
}

core::Result<PrintSpoolPlan> PrintSpooler::plan(const std::vector<PrintPageSource>& pages,
                                                const PrintSpoolOptions& options) {
    if (pages.empty() || options.firstPage >= pages.size()) {
        return std::unexpected(spoolError(core::ErrorCode::InvalidArgument,
                                          "the selected page range is outside the document"));
    }
    const std::size_t lastPage = std::min(options.lastPage, pages.size() - 1);
    if (lastPage < options.firstPage) {
        return std::unexpected(
            spoolError(core::ErrorCode::InvalidArgument, "the selected page range is empty"));
    }
    const double target = options.targetPixelsPerPoint;
    if (!std::isfinite(target) || target <= 0.0 || !std::isfinite(options.minPixelsPerPoint) ||
        options.minPixelsPerPoint <= 0.0 || options.maxBandPixels == 0 ||
        options.maxPagePixels == 0 || options.maxSpoolBytes == 0) {
        return std::unexpected(
            spoolError(core::ErrorCode::InvalidArgument, "invalid print density or budget"));
    }
    for (std::size_t index = options.firstPage; index <= lastPage; ++index) {
        const core::Size size = pages[index].displaySizePoints;
        if (!size.isFinite() || size.isEmpty() || !pages[index].render) {
            return std::unexpected(spoolError(core::ErrorCode::InvalidArgument,
                                              std::format("page {} cannot be printed (no size "
                                                          "or renderer)",
                                                          index + 1)));
        }
    }

    // Global budget: shrink the base density until the whole job fits,
    // never below the floor.
    const double floorDensity = std::min(options.minPixelsPerPoint, target);
    double base = target;
    for (int iteration = 0; iteration < kMaxScaleIterations; ++iteration) {
        PrintSpoolPlan result;
        result.pages.reserve(lastPage - options.firstPage + 1);
        for (std::size_t index = options.firstPage; index <= lastPage; ++index) {
            result.pages.push_back(
                planPage(index, pages[index].displaySizePoints, base, floorDensity, options));
            result.totalBytes = saturatingAdd(result.totalBytes, pageBytes(result.pages.back()));
        }
        if (result.totalBytes <= options.maxSpoolBytes) return result;
        if (base <= floorDensity) {
            return std::unexpected(spoolError(
                core::ErrorCode::OutOfMemory,
                std::format("the print job needs {} MiB of spool even at the minimum density "
                            "({:.0f} dpi), above the {} MiB limit; print fewer pages",
                            result.totalBytes / kMiB, floorDensity * 72.0,
                            options.maxSpoolBytes / kMiB)));
        }
        // Bytes scale with density squared; undershoot slightly so rounding
        // does not force another round.
        const double factor = std::sqrt(static_cast<double>(options.maxSpoolBytes) /
                                        static_cast<double>(result.totalBytes));
        base = std::max(floorDensity, base * factor * 0.99);
    }
    return std::unexpected(
        spoolError(core::ErrorCode::Internal, "print density planning did not converge"));
}

void PrintSpooler::runJob(const std::shared_ptr<Job>& job) {
    const auto cancelledError = [] {
        return spoolError(core::ErrorCode::Cancelled, "print cancelled");
    };

    const auto spoolAll = [&]() -> core::Result<PrintSpool> {
        auto directory = createSpoolDirectory(job->parentDirectory);
        if (!directory.has_value()) return std::unexpected(directory.error());
        DirectoryGuard guard(*directory);

        std::vector<SpooledPage> spooled;
        spooled.reserve(job->plan.pages.size());
        const std::size_t total = job->plan.pages.size();
        for (std::size_t pageNumber = 0; pageNumber < total; ++pageNumber) {
            const PrintPagePlan& page = job->plan.pages[pageNumber];
            const PrintPageSource& source = job->pages[page.pageIndex];
            SpooledPage out{page.pageIndex, page.displaySizePoints, page.pixelsPerPoint, {}};
            for (std::size_t bandIndex = 0; bandIndex < page.bands.size(); ++bandIndex) {
                if (job->cancelled->load()) return std::unexpected(cancelledError());
                const PrintBandPlan& band = page.bands[bandIndex];
                auto bitmap = source.render(band.rectPoints, page.pixelsPerPoint);
                if (!bitmap.has_value()) return std::unexpected(bitmap.error());

                std::filesystem::path file =
                    *directory / std::format("page{}-band{}.bgra", page.pageIndex, bandIndex);
                const core::Status written =
                    writeBand(file, *bitmap, page.pixelWidth, band.pixelHeight);
                if (!written.has_value()) return std::unexpected(written.error());
                out.bands.push_back(SpooledBand{std::move(file), page.pixelWidth, band.pixelHeight,
                                                std::size_t{page.pixelWidth} * kBytesPerPixel,
                                                band.rectPoints});
                // The bitmap dies here: one band in memory at a time.
            }
            spooled.push_back(std::move(out));
            Job::postProgress(job, pageNumber + 1, total);
        }
        if (job->cancelled->load()) return std::unexpected(cancelledError());
        return PrintSpool(guard.release(), std::move(spooled));
    };

    core::Result<PrintSpool> result = std::unexpected(core::Error{});
    try {
        result = spoolAll();
    } catch (const std::bad_alloc&) {
        result = std::unexpected(spoolError(core::ErrorCode::OutOfMemory,
                                            "out of memory while preparing the print job"));
    } catch (...) {
        core::log::error("PrintSpooler: render function threw");
        result = std::unexpected(
            spoolError(core::ErrorCode::Internal, "unexpected failure while preparing the print job"));
    }
    Job::postCompletion(job, std::move(result));
}

} // namespace rivet::editor
