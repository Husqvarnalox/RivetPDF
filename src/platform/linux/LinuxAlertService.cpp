// SPDX-License-Identifier: MPL-2.0
#include "LinuxAlertService.hpp"

#include "LinuxTextShaper.hpp"
#include "X11Window.hpp"
#include "ui/Button.hpp"
#include "ui/TextField.hpp"
#include "ui/Theme.hpp"
#include "ui/Widget.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace rivet::platform {

namespace {

constexpr double kWidth = 460.0;
constexpr double kPadding = 20.0;
constexpr double kGap = 10.0;
constexpr double kButtonHeight = 28.0;
constexpr double kButtonSpacing = 8.0;
constexpr double kFieldHeight = 26.0;
constexpr double kTitleLine = 20.0;
constexpr double kMessageLine = 18.0;

const ui::Font kTitleFont{14.0, ui::Font::Weight::Bold, false, false};
const ui::Font kMessageFont{13.0, ui::Font::Weight::Regular, false, false};
const ui::Font kButtonFont{13.0, ui::Font::Weight::Semibold, false, false};

// Greedy word wrap with the shaper's measurements (the same numbers the
// paint context uses). An over-long single word stays on its own line.
std::vector<std::string> wrap(std::string_view text, const ui::Font& font, double maxWidth) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view paragraph = text.substr(start, end - start);
        start = end + 1;
        std::string line;
        std::size_t pos = 0;
        while (pos < paragraph.size()) {
            std::size_t space = paragraph.find(' ', pos);
            if (space == std::string_view::npos) space = paragraph.size();
            const std::string word(paragraph.substr(pos, space - pos));
            pos = space + 1;
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && text::measureLine(candidate, font).width > maxWidth) {
                lines.push_back(std::move(line));
                line = word;
            } else {
                line = candidate;
            }
        }
        lines.push_back(std::move(line));
        if (end == text.size()) break;
    }
    return lines;
}

struct ButtonSpec {
    std::string label;
    int result = 0;
};

struct AlertSpec {
    std::string title;
    std::string message;
    std::vector<ButtonSpec> buttons; // visual order, left to right
    int defaultResult = 0;           // Return
    int cancelResult = 0;            // Escape / close box
    int dontSaveResult = -1;         // Ctrl+D, -1 = none
    bool withField = false;
    std::string fieldText;
};

class AlertView final : public ui::Widget {
public:
    AlertView(const AlertSpec& spec, std::function<void(int)> finish) : finish_(std::move(finish)) {
        const double textWidth = kWidth - 2.0 * kPadding;
        titleLines_ = wrap(spec.title, kTitleFont, textWidth);
        if (!spec.message.empty()) messageLines_ = wrap(spec.message, kMessageFont, textWidth);

        double y = kPadding + static_cast<double>(titleLines_.size()) * kTitleLine;
        if (!messageLines_.empty()) y += kGap / 2.0 + static_cast<double>(messageLines_.size()) * kMessageLine;
        if (spec.withField) {
            auto field = std::make_unique<ui::TextField>();
            field->setText(spec.fieldText);
            field->setFrame(core::Rect{kPadding, y + kGap, textWidth, kFieldHeight});
            field->setOnEnter([this, result = spec.defaultResult] { finish_(result); });
            field->setOnEscape([this, result = spec.cancelResult] { finish_(result); });
            ui::TextField* raw = field.get();
            field->setOnFocusRequested([raw] { raw->setFocused(true); });
            field->setFocused(true);
            // Select the prefilled text, like the NSAlert accessory field.
            ui::KeyEvent selectAll;
            selectAll.key = ui::Key::Character;
            selectAll.text = "a";
            selectAll.modifiers.command = true;
            raw->onKey(selectAll);
            field_ = raw;
            addChild(std::move(field));
            y += kGap + kFieldHeight;
        }
        y += kPadding;

        double x = kWidth - kPadding;
        for (auto it = spec.buttons.rbegin(); it != spec.buttons.rend(); ++it) {
            const double width =
                std::max(80.0, text::measureLine(it->label, kButtonFont).width + 2.0 * ui::Button::kHorizontalPadding);
            x -= width;
            auto button = std::make_unique<ui::Button>(it->label);
            button->setFrame(core::Rect{x, y, width, kButtonHeight});
            button->setActive(it->result == spec.defaultResult);
            button->setOnClick([this, result = it->result] { finish_(result); });
            addChild(std::move(button));
            x -= kButtonSpacing;
        }
        height_ = y + kButtonHeight + kPadding;
    }

    double height() const { return height_; }
    ui::TextField* field() const { return field_; }

    void paintSelf(ui::PaintContext& context) const override {
        context.fillRect(bounds(), ui::colors().panelBackground);
        double y = kPadding;
        for (const std::string& line : titleLines_) {
            context.drawText(line, core::Rect{kPadding, y, kWidth - 2.0 * kPadding, kTitleLine}, kTitleFont,
                             ui::colors().textPrimary, ui::TextAlign::Left);
            y += kTitleLine;
        }
        y += kGap / 2.0;
        for (const std::string& line : messageLines_) {
            context.drawText(line, core::Rect{kPadding, y, kWidth - 2.0 * kPadding, kMessageLine}, kMessageFont,
                             ui::colors().textSecondary, ui::TextAlign::Left);
            y += kMessageLine;
        }
    }

private:
    std::function<void(int)> finish_;
    std::vector<std::string> titleLines_;
    std::vector<std::string> messageLines_;
    ui::TextField* field_ = nullptr;
    double height_ = 0.0;
};

struct AlertOutcome {
    int result = 0;
    std::string text;
};

AlertOutcome runAlert(X11Connection& connection, X11Window* owner, const AlertSpec& spec) {
    std::optional<int> result;
    AlertView view(spec, [&result](int value) {
        if (!result) result = value;
    });

    X11Window::Options options;
    options.title = spec.title.empty() ? std::string("Rivet") : spec.title;
    options.logicalSize = core::Size{kWidth, view.height()};
    options.resizable = false;
    options.dialog = true;
    options.owner = owner;
    view.setFrame(core::Rect{0.0, 0.0, kWidth, view.height()});

    // Destroyed before `view` (declared after it): the window borrows the tree.
    X11Window window(connection, options);
    window.setRootWidget(&view);
    window.setCloseHandler([&result, cancel = spec.cancelResult] {
        if (!result) result = cancel;
    });
    window.setKeyHandler([&](const ui::KeyEvent& event) {
        if (view.field() != nullptr && view.field()->isFocused() && view.field()->onKey(event)) return true;
        if (event.key == ui::Key::Enter) {
            if (!result) result = spec.defaultResult;
            return true;
        }
        if (event.key == ui::Key::Escape) {
            if (!result) result = spec.cancelResult;
            return true;
        }
        if (spec.dontSaveResult >= 0 && event.key == ui::Key::Character && event.modifiers.command &&
            (event.text == "d" || event.text == "D")) {
            if (!result) result = spec.dontSaveResult;
            return true;
        }
        return false;
    });
    window.show();
    while (!result) {
        window.focusWhenViewable();
        connection.pump(X11Connection::Mode::Modal, &window, -1, -1);
    }
    window.setKeyHandler(nullptr);
    window.setRootWidget(nullptr);
    AlertOutcome outcome;
    outcome.result = *result;
    if (view.field() != nullptr) outcome.text = view.field()->text();
    return outcome;
}

constexpr int kFirst = 0;
constexpr int kSecond = 1;
constexpr int kThird = 2;

} // namespace

SaveChangesChoice LinuxAlertService::askSaveChanges(std::string_view documentTitle) {
    AlertSpec spec;
    spec.title = "Do you want to save the changes made to the document \xE2\x80\x9C" + std::string(documentTitle) +
                 "\xE2\x80\x9D?";
    spec.message = "Your changes will be lost if you don\xE2\x80\x99t save them.";
    spec.buttons = {{"Don\xE2\x80\x99t Save", kThird}, {"Cancel", kSecond}, {"Save", kFirst}};
    spec.defaultResult = kFirst;
    spec.cancelResult = kSecond;
    spec.dontSaveResult = kThird;
    const int answer = runAlert(connection_, owner_, spec).result;
    if (answer == kFirst) return SaveChangesChoice::Save;
    if (answer == kThird) return SaveChangesChoice::DontSave;
    return SaveChangesChoice::Cancel;
}

ReviewChangesChoice LinuxAlertService::askReviewUnsavedChanges(std::size_t unsavedDocumentCount) {
    AlertSpec spec;
    spec.title = "You have " + std::to_string(unsavedDocumentCount) +
                 " documents with unsaved changes. Do you want to review these changes before quitting?";
    spec.message = "If you don\xE2\x80\x99t review your documents, all your changes will be lost.";
    spec.buttons = {{"Discard Changes", kThird}, {"Cancel", kSecond}, {"Review Changes\xE2\x80\xA6", kFirst}};
    spec.defaultResult = kFirst;
    spec.cancelResult = kSecond;
    const int answer = runAlert(connection_, owner_, spec).result;
    if (answer == kFirst) return ReviewChangesChoice::Review;
    if (answer == kThird) return ReviewChangesChoice::DiscardAll;
    return ReviewChangesChoice::Cancel;
}

void LinuxAlertService::showError(std::string_view title, std::string_view message) {
    AlertSpec spec;
    spec.title = std::string(title);
    spec.message = std::string(message);
    spec.buttons = {{"OK", kFirst}};
    runAlert(connection_, owner_, spec);
}

std::optional<std::string> LinuxAlertService::promptForText(std::string_view title, std::string_view message,
                                                            std::string_view defaultText) {
    AlertSpec spec;
    spec.title = std::string(title);
    spec.message = std::string(message);
    spec.buttons = {{"Cancel", kSecond}, {"OK", kFirst}};
    spec.defaultResult = kFirst;
    spec.cancelResult = kSecond;
    spec.withField = true;
    spec.fieldText = std::string(defaultText);
    AlertOutcome outcome = runAlert(connection_, owner_, spec);
    if (outcome.result != kFirst) return std::nullopt;
    return std::move(outcome.text);
}

bool LinuxAlertService::confirm(std::string_view title, std::string_view message, std::string_view confirmLabel) {
    AlertSpec spec;
    spec.title = std::string(title);
    spec.message = std::string(message);
    spec.buttons = {{"Cancel", kSecond}, {std::string(confirmLabel), kFirst}};
    spec.defaultResult = kFirst;
    spec.cancelResult = kSecond;
    return runAlert(connection_, owner_, spec).result == kFirst;
}

} // namespace rivet::platform
