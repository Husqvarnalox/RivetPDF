// SPDX-License-Identifier: MPL-2.0

#include "markdown/math/MathDelimiters.hpp"

namespace rivet::markdown::math {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Whether text[i] is preceded by an odd number of backslashes.
bool escaped(std::string_view text, std::size_t i) {
    std::size_t n = 0;
    while (i > n && text[i - n - 1] == '\\') ++n;
    return n % 2 == 1;
}

bool blankLineBetween(std::string_view text, std::size_t from, std::size_t to) {
    std::size_t lineStart = text.find('\n', from);
    while (lineStart != std::string_view::npos && lineStart < to) {
        std::size_t k = lineStart + 1;
        while (k < to && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r')) ++k;
        if (k < to && text[k] == '\n') return true;
        lineStart = text.find('\n', lineStart + 1);
    }
    return false;
}

} // namespace

bool isInlineMathSpan(std::string_view text, std::size_t open, std::size_t close) {
    if (open >= close || close >= text.size()) return false;
    if (text[open] != '$' || text[close] != '$') return false;
    if (close == open + 1) return false; // empty
    if (escaped(text, open) || escaped(text, close)) return false;
    if (isSpace(text[open + 1])) return false;
    if (isSpace(text[close - 1])) return false;
    if (close + 1 < text.size() && isDigit(text[close + 1])) return false;
    return !blankLineBetween(text, open, close);
}

std::vector<MathSpan> findMathSpans(std::string_view text) {
    std::vector<MathSpan> spans;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '$' || escaped(text, i)) {
            ++i;
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '$') {
            // Display math: up to the next unescaped "$$".
            std::size_t j = i + 2;
            bool found = false;
            while (j + 1 < text.size()) {
                if (text[j] == '$' && text[j + 1] == '$' && !escaped(text, j)) {
                    found = true;
                    break;
                }
                ++j;
            }
            if (found && j > i + 2) {
                spans.push_back({i, j + 2, true});
                i = j + 2;
            } else {
                i += 2;
            }
            continue;
        }
        // Inline: the first unescaped '$' after the opener decides.
        std::size_t close = std::string_view::npos;
        for (std::size_t j = i + 1; j < text.size(); ++j) {
            if (text[j] != '$' || escaped(text, j)) continue;
            if (isInlineMathSpan(text, i, j) && !(j + 1 < text.size() && text[j + 1] == '$')) close = j;
            break;
        }
        if (close != std::string_view::npos) {
            spans.push_back({i, close + 1, false});
            i = close + 1;
        } else {
            ++i;
        }
    }
    return spans;
}

} // namespace rivet::markdown::math
