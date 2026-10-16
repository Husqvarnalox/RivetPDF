// SPDX-License-Identifier: MPL-2.0

#include "PdfiumForms.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "PdfiumAnnotations.h"
#include "PdfiumDisplayTransform.h"
#include "PdfiumStrings.h"
#include "fpdf_annot.h"
#include "fpdf_formfill.h"

// AcroForm reading and filling through the public FPDFAnnot_* / FORM_* API.
//
// Safety (ADR-0007, ADR-0024): the environment is created with version 1 of
// FPDF_FORMFILLINFO (no XFA) and no JavaScript platform, so no script ever
// runs; document-level JS/open/additional actions are never triggered. The
// embedder callbacks are inert stubs: no timers, no navigation, no URI
// launches, no named actions.

namespace rivet::pdf::internal {
namespace {

// /Ff bits not named by fpdf_annot.h (PDF 32000-1, table 228).
constexpr std::uint32_t kFlagDoNotScroll = 1U << 23;
constexpr std::uint32_t kFlagComb = 1U << 24;
// Bounded walk up the /Parent chain for inheritable numbers.
constexpr int kMaxParentDepth = 16;
// UTF-16 code unit used to toggle a focused check box / radio button.
constexpr int kSpaceChar = 0x20;

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "pdf");
}

// --- Inert embedder callbacks --------------------------------------------------

void stubInvalidate(FPDF_FORMFILLINFO*, FPDF_PAGE, double, double, double, double) {}
void stubOutputSelectedRect(FPDF_FORMFILLINFO*, FPDF_PAGE, double, double, double, double) {}
void stubSetCursor(FPDF_FORMFILLINFO*, int) {}
int stubSetTimer(FPDF_FORMFILLINFO*, int, TimerCallback) {
    return 0; // no timers: caret blinking etc. never run
}
void stubKillTimer(FPDF_FORMFILLINFO*, int) {}
FPDF_SYSTEMTIME stubGetLocalTime(FPDF_FORMFILLINFO*) {
    FPDF_SYSTEMTIME time;
    std::memset(&time, 0, sizeof(time));
    return time;
}
void stubOnChange(FPDF_FORMFILLINFO*) {}
FPDF_PAGE stubGetCurrentPage(FPDF_FORMFILLINFO*, FPDF_DOCUMENT) {
    return nullptr;
}
int stubGetRotation(FPDF_FORMFILLINFO*, FPDF_PAGE) {
    return 0;
}
void stubExecuteNamedAction(FPDF_FORMFILLINFO*, FPDF_BYTESTRING) {}
void stubSetTextFieldFocus(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) {}
void stubDoURIAction(FPDF_FORMFILLINFO*, FPDF_BYTESTRING) {}
void stubDoGoToAction(FPDF_FORMFILLINFO*, int, int, float*, int) {}

} // namespace

FormEnvironment::FormEnvironment(FPDF_DOCUMENT document) : document_(document) {
    FPDF_FORMFILLINFO& info = info_.base;
    std::memset(&info, 0, sizeof(info));
    info.version = 1;              // stable interface only: no XFA
    info.m_pJsPlatform = nullptr;  // no JavaScript, ever
    info.FFI_Invalidate = stubInvalidate;
    info.FFI_OutputSelectedRect = stubOutputSelectedRect;
    info.FFI_SetCursor = stubSetCursor;
    info.FFI_SetTimer = stubSetTimer;
    info.FFI_KillTimer = stubKillTimer;
    info.FFI_GetLocalTime = stubGetLocalTime;
    info.FFI_OnChange = stubOnChange;
    info.FFI_GetPage = getPage;
    info.FFI_GetCurrentPage = stubGetCurrentPage;
    info.FFI_GetRotation = stubGetRotation;
    info.FFI_ExecuteNamedAction = stubExecuteNamedAction;
    info.FFI_SetTextFieldFocus = stubSetTextFieldFocus;
    info.FFI_DoURIAction = stubDoURIAction;
    info.FFI_DoGoToAction = stubDoGoToAction;
    info_.owner = this;
    handle_ = FPDFDOC_InitFormFillEnvironment(document, &info);
}

FormEnvironment::~FormEnvironment() {
    closePages();
    if (handle_ != nullptr) FPDFDOC_ExitFormFillEnvironment(handle_);
}

FPDF_PAGE FormEnvironment::page(int index) {
    const auto it = pages_.find(index);
    if (it != pages_.end()) return it->second;
    FPDF_PAGE loaded = FPDF_LoadPage(document_, index);
    if (loaded == nullptr) return nullptr;
    // Registered before the form view exists: PDFium may ask for the same
    // page while it builds the view.
    pages_.emplace(index, loaded);
    if (handle_ != nullptr) FORM_OnAfterLoadPage(loaded, handle_);
    return loaded;
}

void FormEnvironment::closePages() {
    for (auto& [index, loaded] : pages_) {
        (void)index;
        if (handle_ != nullptr) FORM_OnBeforeClosePage(loaded, handle_);
        FPDF_ClosePage(loaded);
    }
    pages_.clear();
}

FPDF_PAGE FormEnvironment::getPage(FPDF_FORMFILLINFO* info, FPDF_DOCUMENT document, int index) {
    FormEnvironment* self = reinterpret_cast<Info*>(info)->owner;
    if (self == nullptr || document != self->document_ || index < 0) return nullptr;
    return self->page(index);
}

namespace {

// A UTF-16 string returned by a PDFium "size query, then fill" getter as
// UTF-8. nullopt = longer than `maxBytes` (a UTF-16 unit never encodes to
// fewer than one UTF-8 byte, so the unit count is a safe lower bound) or a
// failed call. `get(buffer, length)` returns the byte size incl. terminator.
template <typename Getter>
std::optional<std::string> readWide(Getter&& get, std::size_t maxBytes) {
    const unsigned long needed = get(nullptr, 0UL);
    if (needed < 2) return needed == 0 ? std::nullopt : std::optional<std::string>(std::string{});
    if (needed / 2 - 1 > maxBytes) return std::nullopt;
    std::vector<FPDF_WCHAR> buffer((needed + 1) / 2);
    const unsigned long written = get(buffer.data(), needed);
    if (written < 2) return std::string{};
    const std::size_t bytes = std::min<std::size_t>(written, needed) & ~std::size_t{1};
    std::string text = utf16leToUtf8(reinterpret_cast<const std::uint8_t*>(buffer.data()), bytes);
    // Drop the terminator the conversion may have kept.
    while (!text.empty() && text.back() == '\0') text.pop_back();
    if (text.size() > maxBytes) return std::nullopt;
    return text;
}

std::vector<FPDF_WCHAR> wide(std::string_view text) {
    const std::vector<std::uint8_t> bytes = utf8ToUtf16le(text);
    std::vector<FPDF_WCHAR> units(bytes.size() / 2);
    for (std::size_t i = 0; i < units.size(); ++i) {
        units[i] = static_cast<FPDF_WCHAR>(static_cast<unsigned>(bytes[2 * i]) |
                                           (static_cast<unsigned>(bytes[2 * i + 1]) << 8));
    }
    return units;
}

// CR LF / CR -> LF (PDFium's edit control stores paragraph breaks as CR).
std::string normalizeBreaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

PdfFormFieldType typeOf(int type) {
    switch (type) {
    case FPDF_FORMFIELD_PUSHBUTTON:
        return PdfFormFieldType::PushButton;
    case FPDF_FORMFIELD_CHECKBOX:
        return PdfFormFieldType::CheckBox;
    case FPDF_FORMFIELD_RADIOBUTTON:
        return PdfFormFieldType::RadioButton;
    case FPDF_FORMFIELD_COMBOBOX:
        return PdfFormFieldType::ComboBox;
    case FPDF_FORMFIELD_LISTBOX:
        return PdfFormFieldType::ListBox;
    case FPDF_FORMFIELD_TEXTFIELD:
        return PdfFormFieldType::Text;
    case FPDF_FORMFIELD_SIGNATURE:
        return PdfFormFieldType::Signature;
    default:
        return PdfFormFieldType::Unknown;
    }
}

PdfBox boxFromRect(const FS_RECTF& r) {
    PdfBox box{static_cast<double>(r.left), static_cast<double>(r.bottom), static_cast<double>(r.right),
               static_cast<double>(r.top)};
    if (box.left > box.right) std::swap(box.left, box.right);
    if (box.bottom > box.top) std::swap(box.bottom, box.top);
    return box;
}

std::optional<std::string> fieldName(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot) {
    return readWide([&](FPDF_WCHAR* buffer, unsigned long length) {
        return FPDFAnnot_GetFormFieldName(form, annot, buffer, length);
    }, kMaxFormFieldNameBytes);
}

std::optional<std::string> fieldValue(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot) {
    return readWide([&](FPDF_WCHAR* buffer, unsigned long length) {
        return FPDFAnnot_GetFormFieldValue(form, annot, buffer, length);
    }, kMaxFormValueBytes);
}

// /MaxLen of the widget or the nearest ancestor field that has one.
std::uint32_t maxLengthOf(FPDF_ANNOTATION annot) {
    const auto read = [](FPDF_ANNOTATION dict) -> std::optional<std::uint32_t> {
        float value = 0.0F;
        if (FPDFAnnot_HasKey(dict, "MaxLen") == 0 || FPDFAnnot_GetNumberValue(dict, "MaxLen", &value) == 0) {
            return std::nullopt;
        }
        return std::isfinite(value) && value > 0.0F ? static_cast<std::uint32_t>(std::min(value, 1.0e9F)) : 0U;
    };
    if (const auto own = read(annot)) return *own;
    FPDF_ANNOTATION current = FPDFAnnot_GetLinkedAnnot(annot, "Parent");
    std::uint32_t result = 0;
    for (int depth = 0; current != nullptr && depth < kMaxParentDepth; ++depth) {
        if (const auto inherited = read(current)) {
            result = *inherited;
            break;
        }
        FPDF_ANNOTATION next = FPDFAnnot_GetLinkedAnnot(current, "Parent");
        FPDFPage_CloseAnnot(current);
        current = next;
    }
    if (current != nullptr) FPDFPage_CloseAnnot(current);
    return result;
}

bool hasScript(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot) {
    const int events[] = {FPDF_ANNOT_AACTION_KEY_STROKE, FPDF_ANNOT_AACTION_FORMAT,
                          FPDF_ANNOT_AACTION_VALIDATE, FPDF_ANNOT_AACTION_CALCULATE};
    for (const int event : events) {
        if (FPDFAnnot_GetFormAdditionalActionJavaScript(form, annot, event, nullptr, 0) > 2) return true;
    }
    return false;
}

// Field-level facts read from its first widget.
void describeField(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot, PdfFormField& field) {
    const auto flags = static_cast<std::uint32_t>(std::max(0, FPDFAnnot_GetFormFieldFlags(form, annot)));
    field.flags = flags;
    field.readOnly = (flags & FPDF_FORMFLAG_READONLY) != 0;
    field.required = (flags & FPDF_FORMFLAG_REQUIRED) != 0;
    if (field.type == PdfFormFieldType::Text) {
        field.multiline = (flags & FPDF_FORMFLAG_TEXT_MULTILINE) != 0;
        field.password = (flags & FPDF_FORMFLAG_TEXT_PASSWORD) != 0;
        field.doNotScroll = (flags & kFlagDoNotScroll) != 0;
        field.comb = (flags & kFlagComb) != 0;
        field.maxLength = maxLengthOf(annot);
    }
    if (field.type == PdfFormFieldType::ComboBox) {
        field.editable = (flags & FPDF_FORMFLAG_CHOICE_EDIT) != 0;
    }
    if (field.type == PdfFormFieldType::ListBox) {
        field.multiSelect = (flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT) != 0;
    }
    float size = 0.0F;
    if (FPDFAnnot_GetFontSize(form, annot, &size) != 0 && std::isfinite(size) && size > 0.0F && size < 1000.0F) {
        field.fontSize = size;
    }
    if (const auto value = fieldValue(form, annot)) {
        field.value = field.multiline ? normalizeBreaks(*value) : *value;
    } else {
        field.overLimit = true;
    }
    if (field.isChoice()) {
        const int count = FPDFAnnot_GetOptionCount(form, annot);
        if (count > static_cast<int>(kMaxFormOptions)) {
            field.overLimit = true;
        } else {
            for (int i = 0; i < count; ++i) {
                const auto label = readWide([&](FPDF_WCHAR* buffer, unsigned long length) {
                    return FPDFAnnot_GetOptionLabel(form, annot, i, buffer, length);
                }, kMaxFormValueBytes);
                if (!label) {
                    field.overLimit = true;
                    break;
                }
                PdfFormOption option;
                option.label = *label;
                option.exportValue = *label;
                option.selected = FPDFAnnot_IsOptionSelected(form, annot, i) != 0;
                field.options.push_back(std::move(option));
            }
        }
    }
    field.hasScriptActions = hasScript(form, annot);
}

// --- Filling -------------------------------------------------------------------------

struct Target {
    FPDF_PAGE page = nullptr;
    const PdfFormWidget* widget = nullptr;
};

// The widget's annotation, verified to belong to field `name` (null
// otherwise). The caller owns (and closes) the handle.
FPDF_ANNOTATION openWidget(FPDF_FORMHANDLE form, const Target& target, const std::string& name) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(target.page, static_cast<int>(target.widget->annotIndex));
    if (annot == nullptr) return nullptr;
    const auto actual = FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_WIDGET ? fieldName(form, annot) : std::nullopt;
    if (!actual || *actual != name) {
        FPDFPage_CloseAnnot(annot);
        return nullptr;
    }
    return annot;
}

bool replaceText(FPDF_FORMHANDLE form, FPDF_PAGE page, const std::string& text) {
    FORM_SelectAllText(form, page);
    const std::vector<FPDF_WCHAR> units = wide(text);
    FORM_ReplaceSelection(form, page, units.data());
    return true;
}

bool applyText(FPDF_FORMHANDLE form, const Target& target, const std::string& name, const std::string& text) {
    ScopedAnnot annot(openWidget(form, target, name));
    if (annot.get() == nullptr || FORM_SetFocusedAnnot(form, annot.get()) == 0) return false;
    replaceText(form, target.page, text);
    return FORM_ForceToKillFocus(form) != 0;
}

bool applyChoice(FPDF_FORMHANDLE form, const Target& target, const PdfFormField& field,
                 const PdfFormValue& value) {
    ScopedAnnot annot(openWidget(form, target, field.name));
    if (annot.get() == nullptr || FORM_SetFocusedAnnot(form, annot.get()) == 0) return false;
    if (!value.text.empty()) {
        replaceText(form, target.page, value.text);
    } else {
        const int count = FPDFAnnot_GetOptionCount(form, annot.get());
        const auto wanted = [&value](int index) {
            return std::find(value.selected.begin(), value.selected.end(), static_cast<std::uint32_t>(index)) !=
                   value.selected.end();
        };
        if (field.type == PdfFormFieldType::ListBox && field.multiSelect) {
            for (int i = 0; i < count; ++i) {
                if (!wanted(i) && FPDFAnnot_IsOptionSelected(form, annot.get(), i) != 0) {
                    FORM_SetIndexSelected(form, target.page, i, 0);
                }
            }
        }
        for (const std::uint32_t index : value.selected) {
            FORM_SetIndexSelected(form, target.page, static_cast<int>(index), 1);
        }
    }
    return FORM_ForceToKillFocus(form) != 0;
}

bool toggleWidget(FPDF_FORMHANDLE form, const Target& target, const std::string& name) {
    ScopedAnnot annot(openWidget(form, target, name));
    if (annot.get() == nullptr || FORM_SetFocusedAnnot(form, annot.get()) == 0) return false;
    FORM_OnChar(form, target.page, kSpaceChar, 0);
    return FORM_ForceToKillFocus(form) != 0;
}

bool isChecked(FPDF_FORMHANDLE form, const Target& target, const std::string& name) {
    ScopedAnnot annot(openWidget(form, target, name));
    return annot.get() != nullptr && FPDFAnnot_IsChecked(form, annot.get()) != 0;
}

bool applyToggle(FPDF_FORMHANDLE form, const std::vector<Target>& targets, const PdfFormField& field,
                 const PdfFormValue& value) {
    if (value.text.empty()) {
        // Off: toggle the checked widget (check boxes; radio buttons usually
        // refuse, which verification reports).
        for (const Target& target : targets) {
            if (isChecked(form, target, field.name)) return toggleWidget(form, target, field.name);
        }
        return true;
    }
    for (const Target& target : targets) {
        if (target.widget->onState != value.text) continue;
        if (isChecked(form, target, field.name)) return true;
        return toggleWidget(form, target, field.name);
    }
    return false; // no widget of that state in this document
}

bool verify(FPDF_FORMHANDLE form, const std::vector<Target>& targets, const PdfFormField& field,
            const PdfFormValue& value) {
    const Target& first = targets.front();
    ScopedAnnot annot(openWidget(form, first, field.name));
    if (annot.get() == nullptr) return false;
    switch (field.type) {
    case PdfFormFieldType::Text: {
        const auto stored = fieldValue(form, annot.get());
        return stored.has_value() && normalizeBreaks(*stored) == normalizeBreaks(value.text);
    }
    case PdfFormFieldType::ComboBox:
    case PdfFormFieldType::ListBox: {
        if (!value.text.empty()) {
            const auto stored = fieldValue(form, annot.get());
            return stored.has_value() && *stored == value.text;
        }
        const int count = FPDFAnnot_GetOptionCount(form, annot.get());
        for (int i = 0; i < count; ++i) {
            const bool wanted = std::find(value.selected.begin(), value.selected.end(),
                                          static_cast<std::uint32_t>(i)) != value.selected.end();
            if ((FPDFAnnot_IsOptionSelected(form, annot.get(), i) != 0) != wanted) return false;
        }
        return true;
    }
    case PdfFormFieldType::CheckBox:
    case PdfFormFieldType::RadioButton:
        for (const Target& target : targets) {
            const bool on = !value.text.empty() && target.widget->onState == value.text;
            if (isChecked(form, target, field.name) != on) return false;
        }
        return true;
    case PdfFormFieldType::Unknown:
    case PdfFormFieldType::PushButton:
    case PdfFormFieldType::Signature:
        break;
    }
    return false;
}

} // namespace

core::Result<PdfFormInfo> readFormInfo(FPDF_DOCUMENT reader, std::size_t pageCount) {
    PdfFormInfo info;
    const int formType = FPDF_GetFormType(reader);
    if (formType == FORMTYPE_NONE) return info;
    info.hasXfa = formType == FORMTYPE_XFA_FULL || formType == FORMTYPE_XFA_FOREGROUND;

    FormEnvironment env(reader);
    if (env.get() == nullptr) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium could not create the form environment", "pdf"));
    }
    const FPDF_FORMHANDLE form = env.get();

    std::map<std::string, std::size_t> byName;
    std::size_t widgets = 0;
    for (std::size_t pageIndex = 0; pageIndex < pageCount && !info.limitsExceeded; ++pageIndex) {
        // Pages are released after each page so a long document never holds
        // more than a few open (the annotations are closed first).
        struct PageScope {
            FormEnvironment& environment;
            ~PageScope() { environment.closePages(); }
        } scope{env};
        FPDF_PAGE page = env.page(static_cast<int>(pageIndex));
        if (page == nullptr) continue; // unreadable page: its widgets are simply not listed
        const int count = std::max(0, FPDFPage_GetAnnotCount(page));
        for (int i = 0; i < count; ++i) {
            ScopedAnnot annot(FPDFPage_GetAnnot(page, i));
            if (annot.get() == nullptr || FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_WIDGET) continue;
            const PdfFormFieldType type = typeOf(FPDFAnnot_GetFormFieldType(form, annot.get()));
            if (type == PdfFormFieldType::Unknown) continue; // not part of the form
            const auto name = fieldName(form, annot.get());
            if (!name || name->empty()) continue;
            if (++widgets > kMaxFormWidgets) {
                info.limitsExceeded = true;
                break;
            }

            PdfFormWidget widget;
            widget.pageIndex = pageIndex;
            widget.annotIndex = static_cast<std::uint32_t>(i);
            FS_RECTF rect{};
            if (FPDFAnnot_GetRect(annot.get(), &rect) != 0) widget.rect = boxFromRect(rect);
            if (type == PdfFormFieldType::CheckBox || type == PdfFormFieldType::RadioButton) {
                widget.onState = readWide([&](FPDF_WCHAR* buffer, unsigned long length) {
                                     return FPDFAnnot_GetFormFieldExportValue(form, annot.get(), buffer, length);
                                 }, kMaxFormValueBytes)
                                     .value_or(std::string{});
                widget.checked = FPDFAnnot_IsChecked(form, annot.get()) != 0;
            }

            auto known = byName.find(*name);
            if (known == byName.end()) {
                if (info.fields.size() >= kMaxFormFields) {
                    info.limitsExceeded = true;
                    break;
                }
                PdfFormField field;
                field.name = *name;
                field.type = type;
                describeField(form, annot.get(), field);
                info.hasScriptActions = info.hasScriptActions || field.hasScriptActions;
                known = byName.emplace(*name, info.fields.size()).first;
                info.fields.push_back(std::move(field));
            }
            PdfFormField& field = info.fields[known->second];
            if (widget.onState.empty() && field.isToggle()) field.overLimit = true; // no usable on-state
            field.widgets.push_back(std::move(widget));
        }
    }
    return info;
}

core::Status applyFormValues(FPDF_DOCUMENT document,
                             const PdfFormInfo& info,
                             const PdfFormValues& values,
                             const FormPageMap& pageFor,
                             std::vector<std::string>* failures) {
    if (values.empty()) return core::ok();
    if (!info.fillable()) return std::unexpected(invalid("the document's form cannot be filled"));
    for (const auto& [name, value] : values) {
        const std::size_t index = info.find(name);
        if (index == PdfFormInfo::npos) return std::unexpected(invalid("a filled form field does not exist"));
        if (auto valid = validateFormValue(info.fields[index], value); !valid.has_value()) return valid;
    }
    if (FPDF_GetFormType(document) != FORMTYPE_ACRO_FORM) {
        return std::unexpected(invalid("the document's form cannot be filled"));
    }

    FormEnvironment env(document);
    if (env.get() == nullptr) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium could not create the form environment", "pdf"));
    }
    const FPDF_FORMHANDLE form = env.get();
    std::vector<std::string> failed;
    {
        struct Applied {
            const PdfFormField* field;
            const PdfFormValue* value;
            std::vector<Target> targets;
        };
        std::vector<Applied> applied;
        for (const auto& [name, value] : values) {
            const PdfFormField& field = info.fields[info.find(name)];
            std::vector<Target> targets;
            bool unloadable = false;
            for (const PdfFormWidget& widget : field.widgets) {
                const std::optional<int> pageIndex = pageFor(widget.pageIndex);
                if (!pageIndex.has_value()) continue;
                FPDF_PAGE page = env.page(*pageIndex);
                if (page == nullptr) {
                    unloadable = true;
                    continue;
                }
                targets.push_back(Target{page, &widget});
            }
            if (targets.empty()) {
                if (unloadable) failed.push_back(name);
                continue; // no widget in this document: skipped
            }
            bool ok = false;
            switch (field.type) {
            case PdfFormFieldType::Text:
                ok = applyText(form, targets.front(), name, value.text);
                break;
            case PdfFormFieldType::ComboBox:
            case PdfFormFieldType::ListBox:
                ok = applyChoice(form, targets.front(), field, value);
                break;
            case PdfFormFieldType::CheckBox:
            case PdfFormFieldType::RadioButton:
                ok = applyToggle(form, targets, field, value);
                break;
            case PdfFormFieldType::Unknown:
            case PdfFormFieldType::PushButton:
            case PdfFormFieldType::Signature:
                break;
            }
            FORM_ForceToKillFocus(form);
            if (!ok) {
                failed.push_back(name);
                continue;
            }
            applied.push_back(Applied{&field, &value, std::move(targets)});
        }
        // Verified after everything was applied: a later field cannot have
        // undone an earlier one unnoticed.
        for (const Applied& item : applied) {
            if (!verify(form, item.targets, *item.field, *item.value)) failed.push_back(item.field->name);
        }
    }
    if (!failed.empty()) {
        const std::size_t count = failed.size();
        if (failures != nullptr) *failures = std::move(failed);
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               std::to_string(count) + " form field(s) could not be filled",
                                               "pdf"));
    }
    return core::ok();
}

} // namespace rivet::pdf::internal
