// SPDX-License-Identifier: MPL-2.0
#include "LinuxMenuBar.hpp"

#include "LinuxTextShaper.hpp"

#include <algorithm>
#include <cctype>

namespace rivet::platform {

namespace {

constexpr double kTitlePadding = 10.0;
constexpr double kItemHeight = 24.0;
constexpr double kSeparatorHeight = 9.0;
constexpr double kCheckColumn = 22.0;
constexpr double kShortcutGap = 28.0;
constexpr double kRightPadding = 14.0;
constexpr double kMinDropdownWidth = 180.0;
constexpr double kDropdownPadding = 4.0;

const ui::Font kMenuFont{13.0, ui::Font::Weight::Regular, false, false};
constexpr ui::Color kBarBackground{0.95, 0.95, 0.95, 1.0};
constexpr ui::Color kBarBorder{0.80, 0.80, 0.80, 1.0};
constexpr ui::Color kText{0.10, 0.10, 0.10, 1.0};
constexpr ui::Color kDisabledText{0.60, 0.60, 0.60, 1.0};
constexpr ui::Color kShortcutText{0.45, 0.45, 0.45, 1.0};
constexpr ui::Color kHighlight{0.20, 0.45, 0.90, 1.0};
constexpr ui::Color kOpenTitle{0.85, 0.87, 0.91, 1.0};
constexpr ui::Color kDropdownBackground{1.0, 1.0, 1.0, 1.0};
constexpr ui::Color kShadow{0.0, 0.0, 0.0, 0.12};

std::string lowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

double textWidth(const std::string& text) {
    return text::measureLine(text, kMenuFont).width;
}

} // namespace

LinuxMenuBar::LinuxMenuBar(std::vector<MenuBarMenu> menus, Callbacks callbacks)
    : menus_(std::move(menus)), callbacks_(std::move(callbacks)) {}

std::string LinuxMenuBar::shortcutLabel(const MenuBarShortcut& shortcut) {
    std::string label;
    if (shortcut.primary) label += "Ctrl+";
    if (shortcut.shift) label += "Shift+";
    if (shortcut.alt) label += "Alt+";
    std::string key = shortcut.key;
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return label + key;
}

core::Rect LinuxMenuBar::titleRect(std::size_t menuIndex) const {
    double x = 4.0;
    for (std::size_t i = 0; i < menus_.size(); ++i) {
        const double width = textWidth(menus_[i].title) + 2.0 * kTitlePadding;
        if (i == menuIndex) return core::Rect{x, 0.0, width, kHeight};
        x += width;
    }
    return core::Rect{};
}

core::Rect LinuxMenuBar::dropdownRect(std::size_t menuIndex) const {
    if (menuIndex >= menus_.size()) return core::Rect{};
    const MenuBarMenu& menu = menus_[menuIndex];
    double width = kMinDropdownWidth;
    double height = 2.0 * kDropdownPadding;
    for (const MenuBarItem& item : menu.items) {
        double itemWidth = kCheckColumn + textWidth(item.title) + kRightPadding;
        if (item.shortcut.has_value()) itemWidth += kShortcutGap + textWidth(shortcutLabel(*item.shortcut));
        width = std::max(width, itemWidth);
        if (item.separatorBefore) height += kSeparatorHeight;
        height += kItemHeight;
    }
    const core::Rect title = titleRect(menuIndex);
    return core::Rect{title.minX(), kHeight, width, height};
}

std::vector<LinuxMenuBar::ItemSlot> LinuxMenuBar::itemSlots(std::size_t menuIndex) const {
    std::vector<ItemSlot> slots;
    if (menuIndex >= menus_.size()) return slots;
    const core::Rect box = dropdownRect(menuIndex);
    double y = box.minY() + kDropdownPadding;
    const auto& items = menus_[menuIndex].items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].separatorBefore) y += kSeparatorHeight;
        slots.push_back(ItemSlot{core::Rect{box.minX(), y, box.size.width, kItemHeight}, static_cast<int>(i)});
        y += kItemHeight;
    }
    return slots;
}

int LinuxMenuBar::menuAt(core::Point point) const {
    if (point.y < 0.0 || point.y >= kHeight) return -1;
    for (std::size_t i = 0; i < menus_.size(); ++i) {
        const core::Rect rect = titleRect(i);
        if (point.x >= rect.minX() && point.x < rect.maxX()) return static_cast<int>(i);
    }
    return -1;
}

int LinuxMenuBar::itemAt(core::Point point) const {
    if (openMenu_ < 0) return -1;
    for (const ItemSlot& slot : itemSlots(static_cast<std::size_t>(openMenu_))) {
        const core::Rect& r = slot.rect;
        if (point.x >= r.minX() && point.x < r.maxX() && point.y >= r.minY() && point.y < r.maxY()) {
            return slot.itemIndex;
        }
    }
    return -1;
}

bool LinuxMenuBar::enabled(const MenuBarItem& item) const {
    return callbacks_.isEnabled ? callbacks_.isEnabled(item.id) : false;
}

void LinuxMenuBar::activate(const MenuBarItem& item) {
    if (!enabled(item)) return;
    const int id = item.id;
    // Close first: the command may run a modal dialog (nested event loop)
    // and the window must not show a stale open menu meanwhile.
    close();
    if (callbacks_.activate) {
        const auto run = callbacks_.activate; // the callbacks may be replaced while running
        run(id);
    }
}

void LinuxMenuBar::openMenu(std::size_t index) {
    if (index >= menus_.size()) return;
    openMenu_ = static_cast<int>(index);
    hoverItem_ = -1;
}

void LinuxMenuBar::close() {
    openMenu_ = -1;
    hoverItem_ = -1;
}

void LinuxMenuBar::paintBar(ui::PaintContext& context, double width) const {
    context.fillRect(core::Rect{0.0, 0.0, width, kHeight}, kBarBackground);
    context.fillRect(core::Rect{0.0, kHeight - 1.0, width, 1.0}, kBarBorder);
    for (std::size_t i = 0; i < menus_.size(); ++i) {
        const core::Rect rect = titleRect(i);
        if (static_cast<int>(i) == openMenu_) {
            context.fillRect(core::Rect{rect.minX(), 1.0, rect.size.width, kHeight - 2.0}, kOpenTitle);
        }
        context.drawText(menus_[i].title, rect, kMenuFont, kText, ui::TextAlign::Center);
    }
}

void LinuxMenuBar::paintOverlay(ui::PaintContext& context) const {
    if (openMenu_ < 0) return;
    const auto menuIndex = static_cast<std::size_t>(openMenu_);
    const core::Rect box = dropdownRect(menuIndex);
    context.fillRect(core::Rect{box.minX() + 2.0, box.minY() + 2.0, box.size.width, box.size.height}, kShadow);
    context.fillRect(box, kDropdownBackground);
    context.strokeRect(box, kBarBorder, 1.0);

    const auto& items = menus_[menuIndex].items;
    for (const ItemSlot& slot : itemSlots(menuIndex)) {
        const MenuBarItem& item = items[static_cast<std::size_t>(slot.itemIndex)];
        const core::Rect& r = slot.rect;
        if (item.separatorBefore) {
            const double y = r.minY() - kSeparatorHeight / 2.0;
            context.fillRect(core::Rect{r.minX() + 6.0, y, r.size.width - 12.0, 1.0}, kBarBorder);
        }
        const bool isEnabled = enabled(item);
        const bool highlighted = isEnabled && slot.itemIndex == hoverItem_;
        if (highlighted) {
            context.fillRect(core::Rect{r.minX() + 2.0, r.minY(), r.size.width - 4.0, r.size.height}, kHighlight);
        }
        const ui::Color textColor = highlighted ? ui::Color::white() : (isEnabled ? kText : kDisabledText);
        if (item.checkable && callbacks_.isChecked) {
            const std::optional<bool> checked = callbacks_.isChecked(item.id);
            if (checked.value_or(false)) {
                context.drawText("\xE2\x9C\x93", core::Rect{r.minX(), r.minY(), kCheckColumn, r.size.height},
                                 kMenuFont, textColor, ui::TextAlign::Center);
            }
        }
        context.drawText(item.title,
                         core::Rect{r.minX() + kCheckColumn, r.minY(), r.size.width - kCheckColumn, r.size.height},
                         kMenuFont, textColor, ui::TextAlign::Left);
        if (item.shortcut.has_value()) {
            context.drawText(shortcutLabel(*item.shortcut),
                             core::Rect{r.minX(), r.minY(), r.size.width - kRightPadding, r.size.height},
                             kMenuFont, highlighted ? ui::Color::white() : kShortcutText,
                             ui::TextAlign::Right);
        }
    }
}

bool LinuxMenuBar::onPointer(const ui::PointerEvent& event) {
    const int overMenu = menuAt(event.position);
    switch (event.type) {
    case ui::PointerEventType::Down:
        if (overMenu >= 0) {
            if (overMenu == openMenu_) {
                close();
            } else {
                openMenu(static_cast<std::size_t>(overMenu));
            }
            return true;
        }
        if (isOpen()) {
            if (itemAt(event.position) < 0) close(); // click outside dismisses
            return true;
        }
        return false;
    case ui::PointerEventType::Up:
        if (!isOpen()) return false;
        if (const int item = itemAt(event.position); item >= 0) {
            activate(menus_[static_cast<std::size_t>(openMenu_)].items[static_cast<std::size_t>(item)]);
        }
        return true;
    case ui::PointerEventType::Move:
        if (!isOpen()) return false;
        if (overMenu >= 0 && overMenu != openMenu_) openMenu(static_cast<std::size_t>(overMenu));
        hoverItem_ = itemAt(event.position);
        return true;
    case ui::PointerEventType::Scroll:
    case ui::PointerEventType::Entered:
    case ui::PointerEventType::Exited:
        return isOpen();
    }
    return isOpen();
}

void LinuxMenuBar::moveHover(int delta) {
    if (openMenu_ < 0) return;
    const auto& items = menus_[static_cast<std::size_t>(openMenu_)].items;
    if (items.empty()) return;
    const int count = static_cast<int>(items.size());
    int next = hoverItem_;
    for (int step = 0; step < count; ++step) {
        next = next < 0 ? (delta > 0 ? 0 : count - 1) : (next + delta + count) % count;
        if (enabled(items[static_cast<std::size_t>(next)])) {
            hoverItem_ = next;
            return;
        }
    }
}

bool LinuxMenuBar::onKey(const ui::KeyEvent& event) {
    if (!isOpen()) return false;
    const int count = static_cast<int>(menus_.size());
    switch (event.key) {
    case ui::Key::Escape: close(); break;
    case ui::Key::Down: moveHover(+1); break;
    case ui::Key::Up: moveHover(-1); break;
    case ui::Key::Left: openMenu(static_cast<std::size_t>((openMenu_ - 1 + count) % count)); break;
    case ui::Key::Right: openMenu(static_cast<std::size_t>((openMenu_ + 1) % count)); break;
    case ui::Key::Enter:
    case ui::Key::Space:
        if (hoverItem_ >= 0) {
            activate(menus_[static_cast<std::size_t>(openMenu_)].items[static_cast<std::size_t>(hoverItem_)]);
        }
        break;
    default: break;
    }
    return true;
}

bool LinuxMenuBar::handleShortcut(const ui::KeyEvent& event) {
    std::string key;
    switch (event.key) {
    case ui::Key::Character:
    case ui::Key::Plus:
    case ui::Key::Minus: key = lowerAscii(event.text); break;
    default: return false;
    }
    if (key.empty()) return false;
    for (const MenuBarMenu& menu : menus_) {
        for (const MenuBarItem& item : menu.items) {
            if (!item.shortcut.has_value()) continue;
            const MenuBarShortcut& sc = *item.shortcut;
            if (sc.key != key || sc.primary != event.modifiers.command || sc.shift != event.modifiers.shift ||
                sc.alt != event.modifiers.option) {
                continue;
            }
            if (!enabled(item)) return false; // the content may still use the chord
            activate(item);
            return true;
        }
    }
    return false;
}

} // namespace rivet::platform
