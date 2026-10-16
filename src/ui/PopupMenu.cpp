// SPDX-License-Identifier: MPL-2.0
#include "ui/PopupMenu.hpp"

#include "ui/Drawing.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

constexpr double kCheckColumn = 22.0;
constexpr double kRightPadding = 12.0;
constexpr double kShortcutGap = 24.0;
constexpr double kChevronColumn = 16.0;
constexpr double kPerChar = 7.2; // layout estimate of the 13 px UI font

double estimate(const std::string& text) {
    // Count code points, not bytes (titles carry "…" and shortcut glyphs).
    double count = 0.0;
    for (const char c : text) count += ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ? 1.0 : 0.0;
    return count * kPerChar;
}

} // namespace

double PopupMenu::itemsHeight(const std::vector<Item>& items) {
    double height = 2.0 * kPadding;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].separatorBefore && i > 0) height += kSeparatorHeight;
        height += kItemHeight;
    }
    return height;
}

double PopupMenu::itemsWidth(const std::vector<Item>& items) {
    double title = 0.0;
    double shortcut = 0.0;
    bool submenu = false;
    for (const Item& item : items) {
        title = std::max(title, estimate(item.title));
        shortcut = std::max(shortcut, estimate(item.shortcut));
        submenu = submenu || !item.submenu.empty();
    }
    const double width = kCheckColumn + title + (shortcut > 0.0 ? kShortcutGap + shortcut : 0.0) +
                         (submenu ? kChevronColumn : 0.0) + kRightPadding + 2.0 * kPadding;
    return std::max(kMinWidth, width);
}

core::Rect PopupMenu::placeLevel(const std::vector<Item>& items, core::Point topLeft, double avoidLeftX) const {
    const core::Rect area = bounds();
    const double width = itemsWidth(items);
    const double height = std::min(itemsHeight(items), std::max(0.0, area.size.height - 8.0));
    double x = topLeft.x;
    if (x + width > area.maxX() - 4.0) x = avoidLeftX >= 0.0 ? avoidLeftX - width : area.maxX() - 4.0 - width;
    x = std::max(area.minX() + 4.0, x);
    double y = topLeft.y;
    if (y + height > area.maxY() - 4.0) y = std::max(area.minY() + 4.0, area.maxY() - 4.0 - height);
    return core::Rect{x, y, width, height};
}

void PopupMenu::open(std::vector<Item> items, const core::Rect& anchor, const core::Rect& area) {
    items_ = std::move(items);
    open_ = true;
    setFrame(area);
    levels_.clear();
    const core::Point local{anchor.minX() - area.minX(), anchor.maxY() + 4.0 - area.minY()};
    levels_.push_back(Level{&items_, placeLevel(items_, local, -1.0), -1});
    invalidate();
}

void PopupMenu::close() {
    if (!open_) return;
    open_ = false;
    levels_.clear();
    setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
    invalidate();
    if (onClosed_) onClosed_();
}

core::Rect PopupMenu::itemRect(const Level& level, std::size_t index) const {
    double y = level.rect.minY() + kPadding;
    const auto& items = *level.items;
    for (std::size_t i = 0; i <= index && i < items.size(); ++i) {
        if (items[i].separatorBefore && i > 0) y += kSeparatorHeight;
        if (i == index) break;
        y += kItemHeight;
    }
    return core::Rect{level.rect.minX() + kPadding, y, level.rect.size.width - 2.0 * kPadding, kItemHeight};
}

int PopupMenu::itemAt(const Level& level, core::Point point) const {
    if (!level.rect.contains(point)) return -1;
    for (std::size_t i = 0; i < level.items->size(); ++i) {
        if (itemRect(level, i).contains(point)) return static_cast<int>(i);
    }
    return -1;
}

void PopupMenu::openSubmenu(std::size_t levelIndex, std::size_t itemIndex) {
    levels_.resize(levelIndex + 1);
    const Level& parentLevel = levels_[levelIndex];
    const Item& item = (*parentLevel.items)[itemIndex];
    if (item.submenu.empty() || !item.enabled) return;
    const core::Rect row = itemRect(parentLevel, itemIndex);
    const core::Rect placed = placeLevel(item.submenu, core::Point{parentLevel.rect.maxX() - 2.0, row.minY() - kPadding},
                                         parentLevel.rect.minX() + 2.0);
    levels_.push_back(Level{&item.submenu, placed, -1});
}

void PopupMenu::activate(const Item& item) {
    if (!item.enabled) return;
    if (!item.submenu.empty()) return;
    const std::function<void()> action = item.action; // the menu (and item) may be rebuilt by it
    close();
    if (action) action();
}

bool PopupMenu::onMouse(const PointerEvent& event) {
    if (!open_) return false;
    // Deepest level under the pointer.
    int levelIndex = -1;
    int index = -1;
    for (int l = static_cast<int>(levels_.size()) - 1; l >= 0; --l) {
        const int at = itemAt(levels_[static_cast<std::size_t>(l)], event.position);
        if (at >= 0 || levels_[static_cast<std::size_t>(l)].rect.contains(event.position)) {
            levelIndex = l;
            index = at;
            break;
        }
    }
    switch (event.type) {
    case PointerEventType::Move:
    case PointerEventType::Entered:
        if (levelIndex >= 0) {
            const auto l = static_cast<std::size_t>(levelIndex);
            if (levels_[l].hover != index) {
                levels_[l].hover = index;
                if (index >= 0 && !(*levels_[l].items)[static_cast<std::size_t>(index)].submenu.empty()) {
                    openSubmenu(l, static_cast<std::size_t>(index));
                } else if (index >= 0) {
                    levels_.resize(l + 1);
                }
                invalidate();
            }
        }
        break;
    case PointerEventType::Down:
        if (levelIndex < 0) {
            close(); // click outside: dismiss (the click is consumed)
        }
        break;
    case PointerEventType::Up:
        if (levelIndex >= 0 && index >= 0) {
            const Item& item = (*levels_[static_cast<std::size_t>(levelIndex)].items)[static_cast<std::size_t>(index)];
            activate(item);
        }
        break;
    case PointerEventType::Exited:
    case PointerEventType::Scroll: break;
    }
    event.accepted = true;
    return true;
}

void PopupMenu::moveHover(int delta) {
    Level& level = levels_.back();
    const auto& items = *level.items;
    if (items.empty()) return;
    int index = level.hover;
    for (std::size_t step = 0; step < items.size(); ++step) {
        index = (index + delta + static_cast<int>(items.size())) % static_cast<int>(items.size());
        if (index < 0) index = 0;
        if (items[static_cast<std::size_t>(index)].enabled) break;
    }
    level.hover = index;
    invalidate();
}

bool PopupMenu::onKey(const KeyEvent& event) {
    if (!open_) return false;
    switch (event.key) {
    case Key::Escape: close(); break;
    case Key::Down: moveHover(+1); break;
    case Key::Up: moveHover(-1); break;
    case Key::Right: {
        const Level& level = levels_.back();
        if (level.hover >= 0) {
            openSubmenu(levels_.size() - 1, static_cast<std::size_t>(level.hover));
            if (levels_.back().items->size() > 0 && levels_.back().hover < 0) moveHover(+1);
        }
        break;
    }
    case Key::Left:
        if (levels_.size() > 1) levels_.pop_back();
        invalidate();
        break;
    case Key::Enter:
    case Key::Space: {
        const Level& level = levels_.back();
        if (level.hover >= 0) {
            const Item& item = (*level.items)[static_cast<std::size_t>(level.hover)];
            if (!item.submenu.empty()) {
                openSubmenu(levels_.size() - 1, static_cast<std::size_t>(level.hover));
                moveHover(+1);
            } else {
                activate(item);
            }
        }
        break;
    }
    default: break;
    }
    return true; // modal while open
}

void PopupMenu::paintSelf(PaintContext& context) const {
    if (!open_) return;
    const Palette& palette = colors();
    for (const Level& level : levels_) {
        const core::Rect box = level.rect;
        context.fillRoundedRect(core::Rect{box.minX() - 1.0, box.minY() + 1.0, box.size.width + 2.0, box.size.height + 3.0},
                                palette.pageShadow, metrics::kRadiusCard + 1.0);
        context.fillRoundedRect(box, palette.popoverBackground, metrics::kRadiusCard);
        strokeRoundedRect(context, box, palette.popoverBorder, 1.0, metrics::kRadiusCard);
        context.pushClip(box);
        const auto& items = *level.items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& item = items[i];
            core::Rect r = itemRect(level, i);
            r.origin = r.origin - box.origin;
            if (item.separatorBefore && i > 0) {
                context.fillRect(core::Rect{r.minX() + 6.0, r.minY() - kSeparatorHeight / 2.0 - 0.5, r.size.width - 12.0, 1.0},
                                 palette.chromeSeparator);
            }
            const bool highlighted = item.enabled && level.hover == static_cast<int>(i);
            if (highlighted) context.fillRoundedRect(r, palette.accentFill, metrics::kRadiusSmall);
            const Color text = highlighted ? palette.onAccent : (item.enabled ? palette.textPrimary : palette.textDisabled);
            const Color secondary = highlighted ? palette.onAccent : palette.textSecondary;
            if (item.checked) {
                Path check;
                const double cx = r.minX() + kCheckColumn / 2.0;
                const double cy = r.minY() + r.size.height / 2.0;
                check.moveTo({cx - 4.0, cy});
                check.lineTo({cx - 1.2, cy + 3.0});
                check.lineTo({cx + 4.0, cy - 3.5});
                context.strokePath(check, text, 1.6, LineCap::Round, LineJoin::Round);
            }
            context.drawText(item.title,
                             core::Rect{r.minX() + kCheckColumn, r.minY(), r.size.width - kCheckColumn, r.size.height},
                             fonts::kBody, text, TextAlign::Left);
            if (!item.submenu.empty()) {
                drawIcon(context, Icon::ChevronRight,
                         core::Rect{r.maxX() - kChevronColumn, r.minY() + (r.size.height - 14.0) / 2.0, 14.0, 14.0},
                         secondary);
            } else if (!item.shortcut.empty()) {
                context.drawText(item.shortcut, core::Rect{r.minX(), r.minY(), r.size.width - 8.0, r.size.height},
                                 fonts::kBody, secondary, TextAlign::Right);
            }
        }
        context.popClip();
    }
}

} // namespace rivet::ui
