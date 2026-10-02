// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared fixtures of the content-tool tests: a fake ContentBackend serving
// hand-made objects / text blocks with simple undoable commands, a fake image
// decoder / file dialog and a shell-less ContentShell (real DocumentWorkspace
// tab over the fake page engine, real PdfViewport, AnnotationController and
// ContentController).

#include "fakes/FakePageDocument.hpp"

#include "app/AnnotationController.hpp"
#include "app/ContentBackend.hpp"
#include "app/ContentController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/Command.hpp"
#include "editor/DocumentSession.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rivet::test::content {

using editor::ContentCapability;

struct FakeObject {
    core::ObjectId id;
    pdf::PdfContentObjectType type = pdf::PdfContentObjectType::Image;
    core::Rect bounds;
    ContentCapability capability = ContentCapability::MoveOnly;
    std::string reason;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
};

struct FakeBlock {
    core::ObjectId id;
    std::uint64_t tag = 0; // non-zero = written by Rivet
    std::string text;
    core::Rect bounds;
    ContentCapability capability = ContentCapability::FullyEditable;
    std::string reason;
    double fontSize = 12.0;
    double wrapWidth = 0.0;
    pdf::PdfFontInfo font;
    pdf::PdfColor color{0.0F, 0.0F, 0.0F};
    bool edited = false;
};

struct FakePage {
    std::vector<FakeObject> objects;
    std::vector<FakeBlock> blocks; // painted above the objects
    bool loaded = true;
};

// Quad as the real backend reports it: baseline start, baseline end, then the
// top corners (a closed loop).
inline std::array<core::Point, 4> quadOf(const core::Rect& r) {
    return {core::Point{r.minX(), r.maxY()}, core::Point{r.maxX(), r.maxY()}, core::Point{r.maxX(), r.minY()},
            core::Point{r.minX(), r.minY()}};
}

struct FakeState {
    std::map<std::uint64_t, FakePage> pages;
    std::uint64_t nextId = 1000;
    std::atomic<int> executed{0}; // commands executed (safe to poll from another thread)

    // Observation.
    int hitTestCalls = 0;
    double lastTolerance = 0.0;
    int moveCalls = 0, deleteCalls = 0, resizeCalls = 0, replaceCalls = 0, editCalls = 0, addCalls = 0,
        frontCalls = 0;
    core::Point lastMoveDelta;
    core::Rect lastResizeRect;
    editor::TextBlockPatch lastPatch;
    editor::NewTextBlock lastNew;
    std::shared_ptr<const pdf::PdfImageData> lastImage;
    std::optional<pdf::PdfBundledFont> lastUncoveredFont;

    // Configuration.
    std::u32string uncovered;
    bool editMarksEdited = true; // false: an edited block resolves later (overflow check waits)
    std::optional<core::Error> failNext;
    std::map<const void*, std::function<void(core::PageId)>> observers;

    FakePage& page(core::PageId id) { return pages[id.value()]; }
    void fire(core::PageId id) {
        const auto copy = observers;
        for (const auto& [session, callback] : copy) {
            if (callback) callback(id);
        }
    }
};

// Undo = restore a snapshot of the page taken at execute().
class SnapshotCommand final : public editor::Command {
public:
    SnapshotCommand(std::shared_ptr<FakeState> state, core::PageId page, std::function<void(FakePage&)> apply)
        : state_(std::move(state)), page_(page), apply_(std::move(apply)) {}
    std::string_view name() const override { return "Fake content edit"; }
    bool execute() override {
        FakePage& p = state_->page(page_);
        before_ = p;
        apply_(p);
        ++state_->executed;
        return true;
    }
    bool undo() override {
        state_->page(page_) = before_;
        return true;
    }

private:
    std::shared_ptr<FakeState> state_;
    core::PageId page_;
    std::function<void(FakePage&)> apply_;
    FakePage before_;
};

inline FakeBlock* findBlockIn(FakePage& page, core::ObjectId id) {
    for (FakeBlock& b : page.blocks) {
        if (b.id == id) return &b;
    }
    return nullptr;
}
inline FakeObject* findObjectIn(FakePage& page, core::ObjectId id) {
    for (FakeObject& o : page.objects) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

class FakeBackend final : public app::ContentBackend {
public:
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();

    editor::PageContentViewPtr content(editor::DocumentSession&, core::PageId page) const override {
        auto view = std::make_shared<editor::PageContentView>();
        const FakePage& p = s->page(page);
        view->loaded = p.loaded;
        return view;
    }
    std::optional<editor::ContentObjectView> findObject(editor::DocumentSession&, core::PageId page,
                                                        core::ObjectId id) const override {
        FakeObject* o = findObjectIn(s->page(page), id);
        if (o == nullptr) return std::nullopt;
        editor::ContentObjectView v;
        v.id = o->id;
        v.type = o->type;
        v.bounds = o->bounds;
        v.quad = quadOf(o->bounds);
        v.capability = o->capability;
        v.capabilityReason = o->reason;
        v.pixelWidth = o->pixelWidth;
        v.pixelHeight = o->pixelHeight;
        return v;
    }
    std::optional<editor::TextBlockView> findBlock(editor::DocumentSession&, core::PageId page,
                                                   core::ObjectId id) const override {
        FakeBlock* b = findBlockIn(s->page(page), id);
        if (b == nullptr) return std::nullopt;
        editor::TextBlockView v;
        v.id = b->id;
        v.tag = b->tag;
        v.text = b->text;
        v.bounds = b->bounds;
        v.quad = quadOf(b->bounds);
        v.fontSize = b->fontSize;
        v.lineAdvance = b->fontSize * 1.2;
        v.wrapWidth = b->wrapWidth;
        v.font = b->font;
        v.color = b->color;
        v.capability = b->capability;
        v.capabilityReason = b->reason;
        v.edited = b->edited;
        return v;
    }
    std::optional<editor::ContentHit> hitTest(editor::DocumentSession&, core::PageId page, core::Point p,
                                              double tolerance) const override {
        ++s->hitTestCalls;
        s->lastTolerance = tolerance;
        FakePage& pg = s->page(page);
        const auto grown = [&](const core::Rect& r) {
            return core::Rect{r.minX() - tolerance, r.minY() - tolerance, r.size.width + 2.0 * tolerance,
                              r.size.height + 2.0 * tolerance}
                .contains(p);
        };
        for (auto it = pg.blocks.rbegin(); it != pg.blocks.rend(); ++it) {
            if (grown(it->bounds)) return editor::ContentHit{it->id, true};
        }
        for (auto it = pg.objects.rbegin(); it != pg.objects.rend(); ++it) {
            if (grown(it->bounds)) return editor::ContentHit{it->id, false};
        }
        return std::nullopt;
    }

    core::Result<editor::ContentEdit> moveContent(editor::DocumentSession&, core::PageId page,
                                                  std::vector<core::ObjectId> ids,
                                                  core::Point delta) const override {
        ++s->moveCalls;
        s->lastMoveDelta = delta;
        if (auto failure = fail()) return std::unexpected(*failure);
        return make(page, ids, [ids, delta](FakePage& p) {
            for (const core::ObjectId id : ids) {
                if (FakeObject* o = findObjectIn(p, id)) o->bounds = o->bounds.translated(delta);
                if (FakeBlock* b = findBlockIn(p, id)) b->bounds = b->bounds.translated(delta);
            }
        });
    }
    core::Result<editor::ContentEdit> deleteContent(editor::DocumentSession&, core::PageId page,
                                                    std::vector<core::ObjectId> ids) const override {
        ++s->deleteCalls;
        if (auto failure = fail()) return std::unexpected(*failure);
        return make(page, ids, [ids](FakePage& p) {
            for (const core::ObjectId id : ids) {
                std::erase_if(p.objects, [id](const FakeObject& o) { return o.id == id; });
                std::erase_if(p.blocks, [id](const FakeBlock& b) { return b.id == id; });
            }
        });
    }
    core::Result<editor::ContentEdit> resizeContent(editor::DocumentSession&, core::PageId page, core::ObjectId id,
                                                    core::Rect bounds) const override {
        ++s->resizeCalls;
        s->lastResizeRect = bounds;
        if (auto failure = fail()) return std::unexpected(*failure);
        return make(page, {id}, [id, bounds](FakePage& p) {
            if (FakeObject* o = findObjectIn(p, id)) o->bounds = bounds;
        });
    }
    core::Result<editor::ContentEdit> replaceImage(editor::DocumentSession&, core::PageId page, core::ObjectId id,
                                                   std::shared_ptr<const pdf::PdfImageData> image) const override {
        ++s->replaceCalls;
        s->lastImage = image;
        if (auto failure = fail()) return std::unexpected(*failure);
        return make(page, {id}, [id, image](FakePage& p) {
            if (FakeObject* o = findObjectIn(p, id)) {
                o->pixelWidth = image->width;
                o->pixelHeight = image->height;
            }
        });
    }
    core::Result<editor::ContentEdit> editTextBlock(editor::DocumentSession&, core::PageId page,
                                                    core::ObjectId id, editor::TextBlockPatch patch) const override {
        ++s->editCalls;
        s->lastPatch = patch;
        if (auto failure = fail()) return std::unexpected(*failure);
        const bool mark = s->editMarksEdited;
        return make(page, {id}, [id, patch, mark](FakePage& p) {
            FakeBlock* b = findBlockIn(p, id);
            if (b == nullptr) return;
            if (patch.text) b->text = *patch.text;
            if (patch.fontSize) b->fontSize = *patch.fontSize;
            if (patch.color) b->color = *patch.color;
            if (patch.wrapWidth) b->wrapWidth = *patch.wrapWidth;
            if (patch.font) {
                b->tag = b->tag != 0 ? b->tag : id.value();
                b->font.bold = *patch.font == pdf::PdfBundledFont::SansBold;
            }
            b->edited = mark;
        });
    }
    core::Result<editor::ContentEdit> addTextBlock(editor::DocumentSession&, core::PageId page,
                                                   editor::NewTextBlock block) const override {
        ++s->addCalls;
        s->lastNew = block;
        if (auto failure = fail()) return std::unexpected(*failure);
        const core::ObjectId id{s->nextId++};
        return make(page, {id}, [id, block](FakePage& p) {
            FakeBlock b;
            b.id = id;
            b.tag = id.value();
            b.text = block.text;
            b.bounds = core::Rect{block.displayOrigin.x, block.displayOrigin.y - block.fontSize,
                                  block.displayWrapWidth > 0.0 ? block.displayWrapWidth : 100.0,
                                  block.fontSize * 1.2};
            b.fontSize = block.fontSize;
            b.color = block.color;
            b.wrapWidth = block.displayWrapWidth;
            b.edited = true;
            p.blocks.push_back(b);
        });
    }
    core::Result<editor::ContentEdit> bringToFront(editor::DocumentSession&, core::PageId page,
                                                   core::ObjectId id) const override {
        ++s->frontCalls;
        if (auto failure = fail()) return std::unexpected(*failure);
        return make(page, {id}, [id](FakePage& p) {
            const auto it = std::find_if(p.blocks.begin(), p.blocks.end(),
                                         [id](const FakeBlock& b) { return b.id == id; });
            if (it != p.blocks.end()) std::rotate(it, it + 1, p.blocks.end());
        });
    }
    std::u32string uncoveredCodepoints(pdf::PdfBundledFont font, std::string_view) const override {
        s->lastUncoveredFont = font;
        return s->uncovered;
    }
    void observe(editor::DocumentSession& session, std::function<void(core::PageId)> onChanged) const override {
        if (onChanged) s->observers[&session] = std::move(onChanged);
        else s->observers.erase(&session);
    }

private:
    std::optional<core::Error> fail() const {
        std::optional<core::Error> failure = std::move(s->failNext);
        s->failNext.reset();
        return failure;
    }
    core::Result<editor::ContentEdit> make(core::PageId page, std::vector<core::ObjectId> ids,
                                           std::function<void(FakePage&)> apply) const {
        return editor::ContentEdit{std::make_unique<SnapshotCommand>(s, page, std::move(apply)), std::move(ids)};
    }
};

class WaitDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    void pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
    }
    bool waitUntil(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            pump();
            if (predicate()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return predicate();
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(5), [this] { return !queue_.empty(); });
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

class FakeDecoder final : public platform::IImageDecoder {
public:
    mutable std::atomic<int> calls{0};
    core::Result<pdf::PdfImageData> result = [] {
        pdf::PdfImageData data;
        data.width = 20;
        data.height = 10;
        data.stride = 80;
        data.bytes.assign(800, 0);
        return data;
    }();
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t>) const override {
        ++calls;
        return result;
    }
};

class FakeDialog final : public platform::IFileDialog {
public:
    int openImageCalls = 0;
    core::Result<std::filesystem::path> image = std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel"));
    core::Result<std::filesystem::path> openPdf() override {
        return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel"));
    }
    core::Result<std::filesystem::path> openImage() override {
        ++openImageCalls;
        return image;
    }
};

inline std::filesystem::path tempFile(const char* name, const char* ext, const char* body) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-content-"} + name + ext};
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs(body, file);
        std::fclose(file);
    }
    return path;
}

struct ContentShell {
    FakePageEngine engine{3};
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    app::DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    FakeDialog dialog;
    FakeDecoder decoder;
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;
    int relayouts = 0;
    double now = 0.0;
    std::unique_ptr<app::ShellContext> context;
    std::unique_ptr<app::AnnotationController> annotations;
    std::unique_ptr<app::ContentController> content;
    std::shared_ptr<FakeState> fake;

    ContentShell() {
        services.mainDispatcher = &dispatcher;
        services.fileDialog = &dialog;
        services.imageDecoder = &decoder;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{0.0, 0.0, 800.0, 600.0});
        context = std::make_unique<app::ShellContext>(app::ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { statusLog.push_back(std::move(message)); },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [this] { ++relayouts; },
        });
        root->addChild(std::move(vp));
        annotations = std::make_unique<app::AnnotationController>(*context, *root);
        annotations->layout(viewport->frame());
        auto backend = std::make_unique<FakeBackend>();
        fake = backend->s;
        content = std::make_unique<app::ContentController>(*context, *root, *annotations, scheduler,
                                                           std::move(backend), [this] { return now; });
        content->layout(viewport->frame());
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    ~ContentShell() {
        content.reset();
        annotations.reset();
        viewport->clearDocument();
        workspace.setOnActiveTabChanged({});
    }

    void bind() {
        app::DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != app::DocumentTab::State::Ready) {
            viewport->clearDocument();
            annotations->bindTab(nullptr);
            content->bindTab(nullptr);
            return;
        }
        editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        annotations->bindTab(tab);
        content->bindTab(tab);
    }

    app::DocumentTab* open(const char* name) {
        const std::size_t before = workspace.tabCount();
        workspace.openDocument(tempFile(name, ".pdf", "%PDF-1.4\n"));
        if (!dispatcher.waitUntil([this, before] {
                app::DocumentTab* tab = workspace.activeTab();
                return workspace.tabCount() > before && tab != nullptr &&
                       tab->state() == app::DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        return workspace.activeTab();
    }

    editor::DocumentSession& session() { return *workspace.activeTab()->session(); }
    core::PageId pageId(std::size_t index = 0) { return session().pageId(index); }
    FakePage& page(std::size_t index = 0) { return fake->page(pageId(index)); }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }

    FakeObject& addImage(std::uint64_t id, core::Rect bounds, ContentCapability cap = ContentCapability::MoveOnly) {
        FakeObject o;
        o.id = core::ObjectId{id};
        o.type = pdf::PdfContentObjectType::Image;
        o.bounds = bounds;
        o.capability = cap;
        o.pixelWidth = 640;
        o.pixelHeight = 480;
        page().objects.push_back(o);
        return page().objects.back();
    }
    FakeBlock& addBlock(std::uint64_t id, core::Rect bounds, std::string text,
                        ContentCapability cap = ContentCapability::FullyEditable, std::uint64_t tag = 0) {
        FakeBlock b;
        b.id = core::ObjectId{id};
        b.tag = tag;
        b.text = std::move(text);
        b.bounds = bounds;
        b.capability = cap;
        page().blocks.push_back(b);
        return page().blocks.back();
    }

    // Page display point -> viewport point of page `index` (current zoom).
    core::Point toViewport(core::Point pagePoint, std::size_t index = 0) const {
        const core::Rect rect = *viewport->pageRectInViewport(index);
        const double zoom = viewport->zoomFactor();
        return core::Point{rect.origin.x + pagePoint.x * zoom, rect.origin.y + pagePoint.y * zoom};
    }
    bool mouse(ui::PointerEventType type, core::Point pagePoint, bool shift = false) {
        ui::PointerEvent event;
        event.type = type;
        event.button = 1;
        event.position = toViewport(pagePoint);
        event.modifiers.shift = shift;
        return viewport->onMouse(event);
    }
    void click(core::Point pagePoint) {
        mouse(ui::PointerEventType::Down, pagePoint);
        mouse(ui::PointerEventType::Up, pagePoint);
    }
    void drag(core::Point from, core::Point to) {
        mouse(ui::PointerEventType::Down, from);
        mouse(ui::PointerEventType::Move, to);
        mouse(ui::PointerEventType::Up, to);
    }
    bool key(ui::Key key, bool shift = false) {
        ui::KeyEvent event;
        event.key = key;
        event.modifiers.shift = shift;
        return viewport->onKey(event);
    }
};

} // namespace rivet::test::content
