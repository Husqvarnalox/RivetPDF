// SPDX-License-Identifier: MPL-2.0
// Concurrency stress of the annotation pipeline (Phase 4 hardening): lazy
// loads racing the death of their session, renders with hidden annotations
// racing annotation reads and saves of the same PDFium document, stale load
// completions of destroyed documents (ABA), and immutable page-model
// snapshots read by worker threads while the main thread keeps executing,
// undoing and redoing annotation commands. The portable parts run on the
// fake backends (and under ThreadSanitizer); the PDFium parts return early
// when no PDF backend is built in.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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

fs::path fixture(const char* name) {
    return fs::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / name;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-annot-conc-" + std::to_string(::getpid()) + "-" +
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

// A queueing main-thread dispatcher that also counts what was posted, so a
// test can tell that a worker finished its job (the delivery was queued)
// without pumping it.
class CountingDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        ++posted_;
    }
    std::size_t pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return run.size();
    }
    std::size_t posted() const { return posted_.load(); }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    std::atomic<std::size_t> posted_{0};
};

template <typename Pred>
bool waitUntil(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

template <typename Pred>
bool settleCounting(CountingDispatcher& dispatcher, Pred done) {
    return waitUntil([&] {
        dispatcher.pump();
        return done();
    });
}

std::vector<pdf::PdfPageAnnotation> someOriginals() {
    return {makeOriginal(0, highlightAt(100, 500, 200, 515), 1), makePopupEntry(1),
            makeOriginal(2, squareAt(300, 300, 380, 360)), makeOpaque(3), makeOriginal(4, noteAt(50, 600))};
}

AnnotationDraft squareDraft(core::PageId page, double x) {
    AnnotationDraft d;
    d.page = page;
    d.kind = Kind::Square;
    d.rect = core::Rect{x, 100, 40, 30};
    d.style.color = pdf::PdfColor{0.2F, 0.4F, 0.8F};
    d.style.opacity = 1.0F;
    d.style.borderWidth = 2.0F;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Loads racing the death of the session
// ---------------------------------------------------------------------------

// Many pages requested at once, the session destroyed at a different point of
// the pipeline each round (before any job ran, mid-flight, after partial
// delivery). No delivery may reach the callback of a dead session, the
// destructor must not deadlock, and nothing may touch freed memory.
RIVET_TEST(annotationConcurrencyLoadsWhileSessionIsDestroyed) {
    for (int round = 0; round < 24; ++round) {
        FakePageEngine engine(16);
        core::TaskScheduler scheduler{3};
        QueueDispatcher dispatcher;
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, "fake.pdf");
        CHECK(created.has_value());
        if (!created.has_value()) return;
        std::unique_ptr<DocumentSession> session = std::move(*created);
        auto* document = dynamic_cast<FakePageDocument*>(session->documentPtr().get());
        CHECK(document != nullptr);
        if (document == nullptr) return;
        for (std::size_t i = 0; i < session->pageCount(); ++i) document->setAnnotations(i, someOriginals());

        int fired = 0;
        int late = 0;
        bool dead = false;
        session->setOnAnnotationsChanged([&](core::PageId) {
            if (dead) ++late;
            ++fired;
        });
        for (std::size_t i = 0; i < session->pageCount(); ++i) session->annotations().annotations(session->pageId(i));

        // Round 0..3: destroy at once; later rounds let some deliveries land first.
        for (int step = 0; step < round % 5; ++step) {
            std::this_thread::sleep_for(std::chrono::microseconds(150 * (round % 3 + 1)));
            dispatcher.pump();
        }
        const int firedBefore = fired;
        dead = true;
        session.reset();
        const int atDeath = fired;
        CHECK_EQ(atDeath, firedBefore);
        dispatcher.pump(); // everything still queued must be inert
        dispatcher.pump();
        CHECK_EQ(late, 0);
        CHECK_EQ(fired, atDeath);
    }
}

// Without a main dispatcher the completion runs inline on the worker: only
// the mutex-guarded cache is touched, concurrently with main-thread reads.
RIVET_TEST(annotationConcurrencyInlineCompletionRacesCacheReadsAndDestruction) {
    for (int round = 0; round < 20; ++round) {
        FakePageEngine engine(12);
        core::TaskScheduler scheduler{3};
        auto created = DocumentSession::create(engine, scheduler, nullptr, "fake.pdf");
        CHECK(created.has_value());
        if (!created.has_value()) return;
        std::unique_ptr<DocumentSession> session = std::move(*created);
        auto* document = dynamic_cast<FakePageDocument*>(session->documentPtr().get());
        CHECK(document != nullptr);
        if (document == nullptr) return;
        for (std::size_t i = 0; i < session->pageCount(); ++i) document->setAnnotations(i, someOriginals());
        for (std::size_t i = 0; i < session->pageCount(); ++i) session->annotations().annotations(session->pageId(i));
        std::size_t seen = 0;
        for (int k = 0; k < round % 6; ++k) {
            seen = std::max(seen, session->annotations().cachedOriginalPages());
            std::this_thread::yield();
        }
        CHECK(seen <= session->pageCount());
        if (round % 2 == 0) {
            // Let it finish, then the resolved view must be complete.
            CHECK(waitUntil([&] { return session->annotations().cachedOriginalPages() == session->pageCount(); }));
            for (std::size_t i = 0; i < session->pageCount(); ++i) {
                CHECK_EQ(session->annotations().annotations(session->pageId(i))->size(), std::size_t{3});
            }
        }
        session.reset(); // mid-flight on the odd rounds
    }
}

RIVET_TEST(annotationConcurrencyPdfiumLoadsWhileSessionIsDestroyed) {
    auto engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) return;
    core::TaskScheduler scheduler{2};
    for (int round = 0; round < 10; ++round) {
        QueueDispatcher dispatcher;
        auto created = DocumentSession::create(*engine, scheduler, &dispatcher, fixture("annots.pdf"));
        CHECK(created.has_value());
        if (!created.has_value()) return;
        std::unique_ptr<DocumentSession> session = std::move(*created);
        int fired = 0;
        int late = 0;
        bool dead = false;
        session->setOnAnnotationsChanged([&](core::PageId) {
            if (dead) ++late;
            ++fired;
        });
        for (std::size_t i = 0; i < session->pageCount(); ++i) session->annotations().annotations(session->pageId(i));
        if (round % 2 == 1) {
            // Let the first page land, so the destruction hits the rest.
            settle(dispatcher, [&] { return fired > 0; });
        }
        const int firedBefore = fired;
        dead = true;
        session.reset();
        CHECK_EQ(fired, firedBefore);
        dispatcher.pump();
        CHECK_EQ(late, 0);
        CHECK_EQ(fired, firedBefore);
    }
}

// ---------------------------------------------------------------------------
// Stale completions (ABA)
// ---------------------------------------------------------------------------

// A load whose document is destroyed before its delivery is pumped must not
// populate the cache; a NEW document (possibly at the same address) loaded in
// the meantime must show ITS originals, never the old ones.
RIVET_TEST(annotationConcurrencyStaleCompletionNeverPopulatesANewDocument) {
    FakePageEngine engine(3);
    core::TaskScheduler scheduler{2};
    CountingDispatcher dispatcher;
    auto created = DocumentSession::create(engine, scheduler, &dispatcher, "fake.pdf");
    CHECK(created.has_value());
    if (!created.has_value()) return;
    std::unique_ptr<DocumentSession> session = std::move(*created);
    std::vector<core::PageId> changed;
    session->setOnAnnotationsChanged([&](core::PageId page) { changed.push_back(page); });
    auto& model = session->pageModel();

    // Adds a one-page document with `originals` at the end; returns its page.
    const auto insertDocument = [&](std::shared_ptr<FakePageDocument> document) {
        std::shared_ptr<pdf::PdfDocument> base = document;
        document.reset();
        auto sources = PageModel::describeAllPages(base);
        CHECK(sources.has_value());
        base.reset();
        if (!sources.has_value()) return core::PageId{};
        CHECK(session->execute(std::make_unique<InsertPagesCommand>(model, *sources, session->pageCount()))
                  .has_value());
        return session->pageId(session->pageCount() - 1);
    };
    // Undo the insert and run a fresh command so the redo entry (the last
    // owner besides the model) is dropped: the document dies.
    const auto dropLastInsert = [&] {
        CHECK(session->undo());
        CHECK(session->execute(std::make_unique<RotatePagesCommand>(model, std::vector{session->pageId(0)}, 90))
                  .has_value());
    };

    std::size_t sameAddress = 0;
    for (int round = 0; round < 12; ++round) {
        auto oldDocument = std::make_shared<FakePageDocument>(1, "OLD-");
        oldDocument->setAnnotations(0, {makeOriginal(0, squareAt(10, 10, 50, 50))});
        const std::weak_ptr<pdf::PdfDocument> oldWeak = oldDocument;
        const void* const oldAddress = oldDocument.get();
        const core::PageId oldPage = insertDocument(std::move(oldDocument));
        CHECK(oldPage);

        const std::size_t postedBefore = dispatcher.posted();
        changed.clear();
        session->annotations().annotations(oldPage);
        // The worker finished (its delivery is queued) but nothing is pumped.
        CHECK(waitUntil([&] { return dispatcher.posted() > postedBefore; }));
        dropLastInsert();
        CHECK(oldWeak.expired());

        const std::size_t cachedBefore = session->annotations().cachedOriginalPages();
        if (round % 2 == 0) {
            // The stale delivery meets no new request: it must be dropped.
            dispatcher.pump();
            CHECK(changed.empty());
            CHECK_EQ(session->annotations().cachedOriginalPages(), cachedBefore);
        }

        auto newDocument = std::make_shared<FakePageDocument>(1, "NEW-");
        newDocument->setAnnotations(0, {makeOriginal(0, noteAt(30, 30))});
        if (static_cast<const void*>(newDocument.get()) == oldAddress) ++sameAddress;
        const core::PageId newPage = insertDocument(std::move(newDocument));
        CHECK(newPage);
        CHECK(newPage != oldPage);
        // Odd rounds: the new request overtakes the unpumped old delivery.
        session->annotations().annotations(newPage);
        CHECK(settleCounting(dispatcher, [&] { return !changed.empty(); }));
        dispatcher.pump();
        CHECK_EQ(changed.size(), std::size_t{1});
        if (!changed.empty()) CHECK(changed[0] == newPage);

        const auto resolved = session->annotations().annotations(newPage);
        CHECK(resolved != nullptr);
        if (resolved != nullptr) {
            CHECK_EQ(resolved->size(), std::size_t{1});
            if (!resolved->empty()) CHECK((*resolved)[0].kind == Kind::Note); // the NEW document's, not the square
        }
        dropLastInsert();
    }
    // (Whether the allocator reused an address is irrelevant to correctness;
    // it only makes the guard load-bearing.)
    CHECK(sameAddress <= 12);
}

// ---------------------------------------------------------------------------
// Snapshots read by workers while the main thread edits
// ---------------------------------------------------------------------------

namespace {

std::uint64_t digestOf(const PageModelSnapshot& snapshot) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    const auto mixDouble = [&](double v) { mix(std::bit_cast<std::uint64_t>(v)); };
    mix(snapshot.size());
    mix(snapshot.orderRevision());
    mix(snapshot.documentRevision());
    for (const PageEntry& entry : snapshot.entries()) {
        mix(entry.id.value());
        mix(entry.contentRevision);
        mix(entry.rasterRevision);
        if (entry.annotations == nullptr) {
            mix(0x5A5A);
            continue;
        }
        mix(entry.annotations->suppressed.size());
        for (const std::uint32_t index : entry.annotations->suppressed) mix(index);
        mix(entry.annotations->overlay.size());
        for (const OverlayAnnotation& overlay : entry.annotations->overlay) {
            mix(overlay.id.value());
            mixDouble(overlay.data.rect.left);
            mixDouble(overlay.data.rect.bottom);
            mix(overlay.fileIndex.value_or(0xFFFFFFFFu));
        }
    }
    // The save request derived from the snapshot (what a worker would use).
    auto request = snapshot.toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    if (!request.has_value()) {
        mix(0xDEAD);
        return h;
    }
    mix(request->pages.size());
    for (const pdf::PdfAssemblyPage& page : request->pages) {
        mix(page.sourcePageIndex);
        if (page.annotationEdits == nullptr) {
            mix(0xA5A5);
            continue;
        }
        mix(page.annotationEdits->removeIndices.size());
        mix(page.annotationEdits->create.size());
    }
    return h;
}

struct CapturedSnapshot {
    PageSnapshotPtr snapshot;
    std::uint64_t digest = 0;
};

// Worker threads that re-verify captured snapshots and exercise the way a
// render job reads one (source document, view, hidden indices).
class SnapshotReaders {
public:
    explicit SnapshotReaders(std::size_t threads) {
        for (std::size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
    }
    ~SnapshotReaders() { stop(); }

    void submit(CapturedSnapshot captured) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(captured));
        }
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        for (std::thread& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
        threads_.clear();
    }
    int failures() const { return failures_.load(); }
    int verified() const { return verified_.load(); }

private:
    void loop() {
        for (;;) {
            CapturedSnapshot job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return done_ || !queue_.empty(); });
                if (queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            for (int pass = 0; pass < 3; ++pass) {
                if (digestOf(*job.snapshot) != job.digest) ++failures_;
                std::this_thread::yield();
            }
            for (const PageEntry& entry : job.snapshot->entries()) {
                static const std::vector<std::uint32_t> none;
                const std::vector<std::uint32_t>& hidden = entry.annotations ? entry.annotations->suppressed : none;
                const auto bitmap = entry.source->renderPage(entry.sourcePageIndex, entry.view, hidden,
                                                             core::Rect{0, 0, 40, 40}, 0.25);
                if (!bitmap.has_value()) ++failures_;
            }
            ++verified_;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CapturedSnapshot> queue_;
    bool done_ = false;
    std::vector<std::thread> threads_;
    std::atomic<int> failures_{0};
    std::atomic<int> verified_{0};
};

} // namespace

RIVET_TEST(annotationConcurrencyCommandsRaceSnapshotReadersAndSaves) {
    TempDir dir;
    BasicAnnotationFixture<FakeWritableEngine> f("doc.pdf");
    f.document->setAnnotations(0, someOriginals());
    CHECK(f.load(0));

    SnapshotReaders readers(3);
    std::deque<std::unique_ptr<DocumentWriteJob>> jobs;
    std::vector<std::thread> savers;
    std::atomic<int> saveFailures{0};
    std::atomic<int> saved{0};

    int created = 0;
    const auto capture = [&] {
        CapturedSnapshot captured;
        captured.snapshot = f.session->pageSnapshot();
        captured.digest = digestOf(*captured.snapshot);
        readers.submit(std::move(captured));
    };
    capture();
    for (int i = 0; i < 160; ++i) {
        switch (i % 8) {
        case 0:
        case 1:
            if (f.run(createAnnotations(*f.session, {squareDraft(f.id(i % 2), 20.0 + i)}))) ++created;
            break;
        case 2: {
            const auto list = f.list(0);
            if (!list->empty()) f.run(moveAnnotation(*f.session, list->back().id, core::Point{3, 4}));
            break;
        }
        case 3: {
            const auto list = f.list(0);
            if (!list->empty()) f.run(deleteAnnotations(*f.session, {list->front().id}));
            break;
        }
        case 4:
        case 5:
            f.session->undo();
            break;
        case 6:
            f.session->redo();
            break;
        default:
            f.session->undo();
            f.session->redo();
            break;
        }
        f.dispatcher.pump();
        capture();
        if (i % 32 == 31 && jobs.size() < 5) {
            // A real save of the current state, run on a worker while the
            // main thread keeps mutating the model.
            const std::string name = "save-" + std::to_string(i) + ".pdf";
            auto job = makeSaveJob(*f.session, dir / name.c_str());
            CHECK(job.has_value());
            if (job.has_value()) {
                jobs.push_back(std::make_unique<DocumentWriteJob>(std::move(*job)));
                const DocumentWriteJob* raw = jobs.back().get();
                savers.emplace_back([&, raw] {
                    const auto result = runDocumentWrite(f.engine, *raw);
                    if (!result.written.has_value()) ++saveFailures;
                    ++saved;
                });
            }
        }
    }
    for (std::thread& saver : savers) saver.join();
    readers.stop();
    CHECK_GT(created, 10);
    CHECK_EQ(readers.failures(), 0);
    CHECK_EQ(readers.verified(), 161);
    CHECK_EQ(saveFailures.load(), 0);
    CHECK_EQ(saved.load(), static_cast<int>(jobs.size()));
    CHECK_GT(saved.load(), 0);
}

// ---------------------------------------------------------------------------
// PDFium: renders with hidden annotations, annotation reads and saves of one
// document, all through the process-wide call gate
// ---------------------------------------------------------------------------

namespace {

double inkRatio(const core::Bitmap& bitmap, std::uint32_t px, std::uint32_t py, std::uint32_t pw, std::uint32_t ph) {
    if (px >= bitmap.width() || py >= bitmap.height()) return 0.0;
    const std::uint32_t w = std::min(pw, bitmap.width() - px);
    const std::uint32_t h = std::min(ph, bitmap.height() - py);
    const auto* base = reinterpret_cast<const std::uint8_t*>(bitmap.data());
    std::size_t ink = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* row = base + (static_cast<std::size_t>(py) + y) * bitmap.stride() + static_cast<std::size_t>(px) * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            if (row[x * 4] < 250 || row[x * 4 + 1] < 250 || row[x * 4 + 2] < 250) ++ink;
        }
    }
    return w * h == 0 ? 0.0 : static_cast<double>(ink) / static_cast<double>(w * h);
}

// Highlight (annots.pdf index 0) at display (110,75,180,14); Square (index 6)
// at (310,240,80,40).
constexpr double kInk = 0.9;

} // namespace

RIVET_TEST(annotationConcurrencyHiddenRendersReadsAndSavesShareTheCallGate) {
    auto engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) return;
    auto opened = engine->openDocument(fixture("annots.pdf"), {});
    CHECK(opened.has_value());
    if (!opened.has_value()) return;
    const std::shared_ptr<pdf::PdfDocument> document = std::move(*opened);
    const auto info = document->pageInfo(0);
    CHECK(info.has_value());
    if (!info.has_value()) return;
    const pdf::PdfPageView view = info->view;
    const core::Rect all{0, 0, 612, 792};
    TempDir dir;

    std::atomic<int> failures{0};
    std::atomic<int> work{0};
    const auto renderLoop = [&](std::vector<std::uint32_t> hidden, bool expectHighlight, bool expectSquare,
                                int iterations) {
        for (int i = 0; i < iterations; ++i) {
            const auto bitmap = document->renderPage(0, view, hidden, all, 1.0);
            if (!bitmap.has_value()) {
                ++failures;
                continue;
            }
            const bool highlight = inkRatio(*bitmap, 110, 75, 180, 14) > kInk;
            const bool square = inkRatio(*bitmap, 310, 240, 80, 40) > kInk;
            if (highlight != expectHighlight || square != expectSquare) ++failures;
            ++work;
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back([&] { renderLoop({0}, false, true, 8); });
    threads.emplace_back([&] { renderLoop({6}, true, false, 8); });
    threads.emplace_back([&] { renderLoop({}, true, true, 8); });
    threads.emplace_back([&] {
        for (int i = 0; i < 40; ++i) {
            const auto read = document->annotations(0);
            if (!read.has_value() || (*read)->annotsCount != 12 || (*read)->items.size() != 12) {
                ++failures;
                continue;
            }
            // The persistent /F of the annotations the renders hide never changes.
            if ((*read)->items[0].flags != 4u || (*read)->items[6].flags != 4u) ++failures;
            ++work;
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < 4; ++i) {
            pdf::PdfAssemblyRequest request;
            request.mode = pdf::PdfAssemblyRequest::Mode::PreserveBase;
            request.base = document.get();
            std::shared_ptr<pdf::PdfPageAnnotationEdits> edits;
            if (i % 2 == 1) {
                edits = std::make_shared<pdf::PdfPageAnnotationEdits>();
                edits->removeIndices = {0};
            }
            for (std::size_t page = 0; page < document->info().pageCount; ++page) {
                const auto pageInfo = document->pageInfo(page);
                if (!pageInfo.has_value()) {
                    ++failures;
                    return;
                }
                request.pages.push_back(pdf::PdfAssemblyPage{document.get(), page, pageInfo->view,
                                                             page == 0 ? edits : nullptr});
            }
            class Sink final : public pdf::IPdfByteSink {
            public:
                core::Status write(const void* data, std::size_t size) override {
                    const auto* bytes = static_cast<const char*>(data);
                    bytes_.append(bytes, size);
                    return core::ok();
                }
                std::string bytes_;
            } sink;
            if (!engine->assembleDocument(request, sink).has_value()) {
                ++failures;
                continue;
            }
            const fs::path out = dir / (i % 2 == 1 ? "removed.pdf" : "same.pdf");
            {
                std::ofstream file(out, std::ios::binary | std::ios::trunc);
                file.write(sink.bytes_.data(), static_cast<std::streamsize>(sink.bytes_.size()));
            }
            auto reopened = engine->openDocument(out, {});
            if (!reopened.has_value()) {
                ++failures;
                continue;
            }
            const auto page0 = (*reopened)->annotations(0);
            if (!page0.has_value() || (*page0)->annotsCount != (i % 2 == 1 ? 11u : 12u)) ++failures;
            ++work;
        }
    });
    for (std::thread& thread : threads) thread.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_GT(work.load(), 40);

    // Hidden flags were restored: a render without hidden indices shows both
    // annotations again, and the persistent state is untouched.
    const auto after = document->renderPage(0, view, std::span<const std::uint32_t>{}, all, 1.0);
    CHECK(after.has_value());
    if (after.has_value()) {
        CHECK_GT(inkRatio(*after, 110, 75, 180, 14), kInk);
        CHECK_GT(inkRatio(*after, 310, 240, 80, 40), kInk);
    }
    const auto read = document->annotations(0);
    CHECK(read.has_value());
    if (read.has_value() && (*read)->items.size() == 12) {
        CHECK_EQ((*read)->items[0].flags, 4u);
        CHECK_EQ((*read)->items[6].flags, 4u);
    }
}
