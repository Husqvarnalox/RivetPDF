// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "editor/PrintSpooler.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::PrintPageSource;
using rivet::editor::PrintSpool;
using rivet::editor::PrintSpooler;
using rivet::editor::PrintSpoolOptions;
using rivet::editor::PrintSpoolPlan;

namespace {

constexpr Size kLetter{612.0, 792.0};

// Queue dispatcher standing in for the main thread: tasks run only when the
// test pumps them, on the test thread.
class QueueDispatcher final : public rivet::core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
        posted_.notify_all();
    }

    // Runs every queued task; returns how many ran.
    std::size_t pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return run.size();
    }

    // Blocks until at least one task is queued, then runs the queue.
    void waitAndPump() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            posted_.wait(lock, [this] { return !queue_.empty(); });
        }
        pump();
    }

private:
    std::mutex mutex_;
    std::condition_variable posted_;
    std::deque<std::function<void()>> queue_;
};

// Deterministic pixel pattern: a function of page, page-raster row, column
// and channel, so band files can be checked byte for byte.
std::uint8_t patternByte(std::size_t page, std::uint64_t row, std::uint64_t x, std::uint64_t channel) {
    return static_cast<std::uint8_t>((page * 31 + row * 7 + x * 3 + channel * 11) & 0xFFu);
}

// Renders like the PDFium backend: ceil(rect * density) pixels, 64-byte
// aligned stride (wider than the tight spool stride).
Result<Bitmap> renderPattern(std::size_t page, const Rect& rect, double density) {
    const auto width = static_cast<std::uint32_t>(std::ceil(rect.size.width * density));
    const auto height = static_cast<std::uint32_t>(std::ceil(rect.size.height * density));
    auto bitmap = Bitmap::create(width, height);
    if (!bitmap.has_value()) return bitmap;
    const auto firstRow = static_cast<std::uint64_t>(std::llround(rect.minY() * density));
    for (std::uint32_t y = 0; y < height; ++y) {
        std::byte* row = bitmap->data() + std::size_t{y} * bitmap->stride();
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                row[std::size_t{x} * 4 + c] = static_cast<std::byte>(patternByte(page, firstRow + y, x, c));
            }
        }
    }
    return bitmap;
}

struct RenderCounts {
    std::array<std::atomic<int>, 16> perPage{};
    int at(std::size_t page) const { return perPage[page].load(); }
};

std::vector<PrintPageSource> makePages(std::size_t count, Size size, RenderCounts& counts,
                                       std::function<std::optional<Error>(std::size_t)> failWith = {}) {
    std::vector<PrintPageSource> pages;
    for (std::size_t page = 0; page < count; ++page) {
        pages.push_back(PrintPageSource{
            size, [page, &counts, failWith](const Rect& rect, double density) -> Result<Bitmap> {
                counts.perPage[page].fetch_add(1);
                if (failWith) {
                    if (auto error = failWith(page)) return std::unexpected(*error);
                }
                return renderPattern(page, rect, density);
            }});
    }
    return pages;
}

// A fresh, empty per-test spool parent directory.
std::filesystem::path spoolParent(const char* name) {
    auto path = std::filesystem::temp_directory_path() / (std::string{"rivet-spool-test-"} + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

bool isEmptyDirectory(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path) &&
           std::filesystem::directory_iterator(path) == std::filesystem::directory_iterator();
}

struct Outcome {
    std::optional<Result<PrintSpool>> result;
    std::vector<std::pair<std::size_t, std::size_t>> progress;
    std::thread::id callbackThread;
};

// Starts a job and pumps the dispatcher until its completion arrives.
Outcome runJob(PrintSpooler& spooler, QueueDispatcher& dispatcher, std::vector<PrintPageSource> pages,
               PrintSpoolOptions options) {
    Outcome outcome;
    const auto started = spooler.start(
        std::move(pages), std::move(options),
        [&outcome](std::size_t done, std::size_t total) { outcome.progress.emplace_back(done, total); },
        [&outcome](Result<PrintSpool> result) {
            outcome.callbackThread = std::this_thread::get_id();
            outcome.result.emplace(std::move(result));
        });
    CHECK(started.has_value());
    while (!outcome.result.has_value()) dispatcher.waitAndPump();
    return outcome;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

RIVET_TEST(printSpoolBandsRespectPixelBudgetAndTileThePage) {
    PrintSpoolOptions options;
    options.maxBandPixels = 100'000;
    RenderCounts counts;
    const auto plan = PrintSpooler::plan(makePages(1, kLetter, counts), options);
    CHECK(plan.has_value());
    CHECK_EQ(plan->pages.size(), std::size_t{1});
    const auto& page = plan->pages[0];
    CHECK_NEAR(page.pixelsPerPoint, 150.0 / 72.0, 1e-12);
    CHECK_EQ(page.pixelWidth, std::uint32_t{1275});
    CHECK_EQ(page.pixelHeight, std::uint32_t{1650});
    CHECK_GT(page.bands.size(), std::size_t{1});

    std::uint64_t rows = 0;
    for (std::size_t i = 0; i < page.bands.size(); ++i) {
        const auto& band = page.bands[i];
        CHECK_LE(std::uint64_t{page.pixelWidth} * band.pixelHeight, options.maxBandPixels);
        CHECK_EQ(std::uint64_t{band.firstRow}, rows);
        CHECK_EQ(band.rectPoints.minX(), 0.0);
        CHECK_EQ(band.rectPoints.size.width, kLetter.width);
        if (i == 0) CHECK_EQ(band.rectPoints.minY(), 0.0);
        // Contiguous in points: no gap, no overlap.
        if (i > 0) CHECK_EQ(band.rectPoints.minY(), page.bands[i - 1].rectPoints.maxY());
        rows += band.pixelHeight;
    }
    CHECK_EQ(rows, std::uint64_t{page.pixelHeight});
    CHECK_NEAR(page.bands.back().rectPoints.maxY(), kLetter.height, 1e-9);
    CHECK_LT(page.bands.back().pixelHeight, page.bands.front().pixelHeight); // last is shorter

    // The spooled result mirrors the plan and every band file has the
    // declared size.
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    options.spoolParentDirectory = spoolParent("bands");
    Outcome outcome = runJob(spooler, dispatcher, makePages(1, kLetter, counts), options);
    CHECK(outcome.result->has_value());
    const PrintSpool& spool = **outcome.result;
    CHECK_EQ(spool.pages().size(), std::size_t{1});
    CHECK_EQ(spool.pages()[0].bands.size(), page.bands.size());
    CHECK_EQ(counts.at(0), static_cast<int>(page.bands.size()));
    for (const auto& band : spool.pages()[0].bands) {
        CHECK_EQ(band.stride, std::size_t{band.pixelWidth} * 4);
        CHECK_EQ(std::filesystem::file_size(band.file), band.stride * band.pixelHeight);
    }
}

RIVET_TEST(printSpoolHugePageDensityIsBoundedByThePageBudget) {
    RenderCounts counts;
    const Size huge{14400.0, 14400.0};

    // Low floor: the per-page budget decides.
    PrintSpoolOptions options;
    options.minPixelsPerPoint = 0.1;
    auto plan = PrintSpooler::plan(makePages(1, huge, counts), options);
    CHECK(plan.has_value());
    const auto& bounded = plan->pages[0];
    CHECK_LT(bounded.pixelsPerPoint, options.targetPixelsPerPoint);
    CHECK_LE(std::uint64_t{bounded.pixelWidth} * bounded.pixelHeight, options.maxPagePixels);
    CHECK_GT(std::uint64_t{bounded.pixelWidth} * bounded.pixelHeight, options.maxPagePixels * 99 / 100);

    // Default floor (0.5 px/pt): the poster still prints at the floor, and
    // every band still honors the band budget.
    plan = PrintSpooler::plan(makePages(1, huge, counts), PrintSpoolOptions{});
    CHECK(plan.has_value());
    const auto& floored = plan->pages[0];
    CHECK_EQ(floored.pixelsPerPoint, 0.5);
    CHECK_EQ(floored.pixelWidth, std::uint32_t{7200});
    for (const auto& band : floored.bands) {
        CHECK_LE(std::uint64_t{floored.pixelWidth} * band.pixelHeight, PrintSpoolOptions{}.maxBandPixels);
    }
    CHECK_EQ(counts.at(0), 0); // planning never renders
}

RIVET_TEST(printSpoolVeryWidePageStaysWithinTheBandBudget) {
    RenderCounts counts;
    PrintSpoolOptions options;
    options.maxBandPixels = 5000; // < 14400 pt * 0.5 px/pt floor
    const auto plan = PrintSpooler::plan(makePages(1, Size{14400.0, 10.0}, counts), options);
    CHECK(plan.has_value());
    const auto& page = plan->pages[0];
    CHECK_LT(page.pixelsPerPoint, options.minPixelsPerPoint); // band bound beats the floor
    CHECK_LE(std::uint64_t{page.pixelWidth}, options.maxBandPixels);
    for (const auto& band : page.bands) {
        CHECK_EQ(band.pixelHeight, std::uint32_t{1});
        CHECK_LE(std::uint64_t{page.pixelWidth} * band.pixelHeight, options.maxBandPixels);
    }
}

RIVET_TEST(printSpoolGlobalBudgetScalesDensityOrFails) {
    RenderCounts counts;
    PrintSpoolOptions options;
    options.maxSpoolBytes = std::uint64_t{20} << 20; // 10 Letter pages need ~84 MB at 150 dpi
    auto plan = PrintSpooler::plan(makePages(10, kLetter, counts), options);
    CHECK(plan.has_value());
    CHECK_LE(plan->totalBytes, options.maxSpoolBytes);
    CHECK_GT(plan->totalBytes, options.maxSpoolBytes / 2); // scaled, not collapsed
    for (const auto& page : plan->pages) {
        CHECK_LT(page.pixelsPerPoint, options.targetPixelsPerPoint);
        CHECK_GE(page.pixelsPerPoint, options.minPixelsPerPoint);
        CHECK_EQ(page.pixelsPerPoint, plan->pages[0].pixelsPerPoint); // uniform
    }

    // Even the floor does not fit: a clear error, delivered as the job's
    // outcome, and nothing is rendered.
    options.maxSpoolBytes = std::uint64_t{1} << 20;
    plan = PrintSpooler::plan(makePages(10, kLetter, counts), options);
    CHECK(!plan.has_value());
    CHECK(plan.error().code == ErrorCode::OutOfMemory);

    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    options.spoolParentDirectory = spoolParent("budget");
    Outcome outcome = runJob(spooler, dispatcher, makePages(10, kLetter, counts), options);
    CHECK(!outcome.result->has_value());
    CHECK(outcome.result->error().code == ErrorCode::OutOfMemory);
    for (std::size_t page = 0; page < 10; ++page) CHECK_EQ(counts.at(page), 0);
    CHECK(isEmptyDirectory(options.spoolParentDirectory));
}

RIVET_TEST(printSpoolRendersOnlyTheSelectedPageRange) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    RenderCounts counts;
    PrintSpoolOptions options;
    options.firstPage = 1;
    options.lastPage = 3;
    options.spoolParentDirectory = spoolParent("range");
    Outcome outcome = runJob(spooler, dispatcher, makePages(5, kLetter, counts), options);
    CHECK(outcome.result->has_value());
    const auto& pages = (*outcome.result)->pages();
    CHECK_EQ(pages.size(), std::size_t{3});
    for (std::size_t i = 0; i < pages.size(); ++i) CHECK_EQ(pages[i].pageIndex, i + 1);
    CHECK_EQ(counts.at(0), 0);
    CHECK_EQ(counts.at(4), 0);
    for (std::size_t page = 1; page <= 3; ++page) {
        CHECK_EQ(counts.at(page), static_cast<int>(pages[page - 1].bands.size()));
    }
    CHECK_EQ(outcome.progress.size(), std::size_t{3});
    CHECK(outcome.progress.back() == std::make_pair(std::size_t{3}, std::size_t{3}));

    // A range past the end is clamped; one starting past it is an error.
    options.firstPage = 4;
    options.lastPage = 99;
    outcome = runJob(spooler, dispatcher, makePages(5, kLetter, counts), options);
    CHECK(outcome.result->has_value());
    CHECK_EQ((*outcome.result)->pages().size(), std::size_t{1});
    options.firstPage = 5;
    outcome = runJob(spooler, dispatcher, makePages(5, kLetter, counts), options);
    CHECK(!outcome.result->has_value());
    CHECK(outcome.result->error().code == ErrorCode::InvalidArgument);
}

RIVET_TEST(printSpoolRenderFailureAbortsWithThatErrorAndCleansUp) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    RenderCounts counts;
    PrintSpoolOptions options;
    options.spoolParentDirectory = spoolParent("failure");
    const Error failure{ErrorCode::InvalidDocument, "page 3 is broken", "pdf"};
    auto pages = makePages(4, kLetter, counts, [failure](std::size_t page) -> std::optional<Error> {
        if (page == 2) return failure;
        return std::nullopt;
    });
    Outcome outcome = runJob(spooler, dispatcher, std::move(pages), options);
    CHECK(!outcome.result->has_value());
    CHECK(outcome.result->error() == failure);
    CHECK_EQ(counts.at(0), 1);
    CHECK_EQ(counts.at(1), 1);
    CHECK_EQ(counts.at(2), 1);
    CHECK_EQ(counts.at(3), 0); // aborted at the first failure
    CHECK(isEmptyDirectory(options.spoolParentDirectory));
    CHECK(!spooler.isActive());
}

RIVET_TEST(printSpoolCancellationMidSpoolReportsCancelledAndCleansUp) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    RenderCounts counts;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    auto pages = makePages(3, kLetter, counts);
    // Page 1 blocks until the test has requested cancellation.
    pages[1].render = [&entered, released, &counts](const Rect& rect, double density) {
        counts.perPage[1].fetch_add(1);
        entered.set_value();
        released.wait();
        return renderPattern(1, rect, density);
    };
    PrintSpoolOptions options;
    options.spoolParentDirectory = spoolParent("cancel");

    std::optional<Result<PrintSpool>> result;
    CHECK(spooler.start(std::move(pages), options, {}, [&result](Result<PrintSpool> r) {
        result.emplace(std::move(r));
    }).has_value());
    entered.get_future().wait();
    spooler.cancel();
    release.set_value();
    while (!result.has_value()) dispatcher.waitAndPump();

    CHECK(!result->has_value());
    CHECK(result->error().code == ErrorCode::Cancelled);
    CHECK_EQ(counts.at(0), 1);
    CHECK_EQ(counts.at(1), 1);
    CHECK_EQ(counts.at(2), 0);
    CHECK(isEmptyDirectory(options.spoolParentDirectory));
    CHECK(!spooler.isActive());
}

RIVET_TEST(printSpoolDestructionDuringInFlightRenderIsSafe) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    auto spooler = std::make_unique<PrintSpooler>(scheduler, &dispatcher);
    RenderCounts counts;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    auto pages = makePages(2, kLetter, counts);
    pages[0].render = [&entered, released, &counts](const Rect& rect, double density) {
        counts.perPage[0].fetch_add(1);
        entered.set_value();
        released.wait();
        return renderPattern(0, rect, density);
    };
    PrintSpoolOptions options;
    options.spoolParentDirectory = spoolParent("destroy");

    std::atomic<int> callbacks{0};
    CHECK(spooler->start(std::move(pages), options,
                         [&callbacks](std::size_t, std::size_t) { callbacks.fetch_add(1); },
                         [&callbacks](Result<PrintSpool>) { callbacks.fetch_add(1); })
              .has_value());
    entered.get_future().wait();
    const auto token = spooler->cancellationToken();
    CHECK(token != nullptr);

    // Destroy on another thread while the render is blocked; release the
    // render only once the destructor has requested cancellation.
    std::thread destroyer([&spooler] { spooler.reset(); });
    token->wait(false);
    release.set_value();
    destroyer.join();

    // Deliveries queued by the worker are no-ops now.
    dispatcher.pump();
    CHECK_EQ(callbacks.load(), 0);
    CHECK_EQ(counts.at(1), 0); // cancelled before the next band
    CHECK(isEmptyDirectory(options.spoolParentDirectory));
}

RIVET_TEST(printSpoolBandFilesContainExactlyTheRenderedBytes) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    RenderCounts counts;
    PrintSpoolOptions options;
    options.targetPixelsPerPoint = 0.25; // small, odd-sized raster
    options.minPixelsPerPoint = 0.25;
    options.maxBandPixels = 2000;
    options.spoolParentDirectory = spoolParent("bytes");
    Outcome outcome = runJob(spooler, dispatcher, makePages(2, Size{301.0, 399.0}, counts), options);
    CHECK(outcome.result->has_value());
    for (const auto& page : (*outcome.result)->pages()) {
        CHECK_GT(page.bands.size(), std::size_t{1});
        std::uint64_t pageRow = 0;
        for (const auto& band : page.bands) {
            const auto bytes = readFile(band.file);
            CHECK_EQ(bytes.size(), band.stride * band.pixelHeight);
            for (std::uint32_t y = 0; y < band.pixelHeight; ++y) {
                for (std::uint32_t x = 0; x < band.pixelWidth; ++x) {
                    for (std::uint32_t c = 0; c < 4; ++c) {
                        const std::uint8_t actual = bytes[std::size_t{y} * band.stride + std::size_t{x} * 4 + c];
                        if (actual != patternByte(page.pageIndex, pageRow + y, x, c)) {
                            CHECK_EQ(int{actual}, int{patternByte(page.pageIndex, pageRow + y, x, c)});
                        }
                    }
                }
            }
            pageRow += band.pixelHeight;
        }
    }
}

RIVET_TEST(printSpoolCompletionArrivesOnlyThroughTheDispatcher) {
    TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    PrintSpooler spooler(scheduler, &dispatcher);
    RenderCounts counts;
    PrintSpoolOptions options;
    options.spoolParentDirectory = spoolParent("dispatch");

    std::optional<Result<PrintSpool>> result;
    std::thread::id callbackThread;
    std::size_t progressCalls = 0;
    CHECK(spooler.start(makePages(2, kLetter, counts), options,
                        [&progressCalls](std::size_t, std::size_t) { ++progressCalls; },
                        [&result, &callbackThread](Result<PrintSpool> r) {
                            callbackThread = std::this_thread::get_id();
                            result.emplace(std::move(r));
                        })
              .has_value());
    // A second job is refused while one is active.
    CHECK(!spooler.start(makePages(1, kLetter, counts), options, {}, [](Result<PrintSpool>) {}).has_value());

    spooler.waitForWorker();
    CHECK(!result.has_value()); // finished, but not delivered yet
    CHECK_EQ(progressCalls, std::size_t{0});
    CHECK(spooler.isActive());
    dispatcher.pump();
    CHECK(result.has_value());
    CHECK(result->has_value());
    CHECK(callbackThread == std::this_thread::get_id());
    CHECK_EQ(progressCalls, std::size_t{2});
    CHECK(!spooler.isActive());

    // The spool owns its directory: moving transfers it, destruction removes it.
    const std::filesystem::path directory = (*result)->directory();
    CHECK(std::filesystem::is_directory(directory));
#ifndef _WIN32
    const auto perms = std::filesystem::status(directory).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
#endif
    {
        PrintSpool moved = std::move(**result);
        CHECK(std::filesystem::is_directory(directory));
        CHECK(moved.directory() == directory);
        result.reset(); // the moved-from spool owns nothing
        CHECK(std::filesystem::is_directory(directory));
    }
    CHECK(!std::filesystem::exists(directory));
    CHECK(isEmptyDirectory(options.spoolParentDirectory));
}
