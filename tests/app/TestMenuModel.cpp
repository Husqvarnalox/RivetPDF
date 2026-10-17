// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "app/MenuModel.hpp"

#include <set>
#include <string>
#include <tuple>

using namespace rivet::app;

RIVET_TEST(menuBarIsPopulated) {
    const auto bar = buildMenuBar();
    CHECK(!bar.empty());
    for (const Menu& menu : bar) {
        CHECK(!menu.title.empty());
        CHECK(!menu.items.empty());
        CHECK(!menu.items.front().separatorBefore);
        for (const MenuItem& entry : menu.items) CHECK(!entry.title.empty());
    }
}

RIVET_TEST(menuShortcutsAreUnique) {
    std::set<std::tuple<std::string, bool, bool, bool>> seen;
    for (const Menu& menu : buildMenuBar()) {
        for (const MenuItem& entry : menu.items) {
            if (!entry.shortcut) continue;
            const Shortcut& s = *entry.shortcut;
            CHECK(!s.key.empty());
            CHECK(seen.insert({s.key, s.primary, s.shift, s.alt}).second);
        }
    }
}

RIVET_TEST(menuCommandsAreUniqueAndFlatIndexRoundTrips) {
    const auto bar = buildMenuBar();
    const auto flat = flattenMenuItems(bar);
    std::size_t total = 0;
    for (const Menu& menu : bar) total += menu.items.size();
    CHECK(flat.size() == total);

    std::set<std::pair<int, int>> commands;
    std::size_t index = 0;
    for (const Menu& menu : bar) {
        for (const MenuItem& entry : menu.items) {
            CHECK(flat[index] == &entry);
            CHECK(flat[index]->command == entry.command);
            CHECK(commands.insert({static_cast<int>(entry.command.kind), entry.command.value}).second);
            ++index;
        }
    }
}

RIVET_TEST(viewMenuHasZoomCommandsWithDistinctShortcuts) {
    const Menu* view = nullptr;
    const auto bar = buildMenuBar();
    for (const Menu& menu : bar) {
        if (menu.title == "View") view = &menu;
    }
    CHECK(view != nullptr);
    const auto find = [&](ViewCommand command) -> const MenuItem* {
        for (const MenuItem& entry : view->items) {
            if (entry.command == AppCommand::of(command)) return &entry;
        }
        return nullptr;
    };
    const MenuItem* zoomIn = find(ViewCommand::ZoomIn);
    const MenuItem* zoomOut = find(ViewCommand::ZoomOut);
    const MenuItem* actual = find(ViewCommand::ActualSize);
    CHECK(zoomIn != nullptr && zoomIn->shortcut.has_value() && zoomIn->shortcut->key == "=");
    CHECK(zoomOut != nullptr && zoomOut->shortcut.has_value() && zoomOut->shortcut->key == "-");
    CHECK(actual != nullptr && actual->shortcut.has_value() && actual->shortcut->key == "0");
    CHECK(find(ViewCommand::FitWidth) != nullptr);
    CHECK(find(ViewCommand::FitPage) != nullptr);
}

RIVET_TEST(editAndFileMenusCarryClipboardFindAndPrintShortcuts) {
    const auto bar = buildMenuBar();
    const auto shortcutOf = [&](ShellCommand command) -> const Shortcut* {
        for (const Menu& menu : bar) {
            for (const MenuItem& entry : menu.items) {
                if (entry.command == AppCommand::of(command) && entry.shortcut) return &*entry.shortcut;
            }
        }
        return nullptr;
    };
    const Shortcut* copy = shortcutOf(ShellCommand::Copy);
    const Shortcut* all = shortcutOf(ShellCommand::SelectAll);
    const Shortcut* find = shortcutOf(ShellCommand::Find);
    const Shortcut* next = shortcutOf(ShellCommand::FindNext);
    const Shortcut* previous = shortcutOf(ShellCommand::FindPrevious);
    const Shortcut* print = shortcutOf(ShellCommand::Print);
    CHECK(copy != nullptr && copy->key == "c" && copy->primary && !copy->shift);
    CHECK(all != nullptr && all->key == "a" && !all->shift);
    CHECK(find != nullptr && find->key == "f");
    CHECK(next != nullptr && next->key == "g" && !next->shift);
    CHECK(previous != nullptr && previous->key == "g" && previous->shift);
    CHECK(print != nullptr && print->key == "p");
}

RIVET_TEST(menuHasOpenAndQuitWithRole) {
    bool open = false;
    bool quit = false;
    for (const Menu& menu : buildMenuBar()) {
        for (const MenuItem& entry : menu.items) {
            if (entry.command == AppCommand::open()) open = true;
            if (entry.command == AppCommand::quit()) {
                quit = true;
                CHECK(entry.role == MenuRole::Quit);
            }
        }
    }
    CHECK(open);
    CHECK(quit);
}
