// SPDX-License-Identifier: MPL-2.0
#include "app/SecurityDialogs.hpp"

#include "app/PasswordPromptController.hpp"
#include "app/SecurityText.hpp"
#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/SegmentedControl.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kPad = 16.0;
constexpr double kRow = 24.0;
constexpr double kStep = 30.0;
constexpr double kLine = 16.0;
constexpr double kCharWidth = 6.4; // average advance of the 12 pt body font
constexpr double kButtonWidth = 96.0;

ui::ColorToken severityColor(Severity severity) {
    switch (severity) {
    case Severity::Good: return &ui::Palette::success;
    case Severity::Warning: return &ui::Palette::warning;
    case Severity::Bad: return &ui::Palette::danger;
    case Severity::Unknown: break;
    }
    return &ui::Palette::textSecondary;
}

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

// Moves back from `pos` to a code-point boundary.
std::size_t boundary(const std::string& text, std::size_t pos) {
    while (pos > 0 && pos < text.size() && isContinuation(text[pos])) --pos;
    return pos;
}

crypto::SecretBuffer secretOf(ui::TextField& field) {
    crypto::SecretBuffer secret{std::string_view(field.text())};
    field.setText(""); // the widget keeps no copy of a password past the click
    return secret;
}

} // namespace

// ---------------------------------------------------------------- WrappedText

WrappedText::WrappedText(ui::Widget& parent, std::size_t maxLines, ui::Font font) : lineHeight_(kLine) {
    for (std::size_t i = 0; i < maxLines; ++i) {
        auto label = std::make_unique<TextLabel>("", font, &ui::Palette::textSecondary, ui::TextAlign::Left);
        label->setFrame(kHiddenFrame);
        labels_.push_back(label.get());
        parent.addChild(std::move(label));
    }
}

std::vector<std::string> WrappedText::wrap(const std::string& text, std::size_t maxChars) {
    std::vector<std::string> lines;
    maxChars = std::max<std::size_t>(maxChars, 8);
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string paragraph = text.substr(start, end - start);
        while (paragraph.size() > maxChars) {
            std::size_t cut = paragraph.rfind(' ', maxChars);
            if (cut == std::string::npos || cut == 0) cut = boundary(paragraph, maxChars);
            lines.push_back(paragraph.substr(0, cut));
            paragraph.erase(0, paragraph.find_first_not_of(' ', cut) == std::string::npos
                                   ? paragraph.size()
                                   : paragraph.find_first_not_of(' ', cut));
        }
        lines.push_back(std::move(paragraph));
        start = end + 1;
        if (end == text.size()) break;
    }
    return lines;
}

double WrappedText::set(const std::string& text, ui::ColorToken color, double x, double y, double width) {
    hide();
    if (text.empty()) return 0.0;
    std::vector<std::string> lines = wrap(text, static_cast<std::size_t>(width / kCharWidth));
    if (lines.size() > labels_.size()) {
        lines.resize(labels_.size());
        lines.back() += "…";
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        labels_[i]->setText(lines[i]);
        labels_[i]->setColor(color);
        labels_[i]->setFrame(core::Rect{x, y + static_cast<double>(i) * lineHeight_, width, lineHeight_});
    }
    return static_cast<double>(lines.size()) * lineHeight_;
}

void WrappedText::hide() {
    for (TextLabel* label : labels_) label->setFrame(kHiddenFrame);
}

// ---------------------------------------------------------------- SecurityPanel

SecurityPanel::SecurityPanel(ShellContext& context, ui::Widget& parent, double width)
    : context_(context), width_(width) {
    auto panel = std::make_unique<ui::Card>();
    panel_ = panel.get();
    parent.addChild(std::move(panel));
    panel_->setFrame(kHiddenFrame);
}

void SecurityPanel::place(ui::Widget* widget, double x, double y, double width, double height) {
    widget->setFrame(core::Rect{x, y, width, height});
}

void SecurityPanel::hideWidget(ui::Widget* widget) const { widget->setFrame(kHiddenFrame); }

void SecurityPanel::setVisible(bool visible) {
    visible_ = visible;
    if (!visible_) panel_->setFrame(kHiddenFrame);
    else layout(viewportFrame_);
    if (context_.relayout) context_.relayout();
}

void SecurityPanel::setHeight(double height) {
    height_ = height;
    if (visible_) layout(viewportFrame_);
}

void SecurityPanel::layout(const core::Rect& viewportFrame) {
    viewportFrame_ = viewportFrame;
    if (!visible_) return;
    panel_->setFrame(core::Rect{std::max(viewportFrame.origin.x, viewportFrame.center().x - width_ / 2.0),
                                std::max(viewportFrame.origin.y, viewportFrame.center().y - height_ / 2.0), width_,
                                height_});
}

ui::TextField* SecurityPanel::addField(const std::string& placeholder, bool secret) {
    auto field = std::make_unique<ui::TextField>(placeholder);
    ui::TextField* raw = field.get();
    if (secret) raw->setEchoCharacter(U'•');
    raw->setOnFocusRequested([this, raw] { context_.setFocus(raw); });
    raw->setFrame(kHiddenFrame);
    panel_->addChild(std::move(field));
    return raw;
}

ui::Button* SecurityPanel::addButton(const std::string& label, std::function<void()> onClick) {
    auto button = std::make_unique<ui::Button>(label);
    ui::Button* raw = button.get();
    raw->setOnClick(std::move(onClick));
    raw->setFrame(kHiddenFrame);
    panel_->addChild(std::move(button));
    return raw;
}

TextLabel* SecurityPanel::addLabel(const std::string& text, ui::Font font, ui::ColorToken color) {
    auto label = std::make_unique<TextLabel>(text, font, color, ui::TextAlign::Left);
    TextLabel* raw = label.get();
    raw->setFrame(kHiddenFrame);
    panel_->addChild(std::move(label));
    return raw;
}

void SecurityPanel::releaseFocus(std::initializer_list<ui::TextField*> fields) {
    for (ui::TextField* field : fields) {
        if (field != nullptr && field->isFocused()) {
            context_.setFocus(nullptr);
            return;
        }
    }
}

// ---------------------------------------------------------------- SetPasswordDialog

namespace {
constexpr std::array<const char*, 8> kPermissionLabels = {"Print", "Print (high quality)", "Copy text", "Edit",
                                                          "Annotate", "Fill forms", "Accessibility", "Page assembly"};
constexpr double kPasswordWidth = 460.0;
} // namespace

SetPasswordDialog::SetPasswordDialog(ShellContext& context, ui::Widget& parent)
    : SecurityPanel(context, parent, kPasswordWidth), body_(*panel_, 6, ui::Font{12.0}) {
    title_ = addLabel("Set Password", ui::Font{13.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary);
    user_ = addField("Password to open the file (optional)", true);
    userConfirm_ = addField("Confirm password", true);
    owner_ = addField("Owner password for permissions (optional)", true);
    ownerConfirm_ = addField("Confirm owner password", true);
    for (ui::TextField* f : {user_, userConfirm_, owner_, ownerConfirm_}) f->setOnEscape([this] { if (onCancel_) onCancel_(); });
    ownerConfirm_->setOnEnter([this] { submit(); });

    std::vector<ui::SegmentedControl::Segment> segments(2);
    segments[0].label = "AES-256 (recommended)";
    segments[1].label = "AES-128 (older readers)";
    auto cipher = std::make_unique<ui::SegmentedControl>(std::move(segments));
    cipher_ = cipher.get();
    cipher_->setSelected(0);
    cipher_->setFrame(kHiddenFrame);
    panel_->addChild(std::move(cipher));

    permissionsHeader_ = addLabel("Readers may:", ui::Font{12.0}, &ui::Palette::textSecondary);
    for (const char* label : kPermissionLabels) {
        ui::Button* b = addButton(label, {});
        b->setActive(true);
        b->setOnClick([b] { b->setActive(!b->active()); });
        permissionButtons_.push_back(b);
    }
    metadata_ = addButton("Encrypt document metadata", {});
    metadata_->setActive(true);
    metadata_->setOnClick([this] { metadata_->setActive(!metadata_->active()); });
    primary_ = addButton("Protect…", [this] { submit(); });
    primary_->setPrimary(true);
    secondary_ = addButton("Cancel", [this] { if (onCancel_) onCancel_(); });
}

void SetPasswordDialog::clearFields() {
    for (ui::TextField* f : {user_, userConfirm_, owner_, ownerConfirm_}) f->setText("");
}

void SetPasswordDialog::submit() {
    if (mode_ != Mode::Form || !onSubmit_) return;
    SetPasswordForm form;
    form.userPassword = secretOf(*user_);
    form.userPasswordConfirm = secretOf(*userConfirm_);
    form.ownerPassword = secretOf(*owner_);
    form.ownerPasswordConfirm = secretOf(*ownerConfirm_);
    form.cipher = cipher_->selected().value_or(0) == 1 ? pdf::encrypt::Cipher::Aes128 : pdf::encrypt::Cipher::Aes256;
    pdf::encrypt::Permissions& p = form.permissions;
    p.print = permissionButtons_[0]->active();
    p.printHighQuality = permissionButtons_[1]->active();
    p.copy = permissionButtons_[2]->active();
    p.modify = permissionButtons_[3]->active();
    p.annotate = permissionButtons_[4]->active();
    p.fillForms = permissionButtons_[5]->active();
    p.accessibility = permissionButtons_[6]->active();
    p.assemble = permissionButtons_[7]->active();
    form.encryptMetadata = metadata_->active();
    onSubmit_(std::move(form));
}

void SetPasswordDialog::arrange() {
    const double w = kPasswordWidth - 2.0 * kPad;
    for (ui::TextField* f : {user_, userConfirm_, owner_, ownerConfirm_}) hideWidget(f);
    hideWidget(cipher_);
    hideWidget(permissionsHeader_);
    for (ui::Button* b : permissionButtons_) hideWidget(b);
    hideWidget(metadata_);
    hideWidget(primary_);
    hideWidget(secondary_);
    place(title_, kPad, 12.0, w, 18.0);
    if (mode_ != Mode::Form) {
        const double h = body_.set(message_, messageIsError_ ? &ui::Palette::danger : &ui::Palette::textSecondary,
                                   kPad, 40.0, w);
        const double buttonY = 40.0 + h + 12.0;
        if (mode_ == Mode::Progress) {
            secondary_->setLabel("Cancel");
        } else {
            secondary_->setLabel("Close");
        }
        place(secondary_, kPasswordWidth - kPad - kButtonWidth, buttonY, kButtonWidth, kRow);
        setHeight(buttonY + kRow + 14.0);
        return;
    }
    secondary_->setLabel("Cancel");
    double y = 40.0;
    for (ui::TextField* f : {user_, userConfirm_, owner_, ownerConfirm_}) {
        place(f, kPad, y, w, kRow);
        y += kStep;
    }
    y += 6.0;
    place(cipher_, kPad, y, w, kRow);
    y += kRow + 10.0;
    place(permissionsHeader_, kPad, y, w, 16.0);
    y += 18.0;
    const double colWidth = (w - 3.0 * 6.0) / 4.0;
    for (std::size_t i = 0; i < permissionButtons_.size(); ++i) {
        place(permissionButtons_[i], kPad + static_cast<double>(i % 4) * (colWidth + 6.0),
              y + static_cast<double>(i / 4) * kStep, colWidth, kRow);
    }
    y += 2.0 * kStep + 2.0;
    place(metadata_, kPad, y, w, kRow);
    y += kStep + 4.0;
    y += body_.set(message_, &ui::Palette::danger, kPad, y, w);
    y += 8.0;
    place(secondary_, kPasswordWidth - kPad - 2.0 * kButtonWidth - 8.0, y, kButtonWidth, kRow);
    place(primary_, kPasswordWidth - kPad - kButtonWidth, y, kButtonWidth, kRow);
    setHeight(y + kRow + 14.0);
}

void SetPasswordDialog::showDialog(const SetPasswordDialogState& state) {
    mode_ = Mode::Form;
    message_.clear();
    messageIsError_ = false;
    clearFields();
    cipher_->setSelected(state.defaultCipher == pdf::encrypt::Cipher::Aes128 ? 1U : 0U);
    for (ui::Button* b : permissionButtons_) b->setActive(true);
    metadata_->setActive(true);
    title_->setText("Protect “" + PasswordPromptController::shortenName(state.documentTitle, 30) + "”");
    setVisible(true);
    arrange();
    context_.setFocus(user_);
}

void SetPasswordDialog::showBlocked(const std::string& message) {
    mode_ = Mode::Message;
    message_ = message;
    messageIsError_ = false;
    clearFields();
    title_->setText("Set Password");
    setVisible(true);
    arrange();
}

void SetPasswordDialog::showProgress(const std::string& message) {
    mode_ = Mode::Progress;
    message_ = message;
    messageIsError_ = false;
    releaseFocus({user_, userConfirm_, owner_, ownerConfirm_});
    setVisible(true);
    arrange();
}

void SetPasswordDialog::showError(const std::string& message) {
    mode_ = Mode::Form;
    message_ = message;
    messageIsError_ = true;
    clearFields();
    setVisible(true);
    arrange();
    context_.setFocus(user_);
}

void SetPasswordDialog::showDone(const std::string& message) {
    mode_ = Mode::Message;
    message_ = message;
    messageIsError_ = false;
    clearFields();
    title_->setText("Password set");
    setVisible(true);
    arrange();
}

void SetPasswordDialog::close() {
    clearFields();
    releaseFocus({user_, userConfirm_, owner_, ownerConfirm_});
    body_.hide();
    setVisible(false);
}

// ---------------------------------------------------------------- SigningDialog

namespace {
constexpr double kSigningWidth = 480.0;
} // namespace

SigningDialog::SigningDialog(ShellContext& context, ui::Widget& parent)
    : SecurityPanel(context, parent, kSigningWidth), body_(*panel_, 6, ui::Font{12.0}),
      identityDetail_(*panel_, 3, ui::Font{11.0}), levelDetail_(*panel_, 4, ui::Font{11.0}) {
    title_ = addLabel("Sign Document", ui::Font{13.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary);
    identityHeader_ = addLabel("Certificate", ui::Font{12.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary);
    levelHeader_ = addLabel("Signature level", ui::Font{12.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary);
    for (std::size_t i = 0; i < kVisibleIdentities; ++i) {
        identityButtons_.push_back(addButton("", [this, i] { selectIdentity(firstIdentity_ + i); }));
    }
    moreIdentities_ = addButton("More certificates…", [this] {
        firstIdentity_ += kVisibleIdentities;
        if (firstIdentity_ >= state_.identities.size()) firstIdentity_ = 0;
        arrange();
    });
    std::vector<ui::SegmentedControl::Segment> segments(4);
    segments[0].label = "B-B";
    segments[1].label = "B-T";
    segments[2].label = "B-LT";
    segments[3].label = "B-LTA";
    auto level = std::make_unique<ui::SegmentedControl>(std::move(segments));
    level_control_ = level.get();
    level_control_->setSelected(0);
    level_control_->setOnSelect([this](std::size_t i) {
        constexpr std::array<pdf::sign::PadesLevel, 4> levels = {
            pdf::sign::PadesLevel::BaselineB, pdf::sign::PadesLevel::BaselineT, pdf::sign::PadesLevel::BaselineLT,
            pdf::sign::PadesLevel::BaselineLTA};
        level_ = levels[std::min<std::size_t>(i, 3)];
        level_control_->setSelected(i);
        arrange();
    });
    level_control_->setFrame(kHiddenFrame);
    panel_->addChild(std::move(level));
    tsa_ = addField("Time-stamp server address (https://…)", false);
    visible_button_ = addButton("Visible signature on the current page", {});
    visible_button_->setOnClick([this] {
        visibleSignature_ = !visibleSignature_;
        visible_button_->setActive(visibleSignature_);
    });
    reason_ = addField("Reason (optional)", false);
    location_ = addField("Location (optional)", false);
    pin_ = addField("PIN", true);
    pin_->setOnEnter([this] { answerPin(true); });
    for (ui::TextField* f : {tsa_, reason_, location_, pin_}) f->setOnEscape([this] { if (mode_ == Mode::Pin) answerPin(false); else if (onCancel_) onCancel_(); });
    primary_ = addButton("Sign…", [this] {
        if (mode_ == Mode::Pin) answerPin(true);
        else if (mode_ == Mode::Consent) answerConsent(true);
        else submit();
    });
    primary_->setPrimary(true);
    secondary_ = addButton("Cancel", [this] {
        if (mode_ == Mode::Pin) answerPin(false);
        else if (mode_ == Mode::Consent) answerConsent(false);
        else if (onCancel_) onCancel_();
    });
}

void SigningDialog::selectIdentity(std::size_t index) {
    if (index >= state_.identities.size()) return;
    selected_ = index;
    arrange();
}

void SigningDialog::submit() {
    if (mode_ != Mode::Form || !onSubmit_) return;
    SigningForm form;
    form.identityIndex = selected_;
    form.level = level_;
    form.tsaUrl = tsa_->text();
    form.visible = visibleSignature_;
    form.page = state_.currentPage;
    form.reason = reason_->text();
    form.location = location_->text();
    onSubmit_(std::move(form));
}

void SigningDialog::answerPin(bool ok) {
    if (mode_ != Mode::Pin) return;
    auto answer = std::move(pinAnswer_);
    pinAnswer_ = nullptr;
    std::optional<crypto::SecretBuffer> pin;
    if (ok) pin = secretOf(*pin_);
    else pin_->setText("");
    mode_ = Mode::Progress;
    releaseFocus({pin_});
    arrange();
    if (answer) answer(std::move(pin));
}

void SigningDialog::answerConsent(bool allow) {
    if (mode_ != Mode::Consent) return;
    auto answer = std::move(consentAnswer_);
    consentAnswer_ = nullptr;
    mode_ = Mode::Progress;
    arrange();
    if (answer) answer(allow);
}

void SigningDialog::arrange() {
    const double w = kSigningWidth - 2.0 * kPad;
    for (ui::Widget* widget : std::initializer_list<ui::Widget*>{identityHeader_, levelHeader_, moreIdentities_,
                                                                  level_control_, tsa_, visible_button_, reason_,
                                                                  location_, pin_, primary_, secondary_}) {
        hideWidget(widget);
    }
    for (ui::Button* b : identityButtons_) hideWidget(b);
    identityDetail_.hide();
    levelDetail_.hide();
    body_.hide();
    place(title_, kPad, 12.0, w, 18.0);
    const double right = kSigningWidth - kPad;
    const auto buttons = [&](double y, const char* primaryLabel, const char* secondaryLabel) {
        if (primaryLabel != nullptr) {
            primary_->setLabel(primaryLabel);
            place(primary_, right - kButtonWidth, y, kButtonWidth, kRow);
        }
        secondary_->setLabel(secondaryLabel);
        place(secondary_, primaryLabel != nullptr ? right - 2.0 * kButtonWidth - 8.0 : right - kButtonWidth, y,
              kButtonWidth, kRow);
        setHeight(y + kRow + 14.0);
    };
    if (mode_ == Mode::Message || mode_ == Mode::Progress) {
        title_->setText("Sign Document");
        const double h = body_.set(message_, messageIsError_ ? &ui::Palette::danger : &ui::Palette::textSecondary,
                                   kPad, 40.0, w);
        buttons(40.0 + h + 12.0, nullptr, mode_ == Mode::Progress ? "Cancel" : "Close");
        return;
    }
    if (mode_ == Mode::Pin) {
        title_->setText("PIN required");
        const double h = body_.set(pinText_, &ui::Palette::textSecondary, kPad, 40.0, w);
        place(pin_, kPad, 40.0 + h + 8.0, w, kRow);
        buttons(40.0 + h + 8.0 + kStep, "OK", "Cancel");
        return;
    }
    if (mode_ == Mode::Consent) {
        title_->setText("Contact a time-stamp server?");
        const double h = body_.set(consentText_, &ui::Palette::textSecondary, kPad, 40.0, w);
        buttons(40.0 + h + 12.0, "Allow", "Don't allow");
        return;
    }
    // Form
    title_->setText("Sign “" + PasswordPromptController::shortenName(state_.documentTitle, 30) + "”");
    double y = 38.0;
    if (state_.identities.empty()) {
        y += body_.set(state_.identityProblem, &ui::Palette::danger, kPad, y, w);
        buttons(y + 12.0, nullptr, "Close");
        return;
    }
    place(identityHeader_, kPad, y, w, 16.0);
    y += 20.0;
    const std::size_t last = std::min(state_.identities.size(), firstIdentity_ + kVisibleIdentities);
    for (std::size_t i = firstIdentity_; i < last; ++i) {
        ui::Button* b = identityButtons_[i - firstIdentity_];
        const SigningIdentityChoice& c = state_.identities[i];
        b->setLabel(c.label + "  —  " + c.custody + (c.usable ? "" : " (unavailable)"));
        b->setActive(i == selected_);
        place(b, kPad, y, w, kRow);
        y += kRow + 4.0;
    }
    if (state_.identities.size() > kVisibleIdentities) {
        place(moreIdentities_, kPad, y, w, kRow);
        y += kRow + 4.0;
    }
    if (selected_ < state_.identities.size()) {
        const SigningIdentityChoice& c = state_.identities[selected_];
        y += identityDetail_.set(c.usable ? c.detail : c.unusableReason, &ui::Palette::textSecondary, kPad, y, w);
    }
    y += 10.0;
    place(levelHeader_, kPad, y, w, 16.0);
    y += 20.0;
    place(level_control_, kPad, y, w, kRow);
    for (std::size_t i = 1; i < 4; ++i) level_control_->setSegmentEnabled(i, state_.networkAvailable);
    y += kRow + 4.0;
    std::string detail = levelLabel(level_);
    if (levelNeedsNetwork(level_)) {
        switch (state_.onlineMode) {
        case net::OnlineValidationMode::Off:
            detail += " Online validation is turned off in the settings, so this level cannot be used.";
            break;
        case net::OnlineValidationMode::Ask:
            detail += " Rivet asks before contacting the time-stamp server.";
            break;
        case net::OnlineValidationMode::On: break;
        }
    }
    y += levelDetail_.set(detail, &ui::Palette::textSecondary, kPad, y, w);
    y += 6.0;
    if (levelNeedsTimestamp(level_)) {
        place(tsa_, kPad, y, w, kRow);
        y += kStep;
    }
    visible_button_->setActive(visibleSignature_);
    place(visible_button_, kPad, y, w, kRow);
    y += kStep;
    place(reason_, kPad, y, w, kRow);
    y += kStep;
    place(location_, kPad, y, w, kRow);
    y += kStep;
    if (!message_.empty()) y += body_.set(message_, &ui::Palette::danger, kPad, y, w) + 4.0;
    buttons(y + 4.0, "Sign…", "Cancel");
}

void SigningDialog::showDialog(const SigningDialogState& state) {
    state_ = state;
    mode_ = Mode::Form;
    message_.clear();
    messageIsError_ = false;
    selected_ = 0;
    firstIdentity_ = 0;
    for (std::size_t i = 0; i < state_.identities.size(); ++i) {
        if (state_.identities[i].usable) {
            selected_ = i;
            break;
        }
    }
    level_ = state.defaultLevel;
    level_control_->setSelected(0);
    visibleSignature_ = false;
    tsa_->setText(state.tsaUrlDefault);
    reason_->setText("");
    location_->setText("");
    setVisible(true);
    arrange();
}

void SigningDialog::showBlocked(const std::string& message) {
    mode_ = Mode::Message;
    message_ = message;
    messageIsError_ = false;
    setVisible(true);
    arrange();
}

void SigningDialog::showProgress(const std::string& message) {
    mode_ = Mode::Progress;
    message_ = message;
    messageIsError_ = false;
    releaseFocus({tsa_, reason_, location_, pin_});
    setVisible(true);
    arrange();
}

void SigningDialog::showError(const std::string& message) {
    mode_ = Mode::Form;
    message_ = message;
    messageIsError_ = true;
    setVisible(true);
    arrange();
}

void SigningDialog::showDone(const std::string& message) {
    mode_ = Mode::Message;
    message_ = message;
    messageIsError_ = false;
    setVisible(true);
    arrange();
}

void SigningDialog::close() {
    if (pinAnswer_) answerPin(false);
    if (consentAnswer_) answerConsent(false);
    releaseFocus({tsa_, reason_, location_, pin_});
    pin_->setText("");
    body_.hide();
    identityDetail_.hide();
    levelDetail_.hide();
    setVisible(false);
}

void SigningDialog::askPin(const crypto::PinRequest& request,
                           std::function<void(std::optional<crypto::SecretBuffer>)> answer) {
    if (pinAnswer_) answerPin(false); // a previous question is superseded
    pinAnswer_ = std::move(answer);
    pinText_ = "Enter the PIN for “" + request.label + "”.";
    if (request.reason == crypto::PinPromptReason::Incorrect) {
        pinText_ += " The previous PIN was not accepted.";
    } else if (request.reason == crypto::PinPromptReason::LastAttempt) {
        pinText_ += " The previous PIN was not accepted. One more wrong PIN may block the token.";
    }
    if (request.retriesLeft.has_value()) pinText_ += " Attempts left: " + std::to_string(*request.retriesLeft) + ".";
    pin_->setText("");
    mode_ = Mode::Pin;
    setVisible(true);
    arrange();
    context_.setFocus(pin_);
}

void SigningDialog::askConsent(const net::ConsentRequest& request, std::function<void(bool)> answer) {
    if (consentAnswer_) answerConsent(false);
    consentAnswer_ = std::move(answer);
    consentText_ = "Rivet needs to contact " + request.host;
    if (!request.purpose.empty()) consentText_ += " (" + request.purpose + ")";
    consentText_ += ". Only the data needed for this request is sent: a hash of the document, never the document itself. "
                    "Allow this for the current session?";
    mode_ = Mode::Consent;
    setVisible(true);
    arrange();
}

// ---------------------------------------------------------------- SignatureValidationPanel

namespace {
constexpr double kValidationWidth = 560.0;
} // namespace

SignatureValidationPanel::SignatureValidationPanel(ShellContext& context, ui::Widget& parent)
    : SecurityPanel(context, parent, kValidationWidth), summary_(*panel_, 2, ui::Font{12.0}),
      body_(*panel_, 16, ui::Font{12.0}), state_(*panel_, 3, ui::Font{12.0, ui::Font::Weight::Semibold}),
      footnote_(*panel_, 9, ui::Font{11.0}) {
    title_ = addLabel("Signatures", ui::Font{13.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary);
    pager_ = addLabel("", ui::Font{12.0}, &ui::Palette::textSecondary);
    previous_ = addButton("Previous", [this] { flip(-1); });
    next_ = addButton("Next", [this] { flip(1); });
    closeButton_ = addButton("Close", [this] {
        close();
        if (onClose_) onClose_();
    });
    closeButton_->setPrimary(true);
}

void SignatureValidationPanel::flip(int delta) {
    const std::size_t n = model_.rows.size();
    if (n == 0) return;
    row_ = (row_ + n + (delta < 0 ? n - 1 : 1)) % n;
    arrange();
}

void SignatureValidationPanel::arrange() {
    const double w = kValidationWidth - 2.0 * kPad;
    for (ui::Widget* widget : std::initializer_list<ui::Widget*>{pager_, previous_, next_, closeButton_}) hideWidget(widget);
    summary_.hide();
    body_.hide();
    state_.hide();
    footnote_.hide();
    place(title_, kPad, 12.0, w, 18.0);
    double y = 38.0;
    if (!hasModel_) {
        title_->setText("Signatures");
        y += body_.set(loading_ ? "Checking signatures…" : message_, &ui::Palette::textSecondary, kPad, y, w);
    } else {
        title_->setText("Signatures — " + PasswordPromptController::shortenName(model_.documentTitle, 36));
        y += summary_.set(model_.summary, &ui::Palette::textPrimary, kPad, y, w) + 6.0;
        const SignatureRowModel& row = model_.rows[std::min(row_, model_.rows.size() - 1)];
        if (model_.rows.size() > 1) {
            pager_->setText("Signature " + std::to_string(row_ + 1) + " of " + std::to_string(model_.rows.size()));
            place(pager_, kPad, y + 2.0, 200.0, 16.0);
            place(next_, kValidationWidth - kPad - 80.0, y, 80.0, kRow);
            place(previous_, kValidationWidth - kPad - 168.0, y, 80.0, kRow);
            y += kStep;
        }
        y += state_.set(row.heading + "\n" + row.stateTitle, severityColor(row.severity), kPad, y, w) + 4.0;
        std::string text;
        const auto add = [&text](const std::string& line) {
            if (line.empty()) return;
            if (!text.empty()) text += '\n';
            text += line;
        };
        if (!row.signer.empty()) add("Signer: " + row.signer);
        if (!row.field.empty()) add("Field: " + row.field);
        if (!row.signingTime.empty()) add("Claimed signing time: " + row.signingTime);
        add(row.timestamp);
        add("Level: " + row.level);
        add(row.qualification);
        for (const std::string& note : row.notes) add("• " + note);
        y += body_.set(text, &ui::Palette::textSecondary, kPad, y, w) + 8.0;
        y += footnote_.set(model_.footnote, &ui::Palette::textSecondary, kPad, y, w) + 6.0;
    }
    y += 6.0;
    place(closeButton_, kValidationWidth - kPad - kButtonWidth, y, kButtonWidth, kRow);
    setHeight(y + kRow + 14.0);
}

void SignatureValidationPanel::showLoading(const std::string&) {
    loading_ = true;
    hasModel_ = false;
    setVisible(true);
    arrange();
}

void SignatureValidationPanel::showResult(const ValidationPanelModel& model) {
    loading_ = false;
    hasModel_ = !model.rows.empty();
    model_ = model;
    row_ = 0;
    message_ = hasModel_ ? "" : "No digital signatures were found.";
    setVisible(true);
    arrange();
}

void SignatureValidationPanel::showMessage(const std::string& message) {
    loading_ = false;
    hasModel_ = false;
    message_ = message;
    setVisible(true);
    arrange();
}

void SignatureValidationPanel::close() {
    loading_ = false;
    setVisible(false);
}

} // namespace rivet::app
