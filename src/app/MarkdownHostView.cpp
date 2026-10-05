// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownHostView.hpp"

#include "app/MarkdownPreviewView.hpp"
#include "markdown/MarkdownParser.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace rivet::app {

namespace {

const char* modeName(MarkdownDisplayMode mode) {
    switch (mode) {
    case MarkdownDisplayMode::Rendered: return "Rendered";
    case MarkdownDisplayMode::Source: return "Source";
    case MarkdownDisplayMode::Split: return "Split";
    }
    return "";
}

// Rendered mode: the preview widget, fed with a synchronously parsed
// snapshot of the tab's source whenever its revision changes. Source/Split:
// the placeholder text (mode name and first source lines).
//
// Lifetime: bind() (including bind(nullptr)) saves the scroll position into a
// side table keyed by the state and drops the preview's document; the preview
// never holds a pointer into the state. A stale table entry (state destroyed,
// address reused) only restores a clamped scroll offset.
class MarkdownHost final : public MarkdownHostView {
public:
    explicit MarkdownHost(MarkdownHostEnvironment environment) : MarkdownHostView(std::move(environment)) {
        const platform::ShellServices* services = this->environment().services;
        MarkdownPreviewEnvironment env;
        env.dispatcher = this->environment().dispatcher;
        env.scheduler = this->environment().scheduler;
        if (services != nullptr) {
            env.imageDecoder = services->imageDecoder;
            env.clipboard = services->clipboard;
            if (services->urlOpener != nullptr) {
                platform::IExternalUrlOpener* opener = services->urlOpener;
                env.openUrl = [opener](const std::string& url) { return opener->openUrl(url); };
            }
        }
        env.setStatus = this->environment().setStatus;
        auto preview = std::make_unique<MarkdownPreviewView>(std::move(env));
        preview_ = preview.get();
        addChild(std::move(preview));
    }

    void bind(MarkdownTabState* state) override {
        if (state_ != nullptr) scrolls_[state_] = preview_->scrollY();
        state_ = state;
        preview_->clearDocument();
        if (state != nullptr) {
            preview_->setBaseDirectory(state->path().parent_path());
            refresh();
            if (const auto it = scrolls_.find(state); it != scrolls_.end()) preview_->setScrollY(it->second);
        }
        updateFrames();
        invalidate();
    }

    void stateChanged() override {
        refresh();
        updateFrames();
        invalidate();
    }

    ISearchTarget* searchTarget() override { return rendered() ? preview_ : nullptr; }
    bool copySelection() override { return rendered() && preview_->copySelection(); }
    bool selectAll() override { return rendered() && preview_->selectAll(); }

    bool onKey(const ui::KeyEvent& event) override { return rendered() && preview_->onKey(event); }
    void layout() override { updateFrames(); }

    void paint(ui::PaintContext& context) const override {
        paintSelf(context);
        if (rendered()) paintChildren(context);
    }

    void paintSelf(ui::PaintContext& context) const override {
        if (rendered() || state_ == nullptr) {
            if (state_ == nullptr) context.fillRect(bounds(), ui::Color::white());
            return;
        }
        paintPlaceholder(context);
    }

private:
    bool rendered() const { return state_ != nullptr && state_->mode() == MarkdownDisplayMode::Rendered; }

    void updateFrames() {
        preview_->setFrame(rendered() ? bounds() : core::Rect{});
    }

    // Parses on a revision change (synchronously; large documents are the
    // background-parse follow-up).
    void refresh() {
        if (!rendered()) return; // Source/Split do not need the rendered snapshot
        if (preview_->document() != nullptr && preview_->revision() == state_->revision()) return;
        auto document = std::make_shared<const markdown::MarkdownDocument>(
            markdown::makeMarkdownParser()->parse(state_->source()));
        preview_->setDocumentSnapshot(std::move(document), state_->revision(), state_->source());
    }

    void paintPlaceholder(ui::PaintContext& context) const {
        const core::Rect rect = bounds();
        context.fillRect(rect, ui::Color::white());
        constexpr double kLineHeight = 18.0;
        constexpr double kPad = 16.0;
        const ui::Font font{13.0};
        context.drawText(std::string("Markdown tab — ") + modeName(state_->mode()) + " mode",
                         core::Rect{kPad, 8.0, std::max(0.0, rect.size.width - 2 * kPad), kLineHeight},
                         ui::Font{13.0, ui::Font::Weight::Semibold}, ui::Color::gray(0.35), ui::TextAlign::Left);
        std::string_view rest = state_->source();
        double y = 8.0 + 2 * kLineHeight;
        while (!rest.empty() && y + kLineHeight <= rect.size.height) {
            const std::size_t eol = rest.find('\n');
            const std::string_view line = rest.substr(0, eol);
            context.drawText(line, core::Rect{kPad, y, std::max(0.0, rect.size.width - 2 * kPad), kLineHeight},
                             font, ui::Color::gray(0.15), ui::TextAlign::Left);
            y += kLineHeight;
            if (eol == std::string_view::npos) break;
            rest.remove_prefix(eol + 1);
        }
    }

    MarkdownPreviewView* preview_ = nullptr;
    std::unordered_map<const MarkdownTabState*, double> scrolls_;
};

} // namespace

std::unique_ptr<MarkdownHostView> createMarkdownHostView(MarkdownHostEnvironment environment) {
    return std::make_unique<MarkdownHost>(std::move(environment));
}

} // namespace rivet::app
