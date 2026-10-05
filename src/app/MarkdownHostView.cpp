// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownHostView.hpp"

#include "app/MarkdownPreviewView.hpp"
#include "app/MarkdownSourcePane.hpp"
#include "app/MarkdownSourceSearch.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kDividerWidth = 1.0;
constexpr double kDividerGrabWidth = 5.0; // invisible hit area around the 1 px line
constexpr double kMinPaneWidth = 80.0;

bool sourceShown(MarkdownDisplayMode mode) { return mode != MarkdownDisplayMode::Rendered; }
bool previewShown(MarkdownDisplayMode mode) { return mode != MarkdownDisplayMode::Source; }

// The wide invisible handle over the divider: dragging reports the desired
// divider x in host coordinates.
class DividerHandle final : public ui::Widget {
public:
    explicit DividerHandle(std::function<void(double)> onDragTo) : onDragTo_(std::move(onDragTo)) {}

    bool onMouse(const ui::PointerEvent& event) override {
        if (event.type == ui::PointerEventType::Down && event.button == 1) {
            dragging_ = true;
            grabX_ = event.position.x;
            event.accepted = true;
            return true;
        }
        if (event.type == ui::PointerEventType::Move && dragging_) {
            if (event.button == 0) {
                dragging_ = false;
                return false;
            }
            onDragTo_(frame().origin.x + event.position.x - grabX_ + frame().size.width / 2.0);
            return true;
        }
        if (event.type == ui::PointerEventType::Up && dragging_) {
            dragging_ = false;
            return true;
        }
        return false;
    }

private:
    std::function<void(double)> onDragTo_;
    bool dragging_ = false;
    double grabX_ = 0.0;
};

// One ISearchTarget for the whole binding that forwards to the view the
// current mode searches (preview in Rendered, the source editor otherwise)
// and replays its query on the new target when that changes.
class SearchRouter final : public ISearchTarget {
public:
    void attach(ISearchTarget& target) {
        ISearchTarget* raw = &target;
        target.setOnSearchResultsChanged([this, raw] {
            if (raw == delegate_ && onChanged_) onChanged_();
        });
    }

    void setDelegate(ISearchTarget* next) {
        if (next == delegate_) return;
        ISearchTarget* old = delegate_;
        delegate_ = nullptr; // the old target's "cleared" notification must not reach the bar
        if (old != nullptr && !query_.empty()) old->startSearch({});
        delegate_ = next;
        if (delegate_ != nullptr && !query_.empty()) delegate_->startSearch(query_);
        if (onChanged_) onChanged_();
    }
    // The delegate's content was replaced (another tab): run the query again.
    void restart() {
        if (delegate_ != nullptr && !query_.empty()) delegate_->startSearch(query_);
    }
    void clear() { query_.clear(); }

    void startSearch(std::string query) override {
        query_ = std::move(query);
        if (delegate_ != nullptr) delegate_->startSearch(query_);
    }
    void cancelSearch() override {
        if (delegate_ != nullptr) delegate_->cancelSearch();
    }
    void nextMatch() override {
        if (delegate_ != nullptr) delegate_->nextMatch();
    }
    void previousMatch() override {
        if (delegate_ != nullptr) delegate_->previousMatch();
    }
    const std::string& searchQuery() const override { return query_; }
    std::size_t matchCount() const override { return delegate_ != nullptr ? delegate_->matchCount() : 0; }
    std::optional<std::size_t> currentMatch() const override {
        return delegate_ != nullptr ? delegate_->currentMatch() : std::nullopt;
    }
    bool searching() const override { return delegate_ != nullptr && delegate_->searching(); }
    void revealCurrentMatch() override {
        if (delegate_ != nullptr) delegate_->revealCurrentMatch();
    }
    void setOnSearchResultsChanged(std::function<void()> onChanged) override { onChanged_ = std::move(onChanged); }

private:
    ISearchTarget* delegate_ = nullptr;
    std::string query_;
    std::function<void()> onChanged_;
};

class MarkdownHost final : public MarkdownHostView {
public:
    explicit MarkdownHost(MarkdownHostEnvironment environment) : MarkdownHostView(std::move(environment)) {
        const MarkdownHostEnvironment& host = this->environment();
        const platform::ShellServices* services = host.services;

        MarkdownPreviewEnvironment env;
        env.dispatcher = host.dispatcher;
        env.scheduler = host.scheduler;
        if (services != nullptr) {
            env.imageDecoder = services->imageDecoder;
            env.clipboard = services->clipboard;
            if (services->urlOpener != nullptr) {
                platform::IExternalUrlOpener* opener = services->urlOpener;
                env.openUrl = [opener](const std::string& url) { return opener->openUrl(url); };
            }
        }
        env.setStatus = host.setStatus;
        auto preview = std::make_unique<MarkdownPreviewView>(std::move(env));
        preview_ = preview.get();
        addChild(std::move(preview));

        MarkdownSourcePaneEnvironment penv;
        penv.services = services;
        penv.setStatus = host.setStatus;
        penv.setFocus = host.setFocus;
        auto pane = createMarkdownSourcePane(std::move(penv));
        pane_ = pane.get();
        addChild(std::move(pane));

        auto handle = std::make_unique<DividerHandle>([this](double x) { dragDividerTo(x); });
        handle_ = handle.get();
        addChild(std::move(handle));

        sourceSearch_ = std::make_unique<MarkdownSourceSearch>(pane_->editor());
        router_.attach(*preview_);
        router_.attach(*sourceSearch_);

        MarkdownParseEnvironment parse;
        parse.dispatcher = host.dispatcher;
        parse.scheduler = host.scheduler;
        parse.runBackground = host.runBackground;
        coordinator_ = std::make_unique<MarkdownParseCoordinator>(std::move(parse), host.parseTuning);
        coordinator_->setOnParsed([this](MarkdownParseResult result) { onParsed(std::move(result)); });

        pane_->editor().setOnScrolled([this] { onEditorScrolled(); });
        pane_->editor().setOnTextChanged([this] { sourceSearch_->refresh(); });
        preview_->setOnScrolled([this] { onPreviewScrolled(); });
    }

    ~MarkdownHost() override {
        // Wait for running parse jobs first; nothing is delivered afterwards.
        coordinator_.reset();
        pane_->editor().setOnScrolled({});
        pane_->editor().setOnTextChanged({});
        preview_->setOnScrolled({});
    }

    void bind(MarkdownTabState* state) override {
        if (state == state_ && state != nullptr) {
            stateChanged();
            return;
        }
        rememberPreviewPosition();
        coordinator_->unbind();
        state_ = state;
        lastRequested_.reset();
        preview_->clearDocument(); // also forgets the preview's search state
        if (state == nullptr) {
            const bool ownedFocus = pane_->editor().isFocused() || preview_->isFocused();
            pane_->bind(nullptr);
            sourceSearch_->reset();
            router_.setDelegate(nullptr);
            router_.clear();
            if (ownedFocus && environment().setFocus) environment().setFocus(nullptr);
            updateFrames();
            invalidate();
            return;
        }

        appliedMode_ = state->mode();
        preview_->setBaseDirectory(state->path().parent_path());
        pane_->bind(sourceShown(appliedMode_) ? state : nullptr);
        sourceSearch_->reset();
        coordinator_->bind([this] { return MarkdownParseCoordinator::Snapshot{state_->source(), state_->revision()}; });
        router_.setDelegate(delegateFor(appliedMode_));
        router_.restart();

        // Where the user was: Rendered restores the preview's remembered top;
        // with an editor the editor's own remembered caret/scroll lead.
        if (appliedMode_ == MarkdownDisplayMode::Rendered) {
            if (const auto it = positions_.find(state->instanceId()); it != positions_.end()) {
                preview_->scrollToSourceOffset(it->second);
            }
        }
        requestParse(true);
        if (appliedMode_ == MarkdownDisplayMode::Split) syncPreviewFromEditor();
        focusForMode(appliedMode_, false);
        updateFrames();
        invalidate();
    }

    void stateChanged() override {
        if (state_ == nullptr) return;
        const MarkdownDisplayMode mode = state_->mode();
        const bool modeChanged = mode != appliedMode_;
        if (modeChanged) {
            changeMode(mode);
        } else if (sourceShown(mode)) {
            pane_->stateChanged();
            sourceSearch_->refresh();
        }
        requestParse(modeChanged);
        updateFrames();
        invalidate();
    }

    ISearchTarget* searchTarget() override { return state_ != nullptr ? &router_ : nullptr; }

    bool copySelection() override {
        if (state_ == nullptr || !previewShown(appliedMode_)) return false;
        if (appliedMode_ == MarkdownDisplayMode::Split && pane_->editor().isFocused()) return false;
        return preview_->copySelection();
    }
    bool selectAll() override {
        if (state_ == nullptr || !previewShown(appliedMode_)) return false;
        if (appliedMode_ == MarkdownDisplayMode::Split && pane_->editor().isFocused()) return false;
        return preview_->selectAll();
    }

    Panes panes() const override { return panes_; }
    MarkdownPreviewView* previewView() override { return preview_; }
    MarkdownSourcePane* sourcePane() override { return pane_; }
    const MarkdownParseCoordinator* parseCoordinator() const override { return coordinator_.get(); }

    bool onMouse(const ui::PointerEvent& event) override {
        // A click in the preview moves keyboard focus there (copy / scroll keys).
        if (event.type == ui::PointerEventType::Down && state_ != nullptr && previewShown(appliedMode_) &&
            panes_.preview.contains(event.position) && !handle_->frame().contains(event.position)) {
            if (environment().setFocus) environment().setFocus(preview_);
        }
        return MarkdownHostView::onMouse(event);
    }

    bool onKey(const ui::KeyEvent& event) override {
        if (state_ == nullptr || !previewShown(appliedMode_)) return false;
        return preview_->onKey(event);
    }
    void layout() override { updateFrames(); }

    void paintSelf(ui::PaintContext& context) const override {
        if (state_ == nullptr) {
            context.fillRect(bounds(), ui::Color::white());
            return;
        }
        if (appliedMode_ == MarkdownDisplayMode::Split) {
            context.fillRect(panes_.divider, ui::Color::gray(0.8));
        }
    }

private:
    // ------------------------------------------------------------- layout
    void updateFrames() {
        const core::Rect all = bounds();
        Panes panes;
        if (state_ != nullptr) {
            switch (appliedMode_) {
            case MarkdownDisplayMode::Rendered: panes.preview = all; break;
            case MarkdownDisplayMode::Source: panes.source = all; break;
            case MarkdownDisplayMode::Split: {
                const double usable = std::max(0.0, all.size.width - kDividerWidth);
                const double lo = std::min(kMinPaneWidth, usable / 2.0);
                const double left = std::floor(std::clamp(usable * splitRatio_, lo, usable - lo));
                panes.source = core::Rect{0.0, 0.0, left, all.size.height};
                panes.divider = core::Rect{left, 0.0, kDividerWidth, all.size.height};
                panes.preview = core::Rect{left + kDividerWidth, 0.0, usable - left, all.size.height};
                break;
            }
            }
        }
        panes_ = panes;
        pane_->setFrame(panes.source);
        preview_->setFrame(panes.preview);
        handle_->setFrame(panes.divider.isEmpty()
                              ? core::Rect{}
                              : core::Rect{panes.divider.origin.x - (kDividerGrabWidth - kDividerWidth) / 2.0, 0.0,
                                           kDividerGrabWidth, panes.divider.size.height});
    }

    void dragDividerTo(double x) {
        const double usable = std::max(1.0, bounds().size.width - kDividerWidth);
        splitRatio_ = std::clamp(x / usable, 0.0, 1.0);
        updateFrames();
        invalidate();
    }

    // -------------------------------------------------------------- modes
    ISearchTarget* delegateFor(MarkdownDisplayMode mode) {
        return mode == MarkdownDisplayMode::Rendered ? static_cast<ISearchTarget*>(preview_) : sourceSearch_.get();
    }

    void changeMode(MarkdownDisplayMode next) {
        const MarkdownDisplayMode prior = appliedMode_;
        // Where the user was looking, read before anything moves.
        const std::optional<std::size_t> fromPreview =
            previewShown(prior) ? preview_->topSourceOffset() : std::nullopt;
        const std::size_t fromEditor = sourceShown(prior) ? pane_->editor().topVisibleOffset() : 0;

        appliedMode_ = next;
        pane_->bind(sourceShown(next) ? state_ : nullptr);
        sourceSearch_->reset();

        if (!sourceShown(prior) && sourceShown(next) && fromPreview) {
            const SyncGuard guard(*this);
            pane_->editor().scrollToOffset(*fromPreview);
        } else if (!previewShown(prior) && previewShown(next) && sourceShown(prior)) {
            const SyncGuard guard(*this);
            preview_->scrollToSourceOffset(fromEditor);
        }

        router_.setDelegate(delegateFor(next));
        router_.restart();
        sourceSearch_->refresh();
        focusForMode(next, true);
    }

    void focusForMode(MarkdownDisplayMode mode, bool force) {
        const auto& setFocus = environment().setFocus;
        if (!setFocus) return;
        if (sourceShown(mode)) {
            pane_->focusEditor();
        } else if (force) {
            setFocus(preview_);
        } else if (pane_->editor().isFocused()) {
            setFocus(nullptr);
        }
    }

    // -------------------------------------------------------------- parsing
    void requestParse(bool urgent) {
        if (state_ == nullptr || !previewShown(appliedMode_)) return;
        const std::uint64_t revision = state_->revision();
        if (preview_->document() != nullptr && preview_->revision() == revision) return;
        if (lastRequested_ && *lastRequested_ == revision) return;
        lastRequested_ = revision;
        const bool immediate = urgent || appliedMode_ == MarkdownDisplayMode::Rendered || preview_->document() == nullptr;
        coordinator_->requestParse(revision, immediate ? MarkdownParseCoordinator::Urgency::Immediate
                                                       : MarkdownParseCoordinator::Urgency::Debounced);
    }

    void onParsed(MarkdownParseResult result) {
        if (state_ == nullptr || result.document == nullptr) return;
        const std::size_t problems = result.document->diagnostics.size();
        state_->setParseDiagnostics(result.document->diagnostics);
        if (problems != lastDiagnosticCount_) {
            lastDiagnosticCount_ = problems;
            if (problems > 0 && environment().setStatus) {
                environment().setStatus("Preview: parse limits applied — " + result.document->diagnostics.front().message +
                                        (problems > 1 ? " (+" + std::to_string(problems - 1) + " more)" : std::string()));
            }
        }
        preview_->setDocumentSnapshot(std::move(result.document), result.revision, std::move(result.source));
        if (appliedMode_ == MarkdownDisplayMode::Split) syncPreviewFromEditor();
        invalidate();
    }

    // ---------------------------------------------------------- sync scroll
    struct SyncGuard {
        explicit SyncGuard(MarkdownHost& h) : host(h), previous(h.syncing_) { host.syncing_ = true; }
        ~SyncGuard() { host.syncing_ = previous; }
        SyncGuard(const SyncGuard&) = delete;
        SyncGuard& operator=(const SyncGuard&) = delete;
        MarkdownHost& host;
        bool previous;
    };

    // Split only; a programmatic scroll's echo (the other pane's callback) is
    // swallowed by `syncing_`, so the panes never chase each other.
    void onEditorScrolled() {
        if (syncing_ || appliedMode_ != MarkdownDisplayMode::Split) return;
        syncPreviewFromEditor();
    }
    void onPreviewScrolled() {
        if (syncing_ || appliedMode_ != MarkdownDisplayMode::Split) return;
        if (const std::optional<std::size_t> top = preview_->topSourceOffset()) {
            const SyncGuard guard(*this);
            pane_->editor().scrollToOffset(*top);
        }
    }
    void syncPreviewFromEditor() {
        const SyncGuard guard(*this);
        preview_->scrollToSourceOffset(pane_->editor().topVisibleOffset());
    }

    void rememberPreviewPosition() {
        if (state_ == nullptr) return;
        if (const std::optional<std::size_t> top = preview_->topSourceOffset()) positions_[state_->instanceId()] = *top;
    }

    MarkdownPreviewView* preview_ = nullptr;
    MarkdownSourcePane* pane_ = nullptr;
    DividerHandle* handle_ = nullptr;
    std::unique_ptr<MarkdownSourceSearch> sourceSearch_;
    SearchRouter router_;
    std::unique_ptr<MarkdownParseCoordinator> coordinator_;

    MarkdownDisplayMode appliedMode_ = MarkdownDisplayMode::Rendered;
    Panes panes_;
    double splitRatio_ = 0.5;
    bool syncing_ = false;
    std::optional<std::uint64_t> lastRequested_;
    std::size_t lastDiagnosticCount_ = 0;
    std::unordered_map<std::uint64_t, std::size_t> positions_; // tab instance id -> preview top source offset
};

} // namespace

std::unique_ptr<MarkdownHostView> createMarkdownHostView(MarkdownHostEnvironment environment) {
    return std::make_unique<MarkdownHost>(std::move(environment));
}

} // namespace rivet::app
