// SPDX-License-Identifier: MPL-2.0
// Live Markdown preview: the async parse coordinator (generation semantics,
// debounce coalescing, lifetime), the host's Rendered/Source/Split modes
// (geometry, per-tab state, scroll sync, source search) and the byte-exact
// save contract.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "MarkdownTestKit.hpp"
#include "SourceEditorKit.hpp"
#include "app/MarkdownFind.hpp"
#include "app/MarkdownHostView.hpp"
#include "app/MarkdownParseCoordinator.hpp"
#include "app/MarkdownPreviewView.hpp"
#include "app/MarkdownSourcePane.hpp"
#include "app/MarkdownTabState.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "markdown/MarkdownSourceMap.hpp"
#include "platform/PlatformKit.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace rivet;
using namespace rivet::app;

namespace {

class ManualDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    // Runs everything queued so far (tasks queued by those tasks stay queued).
    bool pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return !run.empty();
    }
    std::size_t pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

// A scheduler that runs nothing until the test says so.
class ManualRunner {
public:
    void operator()(std::function<void()> job) { jobs.push_back(std::move(job)); }
    bool runAt(std::size_t index) {
        if (index >= jobs.size()) return false;
        auto job = std::move(jobs[index]);
        jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(index));
        job();
        return true;
    }
    bool runNext() { return runAt(0); }
    std::deque<std::function<void()>> jobs;
};

struct CoordinatorFixture {
    ManualDispatcher dispatcher;
    ManualRunner runner;
    std::string text = "# one\n";
    std::uint64_t revision = 1;
    std::vector<std::uint64_t> applied;
    std::unique_ptr<MarkdownParseCoordinator> coordinator;

    explicit CoordinatorFixture(std::size_t syncBytes = 64 * 1024) {
        MarkdownParseEnvironment env;
        env.dispatcher = &dispatcher;
        env.runBackground = [this](std::function<void()> job) { runner(std::move(job)); };
        MarkdownParseTuning tuning;
        tuning.debounce = std::chrono::milliseconds(0);
        tuning.synchronousParseBytes = syncBytes;
        coordinator = std::make_unique<MarkdownParseCoordinator>(std::move(env), tuning);
        coordinator->setOnParsed([this](MarkdownParseResult r) { applied.push_back(r.revision); });
        coordinator->bind([this] { return MarkdownParseCoordinator::Snapshot{text, revision}; });
    }
    void edit() {
        ++revision;
        text += "x\n";
        coordinator->requestParse(revision, MarkdownParseCoordinator::Urgency::Debounced);
    }
    // Runs jobs and deliveries until nothing is left.
    void drain() {
        while (runner.runNext() || dispatcher.pump()) {
        }
    }
};

} // namespace

// ============================================================== coordinator

RIVET_TEST(coordinatorSmallImmediateRequestParsesSynchronously) {
    CoordinatorFixture f;
    f.coordinator->requestParse(1, MarkdownParseCoordinator::Urgency::Immediate);
    CHECK_EQ(f.applied.size(), 1u);
    CHECK_EQ(f.applied[0], 1u);
    CHECK(f.runner.jobs.empty());
    CHECK_EQ(f.coordinator->appliedRevision(), 1u);
}

RIVET_TEST(coordinatorLargeImmediateRequestParsesInTheBackground) {
    CoordinatorFixture f(16);
    f.text = std::string(100, 'a') + "\n";
    f.coordinator->requestParse(1, MarkdownParseCoordinator::Urgency::Immediate);
    CHECK(f.applied.empty()); // nothing yet: the caller keeps its placeholder
    CHECK(f.coordinator->busy());
    f.drain();
    CHECK_EQ(f.applied.size(), 1u);
    CHECK(!f.coordinator->busy());
}

RIVET_TEST(coordinatorDebouncedEditsCoalesceIntoOneParse) {
    CoordinatorFixture f;
    for (int i = 0; i < 50; ++i) f.edit();
    CHECK_EQ(f.runner.jobs.size(), 1u); // one wait for the whole burst
    f.drain();
    CHECK_EQ(f.applied.size(), 1u);
    CHECK_EQ(f.applied[0], 51u);
    CHECK_EQ(f.coordinator->parsesStarted(), 1u);
}

RIVET_TEST(coordinatorOutOfOrderCompletionKeepsTheNewestRevision) {
    CoordinatorFixture f;
    f.edit(); // rev 2
    f.runner.runNext();
    f.dispatcher.pump(); // debounce fired -> parse job for rev 2 queued
    CHECK_EQ(f.runner.jobs.size(), 1u);
    f.edit(); // rev 3 requested while rev 2 is still parsing
    CHECK_EQ(f.runner.jobs.size(), 2u);
    f.runner.runAt(1);
    f.dispatcher.pump(); // rev 3 debounce -> parse job for rev 3 queued behind rev 2
    CHECK_EQ(f.runner.jobs.size(), 2u);
    f.runner.runAt(1); // rev 3 finishes first
    f.dispatcher.pump();
    CHECK_EQ(f.applied.size(), 1u);
    CHECK_EQ(f.applied[0], 3u);
    f.runner.runAt(0); // rev 2 finishes late
    f.dispatcher.pump();
    CHECK_EQ(f.applied.size(), 1u); // dropped: the UI stays on rev 3
    CHECK_EQ(f.coordinator->appliedRevision(), 3u);
    CHECK_EQ(f.coordinator->resultsDropped(), 1u);
}

RIVET_TEST(coordinatorDropsAResultOlderThanTheLatestRequest) {
    CoordinatorFixture f;
    f.edit(); // rev 2
    f.runner.runNext();
    f.dispatcher.pump(); // parse job for rev 2 queued
    f.edit();            // rev 3 requested, still debouncing
    f.runner.runAt(0);   // rev 2's job completes now
    f.dispatcher.pump();
    CHECK(f.applied.empty());
    f.drain(); // rev 3's own parse
    CHECK_EQ(f.applied.size(), 1u);
    CHECK_EQ(f.applied[0], 3u);
}

RIVET_TEST(coordinatorUnbindDropsInFlightResults) {
    CoordinatorFixture f;
    f.edit();
    f.runner.runNext();
    f.dispatcher.pump();
    f.coordinator->unbind();
    f.drain();
    CHECK(f.applied.empty());
    CHECK(!f.coordinator->bound());
    CHECK(!f.coordinator->busy());
    // Rebinding starts clean.
    f.coordinator->bind([&f] { return MarkdownParseCoordinator::Snapshot{f.text, 7}; });
    f.coordinator->requestParse(7, MarkdownParseCoordinator::Urgency::Immediate);
    CHECK_EQ(f.applied.size(), 1u);
    CHECK_EQ(f.applied[0], 7u);
}

RIVET_TEST(coordinatorDestructionWithQueuedAndPendingWorkIsSafe) {
    CoordinatorFixture f;
    f.edit();
    f.runner.runNext();
    f.dispatcher.pump(); // parse job queued
    f.edit();            // debounce job queued as well
    f.coordinator.reset(); // destroyed with both outstanding
    f.drain();             // late jobs and deliveries must neither crash nor call back
    CHECK(f.applied.empty());
}

RIVET_TEST(coordinatorDestructionDuringARealParseWaitsAndNeverCallsBack) {
    ManualDispatcher dispatcher;
    core::TaskScheduler scheduler{2};
    int applied = 0;
    {
        MarkdownParseEnvironment env;
        env.dispatcher = &dispatcher;
        env.scheduler = &scheduler;
        MarkdownParseTuning tuning;
        tuning.debounce = std::chrono::milliseconds(0);
        tuning.synchronousParseBytes = 16;
        MarkdownParseCoordinator coordinator(env, tuning);
        coordinator.setOnParsed([&](MarkdownParseResult) { ++applied; });
        const std::string big = markdown::testkit::generateMarkdown(400 * 1024);
        coordinator.bind([&] { return MarkdownParseCoordinator::Snapshot{big, 1}; });
        coordinator.requestParse(1, MarkdownParseCoordinator::Urgency::Immediate);
    } // destructor joins the running parse
    dispatcher.pump();
    CHECK_EQ(applied, 0);
}

RIVET_TEST(coordinatorWithoutWorkersParsesSynchronously) {
    ManualDispatcher dispatcher;
    int applied = 0;
    MarkdownParseEnvironment env; // no scheduler, no dispatcher
    MarkdownParseCoordinator coordinator(env);
    coordinator.setOnParsed([&](MarkdownParseResult r) { applied += static_cast<int>(r.revision); });
    coordinator.bind([] { return MarkdownParseCoordinator::Snapshot{"# x\n", 4}; });
    coordinator.requestParse(4, MarkdownParseCoordinator::Urgency::Debounced);
    CHECK_EQ(applied, 4);
}

RIVET_TEST(coordinatorResultCarriesTheParsedSourceAndDiagnostics) {
    ManualDispatcher dispatcher;
    MarkdownParseEnvironment env;
    env.dispatcher = &dispatcher;
    MarkdownParseCoordinator coordinator(env);
    MarkdownParseResult got;
    coordinator.setOnParsed([&](MarkdownParseResult r) { got = std::move(r); });
    coordinator.bind([] { return MarkdownParseCoordinator::Snapshot{"# Title\n\nbody\n", 9}; });
    coordinator.requestParse(9, MarkdownParseCoordinator::Urgency::Immediate);
    CHECK(got.document != nullptr);
    CHECK_EQ(got.revision, 9u);
    CHECK_EQ(got.source, std::string("# Title\n\nbody\n"));
    CHECK_EQ(got.document->headings.size(), 1u);
}

// =========================================================== source search

RIVET_TEST(findInTextIsAsciiCaseInsensitiveAndNonOverlapping) {
    const auto m = findInText("Foo foo FOO\naaa", "foo");
    CHECK_EQ(m.size(), 3u);
    CHECK_EQ(m[1].begin, 4u);
    CHECK_EQ(m[1].end, 7u);
    CHECK_EQ(findInText("aaaa", "aa").size(), 2u);
    CHECK(findInText("abc", "").empty());
    CHECK(findInText("abc", "\xFF").empty()); // invalid UTF-8 query
    CHECK_EQ(findInText("привет Мир", "привет").size(), 1u);
    CHECK(findInText("ПРИВЕТ", "привет").empty()); // only ASCII folds
    CHECK_EQ(findInText("aaaaaa", "a", 4).size(), 4u); // capped
}

// ===================================================================== host

namespace {

struct HostFixture {
    ManualDispatcher dispatcher;
    ManualRunner runner;
    struct Clip final : platform::IClipboard {
        std::string value;
        core::Status setText(const std::string& t) override {
            value = t;
            return {};
        }
        std::string text() const override { return value; }
    } clipboard;
    platform::ShellServices services;
    std::vector<std::string> statuses;
    ui::Widget* focused = nullptr;
    ui::testing::CountingRedrawSink sink;
    std::unique_ptr<MarkdownHostView> host;

    explicit HostFixture(double width = 801.0, double height = 400.0, bool manualWorkers = true,
                         core::TaskScheduler* scheduler = nullptr) {
        services.mainDispatcher = &dispatcher;
        services.clipboard = &clipboard;
        MarkdownHostEnvironment env;
        env.dispatcher = &dispatcher;
        env.scheduler = scheduler;
        env.services = &services;
        env.setStatus = [this](std::string s) { statuses.push_back(std::move(s)); };
        env.setFocus = [this](ui::Widget* w) {
            if (focused != nullptr) focused->setFocused(false);
            focused = w;
            if (w != nullptr) w->setFocused(true);
        };
        env.parseTuning.debounce = std::chrono::milliseconds(0);
        if (manualWorkers) env.runBackground = [this](std::function<void()> job) { runner(std::move(job)); };
        host = createMarkdownHostView(std::move(env));
        host->setRedrawSink(&sink);
        host->setFrame(core::Rect{0.0, 0.0, width, height});
    }

    void drain() {
        while (runner.runNext() || dispatcher.pump()) {
        }
    }
    void paint() {
        ui::testing::FakePaintContext ctx;
        host->paint(ctx);
    }
    // What the shell does: any state change reaches the host.
    void attach(MarkdownTabState& state) {
        state.setOnChanged([this] { host->stateChanged(); });
    }
    void bind(MarkdownTabState& state) {
        attach(state);
        host->bind(&state);
    }
    ui::SourceEditor& editor() { return host->sourcePane()->editor(); }
};

std::unique_ptr<MarkdownTabState> makeState(const std::string& text, MarkdownDisplayMode mode = MarkdownDisplayMode::Rendered) {
    DecodedText decoded;
    decoded.text = text;
    auto state = std::make_unique<MarkdownTabState>("live.md", std::move(decoded));
    state->setMode(mode);
    return state;
}

std::string paragraphs(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += "Paragraph number " + std::to_string(i) + " of the document.\n\n";
    return s;
}

} // namespace

RIVET_TEST(hostRenderedModeShowsOnlyThePreviewAndParsesSmallDocumentsAtOnce) {
    HostFixture f;
    auto state = makeState("# Hi\n\nbody\n");
    f.bind(*state);
    const auto panes = f.host->panes();
    CHECK(panes.source.isEmpty());
    CHECK_EQ(panes.preview.size.width, 801.0);
    CHECK(f.host->previewView()->document() != nullptr); // synchronous, no placeholder flash
    CHECK(f.runner.jobs.empty());
    CHECK(f.host->sourcePane()->state() == nullptr); // no editor mirror while rendered
}

RIVET_TEST(hostSplitGeometryIsFiftyFiftyWithAOnePixelDivider) {
    HostFixture f(801.0, 400.0);
    auto state = makeState("text\n", MarkdownDisplayMode::Split);
    f.bind(*state);
    const auto p = f.host->panes();
    CHECK_EQ(p.source.origin.x, 0.0);
    CHECK_EQ(p.source.size.width, 400.0);
    CHECK_EQ(p.divider.origin.x, 400.0);
    CHECK_EQ(p.divider.size.width, 1.0);
    CHECK_EQ(p.preview.origin.x, 401.0);
    CHECK_EQ(p.preview.size.width, 400.0);
    CHECK_EQ(p.source.size.height, 400.0);
    CHECK(f.host->sourcePane()->state() == state.get());

    state->setMode(MarkdownDisplayMode::Source);
    CHECK(f.host->panes().preview.isEmpty());
    CHECK_EQ(f.host->panes().source.size.width, 801.0);
    CHECK(f.host->panes().divider.isEmpty());
}

RIVET_TEST(hostSplitDividerCanBeDraggedAndStaysClamped) {
    HostFixture f(801.0, 400.0);
    auto state = makeState("text\n", MarkdownDisplayMode::Split);
    f.bind(*state);
    const auto down = ui::testing::pointer(ui::PointerEventType::Down, 400.0, 20.0);
    f.host->onMouse(down);
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Move, 560.0, 20.0));
    CHECK(f.host->panes().source.size.width > 540.0);
    CHECK(f.host->panes().source.size.width < 570.0);
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Move, 5000.0, 20.0));
    CHECK(f.host->panes().preview.size.width >= 79.0); // never collapses a pane
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Up, 5000.0, 20.0));
    const double divider = f.host->panes().divider.origin.x;
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Move, 100.0, 20.0, 0));
    CHECK_EQ(f.host->panes().divider.origin.x, divider); // no drag after release
}

RIVET_TEST(hostModeSwitchKeepsTheSameEditorAndPerTabCaretAndScroll) {
    HostFixture f;
    std::string text;
    for (int i = 0; i < 300; ++i) text += "line " + std::to_string(i) + "\n";
    auto state = makeState(text, MarkdownDisplayMode::Source);
    f.bind(*state);
    ui::SourceEditor* editor = &f.editor();
    editor->setSelection(50, 90);
    editor->scrollToOffset(text.find("line 150"));
    const std::size_t top = editor->topVisibleOffset();
    CHECK(f.focused == editor);

    state->setMode(MarkdownDisplayMode::Rendered);
    CHECK(f.host->panes().source.isEmpty());
    CHECK(f.focused != editor); // focus left the hidden editor
    state->setMode(MarkdownDisplayMode::Source);
    CHECK(&f.editor() == editor);
    CHECK_EQ(editor->selection().begin, 50u);
    CHECK_EQ(editor->selection().end, 90u);
    CHECK(f.focused == editor);
    // The scroll follows what the preview showed when it was visible last.
    CHECK(editor->topVisibleOffset() <= top + 200);
    CHECK_EQ(f.host->sourcePane()->state(), state.get());
}

RIVET_TEST(hostSourceToRenderedCarriesTheReadingPosition) {
    HostFixture f;
    const std::string text = paragraphs(200);
    auto state = makeState(text, MarkdownDisplayMode::Source);
    f.bind(*state);
    f.editor().scrollToOffset(text.find("number 120 "));
    state->setMode(MarkdownDisplayMode::Rendered);
    f.drain();
    f.paint(); // lays out the preview, applying the deferred scroll
    auto* preview = f.host->previewView();
    CHECK(preview->scrollY() > 100.0);
    const std::size_t top = preview->topSourceOffset().value_or(0);
    CHECK(top > text.find("number 100 "));
    CHECK(top < text.find("number 140 "));
}

RIVET_TEST(hostSwitchingTabsKeepsEachTabsOwnScrollAndNeverShowsTheOthersDocument) {
    HostFixture f;
    const std::string a = paragraphs(200);
    auto stateA = makeState(a);
    auto stateB = makeState("# Other tab\n\nsmall\n");
    f.bind(*stateA);
    f.paint();
    auto* preview = f.host->previewView();
    preview->setScrollY(900.0);
    const std::size_t topBefore = preview->topSourceOffset().value_or(0);
    f.bind(*stateB);
    f.paint();
    CHECK_EQ(preview->source(), stateB->source());
    CHECK_EQ(preview->scrollY(), 0.0);
    f.bind(*stateA);
    f.paint();
    CHECK_EQ(preview->source(), stateA->source());
    const std::size_t topAfter = preview->topSourceOffset().value_or(1);
    CHECK(topAfter <= topBefore + 2 && topAfter + 2 >= topBefore);
}

RIVET_TEST(hostLargeDocumentParsesInTheBackgroundAndShowsThePlaceholderUntilReady) {
    HostFixture f;
    auto state = makeState(markdown::testkit::generateMarkdown(200 * 1024));
    f.bind(*state);
    CHECK(f.host->previewView()->document() == nullptr);
    CHECK(f.host->parseCoordinator()->busy());
    f.paint(); // painting without a document is safe
    f.drain();
    CHECK(f.host->previewView()->document() != nullptr);
    CHECK_EQ(f.host->previewView()->revision(), state->revision());
}

RIVET_TEST(hostSplitTypingIntoALargeDocumentDoesNotParsePerKeystroke) {
    HostFixture f;
    auto state = makeState(markdown::testkit::generateMarkdown(1024 * 1024), MarkdownDisplayMode::Split);
    f.bind(*state);
    f.drain(); // initial parse
    CHECK(f.host->previewView()->document() != nullptr);
    const std::size_t before = f.host->parseCoordinator()->parsesStarted();
    const std::uint64_t revisionBefore = state->revision();

    ui::testing::typeString(f.editor(), std::string(200, 'q'));
    CHECK(state->revision() >= revisionBefore + 200);
    CHECK(f.host->previewView()->revision() < state->revision()); // the stale preview stays until the debounce fires
    CHECK_EQ(f.runner.jobs.size(), 1u);                           // one wait for all 200 edits
    f.drain();
    CHECK_EQ(f.host->parseCoordinator()->parsesStarted() - before, 1u);
    CHECK_EQ(f.host->previewView()->revision(), state->revision());
    CHECK_EQ(f.host->previewView()->source(), state->source());
}

RIVET_TEST(hostRenderedModeReparsesAtOnceAfterUndo) {
    HostFixture f;
    auto state = makeState("one\n", MarkdownDisplayMode::Split);
    f.bind(*state);
    ui::testing::typeString(f.editor(), "abc");
    f.drain();
    state->setMode(MarkdownDisplayMode::Rendered);
    CHECK(state->undo()); // small document: no placeholder, no worker
    CHECK(f.runner.jobs.empty());
    CHECK_EQ(f.host->previewView()->source(), state->source());
}

RIVET_TEST(hostClosingTheTabDuringAParseDropsTheResult) {
    HostFixture f;
    auto state = makeState(markdown::testkit::generateMarkdown(200 * 1024));
    f.bind(*state);
    CHECK(f.host->parseCoordinator()->busy());
    f.host->bind(nullptr);
    state.reset(); // the tab (and its state) is gone while the parse job is still queued
    f.drain();
    CHECK(f.host->previewView()->document() == nullptr);
    f.paint();
}

RIVET_TEST(hostLateResultOfThePreviousTabNeverReachesTheNewOne) {
    HostFixture f;
    auto big = makeState(markdown::testkit::generateMarkdown(200 * 1024));
    auto small = makeState("# Small\n");
    f.bind(*big);
    f.runner.runNext(); // the parse of `big` ran, its delivery is still queued
    f.bind(*small);
    CHECK_EQ(f.host->previewView()->source(), small->source());
    f.drain();
    CHECK_EQ(f.host->previewView()->source(), small->source());
    CHECK_EQ(f.host->previewView()->document()->blocks.size(), 1u);
}

RIVET_TEST(hostDestroyedWithQueuedWorkNeverTouchesFreedMemory) {
    HostFixture f;
    auto state = makeState(markdown::testkit::generateMarkdown(200 * 1024), MarkdownDisplayMode::Split);
    f.bind(*state);
    ui::testing::typeString(f.editor(), "abc");
    state->setOnChanged({}); // the shell is gone with the host
    f.host.reset();
    f.drain(); // late jobs and deliveries run against a dead host
    CHECK(true);
}

RIVET_TEST(hostDestroyedWhileARealParseRuns) {
    core::TaskScheduler scheduler{2};
    HostFixture f(801.0, 400.0, false, &scheduler);
    auto state = makeState(markdown::testkit::generateMarkdown(600 * 1024));
    f.bind(*state); // async parse on the real pool
    state->setOnChanged({});
    f.host.reset(); // waits for the running job
    f.dispatcher.pump();
    CHECK(true);
}

RIVET_TEST(hostParseDiagnosticsAreStoredAndReportedOnce) {
    HostFixture f;
    auto state = makeState(std::string(64, '>') + " deep\n");
    f.bind(*state);
    CHECK(!state->parseDiagnostics().empty());
    bool reported = false;
    for (const auto& s : f.statuses) reported = reported || s.find("parse limits") != std::string::npos;
    CHECK(reported);
    const std::size_t count = f.statuses.size();
    f.host->stateChanged(); // nothing new: no repeated status
    CHECK_EQ(f.statuses.size(), count);
}

// ------------------------------------------------------------------ search

RIVET_TEST(hostSourceSearchHighlightsStepsAndWrapsInSourceMode) {
    HostFixture f;
    auto state = makeState("Foo bar\nfoo BAR foo\n", MarkdownDisplayMode::Source);
    f.bind(*state);
    ISearchTarget* target = f.host->searchTarget();
    CHECK(target != nullptr);
    int notified = 0;
    target->setOnSearchResultsChanged([&] { ++notified; });
    target->startSearch("FOO");
    CHECK_EQ(target->matchCount(), 3u);
    CHECK_EQ(target->currentMatch().value_or(99), 0u);
    CHECK_EQ(f.editor().highlightCount(), 3u);
    target->nextMatch();
    CHECK_EQ(target->currentMatch().value_or(99), 1u);
    target->nextMatch();
    target->nextMatch();
    CHECK_EQ(target->currentMatch().value_or(99), 0u); // wrapped
    target->previousMatch();
    CHECK_EQ(target->currentMatch().value_or(99), 2u);
    CHECK(notified >= 4);
    target->startSearch("zzz");
    CHECK_EQ(target->matchCount(), 0u);
    CHECK(!target->currentMatch().has_value());
    target->startSearch("");
    CHECK_EQ(f.editor().highlightCount(), 0u);
}

RIVET_TEST(hostSourceSearchRevealsAMatchFarBelow) {
    HostFixture f(801.0, 200.0);
    std::string text;
    for (int i = 0; i < 500; ++i) text += "filler line " + std::to_string(i) + "\n";
    text += "needle here\n";
    auto state = makeState(text, MarkdownDisplayMode::Source);
    f.bind(*state);
    f.host->searchTarget()->startSearch("needle");
    CHECK_EQ(f.host->searchTarget()->matchCount(), 1u);
    CHECK(f.editor().firstVisibleLine() == 0);
    f.host->searchTarget()->revealCurrentMatch();
    CHECK(f.editor().lastVisibleLine() >= 500);
}

RIVET_TEST(hostSourceSearchFollowsEditsAndSplitSearchesTheSource) {
    HostFixture f;
    auto state = makeState("foo\n", MarkdownDisplayMode::Split);
    f.bind(*state);
    ISearchTarget* target = f.host->searchTarget();
    target->startSearch("foo");
    CHECK_EQ(target->matchCount(), 1u);
    f.editor().setCaretOffset(f.editor().buffer().size());
    ui::testing::typeString(f.editor(), "foo");
    CHECK_EQ(target->matchCount(), 2u); // refreshed from the editor's own edit
    CHECK_EQ(f.editor().highlightCount(), 2u);
    CHECK(state->undo());
    f.host->stateChanged();
    CHECK_EQ(target->matchCount(), 1u); // ...and from undo
}

RIVET_TEST(hostSearchKeepsItsQueryAcrossModeSwitches) {
    HostFixture f;
    auto state = makeState("Foo bar\n\nfoo BAR foo\n", MarkdownDisplayMode::Source);
    f.bind(*state);
    ISearchTarget* target = f.host->searchTarget();
    target->startSearch("foo");
    CHECK_EQ(target->matchCount(), 3u);
    state->setMode(MarkdownDisplayMode::Rendered);
    f.drain();
    f.paint(); // lays the preview out; its matches appear
    CHECK(f.host->searchTarget() == target); // one stable target
    CHECK_EQ(target->searchQuery(), std::string("foo"));
    CHECK_EQ(target->matchCount(), 3u);
    CHECK_EQ(f.editor().highlightCount(), 0u); // the hidden editor shows none
    state->setMode(MarkdownDisplayMode::Source);
    CHECK_EQ(target->matchCount(), 3u);
    CHECK_EQ(f.editor().highlightCount(), 3u);
}

RIVET_TEST(hostSearchTargetIsGoneWhenNothingIsBound) {
    HostFixture f;
    CHECK(f.host->searchTarget() == nullptr);
    auto state = makeState("x\n");
    f.bind(*state);
    CHECK(f.host->searchTarget() != nullptr);
    f.host->bind(nullptr);
    CHECK(f.host->searchTarget() == nullptr);
}

// ------------------------------------------------------------- sync scroll

namespace {

struct SplitScroll {
    HostFixture f;
    std::string text = paragraphs(300);
    std::unique_ptr<MarkdownTabState> state = makeState(text, MarkdownDisplayMode::Split);
    SplitScroll() : f(801.0, 400.0) {
        f.bind(*state);
        f.drain();
        f.paint();
    }
    MarkdownPreviewView& preview() { return *f.host->previewView(); }
    ui::SourceEditor& editor() { return f.editor(); }
};

} // namespace

RIVET_TEST(syncScrollSourceDrivesThePreview) {
    SplitScroll s;
    CHECK(s.preview().scrollY() < 1.0);
    s.editor().scrollToOffset(s.text.find("number 200 "));
    s.f.paint();
    const markdown::MarkdownLayout* layout = s.preview().currentLayout();
    CHECK(layout != nullptr);
    const double expected = std::min(markdown::previewYForSourceOffset(*layout, s.editor().topVisibleOffset()),
                                     s.preview().maxScrollY());
    CHECK(expected > 500.0);
    CHECK_EQ(s.preview().scrollY(), expected);
}

RIVET_TEST(syncScrollPreviewDrivesTheSourceWithoutEchoJitter) {
    SplitScroll s;
    s.preview().setScrollY(1200.0);
    CHECK_EQ(s.preview().scrollY(), 1200.0); // the editor's reaction did not move the preview back
    const std::size_t want = markdown::sourceOffsetForPreviewY(*s.preview().currentLayout(), 1200.0);
    const ui::TextBuffer& buffer = s.editor().buffer();
    CHECK_EQ(s.editor().topVisibleOffset(), buffer.lineStart(buffer.lineOfOffset(want)));
    // Repeating the same scroll is a no-op both ways.
    const double editorY = s.editor().scrollY();
    s.preview().setScrollY(1200.0);
    CHECK_EQ(s.editor().scrollY(), editorY);
    CHECK_EQ(s.preview().scrollY(), 1200.0);
    // A run of small steps stays monotonic on both sides.
    double lastEditor = s.editor().scrollY();
    for (int i = 0; i < 20; ++i) {
        s.preview().scrollBy(15.0);
        CHECK(s.editor().scrollY() >= lastEditor);
        lastEditor = s.editor().scrollY();
        CHECK_EQ(s.preview().scrollY(), 1200.0 + 15.0 * (i + 1));
    }
}

RIVET_TEST(syncScrollIsOffOutsideSplitMode) {
    HostFixture f;
    const std::string text = paragraphs(300);
    auto state = makeState(text, MarkdownDisplayMode::Source);
    f.bind(*state);
    f.drain();
    f.paint();
    f.editor().scrollToOffset(text.find("number 200 "));
    CHECK_EQ(f.host->previewView()->scrollY(), 0.0);
}

RIVET_TEST(syncScrollReappliesAfterAFreshSnapshotLands) {
    SplitScroll s;
    s.editor().scrollToOffset(s.text.find("number 150 "));
    s.f.paint();
    const double before = s.preview().scrollY();
    CHECK(before > 0.0);
    // An edit near the top shifts everything; the new snapshot must come in at the editor's position.
    s.editor().setCaretOffset(0);
    ui::testing::typeString(s.editor(), "new first paragraph\n\n");
    s.f.drain();
    s.f.paint();
    const markdown::MarkdownLayout* layout = s.preview().currentLayout();
    const double expected = std::min(markdown::previewYForSourceOffset(*layout, s.editor().topVisibleOffset()),
                                     s.preview().maxScrollY());
    CHECK(std::abs(s.preview().scrollY() - expected) < 1.0);
}

RIVET_TEST(hostClickInThePreviewMovesFocusThere) {
    HostFixture f;
    auto state = makeState("text\n", MarkdownDisplayMode::Split);
    f.bind(*state);
    CHECK(f.focused == &f.editor());
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Down, 600.0, 50.0));
    CHECK(f.focused == f.host->previewView());
    f.host->onMouse(ui::testing::pointer(ui::PointerEventType::Up, 600.0, 50.0));
}

// ------------------------------------------------------------- save bytes

RIVET_TEST(saveWritesTheEditedSourceVerbatimWithCrlfBomAndUnsupportedSyntax) {
    const std::string body = "# Title\r\n\r\n<div>raw html</div>\r\n\r\n[^1]: footnote text\r\n\r\n- [ ] task\r\n";
    const std::string bytes = "\xEF\xBB\xBF" + body;
    auto decoded = decodeMarkdownBytes(bytes);
    CHECK(decoded.has_value());
    MarkdownTabState state("save.md", std::move(*decoded));
    CHECK_EQ(state.encode(), bytes); // untouched by merely decoding

    HostFixture f;
    f.bind(state);
    state.setMode(MarkdownDisplayMode::Split);
    f.drain();
    CHECK_EQ(state.encode(), bytes); // rendering never rewrites the source
    ui::testing::typeString(f.editor(), "X");
    f.drain();
    CHECK_EQ(state.encode(), "\xEF\xBB\xBF" "X" + body); // one inserted byte, everything else verbatim
    CHECK(state.hasBom());
    CHECK(state.lineEnding() == LineEnding::CRLF);
    CHECK(state.undo());
    CHECK_EQ(state.encode(), bytes);
}
