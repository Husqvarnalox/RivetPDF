// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "pdf/PdfForms.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Engine-independent AcroForm value model (ADR-0024): originalValue,
// validateFormValue, sanitizeFormText. Runs in both build modes.

namespace {

using namespace rivet::pdf;

PdfFormWidget widget(std::string onState = {}, bool checked = false) {
    PdfFormWidget w;
    w.rect = PdfBox{0, 0, 10, 10};
    w.onState = std::move(onState);
    w.checked = checked;
    return w;
}

PdfFormField textField(bool multiline = false, std::uint32_t maxLength = 0) {
    PdfFormField field;
    field.name = "t";
    field.type = PdfFormFieldType::Text;
    field.multiline = multiline;
    field.maxLength = maxLength;
    field.value = "stored";
    field.widgets.push_back(widget());
    return field;
}

PdfFormField choiceField(PdfFormFieldType type, bool multi = false) {
    PdfFormField field;
    field.name = "c";
    field.type = type;
    field.multiSelect = multi;
    for (const char* label : {"A", "B", "C"}) field.options.push_back(PdfFormOption{label, label, false});
    field.options[1].selected = true;
    field.widgets.push_back(widget());
    return field;
}

PdfFormField radioField() {
    PdfFormField field;
    field.name = "r";
    field.type = PdfFormFieldType::RadioButton;
    field.widgets.push_back(widget("One", false));
    field.widgets.push_back(widget("Two", true));
    return field;
}

} // namespace

RIVET_TEST(forms_original_values) {
    CHECK(originalValue(textField()) == PdfFormValue::ofText("stored"));
    CHECK(originalValue(radioField()) == PdfFormValue::ofToggle("Two"));
    PdfFormField box = radioField();
    box.type = PdfFormFieldType::CheckBox;
    box.widgets[1].checked = false;
    CHECK(originalValue(box) == PdfFormValue::ofToggle({}));
    CHECK(originalValue(choiceField(PdfFormFieldType::ComboBox)) == PdfFormValue::ofChoice({1}));
}

RIVET_TEST(forms_fillable_rules) {
    PdfFormField field = textField();
    CHECK(field.fillable());
    field.readOnly = true;
    CHECK(!field.fillable());
    field = textField();
    field.overLimit = true;
    CHECK(!field.fillable());
    field = textField();
    field.widgets.clear();
    CHECK(!field.fillable());
    field = textField();
    field.type = PdfFormFieldType::Signature;
    CHECK(!field.fillable());
    field.type = PdfFormFieldType::PushButton;
    CHECK(!field.fillable());

    PdfFormInfo info;
    info.fields.push_back(textField());
    CHECK(info.fillable());
    CHECK_EQ(info.find("t"), std::size_t{0});
    CHECK_EQ(info.find("nope"), PdfFormInfo::npos);
    info.hasXfa = true;
    CHECK(!info.fillable());
    info.hasXfa = false;
    info.limitsExceeded = true;
    CHECK(!info.fillable());
}

RIVET_TEST(forms_validate_text) {
    const PdfFormField single = textField();
    CHECK(validateFormValue(single, PdfFormValue::ofText("hello")).has_value());
    CHECK(!validateFormValue(single, PdfFormValue::ofText("a\nb")).has_value());
    CHECK(!validateFormValue(single, PdfFormValue::ofToggle("x")).has_value());
    CHECK(!validateFormValue(single, PdfFormValue::ofText(std::string("\xFF\xFE"))).has_value());
    CHECK(!validateFormValue(single, PdfFormValue::ofText(std::string(kMaxFormValueBytes + 1, 'a'))).has_value());
    const PdfFormField multi = textField(true);
    CHECK(validateFormValue(multi, PdfFormValue::ofText("a\nb")).has_value());
    const PdfFormField limited = textField(false, 3);
    CHECK(validateFormValue(limited, PdfFormValue::ofText("\xC3\xA9\xC3\xA9\xC3\xA9")).has_value()); // 3 code points
    CHECK(!validateFormValue(limited, PdfFormValue::ofText("abcd")).has_value());
}

RIVET_TEST(forms_validate_toggles_and_choices) {
    const PdfFormField radio = radioField();
    CHECK(validateFormValue(radio, PdfFormValue::ofToggle("One")).has_value());
    CHECK(validateFormValue(radio, PdfFormValue::ofToggle({})).has_value());
    CHECK(!validateFormValue(radio, PdfFormValue::ofToggle("Three")).has_value());
    CHECK(!validateFormValue(radio, PdfFormValue::ofText("One")).has_value());

    const PdfFormField combo = choiceField(PdfFormFieldType::ComboBox);
    CHECK(validateFormValue(combo, PdfFormValue::ofChoice({2})).has_value());
    CHECK(validateFormValue(combo, PdfFormValue::ofChoice({})).has_value());
    CHECK(!validateFormValue(combo, PdfFormValue::ofChoice({3})).has_value());
    CHECK(!validateFormValue(combo, PdfFormValue::ofChoice({0, 1})).has_value());
    PdfFormValue typed = PdfFormValue::ofChoice({});
    typed.text = "custom";
    CHECK(!validateFormValue(combo, typed).has_value()); // not editable
    PdfFormField editable = combo;
    editable.editable = true;
    CHECK(validateFormValue(editable, typed).has_value());

    const PdfFormField list = choiceField(PdfFormFieldType::ListBox, true);
    CHECK(validateFormValue(list, PdfFormValue::ofChoice({0, 2})).has_value());
    CHECK(!validateFormValue(list, PdfFormValue::ofChoice({2, 0})).has_value());
    CHECK(!validateFormValue(list, PdfFormValue::ofChoice({1, 1})).has_value());
}

RIVET_TEST(forms_sanitize_text) {
    CHECK_EQ(sanitizeFormText(textField(), "a\r\nb\nc"), std::string("a b c"));
    CHECK_EQ(sanitizeFormText(textField(true), "a\nb"), std::string("a\nb"));
    // Cut on code points, never inside one.
    CHECK_EQ(sanitizeFormText(textField(false, 2), "\xC3\xA9\xC3\xA9\xC3\xA9"), std::string("\xC3\xA9\xC3\xA9"));
    CHECK_EQ(sanitizeFormText(textField(false, 0), std::string(kMaxFormValueBytes + 10, 'x')).size(),
             kMaxFormValueBytes);
}

// Reset Form defaults (QA #7): /DV when present, else the type's empty value;
// radio groups and combo boxes without /DV keep their stored value (the
// engine cannot turn them off / empty them).
RIVET_TEST(forms_default_values_for_reset) {
    PdfFormField text = textField();
    CHECK(defaultFormValue(text) == PdfFormValue::ofText(""));
    text.defaultValue = "dflt";
    CHECK(defaultFormValue(text) == PdfFormValue::ofText("dflt"));

    PdfFormField box;
    box.name = "x";
    box.type = PdfFormFieldType::CheckBox;
    box.widgets.push_back(widget("Yes", true));
    CHECK(defaultFormValue(box) == PdfFormValue::ofToggle(""));
    box.defaultValue = "Yes";
    CHECK(defaultFormValue(box) == PdfFormValue::ofToggle("Yes"));

    PdfFormField radio = radioField();
    CHECK(!defaultFormValue(radio).has_value());
    radio.defaultValue = "One";
    CHECK(defaultFormValue(radio) == PdfFormValue::ofToggle("One"));

    PdfFormField list = choiceField(PdfFormFieldType::ListBox);
    CHECK(defaultFormValue(list) == PdfFormValue::ofChoice({}));
    list.defaultValue = "C";
    CHECK(defaultFormValue(list) == PdfFormValue::ofChoice({2}));

    PdfFormField combo = choiceField(PdfFormFieldType::ComboBox);
    CHECK(!defaultFormValue(combo).has_value());
    combo.defaultValue = "A";
    CHECK(defaultFormValue(combo) == PdfFormValue::ofChoice({0}));

    PdfFormField readOnly = textField();
    readOnly.readOnly = true;
    CHECK(!defaultFormValue(readOnly).has_value());

    PdfFormInfo info;
    info.fields = {textField(), radioField(), choiceField(PdfFormFieldType::ListBox)};
    info.fields[2].name = "l";
    const PdfFormValues reset = resetFormValues(info);
    CHECK_EQ(reset.size(), std::size_t{2}); // text and list; the radio keeps "Two"
    CHECK(reset.contains("t") && reset.at("t") == PdfFormValue::ofText(""));
    CHECK(reset.contains("l") && reset.at("l") == PdfFormValue::ofChoice({}));
    info.hasXfa = true;
    CHECK(resetFormValues(info).empty());
}
