// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/SelectionModel.hpp"

#include "core/StrongId.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace rivet::editor {

// One page's slice of a text selection: characters [begin, end) of the
// page's PdfTextPage. end == kToPageEnd means "through the last character"
// (the page's length is only known once its text is loaded).
struct TextRange {
    static constexpr std::uint32_t kToPageEnd = std::numeric_limits<std::uint32_t>::max();

    core::PageId page;
    std::uint32_t begin = 0;
    std::uint32_t end = kToPageEnd;

    bool operator==(const TextRange&) const = default;
};

// Normalizes a (possibly backwards, possibly cross-page) selection into
// per-page ranges in READING order. indexOf maps a PageId to its reading
// position (kInvalid for pages no longer in the document) and pageAt maps a
// position back to its PageId. Returns an empty vector when either end is
// unknown or the selection is empty.
std::vector<TextRange> orderedSelectionRanges(
    const TextSelection& selection,
    const std::function<std::size_t(core::PageId)>& indexOf,
    const std::function<core::PageId(std::size_t)>& pageAt,
    std::size_t invalidIndex);

// Appends the UTF-8 text of chars [begin, end) (end clamped to the page).
// Generated line-break characters (empty bounds) become '\n'; U+FFFD
// characters without geometry are dropped (they carry no text).
void appendRangeText(const pdf::PdfTextPage& page, std::uint32_t begin, std::uint32_t end,
                     std::string& out);

// UTF-8 encoding of one code point (U+FFFD for invalid scalar values).
void appendCodePointUtf8(std::string& out, char32_t codePoint);

} // namespace rivet::editor
