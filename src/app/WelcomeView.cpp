// SPDX-License-Identifier: MPL-2.0
#include "app/WelcomeView.hpp"

#include "ui/Button.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace rivet::app {
namespace {

constexpr double kIconSide = 44.0;
constexpr double kButtonWidth = 132.0;
constexpr double kButtonHeight = 30.0;

// "/Users/me/Documents" -> "~/Documents" (display only).
std::string abbreviateHome(std::string path) {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home == nullptr || *home == '\0') return path;
    const std::string prefix(home);
    if (path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
        (path.size() == prefix.size() || path[prefix.size()] == '/' || path[prefix.size()] == '\\')) {
        return "~" + path.substr(prefix.size());
    }
    return path;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

WelcomeView::WelcomeView() {
    auto button = std::make_unique<ui::Button>("Open\xE2\x80\xA6");
    openButton_ = button.get();
    openButton_->setPrimary(true);
    openButton_->setFont(ui::fonts::kBodyStrong);
    addChild(std::move(button));
}

void WelcomeView::setOnOpen(std::function<void()> onOpen) { openButton_->setOnClick(std::move(onOpen)); }

void WelcomeView::setRecentFiles(const std::vector<std::filesystem::path>& paths) {
    recent_.clear();
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        recent_.push_back(Entry{path, std::filesystem::is_regular_file(path, error)});
    }
    hovered_.reset();
    pressed_.reset();
    layout();
    invalidate();
}

core::Rect WelcomeView::column() const {
    const core::Rect area = bounds();
    const double width = std::min(kColumnWidth, std::max(0.0, area.size.width - 48.0));
    const double listHeight = recent_.empty() ? 0.0 : 28.0 + static_cast<double>(recent_.size()) * kRowHeight;
    const double contentHeight = kIconSide + 16.0 + 32.0 + 26.0 + 24.0 + kButtonHeight + 36.0 + listHeight + 40.0;
    const double top = std::max(24.0, (area.size.height - contentHeight) / 2.0 - 20.0);
    return core::Rect{(area.size.width - width) / 2.0, top, width, contentHeight};
}

double WelcomeView::listTop() const {
    return column().minY() + kIconSide + 16.0 + 32.0 + 26.0 + 24.0 + kButtonHeight + 36.0 + 28.0;
}

core::Rect WelcomeView::recentRowRect(std::size_t index) const {
    const core::Rect c = column();
    return core::Rect{c.minX(), listTop() + static_cast<double>(index) * kRowHeight, c.size.width, kRowHeight};
}

std::optional<std::size_t> WelcomeView::rowAt(core::Point point) const {
    for (std::size_t i = 0; i < recent_.size(); ++i) {
        if (recentRowRect(i).contains(point)) return i;
    }
    return std::nullopt;
}

void WelcomeView::layout() {
    const core::Rect c = column();
    const double y = c.minY() + kIconSide + 16.0 + 32.0 + 26.0 + 24.0;
    openButton_->setFrame(core::Rect{c.minX() + (c.size.width - kButtonWidth) / 2.0, y, kButtonWidth, kButtonHeight});
}

bool WelcomeView::onMouse(const ui::PointerEvent& event) {
    if (Widget::onMouse(event)) return true; // the Open button
    const std::optional<std::size_t> row = rowAt(event.position);
    switch (event.type) {
    case ui::PointerEventType::Down:
        pressed_ = row && recent_[*row].exists ? row : std::nullopt;
        return row.has_value();
    case ui::PointerEventType::Up: {
        const std::optional<std::size_t> was = pressed_;
        pressed_.reset();
        if (was && was == row && onOpenRecent_) {
            const std::filesystem::path path = recent_[*row].path; // the callback may replace the list
            onOpenRecent_(path);
        }
        return was.has_value();
    }
    case ui::PointerEventType::Move:
    case ui::PointerEventType::Entered:
    case ui::PointerEventType::Exited: {
        const std::optional<std::size_t> next = event.type == ui::PointerEventType::Exited ? std::nullopt : row;
        if (next != hovered_) {
            hovered_ = next;
            setTooltip(hovered_ ? utf8(recent_[*hovered_].path) : std::string{});
            invalidate();
        }
        return row.has_value();
    }
    case ui::PointerEventType::Scroll: return false;
    }
    return false;
}

void WelcomeView::paintSelf(ui::PaintContext& context) const {
    const ui::Palette& palette = ui::colors();
    context.fillRect(bounds(), palette.windowBackground);
    const core::Rect c = column();
    double y = c.minY();
    ui::drawIcon(context, ui::Icon::Document, core::Rect{c.center().x - kIconSide / 2.0, y, kIconSide, kIconSide},
                 palette.textSecondary);
    y += kIconSide + 16.0;
    context.drawText("Rivet", core::Rect{c.minX(), y, c.size.width, 32.0}, ui::fonts::kTitle, palette.textPrimary,
                     ui::TextAlign::Center);
    y += 32.0;
    context.drawText("Open a PDF or Markdown document to get started.", core::Rect{c.minX(), y, c.size.width, 26.0},
                     ui::fonts::kBody, palette.textSecondary, ui::TextAlign::Center);

    if (!recent_.empty()) {
        const double headerY = listTop() - 28.0;
        context.drawText("Recent", core::Rect{c.minX() + 10.0, headerY, c.size.width, 24.0}, ui::fonts::kSmallStrong,
                         palette.textSecondary, ui::TextAlign::Left);
        context.fillRect(core::Rect{c.minX(), headerY + 25.0, c.size.width, 1.0}, palette.chromeSeparator);
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            const Entry& entry = recent_[i];
            const core::Rect row = recentRowRect(i);
            if (entry.exists && hovered_ == i) {
                context.fillRoundedRect(row, palette.listSelection, ui::metrics::kRadiusControl);
            }
            const ui::Color nameColor = entry.exists ? palette.textPrimary : palette.textDisabled;
            const ui::Color dirColor = entry.exists ? palette.textSecondary : palette.textDisabled;
            ui::drawIcon(context, ui::Icon::Document, core::Rect{row.minX() + 8.0, row.minY() + 7.0, 16.0, 16.0}, dirColor);
            const std::string name = utf8(entry.path.filename());
            const double nameWidth = std::min(context.measureText(name, ui::fonts::kBody).width, row.size.width * 0.55);
            context.drawText(name, core::Rect{row.minX() + 32.0, row.minY(), nameWidth, row.size.height}, ui::fonts::kBody,
                             nameColor, ui::TextAlign::Left);
            const double whereX = row.minX() + 32.0 + nameWidth + 10.0;
            const double whereWidth = std::max(0.0, row.maxX() - 8.0 - whereX);
            std::string where = abbreviateHome(utf8(entry.path.parent_path()));
            if (!entry.exists) where = "Missing \xE2\x80\x94 " + where;
            // Too long: keep the end of the path (the nearest folders).
            if (context.measureText(where, ui::fonts::kSmall).width > whereWidth) {
                std::string tail = where;
                while (!tail.empty() &&
                       context.measureText("\xE2\x80\xA6" + tail, ui::fonts::kSmall).width > whereWidth) {
                    std::size_t cut = 1;
                    while (cut < tail.size() && (static_cast<unsigned char>(tail[cut]) & 0xC0) == 0x80) ++cut;
                    tail.erase(0, cut);
                }
                where = "\xE2\x80\xA6" + tail;
            }
            context.pushClip(core::Rect{whereX, row.minY(), whereWidth, row.size.height});
            context.drawText(where, core::Rect{0.0, 0.0, 2000.0, row.size.height}, ui::fonts::kSmall, dirColor,
                             ui::TextAlign::Left);
            context.popClip();
        }
    }
    const double hintY = recent_.empty() ? openButton_->frame().maxY() + 28.0 : listTop() + static_cast<double>(recent_.size()) * kRowHeight + 18.0;
    context.drawText("You can also drop files onto this window.", core::Rect{c.minX(), hintY, c.size.width, 22.0},
                     ui::fonts::kSmall, palette.textSecondary, ui::TextAlign::Center);
}

} // namespace rivet::app
