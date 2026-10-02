// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared fixtures of the Phase 5 content-editing integration tests: the REAL
// engine (PDFium when built in) behind the real app shell pieces
// (DocumentWorkspace, PdfViewport, AnnotationController, ContentController
// over the real editor backend, FileController, PageEditingController for the
// search restart wiring). Nothing here uses FPDF_*; the engine is reached
// only through the Rivet pdf API, and generated raw-byte PDFs are the
// fixtures (tests/pdf/PdfFixtures.hpp).
//
// Every test starts with `Rig rig; if (!rig.ok()) return;` so the bodies are
// no-ops (and still compile) in PDFium-OFF builds.

#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "app/AnnotationController.hpp"
#include "app/ContentBackend.hpp"
#include "app/ContentController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/PageEditingController.hpp"
#include "app/ShellContext.hpp"
#include "app/SidebarController.hpp"
#include "app/StatusBarController.hpp"
#include "core/Bitmap.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/ContentCommands.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/TextSearchController.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Expected-failure marker for a CONFIRMED product bug (src/ is not fixed by
// the integration tests). While `cond` is false the bug is reported on stderr
// and the test still passes; once the product is fixed `cond` becomes true and
// the check FAILS with a message asking to promote it to a plain CHECK and
// drop the marker, so a stale marker cannot hide a regression.
#define KNOWN_BUG(id, cond, description)                                                                 \
    do {                                                                                                 \
        if (cond) {                                                                                      \
            rivet::test::reportCheck(false,                                                              \
                                     "KNOWN BUG " id " appears FIXED (" #cond                           \
                                     "): promote it to a plain CHECK and remove the marker",             \
                                     __FILE__, __LINE__);                                                \
        } else {                                                                                         \
            std::fprintf(stderr, "[known bug] %s: %s\n", id, description);                               \
        }                                                                                                \
    } while (false)

namespace rivet::test::integ {

namespace fs = std::filesystem;
namespace pdffix = rivet::test::pdffix;
using app::ContentTool;
using app::DocumentTab;

// Page geometry of the rich fixture (tests/pdf/PdfFixtures.hpp): 300x400.
inline constexpr double kPageHeight = 400.0;

// --- Dispatcher ----------------------------------------------------------------

class WaitDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        ++posted;
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            pump();
            if (predicate()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return predicate();
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [this] { return !queue_.empty(); });
        }
    }
    bool waitPosted(int target) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (posted.load() < target) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
    std::atomic<int> posted{0};

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

// --- Engine decorator -----------------------------------------------------------

// Forwards everything to a real engine; assembleDocument can be parked on a
// gate (the save-in-flight pattern of FakeWritableEngine, over PDFium).
class GatingEngine final : public pdf::PdfEngine {
public:
    explicit GatingEngine(pdf::PdfEngine& inner) : inner_(inner) {}

    bool isAvailable() const override { return inner_.isAvailable(); }
    std::string_view backendName() const override { return inner_.backendName(); }
    core::Result<std::unique_ptr<pdf::PdfDocument>> openDocument(const fs::path& path,
                                                                 std::string_view password) override {
        return inner_.openDocument(path, password);
    }
    core::Result<std::unique_ptr<pdf::PdfDocument>> reopenWithCredentialsOf(const pdf::PdfDocument& credentialsOf,
                                                                            const fs::path& path) override {
        return inner_.reopenWithCredentialsOf(credentialsOf, path);
    }
    core::Status assembleDocument(const pdf::PdfAssemblyRequest& request, pdf::IPdfByteSink& sink,
                                  std::vector<pdf::PdfAssembledPageAnnotations>* report = nullptr,
                                  std::vector<pdf::PdfAssembledPageContent>* contentReport = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++parked_;
            cv_.notify_all();
            cv_.wait(lock, [this] { return !gateClosed_; });
            --parked_;
        }
        ++assemblies;
        return inner_.assembleDocument(request, sink, report, contentReport);
    }

    void closeGate() {
        std::lock_guard<std::mutex> lock(mutex_);
        gateClosed_ = true;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            gateClosed_ = false;
        }
        cv_.notify_all();
    }
    // Waits until `count` assemblies are inside the gate.
    bool waitParked(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(30), [&] { return gateClosed_ && parked_ >= count; });
    }

    std::atomic<int> assemblies{0};

private:
    pdf::PdfEngine& inner_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool gateClosed_ = false;
    int parked_ = 0;
};

// Opens the gate again whatever the test does (a parked worker would hang the
// shell destructor).
struct GateGuard {
    GatingEngine& engine;
    ~GateGuard() { engine.release(); }
};

// --- Platform fakes -------------------------------------------------------------

class ImageDecoderFake final : public platform::IImageDecoder {
public:
    mutable std::atomic<int> calls{0};
    core::Result<pdf::PdfImageData> result = std::unexpected(core::makeError(core::ErrorCode::Unsupported, "unset"));
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t>) const override {
        ++calls;
        return result;
    }
};

class DialogFake final : public platform::IFileDialog {
public:
    int openImageCalls = 0;
    core::Result<fs::path> image = std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel"));
    core::Result<fs::path> openPdf() override {
        return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel"));
    }
    core::Result<fs::path> openImage() override {
        ++openImageCalls;
        return image;
    }
};

class SaveDialogFake final : public platform::ISaveDialog {
public:
    std::optional<fs::path> next;
    std::optional<fs::path> runSavePanel(const Options&) override { return next; }
};

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-contentinteg-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    fs::path operator()(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

inline bool writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// --- Pixels ----------------------------------------------------------------------

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

inline Rgb pixelAt(const core::Bitmap& bitmap, int x, int y) {
    if (!bitmap.isValid() || x < 0 || y < 0 || x >= static_cast<int>(bitmap.width()) ||
        y >= static_cast<int>(bitmap.height())) {
        return {-1, -1, -1};
    }
    const auto* row = reinterpret_cast<const std::uint8_t*>(bitmap.data()) + static_cast<std::size_t>(y) * bitmap.stride();
    const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
    return Rgb{px[2], px[1], px[0]};
}

// Fraction of pixels whose channels differ by more than `channelTolerance`
// (1.0 when the sizes differ).
inline double differingFraction(const core::Bitmap& a, const core::Bitmap& b, int channelTolerance = 24) {
    if (!a.isValid() || !b.isValid() || a.width() != b.width() || a.height() != b.height() || a.width() == 0) {
        return 1.0;
    }
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < a.height(); ++y) {
        for (std::uint32_t x = 0; x < a.width(); ++x) {
            const Rgb pa = pixelAt(a, static_cast<int>(x), static_cast<int>(y));
            const Rgb pb = pixelAt(b, static_cast<int>(x), static_cast<int>(y));
            if (std::abs(pa.r - pb.r) > channelTolerance || std::abs(pa.g - pb.g) > channelTolerance ||
                std::abs(pa.b - pb.b) > channelTolerance) {
                ++differing;
            }
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(a.width()) * static_cast<double>(a.height()));
}

// Page `index` of `document` rendered at `scale`, optionally with `edits`
// applied (what a save would write).
inline core::Bitmap renderOf(pdf::PdfDocument& document, std::size_t index,
                             const pdf::PdfPageContentEditsPtr& edits = nullptr, double scale = 1.0) {
    const auto info = document.pageInfo(index);
    CHECK(info.has_value());
    if (!info.has_value()) return {};
    const core::Size size = pdf::displaySize(info->view);
    auto bitmap = document.renderPage(index, info->view, {}, edits, core::Rect(0.0, 0.0, size.width, size.height), scale);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return {};
    return std::move(*bitmap);
}

// --- The rig ---------------------------------------------------------------------

// What a page presents: the revision pointers a command must (not) change.
struct PageStamp {
    std::uint64_t content = 0;
    std::uint64_t raster = 0;
    const void* annotations = nullptr;
    const void* edits = nullptr;
    bool operator==(const PageStamp&) const = default;
};

struct Rig {
    std::unique_ptr<pdf::PdfEngine> real = pdf::createEngine();
    GatingEngine engine;
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    TempDir dir;
    std::unique_ptr<app::DocumentWorkspace> workspaceOwner;
    app::DocumentWorkspace& workspace;
    SaveDialogFake saveDialog;
    DialogFake dialog;
    ImageDecoderFake decoder;
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;

    std::unique_ptr<app::ShellContext> context;
    std::unique_ptr<app::SidebarController> sidebar;
    std::unique_ptr<app::StatusBarController> status;
    std::unique_ptr<app::PageEditingController> editing;
    std::unique_ptr<app::AnnotationController> annotations;
    std::unique_ptr<app::ContentController> content;
    std::unique_ptr<app::FileController> files;
    double now = 0.0;
    bool lastDownValid_ = false;
    core::Point lastDown_;
    std::chrono::steady_clock::time_point lastDownTime_;

    Rig()
        : engine(*real), workspaceOwner(std::make_unique<app::DocumentWorkspace>(engine, scheduler, &dispatcher)),
          workspace(*workspaceOwner) {
        services.mainDispatcher = &dispatcher;
        services.saveDialog = &saveDialog;
        services.fileDialog = &dialog;
        services.imageDecoder = &decoder;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{180.0, 68.0, 800.0, 600.0});
        context = std::make_unique<app::ShellContext>(app::ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) {
                statusLog.push_back(message);
                if (status != nullptr) status->setStatus(std::move(message));
            },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [] {},
        });
        viewport->setOnFocusRequested([this] { context->setFocus(nullptr); });
        sidebar = std::make_unique<app::SidebarController>(*context, *root);
        root->addChild(std::move(vp));
        status = std::make_unique<app::StatusBarController>(*context, *root, "Ready");
        editing = std::make_unique<app::PageEditingController>(*context, *sidebar, *status);
        annotations = std::make_unique<app::AnnotationController>(*context, *root);
        content = std::make_unique<app::ContentController>(*context, *root, *annotations, scheduler,
                                                           app::createEditorContentBackend(), [this] { return now; });
        files = std::make_unique<app::FileController>(
            engine, *context, scheduler, [this](std::string text) { statusLog.push_back(std::move(text)); }, [] {});
        sidebar->layout(core::Rect{0.0, 68.0, app::SidebarController::kWidth, 600.0});
        status->layout(core::Rect{0.0, 668.0, 1020.0, app::StatusBarController::kHeight});
        annotations->layout(viewport->frame());
        content->layout(viewport->frame());
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    // False when no PDF backend is built in: the test body returns.
    bool ok() const { return real != nullptr && real->isAvailable(); }

    // The application quit: controllers first, then the workspace (and every
    // session in it) while background work may still be running.
    void quit() {
        if (workspaceOwner == nullptr) return;
        workspace.setOnActiveTabChanged({});
        files.reset();
        content.reset();
        annotations.reset();
        editing.reset();
        status.reset();
        sidebar.reset();
        viewport->clearDocument();
        workspaceOwner.reset();
    }
    ~Rig() { quit(); }

    void bind() {
        DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
            viewport->clearDocument();
            sidebar->bindTab(nullptr);
            editing->bindTab(nullptr);
            annotations->bindTab(nullptr);
            content->bindTab(nullptr);
            return;
        }
        editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        sidebar->bindTab(tab);
        editing->bindTab(tab);
        annotations->bindTab(tab);
        content->bindTab(tab);
    }

    // --- Opening ---------------------------------------------------------------

    // Writes `bytes` to <dir>/<name> and opens it in a new tab.
    DocumentTab* openBytes(const std::string& name, const std::string& bytes) {
        CHECK(writeFile(dir(name), bytes));
        return openPath(dir(name));
    }
    DocumentTab* openPath(const fs::path& path) {
        const std::size_t before = workspace.tabCount();
        workspace.openDocument(path);
        if (!dispatcher.waitUntil([this, before] {
                DocumentTab* active = workspace.activeTab();
                return workspace.tabCount() > before && active != nullptr &&
                       active->state() == DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        return workspace.activeTab();
    }
    DocumentTab* openRich(const std::string& name = "rich.pdf") { return openBytes(name, pdffix::richPdf()); }

    // --- Accessors ---------------------------------------------------------------

    DocumentTab* tab() { return workspace.activeTab(); }
    editor::DocumentSession& session() { return *tab()->session(); }
    core::PageId pageId(std::size_t index = 0) { return session().pageId(index); }
    std::size_t depth() { return session().commands().depth(); }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
    bool hasStatus(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
    bool anyStatus(const std::string& part) const {
        return std::any_of(statusLog.begin(), statusLog.end(),
                           [&](const std::string& s) { return s.find(part) != std::string::npos; });
    }

    PageStamp stamp(std::size_t index) {
        const editor::PageEntry* entry = session().pageSnapshot()->find(pageId(index));
        PageStamp out;
        if (entry == nullptr) return out;
        out.content = entry->contentRevision;
        out.raster = entry->rasterRevision;
        out.annotations = entry->annotations.get();
        out.edits = entry->contentEdits.get();
        return out;
    }

    // --- Content ------------------------------------------------------------------

    // The (lazily extracted) content of a page once it is loaded.
    editor::PageContentViewPtr loaded(std::size_t index = 0) {
        editor::PageContentViewPtr view;
        const bool ready = dispatcher.waitUntil([&] {
            view = session().contentService().content(pageId(index));
            return view != nullptr && view->loaded;
        });
        return ready ? view : nullptr;
    }
    // Current content (may be unloaded).
    editor::PageContentViewPtr contentNow(std::size_t index = 0) {
        return session().contentService().content(pageId(index));
    }
    const editor::ContentObjectView* objectOfType(const editor::PageContentView& view, pdf::PdfContentObjectType type,
                                                  std::size_t nth = 0) const {
        for (const auto& object : view.objects) {
            if (object.type != type) continue;
            if (nth-- == 0) return &object;
        }
        return nullptr;
    }
    const editor::TextBlockView* blockWith(const editor::PageContentView& view, const std::string& needle) const {
        for (const auto& block : view.blocks) {
            if (block.text.find(needle) != std::string::npos) return &block;
        }
        return nullptr;
    }
    const editor::ContentObjectView* objectById(const editor::PageContentView& view, core::ObjectId id) const {
        for (const auto& object : view.objects) {
            if (object.id == id) return &object;
        }
        return nullptr;
    }

    // Waits until page `index` is loaded and a block containing `needle`
    // exists; returns a copy of it.
    std::optional<editor::TextBlockView> awaitBlock(const std::string& needle, std::size_t index = 0) {
        std::optional<editor::TextBlockView> found;
        dispatcher.waitUntil([&] {
            const auto view = contentNow(index);
            if (view == nullptr || !view->loaded) return false;
            const auto* block = blockWith(*view, needle);
            if (block == nullptr) return false;
            found = *block;
            return true;
        });
        return found;
    }
    // Waits until page `index` is loaded and NO block contains `needle`.
    bool awaitNoBlock(const std::string& needle, std::size_t index = 0) {
        return dispatcher.waitUntil([&] {
            const auto view = contentNow(index);
            return view != nullptr && view->loaded && blockWith(*view, needle) == nullptr;
        });
    }

    void selectTool() { content->setTool(ContentTool::SelectObject); }
    void addTextTool() { content->setTool(ContentTool::AddText); }

    // --- Pointer input (PAGE DISPLAY points) -------------------------------------

    core::Point toViewport(core::Point pagePoint, std::size_t index = 0) const {
        const core::Rect rect = *viewport->pageRectInViewport(index);
        const double zoom = viewport->zoomFactor();
        return core::Point{rect.origin.x + pagePoint.x * zoom, rect.origin.y + pagePoint.y * zoom};
    }
    // The content layer times double-clicks on the real monotonic clock (not
    // injectable), so two presses of a test within 0.4 s and 5 points would be
    // read as a double-click. Unless a double-click is wanted, a press waits
    // out the remainder of that window (at most ~0.4 s, only when needed).
    bool mouse(ui::PointerEventType type, core::Point pagePoint, bool shift = false, std::size_t index = 0,
               bool allowDoubleClick = false) {
        ui::PointerEvent event;
        event.type = type;
        event.button = 1;
        event.position = toViewport(pagePoint, index);
        event.modifiers.shift = shift;
        if (type == ui::PointerEventType::Down) {
            using Clock = std::chrono::steady_clock;
            if (!allowDoubleClick && lastDownValid_ &&
                std::hypot(event.position.x - lastDown_.x, event.position.y - lastDown_.y) <= 6.0) {
                const auto elapsed = Clock::now() - lastDownTime_;
                const auto window = std::chrono::milliseconds(450);
                if (elapsed < window) std::this_thread::sleep_for(window - elapsed);
            }
            lastDownValid_ = true;
            lastDown_ = event.position;
            lastDownTime_ = Clock::now();
        }
        return viewport->onMouse(event);
    }
    void doubleClick(core::Point pagePoint, std::size_t index = 0) {
        mouse(ui::PointerEventType::Down, pagePoint, false, index, true);
        mouse(ui::PointerEventType::Up, pagePoint, false, index, true);
        mouse(ui::PointerEventType::Down, pagePoint, false, index, true);
        mouse(ui::PointerEventType::Up, pagePoint, false, index, true);
    }
    void click(core::Point pagePoint, std::size_t index = 0) {
        mouse(ui::PointerEventType::Down, pagePoint, false, index);
        mouse(ui::PointerEventType::Up, pagePoint, false, index);
    }
    void drag(core::Point from, core::Point to, bool shift = false, std::size_t index = 0) {
        mouse(ui::PointerEventType::Down, from, shift, index);
        mouse(ui::PointerEventType::Move, to, shift, index);
        mouse(ui::PointerEventType::Up, to, shift, index);
    }
    bool key(ui::Key key, bool shift = false) {
        ui::KeyEvent event;
        event.key = key;
        event.modifiers.shift = shift;
        return viewport->onKey(event);
    }

    // --- Saving --------------------------------------------------------------------

    // Cmd+S: waits for the save to settle ("Saved ..." / failure).
    bool saveAndWait() {
        statusLog.clear();
        files->perform(app::FileCommand::Save);
        return dispatcher.waitUntil([this] { return hasStatus("Saved") || hasStatus("Save failed"); }) &&
               hasStatus("Saved");
    }
    bool saveAsAndWait(const fs::path& destination) {
        saveDialog.next = destination;
        statusLog.clear();
        files->perform(app::FileCommand::SaveAs);
        return dispatcher.waitUntil([this] { return hasStatus("Saved") || hasStatus("Save failed"); }) &&
               hasStatus("Saved");
    }

    // Opens `path` with the real engine, outside the app shell.
    std::unique_ptr<pdf::PdfDocument> reopen(const fs::path& path) {
        auto opened = real->openDocument(path, {});
        CHECK(opened.has_value());
        if (!opened.has_value()) return nullptr;
        return std::move(*opened);
    }
};

inline std::string textOf(pdf::PdfDocument& document, std::size_t page) {
    auto text = document.textPage(page);
    if (!text.has_value()) return {};
    return (*text)->text();
}

inline std::size_t annotationCount(pdf::PdfDocument& document, std::size_t page) {
    auto annotations = document.annotations(page);
    if (!annotations.has_value()) return 0;
    return (*annotations)->items.size();
}

inline std::size_t linkCount(pdf::PdfDocument& document, std::size_t page) {
    auto links = document.pageLinks(page);
    return links.has_value() ? links->size() : 0;
}

inline std::shared_ptr<const pdf::PdfPageContent> contentOf(pdf::PdfDocument& document, std::size_t page) {
    auto content = document.pageContent(page, nullptr);
    CHECK(content.has_value());
    if (!content.has_value()) return nullptr;
    return *content;
}

inline bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

inline core::Point centerOf(const core::Rect& rect) { return rect.center(); }

// First object of the block `blockId` (the lowest z-index among its members).
inline std::optional<std::uint32_t> firstMemberIndex(const editor::PageContentView& view, core::ObjectId blockId) {
    std::optional<std::uint32_t> lowest;
    for (const auto& object : view.objects) {
        if (object.block != blockId) continue;
        if (!lowest.has_value() || object.index < *lowest) lowest = object.index;
    }
    return lowest;
}

// Selects the block containing `needle` (page 0), opens the inline editor,
// replaces its text and commits. False when any step is refused.
inline bool retype(Rig& rig, const std::string& needle, const std::string& replacement) {
    const auto view = rig.contentNow(0);
    if (view == nullptr) return false;
    const auto* block = rig.blockWith(*view, needle);
    if (block == nullptr) return false;
    rig.click(centerOf(block->bounds));
    const auto selected = rig.content->selected();
    if (!selected.has_value() || selected->info.id != block->id) return false;
    rig.content->editSelectedText();
    if (!rig.content->editorOpen()) return false;
    rig.content->editorArea().setText(replacement);
    return rig.content->commitEditor();
}

} // namespace rivet::test::integ
