// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Utf8.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::ui {

// Shortens `text` to at most `maxWidth` (as reported by `measure`) by replacing
// the middle with an ellipsis ("\xE2\x80\xA6"), keeping as much of the head and
// tail as fits (the head gets the extra code point on odd splits). Text that
// already fits is returned unchanged; when even the bare ellipsis does not fit
// the ellipsis alone is returned. Cuts only on UTF-8 code point boundaries and
// is pure: `measure` must be monotonic in the string length (true for any
// real text measurer). Binary search keeps the number of measurements
// logarithmic.
template <typename Measure>
std::string elideMiddle(std::string_view text, double maxWidth, Measure&& measure) {
    if (measure(text) <= maxWidth) return std::string(text);
    constexpr std::string_view ellipsis = "\xE2\x80\xA6";

    // Code point start offsets (+ the end), so cuts never split a sequence.
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!utf8::isContinuationByte(text[i]) || starts.empty()) starts.push_back(i);
    }
    starts.push_back(text.size());
    const std::size_t count = starts.size() - 1;

    const auto build = [&](std::size_t keep) {
        const std::size_t head = (keep + 1) / 2;
        const std::size_t tail = keep - head;
        std::string out(text.substr(0, starts[head]));
        out += ellipsis;
        out += text.substr(starts[count - tail]);
        return out;
    };

    std::size_t low = 0;           // always fits (just the ellipsis) or is the fallback
    std::size_t high = count;      // keep < count: something is elided
    while (low + 1 < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (measure(build(mid)) <= maxWidth) low = mid;
        else high = mid;
    }
    return build(low);
}

} // namespace rivet::ui
