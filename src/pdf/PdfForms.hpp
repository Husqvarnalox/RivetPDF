// SPDX-License-Identifier: MPL-2.0
#pragma once

// Engine-independent AcroForm data (ADR-0024).
//
// Everything here is plain Rivet-owned data: no engine handles, freely
// copyable and shareable across threads once built. Widget rects are in the
// PDF USER SPACE of the page the widget belongs to (like annotations, see
// PdfAnnotation.hpp); the editor maps them to display space.
//
// Field names, values and option labels are UTF-8 and UNTRUSTED (they come
// from arbitrary files or from the user): never log them, never interpret
// them. JavaScript and XFA are never run (ADR-0007): format, keystroke,
// validate and calculate actions are only DETECTED (hasScriptActions).

#include "core/Error.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::pdf {

// Limits (ADR-0024). Past the field/widget limits the whole form is
// read-only (PdfFormInfo::limitsExceeded); a value or option list past its
// limit makes that one field read-only.
inline constexpr std::size_t kMaxFormFields = 5000;
inline constexpr std::size_t kMaxFormWidgets = 20000;
inline constexpr std::size_t kMaxFormValueBytes = 64 * 1024;
inline constexpr std::size_t kMaxFormOptions = 1000;
// Field names are bounded too (a name is a map key and is compared often).
inline constexpr std::size_t kMaxFormFieldNameBytes = 4096;

enum class PdfFormFieldType : std::uint8_t {
    Unknown,
    PushButton,
    CheckBox,
    RadioButton,
    ComboBox,
    ListBox,
    Text,
    Signature, // listed, never fillable (ADR-0025)
};

// One widget annotation of a field.
struct PdfFormWidget {
    std::size_t pageIndex = 0;     // source page in the document
    std::uint32_t annotIndex = 0;  // index in that page's /Annots
    PdfBox rect;                   // user space of that page
    // Check box / radio button: the export value of this widget's "on"
    // appearance state (what /V holds while it is checked). Empty otherwise.
    std::string onState;
    bool checked = false; // check box / radio button: currently on
};

struct PdfFormOption {
    std::string label;
    // The option's export value. The engine API exposes only the display
    // label, so this equals `label` (documented limitation, ADR-0024).
    std::string exportValue;
    bool selected = false;
};

struct PdfFormField {
    std::string name; // fully qualified ("parent.child")
    PdfFormFieldType type = PdfFormFieldType::Unknown;
    std::uint32_t flags = 0; // raw /Ff
    bool readOnly = false;
    bool required = false;
    bool multiline = false;
    bool password = false;
    bool comb = false;
    bool doNotScroll = false;
    bool multiSelect = false; // list box
    bool editable = false;    // combo box with an edit field
    std::uint32_t maxLength = 0; // text: /MaxLen (0 = unlimited)
    float fontSize = 0.0F;       // default appearance size (0 = auto)
    std::string value;           // current /V as text ("Off" / "" = unchecked)
    // /DV (the value Reset Form restores) as text when the field or an
    // ancestor has a string or name /DV; nullopt when absent.
    std::optional<std::string> defaultValue;
    std::vector<PdfFormOption> options;
    std::vector<PdfFormWidget> widgets; // in page, then /Annots order
    // Format/keystroke/validate/calculate JavaScript is attached (it does
    // not run in Rivet; values are written as typed).
    bool hasScriptActions = false;
    // The field's value or options exceeded a limit: shown, not fillable.
    bool overLimit = false;

    bool isChoice() const { return type == PdfFormFieldType::ComboBox || type == PdfFormFieldType::ListBox; }
    bool isToggle() const { return type == PdfFormFieldType::CheckBox || type == PdfFormFieldType::RadioButton; }
    // Whether the field's type can be filled at all (text, toggles, choices)
    // and it is neither read-only nor over a limit.
    bool fillable() const;
};

struct PdfFormInfo {
    std::vector<PdfFormField> fields; // in order of the first widget (page, /Annots)
    // The document carries an XFA form: everything is read-only (XFA is
    // never run, ADR-0007).
    bool hasXfa = false;
    // More fields/widgets than the limits: everything is read-only.
    bool limitsExceeded = false;
    // At least one field has script actions (see PdfFormField).
    bool hasScriptActions = false;

    bool empty() const { return fields.empty(); }
    // Fields can be filled at all (no XFA, within limits).
    bool fillable() const { return !hasXfa && !limitsExceeded; }
    // Index of the field named `name`; npos when unknown. O(n).
    std::size_t find(const std::string& name) const;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

using PdfFormInfoPtr = std::shared_ptr<const PdfFormInfo>;

// An edited field value. Text: `text`. Check box / radio button: `text` is
// the export value of the "on" state to select ("" = off). Choice: the
// selected option indices (ascending, unique), plus `text` for the edit
// field of an editable combo box when no option is selected.
struct PdfFormValue {
    enum class Kind : std::uint8_t { Text, Toggle, Choice };
    Kind kind = Kind::Text;
    std::string text;
    std::vector<std::uint32_t> selected;

    bool operator==(const PdfFormValue&) const = default;

    static PdfFormValue ofText(std::string text) { return PdfFormValue{Kind::Text, std::move(text), {}}; }
    static PdfFormValue ofToggle(std::string onState) {
        return PdfFormValue{Kind::Toggle, std::move(onState), {}};
    }
    static PdfFormValue ofChoice(std::vector<std::uint32_t> selected) {
        return PdfFormValue{Kind::Choice, {}, std::move(selected)};
    }
};

// Edited values keyed by fully qualified field name. Ordered (deterministic
// application order).
using PdfFormValues = std::map<std::string, PdfFormValue>;
using PdfFormValuesPtr = std::shared_ptr<const PdfFormValues>;

// The value a field holds as stored in its file (the baseline edits are
// compared against).
PdfFormValue originalValue(const PdfFormField& field);

// The value Reset Form gives `field` (PDF 32000-1, 12.7.5.3): its /DV, else
// the empty value of its type - "" for text, off for a check box, no
// selection for a list box. nullopt when the field is not fillable or its
// empty value cannot be written by the engine (a radio group or a combo box
// without /DV: PDFium's form filler cannot turn a radio group off or empty
// a combo box), in which case Reset Form keeps the stored value.
std::optional<PdfFormValue> defaultFormValue(const PdfFormField& field);

// The edits Reset Form produces for `info`: every fillable field whose
// default differs from its stored value (fields whose default equals the
// stored value, or that have none, are absent - their stored value shows).
PdfFormValues resetFormValues(const PdfFormInfo& info);

// Whether `value` can be written to `field`: the field is fillable, the
// value kind matches the type, text is valid UTF-8 within kMaxFormValueBytes
// and /MaxLen code points (single-line fields contain no line breaks), a
// toggle names one of the field's widget on-states (or "" for off), choice
// indices are in range, ascending, unique and at most one unless the list
// allows multiple selection. InvalidArgument with a reason (never the value).
core::Status validateFormValue(const PdfFormField& field, const PdfFormValue& value);

// Cuts `text` to at most `maxLength` code points (0 = unlimited) and
// `kMaxFormValueBytes`, on code-point boundaries; single-line fields lose
// their line breaks (each CR/LF run becomes one space).
std::string sanitizeFormText(const PdfFormField& field, std::string text);

} // namespace rivet::pdf
