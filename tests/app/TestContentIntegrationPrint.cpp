// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): printing a document with unsaved content
// edits goes through ONE assembled document; the printed pages show the
// edits; a document without edits is printed from its sources and never
// assembled. Bodies return early without a backend.
#include "ContentIntegrationKit.hpp"

#include "app/PrintCoordinator.hpp"
#include "platform/Print.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

// One printed page, reassembled from its spool bands (BGRA8888 straight).
struct PrintedPage {
    core::Size sizePoints;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::vector<std::uint8_t> bgra;

    Rgb at(double xPoints, double yPoints) const {
        if (pixelWidth == 0 || sizePoints.width <= 0.0) return {-1, -1, -1};
        const double scale = static_cast<double>(pixelWidth) / sizePoints.width;
        const auto x = static_cast<std::uint32_t>(xPoints * scale);
        const auto y = static_cast<std::uint32_t>(yPoints * scale);
        if (x >= pixelWidth || y >= pixelHeight) return {-1, -1, -1};
        const std::uint8_t* px = &bgra[(static_cast<std::size_t>(y) * pixelWidth + x) * 4];
        return Rgb{px[2], px[1], px[0]};
    }
};

// Reads the spool files while they exist (inside printSpool).
class CapturingPrintService final : public platform::IPrintService {
public:
    core::Result<platform::PrintSettings> choosePrintSettings(const platform::PrintSetup& setup) override {
        ++panels;
        platform::PrintSettings settings;
        settings.jobTitle = setup.jobTitle;
        settings.firstPage = 0;
        settings.lastPage = setup.pageCount - 1;
        return settings;
    }

    core::Status printSpool(const platform::PrintSettings&, const platform::PrintSpoolDescription& spool) override {
        pages.clear();
        for (const auto& page : spool.pages) {
            PrintedPage out;
            out.sizePoints = page.displaySizePoints;
            if (page.bands.empty()) continue;
            out.pixelWidth = page.bands.front().pixelWidth;
            std::uint32_t height = 0;
            for (const auto& band : page.bands) height += band.pixelHeight;
            out.pixelHeight = height;
            out.bgra.assign(static_cast<std::size_t>(out.pixelWidth) * height * 4, 0xFF);
            std::size_t row = 0;
            for (const auto& band : page.bands) {
                std::ifstream in(band.file, std::ios::binary);
                std::vector<char> raw(band.stride * band.pixelHeight);
                in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
                for (std::uint32_t y = 0; y < band.pixelHeight; ++y) {
                    std::copy_n(raw.begin() + static_cast<std::ptrdiff_t>(y * band.stride),
                                static_cast<std::size_t>(band.pixelWidth) * 4,
                                reinterpret_cast<char*>(&out.bgra[(row + y) * out.pixelWidth * 4]));
                }
                row += band.pixelHeight;
            }
            pages.push_back(std::move(out));
        }
        ++printed;
        return core::ok();
    }

    std::atomic<int> panels{0};
    std::atomic<int> printed{0};
    std::vector<PrintedPage> pages;
};

std::size_t countAssemblyDirectories() {
    std::error_code ignored;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(fs::temp_directory_path(), ignored)) {
        if (entry.path().filename().string().rfind("rivet-print-doc-", 0) == 0) ++count;
    }
    return count;
}

std::size_t printDocumentOpens(const Rig& rig) {
    std::size_t count = 0;
    for (const fs::path& path : rig.engine.openedPaths()) {
        if (path.string().find("rivet-print-doc-") != std::string::npos) ++count;
    }
    return count;
}

bool isRed(const Rgb& c) { return c.r > 200 && c.g < 60 && c.b < 60; }
bool isWhite(const Rgb& c) { return c.r > 235 && c.g > 235 && c.b > 235; }
bool isGreen(const Rgb& c) { return c.g > 120 && c.r < 60 && c.b < 60; }

struct PrintRun {
    CapturingPrintService service;
    std::vector<std::string> status;
};

bool printAndWait(Rig& rig, PrintRun& run, app::PrintCoordinator& coordinator, app::TabId id) {
    coordinator.print(id);
    return rig.dispatcher.waitUntil([&] {
        return run.service.printed.load() > 0 ||
               (!run.status.empty() && (run.status.back().rfind("Print failed", 0) == 0 ||
                                        run.status.back().rfind("Print cancelled", 0) == 0));
    });
}

} // namespace

// 8. Unsaved content edits are printed: one assembly, the temp copy reopened
// and rendered from, removed afterwards; the document itself is untouched.
RIVET_TEST(integPrintWithContentEditsRendersFromOneAssembledDocument) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich("print.pdf");
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
    const auto* path = rig.objectOfType(*view, PdfContentObjectType::Path, 0);
    CHECK(image != nullptr && path != nullptr);
    if (image == nullptr || path == nullptr) return;
    const ObjectId imageId = image->id;
    const ObjectId pathId = path->id;
    const Point imageCenter = centerOf(image->bounds);
    const Point pathCenter = centerOf(path->bounds);
    const auto original = std::filesystem::file_size(rig.dir("print.pdf"));

    // Delete the image, move the path by (-100, +50), retype the greeting.
    auto& session = rig.session();
    {
        auto remove = editor::deleteContent(session, rig.pageId(0), {imageId});
        CHECK(remove.has_value());
        if (!remove.has_value()) return;
        CHECK(session.execute(std::move(remove->command)).has_value());
    }
    CHECK(rig.loaded(0) != nullptr);
    {
        auto move = editor::moveContent(session, rig.pageId(0), {pathId}, Point{-100.0, 50.0});
        CHECK(move.has_value());
        if (!move.has_value()) return;
        CHECK(session.execute(std::move(move->command)).has_value());
    }
    CHECK(rig.loaded(0) != nullptr);
    CHECK(retype(rig, "Hello World", "Printed"));
    CHECK(rig.awaitBlock("Printed").has_value());
    CHECK(session.isDirty());

    const std::size_t directoriesBefore = countAssemblyDirectories();
    PrintRun run;
    app::PrintCoordinator coordinator{rig.workspace, rig.engine, rig.scheduler, &rig.dispatcher, &run.service,
                                      [&run](std::string text) { run.status.push_back(std::move(text)); }};
    CHECK(printAndWait(rig, run, coordinator, tab->id()));
    CHECK_EQ(run.service.printed.load(), 1);
    CHECK_EQ(run.service.pages.size(), std::size_t{2});

    // Exactly one assembly, and the pages came from the reopened copy.
    CHECK_EQ(rig.engine.assemblies.load(), 1);
    CHECK_EQ(printDocumentOpens(rig), std::size_t{1});
    CHECK_EQ(countAssemblyDirectories(), directoriesBefore); // the working copy is gone

    if (run.service.pages.size() == 2) {
        const PrintedPage& first = run.service.pages[0];
        const PrintedPage& second = run.service.pages[1];
        // The deleted image and the old path position are blank, the moved
        // path is red, and the untouched second page still has its rectangle.
        CHECK(isWhite(first.at(imageCenter.x, imageCenter.y - 15.0)));
        CHECK(isWhite(first.at(pathCenter.x, pathCenter.y)));
        CHECK(isRed(first.at(pathCenter.x - 100.0, pathCenter.y + 50.0)));
        CHECK(isGreen(second.at(100.0, 320.0)));
        CHECK(isWhite(second.at(250.0, 50.0)));
    }

    // Printing neither saved nor reset anything.
    CHECK(session.isDirty());
    CHECK_EQ(std::filesystem::file_size(rig.dir("print.pdf")), original);
    CHECK_EQ(rig.depth(), std::size_t{3});
    coordinator.cancel();
}

// 8b. Without edits nothing is assembled; the sources are rendered directly.
RIVET_TEST(integPrintWithoutContentEditsNeverAssembles) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich("print2.pdf");
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    const std::size_t directoriesBefore = countAssemblyDirectories();
    PrintRun run;
    app::PrintCoordinator coordinator{rig.workspace, rig.engine, rig.scheduler, &rig.dispatcher, &run.service,
                                      [&run](std::string text) { run.status.push_back(std::move(text)); }};
    CHECK(printAndWait(rig, run, coordinator, tab->id()));
    CHECK_EQ(run.service.printed.load(), 1);
    CHECK_EQ(run.service.pages.size(), std::size_t{2});
    CHECK_EQ(rig.engine.assemblies.load(), 0);
    CHECK_EQ(printDocumentOpens(rig), std::size_t{0});
    CHECK_EQ(countAssemblyDirectories(), directoriesBefore);
    if (run.service.pages.size() == 2) {
        CHECK(isRed(run.service.pages[0].at(210.0, 240.0)));
        CHECK(isGreen(run.service.pages[1].at(100.0, 320.0)));
    }
    coordinator.cancel();
}
