// SPDX-License-Identifier: MPL-2.0
#include "app/SearchBarController.hpp"

#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::app {
namespace {

constexpr double kBarWidth = 360.0;
constexpr double kBarHeight = 34.0;

} // namespace

SearchBarController::SearchBarController(ShellContext& context, ui::Widget& parent)
    : context_(context) {
    auto bar = std::make_unique<ui::Container>();
    bar_ = bar.get();
    bar_->setBackgroundColor(ui::Color::rgba(0.97, 0.97, 0.97, 1.0));
    parent.addChild(std::move(bar));

    auto field = std::make_unique<ui::TextField>("Find");
    field_ = field.get();
    field_->setFrame(core::Rect{8.0, 5.0, 180.0, 24.0});
    field_->setOnFocusRequested([this] { context_.setFocus(field_); });
    field_->setOnTextChanged([this](const std::string& text) {
        if (DocumentTab* tab = context_.readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->start(text);
        }
    });
    field_->setOnEnter([this] { step(+1); });
    field_->setOnEscape([this] { setVisible(false); });
    bar_->addChild(std::move(field));

    auto previous = std::make_unique<ui::Button>("↑");
    previous->setFrame(core::Rect{196.0, 5.0, 28.0, 24.0});
    previous->setOnClick([this] { step(-1); });
    bar_->addChild(std::move(previous));

    auto next = std::make_unique<ui::Button>("↓");
    next->setFrame(core::Rect{228.0, 5.0, 28.0, 24.0});
    next->setOnClick([this] { step(+1); });
    bar_->addChild(std::move(next));

    auto count = std::make_unique<TextLabel>("", ui::Font{12.0}, ui::Color::gray(0.35),
                                             ui::TextAlign::Right);
    count->setFrame(core::Rect{264.0, 5.0, 90.0, 24.0});
    countLabel_ = count.get();
    bar_->addChild(std::move(count));

    // Hidden by default. Direct frame here: setVisible() relayouts the whole
    // shell, which is not fully built yet.
    bar_->setFrame(kHiddenFrame);
}

void SearchBarController::bindTab(DocumentTab& tab) {
    if (tab.search() != nullptr) {
        // Delivered on the main thread (TextSearchController marshals worker
        // progress through the dispatcher). A background tab's search may
        // still notify: only the active tab drives the bar and the view.
        DocumentTab* bound = &tab;
        tab.search()->setOnResultsChanged([this, bound] {
            if (context_.workspace.activeTab() != bound) return;
            updateSearchUi();
            revealActiveMatch();
            context_.viewport.invalidate();
        });
    }
    // A different tab has a different search; close the bar on switches.
    setVisible(false);
}

void SearchBarController::layout(const core::Rect& viewportFrame) {
    // Floats at the viewport's top-right; hidden as a zero-size frame (paints
    // nothing, consumes nothing).
    if (!visible_) {
        bar_->setFrame(kHiddenFrame);
        return;
    }
    const double barX = std::max(viewportFrame.origin.x, viewportFrame.maxX() - kBarWidth - 12.0);
    bar_->setFrame(core::Rect{barX, viewportFrame.origin.y + 8.0, kBarWidth, kBarHeight});
}

void SearchBarController::setVisible(bool visible) {
    visible_ = visible;
    if (!visible) {
        if (field_->isFocused()) context_.setFocus(nullptr);
        if (DocumentTab* tab = context_.readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->cancel();
        }
    }
    context_.relayout();
    context_.viewport.invalidate();
}

void SearchBarController::toggle() {
    setVisible(!visible_);
    if (visible_) context_.setFocus(field_);
}

bool SearchBarController::handleEscape() {
    if (!visible_) return false;
    setVisible(false);
    return true;
}

const std::string& SearchBarController::countText() const { return countLabel_->text(); }

void SearchBarController::step(int delta) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || tab->search() == nullptr) return;
    if (delta < 0) {
        tab->search()->previous();
    } else {
        tab->search()->next();
    }
    revealActiveMatch();
}

void SearchBarController::updateSearchUi() {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || tab->search() == nullptr) return;
    const editor::TextSearchController* search = tab->search();
    if (search->query().empty()) {
        countLabel_->setText("");
        return;
    }
    const std::size_t total = search->matches().size();
    const std::optional<std::size_t> active = search->currentIndex();
    if (total == 0) {
        countLabel_->setText(search->searching() ? "Searching…" : "No matches");
    } else {
        countLabel_->setText(std::format("{} / {}", active ? *active + 1 : 0, total));
    }
}

void SearchBarController::revealActiveMatch() {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || tab->search() == nullptr) return;
    const editor::TextSearchController* search = tab->search();
    const std::optional<std::size_t> active = search->currentIndex();
    if (!active.has_value()) return;
    const std::vector<editor::TextSearchController::Match> matches = search->matches();
    if (*active >= matches.size()) return;
    const editor::DocumentSession* session = tab->session();
    const std::size_t pageIndex = session->pageIndexFor(matches[*active].page);
    if (pageIndex == editor::DocumentSession::kInvalidPage) return;
    const std::shared_ptr<const pdf::PdfTextPage> page =
        session->textService().cachedTextPage(matches[*active].page);
    if (page == nullptr) return;
    const std::vector<core::Rect> rects =
        page->rectsForRange(matches[*active].startIndex, matches[*active].count);
    if (!rects.empty()) context_.viewport.revealContentRect(pageIndex, rects.front());
}

} // namespace rivet::app
