#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfTypes.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderScaleKey.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;
using rivet::render::RenderPriority;
using rivet::render::RenderRequest;
using rivet::render::RenderScaleKey;

namespace {

// In-memory PdfDocument: 3 pages {200,300}, {300,200}, {612,792}, no rotation;
// renderPage produces a bitmap of the requested pixel size filled with 0xAB.
class FakePdfDocument final : public PdfDocument {
public:
    FakePdfDocument() : releaseFuture_(renderRelease_.get_future()) {
        info_.pageCount = pageSizes_.size();
        info_.isEncrypted = false;
        info_.title = "fake";
    }

    const PdfDocumentInfo& info() const override { return info_; }

    Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override {
        if (pageIndex >= pageSizes_.size()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "page index out of range", "test"});
        }
        return PdfPageInfo{pageIndex, pageSizes_[pageIndex], rivet::core::PageRotation::None};
    }

    Result<Bitmap> renderPage(std::size_t pageIndex, const Rect& pageRectPoints,
                              double devicePixelsPerPoint) override {
        ++renderCalls;
        lastPageIndex = pageIndex;

        // One-shot blocking hook so tests can hold a render in flight.
        if (blockNextRender.exchange(false)) {
            renderEntered_.set_value();
            (void)releaseFuture_.wait_for(std::chrono::seconds(10));
        }

        if (pageIndex == failingPageIndex.load()) {
            return std::unexpected(Error{ErrorCode::Io, "fake rasterization failure", "test"});
        }

        const auto width = static_cast<std::uint32_t>(pageRectPoints.size.width * devicePixelsPerPoint + 0.5);
        const auto height = static_cast<std::uint32_t>(pageRectPoints.size.height * devicePixelsPerPoint + 0.5);
        auto bitmap = Bitmap::create(width, height);
        if (!bitmap.has_value()) {
            return std::unexpected(bitmap.error());
        }
        std::memset(bitmap->data(), 0xAB, bitmap->sizeBytes());
        return bitmap;
    }

    PdfDocumentInfo info_;
    std::vector<Size> pageSizes_{Size{200, 300}, Size{300, 200}, Size{612, 792}};

    std::atomic<int> renderCalls{0};
    std::atomic<std::size_t> lastPageIndex{0};
    std::atomic<std::size_t> failingPageIndex{static_cast<std::size_t>(-1)};
    std::atomic<bool> blockNextRender{false};

    void releaseRender() { renderRelease_.set_value(); }
    std::future<void> renderEntered() { return renderEntered_.get_future(); }

private:
    std::promise<void> renderEntered_;
    std::promise<void> renderRelease_;
    std::future<void> releaseFuture_;
};

class FakePdfEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }

    std::string_view backendName() const override { return "fake"; }

    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&,
                                                      std::string_view) override {
        auto document = std::make_unique<FakePdfDocument>();
        lastDocument = document.get();
        return document;
    }

    FakePdfDocument* lastDocument = nullptr;
};

class UnavailablePdfEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return false; }

    std::string_view backendName() const override { return "none"; }

    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&,
                                                      std::string_view) override {
        return std::unexpected(Error{ErrorCode::NotAvailable, "no backend in this build", "test"});
    }
};

std::unique_ptr<DocumentSession> makeSession(PdfEngine& engine, TaskScheduler& scheduler) {
    auto session = DocumentSession::create(engine, scheduler, nullptr, std::filesystem::path{"fake.pdf"});
    CHECK(session.has_value());
    return std::move(*session);
}

RenderRequest makeRequest(const DocumentSession& session, std::size_t pageIndex, std::uint32_t tileY = 0) {
    RenderRequest request;
    request.key.documentId = session.id();
    request.key.pageId = session.pageId(pageIndex);
    request.key.scale = RenderScaleKey::fromZoom(1.0);
    request.key.tileX = 0;
    request.key.tileY = tileY;
    const Size pageSize = session.pageSizePoints(pageIndex);
    request.params.pageRectPoints = Rect{0.0, 0.0, pageSize.width, pageSize.height};
    request.params.devicePixelsPerPoint = 1.0;
    return request;
}

// Bounded wait; correctness is asserted through the future's payload, this
// only keeps a hung delivery from hanging the test forever.
template <typename Future>
bool settled(const Future& future, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    return future.wait_for(timeout) == std::future_status::ready;
}

void recordDelivery(std::promise<Result<Bitmap>>& target, Result<Bitmap> result) {
    target.set_value(std::move(result));
}

} // namespace

RIVET_TEST(createBuildsSession) {
    TaskScheduler scheduler(2);
    FakePdfEngine engine;

    auto session = DocumentSession::create(engine, scheduler, nullptr, std::filesystem::path{"fake.pdf"});
    CHECK(session.has_value());
    CHECK_EQ((*session)->pageCount(), std::size_t{3});
    CHECK((*session)->id().value() != 0);
    CHECK_EQ((*session)->revision(), std::uint64_t{1});
    CHECK_EQ((*session)->info().pageCount, std::size_t{3});
    CHECK(!(*session)->info().isEncrypted);
    CHECK_EQ((*session)->path(), std::filesystem::path{"fake.pdf"});

    // Page ids are non-zero and pairwise distinct.
    const PageId id0 = (*session)->pageId(0);
    const PageId id1 = (*session)->pageId(1);
    const PageId id2 = (*session)->pageId(2);
    CHECK(id0.value() != 0);
    CHECK(id1.value() != 0);
    CHECK(id2.value() != 0);
    CHECK(id0 != id1);
    CHECK(id1 != id2);
    CHECK(id0 != id2);

    // Page sizes come from the backend (post-rotation display sizes).
    CHECK(Size::nearlyEqual((*session)->pageSizePoints(0), Size{200.0, 300.0}));
    CHECK(Size::nearlyEqual((*session)->pageSizePoints(1), Size{300.0, 200.0}));
    CHECK(Size::nearlyEqual((*session)->pageSizePoints(2), Size{612.0, 792.0}));

    // Layout: margin 24 on all sides, gap 16 between stacked pages.
    const auto& layout = (*session)->layout();
    CHECK_EQ(layout.pageCount(), std::size_t{3});
    CHECK_EQ(layout.pages()[0].id, id0);
    CHECK_EQ(layout.pages()[1].id, id1);
    CHECK_EQ(layout.pages()[2].id, id2);
    CHECK_NEAR(layout.pageGapPoints(), 16.0, 1e-9);
    CHECK_NEAR(layout.pageMarginPoints(), 24.0, 1e-9);
    // Widest page 612 + 2*24 = 660; heights 300 + 200 + 792 + 2*16 + 2*24 = 1372.
    CHECK(Size::nearlyEqual(layout.contentSizePoints(), Size{660.0, 1372.0}));

    // An engine without a working backend reports NotAvailable.
    UnavailablePdfEngine unavailable;
    const auto failed = DocumentSession::create(unavailable, scheduler, nullptr, std::filesystem::path{"fake.pdf"});
    CHECK(!failed.has_value());
    CHECK_EQ(failed.error().code, ErrorCode::NotAvailable);
}

RIVET_TEST(requestRenderDeliversBitmap) {
    TaskScheduler scheduler(2);
    FakePdfEngine engine;
    auto session = makeSession(engine, scheduler);
    FakePdfDocument* fake = engine.lastDocument;
    CHECK(fake != nullptr);

    auto& source = session->renderSource();
    const RenderRequest request = makeRequest(*session, 0);

    std::promise<Result<Bitmap>> delivered;
    auto deliveredFuture = delivered.get_future();
    source.requestRender(request, RenderPriority::Visible,
                         [&delivered](Result<Bitmap> result) { recordDelivery(delivered, std::move(result)); });

    CHECK(settled(deliveredFuture));
    const Result<Bitmap> result = deliveredFuture.get();
    CHECK(result.has_value());
    CHECK_EQ(result->width(), 200u);
    CHECK_EQ(result->height(), 300u);
    // The fake fills every byte with 0xAB: check the first and the very last.
    CHECK(result->data()[0] == std::byte{0xAB});
    CHECK(result->data()[result->stride() * (result->height() - 1) + result->width() * 4 - 1] ==
          std::byte{0xAB});
    CHECK_EQ(fake->renderCalls.load(), 1);
    CHECK_EQ(fake->lastPageIndex.load(), std::size_t{0});

    // The tile is now in the cache for the current revision...
    CHECK(source.cachedTile(request.key, session->revision()) != nullptr);

    // ...so a second identical request completes SYNCHRONOUSLY from the cache,
    // without another rasterization pass.
    std::atomic<bool> secondDelivered{false};
    Result<Bitmap> secondResult;
    source.requestRender(request, RenderPriority::Visible,
                         [&secondDelivered, &secondResult](Result<Bitmap> delivered2) {
                             secondResult = std::move(delivered2);
                             secondDelivered.store(true, std::memory_order_release);
                         });
    CHECK(secondDelivered.load(std::memory_order_acquire));
    CHECK(secondResult.has_value());
    CHECK_EQ(secondResult->width(), 200u);
    CHECK_EQ(secondResult->height(), 300u);
    CHECK_EQ(fake->renderCalls.load(), 1);
}

RIVET_TEST(renderFailureDeliversError) {
    TaskScheduler scheduler(2);
    FakePdfEngine engine;
    auto session = makeSession(engine, scheduler);
    FakePdfDocument* fake = engine.lastDocument;
    CHECK(fake != nullptr);

    fake->failingPageIndex = 1;
    auto& source = session->renderSource();
    const RenderRequest request = makeRequest(*session, 1);

    std::promise<Result<Bitmap>> delivered;
    auto deliveredFuture = delivered.get_future();
    source.requestRender(request, RenderPriority::Visible,
                         [&delivered](Result<Bitmap> result) { recordDelivery(delivered, std::move(result)); });

    CHECK(settled(deliveredFuture));
    const Result<Bitmap> result = deliveredFuture.get();
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::Io);
    CHECK_EQ(fake->renderCalls.load(), 1);

    // A failed render leaves nothing in the cache.
    CHECK_EQ(source.cachedTile(request.key, session->revision()), nullptr);
}

RIVET_TEST(cancelAllDeliversCancelled) {
    TaskScheduler scheduler(2);
    FakePdfEngine engine;
    auto session = makeSession(engine, scheduler);
    FakePdfDocument* fake = engine.lastDocument;
    CHECK(fake != nullptr);

    auto& source = session->renderSource();

    // Request A: goes in flight and blocks inside the fake's renderPage.
    fake->blockNextRender = true;
    const RenderRequest requestA = makeRequest(*session, 0, /*tileY=*/0);
    std::promise<Result<Bitmap>> deliveredA;
    auto futureA = deliveredA.get_future();
    source.requestRender(requestA, RenderPriority::Visible,
                         [&deliveredA](Result<Bitmap> result) { recordDelivery(deliveredA, std::move(result)); });
    auto entered = fake->renderEntered();
    CHECK(settled(entered)); // A's job is inside renderPage now
    CHECK_EQ(fake->renderCalls.load(), 1);

    // Request B (same page, different tile): queued behind blocked A.
    const RenderRequest requestB = makeRequest(*session, 0, /*tileY=*/1);
    std::promise<Result<Bitmap>> deliveredB;
    auto futureB = deliveredB.get_future();
    source.requestRender(requestB, RenderPriority::Visible,
                         [&deliveredB](Result<Bitmap> result) { recordDelivery(deliveredB, std::move(result)); });

    // cancelAll: queued B is dropped and its callback receives Cancelled.
    // With a null dispatcher that delivery happens inline, synchronously.
    source.cancelAll();
    CHECK(futureB.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const Result<Bitmap> resultB = futureB.get();
    CHECK(!resultB.has_value());
    CHECK_EQ(resultB.error().code, ErrorCode::Cancelled);

    // The in-flight A is not interrupted: releasing the fake lets it complete
    // and deliver its bitmap normally.
    fake->releaseRender();
    CHECK(settled(futureA));
    const Result<Bitmap> resultA = futureA.get();
    CHECK(resultA.has_value());
    CHECK_EQ(resultA->width(), 200u);
    CHECK_EQ(resultA->height(), 300u);

    // Exactly-once, both sides: A rendered once, B never reached the backend.
    CHECK_EQ(fake->renderCalls.load(), 1);
    CHECK(source.cachedTile(requestA.key, session->revision()) != nullptr);
    CHECK_EQ(source.cachedTile(requestB.key, session->revision()), nullptr);
}

RIVET_TEST(revisionPropagation) {
    TaskScheduler scheduler(2);
    FakePdfEngine engine;
    auto session = makeSession(engine, scheduler);
    FakePdfDocument* fake = engine.lastDocument;
    CHECK(fake != nullptr);

    auto& source = session->renderSource();
    const RenderRequest request = makeRequest(*session, 2);
    const std::uint64_t revisionX = session->revision();
    CHECK_EQ(revisionX, std::uint64_t{1});

    // Render at revision X.
    std::promise<Result<Bitmap>> delivered;
    auto deliveredFuture = delivered.get_future();
    source.requestRender(request, RenderPriority::Visible,
                         [&delivered](Result<Bitmap> result) { recordDelivery(delivered, std::move(result)); });
    CHECK(settled(deliveredFuture));
    CHECK(deliveredFuture.get().has_value());
    CHECK(source.cachedTile(request.key, revisionX) != nullptr);

    // "Edit" the document: the revision bumps and the tile rendered at X no
    // longer matches X + 1.
    session->markModified();
    CHECK_EQ(session->revision(), revisionX + 1);
    CHECK_EQ(source.cachedTile(request.key, revisionX + 1), nullptr);

    // A request after the edit re-renders under the new revision.
    const int callsBefore = fake->renderCalls.load();
    std::promise<Result<Bitmap>> delivered2;
    auto deliveredFuture2 = delivered2.get_future();
    source.requestRender(request, RenderPriority::Visible,
                         [&delivered2](Result<Bitmap> result) { recordDelivery(delivered2, std::move(result)); });
    CHECK(settled(deliveredFuture2));
    CHECK(deliveredFuture2.get().has_value());
    CHECK_EQ(fake->renderCalls.load(), callsBefore + 1);
    CHECK(source.cachedTile(request.key, revisionX + 1) != nullptr);
}
