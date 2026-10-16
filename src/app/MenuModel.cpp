// SPDX-License-Identifier: MPL-2.0
#include "app/MenuModel.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace rivet::app {

namespace {

MenuItem item(std::string title, AppCommand command, std::optional<Shortcut> shortcut = std::nullopt,
              bool separatorBefore = false) {
    MenuItem result;
    result.title = std::move(title);
    result.command = command;
    result.shortcut = std::move(shortcut);
    result.separatorBefore = separatorBefore;
    return result;
}

Shortcut cmd(std::string key, bool shift = false) {
    return Shortcut{std::move(key), true, shift, false};
}

} // namespace

std::vector<Menu> buildMenuBar() {
    std::vector<Menu> bar;

    Menu file{"File", {}};
    file.items.push_back(item("Open\xE2\x80\xA6", AppCommand::open(), cmd("o")));
    file.items.push_back(item("Save", AppCommand::of(FileCommand::Save), cmd("s"), true));
    file.items.push_back(item("Save As\xE2\x80\xA6", AppCommand::of(FileCommand::SaveAs), cmd("s", true)));
    file.items.push_back(item("Remove Password and Save As\xE2\x80\xA6",
                              AppCommand::of(FileCommand::SaveAsRemovePassword)));
    file.items.push_back(item("Save Reduced Copy (High)\xE2\x80\xA6", AppCommand::of(FileCommand::SaveReducedHigh)));
    file.items.push_back(
        item("Save Reduced Copy (Medium)\xE2\x80\xA6", AppCommand::of(FileCommand::SaveReducedMedium)));
    file.items.push_back(item("Save Reduced Copy (Small)\xE2\x80\xA6", AppCommand::of(FileCommand::SaveReducedSmall)));
    file.items.push_back(item("Export as PDF\xE2\x80\xA6", AppCommand::of(FileCommand::ExportPdf)));
    file.items.push_back(item("Insert Pages Before Current\xE2\x80\xA6", AppCommand::of(FileCommand::ImportBefore),
                              std::nullopt, true));
    file.items.push_back(
        item("Insert Pages After Current\xE2\x80\xA6", AppCommand::of(FileCommand::ImportAfter)));
    file.items.push_back(item("Merge with PDF\xE2\x80\xA6", AppCommand::of(FileCommand::Merge)));
    file.items.push_back(item("Export Selected Pages\xE2\x80\xA6", AppCommand::of(FileCommand::Extract)));
    file.items.push_back(item("Split PDF by Ranges\xE2\x80\xA6", AppCommand::of(FileCommand::Split)));
    // macOS draws Quit in the application menu; other platforms keep it here.
    MenuItem quit = item("Quit", AppCommand::quit(), cmd("q"), true);
    quit.role = MenuRole::Quit;
    file.items.push_back(std::move(quit));
    bar.push_back(std::move(file));

    Menu edit{"Edit", {}};
    edit.items.push_back(item("Undo", AppCommand::of(PageEditCommand::Undo), cmd("z")));
    edit.items.push_back(item("Redo", AppCommand::of(PageEditCommand::Redo), cmd("z", true)));
    edit.items.push_back(item("Reset Form", AppCommand::of(FormCommand::ResetForm), std::nullopt, true));
    bar.push_back(std::move(edit));

    Menu view{"View", {}};
    const std::pair<const char*, MarkdownDisplayMode> modes[] = {
        {"Rendered", MarkdownDisplayMode::Rendered},
        {"Source", MarkdownDisplayMode::Source},
        {"Split", MarkdownDisplayMode::Split},
    };
    const char* modeKeys[] = {"1", "2", "3"};
    for (std::size_t i = 0; i < 3; ++i) {
        MenuItem m = item(modes[i].first, AppCommand::of(modes[i].second), cmd(modeKeys[i]));
        m.checkable = true;
        view.items.push_back(std::move(m));
    }
    MenuItem highlight =
        item("Highlight Form Fields", AppCommand::of(FormCommand::HighlightFields), std::nullopt, true);
    highlight.checkable = true;
    view.items.push_back(std::move(highlight));
    bar.push_back(std::move(view));

    Menu page{"Page", {}};
    const std::pair<const char*, PageEditCommand> pageItems[] = {
        {"Rotate Left", PageEditCommand::RotateLeft},
        {"Rotate Right", PageEditCommand::RotateRight},
        {"Duplicate", PageEditCommand::DuplicatePages},
        {"Delete", PageEditCommand::DeletePages},
        {"Crop\xE2\x80\xA6", PageEditCommand::Crop},
        {"Reset Crop", PageEditCommand::ResetCrop},
    };
    for (const auto& [title, command] : pageItems) {
        page.items.push_back(item(title, AppCommand::of(command)));
    }
    bar.push_back(std::move(page));

    // Ctrl/Cmd+Shift+E / +T are free (plain E / T are not used).
    Menu content{"Content", {}};
    content.items.push_back(item("Edit Objects", AppCommand::of(ContentCommand::ToolEdit), cmd("e", true)));
    content.items.push_back(item("Add Text", AppCommand::of(ContentCommand::ToolAddText), cmd("t", true)));
    content.items.push_back(
        item("Edit Text", AppCommand::of(ContentCommand::EditText), std::nullopt, true));
    content.items.push_back(item("Replace Image\xE2\x80\xA6", AppCommand::of(ContentCommand::ReplaceImage)));
    content.items.push_back(item("Bring to Front", AppCommand::of(ContentCommand::BringToFront)));
    content.items.push_back(item("Delete Object", AppCommand::of(ContentCommand::DeleteObject)));
    // True redaction (ADR-0026).
    MenuItem redactTool = item("Redact", AppCommand::of(RedactCommand::ToolRedact), std::nullopt, true);
    redactTool.checkable = true;
    content.items.push_back(std::move(redactTool));
    content.items.push_back(
        item("Mark Selection for Redaction", AppCommand::of(RedactCommand::MarkSelection)));
    content.items.push_back(
        item("Apply Redactions\xE2\x80\xA6", AppCommand::of(RedactCommand::ApplyRedactions)));
    content.items.push_back(item("Clear Redaction Marks", AppCommand::of(RedactCommand::ClearMarks)));
    bar.push_back(std::move(content));

    // No single-letter shortcuts: they would clash with typing.
    Menu annotate{"Annotate", {}};
    const std::pair<const char*, AnnotationCommand> tools[] = {
        {"Select Tool", AnnotationCommand::ToolSelect}, {"Highlight", AnnotationCommand::ToolHighlight},
        {"Underline", AnnotationCommand::ToolUnderline}, {"Strike Out", AnnotationCommand::ToolStrikeOut},
        {"Note", AnnotationCommand::ToolNote},           {"Ink", AnnotationCommand::ToolInk},
        {"Rectangle", AnnotationCommand::ToolRectangle}, {"Ellipse", AnnotationCommand::ToolEllipse},
        {"Line", AnnotationCommand::ToolLine},           {"Arrow", AnnotationCommand::ToolArrow},
        {"Stamp", AnnotationCommand::ToolStamp},
    };
    for (const auto& [title, command] : tools) {
        annotate.items.push_back(item(title, AppCommand::of(command)));
    }
    annotate.items.push_back(
        item("Edit Note", AppCommand::of(AnnotationCommand::EditNote), std::nullopt, true));
    annotate.items.push_back(item("Delete Annotation", AppCommand::of(AnnotationCommand::DeleteAnnotation)));
    // Visual signatures (ADR-0025).
    annotate.items.push_back(
        item("Sign\xE2\x80\xA6", AppCommand::of(AnnotationCommand::Sign), std::nullopt, true));
    annotate.items.push_back(
        item("Manage Signatures\xE2\x80\xA6", AppCommand::of(AnnotationCommand::ManageSignatures)));
    bar.push_back(std::move(annotate));

    return bar;
}

std::vector<const MenuItem*> flattenMenuItems(const std::vector<Menu>& bar) {
    std::vector<const MenuItem*> flat;
    for (const Menu& menu : bar) {
        for (const MenuItem& entry : menu.items) flat.push_back(&entry);
    }
    return flat;
}

} // namespace rivet::app
