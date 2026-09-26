// SPDX-License-Identifier: MPL-2.0
#include "app/PasswordPromptController.hpp"

#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace rivet::app {
namespace {

constexpr double kPanelWidth = 340.0;
constexpr double kPanelHeight = 74.0;

} // namespace

PasswordPromptController::PasswordPromptController(ShellContext& context, ui::Widget& parent)
    : context_(context) {
    auto panel = std::make_unique<ui::Container>();
    panel_ = panel.get();
    panel_->setBackgroundColor(ui::Color::rgba(0.97, 0.97, 0.97, 1.0));
    parent.addChild(std::move(panel));

    auto message = std::make_unique<TextLabel>(
        "This document is password protected", ui::Font{13.0, ui::Font::Weight::Semibold},
        ui::Color::gray(0.2), ui::TextAlign::Left);
    message->setFrame(core::Rect{12.0, 10.0, 320.0, 20.0});
    message_ = message.get();
    panel_->addChild(std::move(message));

    auto field = std::make_unique<ui::TextField>("Password");
    field_ = field.get();
    field_->setFrame(core::Rect{12.0, 36.0, 220.0, 24.0});
    field_->setEchoCharacter(U'•');
    field_->setOnFocusRequested([this] { context_.setFocus(field_); });
    field_->setOnEnter([this] {
        submit();
        context_.setFocus(nullptr);
    });
    field_->setOnEscape([this] { context_.setFocus(nullptr); });
    panel_->addChild(std::move(field));

    auto unlock = std::make_unique<ui::Button>("Unlock");
    unlock->setFrame(core::Rect{244.0, 36.0, 76.0, 24.0});
    unlock->setOnClick([this] { submit(); });
    panel_->addChild(std::move(unlock));

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
    message_->setText(std::format("{} is password protected — enter the password:", tab->title()));
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

} // namespace rivet::app
