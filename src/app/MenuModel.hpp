// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationController.hpp"
#include "app/ContentTool.hpp"
#include "app/FileController.hpp"
#include "app/FormLayer.hpp"
#include "app/MarkdownTabState.hpp"
#include "app/PageEditingController.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

// Portable menu/command model. Every platform shell (AppKit NSMenu, Win32
// HMENU, an in-app menu bar widget) renders buildMenuBar() and routes the
// chosen item back through ShellController::perform(AppCommand). New
// features add entries to the one table in MenuModel.cpp.
//
// Main-thread only; pure value types.

// A tagged value identifying any shell command reachable from a menu.
struct AppCommand {
    enum class Kind : std::uint8_t { Open, Quit, File, PageEdit, Annotation, Content, MarkdownMode, Form };

    Kind kind = Kind::Open;
    // The underlying enumerator value for File/PageEdit/Annotation/Content/
    // MarkdownMode/Form; 0 for Open and Quit.
    std::uint8_t value = 0;

    static AppCommand open() { return {Kind::Open, 0}; }
    static AppCommand quit() { return {Kind::Quit, 0}; }
    static AppCommand of(FileCommand c) { return {Kind::File, static_cast<std::uint8_t>(c)}; }
    static AppCommand of(PageEditCommand c) { return {Kind::PageEdit, static_cast<std::uint8_t>(c)}; }
    static AppCommand of(AnnotationCommand c) { return {Kind::Annotation, static_cast<std::uint8_t>(c)}; }
    static AppCommand of(ContentCommand c) { return {Kind::Content, static_cast<std::uint8_t>(c)}; }
    static AppCommand of(FormCommand c) { return {Kind::Form, static_cast<std::uint8_t>(c)}; }
    static AppCommand of(MarkdownDisplayMode m) { return {Kind::MarkdownMode, static_cast<std::uint8_t>(m)}; }

    FileCommand file() const { return static_cast<FileCommand>(value); }
    PageEditCommand pageEdit() const { return static_cast<PageEditCommand>(value); }
    AnnotationCommand annotation() const { return static_cast<AnnotationCommand>(value); }
    ContentCommand content() const { return static_cast<ContentCommand>(value); }
    FormCommand form() const { return static_cast<FormCommand>(value); }
    MarkdownDisplayMode markdownMode() const { return static_cast<MarkdownDisplayMode>(value); }

    friend bool operator==(const AppCommand&, const AppCommand&) = default;
};

// Portable key chord. `primary` is Command on macOS and Ctrl elsewhere.
// `key` is a single lowercase character (e.g. "o", "1").
struct Shortcut {
    std::string key;
    bool primary = true;
    bool shift = false;
    bool alt = false;

    friend bool operator==(const Shortcut&, const Shortcut&) = default;
};

// Placement hint for items that one platform draws elsewhere (macOS shows
// Quit in the application menu, not in File).
enum class MenuRole : std::uint8_t { None, Quit };

struct MenuItem {
    std::string title;
    AppCommand command;
    std::optional<Shortcut> shortcut;
    bool separatorBefore = false;
    bool checkable = false; // check/radio state comes from ShellController::isChecked
    MenuRole role = MenuRole::None;
};

struct Menu {
    std::string title;
    std::vector<MenuItem> items;
};

// The full menu bar: File, Edit, View, Page, Content, Annotate.
std::vector<Menu> buildMenuBar();

// All items in menu order (first menu to last, items first to last). The
// position in this vector is the stable flat index a platform stores in a
// native item tag/id and maps back to the item.
std::vector<const MenuItem*> flattenMenuItems(const std::vector<Menu>& bar);

} // namespace rivet::app
