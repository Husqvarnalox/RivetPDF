#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "editor/DocumentRenderer.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfTypes.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderSource.hpp"
#include "render/TileCache.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::DocumentId;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::editor::kInvalidPageIndex;
using rivet::core::PageId;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::SerialExecutor;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentRenderer;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfPageInfo;
using rivet::render::PhysicalRenderScaleKey;
using rivet::render::RenderPriority;
using rivet::render::RenderRequest;
using rivet::render::RenderResult;

namespace {

// In-memory PdfDocument: 2 pages {200,300} and {300,200}; renderPage produces
// a bitmap of the requested pixel size filled with 0xAB, can fail per page
// index and can block once so tests can hold a render in flight.
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
    std::vector<Size> pageSizes_{Size{200.0, 300.0}, Size{300.0, 200.0}};

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

// A DocumentRenderer over its own cache + executor. Declaration order = reverse
// destruction order: the renderer dies first (see its lifetime contract).
class RendererHarness {
public:
    static constexpr DocumentId kDocumentId{42};

    RendererHarness()
        : renderer_(kDocumentId, document_,
                    [this](PageId id) {
                        const auto it = pageIndex_.find(id);
                        return it == pageIndex_.end() ? kInvalidPageIndex : it->second;
                    },
                    cache_, scheduler_, executor_, /*mainDispatcher=*/nullptr) {}

    FakePdfDocument& document() { return document_; }
    DocumentRenderer& renderer() { return renderer_; }

    PageId pageId(std::size_t index) const { return pageIds_[index]; }

private:
    TaskScheduler scheduler_{2};
    FakePdfDocument document_;
    rivet::render::TileCache cache_;
    SerialExecutor executor_{scheduler_};
    std::unordered_map<PageId, std::size_t> pageIndex_{
        {PageId{101}, 0}, {PageId{102}, 1}, {PageId{103}, 2}};
    std::vector<PageId> pageIds_{PageId{101}, PageId{102}, PageId{103}};
    DocumentRenderer renderer_;
};

RenderRequest makeRequest(const RendererHarness& harness, std::size_t pageIndex, std::uint32_t tileX = 0,
                          std::uint32_t tileY = 0) {
    RenderRequest request;
    request.key.documentId = RendererHarness::kDocumentId;
    request.key.pageId = harness.pageId(pageIndex);
    request.key.scale = PhysicalRenderScaleKey::fromDensities(1.0, 1.0); // zoom 1 on a 1x display
    request.key.tileX = tileX;
    request.key.tileY = tileY;
    request.params.pageRectPoints = Rect{0.0, 0.0, 200.0, 300.0};
    // MUST derive from the key: DocumentRenderer rejects mismatches.
    request.params.devicePixelsPerPoint = request.key.scale.scale();
    return request;
}

// Posts a request and hands back a future for its (exactly one) delivery.
std::future<RenderResult> requestAsync(DocumentRenderer& renderer, const RenderRequest& request) {
    auto promise = std::make_shared<std::promise<RenderResult>>();
    auto future = promise->get_future();
    renderer.requestRender(request, RenderPriority::Visible,
                           [promise](RenderResult result) { promise->set_value(std::move(result)); });
    return future;
}

// Bounded wait: correctness is asserted through the future's payload; this
// only turns a hung delivery into a test failure instead of a hang.
template <typename Future>
bool settled(const Future& future, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    return future.wait_for(timeout) == std::future_status::ready;
}

} // namespace

RIVET_TEST(failedTileIsNotScheduledAgain) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();
    fake.failingPageIndex = 1;

    const RenderRequest request = makeRequest(h, 1);
    auto future = requestAsync(h.renderer(), request);
    CHECK(settled(future));
    const RenderResult first = future.get();
    CHECK(!first.has_value());
    CHECK_EQ(first.error().code, ErrorCode::Io);
    CHECK_EQ(fake.renderCalls.load(), 1);

    // A repaint re-requests the same tile: the recorded failure is delivered
    // inline (requestAsync's future is already ready on return) WITHOUT
    // reaching the backend again.
    auto future2 = requestAsync(h.renderer(), request);
    CHECK(future2.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult second = future2.get();
    CHECK(!second.has_value());
    CHECK_EQ(second.error().code, ErrorCode::Io);
    CHECK_EQ(fake.renderCalls.load(), 1); // no second rasterization
}

RIVET_TEST(setRevisionReschedulesFailedTile) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();
    fake.failingPageIndex = 1;

    const RenderRequest request = makeRequest(h, 1);
    auto future = requestAsync(h.renderer(), request);
    CHECK(settled(future));
    CHECK(!future.get().has_value());
    CHECK_EQ(fake.renderCalls.load(), 1);

    // A revision change is a meaningful state change: the failed tile is
    // scheduled again under the new revision (and fails again here).
    h.renderer().setRevision(2);
    auto future2 = requestAsync(h.renderer(), request);
    CHECK(settled(future2));
    const RenderResult result = future2.get();
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::Io);
    CHECK_EQ(fake.renderCalls.load(), 2);
}

RIVET_TEST(retryFailedTilesReschedulesAtSameRevision) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();
    fake.failingPageIndex = 1;

    const RenderRequest request = makeRequest(h, 1);
    auto future = requestAsync(h.renderer(), request);
    CHECK(settled(future));
    CHECK(!future.get().has_value());
    CHECK_EQ(fake.renderCalls.load(), 1);

    // Same revision: still replayed inline, no scheduling.
    auto replayed = requestAsync(h.renderer(), request);
    CHECK(replayed.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK(!replayed.get().has_value());
    CHECK_EQ(fake.renderCalls.load(), 1);

    // An explicit retry re-arms the tile at the SAME revision.
    h.renderer().retryFailedTiles();
    auto future2 = requestAsync(h.renderer(), request);
    CHECK(settled(future2));
    CHECK(!future2.get().has_value()); // still failing, but freshly rendered
    CHECK_EQ(fake.renderCalls.load(), 2);

    // retryFailedTiles with nothing recorded is a harmless no-op.
    h.renderer().retryFailedTiles();
}

RIVET_TEST(cancelledRequestsAreNotRecordedAsFailures) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();

    // Request A goes in flight and blocks inside the fake.
    fake.blockNextRender = true;
    const RenderRequest requestA = makeRequest(h, 0, 0, 0);
    auto futureA = requestAsync(h.renderer(), requestA);
    auto entered = fake.renderEntered();
    CHECK(settled(entered));
    CHECK_EQ(fake.renderCalls.load(), 1);

    // Request B is queued behind the blocked A, then cancelled.
    const RenderRequest requestB = makeRequest(h, 0, 0, 1);
    auto futureB = requestAsync(h.renderer(), requestB);
    h.renderer().cancelAll();
    CHECK(futureB.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult cancelled = futureB.get();
    CHECK(!cancelled.has_value());
    CHECK_EQ(cancelled.error().code, ErrorCode::Cancelled);

    // A cancellation leaves no failure record: re-requesting B schedules
    // fresh work that renders successfully once A unblocks the stream.
    fake.releaseRender();
    auto futureB2 = requestAsync(h.renderer(), requestB);
    CHECK(settled(futureB2));
    const RenderResult rendered = futureB2.get();
    CHECK(rendered.has_value());
    CHECK_EQ(fake.renderCalls.load(), 2);
    CHECK(h.renderer().cachedTile(requestB.key, h.renderer().revision()) != nullptr);

    // A completed normally as well.
    CHECK(settled(futureA));
    CHECK(futureA.get().has_value());
}

RIVET_TEST(successAfterFailureClearsTheRecord) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();
    fake.failingPageIndex = 0;

    const RenderRequest request = makeRequest(h, 0);
    auto future = requestAsync(h.renderer(), request);
    CHECK(settled(future));
    CHECK(!future.get().has_value());
    CHECK_EQ(fake.renderCalls.load(), 1);

    // Explicit retry, this time with the backend "fixed".
    h.renderer().retryFailedTiles();
    fake.failingPageIndex = static_cast<std::size_t>(-1);

    auto future2 = requestAsync(h.renderer(), request);
    CHECK(settled(future2));
    const RenderResult rendered = future2.get();
    CHECK(rendered.has_value());
    const std::shared_ptr<const Bitmap> tile = *rendered;
    CHECK_EQ(fake.renderCalls.load(), 2);

    // The success cleared the record: the next request goes through the
    // cache's hit path - same shared bitmap, no rasterization, no replay.
    auto future3 = requestAsync(h.renderer(), request);
    CHECK(future3.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult cached = future3.get();
    CHECK(cached.has_value());
    CHECK_EQ((*cached).get(), tile.get());
    CHECK_EQ(fake.renderCalls.load(), 2);
}

RIVET_TEST(invalidRequestsAreRejectedInline) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();

    // Wrong document id: a wiring bug. Rejected synchronously, no work.
    RenderRequest foreign = makeRequest(h, 0);
    foreign.key.documentId = DocumentId{999};
    auto foreignFuture = requestAsync(h.renderer(), foreign);
    CHECK(foreignFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult foreignResult = foreignFuture.get();
    CHECK(!foreignResult.has_value());
    CHECK_EQ(foreignResult.error().code, ErrorCode::InvalidArgument);

    // RasterParams not derived from the key: the produced pixel dimensions
    // could disagree with the cache identity. Rejected synchronously, no work.
    RenderRequest mismatched = makeRequest(h, 0);
    mismatched.params.devicePixelsPerPoint = 1.5;
    auto mismatchedFuture = requestAsync(h.renderer(), mismatched);
    CHECK(mismatchedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult mismatchResult = mismatchedFuture.get();
    CHECK(!mismatchResult.has_value());
    CHECK_EQ(mismatchResult.error().code, ErrorCode::InvalidArgument);

    CHECK_EQ(fake.renderCalls.load(), 0);
}

RIVET_TEST(coalescedRequestsShareOneBitmap) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();

    // Request 1 goes in flight and blocks; request 2 (same tile + revision)
    // coalesces onto the same pending entry.
    fake.blockNextRender = true;
    const RenderRequest request = makeRequest(h, 0);
    auto future1 = requestAsync(h.renderer(), request);
    auto entered = fake.renderEntered();
    CHECK(settled(entered));
    auto future2 = requestAsync(h.renderer(), request);

    fake.releaseRender();
    CHECK(settled(future1));
    CHECK(settled(future2));

    const RenderResult result1 = future1.get();
    const RenderResult result2 = future2.get();
    CHECK(result1.has_value());
    CHECK(result2.has_value());
    // Exactly the same Bitmap instance: deliveries share the cache's ownerhip
    // instead of deep-copying per callback.
    CHECK_EQ((*result1).get(), (*result2).get());
    CHECK_EQ(fake.renderCalls.load(), 1);
}

RIVET_TEST(cacheHitDeliversTheCacheSharedBitmap) {
    RendererHarness h;
    FakePdfDocument& fake = h.document();

    const RenderRequest request = makeRequest(h, 0);
    auto future = requestAsync(h.renderer(), request);
    CHECK(settled(future));
    const RenderResult rendered = future.get();
    CHECK(rendered.has_value());
    const std::shared_ptr<const Bitmap> delivered = *rendered;
    CHECK_EQ(fake.renderCalls.load(), 1);

    // The cache hands out the same instance that was delivered...
    const std::shared_ptr<const Bitmap> probed = h.renderer().cachedTile(request.key, 1);
    CHECK(probed != nullptr);
    CHECK_EQ(probed.get(), delivered.get());

    // ...and so does the synchronous requestRender fast path.
    auto future2 = requestAsync(h.renderer(), request);
    CHECK(future2.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RenderResult hit = future2.get();
    CHECK(hit.has_value());
    CHECK_EQ((*hit).get(), delivered.get());
    CHECK_EQ(fake.renderCalls.load(), 1);
}
