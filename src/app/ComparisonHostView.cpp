// SPDX-License-Identifier: MPL-2.0
#include "app/ComparisonHostView.hpp"

#include "app/TextLabel.hpp"
#include "ui/PaintContext.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace rivet::app {

namespace {

// Semi-transparent fills: they tint the page without hiding the text below,
// on white and on tinted paper alike.
constexpr ui::Color kDeletedFill = ui::Color::rgba(0.90, 0.15, 0.15, 0.30);
constexpr ui::Color kInsertedFill = ui::Color::rgba(0.10, 0.65, 0.20, 0.30);
constexpr ui::Color kDeletedSelected = ui::Color::rgba(0.90, 0.15, 0.15, 0.55);
constexpr ui::Color kInsertedSelected = ui::Color::rgba(0.10, 0.65, 0.20, 0.55);
constexpr ui::Color kDeletedPage = ui::Color::rgba(0.90, 0.15, 0.15, 0.12);
constexpr ui::Color kInsertedPage = ui::Color::rgba(0.10, 0.65, 0.20, 0.12);

constexpr std::size_t kSnippetBytes = 28;

// Cuts at a code point boundary and marks the cut.
std::string shorten(const std::string& text) {
    if (text.size() <= kSnippetBytes) return text;
    std::size_t end = kSnippetBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return text.substr(0, end) + "\xE2\x80\xA6";
}

std::string rowTitle(const editor::ComparisonChange& change) {
    using editor::ChangeKind;
    switch (change.kind) {
    case ChangeKind::Replaced:
        return std::format("p.{}: \"{}\" \xE2\x86\x92 \"{}\"", change.leftPage + 1, shorten(change.leftSnippet),
                           shorten(change.rightSnippet));
    case ChangeKind::Deleted:
        return std::format("p.{}: removed \"{}\"", change.leftPage + 1, shorten(change.leftSnippet));
    case ChangeKind::Inserted:
        return std::format("p.{}: added \"{}\"", change.rightPage + 1, shorten(change.rightSnippet));
    case ChangeKind::PageDeleted:
        return change.pageCount == 1
                   ? std::format("Page {} only in the left document", change.leftPage + 1)
                   : std::format("Pages {}-{} only in the left document", change.leftPage + 1,
                                 change.leftPage + change.pageCount);
    case ChangeKind::PageInserted:
        return change.pageCount == 1
                   ? std::format("Page {} only in the right document", change.rightPage + 1)
                   : std::format("Pages {}-{} only in the right document", change.rightPage + 1,
                                 change.rightPage + change.pageCount);
    }
    return {};
}

} // namespace

// Per-side bridge: overlay highlights only. No text hit testing, selection or
// links in the comparison viewports.
class ComparisonHostView::SideBridge final : public ui::IViewerTextBridge {
public:
    SideBridge(const ComparisonHostView& host, bool left) : host_(host), left_(left) {}

    void warmPage(std::size_t) override {}
    std::optional<std::uint32_t> charIndexAtPoint(std::size_t, const core::Point&) override {
        return std::nullopt;
    }
    std::vector<ui::OverlayRect> overlayRects(std::size_t pageIndex) override {
        return host_.overlaysFor(left_, pageIndex);
    }
    void selectionDragBegan(std::size_t, std::uint32_t, bool) override {}
    void selectionDragMoved(std::size_t, std::uint32_t) override {}
    void selectionDragEnded() override {}
    void selectionCleared() override {}
    std::optional<ui::ViewerLinkHit> linkAtPoint(std::size_t, const core::Point&) override {
        return std::nullopt;
    }
    std::vector<core::Rect> linkRects(std::size_t) override { return {}; }
    void linkActivated(const ui::ViewerLinkHit&) override {}

private:
    const ComparisonHostView& host_;
    bool left_;
};

ComparisonHostView::ComparisonHostView(Environment environment) : environment_(std::move(environment)) {
    auto previous = std::make_unique<ui::Button>("Previous");
    previousButton_ = previous.get();
    previousButton_->setOnClick([this] { previousChange(); });
    addChild(std::move(previous));

    auto next = std::make_unique<ui::Button>("Next");
    nextButton_ = next.get();
    nextButton_->setOnClick([this] { nextChange(); });
    addChild(std::move(next));

    auto summary = std::make_unique<TextLabel>("", ui::Font{13.0, ui::Font::Weight::Semibold},
                                               ui::Color::gray(0.25));
    summary_ = summary.get();
    addChild(std::move(summary));

    auto list = std::make_unique<ui::OutlinePanel>();
    list_ = list.get();
    list_->setOnRowActivated([this](std::size_t row) { selectChange(row); });
    addChild(std::move(list));

    auto leftCaption = std::make_unique<TextLabel>("", ui::Font{12.0, ui::Font::Weight::Semibold},
                                                   ui::Color::gray(0.30), ui::TextAlign::Center);
    leftCaption_ = leftCaption.get();
    addChild(std::move(leftCaption));
    auto rightCaption = std::make_unique<TextLabel>("", ui::Font{12.0, ui::Font::Weight::Semibold},
                                                    ui::Color::gray(0.30), ui::TextAlign::Center);
    rightCaption_ = rightCaption.get();
    addChild(std::move(rightCaption));

    leftBridge_ = std::make_unique<SideBridge>(*this, true);
    rightBridge_ = std::make_unique<SideBridge>(*this, false);

    auto left = std::make_unique<ui::PdfViewport>();
    leftViewport_ = left.get();
    leftViewport_->setTextBridge(leftBridge_.get());
    leftViewport_->setScrollChangedCallback([this] { syncScroll(true); });
    leftViewport_->setOnFocusRequested([this] {
        focusLeft_ = true;
        if (environment_.setFocus) environment_.setFocus(nullptr);
    });
    addChild(std::move(left));

    auto right = std::make_unique<ui::PdfViewport>();
    rightViewport_ = right.get();
    rightViewport_->setTextBridge(rightBridge_.get());
    rightViewport_->setScrollChangedCallback([this] { syncScroll(false); });
    rightViewport_->setOnFocusRequested([this] {
        focusLeft_ = false;
        if (environment_.setFocus) environment_.setFocus(nullptr);
    });
    addChild(std::move(right));

    auto status = std::make_unique<TextLabel>("", ui::Font{14.0, ui::Font::Weight::Semibold},
                                              ui::Color::gray(0.35), ui::TextAlign::Center);
    statusLabel_ = status.get();
    addChild(std::move(status));
}

ComparisonHostView::~ComparisonHostView() {
    detachViewports();
    leftViewport_->setTextBridge(nullptr);
    rightViewport_->setTextBridge(nullptr);
}

void ComparisonHostView::bind(ComparisonTabState* state) {
    if (state_ == state) {
        refresh();
        return;
    }
    detachViewports();
    state_ = state;
    selected_.reset();
    refresh();
}

void ComparisonHostView::detachViewports() {
    leftViewport_->clearDocument();
    rightViewport_->clearDocument();
    attachedLeft_ = nullptr;
    attachedRight_ = nullptr;
}

void ComparisonHostView::attachViewports() {
    if (state_ == nullptr) return;
    editor::DocumentSession* left = state_->leftSession();
    editor::DocumentSession* right = state_->rightSession();
    if (left == nullptr || right == nullptr) {
        detachViewports();
        return;
    }
    if (attachedLeft_ != left) {
        leftViewport_->setDocument(left->id(), &left->layout(), &left->renderSource(),
                                   [left] { return left->revision(); }, &state_->leftView());
        attachedLeft_ = left;
    }
    if (attachedRight_ != right) {
        rightViewport_->setDocument(right->id(), &right->layout(), &right->renderSource(),
                                    [right] { return right->revision(); }, &state_->rightView());
        attachedRight_ = right;
    }
    if (!state_->viewsInitialized()) {
        state_->markViewsInitialized();
        leftViewport_->setFitMode(render::ZoomState::FitMode::Width);
        rightViewport_->setFitMode(render::ZoomState::FitMode::Width);
    }
}

void ComparisonHostView::rebuildChanges() {
    leftExtraPages_.clear();
    rightExtraPages_.clear();
    std::vector<ui::OutlineRow> rows;
    const editor::ComparisonResult* result = state_ != nullptr ? state_->result() : nullptr;
    if (result != nullptr) {
        rows.reserve(result->changes.size());
        for (const editor::ComparisonChange& change : result->changes) {
            rows.push_back(ui::OutlineRow{rowTitle(change), 0, false, false});
            if (change.kind == editor::ChangeKind::PageDeleted) {
                leftExtraPages_.resize(std::max(leftExtraPages_.size(), change.leftPage + change.pageCount), false);
                for (std::size_t i = 0; i < change.pageCount; ++i) leftExtraPages_[change.leftPage + i] = true;
            } else if (change.kind == editor::ChangeKind::PageInserted) {
                rightExtraPages_.resize(std::max(rightExtraPages_.size(), change.rightPage + change.pageCount),
                                        false);
                for (std::size_t i = 0; i < change.pageCount; ++i) rightExtraPages_[change.rightPage + i] = true;
            }
        }
        if (selected_.has_value() && *selected_ >= rows.size()) selected_.reset();
    } else {
        selected_.reset();
    }
    list_->setRows(std::move(rows));
    list_->setSelectedIndex(selected_);
}

void ComparisonHostView::updateLabels() {
    std::string summary;
    std::string status;
    if (state_ == nullptr) {
        // nothing bound
    } else {
        leftCaption_->setText(state_->leftPath().filename().string());
        rightCaption_->setText(state_->rightPath().filename().string());
        switch (state_->phase()) {
        case ComparisonTabState::Phase::Opening:
            summary = "Opening documents\xE2\x80\xA6";
            status = "Opening documents\xE2\x80\xA6";
            break;
        case ComparisonTabState::Phase::Comparing:
            summary = "Comparing\xE2\x80\xA6";
            break;
        case ComparisonTabState::Phase::Failed:
            summary = "Comparison failed";
            status = state_->errorText();
            break;
        case ComparisonTabState::Phase::Ready: {
            const editor::ComparisonResult* result = state_->result();
            if (result == nullptr || result->changes.empty()) {
                summary = "No differences found";
            } else {
                const std::size_t count = result->changes.size();
                summary = std::format("{} difference{}", count, count == 1 ? "" : "s");
            }
            if (result != nullptr && result->approximatePages > 0) {
                summary += std::format(" ({} page{} compared by line)", result->approximatePages,
                                       result->approximatePages == 1 ? "" : "s");
            }
            if (result != nullptr && result->extractionFailures > 0) {
                summary += std::format(" - text unavailable on {} page{}", result->extractionFailures,
                                       result->extractionFailures == 1 ? "" : "s");
            }
            break;
        }
        }
    }
    if (state_ == nullptr) {
        leftCaption_->setText("");
        rightCaption_->setText("");
    }
    summary_->setText(std::move(summary));
    statusLabel_->setText(std::move(status));
}

void ComparisonHostView::refresh() {
    if (state_ == nullptr) {
        detachViewports();
    } else {
        attachViewports();
    }
    rebuildChanges();
    updateLabels();
    layout();
    invalidate();
}

void ComparisonHostView::selectChange(std::size_t index) {
    const editor::ComparisonResult* result = state_ != nullptr ? state_->result() : nullptr;
    if (result == nullptr || index >= result->changes.size()) return;
    selected_ = index;
    list_->setSelectedIndex(selected_);
    list_->revealRow(index);
    const editor::ComparisonChange& change = result->changes[index];
    // Reveal on one side only: the scroll synchronization brings the other
    // side to the same page position.
    const bool revealLeft = !change.leftRect.isEmpty() ||
                            (change.kind == editor::ChangeKind::PageDeleted);
    const bool revealRight = !change.rightRect.isEmpty() || change.kind == editor::ChangeKind::PageInserted;
    syncing_ = false;
    if (revealLeft && !change.leftRect.isEmpty()) {
        leftViewport_->revealContentRect(change.leftPage, change.leftRect);
    } else if (revealLeft) {
        leftViewport_->goToPage(change.leftPage);
    } else if (revealRight && !change.rightRect.isEmpty()) {
        rightViewport_->revealContentRect(change.rightPage, change.rightRect);
    } else if (revealRight) {
        rightViewport_->goToPage(change.rightPage);
    }
    leftViewport_->invalidate();
    rightViewport_->invalidate();
}

void ComparisonHostView::nextChange() {
    const editor::ComparisonResult* result = state_ != nullptr ? state_->result() : nullptr;
    if (result == nullptr || result->changes.empty()) return;
    const std::size_t count = result->changes.size();
    selectChange(selected_.has_value() ? (*selected_ + 1) % count : 0);
}

void ComparisonHostView::previousChange() {
    const editor::ComparisonResult* result = state_ != nullptr ? state_->result() : nullptr;
    if (result == nullptr || result->changes.empty()) return;
    const std::size_t count = result->changes.size();
    selectChange(selected_.has_value() ? (*selected_ + count - 1) % count : count - 1);
}

const render::PageLayout* ComparisonHostView::layoutOf(bool leftSide) const {
    if (state_ == nullptr) return nullptr;
    const editor::DocumentSession* session = leftSide ? attachedLeft_ : attachedRight_;
    return session != nullptr ? &session->layout() : nullptr;
}

std::vector<ui::OverlayRect> ComparisonHostView::overlaysFor(bool leftSide, std::size_t pageIndex) const {
    std::vector<ui::OverlayRect> out;
    const editor::ComparisonResult* result = state_ != nullptr ? state_->result() : nullptr;
    if (result == nullptr) return out;

    const std::vector<bool>& extra = leftSide ? leftExtraPages_ : rightExtraPages_;
    const render::PageLayout* layout = layoutOf(leftSide);
    if (pageIndex < extra.size() && extra[pageIndex] && layout != nullptr && pageIndex < layout->pageCount()) {
        const core::Size size = layout->pages()[pageIndex].sizePoints;
        out.push_back(ui::OverlayRect{core::Rect{0.0, 0.0, size.width, size.height},
                                      leftSide ? kDeletedPage : kInsertedPage});
    }
    if (pageIndex < result->pages.size()) {
        const editor::PageComparison& page = result->pages[pageIndex];
        for (const core::Rect& rect : leftSide ? page.deletedRects : page.insertedRects) {
            out.push_back(ui::OverlayRect{rect, leftSide ? kDeletedFill : kInsertedFill});
        }
    }
    if (selected_.has_value() && *selected_ < result->changes.size()) {
        const editor::ComparisonChange& change = result->changes[*selected_];
        const std::size_t first = leftSide ? change.leftPage : change.rightPage;
        const core::Rect& rect = leftSide ? change.leftRect : change.rightRect;
        const bool pageChange = leftSide ? change.kind == editor::ChangeKind::PageDeleted
                                         : change.kind == editor::ChangeKind::PageInserted;
        if (pageChange) {
            if (pageIndex >= first && pageIndex < first + change.pageCount && layout != nullptr &&
                pageIndex < layout->pageCount()) {
                const core::Size size = layout->pages()[pageIndex].sizePoints;
                out.push_back(ui::OverlayRect{core::Rect{0.0, 0.0, size.width, size.height},
                                              leftSide ? kDeletedPage : kInsertedPage});
            }
        } else if (first == pageIndex && !rect.isEmpty()) {
            out.push_back(ui::OverlayRect{core::Rect{rect.minX() - 1.0, rect.minY() - 1.0, rect.size.width + 2.0,
                                                     rect.size.height + 2.0},
                                          leftSide ? kDeletedSelected : kInsertedSelected});
        }
    }
    return out;
}

// Maps the scroll position of one side to the same page position on the
// other: page index plus the fraction of that page (content-space points, so
// independent of each side's zoom). Above the first page the raw offset is
// carried over; pages the other document does not have leave it alone.
void ComparisonHostView::syncScroll(bool fromLeft) {
    if (syncing_) return;
    const render::PageLayout* source = layoutOf(fromLeft);
    const render::PageLayout* target = layoutOf(!fromLeft);
    if (source == nullptr || target == nullptr || source->pageCount() == 0 || target->pageCount() == 0) return;

    ui::PdfViewport& from = viewportOf(fromLeft);
    ui::PdfViewport& to = viewportOf(!fromLeft);
    const double y = from.scrollOffsetPoints().y;

    double wanted = y;
    if (y >= source->pageTopOffsetPoints(0)) {
        std::size_t lo = 0;
        std::size_t hi = source->pageCount();
        while (lo + 1 < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (source->pageTopOffsetPoints(mid) <= y) lo = mid; else hi = mid;
        }
        if (lo >= target->pageCount()) return;
        const double span = source->pageFramePoints(lo).size.height + source->pageGapPoints();
        const double fraction = span > 0.0 ? std::clamp((y - source->pageTopOffsetPoints(lo)) / span, 0.0, 1.0) : 0.0;
        const double targetSpan = target->pageFramePoints(lo).size.height + target->pageGapPoints();
        wanted = target->pageTopOffsetPoints(lo) + fraction * targetSpan;
    }
    const core::Point current = to.scrollOffsetPoints();
    if (std::abs(current.y - wanted) < 0.01) return;
    syncing_ = true;
    to.setScrollOffsetPoints(core::Point{current.x, wanted});
    syncing_ = false;
}

bool ComparisonHostView::onKey(const ui::KeyEvent& event) {
    return viewportOf(focusLeft_).onKey(event);
}

void ComparisonHostView::layout() {
    const double width = frame().size.width;
    const double height = frame().size.height;
    previousButton_->setFrame(core::Rect{8.0, 6.0, 76.0, 24.0});
    nextButton_->setFrame(core::Rect{90.0, 6.0, 76.0, 24.0});
    summary_->setFrame(core::Rect{176.0, 0.0, std::max(0.0, width - 184.0), kHeaderHeight});

    const double bodyTop = kHeaderHeight;
    const double bodyHeight = std::max(0.0, height - bodyTop);
    const double listWidth = std::min(kListWidth, width * 0.4);
    list_->setFrame(core::Rect{0.0, bodyTop, listWidth, bodyHeight});

    const double paneLeft = listWidth + 1.0;
    const double paneWidth = std::max(0.0, width - paneLeft);
    const double half = std::floor(paneWidth / 2.0);
    const double viewportTop = bodyTop + kCaptionHeight;
    const double viewportHeight = std::max(0.0, bodyHeight - kCaptionHeight);
    leftCaption_->setFrame(core::Rect{paneLeft, bodyTop, half, kCaptionHeight});
    rightCaption_->setFrame(core::Rect{paneLeft + half + 1.0, bodyTop, std::max(0.0, paneWidth - half - 1.0),
                                       kCaptionHeight});
    leftViewport_->setFrame(core::Rect{paneLeft, viewportTop, half, viewportHeight});
    rightViewport_->setFrame(core::Rect{paneLeft + half + 1.0, viewportTop,
                                        std::max(0.0, paneWidth - half - 1.0), viewportHeight});
    statusLabel_->setFrame(core::Rect{paneLeft, viewportTop, paneWidth, viewportHeight});
}

void ComparisonHostView::paintSelf(ui::PaintContext& context) const {
    const double width = frame().size.width;
    const double height = frame().size.height;
    context.fillRect(core::Rect{0.0, 0.0, width, height}, ui::Color::gray(0.93));
    context.fillRect(core::Rect{0.0, 0.0, width, kHeaderHeight}, ui::Color::gray(0.96));
    context.drawLine(core::Point{0.0, kHeaderHeight - 0.5}, core::Point{width, kHeaderHeight - 0.5},
                     ui::Color::gray(0.80), 1.0);
    const core::Rect& list = list_->frame();
    context.fillRect(list, ui::Color::white());
    context.fillRect(core::Rect{list.maxX(), kHeaderHeight, 1.0, std::max(0.0, height - kHeaderHeight)},
                     ui::Color::gray(0.80));
    const core::Rect& right = rightViewport_->frame();
    context.fillRect(core::Rect{right.minX() - 1.0, kHeaderHeight, 1.0, std::max(0.0, height - kHeaderHeight)},
                     ui::Color::gray(0.80));
}

} // namespace rivet::app
