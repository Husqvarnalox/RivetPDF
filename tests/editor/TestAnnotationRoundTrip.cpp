// SPDX-License-Identifier: MPL-2.0
// Annotation round trip against the real backend: create every kind through
// the command pipeline, save with the real saver, rebase, reopen the file in
// a NEW session and compare. Returns early only when no PDF backend is built
// in. This is the Phase 4A gate: open -> create -> save -> reopen -> verify.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
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

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-annot-rt-" + std::to_string(::getpid()) + "-" +
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

constexpr double kEps = 0.01;

bool near(const core::Point& a, const core::Point& b) {
    return std::abs(a.x - b.x) <= kEps && std::abs(a.y - b.y) <= kEps;
}

bool nearRect(const core::Rect& a, const core::Rect& b) {
    return near(a.origin, b.origin) && std::abs(a.size.width - b.size.width) <= kEps &&
           std::abs(a.size.height - b.size.height) <= kEps;
}

AnnotationDraft base(core::PageId page, Kind kind) {
    AnnotationDraft d;
    d.page = page;
    d.kind = kind;
    d.style.color = pdf::PdfColor{0.2F, 0.4F, 0.8F};
    d.style.opacity = 0.5F;
    d.style.borderWidth = 2.0F;
    return d;
}

// Opens `path` in a fresh session with a queueing dispatcher.
struct Opened {
    Opened(pdf::PdfEngine& engine, const fs::path& path) {
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        if (created.has_value()) session = std::move(*created);
    }

    // Waits for the originals of `page` and returns the resolved list.
    AnnotationService::Resolved load(core::PageId page) {
        std::atomic<bool> done{false};
        session->setOnAnnotationsChanged([&](core::PageId changed) {
            if (changed == page) done = true;
        });
        auto list = session->annotations().annotations(page);
        CHECK(settle(dispatcher, [&] { return done.load() || session->annotations().cachedOriginalPages() > 0; }));
        session->setOnAnnotationsChanged({});
        return session->annotations().annotations(page);
    }

    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<DocumentSession> session;
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

} // namespace

RIVET_TEST(annotationRoundTripAllKindsThroughSaveRebaseAndReopen) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) return;

    TempDir dir;
    const fs::path path = dir / "rt.pdf";
    fs::copy_file(fs::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / "markers-5.pdf", path);

    Opened first(*engine, path);
    if (!first.session) return;
    {
        const auto probe = first.session->document().annotations(0);
        CHECK(probe.has_value());
        if (!probe.has_value()) return;
    }
    DocumentSession& session = *first.session;
    const core::PageId page = session.pageId(1);

    AnnotationDraft square = base(page, Kind::Square);
    square.rect = core::Rect{40, 60, 120, 50};
    AnnotationDraft highlight = base(page, Kind::Highlight);
    highlight.quads = {DisplayQuad{core::Point{40, 200}, core::Point{220, 200}, core::Point{40, 216},
                                   core::Point{220, 216}}};
    AnnotationDraft note = base(page, Kind::Note);
    note.rect = core::Rect{300, 40, 20, 20};
    note.contents = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\n\xD0\xBC\xD0\xB8\xD1\x80"; // Cyrillic, 2 lines
    AnnotationDraft ink = base(page, Kind::Ink);
    ink.strokes = {{core::Point{40, 300}, core::Point{80, 330}, core::Point{120, 300}},
                   {core::Point{150, 300}, core::Point{170, 330}}};
    AnnotationDraft stamp = base(page, Kind::Stamp);
    stamp.rect = core::Rect{40, 400, 140, 50};
    stamp.stampName = pdf::PdfStampName::Draft;
    std::vector<AnnotationDraft> drafts{square, highlight, note, ink, stamp};

    auto edit = createAnnotations(session, drafts);
    CHECK(edit.has_value());
    if (!edit.has_value()) return;
    const std::vector<core::AnnotationId> ids = edit->ids;
    CHECK(session.execute(std::move(edit->command)).has_value());
    CHECK(session.isDirty());

    CHECK(saveAndRebase(session, *engine, path));
    CHECK(!session.isDirty());

    // After the rebase the created items live in the file at the reported
    // indices; those stay suppressed from the raster, and the ids are kept.
    const PageAnnotationStatePtr state = session.pageSnapshot()->find(page)->annotations;
    CHECK(state != nullptr);
    if (state == nullptr) return;
    CHECK_EQ(state->overlay.size(), ids.size());
    std::vector<std::uint32_t> created;
    for (std::size_t i = 0; i < state->overlay.size(); ++i) {
        CHECK(state->overlay[i].id == ids[i]);
        CHECK(state->overlay[i].fileIndex.has_value());
        if (state->overlay[i].fileIndex.has_value()) created.push_back(*state->overlay[i].fileIndex);
    }
    std::sort(created.begin(), created.end());
    CHECK(state->suppressed == created);

    // Reopen in a NEW session and compare against the drafts.
    Opened second(*engine, path);
    if (!second.session) return;
    const core::PageId page2 = second.session->pageId(1);
    const auto list = second.load(page2);
    CHECK(list != nullptr);
    if (list == nullptr) return;
    CHECK_EQ(list->size(), drafts.size());
    const auto viewOfKind = [&](Kind kind) -> const AnnotationView* {
        for (const AnnotationView& view : *list) {
            if (view.kind == kind) return &view;
        }
        return nullptr;
    };
    for (const AnnotationDraft& d : drafts) {
        const AnnotationView* view = viewOfKind(d.kind);
        CHECK(view != nullptr);
        if (view == nullptr) continue;
        switch (d.kind) {
        case Kind::Square:
        case Kind::Note:
        case Kind::Stamp:
            CHECK(nearRect(view->bounds, d.rect));
            break;
        case Kind::Highlight:
            CHECK_EQ(view->quads.size(), std::size_t{1});
            if (!view->quads.empty()) {
                for (std::size_t k = 0; k < 4; ++k) CHECK(near(view->quads[0][k], d.quads[0][k]));
            }
            break;
        case Kind::Ink:
            CHECK_EQ(view->strokes.size(), d.strokes.size());
            for (std::size_t s = 0; s < d.strokes.size() && s < view->strokes.size(); ++s) {
                CHECK_EQ(view->strokes[s].size(), d.strokes[s].size());
                for (std::size_t k = 0; k < d.strokes[s].size() && k < view->strokes[s].size(); ++k) {
                    CHECK(near(view->strokes[s][k], d.strokes[s][k]));
                }
            }
            break;
        default:
            break;
        }
        if (d.kind == Kind::Note) CHECK_EQ(view->contents, d.contents);
        if (d.kind == Kind::Stamp) CHECK(view->stampName == pdf::PdfStampName::Draft);
    }

    // Delete one (the note) in the rebased session, save again, reopen.
    auto del = deleteAnnotations(session, {ids[2]});
    CHECK(del.has_value());
    if (!del.has_value()) return;
    CHECK(session.execute(std::move(del->command)).has_value());
    CHECK(session.isDirty());
    CHECK(saveAndRebase(session, *engine, path));
    CHECK(!session.isDirty());

    Opened third(*engine, path);
    if (!third.session) return;
    const auto after = third.load(third.session->pageId(1));
    CHECK(after != nullptr);
    if (after == nullptr) return;
    CHECK_EQ(after->size(), drafts.size() - 1);
    for (const AnnotationView& view : *after) CHECK(view.kind != Kind::Note);
}
