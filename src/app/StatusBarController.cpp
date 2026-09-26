// SPDX-License-Identifier: MPL-2.0
#include "app/StatusBarController.hpp"

#include "app/TextLabel.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

namespace rivet::app {
namespace {

constexpr double kPageFieldWidth = 44.0;
constexpr double kPageCountWidth = 64.0;

// Page indicator text, "12" style; page numbers shown are 1-based.
std::string pageFieldText(std::size_t page) { return std::format("{}", page + 1); }

} // namespace

std::string zoomPercentText(double zoom) { return std::format("{:.0f}%", zoom * 100.0); }

StatusBarController::StatusBarController(ShellContext& context, ui::Widget& parent,
                                         std::string initialStatus)
    : context_(context) {
    auto bar = std::make_unique<ui::Container>();
    bar_ = bar.get();
    bar_->setBackgroundColor(ui::Color::rgba(0.93, 0.93, 0.93, 1.0));
    parent.addChild(std::move(bar));

    auto statusLabel = std::make_unique<TextLabel>(std::move(initialStatus), ui::Font{12.0},
                                                   ui::Color::gray(0.35), ui::TextAlign::Left);
    statusLabel->setFrame(core::Rect{0.0, 0.0, 100.0, kHeight});
    statusLabel_ = statusLabel.get();
    bar_->addChild(std::move(statusLabel));

    // Page indicator on the right side of the status bar:
    //   Page [ field ] / 348
    auto pageCaption = std::make_unique<TextLabel>("Page", ui::Font{12.0}, ui::Color::gray(0.35),
                                                   ui::TextAlign::Right);
    pageCaption->setFrame(core::Rect{0.0, 3.0, 36.0, 20.0});
    pageCaptionLabel_ = pageCaption.get();
    bar_->addChild(std::move(pageCaption));

    auto pageField = std::make_unique<ui::TextField>("1");
    pageField_ = pageField.get();
    pageField_->setFrame(core::Rect{0.0, 2.0, kPageFieldWidth, 22.0});
    pageField_->setOnFocusRequested([this] { context_.setFocus(pageField_); });
    pageField_->setOnEnter([this] { commitPageField(); });
    pageField_->setOnEscape([this] { context_.setFocus(nullptr); });
    bar_->addChild(std::move(pageField));

    auto pageCount = std::make_unique<TextLabel>("/ 0", ui::Font{12.0}, ui::Color::gray(0.35),
                                                 ui::TextAlign::Left);
    pageCount->setFrame(core::Rect{0.0, 3.0, 60.0, 20.0});
    pageCountLabel_ = pageCount.get();
    bar_->addChild(std::move(pageCount));
}

void StatusBarController::layout(const core::Rect& frame) {
    bar_->setFrame(frame);
    const double width = frame.size.width;
    statusLabel_->setFrame(core::Rect{12.0, 0.0, std::max(0.0, width - 220.0), kHeight});

    // Page indicator cluster, right-aligned: "Page [field] / N". The count
    // label gets a fixed generous width (no PaintContext during layout).
    const double pageCountX = std::max(0.0, width - kPageCountWidth - 12.0);
    pageCountLabel_->setFrame(core::Rect{pageCountX, 3.0, kPageCountWidth, 20.0});
    const double fieldX = std::max(0.0, pageCountX - kPageFieldWidth - 8.0);
    pageField_->setFrame(core::Rect{fieldX, 2.0, kPageFieldWidth, 22.0});
    pageCaptionLabel_->setFrame(core::Rect{std::max(0.0, fieldX - 44.0), 3.0, 36.0, 20.0});
}

void StatusBarController::setStatus(std::string text) { statusLabel_->setText(std::move(text)); }

const std::string& StatusBarController::statusText() const { return statusLabel_->text(); }

const std::string& StatusBarController::pageCountText() const { return pageCountLabel_->text(); }

void StatusBarController::updatePageIndicator() {
    if (DocumentTab* tab = context_.workspace.activeTab(); tab != nullptr && tab->session() != nullptr) {
        pageCountLabel_->setText(std::format("/ {}", tab->session()->pageCount()));
        // Do not fight a focused field: the user is editing the page number.
        if (!pageField_->isFocused()) pageField_->setText(pageFieldText(tab->currentPage()));
        return;
    }
    pageCountLabel_->setText("/ 0");
    pageField_->setText("");
}

void StatusBarController::commitPageField() {
    // An invalid page number is ignored (never crashes, never creates
    // invalid scroll state); focus returns to the viewport either way.
    if (DocumentTab* tab = context_.workspace.activeTab(); tab != nullptr && tab->session() != nullptr) {
        if (const std::optional<std::size_t> page =
                parsePageNumber(pageField_->text(), tab->session()->pageCount())) {
            context_.viewport.goToPage(*page);
        }
    }
    context_.setFocus(nullptr);
}

std::optional<std::size_t> StatusBarController::parsePageNumber(std::string_view text,
                                                                std::size_t pageCount) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::size_t page = 0;
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), page);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    if (page < 1 || page > pageCount) return std::nullopt;
    return page - 1;
}

} // namespace rivet::app
