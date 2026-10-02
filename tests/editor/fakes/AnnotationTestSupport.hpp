// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared fixture of the annotation tests: a DocumentSession over the fake
// page backend with a queueing main-thread dispatcher (so deliveries are
// pumped explicitly) and helpers to configure originals and wait for the
// service's lazy loads.

#include "FakePageDocument.hpp"
#include "FakeWritableEngine.hpp"

#include "RivetTest.h"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/AnnotationService.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace rivet::test {

class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    // Runs everything queued so far; returns how many tasks ran.
    std::size_t pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return run.size();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

// Pumps `dispatcher` until `done()` or the timeout (5 s).
template <typename Pred>
bool settle(QueueDispatcher& dispatcher, Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    dispatcher.pump();
    return done();
}

// An editable original at /Annots `index` (data is normalized).
inline pdf::PdfPageAnnotation makeOriginal(std::uint32_t index, pdf::PdfAnnotationData data,
                                           std::optional<std::uint32_t> popup = std::nullopt) {
    pdf::normalizeAnnotation(data);
    pdf::PdfPageAnnotation item;
    item.index = index;
    item.kind = data.kind;
    item.editable = true;
    item.popupIndex = popup;
    item.data = std::move(data);
    return item;
}

inline pdf::PdfPageAnnotation makePopupEntry(std::uint32_t index) {
    pdf::PdfPageAnnotation item;
    item.index = index;
    item.kind = pdf::PdfAnnotationKind::Other;
    item.isPopup = true;
    return item;
}

inline pdf::PdfPageAnnotation makeOpaque(std::uint32_t index) {
    pdf::PdfPageAnnotation item;
    item.index = index;
    item.kind = pdf::PdfAnnotationKind::Other;
    item.data.kind = pdf::PdfAnnotationKind::Other;
    item.data.rect = pdf::PdfBox{10, 10, 60, 40};
    return item;
}

inline pdf::PdfAnnotationData squareAt(double left, double bottom, double right, double top, float width = 2.0F) {
    pdf::PdfAnnotationData d;
    d.kind = pdf::PdfAnnotationKind::Square;
    d.rect = pdf::PdfBox{left, bottom, right, top};
    d.borderWidth = width;
    d.contents = "sq";
    return d;
}

inline pdf::PdfAnnotationData highlightAt(double left, double bottom, double right, double top) {
    pdf::PdfAnnotationData d;
    d.kind = pdf::PdfAnnotationKind::Highlight;
    d.quads = {pdf::PdfQuad{{left, top}, {right, top}, {left, bottom}, {right, bottom}}};
    return d;
}

inline pdf::PdfAnnotationData noteAt(double left, double bottom) {
    pdf::PdfAnnotationData d;
    d.kind = pdf::PdfAnnotationKind::Note;
    d.rect = pdf::PdfBox{left, bottom, left + 20.0, bottom + 20.0};
    d.contents = "note";
    return d;
}

template <typename Engine>
struct BasicAnnotationFixture {
    template <typename... Args>
    explicit BasicAnnotationFixture(const std::filesystem::path& path, Args&&... args)
        : engine(std::forward<Args>(args)...) {
        auto created = editor::DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        session = std::move(*created);
        // The base document is a FakePageDocument in both engines used here.
        document = dynamic_cast<FakePageDocument*>(session->documentPtr().get());
        CHECK(document != nullptr);
        for (std::size_t i = 0; i < session->pageCount(); ++i) ids.push_back(session->pageId(i));
        session->setOnAnnotationsChanged([this](core::PageId page) {
            changes.push_back(page);
            loaded.insert(page);
        });
    }

    core::PageId id(std::size_t index) const { return ids.at(index); }

    // Requests the page's originals and waits for them.
    bool load(core::PageId page) {
        if (loaded.count(page) != 0) return true;
        session->annotations().annotations(page);
        return settle(dispatcher, [&] { return loaded.count(page) != 0; });
    }
    bool load(std::size_t index) { return load(id(index)); }

    editor::AnnotationService::Resolved list(core::PageId page) { return session->annotations().annotations(page); }
    editor::AnnotationService::Resolved list(std::size_t index) { return list(id(index)); }

    // Executes an edit; false on any error.
    bool run(core::Result<editor::AnnotationEdit> edit) {
        if (!edit.has_value()) return false;
        return session->execute(std::move(edit->command)).has_value();
    }

    const editor::PageEntry& entry(std::size_t index) const { return *session->pageSnapshot()->find(id(index)); }

    Engine engine;
    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<editor::DocumentSession> session;
    FakePageDocument* document = nullptr;
    std::vector<core::PageId> ids;
    std::vector<core::PageId> changes;
    std::set<core::PageId> loaded;
};

struct AnnotationFixture : BasicAnnotationFixture<FakePageEngine> {
    explicit AnnotationFixture(std::size_t pages = 3) : BasicAnnotationFixture("fake.pdf", pages) {}
};

} // namespace rivet::test
