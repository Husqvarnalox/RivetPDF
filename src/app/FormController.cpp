// SPDX-License-Identifier: MPL-2.0
#include "app/FormController.hpp"
#include "ui/Theme.hpp"

#include "app/AnnotationPainter.hpp"

#include "editor/DocumentSession.hpp"
#include "editor/FormCommands.hpp"
#include "editor/FormService.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"
#include "ui/TextField.hpp"
#include "ui/Widget.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kEditorPadding = 2.0;
constexpr double kFocusWidth = 1.5;
constexpr char32_t kBullet = 0x2022;

ui::Color highlightColor() { return ui::colors().formFieldHighlight; }
ui::Color hoverColor() { return ui::colors().formFieldHover; }
ui::Color editedBorder() { return ui::colors().formFieldEditedBorder; }
ui::Color inkColor() { return ui::colors().editorInk; }

core::Rect toViewport(const core::Rect& pageRect, double zoom, const core::Rect& display) {
    return AnnotationPainter::toViewport(pageRect, zoom, display);
}

// `r` relative to `origin`: pushClip(rect) translates the origin to
// rect.origin (PaintContext contract), so geometry drawn inside the clip must
// be expressed relative to the clip rect.
core::Rect relativeTo(const core::Rect& origin, const core::Rect& r) {
    return core::Rect{r.origin.x - origin.origin.x, r.origin.y - origin.origin.y, r.size.width, r.size.height};
}

bool contains(const core::Rect& rect, core::Point p) {
    return p.x >= rect.minX() && p.x <= rect.maxX() && p.y >= rect.minY() && p.y <= rect.maxY();
}

// A check mark inside `box` (viewport space).
void paintCheck(ui::PaintContext& context, const core::Rect& box) {
    const double inset = std::min(box.size.width, box.size.height) * 0.2;
    const core::Rect r{box.origin.x + inset, box.origin.y + inset, box.size.width - 2.0 * inset,
                       box.size.height - 2.0 * inset};
    const double width = std::max(1.0, std::min(r.size.width, r.size.height) * 0.15);
    const core::Point a{r.minX(), r.minY() + r.size.height * 0.55};
    const core::Point b{r.minX() + r.size.width * 0.38, r.maxY()};
    const core::Point c{r.maxX(), r.minY()};
    context.drawLine(a, b, inkColor(), width);
    context.drawLine(b, c, inkColor(), width);
}

// A filled radio dot centered in `box` (viewport space).
void paintDot(ui::PaintContext& context, const core::Rect& box) {
    const double d = std::min(box.size.width, box.size.height) * 0.5;
    const core::Rect dot{box.origin.x + (box.size.width - d) / 2.0, box.origin.y + (box.size.height - d) / 2.0, d, d};
    context.fillRoundedRect(dot, inkColor(), d / 2.0);
}

} // namespace

FormController::FormController(ShellContext& context, ui::Widget& parent, AnnotationController& annotations)
    : context_(context), annotations_(annotations), layer_(*this) {
    auto line = std::make_unique<ui::TextField>();
    lineEditor_ = line.get();
    lineEditor_->setOnFocusRequested([this] { context_.setFocus(lineEditor_); });
    lineEditor_->setOnEnter([this] { commitEditor(); });
    lineEditor_->setOnEscape([this] { cancelEditor(); });
    lineEditor_->setFrame(kHiddenFrame);
    parent.addChild(std::move(line));

    auto area = std::make_unique<ui::TextArea>();
    areaEditor_ = area.get();
    areaEditor_->setBackground(ui::colors().editorPaper);
    areaEditor_->setTextColor(ui::colors().editorInk);
    areaEditor_->setMaxBytes(pdf::kMaxFormValueBytes);
    areaEditor_->setOnFocusRequested([this] { context_.setFocus(areaEditor_); });
    areaEditor_->setOnCommit([this] { commitEditor(); });
    areaEditor_->setOnEscape([this] { cancelEditor(); });
    areaEditor_->setFrame(kHiddenFrame);
    parent.addChild(std::move(area));

    context_.viewport.addLayer(&layer_, ui::PdfViewport::LayerPlacement::BelowAnnotations);
    pendingEditsToken_ = context_.pendingEdits.add([this] { return commitEditor(); });
}

FormController::~FormController() {
    context_.pendingEdits.remove(pendingEditsToken_);
    context_.viewport.removeLayer(&layer_);
    // The workspace (and every session) outlives the controllers: drop the
    // load observers so no callback fires into a dead controller.
    for (const TabId tabId : observedTabs_) {
        if (DocumentTab* tab = context_.workspace.tabById(tabId); tab != nullptr && tab->session() != nullptr) {
            tab->session()->forms().setOnChanged(nullptr);
        }
    }
}

void FormController::bindTab(DocumentTab* tab) {
    if (editing_.has_value() && !commitEditor()) cancelEditor();
    closePopup();
    clearFocus();
    hover_.reset();
    std::erase_if(observedTabs_, [this](TabId id) { return context_.workspace.tabById(id) == nullptr; });
    if (tab == nullptr || tab->session() == nullptr) return;
    const TabId id = tab->id();
    if (std::find(observedTabs_.begin(), observedTabs_.end(), id) == observedTabs_.end()) {
        observedTabs_.push_back(id);
        tab->session()->forms().setOnChanged([this, id] {
            DocumentTab* active = activeTab();
            if (active == nullptr || active->id() != id) return;
            announce(*active);
            repaint();
        });
    }
    tab->session()->forms().ensureLoaded();
    if (tab->session()->forms().state() == editor::FormService::State::Ready) announce(*tab);
}

void FormController::layout(const core::Rect& viewportFrame) {
    viewportFrame_ = viewportFrame;
}

// --- Queries -------------------------------------------------------------------

pdf::PdfFormInfoPtr FormController::formOf(const DocumentTab& tab) const {
    const editor::DocumentSession* session = tab.session();
    return session != nullptr ? session->forms().info() : nullptr;
}

bool FormController::interactive() const {
    return activeTab() != nullptr && annotations_.tool() == AnnotationTool::Select && !annotations_.suspended();
}

std::optional<FormController::Target> FormController::widgetAt(std::size_t page, core::Point display) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || page >= tab->session()->pageCount()) return std::nullopt;
    editor::DocumentSession& session = *tab->session();
    const core::PageId pageId = session.pageId(page);
    const std::vector<editor::FormWidgetView> widgets = session.forms().widgets(pageId);
    // Topmost first: /Annots order paints later entries above earlier ones.
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
        if (contains(it->displayRect, display)) return Target{page, pageId, it->field, it->widget, it->displayRect};
    }
    return std::nullopt;
}

std::optional<FormController::Target> FormController::resolve(const Focus& focus) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->id() != focus.tab) return std::nullopt;
    editor::DocumentSession& session = *tab->session();
    const std::size_t page = session.pageIndexFor(focus.page);
    if (page == editor::DocumentSession::kInvalidPage) return std::nullopt;
    for (const editor::FormWidgetView& view : session.forms().widgets(focus.page)) {
        if (view.field == focus.field && view.widget == focus.widget) {
            return Target{page, focus.page, view.field, view.widget, view.displayRect};
        }
    }
    return std::nullopt;
}

std::vector<FormStop> FormController::stops() const {
    std::vector<FormStop> result;
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return result;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || !info->fillable()) return result;
    editor::DocumentSession& session = *tab->session();
    for (std::size_t page = 0; page < session.pageCount(); ++page) {
        for (const editor::FormWidgetView& view : session.forms().widgets(session.pageId(page))) {
            const pdf::PdfFormField& field = info->fields[view.field];
            if (!field.fillable()) continue;
            result.push_back(FormStop{page, field.widgets[view.widget].annotIndex, view.field, view.widget});
        }
    }
    sortFormStops(result);
    return result;
}

std::optional<FormController::PopupState> FormController::popup() const {
    if (!popup_.has_value()) return std::nullopt;
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return std::nullopt;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || popup_->target.field >= info->fields.size()) return std::nullopt;
    const pdf::PdfFormField& field = info->fields[popup_->target.field];
    PopupState state;
    state.field = popup_->target.field;
    state.highlighted = popup_->highlighted;
    state.multi = field.type == pdf::PdfFormFieldType::ListBox && field.multiSelect;
    const auto value = tab->session()->forms().currentValue(popup_->target.field);
    for (std::size_t i = 0; i < field.options.size(); ++i) {
        state.labels.push_back(field.options[i].label);
        const bool on = value.has_value() && std::find(value->selected.begin(), value->selected.end(),
                                                       static_cast<std::uint32_t>(i)) != value->selected.end();
        state.selected.push_back(on);
    }
    return state;
}

std::optional<core::Rect> FormController::popupFrame(const ui::ViewportToolHost& host) const {
    if (!popup_.has_value()) return std::nullopt;
    const auto state = popup();
    if (!state.has_value()) return std::nullopt;
    const std::optional<core::Rect> pageRect = host.pageRectInViewport(popup_->target.page);
    if (!pageRect.has_value()) return std::nullopt;
    const core::Rect widget = toViewport(*pageRect, host.zoomFactor(), popup_->target.displayRect);
    const double rows = static_cast<double>(std::min(kPopupMaxRows, std::max<std::size_t>(1, state->labels.size())));
    return core::Rect{widget.minX(), widget.maxY(), std::max(widget.size.width, kPopupMinWidth),
                      rows * kPopupRowHeight};
}

// --- Focus and activation ---------------------------------------------------------

void FormController::focusOn(const Target& target) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    focus_ = Focus{tab->id(), target.pageId, target.field, target.widget};
    repaint();
}

void FormController::clearFocus() {
    if (!focus_.has_value()) return;
    focus_.reset();
    repaint();
}

void FormController::reportUnfillable(const pdf::PdfFormInfo& info, const pdf::PdfFormField& field) {
    if (info.hasXfa) {
        context_.setStatus("Form: XFA forms are view-only in Rivet");
    } else if (info.limitsExceeded) {
        context_.setStatus("Form: this form has too many fields to fill");
    } else if (field.type == pdf::PdfFormFieldType::Signature) {
        context_.setStatus("Form: signature fields cannot be filled");
    } else if (field.readOnly) {
        context_.setStatus("Form: this field is read-only");
    } else if (field.overLimit) {
        context_.setStatus("Form: this field exceeds the supported size and is read-only");
    }
}

void FormController::activate(const Target& target) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || target.field >= info->fields.size()) return;
    const pdf::PdfFormField& field = info->fields[target.field];
    if (field.type == pdf::PdfFormFieldType::Text) {
        openEditor(target);
    } else if (field.isToggle()) {
        toggle(target);
    } else if (field.isChoice()) {
        openPopup(target);
    }
}

bool FormController::setValue(const Target& target, pdf::PdfFormValue value) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return false;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || target.field >= info->fields.size()) return false;
    editor::DocumentSession& session = *tab->session();
    auto command = editor::makeSetFormValue(session.pageModel(), *info, info->fields[target.field].name,
                                            std::move(value));
    if (!command.has_value()) {
        context_.setStatus("Form: " + command.error().message);
        return false;
    }
    if (*command == nullptr) return true; // nothing changes
    const core::Status status = session.execute(std::move(*command));
    if (!status.has_value()) {
        context_.setStatus("Form: " + status.error().message);
        return false;
    }
    repaint();
    return true;
}

void FormController::toggle(const Target& target) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr) return;
    const pdf::PdfFormField& field = info->fields[target.field];
    const std::string& onState = field.widgets[target.widget].onState;
    const auto current = tab->session()->forms().currentValue(target.field);
    const bool isOn = current.has_value() && !onState.empty() && current->text == onState;
    if (isOn && field.type == pdf::PdfFormFieldType::RadioButton) return; // radios do not toggle off
    setValue(target, pdf::PdfFormValue::ofToggle(isOn ? std::string{} : onState));
}

void FormController::openEditor(const Target& target) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus("Form: " + tab->session()->editingLockReason());
        return;
    }
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr) return;
    const pdf::PdfFormField& field = info->fields[target.field];
    const auto current = tab->session()->forms().currentValue(target.field);
    Editing editing;
    editing.tab = tab->id();
    editing.target = target;
    editing.multiline = field.multiline;
    editing.original = current.has_value() ? current->text : std::string{};
    editing.fontSize = formFontSize(field, target.displayRect, editing.original, {});
    editing_ = std::move(editing);
    if (editing_->multiline) {
        areaEditor_->setText(editing_->original);
        context_.setFocus(areaEditor_);
    } else {
        lineEditor_->setEchoCharacter(field.password ? kBullet : char32_t{0});
        lineEditor_->setText(editing_->original);
        context_.setFocus(lineEditor_);
    }
    placeEditor(context_.viewport);
    repaint();
}

void FormController::hideEditor() {
    const bool focused = lineEditor_->isFocused() || areaEditor_->isFocused();
    editing_.reset();
    lineEditor_->setFrame(kHiddenFrame);
    lineEditor_->setText({});
    lineEditor_->setEchoCharacter(char32_t{0});
    areaEditor_->setFrame(kHiddenFrame);
    areaEditor_->setText({});
    if (focused) context_.setFocus(nullptr);
    repaint();
}

bool FormController::commitEditor() {
    if (!editing_.has_value()) return true;
    DocumentTab* tab = context_.workspace.tabById(editing_->tab);
    if (tab == nullptr || tab->session() == nullptr) {
        hideEditor();
        return true;
    }
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || editing_->target.field >= info->fields.size()) {
        hideEditor();
        return true;
    }
    const pdf::PdfFormField& field = info->fields[editing_->target.field];
    const std::string text =
        pdf::sanitizeFormText(field, editing_->multiline ? areaEditor_->text() : lineEditor_->text());
    if (text == editing_->original) {
        hideEditor();
        return true;
    }
    editor::DocumentSession& session = *tab->session();
    if (session.isEditingLocked()) {
        context_.setStatus("Form: " + session.editingLockReason());
        return false;
    }
    auto command = editor::makeSetFormValue(session.pageModel(), *info, field.name, pdf::PdfFormValue::ofText(text));
    if (!command.has_value()) {
        context_.setStatus("Form: " + command.error().message);
        return false;
    }
    if (*command != nullptr) {
        const core::Status status = session.execute(std::move(*command));
        if (!status.has_value()) {
            context_.setStatus("Form: " + status.error().message);
            return false;
        }
    }
    hideEditor();
    return true;
}

void FormController::cancelEditor() {
    if (editing_.has_value()) hideEditor();
}

void FormController::openPopup(const Target& target) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr || info->fields[target.field].options.empty()) {
        context_.setStatus("Form: this field has no options");
        return;
    }
    Popup popup;
    popup.tab = tab->id();
    popup.target = target;
    const auto current = tab->session()->forms().currentValue(target.field);
    if (current.has_value() && !current->selected.empty()) popup.highlighted = current->selected.front();
    popup_ = popup;
    repaint();
}

void FormController::closePopup() {
    if (!popup_.has_value()) return;
    popup_.reset();
    repaint();
}

void FormController::choose(std::size_t option) {
    const auto state = popup();
    if (!state.has_value() || option >= state->labels.size()) return;
    const Target target = popup_->target;
    if (state->multi) {
        std::vector<std::uint32_t> selected;
        for (std::size_t i = 0; i < state->selected.size(); ++i) {
            const bool on = i == option ? !state->selected[i] : state->selected[i];
            if (on) selected.push_back(static_cast<std::uint32_t>(i));
        }
        popup_->highlighted = option;
        setValue(target, pdf::PdfFormValue::ofChoice(std::move(selected)));
        return; // multi-select keeps the popup open
    }
    closePopup();
    setValue(target, pdf::PdfFormValue::ofChoice({static_cast<std::uint32_t>(option)}));
}

void FormController::traverse(bool backwards) {
    const std::vector<FormStop> order = stops();
    if (order.empty()) return;
    std::optional<std::size_t> current;
    if (focus_.has_value()) {
        if (const auto target = resolve(*focus_)) {
            const FormStop here{target->page, 0, target->field, target->widget};
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i].page == here.page && order[i].field == here.field && order[i].widget == here.widget) {
                    current = i;
                }
            }
        }
    }
    const std::optional<std::size_t> next = nextFormStop(order, current, backwards);
    if (!next.has_value()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    const FormStop& stop = order[*next];
    const Focus focus{tab->id(), tab->session()->pageId(stop.page), stop.field, stop.widget};
    const auto target = resolve(focus);
    if (!target.has_value()) return;
    focusOn(*target);
    // Keep the focused widget visible.
    const std::optional<core::Rect> pageRect = context_.viewport.pageRectInViewport(target->page);
    const double zoom = context_.viewport.zoomFactor();
    const core::Rect bounds = context_.viewport.viewportBounds();
    if (!pageRect.has_value() || !contains(bounds, toViewport(*pageRect, zoom, target->displayRect).origin) ||
        !contains(bounds, core::Point{toViewport(*pageRect, zoom, target->displayRect).maxX(),
                                      toViewport(*pageRect, zoom, target->displayRect).maxY()})) {
        context_.viewport.revealContentRect(target->page, target->displayRect);
    }
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info != nullptr && info->fields[target->field].type == pdf::PdfFormFieldType::Text) openEditor(*target);
}

void FormController::announce(DocumentTab& tab) {
    if (announced_.contains(tab.id())) return;
    const pdf::PdfFormInfoPtr info = formOf(tab);
    if (info == nullptr || info->empty()) return;
    announced_.insert(tab.id());
    if (info->hasXfa) {
        context_.setStatus("Form: XFA forms are view-only in Rivet");
    } else if (info->limitsExceeded) {
        context_.setStatus("Form: this form has too many fields to fill; it is view-only");
    } else if (info->hasScriptActions) {
        context_.setStatus("Form: field scripts do not run in Rivet; values are saved as typed");
    }
}

void FormController::repaint() {
    context_.viewport.invalidate();
}

// --- Layer client ---------------------------------------------------------------

bool FormController::popupPress(const ui::ViewportToolHost& host, core::Point viewportPoint) {
    const std::optional<core::Rect> frame = popupFrame(host);
    if (!frame.has_value() || !contains(*frame, viewportPoint)) {
        closePopup();
        return true;
    }
    const auto row = static_cast<std::size_t>((viewportPoint.y - frame->minY()) / kPopupRowHeight);
    choose(popup_->scroll + row);
    return true;
}

bool FormController::press(std::size_t page, core::Point display) {
    const std::optional<Target> target = widgetAt(page, display);
    DocumentTab* tab = activeTab();
    if (!target.has_value() || tab == nullptr) {
        if (editing_.has_value() && !commitEditor()) return true; // keep the refused editor
        clearFocus();
        return false;
    }
    const pdf::PdfFormInfoPtr info = formOf(*tab);
    if (info == nullptr) return false;
    const pdf::PdfFormField& field = info->fields[target->field];
    if (!info->fillable() || !field.fillable()) {
        if (editing_.has_value() && !commitEditor()) return true;
        clearFocus();
        reportUnfillable(*info, field);
        return false; // text selection keeps working over view-only fields
    }
    if (editing_.has_value()) {
        const bool same = editing_->target.pageId == target->pageId && editing_->target.field == target->field &&
                          editing_->target.widget == target->widget;
        if (same) return false; // the editor widget handles its own clicks
        if (!commitEditor()) return true;
    }
    focusOn(*target);
    activate(*target);
    return true;
}

void FormController::pressOutside() {
    if (editing_.has_value() && !commitEditor()) return;
    clearFocus();
}

void FormController::hover(bool onPage, std::size_t page, core::Point display) {
    std::optional<std::pair<std::size_t, core::Point>> next;
    if (onPage) next = std::make_pair(page, display);
    const auto widgetOf = [this](const std::optional<std::pair<std::size_t, core::Point>>& at) {
        return at.has_value() ? widgetAt(at->first, at->second) : std::nullopt;
    };
    const auto before = widgetOf(hover_);
    hover_ = next;
    const auto after = widgetOf(hover_);
    const bool changed = before.has_value() != after.has_value() ||
                         (before.has_value() && (before->pageId != after->pageId || before->field != after->field ||
                                                 before->widget != after->widget));
    if (changed) repaint();
}

bool FormController::key(const ui::KeyEvent& event) {
    if (event.modifiers.command || event.modifiers.control) return false;
    if (popup_.has_value()) {
        const auto state = popup();
        if (!state.has_value()) {
            closePopup();
            return false;
        }
        const std::size_t count = state->labels.size();
        switch (event.key) {
        case ui::Key::Up:
            if (popup_->highlighted > 0) --popup_->highlighted;
            break;
        case ui::Key::Down:
            if (popup_->highlighted + 1 < count) ++popup_->highlighted;
            break;
        case ui::Key::Enter:
        case ui::Key::Space:
            choose(popup_->highlighted);
            if (state->multi && event.key == ui::Key::Enter) closePopup();
            return true;
        case ui::Key::Escape:
        case ui::Key::Tab:
            closePopup();
            if (event.key == ui::Key::Tab) traverse(event.modifiers.shift);
            return true;
        default:
            return true; // the popup owns the keyboard while open
        }
        // Keep the highlighted row visible.
        if (popup_->highlighted < popup_->scroll) popup_->scroll = popup_->highlighted;
        if (popup_->highlighted >= popup_->scroll + kPopupMaxRows) popup_->scroll = popup_->highlighted + 1 - kPopupMaxRows;
        repaint();
        return true;
    }
    if (event.key == ui::Key::Tab) {
        // From an open editor (its widget leaves Tab alone): commit, move.
        if (editing_.has_value() && !commitEditor()) return true;
        if (stops().empty()) return false;
        traverse(event.modifiers.shift);
        return true;
    }
    if (!focus_.has_value()) return false;
    const std::optional<Target> target = resolve(*focus_);
    if (!target.has_value()) {
        clearFocus();
        return false;
    }
    switch (event.key) {
    case ui::Key::Space:
    case ui::Key::Enter:
        activate(*target);
        return true;
    case ui::Key::Escape:
        clearFocus();
        return true;
    default:
        break;
    }
    return false;
}

bool FormController::handleEscape() {
    if (editing_.has_value()) {
        cancelEditor();
        return true;
    }
    if (popup_.has_value()) {
        closePopup();
        return true;
    }
    if (focus_.has_value()) {
        clearFocus();
        return true;
    }
    return false;
}

void FormController::placeEditor(const ui::ViewportToolHost& host) {
    if (!editing_.has_value()) return;
    // The editor follows its target (moved page, zoom, scroll); a vanished
    // target closes it without a command.
    const std::optional<Target> target =
        resolve(Focus{editing_->tab, editing_->target.pageId, editing_->target.field, editing_->target.widget});
    if (!target.has_value()) {
        hideEditor();
        return;
    }
    editing_->target = *target;
    const std::optional<core::Rect> pageRect = host.pageRectInViewport(target->page);
    const double zoom = host.zoomFactor();
    ui::Widget* editor = editing_->multiline ? static_cast<ui::Widget*>(areaEditor_) : lineEditor_;
    ui::Widget* other = editing_->multiline ? static_cast<ui::Widget*>(lineEditor_) : areaEditor_;
    other->setFrame(kHiddenFrame);
    if (!pageRect.has_value() || zoom <= 0.0) {
        editor->setFrame(kHiddenFrame);
        return;
    }
    const core::Rect rect = toViewport(*pageRect, zoom, target->displayRect);
    core::Rect frame{viewportFrame_.origin.x + rect.origin.x - kEditorPadding,
                     viewportFrame_.origin.y + rect.origin.y - kEditorPadding, rect.size.width + 2.0 * kEditorPadding,
                     rect.size.height + 2.0 * kEditorPadding};
    if (editing_->multiline) {
        const double size = editing_->fontSize * zoom;
        areaEditor_->setMetrics(size, size * kFormLineFactor, kEditorPadding);
    } else {
        frame.size.height = std::max(frame.size.height, ui::TextField::kMinHeight);
    }
    const core::Rect view = viewportFrame_;
    const bool visible = frame.maxX() > view.minX() && frame.minX() < view.maxX() && frame.maxY() > view.minY() &&
                         frame.minY() < view.maxY();
    editor->setFrame(visible ? frame : kHiddenFrame);
}

void FormController::paintPage(const ui::ViewportToolHost& host, std::size_t page, const core::Rect& pageRect,
                               ui::PaintContext& context) const {
    paintWidgets(page, pageRect, host.zoomFactor(), context, true);
}

void FormController::paintThumbnail(std::size_t page, const core::Rect& pageRect, double zoom,
                                    ui::PaintContext& context) const {
    paintWidgets(page, pageRect, zoom, context, false);
}

void FormController::paintWidgets(std::size_t page, const core::Rect& pageRect, double zoom,
                                  ui::PaintContext& context, bool decorations) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || page >= tab->session()->pageCount()) return;
    editor::DocumentSession& session = *tab->session();
    const pdf::PdfFormInfoPtr info = session.forms().info();
    if (info == nullptr || info->empty()) return;
    const core::PageId pageId = session.pageId(page);
    const std::vector<editor::FormWidgetView> widgets = session.forms().widgets(pageId);
    if (widgets.empty()) return;
    const std::optional<Target> hovered = decorations && hover_.has_value() && hover_->first == page
                                              ? widgetAt(hover_->first, hover_->second)
                                              : std::nullopt;
    const FormTextMeasure measure = [&context](std::string_view text, double size) {
        return context.measureText(text, ui::Font{size}).width;
    };
    for (const editor::FormWidgetView& view : widgets) {
        const pdf::PdfFormField& field = info->fields[view.field];
        const core::Rect rect = toViewport(pageRect, zoom, view.displayRect);
        const bool fillable = info->fillable() && field.fillable();
        if (decorations && highlight_ && fillable) context.fillRect(rect, highlightColor());
        const bool editing = decorations && editing_.has_value() && editing_->target.pageId == pageId &&
                             editing_->target.field == view.field && editing_->target.widget == view.widget;
        if (view.edited && !editing) {
            // The raster hides this widget: draw the edited value.
            if (decorations) context.strokeRect(rect, editedBorder(), 1.0);
            const auto value = session.forms().currentValue(view.field);
            if (value.has_value()) {
                if (field.isToggle()) {
                    const std::string& onState = field.widgets[view.widget].onState;
                    if (!onState.empty() && value->text == onState) {
                        if (field.type == pdf::PdfFormFieldType::RadioButton) {
                            paintDot(context, rect);
                        } else {
                            paintCheck(context, rect);
                        }
                    }
                } else if (field.type == pdf::PdfFormFieldType::ListBox) {
                    const double size = formFontSize(field, view.displayRect, {}, {});
                    const double rowHeight = std::max(1.0, size * kFormLineFactor);
                    context.pushClip(rect);
                    for (std::size_t i = 0; i < field.options.size(); ++i) {
                        const core::Rect row{view.displayRect.origin.x,
                                             view.displayRect.origin.y + rowHeight * static_cast<double>(i),
                                             view.displayRect.size.width, rowHeight};
                        if (row.minY() > view.displayRect.maxY()) break;
                        const bool on = std::find(value->selected.begin(), value->selected.end(),
                                                  static_cast<std::uint32_t>(i)) != value->selected.end();
                        const core::Rect viewRow = relativeTo(rect, toViewport(pageRect, zoom, row));
                        if (on) context.fillRect(viewRow, ui::colors().formOptionHighlight);
                        const core::Rect textRow{viewRow.origin.x + kFormTextPadding * zoom, viewRow.origin.y,
                                                 std::max(0.0, viewRow.size.width - 2.0 * kFormTextPadding * zoom),
                                                 viewRow.size.height};
                        context.drawText(field.options[i].label, textRow, ui::Font{size * zoom}, inkColor(),
                                         ui::TextAlign::Left);
                    }
                    context.popClip();
                } else {
                    std::string text = field.isChoice() ? formChoiceText(field, *value) : value->text;
                    if (field.password) text = maskFormText(text);
                    pdf::PdfFormField layoutField = field;
                    if (field.isChoice()) layoutField.multiline = false;
                    const FormTextLayout layout = layoutFormText(layoutField, view.displayRect, text, measure);
                    context.pushClip(rect);
                    for (const FormTextLine& line : layout.lines) {
                        context.drawText(line.text, relativeTo(rect, toViewport(pageRect, zoom, line.rect)),
                                         ui::Font{layout.fontSize * zoom}, inkColor(),
                                         line.centered ? ui::TextAlign::Center : ui::TextAlign::Left);
                    }
                    context.popClip();
                }
            }
        }
        if (!decorations) continue;
        const bool focused = focus_.has_value() && focus_->tab == tab->id() && focus_->page == pageId &&
                             focus_->field == view.field && focus_->widget == view.widget;
        if (focused) {
            context.strokeRect(rect, AnnotationPainter::accentColor(), kFocusWidth);
        } else if (hovered.has_value() && hovered->field == view.field && hovered->widget == view.widget &&
                   fillable && interactive()) {
            context.strokeRect(rect, hoverColor(), 1.0);
        }
    }
}

void FormController::paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    const std::optional<core::Rect> frame = popupFrame(host);
    const auto state = popup();
    if (!frame.has_value() || !state.has_value()) return;
    context.fillRect(*frame, ui::colors().editorPaper);
    context.strokeRect(*frame, ui::colors().editorPaperBorder, 1.0);
    const std::size_t first = popup_->scroll;
    for (std::size_t row = 0; row < kPopupMaxRows && first + row < state->labels.size(); ++row) {
        const std::size_t option = first + row;
        const core::Rect rowRect{frame->minX(), frame->minY() + kPopupRowHeight * static_cast<double>(row),
                                 frame->size.width, kPopupRowHeight};
        if (option == state->highlighted) context.fillRect(rowRect, ui::colors().formOptionHighlight);
        const core::Rect textRect{rowRect.minX() + 22.0, rowRect.minY(), std::max(0.0, rowRect.size.width - 28.0),
                                  rowRect.size.height};
        if (state->selected[option]) {
            paintCheck(context, core::Rect{rowRect.minX() + 4.0, rowRect.minY() + 3.0, 14.0, 14.0});
        }
        context.drawText(state->labels[option], textRect, ui::Font{13.0}, inkColor(), ui::TextAlign::Left);
    }
}

// --- Menus ---------------------------------------------------------------------------

bool FormController::canPerform(FormCommand command) const {
    switch (command) {
    case FormCommand::HighlightFields:
        return true;
    case FormCommand::ResetForm: {
        DocumentTab* tab = activeTab();
        if (tab == nullptr || tab->session()->isEditingLocked()) return false;
        const pdf::PdfFormValuesPtr& values = tab->session()->pageSnapshot()->formValues();
        return values != nullptr && !values->empty();
    }
    }
    return false;
}

void FormController::perform(FormCommand command) {
    switch (command) {
    case FormCommand::HighlightFields:
        highlight_ = !highlight_;
        repaint();
        return;
    case FormCommand::ResetForm: {
        DocumentTab* tab = activeTab();
        if (tab == nullptr) return;
        cancelEditor();
        closePopup();
        const pdf::PdfFormInfoPtr info = formOf(*tab);
        if (info == nullptr) return;
        editor::DocumentSession& session = *tab->session();
        auto reset = editor::makeResetForm(session.pageModel(), *info);
        if (!reset.has_value()) {
            context_.setStatus("Form: " + reset.error().message);
            return;
        }
        if (*reset == nullptr) {
            context_.setStatus("Form: nothing to reset");
            return;
        }
        const core::Status status = session.execute(std::move(*reset));
        if (!status.has_value()) {
            context_.setStatus("Form: " + status.error().message);
            return;
        }
        context_.setStatus("Form reset");
        repaint();
        return;
    }
    }
}

std::optional<bool> FormController::isChecked(FormCommand command) const {
    if (command == FormCommand::HighlightFields) return highlight_;
    return std::nullopt;
}

} // namespace rivet::app
