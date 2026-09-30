// SPDX-License-Identifier: MPL-2.0
// Save pipeline (editor layer): DocumentWriteJob capture, runDocumentWrite
// (assembly -> buffered sink -> AtomicFileWriter -> reopen), editing lock,
// DocumentSession::rebaseOnto. Portable bodies use FakeWritableEngine; the
// pdfium* bodies run the real backend against fixtures (early return when
// PDFium is not built in).
#include "RivetTest.h"

#include "fakes/FakeWritableEngine.hpp"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef RIVET_EDITOR_PDF_FIXTURE_DIR
#define RIVET_EDITOR_PDF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace editor = rivet::editor;
using editor::DocumentSession;
using editor::DocumentWriteJob;
using editor::PageModel;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::test::FakeWritableEngine;

// Unique scratch directory, removed (after restoring permissions) on exit.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-saver-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::permissions(path_, fs::perms::owner_all, fs::perm_options::add, ignored);
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }
    fs::path operator/(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

std::string readFile(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    std::string data(ec ? 0 : static_cast<std::size_t>(size), '\0');
    std::ifstream in(path, std::ios::binary);
    if (!data.empty()) in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return data;
}

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::size_t entryCount(const fs::path& dir) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir)) ++count;
    return count;
}

std::unique_ptr<DocumentSession> openSession(PdfEngine& engine, core::TaskScheduler& scheduler,
                                             const fs::path& path, std::string_view password = {}) {
    auto created = DocumentSession::create(engine, scheduler, nullptr, path, password);
    CHECK(created.has_value());
    if (!created.has_value()) return nullptr;
    return std::move(*created);
}

bool run(DocumentSession& session, std::unique_ptr<editor::Command> command) {
    const auto status = session.execute(std::move(command));
    CHECK(status.has_value());
    return status.has_value();
}

} // namespace

// ---------------------------------------------------------------------------
// Portable (fake engine)
// ---------------------------------------------------------------------------

RIVET_TEST(saverSaveThenRebaseKeepsIdentityAndCleansState) {
    TempDir dir;
    FakeWritableEngine engine;
    core::TaskScheduler scheduler(2);
    const fs::path path = dir / "doc.pdf";
    writeFile(path, "not a fake file"); // opens as a 5-page "doc-" fixture
    auto session = openSession(engine, scheduler, path);
    if (!session) return;
    auto& model = session->pageModel();
    const auto p1 = session->pageId(0), p2 = session->pageId(1), p4 = session->pageId(3), p5 = session->pageId(4);

    // Import 2 pages of another document (the model keeps it alive).
    auto other = engine.openDocument(dir / "other.pdf", {});
    CHECK(other.has_value());
    std::shared_ptr<PdfDocument> imported = std::move(*other);
    std::weak_ptr<PdfDocument> importedWeak = imported;
    const std::size_t importPages[] = {0, 2};
    auto sources = PageModel::describePages(imported, importPages);
    CHECK(sources.has_value());
    imported.reset();

    run(*session, std::make_unique<editor::RotatePagesCommand>(model, std::vector{p2}, 90));
    run(*session, std::make_unique<editor::DeletePagesCommand>(model, std::vector{p4}));
    run(*session, std::make_unique<editor::MovePagesCommand>(model, std::vector{p5}, 0));
    run(*session, std::make_unique<editor::InsertPagesCommand>(model, *sources, 2));
    sources->clear(); // from here on only the model/commands reference it
    CHECK(session->isDirty());
    CHECK(!importedWeak.expired());

    const std::vector<core::PageId> order = session->pageOrder();
    std::vector<std::uint64_t> revisions;
    for (const auto& entry : session->pageSnapshot()->entries()) revisions.push_back(entry.contentRevision);

    auto job = editor::makeSaveJob(*session, path);
    CHECK(job.has_value());
    if (!job) return;
    CHECK(job->kind == DocumentWriteJob::Kind::Save);
    CHECK(job->request.mode == rivet::pdf::PdfAssemblyRequest::Mode::PreserveBase);
    const auto result = editor::runDocumentWrite(engine, *job);
    CHECK(result.written.has_value());
    CHECK(result.bytesWritten > 0);
    CHECK((rivet::test::fakeFileMarkers(path) ==
           std::vector<std::string>{"doc-5", "doc-1", "other-1", "other-3", "doc-2", "doc-3"}));
    CHECK(result.rebase.has_value());
    if (!result.rebase || !result.rebase->has_value()) return;
    CHECK_EQ(engine.reopens.load(), 1);

    const std::uint64_t revisionBefore = session->documentRevision();
    job->snapshot.reset(); // the job held the imported document too
    const auto rebased = session->rebaseOnto(**result.rebase);
    CHECK(rebased.has_value());
    session->markSaved();
    CHECK(!session->isDirty());
    CHECK(session->pageOrder() == order);
    CHECK(session->documentRevision() > revisionBefore);
    const auto snapshot = session->pageSnapshot();
    for (std::size_t i = 0; i < snapshot->size(); ++i) {
        const auto& entry = snapshot->at(i);
        CHECK(entry.source == session->documentPtr());
        CHECK_EQ(entry.sourcePageIndex, i);
        CHECK_EQ(entry.contentRevision, revisions[i]); // display-identical: tiles stay valid
    }
    CHECK(snapshot->isIdentityOrder());
    CHECK_EQ(session->info().pageCount, 6u);
    // Undo history cannot be re-targeted: cleared.
    CHECK(!session->commands().canUndo());
    CHECK(!session->undo());
    // Imported documents are no longer referenced.
    CHECK(importedWeak.expired());

    // Dirty tracking keeps working on the new base; undo back to saved = clean.
    run(*session, std::make_unique<editor::RotatePagesCommand>(model, std::vector{p1}, 180));
    CHECK(session->isDirty());
    CHECK(session->undo());
    CHECK(!session->isDirty());

    // A second save from the rebased model writes the same pages.
    auto second = editor::makeSaveJob(*session, path);
    CHECK(second.has_value());
    if (!second) return;
    const auto again = editor::runDocumentWrite(engine, *second);
    CHECK(again.written.has_value());
    CHECK((rivet::test::fakeFileMarkers(path) ==
           std::vector<std::string>{"doc-5", "doc-1", "other-1", "other-3", "doc-2", "doc-3"}));
}

RIVET_TEST(saverRebaseRejectsMismatchedPageCount) {
    TempDir dir;
    FakeWritableEngine engine;
    engine.pageCounts["three.pdf"] = 3;
    core::TaskScheduler scheduler(1);
    auto session = openSession(engine, scheduler, dir / "doc.pdf");
    if (!session) return;
    auto target = DocumentSession::prepareRebase(engine, dir / "three.pdf", session->document());
    CHECK(target.has_value());
    if (!target) return;
    const auto base = session->documentPtr();
    const auto status = session->rebaseOnto(std::move(*target));
    CHECK(!status.has_value());
    CHECK(status.error().code == core::ErrorCode::InvalidArgument);
    CHECK(session->documentPtr() == base);
    CHECK_EQ(session->pageCount(), 5u);
}

RIVET_TEST(saverFailureLeavesOriginalIntact) {
    TempDir dir;
    FakeWritableEngine engine;
    core::TaskScheduler scheduler(1);
    const fs::path path = dir / "doc.pdf";
    writeFile(path, "ORIGINAL");
    auto session = openSession(engine, scheduler, path);
    if (!session) return;
    run(*session, std::make_unique<editor::RotatePagesCommand>(session->pageModel(),
                                                               std::vector{session->pageId(0)}, 90));
    auto job = editor::makeSaveJob(*session, path);
    CHECK(job.has_value());
    if (!job) return;

    // Assembly failure.
    engine.failAssembly = true;
    auto failed = editor::runDocumentWrite(engine, *job);
    CHECK(!failed.written.has_value());
    CHECK(!failed.rebase.has_value());
    engine.failAssembly = false;
    CHECK_EQ(readFile(path), std::string("ORIGINAL"));

    // Disk full while writing (injected ENOSPC).
    editor::DocumentWriteControl diskFull;
    diskFull.faultInjector = [](core::io::AtomicWriteFault fault) {
        return fault == core::io::AtomicWriteFault::Write ? ENOSPC : 0;
    };
    failed = editor::runDocumentWrite(engine, *job, diskFull);
    CHECK(!failed.written.has_value());
    CHECK(failed.written.error().code == core::ErrorCode::DiskFull);
    CHECK_EQ(readFile(path), std::string("ORIGINAL"));

    // Rename refused (I/O error at the last step).
    editor::DocumentWriteControl renameFails;
    renameFails.faultInjector = [](core::io::AtomicWriteFault fault) {
        return fault == core::io::AtomicWriteFault::Rename ? EIO : 0;
    };
    failed = editor::runDocumentWrite(engine, *job, renameFails);
    CHECK(!failed.written.has_value());
    CHECK_EQ(readFile(path), std::string("ORIGINAL"));

    // Cancelled before the commit.
    editor::DocumentWriteControl cancelled;
    cancelled.cancelled = [] { return true; };
    failed = editor::runDocumentWrite(engine, *job, cancelled);
    CHECK(!failed.written.has_value());
    CHECK(failed.written.error().code == core::ErrorCode::Cancelled);
    CHECK_EQ(readFile(path), std::string("ORIGINAL"));

    // No temp files left behind; the session is still dirty (never marked).
    CHECK_EQ(entryCount(dir.path()), 1u);
    CHECK(session->isDirty());
}

RIVET_TEST(saverUnwritableDirectoryIsPermissionDenied) {
    if (::geteuid() == 0) return; // root ignores directory permissions
    TempDir dir;
    FakeWritableEngine engine;
    core::TaskScheduler scheduler(1);
    auto session = openSession(engine, scheduler, dir / "doc.pdf");
    if (!session) return;
    const fs::path locked = dir / "locked";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    auto job = editor::makeSaveJob(*session, locked / "out.pdf");
    CHECK(job.has_value());
    if (!job) return;
    const auto result = editor::runDocumentWrite(engine, *job);
    CHECK(!result.written.has_value());
    CHECK(result.written.error().code == core::ErrorCode::PermissionDenied);
    CHECK(!fs::exists(locked / "out.pdf"));
    // Invalid destination.
    CHECK(!editor::makeSaveJob(*session, fs::path{}).has_value());
    auto missingDir = editor::makeSaveJob(*session, dir / "missing" / "out.pdf");
    CHECK(missingDir.has_value());
    if (missingDir) CHECK(!editor::runDocumentWrite(engine, *missingDir).written.has_value());
}

RIVET_TEST(saverExtractWritesSelectedPagesInModelOrder) {
    TempDir dir;
    FakeWritableEngine engine;
    core::TaskScheduler scheduler(1);
    auto session = openSession(engine, scheduler, dir / "doc.pdf");
    if (!session) return;
    const std::vector<core::PageId> pages{session->pageId(3), session->pageId(1), session->pageId(3)};
    auto job = editor::makeExtractJob(*session, pages, dir / "extract.pdf");
    CHECK(job.has_value());
    if (!job) return;
    CHECK(!job->prepareRebase);
    const auto result = editor::runDocumentWrite(engine, *job);
    CHECK(result.written.has_value());
    CHECK(!result.rebase.has_value());
    CHECK((rivet::test::fakeFileMarkers(dir / "extract.pdf") == std::vector<std::string>{"doc-2", "doc-4"}));
    CHECK(!session->isDirty());
    CHECK(!editor::makeExtractJob(*session, {}, dir / "none.pdf").has_value());
}

RIVET_TEST(saverEditingLockBlocksCommandsAndHistory) {
    FakeWritableEngine engine;
    core::TaskScheduler scheduler(1);
    auto session = openSession(engine, scheduler, "doc.pdf");
    if (!session) return;
    auto& model = session->pageModel();
    run(*session, std::make_unique<editor::RotatePagesCommand>(model, std::vector{session->pageId(0)}, 90));
    session->setEditingLocked(true, "Saving…");
    CHECK(session->isEditingLocked());
    const auto refused =
        session->execute(std::make_unique<editor::DeletePagesCommand>(model, std::vector{session->pageId(1)}));
    CHECK(!refused.has_value());
    CHECK(refused.error().code == core::ErrorCode::Unsupported);
    CHECK_EQ(session->pageCount(), 5u);
    CHECK(!session->undo());
    CHECK(session->isDirty());
    session->setEditingLocked(false);
    CHECK(session->undo());
    CHECK(!session->isDirty());
    CHECK(session->redo());
}

// ---------------------------------------------------------------------------
// PDFium round trips
// ---------------------------------------------------------------------------

namespace {

fs::path fixture(const char* name) { return fs::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / name; }

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
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

RIVET_TEST(pdfiumSaveOverSourceRebaseAndReopen) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    TempDir dir;
    const fs::path path = dir / "markers.pdf";
    fs::copy_file(fixture("markers-5.pdf"), path);
    core::TaskScheduler scheduler(2);
    auto session = openSession(*engine, scheduler, path);
    if (!session) return;
    auto& model = session->pageModel();
    const auto p1 = session->pageId(0), p2 = session->pageId(1), p3 = session->pageId(2),
               p4 = session->pageId(3), p5 = session->pageId(4);

    auto importDoc = engine->openDocument(fixture("import-3.pdf"));
    CHECK(importDoc.has_value());
    if (!importDoc) return;
    std::shared_ptr<PdfDocument> imported = std::move(*importDoc);
    const std::size_t importPages[] = {1};
    auto sources = PageModel::describePages(imported, importPages);
    CHECK(sources.has_value());
    imported.reset();

    const PdfBox crop{50.0, 80.0, 400.0, 600.0};
    run(*session, std::make_unique<editor::MovePagesCommand>(model, std::vector{p4}, 0));  // 4 1 2 3 5
    run(*session, std::make_unique<editor::DeletePagesCommand>(model, std::vector{p5}));  // 4 1 2 3
    run(*session, std::make_unique<editor::RotatePagesCommand>(model, std::vector{p2}, 90));
    run(*session, std::make_unique<editor::CropPagesCommand>(model, std::vector{p3}, crop));
    run(*session, std::make_unique<editor::InsertPagesCommand>(model, *sources, 4));      // + IMPORT-2
    const std::vector<std::string> expected{"PAGE-4", "PAGE-1", "PAGE-2", "PAGE-3", "IMPORT-2"};
    const std::vector<core::PageId> order = session->pageOrder();

    // Save OVER the file the session is reading from.
    auto job = editor::makeSaveJob(*session, path);
    CHECK(job.has_value());
    if (!job) return;
    const auto result = editor::runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    if (!result.written) return;
    std::fprintf(stderr, "    save markers-5 (+import, 5 pages): %lld us, %llu bytes\n",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(result.elapsed).count()),
                 static_cast<unsigned long long>(result.bytesWritten));
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (!result.rebase || !result.rebase->has_value()) return;
    job->snapshot.reset();
    CHECK(session->rebaseOnto(**result.rebase).has_value());
    session->markSaved();
    CHECK(!session->isDirty());
    CHECK(session->pageOrder() == order);
    CHECK(markers(session->document()) == expected);

    // Independent reopen of the path: markers, rotation and crop persisted.
    auto reopened = engine->openDocument(path);
    CHECK(reopened.has_value());
    if (!reopened) return;
    CHECK(markers(**reopened) == expected);
    CHECK((*reopened)->pageInfo(2)->view.rotation == core::PageRotation::Clockwise90);
    CHECK(nearBox((*reopened)->pageInfo(3)->view.cropBox, crop));
    CHECK((*reopened)->pageInfo(0)->view.rotation == core::PageRotation::None);
    // The session's views equal the file's (display-identical rebase).
    for (std::size_t i = 0; i < session->pageCount(); ++i) {
        const auto info = (*reopened)->pageInfo(i);
        CHECK(info.has_value());
        if (!info) continue;
        CHECK(info->view.rotation == session->pageSnapshot()->at(i).view.rotation);
        CHECK(nearBox(info->view.cropBox, session->pageSnapshot()->at(i).view.cropBox));
    }

    // Keep editing on the rebased document and save again (the new base is
    // itself read through a retained descriptor while being replaced).
    run(*session, std::make_unique<editor::DeletePagesCommand>(model, std::vector{p1}));
    auto second = editor::makeSaveJob(*session, path);
    CHECK(second.has_value());
    if (!second) return;
    const auto again = editor::runDocumentWrite(*engine, *second);
    CHECK(again.written.has_value());
    auto reread = engine->openDocument(path);
    CHECK(reread.has_value());
    if (reread) CHECK((markers(**reread) == std::vector<std::string>{"PAGE-4", "PAGE-2", "PAGE-3", "IMPORT-2"}));
    (void)p3;
}

RIVET_TEST(pdfiumSaveAsLeavesSourceAndSaves100Pages) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    TempDir dir;
    core::TaskScheduler scheduler(2);
    auto session = openSession(*engine, scheduler, fixture("markers-5.pdf"));
    if (!session) return;
    const auto sourceBytes = readFile(fixture("markers-5.pdf"));
    // Synthetic ~100-page document: duplicate all pages repeatedly.
    while (session->pageCount() < 100) {
        run(*session, std::make_unique<editor::DuplicatePagesCommand>(session->pageModel(), session->pageOrder()));
    }
    auto job = editor::makeSaveJob(*session, dir / "big.pdf");
    CHECK(job.has_value());
    if (!job) return;
    const auto result = editor::runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    std::fprintf(stderr, "    save %zu pages: %lld us, %llu bytes\n", session->pageCount(),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(result.elapsed).count()),
                 static_cast<unsigned long long>(result.bytesWritten));
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (result.rebase && result.rebase->has_value()) {
        CHECK(session->rebaseOnto(**result.rebase, dir / "big.pdf").has_value());
        CHECK(session->path() == dir / "big.pdf");
        CHECK_EQ(session->document().info().pageCount, session->pageCount());
    }
    CHECK(readFile(fixture("markers-5.pdf")) == sourceBytes); // Save As never touches the source
}

RIVET_TEST(pdfiumEncryptedSaveKeepsPasswordAndRebases) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    TempDir dir;
    const fs::path path = dir / "password.pdf";
    fs::copy_file(fixture("password.pdf"), path);
    core::TaskScheduler scheduler(1);
    auto session = openSession(*engine, scheduler, path, "rivet");
    if (!session) return;
    run(*session, std::make_unique<editor::RotatePagesCommand>(session->pageModel(),
                                                               std::vector{session->pageId(0)}, 270));
    auto job = editor::makeSaveJob(*session, path);
    CHECK(job.has_value());
    if (!job) return;
    const auto result = editor::runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    // Reopened with the retained password (never passed through the editor).
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (result.rebase && result.rebase->has_value()) CHECK(session->rebaseOnto(**result.rebase).has_value());
    auto locked = engine->openDocument(path);
    CHECK(!locked.has_value());
    if (!locked) CHECK(locked.error().code == core::ErrorCode::PasswordRequired);
    auto unlocked = engine->openDocument(path, "rivet");
    CHECK(unlocked.has_value());
    if (unlocked) CHECK((*unlocked)->pageInfo(0)->view.rotation == core::PageRotation::Clockwise270);
}

RIVET_TEST(pdfiumExtractSelectedPages) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    TempDir dir;
    core::TaskScheduler scheduler(1);
    auto session = openSession(*engine, scheduler, fixture("markers-5.pdf"));
    if (!session) return;
    run(*session, std::make_unique<editor::RotatePagesCommand>(session->pageModel(),
                                                               std::vector{session->pageId(3)}, 90));
    const std::vector<core::PageId> pages{session->pageId(3), session->pageId(1)};
    auto job = editor::makeExtractJob(*session, pages, dir / "pages.pdf");
    CHECK(job.has_value());
    if (!job) return;
    const auto result = editor::runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    auto out = engine->openDocument(dir / "pages.pdf");
    CHECK(out.has_value());
    if (!out) return;
    CHECK((markers(**out) == std::vector<std::string>{"PAGE-2", "PAGE-4"}));
    CHECK((*out)->pageInfo(1)->view.rotation == core::PageRotation::Clockwise90);
    CHECK(session->isDirty()); // extract does not save the document
}
