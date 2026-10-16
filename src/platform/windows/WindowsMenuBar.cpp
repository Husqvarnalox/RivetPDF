// SPDX-License-Identifier: MPL-2.0
#include "WindowsMenuBar.hpp"

#include "WinCommon.hpp"

#include <cctype>

namespace rivet::platform {

namespace {

std::string lowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// '&' introduces a mnemonic in Win32 menu text: escape literal ampersands.
std::string escapeAmpersands(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '&') out += '&';
        out += c;
    }
    return out;
}

std::string shortcutText(const WinMenuShortcut& shortcut) {
    std::string text;
    if (shortcut.primary) text += "Ctrl+";
    if (shortcut.shift) text += "Shift+";
    if (shortcut.alt) text += "Alt+";
    std::string key = shortcut.key;
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text + key;
}

} // namespace

WindowsMenuBar::WindowsMenuBar(std::vector<WinMenu> menus) : menus_(std::move(menus)) {
    bar_ = ::CreateMenu();
    for (const WinMenu& menu : menus_) {
        HMENU popup = ::CreatePopupMenu();
        for (const WinMenuItem& item : menu.items) {
            if (item.separatorBefore && ::GetMenuItemCount(popup) > 0) {
                ::AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
            }
            std::string label = escapeAmpersands(item.title);
            if (item.shortcut.has_value()) label += "\t" + shortcutText(*item.shortcut);
            ::AppendMenuW(popup, MF_STRING, kCommandBase + static_cast<UINT_PTR>(item.id), win::widen(label).c_str());
        }
        popups_.push_back(popup);
        // Alt mnemonic on the first letter of each top-level title.
        const std::string title = "&" + escapeAmpersands(menu.title);
        ::AppendMenuW(bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), win::widen(title).c_str());
    }
}

WindowsMenuBar::~WindowsMenuBar() {
    // An attached menu is destroyed with its window.
    if (!attached_ && bar_ != nullptr) ::DestroyMenu(bar_);
}

void WindowsMenuBar::refresh(HMENU__* popup) {
    std::size_t index = 0;
    while (index < popups_.size() && popups_[index] != popup) ++index;
    if (index == popups_.size()) return;
    for (const WinMenuItem& item : menus_[index].items) {
        const UINT commandId = kCommandBase + static_cast<UINT>(item.id);
        const bool enabled = callbacks_.isEnabled ? callbacks_.isEnabled(item.id) : false;
        ::EnableMenuItem(popup, commandId, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        if (callbacks_.isChecked) {
            const std::optional<bool> checked = callbacks_.isChecked(item.id);
            if (checked.has_value()) {
                ::CheckMenuItem(popup, commandId, MF_BYCOMMAND | (*checked ? MF_CHECKED : MF_UNCHECKED));
            }
        }
    }
}

bool WindowsMenuBar::command(unsigned commandId) {
    if (commandId < kCommandBase) return false;
    const int id = static_cast<int>(commandId - kCommandBase);
    for (const WinMenu& menu : menus_) {
        for (const WinMenuItem& item : menu.items) {
            if (item.id != id) continue;
            // Re-validate: the state may have changed since the popup opened.
            if (callbacks_.isEnabled && !callbacks_.isEnabled(id)) return true;
            if (callbacks_.activate) {
                const auto activate = callbacks_.activate; // may be replaced while running
                activate(id);
            }
            return true;
        }
    }
    return false;
}

bool WindowsMenuBar::handleShortcut(const ui::KeyEvent& event) {
    std::string key;
    switch (event.key) {
    case ui::Key::Character:
    case ui::Key::Plus:
    case ui::Key::Minus: key = lowerAscii(event.text); break;
    default: return false;
    }
    if (key.empty()) return false;
    for (const WinMenu& menu : menus_) {
        for (const WinMenuItem& item : menu.items) {
            if (!item.shortcut.has_value()) continue;
            const WinMenuShortcut& sc = *item.shortcut;
            if (sc.key != key || sc.primary != event.modifiers.command || sc.shift != event.modifiers.shift ||
                sc.alt != event.modifiers.option) {
                continue;
            }
            if (!callbacks_.isEnabled || !callbacks_.isEnabled(item.id)) return false;
            if (callbacks_.activate) {
                const auto activate = callbacks_.activate;
                activate(item.id);
            }
            return true;
        }
    }
    return false;
}

} // namespace rivet::platform
