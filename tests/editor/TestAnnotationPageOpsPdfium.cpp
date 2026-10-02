// SPDX-License-Identifier: MPL-2.0
// Page operations combined with annotations against the real PDFium backend
// (Phase 4 hardening): each test performs an operation through the page
// commands and the annotation factories on a DocumentSession over a copy of
// annots.pdf (or markers-5.pdf), saves with the real saver + rebase, reopens
// the file in a NEW session and verifies through the annotation service
// (and PdfDocument::annotations for opaque entries). Every body returns early
// when no PDF backend is built in.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/AnnotationGeometry.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <string>
#include <system_error>
#include <vector>

#ifndef RIVET_EDITOR_PDF_FIXTURE_DIR
#define RIVET_EDITOR_PDF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using Kind = pdf::PdfAnnotationKind;

constexpr double kEps = 0.01;

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-annot-ops-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    fs::path operator/(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

bool near(double a, double b) {
    return std::abs(a - b) <= kEps;
}

bool nearRect(const core::Rect& a, const core::Rect& b) {
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) && near(a.size.width, b.size.width) &&
           near(a.size.height, b.size.height);
}

bool nearBox(const pdf::PdfBox& a, const pdf::PdfBox& b) {
    return near(a.left, b.left) && near(a.bottom, b.bottom) && near(a.right, b.right) && near(a.top, b.top);
}

fs::path fixture(const char* name) {
    return fs::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / name;
}

// One open session with a queueing dispatcher and per-page originals loading.
struct Opened {
    Opened(pdf::PdfEngine& engine, const fs::path& path) {
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        if (!created.has_value()) return;
        session = std::move(*created);
        session->setOnAnnotationsChanged([this](core::PageId page) { loaded.insert(keyOf(page)); });
    }

    // Originals are cached per (source document, source page): a duplicate of
    // a loaded page resolves at once, without a notification of its own.
    using SourceKey = std::pair<const pdf::PdfDocument*, std::size_t>;
    SourceKey keyOf(core::PageId page) const {
        const PageEntry* entry = session->pageSnapshot()->find(page);
        return entry ? SourceKey{entry->source.get(), entry->sourcePageIndex} : SourceKey{nullptr, 0};
    }

    // Requests the originals of `page`, waits for them and returns the
    // resolved list (editable originals first, then overlay items).
    AnnotationService::Resolved load(core::PageId page) {
        if (loaded.count(keyOf(page)) == 0) {
            session->annotations().annotations(page);
            CHECK(settle(dispatcher, [&] { return loaded.count(keyOf(page)) != 0; }));
        }
        return session->annotations().annotations(page);
    }
    AnnotationService::Resolved load(std::size_t index) { return load(session->pageId(index)); }

    std::vector<AnnotationView> views(std::size_t index) {
        const auto list = load(index);
        CHECK(list != nullptr);
        return list ? *list : std::vector<AnnotationView>{};
    }

    const PageEntry& entry(std::size_t index) const { return *session->pageSnapshot()->find(session->pageId(index)); }

    // The user-space annotations of the BASE document page `index` (opaque
    // ones included). Only meaningful while the model is the identity over
    // the base (a freshly opened file).
    pdf::PdfPageAnnotationsPtr raw(std::size_t index) {
        const auto read = session->document().annotations(index);
        CHECK(read.has_value());
        return read.has_value() ? *read : nullptr;
    }

    bool run(core::Result<AnnotationEdit> edit) {
        CHECK(edit.has_value());
        if (!edit.has_value()) return false;
        const auto status = session->execute(std::move(edit->command));
        CHECK(status.has_value());
        return status.has_value();
    }

    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<DocumentSession> session;
    std::set<SourceKey> loaded;
};

bool saveAndRebase(DocumentSession& session, pdf::PdfEngine& engine, const fs::path& path) {
    auto job = makeSaveJob(session, path);
    CHECK(job.has_value());
    if (!job) return false;
    auto result = runDocumentWrite(engine, *job);
    CHECK(result.written.has_value());
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (!result.written || !result.rebase || !result.rebase->has_value()) return false;
    job->snapshot.reset();
    const auto status = session.rebaseOnto(std::move(**result.rebase));
    CHECK(status.has_value());
    if (!status.has_value()) return false;
    session.markSaved();
    return true;
}

struct Setup {
    Setup(const char* name) {
        engine = pdf::createEngine();
        if (!engine || !engine->isAvailable()) {
            engine.reset();
            return;
        }
        path = dir / "work.pdf";
        fs::copy_file(fixture(name), path);
    }
    explicit operator bool() const { return engine != nullptr; }

    std::unique_ptr<pdf::PdfEngine> engine;
    TempDir dir;
    fs::path path;
};

const AnnotationView* findKind(const std::vector<AnnotationView>& views, Kind kind, std::size_t nth = 0) {
    for (const AnnotationView& view : views) {
        if (view.kind == kind && nth-- == 0) return &view;
    }
    return nullptr;
}

std::vector<Kind> kindsOf(const std::vector<AnnotationView>& views) {
    std::vector<Kind> kinds;
    for (const AnnotationView& view : views) kinds.push_back(view.kind);
    return kinds;
}

AnnotationDraft squareDraft(core::PageId page, core::Rect rect) {
    AnnotationDraft d;
    d.page = page;
    d.kind = Kind::Square;
    d.rect = rect;
    d.style.color = pdf::PdfColor{0.2F, 0.4F, 0.8F};
    d.style.opacity = 1.0F;
    d.style.borderWidth = 2.0F;
    return d;
}

// annots.pdf page 1 has these editable originals, in /Annots order.
const std::vector<Kind> kPage1Kinds{Kind::Highlight, Kind::Underline, Kind::StrikeOut, Kind::Note,
                                    Kind::Ink,       Kind::Square,    Kind::Circle,    Kind::Stamp};

// The opaque entries of annots.pdf page 1 (by /Annots index) after any edit:
// third-party Line (8), FreeText (10), Squiggly (11), the Note's Popup (4).
void expectOpaqueEntriesIntact(const pdf::PdfPageAnnotations& page, std::uint32_t lineIndex = 8,
                               std::uint32_t popupIndex = 4, std::uint32_t freeTextIndex = 10,
                               std::uint32_t squigglyIndex = 11) {
    for (const std::uint32_t i : {lineIndex, freeTextIndex, squigglyIndex}) {
        CHECK(i < page.items.size());
        if (i >= page.items.size()) continue;
        CHECK(!page.items[i].editable);
        CHECK(page.items[i].kind == Kind::Other);
        CHECK(!page.items[i].isPopup);
    }
    CHECK(popupIndex < page.items.size());
    if (popupIndex < page.items.size()) CHECK(page.items[popupIndex].isPopup);
}

} // namespace

// ---------------------------------------------------------------------------
// Reorder
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsReorderCarriesAnnotations) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const std::vector<AnnotationView> before = first.views(0);
    CHECK(kindsOf(before) == kPage1Kinds);

    CHECK(first.session
              ->execute(std::make_unique<MovePagesCommand>(first.session->pageModel(),
                                                           std::vector<core::PageId>{first.session->pageId(0)}, 2))
              .has_value());
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    CHECK_EQ(second.session->pageCount(), std::size_t{3});
    CHECK_EQ(second.raw(0)->annotsCount, std::uint32_t{1}); // old page 2
    CHECK_EQ(second.raw(1)->annotsCount, std::uint32_t{1}); // old page 3
    const auto moved = second.raw(2);
    if (!moved) return;
    CHECK_EQ(moved->annotsCount, std::uint32_t{12});
    expectOpaqueEntriesIntact(*moved);

    const std::vector<AnnotationView> after = second.views(2);
    CHECK(kindsOf(after) == kPage1Kinds);
    if (after.size() != before.size()) return;
    for (std::size_t i = 0; i < after.size(); ++i) CHECK(nearRect(after[i].bounds, before[i].bounds));
    // The Note keeps its text and the single-square pages their kind.
    const AnnotationView* note = findKind(after, Kind::Note);
    if (note != nullptr) CHECK_EQ(note->contents, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80");
    CHECK_EQ(second.views(0).size(), std::size_t{1});
    CHECK_EQ(second.views(1).size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Rotate + create in display space
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsRotateThenCreateMapsDisplayToUserSpace) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const core::PageId page = first.session->pageId(0);
    first.load(page);
    CHECK(first.session
              ->execute(std::make_unique<RotatePagesCommand>(first.session->pageModel(),
                                                             std::vector<core::PageId>{page}, 90))
              .has_value());
    const pdf::PdfPageView view = first.entry(0).view;
    CHECK(view.rotation == core::PageRotation::Clockwise90);

    const core::Rect display{120, 80, 140, 60};
    CHECK(first.run(createAnnotations(*first.session, {squareDraft(page, display)})));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const PageEntry& entry = second.entry(0);
    CHECK(entry.view.rotation == core::PageRotation::Clockwise90);

    // User space: exactly what the display rect maps to under the rotated view.
    const pdf::PdfBox wantUser = geometry::rectToUserBox(view, display);
    const auto raw = second.raw(0);
    if (!raw) return;
    CHECK_EQ(raw->annotsCount, std::uint32_t{13});
    const pdf::PdfPageAnnotation* created = nullptr;
    for (const pdf::PdfPageAnnotation& item : raw->items) {
        if (item.editable && item.kind == Kind::Square && nearBox(item.data.rect, wantUser)) created = &item;
    }
    CHECK(created != nullptr);

    // And its display view on the reopened rotated page equals the display
    // rect it was created with.
    const std::vector<AnnotationView> views = second.views(0);
    bool found = false;
    for (const AnnotationView& v : views) {
        if (v.kind == Kind::Square && nearRect(v.bounds, display)) found = true;
    }
    CHECK(found);
    CHECK_EQ(views.size(), kPage1Kinds.size() + 1);
}

// ---------------------------------------------------------------------------
// Crop
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsCropKeepsAnnotationsOutsideTheCropBox) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const core::PageId page = first.session->pageId(0);
    const std::vector<AnnotationView> before = first.views(0);
    const auto crop = [&](std::optional<pdf::PdfBox> box) {
        return first.session->execute(std::make_unique<CropPagesCommand>(
            first.session->pageModel(), std::vector<core::PageId>{page}, box));
    };

    // Every annotation lies outside this crop box.
    const pdf::PdfBox small{10, 10, 90, 90};
    CHECK(crop(small).has_value());
    const std::vector<AnnotationView> cropped = first.views(0);
    CHECK_EQ(cropped.size(), before.size());
    // Reset to the native box: display bounds are back.
    CHECK(crop(std::nullopt).has_value());
    const std::vector<AnnotationView> reset = first.views(0);
    CHECK_EQ(reset.size(), before.size());
    for (std::size_t i = 0; i < std::min(reset.size(), before.size()); ++i) {
        CHECK(nearRect(reset[i].bounds, before[i].bounds));
    }

    CHECK(crop(small).has_value());
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const auto raw = second.raw(0);
    if (!raw) return;
    CHECK_EQ(raw->annotsCount, std::uint32_t{12});
    expectOpaqueEntriesIntact(*raw);
    CHECK(nearBox(second.entry(0).view.cropBox, small));
    CHECK(kindsOf(second.views(0)) == kPage1Kinds);

    // Widening the crop back to the whole media box shows them where they were.
    CHECK(second.session
              ->execute(std::make_unique<CropPagesCommand>(second.session->pageModel(),
                                                           std::vector<core::PageId>{second.session->pageId(0)},
                                                           pdf::PdfBox{0, 0, 612, 792}))
              .has_value());
    const std::vector<AnnotationView> widened = second.views(0);
    CHECK_EQ(widened.size(), before.size());
    for (std::size_t i = 0; i < std::min(widened.size(), before.size()); ++i) {
        CHECK(nearRect(widened[i].bounds, before[i].bounds));
    }
}

// ---------------------------------------------------------------------------
// Duplicate
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsDuplicateThenEditTheCopyOnly) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const core::PageId original = first.session->pageId(0);
    const std::vector<AnnotationView> before = first.views(0);

    auto duplicate = std::make_unique<DuplicatePagesCommand>(first.session->pageModel(),
                                                             std::vector<core::PageId>{original});
    const DuplicatePagesCommand* handle = duplicate.get();
    CHECK(first.session->execute(std::move(duplicate)).has_value());
    CHECK_EQ(handle->createdIds().size(), std::size_t{1});
    if (handle->createdIds().size() != 1) return;
    const core::PageId copy = handle->createdIds()[0];
    CHECK(first.session->pageIndexFor(copy) == 1);

    const std::vector<AnnotationView> copyViews = first.views(1);
    CHECK(kindsOf(copyViews) == kPage1Kinds);
    const AnnotationView* origSquare = findKind(before, Kind::Square);
    const AnnotationView* copySquare = findKind(copyViews, Kind::Square);
    CHECK(origSquare != nullptr && copySquare != nullptr);
    if (origSquare == nullptr || copySquare == nullptr) return;
    CHECK(origSquare->id != copySquare->id);
    for (std::size_t i = 0; i < std::min(before.size(), copyViews.size()); ++i) {
        CHECK(before[i].id != copyViews[i].id);
    }

    const core::Point delta{25, 40};
    CHECK(first.run(moveAnnotation(*first.session, copySquare->id, delta)));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    CHECK_EQ(second.session->pageCount(), std::size_t{4});
    const std::vector<AnnotationView> a = second.views(0);
    const std::vector<AnnotationView> b = second.views(1);
    CHECK(kindsOf(a) == kPage1Kinds);
    {
        // The edited Square was re-created at the end of the copy's /Annots.
        std::vector<Kind> wantB = kPage1Kinds;
        wantB.erase(std::find(wantB.begin(), wantB.end(), Kind::Square));
        wantB.push_back(Kind::Square);
        CHECK(kindsOf(b) == wantB);
    }
    CHECK_EQ(second.raw(0)->annotsCount, std::uint32_t{12});
    CHECK_EQ(second.raw(1)->annotsCount, std::uint32_t{12});
    expectOpaqueEntriesIntact(*second.raw(0));
    expectOpaqueEntriesIntact(*second.raw(1), 7, 4, 9, 10); // the copy's Square (6) moved to the end
    if (a.size() != before.size() || b.size() != before.size()) return;
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(nearRect(a[i].bounds, before[i].bounds));
    for (const AnnotationView& v : b) {
        if (v.kind == Kind::Square) {
            core::Rect moved = origSquare->bounds;
            moved.origin.x += delta.x;
            moved.origin.y += delta.y;
            CHECK(nearRect(v.bounds, moved));
        } else {
            const AnnotationView* same = findKind(before, v.kind);
            CHECK(same != nullptr);
            if (same != nullptr) CHECK(nearRect(v.bounds, same->bounds));
        }
    }
    CHECK(a[0].id != b[0].id);
}

// ---------------------------------------------------------------------------
// Delete page + undo
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsDeletePageAndUndoRestoresAnnotationState) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const core::PageId page = first.session->pageId(0);
    const AnnotationService::Resolved list = first.load(page);
    if (!list) return;
    const std::vector<AnnotationView> before = *list;
    const AnnotationView* square = findKind(before, Kind::Square);
    CHECK(square != nullptr);
    if (square == nullptr) return;
    // Give the page an edit so there is overlay state to restore.
    CHECK(first.run(moveAnnotation(*first.session, square->id, core::Point{10, 10})));
    const std::vector<AnnotationView> edited = first.views(0);

    CHECK(first.session
              ->execute(std::make_unique<DeletePagesCommand>(first.session->pageModel(),
                                                             std::vector<core::PageId>{page}))
              .has_value());
    CHECK_EQ(first.session->pageCount(), std::size_t{2});
    CHECK(first.session->undo());
    CHECK_EQ(first.session->pageCount(), std::size_t{3});
    CHECK(first.session->pageId(0) == page);

    const std::vector<AnnotationView> restored = first.views(0);
    CHECK_EQ(restored.size(), edited.size());
    for (std::size_t i = 0; i < std::min(restored.size(), edited.size()); ++i) {
        CHECK(restored[i].id == edited[i].id);
        CHECK(restored[i].kind == edited[i].kind);
        CHECK(nearRect(restored[i].bounds, edited[i].bounds));
    }

    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));
    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    CHECK_EQ(second.session->pageCount(), std::size_t{3});
    CHECK_EQ(second.raw(0)->annotsCount, std::uint32_t{12});
    const std::vector<AnnotationView> after = second.views(0);
    CHECK(kindsOf(after) == kindsOf(edited));
    const AnnotationView* movedSquare = findKind(after, Kind::Square);
    const AnnotationView* editedSquare = findKind(edited, Kind::Square);
    if (movedSquare != nullptr && editedSquare != nullptr) CHECK(nearRect(movedSquare->bounds, editedSquare->bounds));
}

RIVET_TEST(annotationPageOpsDeletedPageDropsItsAnnotationsFromTheFile) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    CHECK(first.session
              ->execute(std::make_unique<DeletePagesCommand>(first.session->pageModel(),
                                                             std::vector<core::PageId>{first.session->pageId(0)}))
              .has_value());
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));
    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    CHECK_EQ(second.session->pageCount(), std::size_t{2});
    CHECK_EQ(second.raw(0)->annotsCount, std::uint32_t{1});
    CHECK_EQ(second.raw(1)->annotsCount, std::uint32_t{1});
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsImportedPagesKeepAllAnnotations) {
    Setup setup("markers-5.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;

    auto opened = setup.engine->openDocument(fixture("annots.pdf"), {});
    CHECK(opened.has_value());
    if (!opened.has_value()) return;
    std::shared_ptr<pdf::PdfDocument> other = std::move(*opened);
    auto pages = PageModel::describeAllPages(other);
    CHECK(pages.has_value());
    if (!pages.has_value()) return;
    CHECK(first.session
              ->execute(std::make_unique<InsertPagesCommand>(first.session->pageModel(), *pages, 5))
              .has_value());
    CHECK_EQ(first.session->pageCount(), std::size_t{8});
    // The imported page presents its originals through the service too.
    const std::vector<AnnotationView> importedBefore = first.views(5);
    CHECK(kindsOf(importedBefore) == kPage1Kinds);

    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));
    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    CHECK_EQ(second.session->pageCount(), std::size_t{8});
    for (std::size_t i = 0; i < 5; ++i) CHECK_EQ(second.raw(i)->annotsCount, std::uint32_t{0});

    const auto sourcePage = other->annotations(0);
    CHECK(sourcePage.has_value());
    const auto importedPage = second.raw(5);
    if (!sourcePage.has_value() || !importedPage) return;
    CHECK_EQ(importedPage->annotsCount, (*sourcePage)->annotsCount);
    CHECK_EQ(importedPage->items.size(), (*sourcePage)->items.size());
    for (std::size_t i = 0; i < std::min(importedPage->items.size(), (*sourcePage)->items.size()); ++i) {
        const pdf::PdfPageAnnotation& got = importedPage->items[i];
        const pdf::PdfPageAnnotation& want = (*sourcePage)->items[i];
        CHECK(got.kind == want.kind);
        CHECK_EQ(got.editable, want.editable);
        CHECK_EQ(got.isPopup, want.isPopup);
        CHECK(nearBox(got.data.rect, want.data.rect));
    }
    expectOpaqueEntriesIntact(*importedPage);
    CHECK_EQ(second.raw(6)->annotsCount, std::uint32_t{1});
    CHECK_EQ(second.raw(7)->annotsCount, std::uint32_t{1});
    CHECK(kindsOf(second.views(5)) == kPage1Kinds);
}

// ---------------------------------------------------------------------------
// Delete / edit existing originals
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsDeleteOriginalRemovesItsPopupToo) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const auto originalRaw = first.raw(0);
    if (!originalRaw || originalRaw->items.size() != 12) return;
    const std::vector<AnnotationView> before = first.views(0);
    const AnnotationView* note = findKind(before, Kind::Note);
    CHECK(note != nullptr);
    if (note == nullptr) return;
    CHECK(first.run(deleteAnnotations(*first.session, {note->id})));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const auto raw = second.raw(0);
    if (!raw) return;
    CHECK_EQ(raw->annotsCount, std::uint32_t{10}); // note (3) and popup (4) are gone
    CHECK_EQ(raw->items.size(), std::size_t{10});
    std::size_t next = 0;
    for (std::size_t i = 0; i < originalRaw->items.size(); ++i) {
        if (i == 3 || i == 4) continue;
        if (next >= raw->items.size()) break;
        const pdf::PdfPageAnnotation& got = raw->items[next++];
        const pdf::PdfPageAnnotation& want = originalRaw->items[i];
        CHECK(got.kind == want.kind);
        CHECK_EQ(got.editable, want.editable);
        CHECK(!got.isPopup);
        CHECK(nearBox(got.data.rect, want.data.rect));
    }
    std::vector<Kind> wantKinds = kPage1Kinds;
    wantKinds.erase(wantKinds.begin() + 3);
    CHECK(kindsOf(second.views(0)) == wantKinds);
}

RIVET_TEST(annotationPageOpsMoveOriginalsKeepsContentsAuthorAndName) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const std::vector<AnnotationView> before = first.views(0);
    const AnnotationView* square = findKind(before, Kind::Square);
    const AnnotationView* note = findKind(before, Kind::Note);
    CHECK(square != nullptr && note != nullptr);
    if (square == nullptr || note == nullptr) return;
    const core::Point squareDelta{30, -12};
    const core::Point noteDelta{-20, 45};
    CHECK(first.run(moveAnnotation(*first.session, square->id, squareDelta)));
    CHECK(first.run(moveAnnotation(*first.session, note->id, noteDelta)));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const std::vector<AnnotationView> after = second.views(0);
    CHECK_EQ(after.size(), before.size());
    const AnnotationView* movedSquare = findKind(after, Kind::Square);
    const AnnotationView* movedNote = findKind(after, Kind::Note);
    CHECK(movedSquare != nullptr && movedNote != nullptr);
    if (movedSquare == nullptr || movedNote == nullptr) return;
    core::Rect wantSquare = square->bounds;
    wantSquare.origin.x += squareDelta.x;
    wantSquare.origin.y += squareDelta.y;
    CHECK(nearRect(movedSquare->bounds, wantSquare));
    core::Rect wantNote = note->bounds;
    wantNote.origin.x += noteDelta.x;
    wantNote.origin.y += noteDelta.y;
    CHECK(nearRect(movedNote->bounds, wantNote));
    CHECK_EQ(movedNote->contents, note->contents);
    CHECK_EQ(movedNote->author, "Tester");

    // /NM and /Contents survive in the stored data.
    const auto raw = second.raw(0);
    if (!raw) return;
    bool sawNote = false;
    for (const pdf::PdfPageAnnotation& item : raw->items) {
        if (item.editable && item.kind == Kind::Note) {
            sawNote = true;
            CHECK_EQ(item.data.name, "note-1");
            CHECK_EQ(item.data.contents, note->contents);
            CHECK_EQ(item.data.author, "Tester");
        }
    }
    CHECK(sawNote);
    // Everything that was not edited is untouched.
    // (Edited annotations are re-created at the end of /Annots, so compare by kind.)
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].kind == Kind::Square || before[i].kind == Kind::Note) continue;
        const AnnotationView* same = findKind(after, before[i].kind);
        CHECK(same != nullptr);
        if (same != nullptr) CHECK(nearRect(same->bounds, before[i].bounds));
    }
}

// ---------------------------------------------------------------------------
// Unsupported kinds survive an edit of the same page
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsUnsupportedKindsSurviveCreatingAnAnnotation) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    const core::PageId page = first.session->pageId(0);
    first.load(page);
    CHECK(first.run(createAnnotations(*first.session, {squareDraft(page, core::Rect{50, 600, 80, 60})})));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const auto raw = second.raw(0);
    if (!raw) return;
    CHECK_EQ(raw->annotsCount, std::uint32_t{13});
    expectOpaqueEntriesIntact(*raw);
    // The created Square is appended after every original.
    if (raw->items.size() == 13) {
        CHECK(raw->items[12].editable);
        CHECK(raw->items[12].kind == Kind::Square);
    }
    CHECK_EQ(second.views(0).size(), kPage1Kinds.size() + 1);
    // Pages 2 and 3 are untouched.
    CHECK_EQ(second.raw(1)->annotsCount, std::uint32_t{1});
    CHECK_EQ(second.raw(2)->annotsCount, std::uint32_t{1});
}

// ---------------------------------------------------------------------------
// Dirty state
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsDirtyStateFollowsCreateUndoRedoSave) {
    Setup setup("annots.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    DocumentSession& session = *first.session;
    const core::PageId page = session.pageId(1);
    first.load(page);
    CHECK(!session.isDirty());

    CHECK(first.run(createAnnotations(session, {squareDraft(page, core::Rect{20, 20, 60, 40})})));
    CHECK(session.isDirty());
    CHECK(session.undo());
    CHECK(!session.isDirty());
    CHECK(session.redo());
    CHECK(session.isDirty());
    CHECK(saveAndRebase(session, *setup.engine, setup.path));
    CHECK(!session.isDirty());
}

// ---------------------------------------------------------------------------
// Hostile originals through the editor
// ---------------------------------------------------------------------------

RIVET_TEST(annotationPageOpsHostilePopupAndReplyLoopsSurviveEditing) {
    Setup setup("annots-hostile.pdf");
    if (!setup) return;
    Opened first(*setup.engine, setup.path);
    if (!first.session) return;
    // Page 5: notes whose /Popup is themselves (0), whose popup lives on
    // another page (2), and an /IRT cycle (3, 4).
    const std::vector<AnnotationView> before = first.views(5);
    CHECK_EQ(before.size(), std::size_t{4}); // the four Text notes; popup/file/links/widget are opaque
    if (before.size() != 4) return;
    CHECK(first.run(deleteAnnotations(*first.session, {before[0].id, before[1].id})));
    CHECK(first.run(moveAnnotation(*first.session, before[2].id, core::Point{15, 15})));
    CHECK(saveAndRebase(*first.session, *setup.engine, setup.path));

    Opened second(*setup.engine, setup.path);
    if (!second.session) return;
    const auto raw = second.raw(5);
    if (!raw) return;
    // Page had 9 entries: 2 deleted notes (the self popup is the note itself)
    // and the moved note removed and re-created at the end.
    CHECK_EQ(raw->annotsCount, std::uint32_t{7});
    const std::vector<AnnotationView> after = second.views(5);
    CHECK_EQ(after.size(), std::size_t{2}); // the untouched note + the moved one
    // Untouched pages still read.
    CHECK_EQ(second.raw(6)->annotsCount, std::uint32_t{2});
    CHECK_EQ(second.raw(7)->annotsCount, std::uint32_t{3});
}
