// SPDX-License-Identifier: MPL-2.0
#include "app/FormLayout.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::app {
namespace {

bool isContinuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

// Code points of `text` as UTF-8 substrings (stray bytes stand alone).
std::vector<std::string_view> codePoints(std::string_view text) {
    std::vector<std::string_view> points;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = i + 1;
        while (end < text.size() && isContinuation(static_cast<unsigned char>(text[end]))) ++end;
        points.push_back(text.substr(i, end - i));
        i = end;
    }
    return points;
}

// Greedy wrap of one paragraph into lines no wider than `width`.
void wrapParagraph(std::string_view paragraph, double width, double size, const FormTextMeasure& measure,
                   std::vector<std::string>& out) {
    if (paragraph.empty()) {
        out.emplace_back();
        return;
    }
    std::string line;
    std::size_t i = 0;
    while (i < paragraph.size()) {
        // Next word including its trailing spaces.
        std::size_t end = paragraph.find(' ', i);
        end = end == std::string_view::npos ? paragraph.size() : end;
        while (end < paragraph.size() && paragraph[end] == ' ') ++end;
        const std::string_view word = paragraph.substr(i, end - i);
        std::string candidate = line;
        candidate.append(word);
        if (measure(candidate, size) <= width || line.empty()) {
            if (line.empty() && measure(word, size) > width) {
                // A word wider than the line: break at code points.
                for (const std::string_view point : codePoints(word)) {
                    std::string next = line;
                    next.append(point);
                    if (!line.empty() && measure(next, size) > width) {
                        out.push_back(std::move(line));
                        line.assign(point);
                    } else {
                        line = std::move(next);
                    }
                }
            } else {
                line = std::move(candidate);
            }
            i = end;
            continue;
        }
        out.push_back(std::move(line));
        line.clear();
    }
    out.push_back(std::move(line));
}

} // namespace

std::string maskFormText(std::string_view text) {
    std::string masked;
    for (std::size_t i = 0; i < codePoints(text).size(); ++i) masked += "\xE2\x80\xA2";
    return masked;
}

double formFontSize(const pdf::PdfFormField& field, const core::Rect& rect, std::string_view text,
                    const FormTextMeasure& measure) {
    const double height = std::max(0.0, rect.size.height - 2.0 * kFormTextPadding);
    if (field.fontSize > 0.0F) return static_cast<double>(field.fontSize);
    if (field.multiline) return std::clamp(kFormDefaultMultilineFontSize, kFormMinFontSize, std::max(kFormMinFontSize, height));
    double size = std::clamp(height * 0.75, kFormMinFontSize, kFormMaxAutoFontSize);
    const double width = std::max(0.0, rect.size.width - 2.0 * kFormTextPadding);
    if (!field.comb && !text.empty() && measure) {
        const double measured = measure(text, size);
        if (measured > width && measured > 0.0) size = std::max(kFormMinFontSize, size * width / measured);
    }
    return size;
}

FormTextLayout layoutFormText(const pdf::PdfFormField& field, const core::Rect& rect, std::string_view text,
                              const FormTextMeasure& measure) {
    FormTextLayout layout;
    layout.fontSize = formFontSize(field, rect, text, measure);
    const double size = layout.fontSize;
    if (field.comb && field.maxLength > 0 && !field.multiline) {
        const double cell = rect.size.width / static_cast<double>(field.maxLength);
        const std::vector<std::string_view> points = codePoints(text);
        for (std::size_t i = 0; i < points.size() && i < field.maxLength; ++i) {
            layout.lines.push_back(FormTextLine{
                std::string(points[i]),
                core::Rect{rect.origin.x + cell * static_cast<double>(i), rect.origin.y, cell, rect.size.height}, true});
        }
        return layout;
    }
    const double innerX = rect.origin.x + kFormTextPadding;
    const double innerWidth = std::max(0.0, rect.size.width - 2.0 * kFormTextPadding);
    if (!field.multiline) {
        layout.lines.push_back(FormTextLine{std::string(text),
                                            core::Rect{innerX, rect.origin.y, innerWidth, rect.size.height}, false});
        return layout;
    }
    std::vector<std::string> wrapped;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        if (measure) {
            wrapParagraph(text.substr(start, end - start), innerWidth, size, measure, wrapped);
        } else {
            wrapped.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    const double lineHeight = size * kFormLineFactor;
    double y = rect.origin.y + kFormTextPadding;
    for (std::string& line : wrapped) {
        if (y + lineHeight > rect.maxY() + 0.01) break; // no room: dropped (the editor shows all)
        layout.lines.push_back(FormTextLine{std::move(line), core::Rect{innerX, y, innerWidth, lineHeight}, false});
        y += lineHeight;
    }
    return layout;
}

std::string formChoiceText(const pdf::PdfFormField& field, const pdf::PdfFormValue& value) {
    if (value.selected.empty()) return value.text;
    std::string text;
    for (const std::uint32_t index : value.selected) {
        if (index >= field.options.size()) continue;
        if (!text.empty()) text += ", ";
        text += field.options[index].label;
    }
    return text;
}

void sortFormStops(std::vector<FormStop>& stops) {
    std::sort(stops.begin(), stops.end(), [](const FormStop& a, const FormStop& b) {
        if (a.page != b.page) return a.page < b.page;
        if (a.annotIndex != b.annotIndex) return a.annotIndex < b.annotIndex;
        return a.widget < b.widget;
    });
}

std::optional<std::size_t> nextFormStop(const std::vector<FormStop>& order, std::optional<std::size_t> current,
                                        bool backwards) {
    if (order.empty()) return std::nullopt;
    const std::size_t n = order.size();
    if (!current.has_value() || *current >= n) return backwards ? n - 1 : 0;
    return backwards ? (*current + n - 1) % n : (*current + 1) % n;
}

} // namespace rivet::app
