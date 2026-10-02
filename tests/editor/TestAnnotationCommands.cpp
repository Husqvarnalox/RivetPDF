// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "core/Error.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/AnnotationGeometry.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/PageCommands.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

// Annotation commands over the page model: creation in display space for
// every view, delete / edit semantics (originals become overlay items with
// the same id), capabilities and limits, undo/redo exactness, interaction
// with page operations, assembly requests, save/rebase re-keying, render
// targets, and dirty tracking.

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::ErrorCode;
using core::PageId;
using core::Point;
using core::Rect;
using Kind = pdf::PdfAnnotationKind;

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("rivet-annot-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter++));
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

AnnotationDraft draft(PageId page, Kind kind) {
    AnnotationDraft d;
    d.page = page;
    d.kind = kind;
    d.style.color = pdf::PdfColor{0.2F, 0.4F, 0.8F};
    d.style.opacity = 0.75F;
    d.style.borderWidth = 3.0F;
    return d;
}

AnnotationDraft squareDraft(PageId page, Rect rect = Rect{40, 50, 80, 40}) {
    AnnotationDraft d = draft(page, Kind::Square);
    d.rect = rect;
    return d;
}

AnnotationDraft highlightDraft(PageId page) {
    AnnotationDraft d = draft(page, Kind::Highlight);
    d.quads = {DisplayQuad{Point{40, 200}, Point{200, 200}, Point{40, 215}, Point{200, 215}}};
    return d;
}

AnnotationDraft noteDraft(PageId page) {
    AnnotationDraft d = draft(page, Kind::Note);
    d.rect = Rect{300, 40, 20, 20};
    d.contents = "note";
    return d;
}

AnnotationDraft inkDraft(PageId page) {
    AnnotationDraft d = draft(page, Kind::Ink);
    d.strokes = {{Point{40, 300}, Point{80, 330}, Point{120, 300}}, {Point{150, 300}, Point{160, 320}}};
    return d;
}

AnnotationDraft lineDraft(PageId page, Kind kind = Kind::Line) {
    AnnotationDraft d = draft(page, kind);
    d.lineStart = Point{40, 350};
    d.lineEnd = Point{200, 350};
    return d;
}

AnnotationDraft stampDraft(PageId page) {
    AnnotationDraft d = draft(page, Kind::Stamp);
    d.rect = Rect{40, 250, 120, 40};
    d.stampName = pdf::PdfStampName::Confidential;
    return d;
}

std::vector<AnnotationDraft> allKinds(PageId page) {
    AnnotationDraft circle = draft(page, Kind::Circle);
    circle.rect = Rect{150, 50, 80, 70};
    circle.style.interiorColor = pdf::PdfColor{1, 1, 0};
    return {squareDraft(page), circle, highlightDraft(page), noteDraft(page), inkDraft(page),
            lineDraft(page),   lineDraft(page, Kind::Arrow), stampDraft(page)};
}

bool near(const Point& a, const Point& b, double eps = 1e-6) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

bool nearRect(const Rect& a, const Rect& b, double eps = 1e-6) {
    return near(a.origin, b.origin, eps) && std::abs(a.size.width - b.size.width) <= eps &&
           std::abs(a.size.height - b.size.height) <= eps;
}

// Creates the drafts as one command; returns the new ids (empty on failure).
std::vector<core::AnnotationId> create(AnnotationFixture& f, std::vector<AnnotationDraft> drafts) {
    auto edit = createAnnotations(*f.session, std::move(drafts));
    CHECK(edit.has_value());
    if (!edit.has_value()) return {};
    auto ids = edit->ids;
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    return ids;
}

AnnotationView viewOfPage(AnnotationFixture& f, PageId page, core::AnnotationId id) {
    const auto found = f.session->annotations().find(page, id);
    CHECK(found.has_value());
    return found.value_or(AnnotationView{});
}

AnnotationView viewOf(AnnotationFixture& f, std::size_t page, core::AnnotationId id) {
    const auto found = f.session->annotations().find(f.id(page), id);
    CHECK(found.has_value());
    return found.value_or(AnnotationView{});
}

ErrorCode codeOf(const core::Result<AnnotationEdit>& edit) {
    return edit.has_value() ? ErrorCode::Internal : edit.error().code;
}

template <typename Fixture>
std::vector<core::AnnotationId> create2(Fixture& f, std::size_t page) {
    auto edit = createAnnotations(*f.session, {squareDraft(f.id(page))});
    CHECK(edit.has_value());
    if (!edit.has_value()) return {};
    auto ids = edit->ids;
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    return ids;
}

std::vector<pdf::PdfPageAnnotation> originalsWithPopup() {
    // 0 square, 1 note, 2 highlight (popup 3), 3 popup, 4 opaque.
    return {makeOriginal(0, squareAt(100, 100, 220, 180, 3.0F)), makeOriginal(1, noteAt(300, 600)),
            makeOriginal(2, highlightAt(100, 500, 200, 515), 3), makePopupEntry(3), makeOpaque(4)};
}

} // namespace

// --- Creation -------------------------------------------------------------------

RIVET_TEST(annotationCreateAllKindsInEveryView) {
    struct View {
        int degrees;
        std::optional<pdf::PdfBox> crop;
    };
    const View views[] = {{0, {}},  {90, {}},  {180, {}},  {270, {}},
                          {0, pdf::PdfBox{50, 100, 450, 700}}, {90, pdf::PdfBox{50, 100, 450, 700}},
                          {180, pdf::PdfBox{50, 100, 450, 700}}, {270, pdf::PdfBox{50, 100, 450, 700}}};
    for (const View& v : views) {
        AnnotationFixture f;
        if (v.crop.has_value()) {
            CHECK(f.session->execute(std::make_unique<CropPagesCommand>(f.session->pageModel(),
                                                                        std::vector<PageId>{f.id(0)}, v.crop))
                      .has_value());
        }
        if (v.degrees != 0) {
            CHECK(f.session->execute(std::make_unique<RotatePagesCommand>(f.session->pageModel(),
                                                                          std::vector<PageId>{f.id(0)}, v.degrees))
                      .has_value());
        }
        const auto drafts = allKinds(f.id(0));
        const auto ids = create(f, drafts);
        CHECK_EQ(ids.size(), drafts.size());
        CHECK(f.load(0));
        const auto resolved = f.list(0);
        CHECK_EQ(resolved->size(), drafts.size());
        for (std::size_t i = 0; i < drafts.size() && i < resolved->size(); ++i) {
            const AnnotationView& view = (*resolved)[i];
            const AnnotationDraft& d = drafts[i];
            CHECK(view.id == ids[i]);
            CHECK(view.kind == d.kind);
            CHECK(view.drawnByOverlay);
            CHECK(view.appearance.has_value());
            CHECK(view.style.color == d.style.color);
            CHECK(view.style.interiorColor == d.style.interiorColor);
            switch (d.kind) {
            case Kind::Square:
            case Kind::Circle:
            case Kind::Note:
                CHECK(nearRect(view.bounds, d.rect));
                break;
            case Kind::Stamp:
                CHECK(nearRect(view.bounds, d.rect));
                CHECK(view.stampName == pdf::PdfStampName::Confidential);
                // The label reads upright on screen whatever the page rotation.
                for (const DisplayText& text : view.appearance->texts) CHECK_EQ(text.rotation, 0);
                break;
            case Kind::Highlight:
            case Kind::Underline:
            case Kind::StrikeOut:
                CHECK_EQ(view.quads.size(), std::size_t{1});
                for (std::size_t k = 0; k < 4; ++k) CHECK(near(view.quads[0][k], d.quads[0][k]));
                break;
            case Kind::Ink:
                CHECK_EQ(view.strokes.size(), d.strokes.size());
                for (std::size_t s = 0; s < d.strokes.size() && s < view.strokes.size(); ++s) {
                    CHECK_EQ(view.strokes[s].size(), d.strokes[s].size());
                    for (std::size_t k = 0; k < d.strokes[s].size() && k < view.strokes[s].size(); ++k) {
                        CHECK(near(view.strokes[s][k], d.strokes[s][k]));
                    }
                }
                break;
            case Kind::Line:
            case Kind::Arrow:
                CHECK(near(view.lineStart, d.lineStart));
                CHECK(near(view.lineEnd, d.lineEnd));
                break;
            case Kind::Other:
                CHECK(false);
                break;
            }
        }
    }
}

RIVET_TEST(annotationCreateUndoRedoRestoresIdsStatesAndRevisions) {
    AnnotationFixture f;
    const std::uint64_t contentBefore = f.entry(0).contentRevision;
    const std::uint64_t rasterBefore = f.entry(0).rasterRevision;
    CHECK(f.entry(0).annotations == nullptr);
    CHECK(!f.session->isDirty());

    const auto ids = create(f, {squareDraft(f.id(0)), noteDraft(f.id(0))});
    CHECK_EQ(ids.size(), std::size_t{2});
    CHECK(ids[0] != ids[1]);
    const PageAnnotationStatePtr created = f.entry(0).annotations;
    CHECK(created != nullptr);
    CHECK_EQ(created->overlay.size(), std::size_t{2});
    CHECK(created->suppressed.empty());
    CHECK(created->overlay[0].id == ids[0]);
    CHECK(!created->overlay[0].fileIndex.has_value());
    // A creation changes neither the raster nor the content revision.
    CHECK_EQ(f.entry(0).rasterRevision, rasterBefore);
    CHECK_EQ(f.entry(0).contentRevision, contentBefore);
    CHECK(f.session->isDirty());

    CHECK(f.session->undo());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK_EQ(f.entry(0).rasterRevision, rasterBefore);
    CHECK(!f.session->isDirty()); // dirty state comes from the command stack only

    CHECK(f.session->redo());
    CHECK(f.entry(0).annotations == created); // exactly the same state object
    CHECK(f.entry(0).annotations->overlay[0].id == ids[0]);
    CHECK(f.entry(0).annotations->overlay[1].id == ids[1]);
    CHECK(f.session->isDirty());
}

RIVET_TEST(annotationCreateOnSeveralPagesIsOneUndoStep) {
    AnnotationFixture f;
    const auto ids = create(f, {squareDraft(f.id(0)), highlightDraft(f.id(1)), noteDraft(f.id(2))});
    CHECK_EQ(ids.size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i) CHECK(f.entry(i).annotations != nullptr);
    CHECK_EQ(f.session->commands().depth(), std::size_t{1});
    CHECK(f.session->undo());
    for (std::size_t i = 0; i < 3; ++i) CHECK(f.entry(i).annotations == nullptr);
    CHECK(!f.session->undo());
    CHECK(f.session->redo());
    for (std::size_t i = 0; i < 3; ++i) CHECK(f.entry(i).annotations != nullptr);
}

RIVET_TEST(annotationCreateRejectsInvalidDrafts) {
    AnnotationFixture f;
    const PageId page = f.id(0);
    CHECK_EQ(codeOf(createAnnotations(*f.session, {})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(createAnnotations(*f.session, {draft(page, Kind::Other)})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(createAnnotations(*f.session, {squareDraft(PageId{9999})})), ErrorCode::NotFound);
    CHECK_EQ(codeOf(createAnnotations(*f.session, {squareDraft(page, Rect{10, 10, 0.2, 0.2})})),
             ErrorCode::InvalidArgument); // smaller than the minimum
    CHECK_EQ(codeOf(createAnnotations(*f.session, {highlightDraft(page), draft(page, Kind::Highlight)})),
             ErrorCode::InvalidArgument); // no quads
    AnnotationDraft nan = squareDraft(page);
    nan.rect.origin.x = std::nan("");
    CHECK_EQ(codeOf(createAnnotations(*f.session, {nan})), ErrorCode::InvalidArgument);
    AnnotationDraft badText = noteDraft(page);
    badText.contents = std::string("a\xC3\x28");
    CHECK_EQ(codeOf(createAnnotations(*f.session, {badText})), ErrorCode::InvalidArgument);
    AnnotationDraft longText = noteDraft(page);
    longText.contents.assign(pdf::kMaxContentsBytes + 1, 'x');
    CHECK_EQ(codeOf(createAnnotations(*f.session, {longText})), ErrorCode::InvalidArgument);
    AnnotationDraft shortLine = lineDraft(page);
    shortLine.lineEnd = Point{40.1, 350};
    CHECK_EQ(codeOf(createAnnotations(*f.session, {shortLine})), ErrorCode::InvalidArgument);

    // Ink limits: too many strokes, too many points in total / per stroke.
    AnnotationDraft manyStrokes = draft(page, Kind::Ink);
    for (std::size_t i = 0; i < pdf::kMaxInkStrokes + 1; ++i) manyStrokes.strokes.push_back({Point{10, 10}});
    CHECK_EQ(codeOf(createAnnotations(*f.session, {manyStrokes})), ErrorCode::InvalidArgument);
    AnnotationDraft manyPoints = draft(page, Kind::Ink);
    for (int s = 0; s < 6; ++s) {
        std::vector<Point> stroke;
        for (int i = 0; i < 9000; ++i) stroke.push_back(Point{10.0 + i % 100, 10.0 + s});
        manyPoints.strokes.push_back(std::move(stroke));
    }
    CHECK_EQ(codeOf(createAnnotations(*f.session, {manyPoints})), ErrorCode::InvalidArgument);
    AnnotationDraft longStroke = draft(page, Kind::Ink);
    longStroke.strokes.push_back(std::vector<Point>(pdf::kMaxInkPointsPerStroke + 1, Point{20, 20}));
    CHECK_EQ(codeOf(createAnnotations(*f.session, {longStroke})), ErrorCode::InvalidArgument);

    // More overlay items than a page may hold.
    std::vector<AnnotationDraft> many(pdf::kMaxAnnotationsPerPage + 1, noteDraft(page));
    CHECK_EQ(codeOf(createAnnotations(*f.session, std::move(many))), ErrorCode::InvalidArgument);
    CHECK(f.entry(0).annotations == nullptr); // nothing was applied
    CHECK(!f.session->isDirty());
}

// --- Delete ---------------------------------------------------------------------

RIVET_TEST(annotationDeleteOriginalSuppressesIndexAndPopup) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const auto list = f.list(0);
    CHECK_EQ(list->size(), std::size_t{3});
    const core::AnnotationId highlight = (*list)[2].id;
    const std::uint64_t rasterBefore = f.entry(0).rasterRevision;

    auto edit = deleteAnnotations(*f.session, {highlight});
    CHECK(edit.has_value());
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    const PageAnnotationState* state = f.entry(0).annotations.get();
    CHECK(state != nullptr);
    CHECK(state->suppressed == (std::vector<std::uint32_t>{2, 3}));
    CHECK(state->overlay.empty());
    CHECK(f.entry(0).rasterRevision != rasterBefore); // the engine raster changed
    CHECK_EQ(f.list(0)->size(), std::size_t{2});
    CHECK(!f.session->annotations().find(f.id(0), highlight).has_value());
    CHECK(!f.session->annotations().locate(highlight).has_value()); // deleted

    const PageAnnotationStatePtr afterState = f.entry(0).annotations;
    const std::uint64_t afterRaster = f.entry(0).rasterRevision;
    CHECK(f.session->undo());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK_EQ(f.entry(0).rasterRevision, rasterBefore);
    CHECK_EQ(f.list(0)->size(), std::size_t{3});
    CHECK((*f.list(0))[2].id == highlight);
    CHECK(f.session->redo());
    CHECK(f.entry(0).annotations == afterState);
    CHECK_EQ(f.entry(0).rasterRevision, afterRaster);
}

RIVET_TEST(annotationDeleteOverlayDropsItWithoutRasterChange) {
    AnnotationFixture f;
    const auto ids = create(f, {squareDraft(f.id(0)), noteDraft(f.id(0))});
    const std::uint64_t raster = f.entry(0).rasterRevision;
    auto edit = deleteAnnotations(*f.session, {ids[0]});
    CHECK(edit.has_value());
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    CHECK_EQ(f.entry(0).annotations->overlay.size(), std::size_t{1});
    CHECK(f.entry(0).annotations->overlay[0].id == ids[1]);
    CHECK_EQ(f.entry(0).rasterRevision, raster);
    // Deleting the last one returns the page to "untouched".
    auto last = deleteAnnotations(*f.session, {ids[1]});
    CHECK(last.has_value());
    CHECK(f.session->execute(std::move(last->command)).has_value());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK(f.session->undo());
    CHECK(f.session->undo());
    CHECK_EQ(f.entry(0).annotations->overlay.size(), std::size_t{2});
    CHECK(f.entry(0).annotations->overlay[0].id == ids[0]);
}

RIVET_TEST(annotationDeleteSeveralAcrossPagesAndErrors) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const auto ids = create(f, {squareDraft(f.id(1)), noteDraft(f.id(1))});
    const auto originals = f.list(0);
    auto edit = deleteAnnotations(*f.session, {(*originals)[0].id, ids[1], (*originals)[1].id});
    CHECK(edit.has_value());
    CHECK_EQ(edit->ids.size(), std::size_t{3});
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    CHECK(f.entry(0).annotations->suppressed == (std::vector<std::uint32_t>{0, 1}));
    CHECK_EQ(f.entry(1).annotations->overlay.size(), std::size_t{1});
    CHECK(f.session->undo()); // one step for everything
    CHECK(f.entry(0).annotations == nullptr);
    CHECK_EQ(f.entry(1).annotations->overlay.size(), std::size_t{2});

    CHECK_EQ(codeOf(deleteAnnotations(*f.session, {})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(deleteAnnotations(*f.session, {ids[0], ids[0]})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(deleteAnnotations(*f.session, {core::AnnotationId{424242}})), ErrorCode::NotFound);
}

// --- Edit: originals become overlay items --------------------------------------

RIVET_TEST(annotationEditingAnOriginalMovesItToTheOverlayKeepingItsId) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const auto before = f.list(0);
    const core::AnnotationId square = (*before)[0].id;
    const Rect boundsBefore = (*before)[0].bounds;
    const std::uint64_t rasterBefore = f.entry(0).rasterRevision;

    auto edit = moveAnnotation(*f.session, square, Point{10, 20});
    CHECK(edit.has_value());
    CHECK(edit->ids == std::vector<core::AnnotationId>{square});
    CHECK(f.session->execute(std::move(edit->command)).has_value());

    const PageAnnotationState& state = *f.entry(0).annotations;
    CHECK(state.suppressed == (std::vector<std::uint32_t>{0}));
    CHECK_EQ(state.overlay.size(), std::size_t{1});
    CHECK(state.overlay[0].id == square);
    CHECK(!state.overlay[0].fileIndex.has_value());
    CHECK(f.entry(0).rasterRevision != rasterBefore);

    const auto after = f.list(0);
    CHECK_EQ(after->size(), std::size_t{3});
    // The edited annotation now comes last (overlay on top), same id, drawn by the overlay.
    CHECK((*after)[2].id == square);
    CHECK((*after)[2].drawnByOverlay);
    CHECK(nearRect((*after)[2].bounds, Rect{boundsBefore.origin.x + 10, boundsBefore.origin.y + 20,
                                            boundsBefore.size.width, boundsBefore.size.height}));
    CHECK((*after)[0].id == (*before)[1].id); // the others keep their order
    CHECK(!(*after)[0].drawnByOverlay);

    // Editing it again replaces the overlay item in place.
    auto again = moveAnnotation(*f.session, square, Point{5, 0});
    CHECK(again.has_value());
    CHECK(f.session->execute(std::move(again->command)).has_value());
    CHECK_EQ(f.entry(0).annotations->overlay.size(), std::size_t{1});
    CHECK(f.entry(0).annotations->suppressed == (std::vector<std::uint32_t>{0}));
    CHECK_EQ(f.entry(0).rasterRevision, f.entry(0).rasterRevision);

    CHECK(f.session->undo());
    CHECK(f.session->undo());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK_EQ(f.entry(0).rasterRevision, rasterBefore);
    CHECK(nearRect((*f.list(0))[0].bounds, boundsBefore));
}

RIVET_TEST(annotationEditingAnOriginalWithPopupSuppressesThePopupToo) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const core::AnnotationId highlight = (*f.list(0))[2].id;
    auto edit = restyleAnnotations(*f.session, {highlight}, StylePatch{pdf::PdfColor{1, 0, 0}, {}, {}, {}});
    CHECK(edit.has_value());
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    CHECK(f.entry(0).annotations->suppressed == (std::vector<std::uint32_t>{2, 3}));
    const AnnotationView view = viewOf(f, 0, highlight);
    CHECK(view.style.color == (pdf::PdfColor{1, 0, 0}));
}

// --- Move / resize / line / restyle / contents ---------------------------------

RIVET_TEST(annotationMoveEveryMovableKindInARotatedView) {
    AnnotationFixture f;
    CHECK(f.session->execute(std::make_unique<RotatePagesCommand>(f.session->pageModel(),
                                                                  std::vector<PageId>{f.id(0)}, 90))
              .has_value());
    auto drafts = allKinds(f.id(0));
    drafts.erase(drafts.begin() + 2); // not the highlight: markup cannot move
    const auto ids = create(f, drafts);
    CHECK_EQ(ids.size(), std::size_t{7});
    for (const core::AnnotationId id : ids) {
        const AnnotationView before = viewOf(f, 0, id);
        CHECK(f.run(moveAnnotation(*f.session, id, Point{12, -7})));
        const AnnotationView after = viewOf(f, 0, id);
        CHECK(nearRect(after.bounds, Rect{before.bounds.origin.x + 12, before.bounds.origin.y - 7,
                                          before.bounds.size.width, before.bounds.size.height}));
        CHECK(near(after.lineStart, Point{before.lineStart.x + 12, before.lineStart.y - 7}));
        CHECK_EQ(after.contents, before.contents);
        CHECK(after.style.color == before.style.color);
    }
    CHECK_EQ(f.entry(0).annotations->overlay.size(), std::size_t{7}); // edited in place, none duplicated
}

RIVET_TEST(annotationResizeEveryResizableKind) {
    AnnotationFixture f;
    AnnotationDraft arrow = lineDraft(f.id(0), Kind::Arrow);
    arrow.lineStart = Point{100, 400};
    arrow.lineEnd = Point{200, 400};
    AnnotationDraft circle = draft(f.id(0), Kind::Circle);
    circle.rect = Rect{150, 50, 80, 70};
    const auto ids = create(f, {squareDraft(f.id(0)), circle, inkDraft(f.id(0)), lineDraft(f.id(0)), arrow,
                                stampDraft(f.id(0))});
    for (const core::AnnotationId id : ids) {
        const AnnotationView before = viewOf(f, 0, id);
        const Rect target{before.bounds.origin.x + 5, before.bounds.origin.y + 3, before.bounds.size.width * 2.0,
                          before.bounds.size.height + 20.0};
        CHECK(f.run(resizeAnnotation(*f.session, id, target)));
        const AnnotationView after = viewOf(f, 0, id);
        const bool horizontalLine = before.kind == Kind::Line || before.kind == Kind::Arrow; // zero-height points
        CHECK_NEAR(after.bounds.minX(), target.minX(), 1e-6);
        CHECK_NEAR(after.bounds.maxX(), target.maxX(), 1e-6);
        if (!horizontalLine) {
            CHECK_NEAR(after.bounds.minY(), target.minY(), 1e-6);
            CHECK_NEAR(after.bounds.maxY(), target.maxY(), 1e-6);
        }
        // Resizing back to the original bounds restores the geometry (no drift).
        CHECK(f.run(resizeAnnotation(*f.session, id, before.bounds)));
        const AnnotationView restored = viewOf(f, 0, id);
        CHECK_NEAR(restored.bounds.minX(), before.bounds.minX(), 1e-5);
        CHECK_NEAR(restored.bounds.maxX(), before.bounds.maxX(), 1e-5);
        if (!horizontalLine) {
            CHECK_NEAR(restored.bounds.minY(), before.bounds.minY(), 1e-5);
            CHECK_NEAR(restored.bounds.maxY(), before.bounds.maxY(), 1e-5);
        }
    }
}

RIVET_TEST(annotationResizeScalesInkPointsProportionallyKeepingMargins) {
    AnnotationFixture f;
    const auto ids = create(f, {inkDraft(f.id(0))});
    const AnnotationView before = viewOf(f, 0, ids[0]);
    const auto extent = [](const AnnotationView& v) {
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (const auto& stroke : v.strokes) {
            for (const Point& p : stroke) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
        }
        return Rect{minX, minY, maxX - minX, maxY - minY};
    };
    const Rect pointsBefore = extent(before);
    const Rect target{before.bounds.origin.x + 10, before.bounds.origin.y - 5, before.bounds.size.width * 2.0,
                      before.bounds.size.height * 1.5};
    CHECK(f.run(resizeAnnotation(*f.session, ids[0], target)));
    const AnnotationView after = viewOf(f, 0, ids[0]);
    const Rect pointsAfter = extent(after);
    // Margins between the point box and the bounds are unchanged.
    CHECK_NEAR(pointsAfter.minX() - after.bounds.minX(), pointsBefore.minX() - before.bounds.minX(), 1e-6);
    CHECK_NEAR(after.bounds.maxX() - pointsAfter.maxX(), before.bounds.maxX() - pointsBefore.maxX(), 1e-6);
    CHECK_NEAR(pointsAfter.minY() - after.bounds.minY(), pointsBefore.minY() - before.bounds.minY(), 1e-6);
    // Every point keeps its relative position inside the point box.
    for (std::size_t s = 0; s < before.strokes.size(); ++s) {
        for (std::size_t i = 0; i < before.strokes[s].size(); ++i) {
            const double fxBefore = (before.strokes[s][i].x - pointsBefore.minX()) / pointsBefore.size.width;
            const double fxAfter = (after.strokes[s][i].x - pointsAfter.minX()) / pointsAfter.size.width;
            CHECK_NEAR(fxAfter, fxBefore, 1e-6);
            const double fyBefore = (before.strokes[s][i].y - pointsBefore.minY()) / pointsBefore.size.height;
            const double fyAfter = (after.strokes[s][i].y - pointsAfter.minY()) / pointsAfter.size.height;
            CHECK_NEAR(fyAfter, fyBefore, 1e-6);
        }
    }
}

RIVET_TEST(annotationSetLineEndpoints) {
    AnnotationFixture f;
    const auto ids = create(f, {lineDraft(f.id(0)), lineDraft(f.id(0), Kind::Arrow), inkDraft(f.id(0))});
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(f.run(setLineEndpoints(*f.session, ids[i], Point{60, 70}, Point{250, 180})));
        const AnnotationView v = viewOf(f, 0, ids[i]);
        CHECK(near(v.lineStart, Point{60, 70}));
        CHECK(near(v.lineEnd, Point{250, 180}));
    }
    CHECK_EQ(codeOf(setLineEndpoints(*f.session, ids[2], Point{1, 1}, Point{50, 50})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(setLineEndpoints(*f.session, ids[0], Point{10, 10}, Point{10.1, 10})), ErrorCode::InvalidArgument);
}

RIVET_TEST(annotationRestyleAppliesOnlyWhatFitsEachKind) {
    AnnotationFixture f;
    AnnotationDraft circle = draft(f.id(0), Kind::Circle);
    circle.rect = Rect{150, 50, 80, 70};
    const auto ids = create(f, {squareDraft(f.id(0)), circle, highlightDraft(f.id(0)), noteDraft(f.id(0))});
    StylePatch patch;
    patch.color = pdf::PdfColor{0, 1, 0};
    patch.interiorColor = std::optional<pdf::PdfColor>{pdf::PdfColor{1, 1, 1}};
    patch.opacity = 0.5F;
    patch.borderWidth = 6.0F;
    auto edit = restyleAnnotations(*f.session, ids, patch);
    CHECK(edit.has_value());
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    CHECK_EQ(f.entry(0).annotations->overlay.size(), std::size_t{4});

    const AnnotationView square = viewOf(f, 0, ids[0]);
    CHECK(square.style.color == (pdf::PdfColor{0, 1, 0}));
    CHECK(square.style.interiorColor == std::optional<pdf::PdfColor>{pdf::PdfColor{1, 1, 1}});
    CHECK_NEAR(square.style.opacity, 0.5F, 1e-6F);
    CHECK_NEAR(square.style.borderWidth, 6.0F, 1e-6F);
    const AnnotationView highlight = viewOf(f, 0, ids[2]);
    CHECK(highlight.style.color == (pdf::PdfColor{0, 1, 0}));
    CHECK_NEAR(highlight.style.opacity, 0.5F, 1e-6F);
    CHECK(!highlight.style.interiorColor.has_value()); // no fill on markup
    const AnnotationView note = viewOf(f, 0, ids[3]);
    CHECK(note.style.color == (pdf::PdfColor{0, 1, 0}));
    CHECK_NEAR(note.style.opacity, 0.75F, 1e-6F); // notes ignore opacity

    // Removing the fill; a patch that changes nothing is refused.
    StylePatch noFill;
    noFill.interiorColor = std::optional<std::optional<pdf::PdfColor>>{std::optional<pdf::PdfColor>{}};
    CHECK(f.run(restyleAnnotations(*f.session, {ids[0]}, noFill)));
    CHECK(!viewOf(f, 0, ids[0]).style.interiorColor.has_value());
    CHECK_EQ(codeOf(restyleAnnotations(*f.session, {ids[0]}, noFill)), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(restyleAnnotations(*f.session, {ids[0]}, StylePatch{})), ErrorCode::InvalidArgument);
}

RIVET_TEST(annotationEditContents) {
    AnnotationFixture f;
    AnnotationDraft stamp = stampDraft(f.id(0));
    const auto ids = create(f, {highlightDraft(f.id(0)), noteDraft(f.id(0)), squareDraft(f.id(0)), stamp});
    const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\n\xD0\xBC\xD0\xB8\xD1\x80";
    CHECK(f.run(editContents(*f.session, ids[0], cyrillic)));
    CHECK(f.run(editContents(*f.session, ids[1], cyrillic)));
    CHECK_EQ(viewOf(f, 0, ids[0]).contents, cyrillic);
    CHECK_EQ(viewOf(f, 0, ids[1]).contents, cyrillic);
    // Same text: nothing to do.
    CHECK_EQ(codeOf(editContents(*f.session, ids[1], cyrillic)), ErrorCode::InvalidArgument);
    // Squares and stamps have no editable text (capability).
    CHECK_EQ(codeOf(editContents(*f.session, ids[2], "x")), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(editContents(*f.session, ids[3], "x")), ErrorCode::InvalidArgument);
    // Invalid UTF-8 and over-long text are refused.
    CHECK_EQ(codeOf(editContents(*f.session, ids[1], std::string("\xC3\x28"))), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(editContents(*f.session, ids[1], std::string(pdf::kMaxContentsBytes + 1, 'a'))),
             ErrorCode::InvalidArgument);
    CHECK(f.run(editContents(*f.session, ids[1], std::string(pdf::kMaxContentsBytes, 'a'))));
    CHECK(f.run(editContents(*f.session, ids[1], "")));
    CHECK_EQ(viewOf(f, 0, ids[1]).contents, std::string());
}

RIVET_TEST(annotationCapabilitiesAreEnforced) {
    AnnotationFixture f;
    const auto ids = create(f, {highlightDraft(f.id(0)), noteDraft(f.id(0)), squareDraft(f.id(0))});
    // Markup: no move, no resize.
    CHECK_EQ(codeOf(moveAnnotation(*f.session, ids[0], Point{5, 5})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(resizeAnnotation(*f.session, ids[0], Rect{0, 0, 50, 50})), ErrorCode::InvalidArgument);
    // Note: moves, but does not resize.
    CHECK(moveAnnotation(*f.session, ids[1], Point{5, 5}).has_value());
    CHECK_EQ(codeOf(resizeAnnotation(*f.session, ids[1], Rect{0, 0, 50, 50})), ErrorCode::InvalidArgument);
    // Bad geometry arguments.
    CHECK_EQ(codeOf(resizeAnnotation(*f.session, ids[2], Rect{0, 0, 0, 10})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(resizeAnnotation(*f.session, ids[2], Rect{0, 0, std::nan(""), 10})), ErrorCode::InvalidArgument);
    CHECK_EQ(codeOf(moveAnnotation(*f.session, ids[2], Point{std::nan(""), 0})), ErrorCode::InvalidArgument);
    // A resize below the minimum size is not writable.
    CHECK_EQ(codeOf(resizeAnnotation(*f.session, ids[2], Rect{10, 10, 0.3, 0.3})), ErrorCode::InvalidArgument);
    // Unknown ids.
    CHECK_EQ(codeOf(moveAnnotation(*f.session, core::AnnotationId{777}, Point{1, 1})), ErrorCode::NotFound);
}

RIVET_TEST(annotationStaleCommandFailsWithoutTouchingTheModel) {
    AnnotationFixture f;
    const auto ids = create(f, {squareDraft(f.id(0))});
    auto first = moveAnnotation(*f.session, ids[0], Point{5, 5});
    auto second = moveAnnotation(*f.session, ids[0], Point{0, 9});
    CHECK(first.has_value() && second.has_value());
    CHECK(f.session->execute(std::move(first->command)).has_value());
    const PageSnapshotPtr snapshot = f.session->pageSnapshot();
    const PageAnnotationStatePtr state = f.entry(0).annotations;
    const auto status = f.session->execute(std::move(second->command)); // built against the old state
    CHECK(!status.has_value());
    CHECK(status.error().code == ErrorCode::InvalidArgument);
    CHECK(f.session->pageSnapshot() == snapshot); // no publish
    CHECK(f.entry(0).annotations == state);
    CHECK_EQ(f.session->commands().depth(), std::size_t{2}); // create + first move only
}

RIVET_TEST(annotationDirtyComesFromTheCommandStackOnly) {
    AnnotationFixture f;
    bool dirty = false;
    int notifications = 0;
    f.session->setOnDirtyChanged([&](bool d) {
        dirty = d;
        ++notifications;
    });
    const auto ids = create(f, {squareDraft(f.id(0))});
    CHECK(dirty && f.session->isDirty());
    CHECK(f.run(moveAnnotation(*f.session, ids[0], Point{3, 3})));
    CHECK(f.session->undo());
    CHECK(f.session->isDirty());
    CHECK(f.session->undo());
    CHECK(!f.session->isDirty());
    CHECK(!dirty);
    CHECK(f.session->redo());
    CHECK(f.session->isDirty());
    f.session->markSaved();
    CHECK(!f.session->isDirty());
    CHECK(f.session->undo());
    CHECK(f.session->isDirty()); // differs from the saved state
}

// --- Page operations ------------------------------------------------------------

RIVET_TEST(annotationPageOperationsKeepAndReprojectState) {
    AnnotationFixture f(4);
    const auto ids = create(f, {squareDraft(f.id(0))});
    const PageAnnotationStatePtr state = f.entry(0).annotations;
    const PageId page = f.id(0);

    // Reorder keeps the state.
    CHECK(f.session->execute(std::make_unique<MovePagesCommand>(f.session->pageModel(), std::vector<PageId>{page}, 3))
              .has_value());
    CHECK(f.session->pageSnapshot()->find(page)->annotations == state);

    // Rotate: both revisions are fresh and equal; the data is untouched; the view re-projects.
    const PageEntry beforeRotate = *f.session->pageSnapshot()->find(page);
    const Rect boundsBefore = viewOfPage(f, page, ids[0]).bounds;
    CHECK(f.session->execute(std::make_unique<RotatePagesCommand>(f.session->pageModel(), std::vector<PageId>{page}, 90))
              .has_value());
    const PageEntry& rotated = *f.session->pageSnapshot()->find(page);
    CHECK(rotated.annotations == state);
    CHECK(rotated.contentRevision != beforeRotate.contentRevision);
    CHECK_EQ(rotated.rasterRevision, rotated.contentRevision);
    CHECK(!nearRect(viewOfPage(f, page, ids[0]).bounds, boundsBefore));
    CHECK(f.session->undo());
    CHECK_EQ(f.session->pageSnapshot()->find(page)->contentRevision, beforeRotate.contentRevision);
    CHECK_EQ(f.session->pageSnapshot()->find(page)->rasterRevision, beforeRotate.rasterRevision);
    CHECK(nearRect(viewOfPage(f, page, ids[0]).bounds, boundsBefore));

    // Crop away the annotation: the data stays (it simply lies outside).
    CHECK(f.session->execute(std::make_unique<CropPagesCommand>(
                                 f.session->pageModel(), std::vector<PageId>{page},
                                 std::optional<pdf::PdfBox>{pdf::PdfBox{400, 400, 600, 700}}))
              .has_value());
    CHECK(f.session->pageSnapshot()->find(page)->annotations == state);
    CHECK_EQ(f.session->pageSnapshot()->find(page)->annotations->overlay.size(), std::size_t{1});
    CHECK(f.session->undo());
    CHECK(nearRect(viewOfPage(f, page, ids[0]).bounds, boundsBefore));
}

RIVET_TEST(annotationDuplicatePageCopiesStateWithFreshIds) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const core::AnnotationId original = (*f.list(0))[0].id;
    CHECK(f.run(moveAnnotation(*f.session, original, Point{4, 4}))); // edited original -> overlay item
    const auto ids = create(f, {noteDraft(f.id(0))});
    const PageId page = f.id(0);
    const std::size_t sizeBefore = f.session->pageCount();

    CHECK(f.session->execute(std::make_unique<DuplicatePagesCommand>(f.session->pageModel(), std::vector<PageId>{page}))
              .has_value());
    CHECK_EQ(f.session->pageCount(), sizeBefore + 1);
    const auto snapshotAfterDuplicate = f.session->pageSnapshot(); // keeps `copy` alive across later edits
    const PageEntry& copy = snapshotAfterDuplicate->at(1);
    CHECK(copy.id != page);
    CHECK(copy.annotations != nullptr);
    CHECK(copy.annotations != f.entry(0).annotations);
    CHECK(copy.annotations->suppressed == f.entry(0).annotations->suppressed);
    CHECK_EQ(copy.annotations->overlay.size(), std::size_t{2});
    for (std::size_t i = 0; i < 2; ++i) {
        const OverlayAnnotation& a = f.entry(0).annotations->overlay[i];
        const OverlayAnnotation& b = copy.annotations->overlay[i];
        CHECK(a.id != b.id); // fresh ids
        CHECK(a.data == b.data);
        CHECK(a.fileIndex == b.fileIndex);
    }
    CHECK(copy.annotations->overlay[0].id != original && copy.annotations->overlay[1].id != ids[0]);

    // Independent: editing the copy leaves the original page alone.
    const PageAnnotationStatePtr originalState = f.entry(0).annotations;
    CHECK(f.run(moveAnnotation(*f.session, copy.annotations->overlay[1].id, Point{30, 30})));
    CHECK(f.entry(0).annotations == originalState);
    CHECK(f.session->pageSnapshot()->at(1).annotations != copy.annotations);
}

RIVET_TEST(annotationDeletePageAndUndoRestoresStateAndIds) {
    AnnotationFixture f;
    const auto ids = create(f, {squareDraft(f.id(1)), noteDraft(f.id(1))});
    const PageAnnotationStatePtr state = f.entry(1).annotations;
    const PageId page = f.id(1);
    CHECK(f.session->execute(std::make_unique<DeletePagesCommand>(f.session->pageModel(), std::vector<PageId>{page}))
              .has_value());
    CHECK(f.session->pageSnapshot()->find(page) == nullptr);
    CHECK(f.session->undo());
    const PageEntry* back = f.session->pageSnapshot()->find(page);
    CHECK(back != nullptr);
    if (back == nullptr) return;
    CHECK(back->annotations == state);
    CHECK(back->annotations->overlay[0].id == ids[0]);
    CHECK(back->annotations->overlay[1].id == ids[1]);
    CHECK(f.session->annotations().find(page, ids[1]).has_value());
}

RIVET_TEST(annotationInsertedPagesStartUntouched) {
    AnnotationFixture f;
    std::shared_ptr<pdf::PdfDocument> imported = std::make_shared<FakePageDocument>(2, "other-");
    const std::size_t importPages[] = {0, 1};
    auto sources = PageModel::describePages(imported, importPages);
    CHECK(sources.has_value());
    if (!sources.has_value()) return;
    CHECK(f.session->execute(std::make_unique<InsertPagesCommand>(f.session->pageModel(), *sources, 1))
              .has_value());
    CHECK(f.session->pageCount() == 5);
    for (std::size_t i = 1; i <= 2; ++i) {
        CHECK(f.session->pageSnapshot()->at(i).annotations == nullptr);
        CHECK_EQ(f.session->pageSnapshot()->at(i).rasterRevision, f.session->pageSnapshot()->at(i).contentRevision);
    }
}

RIVET_TEST(annotationEditsDoNotTouchContentRevisionOrText) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const std::uint64_t content = f.entry(0).contentRevision;
    const int extractions = f.document->extractions.load();
    const auto ids = create(f, {squareDraft(f.id(0))});
    CHECK(f.run(moveAnnotation(*f.session, ids[0], Point{2, 2})));
    CHECK(f.run(moveAnnotation(*f.session, (*f.list(0))[0].id, Point{2, 2})));
    CHECK(f.run(deleteAnnotations(*f.session, {ids[0]})));
    CHECK_EQ(f.entry(0).contentRevision, content);
    CHECK_EQ(f.document->extractions.load(), extractions);
}

// --- Assembly requests ----------------------------------------------------------

RIVET_TEST(annotationAssemblyRequestCarriesEdits) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const core::AnnotationId highlight = (*f.list(0))[2].id;
    CHECK(f.run(deleteAnnotations(*f.session, {highlight})));
    const auto created = create(f, {squareDraft(f.id(0)), noteDraft(f.id(0))});
    const auto ids1 = create(f, {squareDraft(f.id(1))});
    (void)created;
    (void)ids1;

    const auto save = f.session->pageSnapshot()->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    CHECK(save.has_value());
    if (!save) return;
    CHECK_EQ(save->pages.size(), std::size_t{3});
    const auto& edits0 = save->pages[0].annotationEdits;
    CHECK(edits0 != nullptr);
    if (edits0 != nullptr) {
        CHECK(edits0->removeIndices == (std::vector<std::uint32_t>{2, 3}));
        CHECK_EQ(edits0->create.size(), std::size_t{2});
        CHECK(edits0->create[0].kind == Kind::Square);
        CHECK(edits0->create[1].kind == Kind::Note);
    }
    const auto& edits1 = save->pages[1].annotationEdits;
    CHECK(edits1 != nullptr);
    if (edits1 != nullptr) {
        CHECK(edits1->removeIndices.empty());
        CHECK_EQ(edits1->create.size(), std::size_t{1});
    }
    CHECK(save->pages[2].annotationEdits == nullptr); // untouched page

    // An extract (Fresh) request of a subset carries the edits of those pages too.
    const std::vector<PageId> subset{f.id(1)};
    const auto extract = f.session->pageSnapshot()->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Extract, subset);
    CHECK(extract.has_value());
    if (extract && extract->pages.size() == 1) {
        CHECK(extract->pages[0].annotationEdits != nullptr);
    }
}

// --- Render targets -------------------------------------------------------------

namespace {

bool renderPage(AnnotationFixture& f, PageId page, std::uint64_t revision) {
    const PageEntry* entry = f.session->pageSnapshot()->find(page);
    CHECK(entry != nullptr);
    if (entry == nullptr) return false;
    render::RenderRequest r;
    r.key.documentId = f.session->id();
    r.key.pageId = page;
    r.key.scale = render::PhysicalRenderScaleKey::fromDensities(0.25, 1.0);
    r.key.contentRevision = revision;
    const core::Size size = pdf::displaySize(entry->view);
    r.params.pageRectPoints = Rect{0.0, 0.0, size.width, size.height};
    r.params.devicePixelsPerPoint = r.key.scale.scale();
    auto done = std::make_shared<std::atomic<int>>(0); // 0 pending, 1 ok, 2 failed
    f.session->renderSource().requestRender(r, render::RenderPriority::Visible, [done](render::RenderResult result) {
        done->store(result.has_value() ? 1 : 2);
    });
    CHECK(settle(f.dispatcher, [&] { return done->load() != 0; }));
    return done->load() == 1;
}

} // namespace

RIVET_TEST(annotationRenderTargetHidesSuppressedIndicesAndKeysTheRasterRevision) {
    AnnotationFixture f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    CHECK(renderPage(f, f.id(0), f.entry(0).rasterRevision));
    CHECK(f.document->lastHiddenAnnotations().empty());

    CHECK(f.run(deleteAnnotations(*f.session, {(*f.list(0))[2].id})));
    const std::uint64_t raster = f.entry(0).rasterRevision;
    CHECK(renderPage(f, f.id(0), raster));
    CHECK((f.document->lastHiddenAnnotations() == std::vector<std::uint32_t>{2, 3}));

    // A request for the previous raster revision is stale and fails.
    CHECK(!renderPage(f, f.id(0), raster - 1));
}

// --- Save and rebase ------------------------------------------------------------

namespace {

struct SaveFixture {
    SaveFixture() : f(dir / "doc.pdf") {}
    TempDir dir;
    BasicAnnotationFixture<FakeWritableEngine> f;

    // Saves the session's model to `path` and rebases onto the result.
    core::Status saveAndRebase() {
        const auto path = dir / "out.pdf";
        auto job = makeSaveJob(*f.session, path);
        CHECK(job.has_value());
        if (!job) return core::ok();
        const auto result = runDocumentWrite(f.engine, *job);
        CHECK(result.written.has_value());
        CHECK(result.rebase.has_value());
        if (!result.rebase || !result.rebase->has_value()) return core::ok();
        job->snapshot.reset();
        auto status = f.session->rebaseOnto(std::move(**result.rebase));
        if (status) f.session->markSaved();
        return status;
    }
};

} // namespace

RIVET_TEST(annotationSaveRebaseRekeysIdsAndKeepsRasterRevision) {
    SaveFixture s;
    auto& f = s.f;
    f.document->setAnnotations(0, originalsWithPopup());
    CHECK(f.load(0));
    const auto before = f.list(0);
    const core::AnnotationId edited = (*before)[0].id;
    CHECK(f.run(moveAnnotation(*f.session, edited, Point{5, 5})));
    const auto created = create2(f, 0);
    (void)created;
    CHECK(f.session->isDirty());
    const std::uint64_t raster = f.entry(0).rasterRevision;
    const std::uint64_t content = f.entry(0).contentRevision;
    const PageAnnotationState stateBefore = *f.entry(0).annotations;
    CHECK_EQ(stateBefore.overlay.size(), std::size_t{2});

    CHECK(s.saveAndRebase());
    CHECK(!f.session->isDirty());
    CHECK_EQ(f.entry(0).rasterRevision, raster);
    CHECK_EQ(f.entry(0).contentRevision, content);
    CHECK(f.entry(0).annotations != nullptr);
    if (f.entry(0).annotations == nullptr) return;
    const PageAnnotationState& after = *f.entry(0).annotations;
    // The report lists the created indices at the end of /Annots; those stay suppressed.
    CHECK(after.suppressed.size() == after.overlay.size());
    for (std::size_t i = 0; i < after.overlay.size(); ++i) {
        CHECK(after.overlay[i].id == stateBefore.overlay[i].id);
        CHECK(after.overlay[i].fileIndex.has_value());
        CHECK(std::find(after.suppressed.begin(), after.suppressed.end(), *after.overlay[i].fileIndex) !=
              after.suppressed.end());
    }
    // Ids survive: the annotation is still found, and editing it again works.
    CHECK(f.session->annotations().find(f.id(0), edited).has_value());
    CHECK(f.run(moveAnnotation(*f.session, edited, Point{1, 1})));
    CHECK(f.session->isDirty());
    // The undo history was cleared by the rebase.
    CHECK(f.session->undo());
    CHECK(!f.session->undo());
}

RIVET_TEST(annotationSaveRebaseWithoutReportDropsStateWithFreshRaster) {
    SaveFixture s;
    auto& f = s.f;
    f.engine.reportAnnotations = false;
    create2(f, 0);
    const std::uint64_t raster = f.entry(0).rasterRevision;
    CHECK(s.saveAndRebase());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK(f.entry(0).rasterRevision != raster);
}

RIVET_TEST(annotationSaveRebaseWithMismatchingReportDropsState) {
    SaveFixture s;
    auto& f = s.f;
    create2(f, 0);
    const std::uint64_t raster = f.entry(0).rasterRevision;
    std::vector<pdf::PdfAssembledPageAnnotations> wrong(f.session->pageCount());
    f.engine.reportOverride = wrong; // createdIndices empty != one overlay item
    CHECK(s.saveAndRebase());
    CHECK(f.entry(0).annotations == nullptr);
    CHECK(f.entry(0).rasterRevision != raster);
}

RIVET_TEST(annotationSaveSendsEditsToTheEngine) {
    SaveFixture s;
    auto& f = s.f;
    f.document->setAnnotations(1, originalsWithPopup());
    CHECK(f.load(1));
    CHECK(f.run(deleteAnnotations(*f.session, {(*f.list(1))[0].id})));
    create2(f, 0);
    CHECK(s.saveAndRebase());
    CHECK_EQ(f.engine.lastEdits.size(), f.session->pageCount());
    CHECK_EQ(f.engine.lastEdits[0].create.size(), std::size_t{1});
    CHECK(f.engine.lastEdits[1].removeIndices == (std::vector<std::uint32_t>{0}));
}
