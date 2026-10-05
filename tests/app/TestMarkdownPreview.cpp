// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "Fakes.hpp"
#include "MarkdownTestKit.hpp"
#include "app/MarkdownHostView.hpp"
#include "app/MarkdownImageStore.hpp"
#include "app/MarkdownLinks.hpp"
#include "app/MarkdownPaintMeasurer.hpp"
#include "app/MarkdownPreviewView.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rivet;
using namespace rivet::app;

namespace {

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
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_until(lock, deadline, [this] { return !queue_.empty(); })) return predicate();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

class FakeClipboard final : public platform::IClipboard {
public:
    std::string value;
    core::Status setText(const std::string& text) override {
        value = text;
        return {};
    }
    std::string text() const override { return value; }
};

// File content "WxH" decodes to a W by H BGRA image; anything else is invalid.
class FakeDecoder final : public platform::IImageDecoder {
public:
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const override {
        const std::string text(encoded.begin(), encoded.end());
        const std::size_t x = text.find('x');
        if (x == std::string::npos) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "bad", "test"));
        }
        pdf::PdfImageData out;
        out.format = pdf::PdfImageData::Format::Bgra;
        out.width = static_cast<std::uint32_t>(std::stoul(text.substr(0, x)));
        out.height = static_cast<std::uint32_t>(std::stoul(text.substr(x + 1)));
        out.stride = out.width * 4;
        out.bytes.assign(static_cast<std::size_t>(out.stride) * out.height, 0x80);
        return out;
    }
};

struct Env {
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    FakeClipboard clipboard;
    FakeDecoder decoder;
    std::vector<std::string> opened;
    std::vector<std::string> statuses;
    int changed = 0;

    MarkdownPreviewEnvironment make() {
        MarkdownPreviewEnvironment e;
        e.dispatcher = &dispatcher;
        e.scheduler = &scheduler;
        e.imageDecoder = &decoder;
        e.clipboard = &clipboard;
        e.openUrl = [this](const std::string& url) {
            opened.push_back(url);
            return core::Status{};
        };
        e.setStatus = [this](std::string s) { statuses.push_back(std::move(s)); };
        return e;
    }
};

std::shared_ptr<const markdown::MarkdownDocument> parseDoc(const std::string& src) {
    return std::make_shared<const markdown::MarkdownDocument>(markdown::testkit::parse(src));
}

struct Fixture {
    Env env;
    ui::testing::CountingRedrawSink sink;
    std::unique_ptr<MarkdownPreviewView> view;
    ui::testing::FakePaintContext ctx;

    explicit Fixture(const std::string& src, double w = 600.0, double h = 300.0) {
        view = std::make_unique<MarkdownPreviewView>(env.make());
        view->setRedrawSink(&sink);
        view->setFrame(core::Rect{0.0, 0.0, w, h});
        view->setDocumentSnapshot(parseDoc(src), 1, src);
        paint();
    }
    void paint() {
        ctx = ui::testing::FakePaintContext{};
        view->paint(ctx);
    }
    void mouse(ui::PointerEventType type, core::Point p, int button = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.position = p;
        e.button = button;
        view->onMouse(e);
    }
    void click(core::Point p) {
        mouse(ui::PointerEventType::Down, p);
        mouse(ui::PointerEventType::Up, p);
    }
    // View-space point inside the first run of the given text.
    core::Point pointIn(std::string_view text, double dx = 3.0) {
        const auto* layout = view->currentLayout();
        for (const auto& run : layout->runs) {
            if (run.text.find(text) != std::string::npos) {
                const auto& line = layout->lines[run.line];
                return {run.x + dx, line.top + line.height / 2.0 - view->scrollY()};
            }
        }
        return {-1.0, -1.0};
    }
};

std::string manyParagraphs(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += "Paragraph number " + std::to_string(i) + " of the document.\n\n";
    return s;
}

} // namespace

RIVET_TEST(previewMeasurerBoundUsesTheContextAndUnboundFallsBack) {
    MarkdownPaintMeasurer measurer;
    markdown::TextStyle style;
    style.size = 15.0;
    const markdown::TextMetrics unbound = measurer.measure("hello", style);
    CHECK(unbound.width > 0.0);
    ui::testing::FakePaintContext ctx;
    measurer.bind(&ctx);
    const markdown::TextMetrics bound = measurer.measure("hello", style);
    CHECK_EQ(bound.width, 35.0); // 7 px per character in the fake
    measurer.unbind();

    markdown::TextStyle bold;
    bold.kind = markdown::TextKind::H1;
    CHECK(MarkdownPaintMeasurer::fontFor(bold).weight == ui::Font::Weight::Bold);
    markdown::TextStyle mono;
    mono.monospace = true;
    mono.italic = true;
    CHECK(MarkdownPaintMeasurer::fontFor(mono).monospace);
    CHECK(MarkdownPaintMeasurer::fontFor(mono).italic);
}

RIVET_TEST(previewMeasureCacheIsInvalidatedByAScaleChange) {
    Fixture f("Some text for the cache.\n");
    CHECK_EQ(f.view->layoutCount(), 1u);
    f.paint();
    CHECK_EQ(f.view->layoutCount(), 1u); // nothing changed
    f.ctx = ui::testing::FakePaintContext{};
    f.ctx.backingScaleValue = 2.0;
    f.view->paint(f.ctx);
    CHECK_EQ(f.view->layoutCount(), 2u); // scale change relayouts
}

RIVET_TEST(previewPaintsOnlyTheVisibleBlocks) {
    Fixture f(manyParagraphs(500), 600.0, 300.0);
    const auto* layout = f.view->currentLayout();
    CHECK(layout != nullptr);
    CHECK(layout->contentHeight > 300.0 * 20);
    const std::size_t visible = f.ctx.texts.size();
    CHECK(visible > 0);
    CHECK(visible < 40);
    for (const auto& t : f.ctx.texts) CHECK(t.rect.maxY() > -40.0 && t.rect.minY() < 340.0);
    bool first = false;
    for (const auto& t : f.ctx.texts) first = first || t.text.find("number 0 ") != std::string::npos;
    CHECK(first);

    f.view->setScrollY(f.view->maxScrollY());
    f.paint();
    bool last = false;
    bool stillFirst = false;
    for (const auto& t : f.ctx.texts) {
        last = last || t.text.find("number 499 ") != std::string::npos;
        stillFirst = stillFirst || t.text.find("number 0 ") != std::string::npos;
    }
    CHECK(last);
    CHECK(!stillFirst);
    CHECK(f.ctx.texts.size() < 40);
    CHECK_EQ(f.ctx.clipDepth, 0);
}

RIVET_TEST(previewScrollClampsWheelAndRelayoutKeepsTheTopBlock) {
    Fixture f(manyParagraphs(200), 600.0, 300.0);
    f.view->setScrollY(-50.0);
    CHECK_EQ(f.view->scrollY(), 0.0);
    f.view->setScrollY(1e9);
    CHECK_EQ(f.view->scrollY(), f.view->maxScrollY());

    f.view->setScrollY(2000.0);
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEventType::Scroll;
    wheel.position = {100.0, 100.0};
    wheel.scrollDelta = {0.0, 120.0};
    CHECK(f.view->onMouse(wheel));
    CHECK_EQ(f.view->scrollY(), 2120.0);

    const auto* layout = f.view->currentLayout();
    const std::size_t block = layout->blockIndexAtY(f.view->scrollY());
    const std::uint32_t id = layout->blocks[block].blockId;
    f.view->setFrame(core::Rect{0.0, 0.0, 320.0, 300.0});
    f.paint();
    CHECK_EQ(f.view->layoutCount(), 2u);
    const auto* after = f.view->currentLayout();
    const std::size_t block2 = after->blockIndexAtY(f.view->scrollY());
    CHECK_EQ(after->blocks[block2].blockId, id);
}

RIVET_TEST(previewLinkPolicyOpensOnlyAllowedSchemes) {
    CHECK(classifyMarkdownLink("https://example.com").kind == MarkdownLinkKind::External);
    CHECK(classifyMarkdownLink("mailto:a@b.c").kind == MarkdownLinkKind::External);
    CHECK(classifyMarkdownLink("#intro").kind == MarkdownLinkKind::Anchor);
    CHECK(classifyMarkdownLink("docs/readme.md").kind == MarkdownLinkKind::Local);
    CHECK(classifyMarkdownLink("/etc/passwd").kind == MarkdownLinkKind::Local);
    CHECK(classifyMarkdownLink("javascript:alert(1)").kind == MarkdownLinkKind::Unsafe);
    CHECK(classifyMarkdownLink("file:///etc/passwd").kind == MarkdownLinkKind::Unsafe);
    CHECK(classifyMarkdownLink("data:text/html,x").kind == MarkdownLinkKind::Unsafe);
    CHECK(classifyMarkdownLink("").kind == MarkdownLinkKind::Empty);

    Fixture f("# Title\n\ntext\n");
    f.view->activateLink("https://example.com/x");
    f.view->activateLink("mailto:a@b.c");
    CHECK_EQ(f.env.opened.size(), 2u);
    f.view->activateLink("javascript:alert(1)");
    f.view->activateLink("file:///etc/passwd");
    f.view->activateLink("relative/file.md");
    f.view->activateLink("x-custom://thing");
    CHECK_EQ(f.env.opened.size(), 2u); // nothing else reached the opener
    CHECK(!f.env.statuses.empty());
}

RIVET_TEST(previewClickOnALinkOpensItAndADragDoesNot) {
    Fixture f("A [site](https://example.com/page) link and [bad](javascript:alert(1)) one.\n");
    const core::Point p = f.pointIn("site");
    f.click(p);
    CHECK_EQ(f.env.opened.size(), 1u);
    CHECK_EQ(f.env.opened[0], std::string("https://example.com/page"));
    const core::Point q = f.pointIn("bad");
    f.click(q);
    CHECK_EQ(f.env.opened.size(), 1u);

    f.mouse(ui::PointerEventType::Down, p);
    f.mouse(ui::PointerEventType::Move, {p.x + 25.0, p.y});
    f.mouse(ui::PointerEventType::Up, {p.x + 25.0, p.y});
    CHECK_EQ(f.env.opened.size(), 1u);
}

RIVET_TEST(previewAnchorLinkScrollsToTheHeading) {
    Fixture f("# Top\n\n" + manyParagraphs(100) + "## The Target\n\nend\n", 600.0, 300.0);
    CHECK_EQ(f.view->scrollY(), 0.0);
    CHECK(f.view->scrollToAnchor("#the-target"));
    CHECK(f.view->scrollY() > 1000.0);
    const auto decision = f.view->activateLink("#top");
    CHECK(decision.kind == MarkdownLinkKind::Anchor);
    CHECK(f.view->scrollY() < 20.0);
    CHECK(!f.view->scrollToAnchor("#missing"));
    f.view->activateLink("#missing");
    CHECK(!f.env.statuses.empty());
}

RIVET_TEST(previewImageStoreLoadsLocalFilesAndBlocksTheRest) {
    const fs::path dir = fs::temp_directory_path() / "rivet-preview-images";
    fs::create_directories(dir);
    const auto write = [&](const char* name, const char* content) {
        std::ofstream(dir / name, std::ios::binary) << content;
    };
    write("ok.img", "16x8");
    write("huge.img", "20000x10");
    write("junk.img", "not an image");

    Env env;
    std::shared_ptr<MarkdownImageStore> store = MarkdownImageStore::create(
        MarkdownImageStore::Environment{&env.dispatcher, &env.scheduler, &env.decoder});
    store->setBaseDirectory(dir);
    int changes = 0;
    store->setOnChanged([&] { ++changes; });

    CHECK(store->imageInfo("ok.img").state == markdown::ImageState::Unknown); // starts the load
    CHECK(store->imageInfo("huge.img").state == markdown::ImageState::Unknown);
    CHECK(store->imageInfo("junk.img").state == markdown::ImageState::Unknown);
    CHECK(store->imageInfo("missing.img").state == markdown::ImageState::Unknown);
    CHECK(store->imageInfo("https://example.com/a.png").state == markdown::ImageState::Blocked);
    CHECK(store->blockReason("https://example.com/a.png") == ImageBlockReason::Remote);

    CHECK(env.dispatcher.waitUntil([&] { return changes >= 4; }));
    const markdown::ImageInfo ok = store->imageInfo("ok.img");
    CHECK(ok.state == markdown::ImageState::Known);
    CHECK_EQ(ok.width, 16.0);
    CHECK_EQ(ok.height, 8.0);
    CHECK(store->bitmap("ok.img") != nullptr);
    CHECK(store->imageInfo("huge.img").state == markdown::ImageState::Blocked);
    CHECK(store->blockReason("huge.img") == ImageBlockReason::TooLarge);
    CHECK(store->blockReason("junk.img") == ImageBlockReason::Unreadable);
    CHECK(store->blockReason("missing.img") == ImageBlockReason::Unreadable);
    CHECK(store->resolve("https://x/y.png").empty());
    fs::remove_all(dir);
}

RIVET_TEST(previewImageCompletionAfterDestructionIsDiscarded) {
    const fs::path dir = fs::temp_directory_path() / "rivet-preview-images-gone";
    fs::create_directories(dir);
    std::ofstream(dir / "a.img", std::ios::binary) << "32x32";

    Env env;
    int changes = 0;
    {
        auto store = MarkdownImageStore::create(
            MarkdownImageStore::Environment{&env.dispatcher, &env.scheduler, &env.decoder});
        store->setBaseDirectory(dir);
        store->setOnChanged([&] { ++changes; });
        (void)store->imageInfo("a.img");
    } // destroyed with the load possibly in flight
    env.dispatcher.pump();
    CHECK_EQ(changes, 0);
    fs::remove_all(dir);
}

RIVET_TEST(previewImageArrivalRelayoutsAndPaintsTheBitmap) {
    const fs::path dir = fs::temp_directory_path() / "rivet-preview-images-view";
    fs::create_directories(dir);
    std::ofstream(dir / "pic.img", std::ios::binary) << "40x20";

    Fixture f("Before\n\n![a picture](pic.img)\n\nAfter\n");
    f.view->setBaseDirectory(dir);
    f.paint();
    const std::size_t before = f.view->layoutCount();
    bool placeholder = false;
    for (const auto& t : f.ctx.texts) placeholder = placeholder || t.text.find("a picture") != std::string::npos;
    CHECK(placeholder || !f.ctx.roundedFills.empty());

    CHECK(f.env.dispatcher.waitUntil([&] { return f.view->imageStore().entryCount() > 0 &&
                                                  f.view->imageStore().cachedBytes() > 0; }));
    f.paint();
    CHECK(f.view->layoutCount() > before);
    CHECK(!f.ctx.bitmaps.empty());
    fs::remove_all(dir);
}

RIVET_TEST(previewRemoteImageShowsABlockedPlaceholder) {
    Fixture f("![tracker](https://example.com/pixel.png)\n");
    f.paint();
    bool blocked = false;
    for (const auto& t : f.ctx.texts) blocked = blocked || t.text.find("Remote image blocked") != std::string::npos;
    CHECK(blocked);
    CHECK(f.ctx.bitmaps.empty());
}

RIVET_TEST(previewSelectionWordBlockAllAndCopy) {
    Fixture f("Hello brave world\n\nSecond paragraph here\n");
    // Double click selects the word.
    const core::Point p = f.pointIn("Hello", 4.0);
    f.click(p);
    f.click(p);
    CHECK(f.view->hasSelection());
    CHECK_EQ(f.view->selectedText(), std::string("Hello"));
    CHECK(f.view->copySelection());
    CHECK_EQ(f.env.clipboard.value, std::string("Hello"));

    // Triple click selects the whole block.
    f.click(p);
    CHECK_EQ(f.view->selectedText(), std::string("Hello brave world"));

    // Cmd+A and a cross-block copy.
    CHECK(f.view->selectAll());
    const std::string all = f.view->selectedText();
    CHECK(all.find("Hello brave world") != std::string::npos);
    CHECK(all.find("Second paragraph here") != std::string::npos);
    ui::KeyEvent copy;
    copy.key = ui::Key::Character;
    copy.text = "c";
    copy.modifiers.command = true;
    CHECK(f.view->onKey(copy));
    CHECK_EQ(f.env.clipboard.value, all);

    f.paint();
    CHECK(!f.ctx.fills.empty());
    f.view->clearSelection();
    CHECK(!f.view->hasSelection());
}

RIVET_TEST(previewDragSelectsAcrossBlocks) {
    Fixture f("First block text\n\nSecond block text\n");
    const core::Point a = f.pointIn("First", 2.0);
    const core::Point b = f.pointIn("Second", 60.0);
    f.mouse(ui::PointerEventType::Down, a);
    f.mouse(ui::PointerEventType::Move, {(a.x + b.x) / 2, (a.y + b.y) / 2});
    f.mouse(ui::PointerEventType::Move, b);
    f.mouse(ui::PointerEventType::Up, b);
    const std::string text = f.view->selectedText();
    CHECK(text.find("First block text") != std::string::npos);
    CHECK(text.find("Second") != std::string::npos);
    CHECK(text.find("Second block text") == std::string::npos); // ended mid-line
}

RIVET_TEST(previewSearchStepsWrapsAndHighlights) {
    Fixture f("Alpha foo beta.\n\nSecond Foo here.\n\nThird FOO there.\n");
    int notified = 0;
    f.view->setOnSearchResultsChanged([&] { ++notified; });
    f.view->startSearch("foo");
    CHECK_EQ(f.view->matchCount(), 3u);
    CHECK(f.view->currentMatch().has_value());
    CHECK_EQ(*f.view->currentMatch(), 0u);
    f.view->nextMatch();
    CHECK_EQ(*f.view->currentMatch(), 1u);
    f.view->nextMatch();
    f.view->nextMatch();
    CHECK_EQ(*f.view->currentMatch(), 0u); // wrapped
    f.view->previousMatch();
    CHECK_EQ(*f.view->currentMatch(), 2u); // wrapped backwards
    CHECK(notified >= 4);

    f.paint();
    const MarkdownPreviewPalette palette;
    int normal = 0;
    int current = 0;
    for (const auto& r : f.ctx.fills) {
        if (r.color.r == palette.match.r && r.color.g == palette.match.g && r.color.a == palette.match.a) ++normal;
        if (r.color.r == palette.currentMatch.r && r.color.g == palette.currentMatch.g &&
            r.color.a == palette.currentMatch.a) ++current;
    }
    CHECK_EQ(normal, 2);
    CHECK_EQ(current, 1);

    f.view->startSearch("zzz");
    CHECK_EQ(f.view->matchCount(), 0u);
    CHECK(!f.view->currentMatch().has_value());
    f.view->startSearch("");
    CHECK_EQ(f.view->matchCount(), 0u);
}

RIVET_TEST(previewSearchScrollsToTheCurrentMatch) {
    Fixture f(manyParagraphs(300) + "The needle sits here.\n", 600.0, 300.0);
    f.view->startSearch("needle");
    CHECK_EQ(f.view->matchCount(), 1u);
    f.view->revealCurrentMatch();
    CHECK(f.view->scrollY() > 1000.0);
    f.paint();
    bool seen = false;
    for (const auto& t : f.ctx.texts) seen = seen || t.text.find("needle") != std::string::npos;
    CHECK(seen);
}

RIVET_TEST(previewSearchSurvivesANewSnapshot) {
    Fixture f("one foo\n");
    f.view->startSearch("foo");
    CHECK_EQ(f.view->matchCount(), 1u);
    const std::string next = "foo foo foo\n";
    f.view->setDocumentSnapshot(parseDoc(next), 2, next);
    f.paint();
    CHECK_EQ(f.view->matchCount(), 3u);
    f.view->clearDocument();
    CHECK_EQ(f.view->matchCount(), 0u);
    f.paint(); // empty view paints without a document
}

RIVET_TEST(previewLargeGeneratedDocumentSmoke) {
    const std::string src = markdown::testkit::generateMarkdown(1500 * 1024);
    Fixture f(src, 800.0, 600.0);
    const auto* layout = f.view->currentLayout();
    CHECK(layout != nullptr);
    CHECK(layout->contentHeight > 10000.0);
    CHECK(f.ctx.texts.size() < 600);
    for (double y : {0.0, layout->contentHeight / 2, layout->contentHeight}) {
        f.view->setScrollY(y);
        f.paint();
        CHECK(f.ctx.texts.size() < 600);
        CHECK_EQ(f.ctx.clipDepth, 0);
    }
    f.view->startSearch("lorem");
    CHECK(f.view->matchCount() > 0);
    CHECK(f.view->selectAll());
    CHECK(!f.view->selectedText().empty());
}

RIVET_TEST(previewOverflowingBlocksScrollHorizontallyOnTheirOwn) {
    const std::string src = "```\n" + std::string(400, 'x') + "\n```\n\nafter\n";
    Fixture f(src, 300.0, 300.0);
    const auto* layout = f.view->currentLayout();
    std::size_t code = layout->blocks.size();
    for (std::size_t i = 0; i < layout->blocks.size(); ++i) {
        if (layout->blocks[i].overflowsHorizontally()) code = i;
    }
    CHECK(code < layout->blocks.size());
    CHECK_EQ(f.view->blockScrollX(code), 0.0);
    f.view->scrollBlockBy(code, 100.0);
    CHECK_EQ(f.view->blockScrollX(code), 100.0);
    f.view->scrollBlockBy(code, 1e9);
    CHECK(f.view->blockScrollX(code) < 5000.0);
    f.view->scrollBlockBy(code, -1e9);
    CHECK_EQ(f.view->blockScrollX(code), 0.0);
    f.paint();
    CHECK_EQ(f.ctx.clipDepth, 0);
}

RIVET_TEST(markdownHostRendersAndTracksTheStateRevision) {
    Env env;
    ui::testing::CountingRedrawSink sink;
    platform::ShellServices services;
    services.mainDispatcher = &env.dispatcher;
    services.clipboard = &env.clipboard;
    MarkdownHostEnvironment henv;
    henv.dispatcher = &env.dispatcher;
    henv.scheduler = &env.scheduler;
    henv.services = &services;
    auto host = createMarkdownHostView(henv);
    host->setRedrawSink(&sink);
    host->setFrame(core::Rect{0.0, 0.0, 600.0, 300.0});

    DecodedText decoded;
    decoded.text = "# Heading\n\nbody foo\n";
    MarkdownTabState state("/tmp/rivet-host-test.md", decoded);
    CHECK(state.mode() == MarkdownDisplayMode::Rendered);
    host->bind(&state);
    CHECK(host->searchTarget() != nullptr);
    ui::testing::FakePaintContext ctx;
    host->paint(ctx);
    bool heading = false;
    for (const auto& t : ctx.texts) heading = heading || t.text == "Heading";
    CHECK(heading);

    host->searchTarget()->startSearch("foo");
    CHECK_EQ(host->searchTarget()->matchCount(), 1u);

    state.setMode(MarkdownDisplayMode::Source);
    host->stateChanged();
    CHECK(host->searchTarget() != nullptr); // Source mode searches the source text

    host->bind(nullptr); // unbinding drops everything; painting is safe
    ctx = ui::testing::FakePaintContext{};
    host->paint(ctx);
    CHECK(host->searchTarget() == nullptr);
}
