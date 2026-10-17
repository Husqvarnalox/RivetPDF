// SPDX-License-Identifier: MPL-2.0
#pragma once

// Retained-widget views for the signature and Set Password features
// (ADR-0048): thin glue between the portable controllers and the Rivet widget
// toolkit. No logic beyond layout and field plumbing lives here.

#include "app/DigitalSignatureController.hpp"
#include "app/SetPasswordController.hpp"
#include "app/ShellContext.hpp"
#include "app/SignatureValidationController.hpp"
#include "ui/Container.hpp"
#include "ui/Theme.hpp"
#include "ui/UiTypes.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rivet::ui {
class Button;
class Card;
class SegmentedControl;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

class TextLabel;

// A fixed pool of single-line labels that shows wrapped text.
class WrappedText {
public:
    WrappedText(ui::Widget& parent, std::size_t maxLines, ui::Font font);
    // Wraps `text` ('\n' forces a break) to `width`, lays the lines out from
    // (x, y); returns the height used. Text beyond the pool is cut with "…".
    double set(const std::string& text, ui::ColorToken color, double x, double y, double width);
    void hide();
    static std::vector<std::string> wrap(const std::string& text, std::size_t maxChars);

private:
    std::vector<TextLabel*> labels_;
    double lineHeight_;
};

// Shared card plumbing: centred floating panel hidden by kHiddenFrame.
class SecurityPanel {
public:
    SecurityPanel(ShellContext& context, ui::Widget& parent, double width);
    virtual ~SecurityPanel() = default;

    SecurityPanel(const SecurityPanel&) = delete;
    SecurityPanel& operator=(const SecurityPanel&) = delete;

    bool visible() const { return visible_; }
    void layout(const core::Rect& viewportFrame);

protected:
    void setVisible(bool visible);
    void setHeight(double height);
    ui::TextField* addField(const std::string& placeholder, bool secret);
    ui::Button* addButton(const std::string& label, std::function<void()> onClick);
    TextLabel* addLabel(const std::string& text, ui::Font font, ui::ColorToken color);
    static void place(ui::Widget* widget, double x, double y, double width, double height);
    void hideWidget(ui::Widget* widget) const;
    void releaseFocus(std::initializer_list<ui::TextField*> fields);

    ShellContext& context_;
    ui::Card* panel_ = nullptr;
    double width_;
    double height_ = 100.0;
    bool visible_ = false;
    core::Rect viewportFrame_;
};

class SetPasswordDialog final : public SecurityPanel, public ISetPasswordView {
public:
    SetPasswordDialog(ShellContext& context, ui::Widget& parent);

    void setOnSubmit(std::function<void(SetPasswordForm)> fn) { onSubmit_ = std::move(fn); }
    void setOnCancel(std::function<void()> fn) { onCancel_ = std::move(fn); }

    void showDialog(const SetPasswordDialogState& state) override;
    void showBlocked(const std::string& message) override;
    void showProgress(const std::string& message) override;
    void showError(const std::string& message) override;
    void showDone(const std::string& message) override;
    void close() override;

private:
    enum class Mode { Form, Message, Progress };
    void arrange();
    void submit();
    void clearFields();

    std::function<void(SetPasswordForm)> onSubmit_;
    std::function<void()> onCancel_;
    Mode mode_ = Mode::Form;
    std::string message_;
    bool messageIsError_ = false;
    std::string documentTitle_;
    TextLabel* title_ = nullptr;
    TextLabel* permissionsHeader_ = nullptr;
    WrappedText body_;
    ui::TextField* user_ = nullptr;
    ui::TextField* userConfirm_ = nullptr;
    ui::TextField* owner_ = nullptr;
    ui::TextField* ownerConfirm_ = nullptr;
    ui::SegmentedControl* cipher_ = nullptr;
    std::vector<ui::Button*> permissionButtons_; // allowed-action toggles, same order as kPermissionLabels
    ui::Button* metadata_ = nullptr;
    ui::Button* primary_ = nullptr;
    ui::Button* secondary_ = nullptr;
};

class SigningDialog final : public SecurityPanel, public ISigningView {
public:
    SigningDialog(ShellContext& context, ui::Widget& parent);

    void setOnSubmit(std::function<void(SigningForm)> fn) { onSubmit_ = std::move(fn); }
    void setOnCancel(std::function<void()> fn) { onCancel_ = std::move(fn); }

    void showDialog(const SigningDialogState& state) override;
    void showBlocked(const std::string& message) override;
    void showProgress(const std::string& message) override;
    void showError(const std::string& message) override;
    void showDone(const std::string& message) override;
    void close() override;
    void askPin(const crypto::PinRequest& request,
                std::function<void(std::optional<crypto::SecretBuffer>)> answer) override;
    void askConsent(const net::ConsentRequest& request, std::function<void(bool)> answer) override;

private:
    enum class Mode { Form, Message, Progress, Pin, Consent };
    static constexpr std::size_t kVisibleIdentities = 4;
    void arrange();
    void submit();
    void selectIdentity(std::size_t index);
    void answerPin(bool ok);
    void answerConsent(bool allow);

    std::function<void(SigningForm)> onSubmit_;
    std::function<void()> onCancel_;
    SigningDialogState state_;
    Mode mode_ = Mode::Form;
    std::string message_;
    bool messageIsError_ = false;
    std::size_t selected_ = 0;
    std::size_t firstIdentity_ = 0;
    pdf::sign::PadesLevel level_ = pdf::sign::PadesLevel::BaselineB;
    bool visibleSignature_ = false;
    std::function<void(std::optional<crypto::SecretBuffer>)> pinAnswer_;
    std::function<void(bool)> consentAnswer_;
    std::string pinText_;
    std::string consentText_;

    TextLabel* title_ = nullptr;
    TextLabel* identityHeader_ = nullptr;
    TextLabel* levelHeader_ = nullptr;
    WrappedText body_;
    WrappedText identityDetail_;
    WrappedText levelDetail_;
    std::vector<ui::Button*> identityButtons_;
    ui::Button* moreIdentities_ = nullptr;
    ui::SegmentedControl* level_control_ = nullptr;
    ui::TextField* tsa_ = nullptr;
    ui::Button* visible_button_ = nullptr;
    ui::TextField* reason_ = nullptr;
    ui::TextField* location_ = nullptr;
    ui::TextField* pin_ = nullptr;
    ui::Button* primary_ = nullptr;
    ui::Button* secondary_ = nullptr;
};

class SignatureValidationPanel final : public SecurityPanel, public ISignatureValidationView {
public:
    SignatureValidationPanel(ShellContext& context, ui::Widget& parent);

    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }

    void showLoading(const std::string& title) override;
    void showResult(const ValidationPanelModel& model) override;
    void showMessage(const std::string& message) override;
    void close() override;

private:
    void arrange();
    void flip(int delta);

    std::function<void()> onClose_;
    ValidationPanelModel model_;
    std::string message_;
    bool loading_ = false;
    bool hasModel_ = false;
    std::size_t row_ = 0;
    TextLabel* title_ = nullptr;
    TextLabel* pager_ = nullptr;
    WrappedText summary_;
    WrappedText body_;
    WrappedText state_;
    WrappedText footnote_;
    ui::Button* previous_ = nullptr;
    ui::Button* next_ = nullptr;
    ui::Button* closeButton_ = nullptr;
};

} // namespace rivet::app
