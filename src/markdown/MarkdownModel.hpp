// SPDX-License-Identifier: MPL-2.0
#pragma once

// Rivet-owned Markdown document model. Produced by an IMarkdownParser, consumed
// by the layout engine and by search. Nothing here depends on the parsing
// library or on any UI/platform code.
//
// The source text is authoritative: the model never regenerates Markdown.
// Every block and inline carries a byte range [start, end) into the source
// the parser was given (after BOM stripping, see MarkdownDocument::sourceOffsetBase).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::markdown {

struct SourceRange {
    std::size_t start = 0;
    std::size_t end = 0;

    constexpr std::size_t size() const { return end - start; }
    constexpr bool empty() const { return end <= start; }
    constexpr bool contains(std::size_t offset) const { return offset >= start && offset < end; }
    constexpr bool operator==(const SourceRange&) const = default;
};

// ---------------------------------------------------------------- inlines

enum class InlineKind : std::uint8_t {
    Text,
    Emphasis,
    Strong,
    Strike,
    Code,
    Link,
    Image,
    SoftBreak,
    HardBreak,
};

struct Inline {
    InlineKind kind = InlineKind::Text;
    // Source range of the whole construct including its delimiters
    // (see MarkdownParser.hpp for accuracy limits).
    SourceRange range;
    // Text: decoded text (entities resolved, escapes removed).
    // Code: the code span contents.
    // Image: the alt text flattened to plain text.
    std::string text;
    // Link / Image: destination and optional title.
    std::string url;
    std::string title;
    // Emphasis / Strong / Strike / Link: nested inlines.
    std::vector<Inline> children;
};

// ----------------------------------------------------------------- blocks

enum class BlockKind : std::uint8_t {
    Paragraph,
    Heading,
    CodeBlock,
    Quote,
    List,
    Table,
    HorizontalRule,
    // A paragraph consisting solely of one image (inlines holds that image).
    ImageBlock,
    // Raw HTML kept as plain text in `text`. Never rendered as HTML.
    HtmlBlock,
};

enum class ColumnAlign : std::uint8_t { Default, Left, Center, Right };

struct Block;

struct ListItem {
    SourceRange range;
    bool isTask = false;
    bool checked = false;
    std::vector<Block> children;
};

struct TableCell {
    SourceRange range;
    std::vector<Inline> inlines;
};

struct TableRow {
    SourceRange range;
    std::vector<TableCell> cells;
};

struct Block {
    // Unique within the document, assigned in document (pre-)order starting at 0.
    std::uint32_t id = 0;
    BlockKind kind = BlockKind::Paragraph;
    SourceRange range;

    // Paragraph, Heading, ImageBlock.
    std::vector<Inline> inlines;

    // Heading.
    int level = 0;
    std::string slug;

    // CodeBlock: info string (e.g. "cpp") and verbatim text. HtmlBlock: raw text.
    std::string info;
    std::string text;
    // CodeBlock fenced with ``` or ~~~ (false: indented).
    bool fenced = false;

    // Quote.
    std::vector<Block> children;

    // List.
    bool ordered = false;
    unsigned start = 1;
    bool tight = true;
    std::vector<ListItem> items;

    // Table. `header` is the single header row; `body` the remaining rows.
    // Rows may be padded to alignments.size() cells by the parser.
    std::vector<ColumnAlign> alignments;
    TableRow header;
    std::vector<TableRow> body;
};

// -------------------------------------------------------------- document

enum class DiagnosticSeverity : std::uint8_t { Info, Warning, Error };

enum class DiagnosticCode : std::uint8_t {
    SourceTruncated,    // source larger than ParseLimits::maxSourceBytes
    NestingTooDeep,     // container nesting beyond ParseLimits::maxNestingDepth was flattened
    InlineTooDeep,      // inline nesting beyond ParseLimits::maxInlineDepth was flattened
    TooManyBlocks,      // parse stopped at ParseLimits::maxBlocks
    TableTooLarge,      // table cells beyond limits were dropped or parse stopped
    CodeBlockTruncated, // code/HTML block text cut at ParseLimits::maxCodeBlockBytes
    InvalidUtf8,        // invalid byte sequences were replaced with U+FFFD in text
    OutOfMemory,        // allocation failure; the document is partial
    InternalError,      // parser reported a failure; the document is partial
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Warning;
    DiagnosticCode code = DiagnosticCode::InternalError;
    std::string message;
    SourceRange range;
};

struct HeadingEntry {
    std::uint32_t blockId = 0;
    int level = 1;
    std::string slug;  // unique within the document (see MarkdownSlug.hpp)
    std::string text;  // plain text of the heading
    SourceRange range; // source range of the heading block
};

struct MarkdownDocument {
    std::vector<Block> blocks;
    // All headings in document order.
    std::vector<HeadingEntry> headings;
    std::vector<Diagnostic> diagnostics;
    // Number of Blocks in the whole tree (ids are 0..blockCount-1).
    std::uint32_t blockCount = 0;
    // Number of bytes stripped from the front of the original buffer (UTF-8 BOM,
    // 0 or 3). All source ranges are relative to the text AFTER the BOM; add
    // this to map into the original buffer.
    std::size_t sourceOffsetBase = 0;
    // Length in bytes of the (BOM-stripped, possibly truncated) source parsed.
    std::size_t sourceLength = 0;

    // Anchor lookup. `anchor` may be given with or without a leading '#'.
    // Returns nullptr when unknown. Pointers stay valid while the document lives.
    const HeadingEntry* findHeadingBySlug(std::string_view anchor) const;
};

} // namespace rivet::markdown
