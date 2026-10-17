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
    file.items.push_back(item("Compare With\xE2\x80\xA6", AppCommand::of(FileCommand::Compare), std::nullopt, true));
    file.items.push_back(item("Print\xE2\x80\xA6", AppCommand::of(ShellCommand::Print), cmd("p"), true));
    // macOS draws Quit in the application menu; other platforms keep it here.
    MenuItem quit = item("Quit", AppCommand::quit(), cmd("q"), true);
    quit.role = MenuRole::Quit;
    file.items.push_back(std::move(quit));
    bar.push_back(std::move(file));

    Menu edit{"Edit", {}};
    edit.items.push_back(item("Undo", AppCommand::of(PageEditCommand::Undo), cmd("z")));
    edit.items.push_back(item("Redo", AppCommand::of(PageEditCommand::Redo), cmd("z", true)));
    edit.items.push_back(item("Copy", AppCommand::of(ShellCommand::Copy), cmd("c"), true));
    edit.items.push_back(item("Select All", AppCommand::of(ShellCommand::SelectAll), cmd("a")));
    edit.items.push_back(item("Find\xE2\x80\xA6", AppCommand::of(ShellCommand::Find), cmd("f"), true));
    edit.items.push_back(item("Find Next", AppCommand::of(ShellCommand::FindNext), cmd("g")));
    edit.items.push_back(item("Find Previous", AppCommand::of(ShellCommand::FindPrevious), cmd("g", true)));
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
    // Zoom (PDF / comparison tabs). Zoom In is bound to the unshifted '=' key
    // (Cmd/Ctrl+= and, via the shell's own key handling, Cmd/Ctrl++).
    view.items.push_back(item("Zoom In", AppCommand::of(ViewCommand::ZoomIn), cmd("="), true));
    view.items.push_back(item("Zoom Out", AppCommand::of(ViewCommand::ZoomOut), cmd("-")));
    view.items.push_back(item("Actual Size", AppCommand::of(ViewCommand::ActualSize), cmd("0")));
    view.items.push_back(item("Fit Width", AppCommand::of(ViewCommand::FitWidth)));
    view.items.push_back(item("Fit Page", AppCommand::of(ViewCommand::FitPage)));
    MenuItem highlight =
        item("Highlight Form Fields", AppCommand::of(FormCommand::HighlightFields), std::nullopt, true);
    highlight.checkable = true;
    view.items.push_back(std::move(highlight));
    bar.push_back(std::move(view));

    // Markdown formatting (ADR-0031); enabled only for Markdown tabs. Works on
    // the Rendered view's selection and on the Source editor's alike.
    Menu format{"Format", {}};
    format.items.push_back(item("Bold", AppCommand::of(MarkdownFormatCommand::Bold), cmd("b")));
    format.items.push_back(item("Italic", AppCommand::of(MarkdownFormatCommand::Italic), cmd("i")));
    format.items.push_back(
        item("Strikethrough", AppCommand::of(MarkdownFormatCommand::Strikethrough), cmd("x", true)));
    format.items.push_back(item("Inline Code", AppCommand::of(MarkdownFormatCommand::Code), cmd("e")));
    format.items.push_back(item("Link", AppCommand::of(MarkdownFormatCommand::Link), cmd("k")));
    const std::pair<const char*, MarkdownFormatCommand> levels[] = {
        {"Paragraph", MarkdownFormatCommand::Paragraph}, {"Heading 1", MarkdownFormatCommand::Heading1},
        {"Heading 2", MarkdownFormatCommand::Heading2},  {"Heading 3", MarkdownFormatCommand::Heading3},
        {"Heading 4", MarkdownFormatCommand::Heading4},  {"Heading 5", MarkdownFormatCommand::Heading5},
        {"Heading 6", MarkdownFormatCommand::Heading6},
    };
    for (std::size_t i = 0; i < std::size(levels); ++i) {
        // Cmd/Ctrl+Alt+0..6 (plain Cmd+digits switch the display mode).
        format.items.push_back(item(levels[i].first, AppCommand::of(levels[i].second),
                                    Shortcut{std::to_string(i), true, false, true}, i == 0));
    }
    format.items.push_back(
        item("Bulleted List", AppCommand::of(MarkdownFormatCommand::BulletList), std::nullopt, true));
    format.items.push_back(item("Numbered List", AppCommand::of(MarkdownFormatCommand::NumberedList)));
    format.items.push_back(item("Task List", AppCommand::of(MarkdownFormatCommand::TaskList)));
    format.items.push_back(item("Quote", AppCommand::of(MarkdownFormatCommand::Quote)));
    bar.push_back(std::move(format));

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
    // OCR (ADR-0027): enabled only where the platform has a text recognizer.
    content.items.push_back(item("Recognize Text (Pages Without Text)",
                                 AppCommand::of(ContentCommand::RecognizeTextMissing), std::nullopt, true));
    content.items.push_back(
        item("Recognize Text (All Pages)", AppCommand::of(ContentCommand::RecognizeTextAll)));
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
