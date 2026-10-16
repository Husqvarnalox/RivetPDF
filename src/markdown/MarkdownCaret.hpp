// SPDX-License-Identifier: MPL-2.0
#pragma once

// Caret geometry for WYSIWYG editing of the rendered Markdown (ADR-0031).
//
// The editor's caret and selection live in SOURCE byte offsets; this map
// relates them to positions in a MarkdownLayout (run + byte offset) using the
// per-run provenance spans (LayoutRun::spanBegin). Pure, built from one
// layout and valid while that layout lives.
//
// Caret stops: every extended-grapheme boundary of every EDITABLE run (runs
// of blocks that are not read-only islands, with known provenance), except
//  - positions strictly inside an atomic span (an escape, an entity, an
//    inline image, a collapsed soft break): the piece is one unit, and
//  - the start of a run that directly follows another editable run on the
//    same line: it is the same visual point as the previous run's end, which
//    is the stop kept (typing there continues the previous run's style).
// Stops are ordered like the layout (document order); their source offsets
// are non-decreasing.

#include "markdown/MarkdownLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::markdown {

class CaretMap {
public:
    CaretMap() = default;
    explicit CaretMap(const MarkdownLayout& layout);

    const MarkdownLayout* layout() const { return layout_; }
    // No editable text at all (empty document, only islands).
    bool empty() const { return runs_.empty(); }

    // ---- conversions
    bool isStop(TextPosition pos) const;
    // Source offset of a stop (any run position is accepted; see the class comment).
    std::size_t sourceOf(TextPosition pos) const;
    // The stop whose source offset is nearest to `offset` (ties: the earlier
    // stop). Offsets inside hidden markup ("**", a link target, a fence) snap
    // to the closest rendered position. nullopt when empty().
    std::optional<TextPosition> positionOf(std::size_t offset) const;
    // Nearest stop to an arbitrary run position (a hit-test result).
    std::optional<TextPosition> snap(TextPosition pos) const;
    // Layout block index of a stop's run.
    std::size_t blockOf(TextPosition pos) const;

    // ---- movement (all return stops)
    std::optional<TextPosition> first() const;
    std::optional<TextPosition> last() const;
    std::optional<TextPosition> next(TextPosition pos) const;     // one character right
    std::optional<TextPosition> previous(TextPosition pos) const; // one character left
    TextPosition lineStart(TextPosition pos) const;               // visual line
    TextPosition lineEnd(TextPosition pos) const;
    std::optional<TextPosition> wordLeft(TextPosition pos) const;
    std::optional<TextPosition> wordRight(TextPosition pos) const;
    // The word (letters/digits/underscore/non-ASCII) around `pos`, or the
    // single character after it when that is not a word character.
    std::pair<TextPosition, TextPosition> wordAt(TextPosition pos) const;
    // The stop `deltaLines` visual lines away (skipping lines without editable
    // text) closest to document x `x`; clamps at the first/last line.
    TextPosition vertical(TextPosition pos, double x, long deltaLines, const ITextMeasurer& measurer) const;
    // Closest stop on the visual line of `pos` to document x `x`.
    TextPosition onLineAtX(std::uint32_t line, double x, const ITextMeasurer& measurer) const;
    // Line (index into layout.lines) of a stop.
    std::uint32_t lineOf(TextPosition pos) const;

    // ---- characters
    // Source bytes of the character (grapheme) just before `pos` inside the
    // same layout block: the character's own bytes when it is in the same
    // run; the separator between the previous stop and `pos` (soft-wrap
    // space, code-line break, hard break) when `pos` starts a line. nullopt
    // at the first stop of a block (a block boundary) or of the document.
    std::optional<SourceRange> charBefore(TextPosition pos) const;
    // Plain text of the character after `pos` (empty at the end of a line).
    std::string_view charTextAfter(TextPosition pos) const;
    std::string_view charTextBefore(TextPosition pos) const;

private:
    struct LineInfo {
        std::uint32_t line = 0;
        std::uint32_t firstSlot = 0; // [firstSlot, lastSlot] into runs_
        std::uint32_t lastSlot = 0;
    };
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    std::uint32_t slotOf(std::uint32_t run) const { return run < slot_.size() ? slot_[run] : kNoSlot; }
    bool joinsPrevious(std::uint32_t slot) const; // the run's offset 0 is not a stop
    bool validOffset(std::uint32_t run, std::size_t offset) const; // boundary, not inside an atomic span
    std::size_t nextBoundary(std::uint32_t run, std::size_t offset) const;
    std::size_t previousBoundary(std::uint32_t run, std::size_t offset) const;
    std::size_t charSourceStart(std::uint32_t run, std::size_t offset) const;
    std::size_t charSourceEnd(std::uint32_t run, std::size_t offset) const;
    std::size_t lineInfoIndex(std::uint32_t line) const;
    std::optional<TextPosition> nearestInRun(std::uint32_t slot, std::size_t offset, std::size_t* distance) const;

    const MarkdownLayout* layout_ = nullptr;
    std::vector<std::uint32_t> runs_;       // editable run indices, layout order
    std::vector<std::uint32_t> runBlock_;   // per slot: layout block index
    std::vector<std::size_t> runSource_;    // per slot: running max of the first source offset (for search)
    std::vector<std::uint32_t> slot_;       // run index -> slot (kNoSlot when not editable)
    std::vector<LineInfo> lines_;           // visual lines with editable runs, layout order
};

} // namespace rivet::markdown
