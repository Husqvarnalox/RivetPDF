// SPDX-License-Identifier: MPL-2.0
#include "pdf/PdfForms.hpp"

#include "pdf/PdfTextLayout.hpp"

#include <algorithm>
#include <string_view>

namespace rivet::pdf {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "pdf");
}

bool isContinuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

// Byte length of the code point starting at `i` (1 for a stray byte).
std::size_t codePointLength(std::string_view text, std::size_t i) {
    std::size_t end = i + 1;
    while (end < text.size() && isContinuation(static_cast<unsigned char>(text[end]))) ++end;
    return end - i;
}

std::size_t codePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if (!isContinuation(static_cast<unsigned char>(c))) ++count;
    }
    return count;
}

bool hasLineBreak(std::string_view text) {
    return text.find_first_of("\r\n") != std::string_view::npos;
}

} // namespace

bool PdfFormField::fillable() const {
    if (readOnly || overLimit || widgets.empty()) return false;
    switch (type) {
    case PdfFormFieldType::Text:
    case PdfFormFieldType::CheckBox:
    case PdfFormFieldType::RadioButton:
    case PdfFormFieldType::ComboBox:
    case PdfFormFieldType::ListBox:
        return true;
    case PdfFormFieldType::Unknown:
    case PdfFormFieldType::PushButton:
    case PdfFormFieldType::Signature:
        break;
    }
    return false;
}

std::size_t PdfFormInfo::find(const std::string& name) const {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) return i;
    }
    return npos;
}

PdfFormValue originalValue(const PdfFormField& field) {
    if (field.isToggle()) {
        for (const PdfFormWidget& widget : field.widgets) {
            if (widget.checked) return PdfFormValue::ofToggle(widget.onState);
        }
        return PdfFormValue::ofToggle({});
    }
    if (field.isChoice()) {
        std::vector<std::uint32_t> selected;
        for (std::size_t i = 0; i < field.options.size(); ++i) {
            if (field.options[i].selected) selected.push_back(static_cast<std::uint32_t>(i));
        }
        PdfFormValue value = PdfFormValue::ofChoice(std::move(selected));
        if (value.selected.empty() && field.editable) value.text = field.value;
        return value;
    }
    return PdfFormValue::ofText(field.value);
}

core::Status validateFormValue(const PdfFormField& field, const PdfFormValue& value) {
    if (!field.fillable()) return std::unexpected(invalid("the form field cannot be filled"));
    const auto checkText = [&field](const std::string& text) -> core::Status {
        if (text.size() > kMaxFormValueBytes) return std::unexpected(invalid("the form value is too long"));
        bool bad = false;
        (void)decodeUtf8(text, &bad);
        if (bad) return std::unexpected(invalid("the form value is not valid UTF-8"));
        if (field.maxLength > 0 && codePointCount(text) > field.maxLength) {
            return std::unexpected(invalid("the form value exceeds the field's maximum length"));
        }
        if (!field.multiline && hasLineBreak(text)) {
            return std::unexpected(invalid("a single-line form field cannot hold line breaks"));
        }
        return core::ok();
    };
    switch (field.type) {
    case PdfFormFieldType::Text:
        if (value.kind != PdfFormValue::Kind::Text || !value.selected.empty()) {
            return std::unexpected(invalid("a text field needs a text value"));
        }
        return checkText(value.text);
    case PdfFormFieldType::CheckBox:
    case PdfFormFieldType::RadioButton: {
        if (value.kind != PdfFormValue::Kind::Toggle || !value.selected.empty()) {
            return std::unexpected(invalid("a check box or radio button needs an on/off value"));
        }
        if (value.text.empty()) return core::ok();
        const bool known = std::any_of(field.widgets.begin(), field.widgets.end(),
                                       [&value](const PdfFormWidget& w) { return w.onState == value.text; });
        if (!known) return std::unexpected(invalid("the form value names no state of the field"));
        return core::ok();
    }
    case PdfFormFieldType::ComboBox:
    case PdfFormFieldType::ListBox: {
        if (value.kind != PdfFormValue::Kind::Choice) {
            return std::unexpected(invalid("a choice field needs selected options"));
        }
        for (std::size_t i = 0; i < value.selected.size(); ++i) {
            if (value.selected[i] >= field.options.size()) {
                return std::unexpected(invalid("a selected option is out of range"));
            }
            if (i > 0 && value.selected[i - 1] >= value.selected[i]) {
                return std::unexpected(invalid("selected options must be ascending and unique"));
            }
        }
        const bool multi = field.type == PdfFormFieldType::ListBox && field.multiSelect;
        if (!multi && value.selected.size() > 1) {
            return std::unexpected(invalid("the field allows one selected option"));
        }
        if (!value.text.empty()) {
            if (!field.editable || !value.selected.empty()) {
                return std::unexpected(invalid("only an editable combo box takes typed text"));
            }
            return checkText(value.text);
        }
        return core::ok();
    }
    case PdfFormFieldType::Unknown:
    case PdfFormFieldType::PushButton:
    case PdfFormFieldType::Signature:
        break;
    }
    return std::unexpected(invalid("the form field cannot be filled"));
}

std::string sanitizeFormText(const PdfFormField& field, std::string text) {
    if (!field.multiline) {
        std::string flat;
        flat.reserve(text.size());
        bool inBreak = false;
        for (const char c : text) {
            if (c == '\r' || c == '\n') {
                if (!inBreak) flat.push_back(' ');
                inBreak = true;
            } else {
                flat.push_back(c);
                inBreak = false;
            }
        }
        text = std::move(flat);
    }
    std::size_t end = 0;
    std::size_t points = 0;
    while (end < text.size()) {
        if (field.maxLength > 0 && points >= field.maxLength) break;
        const std::size_t length = codePointLength(text, end);
        if (end + length > kMaxFormValueBytes) break;
        end += length;
        ++points;
    }
    text.resize(end);
    return text;
}

} // namespace rivet::pdf
