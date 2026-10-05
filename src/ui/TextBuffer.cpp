// SPDX-License-Identifier: MPL-2.0
#include "ui/TextBuffer.hpp"

#include <algorithm>

namespace rivet::ui {

namespace {

// Length of the valid UTF-8 sequence starting at s[i], or 0 when invalid
// (overlong forms, surrogates, > U+10FFFF, truncated). On 0, `skip` receives
// how many bytes form the maximal bad subpart (>= 1).
std::size_t validSequenceLength(std::string_view s, std::size_t i, std::size_t& skip) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    skip = 1;
    if (b0 < 0x80) return 1;
    std::size_t need = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        if (b0 == 0xE0) lo = 0xA0;
        if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        if (b0 == 0xF0) lo = 0x90;
        if (b0 == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    for (std::size_t k = 1; k <= need; ++k) {
        if (i + k >= s.size()) return 0;
        const auto b = static_cast<unsigned char>(s[i + k]);
        const unsigned char l = k == 1 ? lo : 0x80;
        const unsigned char h = k == 1 ? hi : 0xBF;
        if (b < l || b > h) return 0;
        skip = k + 1;
    }
    return need + 1;
}

} // namespace

bool TextBuffer::needsNormalization(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\r') return true;
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t skip = 1;
        const std::size_t length = validSequenceLength(text, i, skip);
        if (length == 0) return true;
        i += length;
    }
    return false;
}

std::string TextBuffer::normalize(std::string_view text) {
    if (!needsNormalization(text)) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back('\n');
            i += (i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x80) {
            out.push_back(c);
            ++i;
            continue;
        }
        std::size_t skip = 1;
        const std::size_t length = validSequenceLength(text, i, skip);
        if (length == 0) {
            out.append("\xEF\xBF\xBD"); // U+FFFD
            i += skip;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

TextBuffer::TextBuffer(std::string_view text) { assign(text); }

void TextBuffer::assign(std::string_view text) {
    text_ = normalize(text);
    rebuildIndex();
    ++revision_;
}

void TextBuffer::rebuildIndex() {
    lineStarts_.assign(1, 0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') lineStarts_.push_back(i + 1);
    }
}

bool TextBuffer::isBoundary(std::size_t offset) const {
    if (offset > text_.size()) return false;
    return offset == text_.size() || (static_cast<unsigned char>(text_[offset]) & 0xC0) != 0x80;
}

bool TextBuffer::replace(std::size_t offset, std::size_t removeLength, std::string_view insert,
                         std::size_t* insertedLength) {
    if (offset > text_.size() || removeLength > text_.size() - offset) return false;
    if (!isBoundary(offset) || !isBoundary(offset + removeLength)) return false;
    const std::string stored = normalize(insert);
    if (insertedLength != nullptr) *insertedLength = stored.size();
    if (removeLength == 0 && stored.empty()) return true;

    // Line index: entries with start in (offset, offset+removeLength] vanish,
    // the inserted '\n's add new ones, later entries shift by the delta.
    const auto first = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    const auto last = std::upper_bound(first, lineStarts_.end(), offset + removeLength);
    std::vector<std::size_t> added;
    for (std::size_t i = 0; i < stored.size(); ++i) {
        if (stored[i] == '\n') added.push_back(offset + i + 1);
    }
    const std::size_t firstIndex = static_cast<std::size_t>(first - lineStarts_.begin());
    const std::size_t removedEntries = static_cast<std::size_t>(last - first);
    const std::ptrdiff_t delta =
        static_cast<std::ptrdiff_t>(stored.size()) - static_cast<std::ptrdiff_t>(removeLength);

    if (removedEntries != added.size()) {
        lineStarts_.erase(lineStarts_.begin() + static_cast<std::ptrdiff_t>(firstIndex),
                          lineStarts_.begin() + static_cast<std::ptrdiff_t>(firstIndex + removedEntries));
        lineStarts_.insert(lineStarts_.begin() + static_cast<std::ptrdiff_t>(firstIndex), added.begin(),
                           added.end());
    } else {
        std::copy(added.begin(), added.end(), lineStarts_.begin() + static_cast<std::ptrdiff_t>(firstIndex));
    }
    if (delta != 0) {
        for (std::size_t i = firstIndex + added.size(); i < lineStarts_.size(); ++i) {
            lineStarts_[i] = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(lineStarts_[i]) + delta);
        }
    }
    text_.replace(offset, removeLength, stored);
    ++revision_;
    return true;
}

std::string TextBuffer::slice(std::size_t offset, std::size_t length) const {
    return std::string(view(offset, length));
}

std::string_view TextBuffer::view(std::size_t offset, std::size_t length) const {
    if (offset >= text_.size()) return {};
    return std::string_view(text_).substr(offset, length);
}

std::size_t TextBuffer::lineStart(std::size_t line) const {
    return lineStarts_[std::min(line, lineStarts_.size() - 1)];
}

std::size_t TextBuffer::lineEnd(std::size_t line) const {
    if (line + 1 >= lineStarts_.size()) return text_.size();
    return lineStarts_[line + 1] - 1;
}

std::size_t TextBuffer::lineOfOffset(std::size_t offset) const {
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), std::min(offset, text_.size()));
    return static_cast<std::size_t>(it - lineStarts_.begin()) - 1;
}

std::size_t TextBuffer::longestLineBytes() const {
    longestLine();
    return longestBytes_;
}

std::size_t TextBuffer::longestLine() const {
    if (longestRevision_ != revision_) {
        longestRevision_ = revision_;
        longestLine_ = 0;
        longestBytes_ = 0;
        for (std::size_t i = 0; i < lineStarts_.size(); ++i) {
            const std::size_t bytes = lineEnd(i) - lineStarts_[i];
            if (bytes > longestBytes_) {
                longestBytes_ = bytes;
                longestLine_ = i;
            }
        }
    }
    return longestLine_;
}

TextBuffer::Position TextBuffer::positionOfOffset(std::size_t offset) const {
    const std::size_t clamped = std::min(offset, text_.size());
    const std::size_t line = lineOfOffset(clamped);
    return Position{line, clamped - lineStarts_[line]};
}

std::size_t TextBuffer::offsetOfPosition(Position position) const {
    const std::size_t line = std::min(position.line, lineStarts_.size() - 1);
    std::size_t offset = std::min(lineStarts_[line] + position.column, lineEnd(line));
    while (offset > 0 && offset < text_.size() && (static_cast<unsigned char>(text_[offset]) & 0xC0) == 0x80) {
        --offset;
    }
    return offset;
}

} // namespace rivet::ui
