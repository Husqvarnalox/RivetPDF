// SPDX-License-Identifier: MPL-2.0
#include "app/PasswordPromptController.hpp"
#include "ui/Theme.hpp"

#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rivet::app {
namespace {

constexpr double kPanelWidth = 380.0;
constexpr double kPanelHeight = 112.0;
constexpr double kPadding = 16.0;
// File names longer than this (code points) are shortened in the middle so
// the sentence always fits the panel (QA #12: it was cut off mid-phrase).
constexpr std::size_t kMaxNameCodePoints = 34;

bool isContinuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

} // namespace

std::string PasswordPromptController::shortenName(std::string_view name, std::size_t maxCodePoints) {
    std::vector<std::size_t> starts; // byte offset of every code point
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (!isContinuation(name[i])) starts.push_back(i);
    }
    if (starts.size() <= maxCodePoints || maxCodePoints < 3) return std::string(name);
    const std::size_t keep = maxCodePoints - 1; // one for the ellipsis
    const std::size_t head = (keep + 1) / 2;
    const std::size_t tail = keep - head;
    std::string out(name.substr(0, starts[head]));
    out += "\u2026";
    out += name.substr(starts[starts.size() - tail]);
    return out;
}

PasswordPromptController::PasswordPromptController(ShellContext& context, ui::Widget& parent)
    : context_(context) {
    auto panel = std::make_unique<ui::Container>();
    panel_ = panel.get();
    panel_->setBackground(&ui::Palette::panelBackground);
    parent.addChild(std::move(panel));

    const double width = kPanelWidth - 2.0 * kPadding;
    auto title = std::make_unique<TextLabel>("Password required", ui::Font{13.0, ui::Font::Weight::Semibold},
                                             &ui::Palette::textPrimary, ui::TextAlign::Left);
    title->setFrame(core::Rect{kPadding, 12.0, width, 18.0});
    title_ = title.get();
    panel_->addChild(std::move(title));

    auto message = std::make_unique<TextLabel>("", ui::Font{12.0}, &ui::Palette::textSecondary,
                                               ui::TextAlign::Left);
    message->setFrame(core::Rect{kPadding, 32.0, width, 18.0});
    message_ = message.get();
    panel_->addChild(std::move(message));

    auto field = std::make_unique<ui::TextField>("Password");
    field_ = field.get();
    field_->setFrame(core::Rect{kPadding, 56.0, width - 84.0, 24.0});
    field_->setEchoCharacter(U'•');
    field_->setOnFocusRequested([this] { context_.setFocus(field_); });
    field_->setOnEnter([this] {
        submit();
        context_.setFocus(nullptr);
    });
    field_->setOnEscape([this] { context_.setFocus(nullptr); });
    panel_->addChild(std::move(field));

    auto unlock = std::make_unique<ui::Button>("Unlock");
    unlock->setFrame(core::Rect{kPanelWidth - kPadding - 76.0, 56.0, 76.0, 24.0});
    unlock->setOnClick([this] { submit(); });
    panel_->addChild(std::move(unlock));

    auto error = std::make_unique<TextLabel>("", ui::Font{12.0}, &ui::Palette::danger, ui::TextAlign::Left);
    error->setFrame(core::Rect{kPadding, 84.0, width, 18.0});
    error_ = error.get();
    panel_->addChild(std::move(error));

    panel_->setFrame(kHiddenFrame);
}

void PasswordPromptController::bindTab(DocumentTab* tab) {
    visible_ = tab != nullptr && tab->state() == DocumentTab::State::NeedsPassword;
    if (!visible_) {
        panel_->setFrame(kHiddenFrame);
        // A hidden field must not keep swallowing keys (e.g. after Unlock
        // started the retry and the tab went back to Loading).
        if (field_->isFocused()) context_.setFocus(nullptr);
        return;
    }
    message_->setText(std::format("\u201C{}\u201D is protected. Enter its password to open it.",
                                  shortenName(tab->title(), kMaxNameCodePoints)));
    // A retry that ends here again means the password was wrong (the field
    // was cleared before the retry): say so instead of silently clearing.
    error_->setText(tab->passwordRejected() ? "Incorrect password. Try again." : "");
    layout(viewportFrame_);
    context_.setFocus(field_);
}

void PasswordPromptController::layout(const core::Rect& viewportFrame) {
    viewportFrame_ = viewportFrame;
    if (!visible_) return;
    panel_->setFrame(core::Rect{std::max(viewportFrame.origin.x, viewportFrame.center().x - kPanelWidth / 2.0),
                                std::max(viewportFrame.origin.y, viewportFrame.center().y - kPanelHeight / 2.0),
                                kPanelWidth, kPanelHeight});
}

void PasswordPromptController::submit() {
    DocumentTab* tab = context_.workspace.activeTab();
    if (tab == nullptr || tab->state() != DocumentTab::State::NeedsPassword) return;
    // Clear the field BEFORE retrying: the retry rebinds the shell
    // synchronously and the password must not linger in the widget.
    std::string password = field_->text();
    field_->setText("");
    context_.workspace.retryWithPassword(context_.workspace.activeIndex(), std::move(password));
}

const std::string& PasswordPromptController::messageText() const { return message_->text(); }

const std::string& PasswordPromptController::errorText() const { return error_->text(); }

} // namespace rivet::app
