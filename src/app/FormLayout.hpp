// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pure layout helpers of the form overlay and traversal (ADR-0024): how an
// edited field's value is laid out in its widget rect (font size, comb
// cells, multiline wrapping, password masking, choice text) and the Tab
// order over widgets. No painting, no document access: everything works on
// page display space rects and a text-measuring callback, so it is unit
// tested without a platform.

#include "core/geometry/Rect.hpp"
#include "pdf/PdfForms.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::app {

// Width of `text` set at `fontSize` points (any consistent unit).
using FormTextMeasure = std::function<double(std::string_view text, double fontSize)>;

// Inner padding of a text widget (page points).
inline constexpr double kFormTextPadding = 2.0;
inline constexpr double kFormMinFontSize = 4.0;
inline constexpr double kFormMaxAutoFontSize = 24.0;
inline constexpr double kFormDefaultMultilineFontSize = 12.0;
inline constexpr double kFormLineFactor = 1.15;

struct FormTextLine {
    std::string text;
    core::Rect rect; // page display space; the text is vertically centered
    bool centered = false; // comb cells: center in the rect, else left-align
};

struct FormTextLayout {
    double fontSize = 0.0;
    std::vector<FormTextLine> lines;
};

// One bullet (U+2022) per code point.
std::string maskFormText(std::string_view text);

// The font size the overlay uses: the field's own (/DA) size, else "auto":
// single-line text fits the rect height (bounded) and shrinks to fit its
// width; multiline auto uses 12 pt (bounded by the rect height).
double formFontSize(const pdf::PdfFormField& field, const core::Rect& rect, std::string_view text,
                    const FormTextMeasure& measure);

// Lays out `text` (already masked for password fields) in `rect`: comb
// fields get one centered code point per cell (maxLength cells), multiline
// fields wrap at spaces (long words at code points) from the top, other
// fields are one left-aligned line. Lines below the rect are dropped.
FormTextLayout layoutFormText(const pdf::PdfFormField& field, const core::Rect& rect, std::string_view text,
                              const FormTextMeasure& measure);

// What a choice field shows: the selected option labels joined by ", " (or
// an editable combo box's typed text).
std::string formChoiceText(const pdf::PdfFormField& field, const pdf::PdfFormValue& value);

// One focusable widget for Tab traversal.
struct FormStop {
    std::size_t page = 0;          // layout (model) index
    std::uint32_t annotIndex = 0;  // /Annots index on that page
    std::size_t field = 0;
    std::size_t widget = 0;
    bool operator==(const FormStop&) const = default;
};

// Page order, then widget (/Annots) order.
void sortFormStops(std::vector<FormStop>& stops);

// The stop after (Tab) or before (Shift+Tab) `current` in `order`, wrapping
// around; the first (last) stop without a current one. nullopt when empty.
std::optional<std::size_t> nextFormStop(const std::vector<FormStop>& order, std::optional<std::size_t> current,
                                        bool backwards);

} // namespace rivet::app
