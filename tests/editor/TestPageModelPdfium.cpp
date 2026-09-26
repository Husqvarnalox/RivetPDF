// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef RIVET_EDITOR_PDF_FIXTURE_DIR
#define RIVET_EDITOR_PDF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// End-to-end page editing against PDFium: commands on a DocumentSession over
// markers-5.pdf (+ an import from import-3.pdf), PageModel -> assembly
// request -> PdfEngine::assembleDocument into memory -> reload -> verify
// marker order, rotation and crop. Bodies return early (after an OFF-build
// sanity CHECK) when no PDFium backend is built in.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::editor::DocumentSession;
using rivet::editor::PageModel;
using rivet::editor::PageModelSnapshot;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;

fs::path fixture(const char* name) {
    return fs::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / name;
}

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
}

class MemorySink final : public rivet::pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return core::ok();
    }
    const std::vector<char>& bytes() const { return bytes_; }

private:
    std::vector<char> bytes_;
};

std::unique_ptr<PdfDocument> reload(PdfEngine& engine, const MemorySink& sink) {
    static std::atomic<int> counter{0};
    const fs::path path = fs::temp_directory_path() /
                          ("rivet-page-model-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(counter.fetch_add(1)) + ".pdf");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes().data(), static_cast<std::streamsize>(sink.bytes().size()));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path);
    std::error_code ignored;
    fs::remove(path, ignored);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

std::string marker(const PdfDocument& document, std::size_t page) {
    const auto text = document.textPage(page);
    if (!text.has_value()) return "<no text>";
    const std::string& all = (*text)->text();
    for (const std::string_view prefix : {std::string_view("PAGE-"), std::string_view("IMPORT-")}) {
        const auto at = all.find(prefix);
        if (at != std::string::npos && at + prefix.size() < all.size()) return all.substr(at, prefix.size() + 1);
    }
    return all;
}

std::vector<std::string> markers(const PdfDocument& document) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < document.info().pageCount; ++i) result.push_back(marker(document, i));
    return result;
}

bool nearBox(const PdfBox& a, const PdfBox& b, double eps = 0.01) {
    return std::fabs(a.left - b.left) <= eps && std::fabs(a.bottom - b.bottom) <= eps &&
           std::fabs(a.right - b.right) <= eps && std::fabs(a.top - b.top) <= eps;
}

} // namespace

RIVET_TEST(pdfiumPageModelSaveRoundTrip) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    core::TaskScheduler scheduler(2);
    auto created = DocumentSession::create(*engine, scheduler, nullptr, fixture("markers-5.pdf"));
    CHECK(created.has_value());
    if (!created.has_value()) return;
    DocumentSession& session = **created;
    const auto id = [&](std::size_t i) { return session.pageId(i); };
    const auto p1 = id(0), p2 = id(1), p3 = id(2), p4 = id(3), p5 = id(4);

    // Import page 2 of import-3 through the engine (shared ownership).
    auto imported = engine->openDocument(fixture("import-3.pdf"));
    CHECK(imported.has_value());
    if (!imported.has_value()) return;
    std::shared_ptr<PdfDocument> other = std::move(*imported);
    const std::size_t importIndex[] = {1};
    auto sources = PageModel::describePages(other, importIndex);
    CHECK(sources.has_value());
    other.reset(); // the model keeps it alive from here on

    auto& model = session.pageModel();
    const auto run = [&](std::unique_ptr<rivet::editor::Command> command) {
        const auto status = session.execute(std::move(command));
        CHECK(status.has_value());
    };
    run(std::make_unique<rivet::editor::MovePagesCommand>(model, std::vector{p5}, 0));     // 5 1 2 3 4
    run(std::make_unique<rivet::editor::DeletePagesCommand>(model, std::vector{p2}));      // 5 1 3 4
    run(std::make_unique<rivet::editor::RotatePagesCommand>(model, std::vector{p3}, 90));  // 3 rotated
    const PdfBox crop{50.0, 80.0, 400.0, 600.0};
    run(std::make_unique<rivet::editor::CropPagesCommand>(model, std::vector{p4}, crop));  // 4 cropped
    run(std::make_unique<rivet::editor::DuplicatePagesCommand>(model, std::vector{p1}));   // 5 1 1' 3 4
    run(std::make_unique<rivet::editor::InsertPagesCommand>(model, *sources, 5));          // ... + IMPORT-2
    // A step that is undone must not show up in the output.
    run(std::make_unique<rivet::editor::RotatePagesCommand>(model, std::vector{p5}, 180));
    CHECK(session.undo());
    CHECK(session.isDirty());

    const auto snapshot = session.pageSnapshot(); // keeps every document alive
    auto request = snapshot->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    CHECK(request.has_value());
    if (!request.has_value()) return;
    MemorySink sink;
    const auto status = engine->assembleDocument(*request, sink);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    auto out = reload(*engine, sink);
    if (!out) return;
    CHECK((markers(*out) ==
           std::vector<std::string>{"PAGE-5", "PAGE-1", "PAGE-1", "PAGE-3", "PAGE-4", "IMPORT-2"}));
    for (std::size_t i = 0; i < snapshot->size() && i < out->info().pageCount; ++i) {
        const auto info = out->pageInfo(i);
        CHECK(info.has_value());
        if (!info) continue;
        CHECK(info->view.rotation == snapshot->at(i).view.rotation);
        CHECK(nearBox(info->view.cropBox, snapshot->at(i).view.cropBox));
    }
    CHECK(out->pageInfo(3)->view.rotation == core::PageRotation::Clockwise90);
    CHECK(nearBox(out->pageInfo(4)->view.cropBox, crop));
    CHECK(out->pageInfo(0)->view.rotation == core::PageRotation::None);
    session.markSaved();
    CHECK(!session.isDirty());

    // Extract: pages 3 and 5 in model order -> PAGE-5, PAGE-3.
    const std::vector subset{p3, p5};
    auto extract = snapshot->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Extract, subset);
    CHECK(extract.has_value());
    if (!extract.has_value()) return;
    MemorySink extractSink;
    CHECK(engine->assembleDocument(*extract, extractSink).has_value());
    auto extracted = reload(*engine, extractSink);
    if (!extracted) return;
    CHECK((markers(*extracted) == std::vector<std::string>{"PAGE-5", "PAGE-3"}));
    CHECK(extracted->pageInfo(1)->view.rotation == core::PageRotation::Clockwise90);

    // The live session renders/extracts through the model: the rotated page's
    // text comes back in the rotated view's display space (sanity: text found).
    const auto* rotated = snapshot->find(p3);
    CHECK(rotated != nullptr);
    if (rotated != nullptr) {
        const auto page = session.textService().textPageNow(*rotated);
        CHECK(page != nullptr);
        if (page) CHECK(page->text().find("PAGE-3") != std::string::npos);
    }
}
