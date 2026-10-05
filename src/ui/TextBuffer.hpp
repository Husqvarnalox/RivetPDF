// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::ui {

// UTF-8 text store with an incremental line index, used by the source editor.
//
// Invariants (always, whatever is fed in):
//   - the content is valid UTF-8 (invalid sequences are replaced by U+FFFD on
//     the way in, never stored);
//   - line breaks are '\n' only (CRLF and bare CR are normalized on the way in);
//   - offsets are byte offsets on code-point boundaries.
//
// Storage choice: a plain std::string plus a vector of line-start offsets.
// A benchmark (insert one byte in the middle, shifting the index) measured
// ~14 us/edit at 1 MB, ~50 us at 5 MB and ~0.6 ms at 64 MB (the largest file
// the app opens), so a piece table / gap buffer would only add complexity.
// Edits are O(N) memmoves, not O(N) parsing, and the index shift is a tight
// add loop.
//
// Lines: a text with k '\n' has k+1 lines (the last one may be empty); an
// empty text has one empty line. lineEnd() excludes the '\n'.
//
// Main thread only. Contents are untrusted: nothing here logs them.
class TextBuffer {
public:
    TextBuffer() = default;
    // Normalizes `text` (see normalize()).
    explicit TextBuffer(std::string_view text);

    // CRLF and bare CR -> LF, invalid UTF-8 -> U+FFFD (one per maximal bad
    // subpart). Returns the input unchanged (fast path) when already clean.
    static std::string normalize(std::string_view text);
    static bool needsNormalization(std::string_view text);

    const std::string& text() const { return text_; }
    std::size_t size() const { return text_.size(); }
    bool empty() const { return text_.empty(); }
    // Bumped by every successful mutation.
    std::uint64_t revision() const { return revision_; }

    // Replaces the whole content (normalized). O(N).
    void assign(std::string_view text);

    // True when `offset` is within [0, size()] and on a code-point boundary.
    bool isBoundary(std::size_t offset) const;

    // Replaces [offset, offset+removeLength) by `insert` (normalized here).
    // Returns false and changes nothing when the range is out of bounds or
    // either end is not on a boundary. `insertedLength` (optional) receives
    // the byte length actually stored (differs from insert.size() when
    // normalization changed it).
    bool replace(std::size_t offset, std::size_t removeLength, std::string_view insert,
                 std::size_t* insertedLength = nullptr);
    bool insert(std::size_t offset, std::string_view text, std::size_t* insertedLength = nullptr) {
        return replace(offset, 0, text, insertedLength);
    }
    bool erase(std::size_t offset, std::size_t length) { return replace(offset, length, {}); }

    // Copy of [offset, offset+length) clamped to the text.
    std::string slice(std::size_t offset, std::size_t length) const;
    std::string_view view(std::size_t offset, std::size_t length) const;
    // Whole-content copy (parse snapshots): one memcpy.
    std::string snapshot() const { return text_; }

    // --- Lines ---------------------------------------------------------------
    std::size_t lineCount() const { return lineStarts_.size(); }
    std::size_t lineStart(std::size_t line) const;
    // Offset of the line's '\n' (or size() for the last line).
    std::size_t lineEnd(std::size_t line) const;
    // Line containing `offset` (clamped to the text). An offset just after a
    // '\n' belongs to the next line. O(log lines).
    std::size_t lineOfOffset(std::size_t offset) const;
    // Byte length of the longest line, O(lines) (cached per revision).
    std::size_t longestLineBytes() const;
    std::size_t longestLine() const; // index of that line

    struct Position {
        std::size_t line = 0;
        std::size_t column = 0; // bytes from the line start
        bool operator==(const Position&) const = default;
    };
    Position positionOfOffset(std::size_t offset) const;
    // Offset of (line, column bytes), clamped to the line and floored to a
    // code-point boundary.
    std::size_t offsetOfPosition(Position position) const;

    // Hard cap on line-index work for pathological inputs is not needed: the
    // index is one 8-byte entry per '\n'.

private:
    void rebuildIndex();

    std::string text_;
    std::vector<std::size_t> lineStarts_{0};
    std::uint64_t revision_ = 0;
    mutable std::uint64_t longestRevision_ = static_cast<std::uint64_t>(-1);
    mutable std::size_t longestLine_ = 0;
    mutable std::size_t longestBytes_ = 0;
};

} // namespace rivet::ui
