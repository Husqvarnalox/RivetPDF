// SPDX-License-Identifier: MPL-2.0
#pragma once

// Source edits for WYSIWYG editing of rendered Markdown (ADR-0031).
//
// The source text stays authoritative: every gesture in the rendered view is
// turned into ONE replacement of a source byte range (an Edit), which the app
// runs as a TextEditCommand and re-parses. Everything here is pure: it reads
// the source (and, where block/inline structure matters, the document parsed
// from exactly that source) and never touches UI, platform or MD4C.
//
// Guarantees (checked by tests through a parse round trip):
//  - typed text is escaped so it renders literally and never changes the
//    surrounding structure (no accidental headings, lists, emphasis, links);
//  - deleting rendered characters never leaves broken markup: an inline
//    span whose whole content is deleted goes with its delimiters, whitespace
//    is never left just inside a closing/opening emphasis delimiter, and a
//    line that would start with structural markup is escaped;
//  - inline formatting toggles are idempotent: applying the same command
//    twice restores the original source byte-for-byte;
//  - bytes outside the edited range are never touched.
// Offsets are byte offsets on UTF-8 code point boundaries.

#include "markdown/MarkdownModel.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::markdown {
class CaretMap;
}

namespace rivet::markdown::editing {

// Replace [offset, offset+removeLength) by `insert`; afterwards the selection
// is [anchor, caret] (offsets in the edited text).
struct Edit {
    std::size_t offset = 0;
    std::size_t removeLength = 0;
    std::string insert;
    std::size_t anchor = 0;
    std::size_t caret = 0;

    bool changesText() const { return removeLength != 0 || !insert.empty(); }
};

// The source with `edit` applied (tests and compound operations).
std::string applied(std::string_view source, const Edit& edit);
// The smallest single Edit turning `before` into `after`, with the given
// final selection.
Edit diffEdit(std::string_view before, std::string_view after, std::size_t anchor, std::size_t caret);

// ---------------------------------------------------------------- line prefix

// Container/marker prefix of the source line holding `offset`:
//   [lineStart, quoteEnd)        "> > " block quote markers (with one space each)
//   [quoteEnd, markerStart)      indentation before a list marker
//   [markerStart, markerEnd)     "- " / "12. " list marker with its spaces
//   [markerEnd, taskEnd)         "[ ] " task box
//   [taskEnd, headingEnd)        "## " ATX heading marker
//   contentStart                 first byte after the above and spaces
struct LinePrefix {
    enum class Marker : std::uint8_t { None, Bullet, Ordered };
    std::size_t lineStart = 0;
    std::size_t lineEnd = 0; // before the '\n'
    std::size_t quoteEnd = 0;
    std::size_t markerStart = 0;
    std::size_t markerEnd = 0;
    std::size_t taskEnd = 0;
    std::size_t headingEnd = 0;
    std::size_t contentStart = 0;
    Marker marker = Marker::None;
    char bullet = '-';
    char delimiter = '.';
    unsigned long number = 0;
    bool task = false;
    bool taskChecked = false;
    int headingLevel = 0;

    bool blank() const { return contentStart >= lineEnd; }
};
LinePrefix scanLinePrefix(std::string_view source, std::size_t offset);

// ------------------------------------------------------------------- context

// What surrounds a source offset in a parsed document.
struct Context {
    const Block* leaf = nullptr;          // innermost leaf block containing the offset (null in a gap)
    const ListItem* item = nullptr;       // innermost list item containing the offset
    const Block* list = nullptr;          // the list owning `item`
    std::size_t itemIndex = 0;            // index of `item` in `list`
    bool inQuote = false;
    bool editableCode = false;            // inside the content lines of an editable code block
    bool inCodeSpan = false;
    bool island = false;                  // inside a read-only island
    std::vector<const Inline*> spans;     // enclosing inline spans, outermost first
};
Context contextAt(const MarkdownDocument* doc, std::size_t offset);

// Content range of an inline span (between its delimiters). Emphasis /
// Strong / Strike / Link: the children's extent; Code: its text pieces.
SourceRange inlineContent(const Inline& span);

// ------------------------------------------------------------------- typing

// Escapes `typed` (plain text, no line breaks) for insertion at `offset`.
// `lineStart`: the insertion point is the first content byte of its line.
std::string escapeTyped(std::string_view source, std::size_t offset, std::string_view typed, bool lineStart,
                        bool inHeading);
// Whether `offset` is the first content byte of its line (after quote,
// list, task and heading markers).
bool atLineContentStart(std::string_view source, std::size_t offset);

// Typing (or IME commit) of `typed` replacing [selBegin, selEnd). In code
// (block content or a code span) the text goes in verbatim; elsewhere it is
// escaped, and leading/trailing whitespace is moved outside emphasis
// delimiters it would otherwise break. Line breaks are not allowed here
// (use insertParagraphBreak / pastePlainText).
Edit insertText(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd,
                std::string_view typed);

// Deletes the rendered content [begin, end): inline spans whose whole content
// is deleted go with their delimiters, delimiters of partly covered spans are
// kept, whitespace is moved out of emphasis edges and a line that would now
// start with structural markup is escaped. Caret at the deletion point.
Edit deleteRange(std::string_view source, const MarkdownDocument* doc, std::size_t begin, std::size_t end);

// Backspace (forward = false) / Delete at a collapsed caret, using the
// rendered layout of `doc` (`map`): removes one rendered character (a
// grapheme; an escape, entity or inline image as a whole), or at a block
// boundary joins the two blocks by deleting the source between them. A caret
// on a blank line (opened by Enter) deletes that gap. Code blocks never join
// with their neighbours; tables / HTML / special blocks in the way block the
// deletion, an image block in the way is returned for selection instead
// (deleting it is then an ordinary selection delete).
struct DeleteResult {
    Edit edit;                               // changes nothing when blocked / selecting
    bool blocked = false;                    // nothing deletable there
    std::optional<SourceRange> selectIsland; // select this image block instead
};
DeleteResult deleteCharacter(std::string_view source, const MarkdownDocument* doc, const CaretMap& map,
                             std::size_t caret, bool forward);

// Enter: splits the paragraph (blank line), continues a list item (same
// marker, number + 1, unchecked task box) or a quote, exits an empty item /
// empty quote line, splits a heading into heading + paragraph; in a code block
// inserts a newline keeping the line's indentation. At the very start of a
// block a blank line is opened above it (the caret stays on it).
Edit insertParagraphBreak(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin,
                          std::size_t selEnd);
// Shift+Enter: a hard line break ("\" + newline) continuing the container.
Edit insertLineBreak(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd);

// Pastes plain text: every line escaped, line breaks become paragraph /
// list-item / quote breaks like Enter (verbatim in code).
Edit pastePlainText(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd,
                    std::string_view text);

// --------------------------------------------------------------- formatting

enum class InlineFormat : std::uint8_t { Bold, Italic, Strikethrough, Code, Link };
enum class BlockFormat : std::uint8_t {
    Paragraph, // heading level 0
    Heading1,
    Heading2,
    Heading3,
    Heading4,
    Heading5,
    Heading6,
    BulletList,
    NumberedList,
    TaskList,
    Quote,
};

// Wraps or unwraps [selBegin, selEnd) (an empty selection works on the word
// around it). Text-only: works on any source, parsed or not. `url` is the
// link target for Link (unused otherwise). Returns an edit that changes
// nothing when there is nothing to format.
Edit toggleInline(std::string_view source, std::size_t selBegin, std::size_t selEnd, InlineFormat format,
                  std::string_view url = {});
// Applies a block format to every line touched by the selection; lists and
// quote toggle off when every line already has them, a heading level toggles
// back to a paragraph when every line already has it.
Edit applyBlockFormat(std::string_view source, std::size_t selBegin, std::size_t selEnd, BlockFormat format);

} // namespace rivet::markdown::editing
