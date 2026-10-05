// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pure Markdown layout: turns a MarkdownDocument plus a viewport width into
// positioned lines/runs and decorations. No painting, no platform or font
// code: text metrics and image sizes come from the abstract interfaces below,
// so the whole engine is unit-testable with a fixed-width fake measurer.
//
// Coordinates: document space, y grows downward, origin top-left. Units are
// whatever the measurer uses (typically logical points).
//
// Complexity: layoutMarkdown() is O(N) in document size (every token is
// measured at most once per distinct (style, word) thanks to MeasureCache, and
// each token is placed exactly once). Relayout for a new width is another O(N)
// pass; keeping a MeasureCache across passes makes it measurement-free.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "markdown/MarkdownModel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rivet::markdown {

// ------------------------------------------------------------ abstractions

enum class TextKind : std::uint8_t { Body, H1, H2, H3, H4, H5, H6, Code, Quote, Link, Table };

struct TextStyle {
    TextKind kind = TextKind::Body;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool monospace = false;
    double size = 15.0;

    bool operator==(const TextStyle&) const = default;
};

struct TextMetrics {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

// Measures UTF-8 text laid out on one line in the given style. Must be pure
// and deterministic for a given (text, style).
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;
    virtual TextMetrics measure(std::string_view utf8, const TextStyle& style) const = 0;
};

enum class ImageState : std::uint8_t {
    Unknown, // not loaded yet: a default placeholder is laid out
    Known,   // intrinsic size available
    Blocked, // not allowed (remote, unsafe path, failed): a small blocked placeholder
};

struct ImageInfo {
    ImageState state = ImageState::Unknown;
    double width = 0.0;
    double height = 0.0;
};

class IImageSizeProvider {
public:
    virtual ~IImageSizeProvider() = default;
    virtual ImageInfo imageInfo(std::string_view url) const = 0;
};

// All default typography and spacing constants live here.
struct Typography {
    double bodySize = 15.0;
    double codeSize = 13.0;
    double tableSize = 14.0;
    std::array<double, 6> headingSize{28.0, 24.0, 20.0, 17.0, 15.0, 14.0};
    double lineHeightFactor = 1.45;

    double pageMarginX = 24.0;
    double pageMarginY = 16.0;
    double minContentWidth = 8.0; // available width is never narrower than this

    double paragraphGap = 12.0;
    double tightGap = 4.0;          // between children of a tight list item / between tight items
    double headingGapBeforeFactor = 0.9; // x heading size, when not first in its container
    double headingGapAfter = 6.0;
    double ruleGap = 14.0;
    double ruleHeight = 1.0;

    double listIndent = 28.0;       // minimum marker column width
    double listMarkerGap = 8.0;     // space between marker and text
    double bulletSize = 5.0;
    double checkboxSize = 14.0;
    double quoteIndent = 18.0;
    double quoteBarWidth = 3.0;
    double quoteBarInset = 2.0;

    double codePadding = 10.0;
    int tabSize = 4;

    double tableCellPaddingX = 10.0;
    double tableCellPaddingY = 5.0;
    double tableMaxColumnWidth = 360.0; // widest unbreakable word honoured before it is broken

    double maxImageHeight = 480.0;
    double imagePlaceholderWidth = 320.0;
    double imagePlaceholderHeight = 180.0;
    double imageBlockedWidth = 240.0;
    double imageBlockedHeight = 48.0;
};

// ----------------------------------------------------------------- caching

struct StyleMetrics {
    double ascent = 0.0;
    double descent = 0.0;
    double spaceWidth = 0.0;
};

// Memoises word widths and per-style metrics. Keep one per (measurer, font
// configuration) and reuse it across relayouts; clear() it when fonts or the
// display scale change. Not thread-safe; use one per thread.
class MeasureCache {
public:
    double width(const ITextMeasurer& m, std::string_view text, const TextStyle& style);
    StyleMetrics styleMetrics(const ITextMeasurer& m, const TextStyle& style);
    void clear();
    std::size_t size() const { return widths_.size(); }

private:
    std::unordered_map<std::string, double> widths_;
    std::unordered_map<std::uint64_t, StyleMetrics> styles_;
};

// ------------------------------------------------------------------ output

// How a line ends; drives plain-text extraction of selections.
enum class LineEnd : std::uint8_t {
    Wrap,      // soft wrap: ' ' when joined
    HardBreak, // '\n'
    BlockEnd,  // last line of a paragraph/heading/...: '\n'
    CodeLine,  // '\n'
    CellEnd,   // last line of a table cell, more cells follow in the row: '\t'
    RowEnd,    // last line of the last cell of a row: '\n'
};

struct LayoutRun {
    std::string text;
    TextStyle style;
    // 1-based index into MarkdownLayout::links; 0 = none. For image runs
    // (isImage) this is the image URL.
    std::uint32_t link = 0;
    bool isImage = false;
    double x = 0.0;
    double baseline = 0.0;
    double width = 0.0;
    std::uint32_t line = 0; // index into MarkdownLayout::lines
    // Source range of this run's text (see Md4cMarkdownParser.hpp for accuracy).
    // `exactSource` means source.size() == text.size() and byte i of the text is
    // byte source.start + i of the source.
    SourceRange source;
    bool exactSource = false;
};

struct LayoutLine {
    double top = 0.0;
    double height = 0.0;
    double baseline = 0.0;
    double x = 0.0;
    double width = 0.0;
    std::uint32_t runBegin = 0; // [runBegin, runEnd) into MarkdownLayout::runs
    std::uint32_t runEnd = 0;
    LineEnd end = LineEnd::BlockEnd;
};

struct LinkTarget {
    std::string url;
    std::string title;
};

enum class DecorationKind : std::uint8_t {
    CodeBackground,
    QuoteBar,
    ListBullet,
    ListNumber,
    TaskCheckbox,
    Rule,
    ImagePlaceholder,
};

struct Decoration {
    DecorationKind kind = DecorationKind::Rule;
    core::Rect rect;
    // ListNumber: the label ("12."). ImagePlaceholder: alt text.
    std::string text;
    // ImagePlaceholder: URL as written in the document.
    std::string url;
    // ListNumber: style and baseline for drawing the label.
    TextStyle style;
    double baseline = 0.0;
    bool checked = false;                  // TaskCheckbox
    ImageState imageState = ImageState::Unknown; // ImagePlaceholder
    std::uint32_t blockId = 0;             // source Block (or container) the decoration belongs to
};

struct TableCellLayout {
    core::Rect rect;
    ColumnAlign align = ColumnAlign::Default;
    std::uint32_t row = 0; // 0 = header
    std::uint32_t col = 0;
    bool header = false;
    std::uint32_t lineBegin = 0;
    std::uint32_t lineEnd = 0;
    SourceRange source;
};

enum class LayoutBlockKind : std::uint8_t { Paragraph, Heading, Code, Html, Table, Rule, Image };

struct LayoutBlock {
    std::uint32_t blockId = 0;
    LayoutBlockKind kind = LayoutBlockKind::Paragraph;
    int headingLevel = 0;
    core::Rect rect;             // the block's box (code: including padding)
    // Extent of the block's content. Greater than rect.size.width when the
    // content overflows horizontally (code lines, wide tables): the app
    // should clip to `rect` and offer horizontal scrolling.
    double contentWidth = 0.0;
    SourceRange source;
    std::uint32_t lineBegin = 0; // [lineBegin, lineEnd) into MarkdownLayout::lines
    std::uint32_t lineEnd = 0;
    std::uint32_t cellBegin = 0; // tables: [cellBegin, cellEnd) into MarkdownLayout::tableCells
    std::uint32_t cellEnd = 0;
    std::uint32_t decoBegin = 0; // decorations created by this block (code bg, rule, image, markers)
    std::uint32_t decoEnd = 0;

    bool overflowsHorizontally() const { return contentWidth > rect.size.width + 0.5; }
};

// A caret position: a run and a byte offset (code point boundary) in its text.
struct TextPosition {
    std::uint32_t run = 0;
    std::uint32_t offset = 0;
    auto operator<=>(const TextPosition&) const = default;
};

struct HitResult {
    bool valid = false;
    std::size_t block = 0;  // index into MarkdownLayout::blocks
    std::size_t line = 0;
    TextPosition position;
    bool onText = false;    // the point lies inside the run's box
    std::uint32_t link = 0; // link/image target of the run under the point (1-based; 0 none)
    std::size_t sourceOffset = 0; // best-effort source offset of the caret
};

struct MarkdownLayout {
    std::vector<LayoutBlock> blocks;      // flattened leaf blocks, in document order (ascending y)
    std::vector<LayoutLine> lines;
    std::vector<LayoutRun> runs;
    std::vector<Decoration> decorations;
    std::vector<TableCellLayout> tableCells;
    std::vector<LinkTarget> links;
    // Block id -> index in `blocks` (container ids map to their first leaf);
    // kNoBlock when the block produced nothing.
    std::vector<std::uint32_t> blockIndexById;

    double viewportWidth = 0.0;
    double contentWidth = 0.0;  // >= viewportWidth; larger when some block overflows
    double contentHeight = 0.0;

    static constexpr std::uint32_t kNoBlock = 0xFFFFFFFFu;

    // ---- lookup by position (O(log N))
    // Index of the block containing y, or the nearest one; npos when empty.
    std::size_t blockIndexAtY(double y) const;
    // Blocks intersecting [y0, y1): half-open index range.
    std::pair<std::size_t, std::size_t> blockRangeForY(double y0, double y1) const;
    // Decorations intersecting [y0, y1) (linear scan of the decoration list).
    std::vector<std::size_t> decorationsInRange(double y0, double y1) const;
    // Block whose source range contains `offset` (or the nearest preceding one).
    std::size_t blockIndexAtSource(std::size_t offset) const;
    // Top y of the block with this source Block id; -1 when unknown.
    double yForBlockId(std::uint32_t blockId) const;
    double yForSourceOffset(std::size_t offset) const;

    // ---- hit-testing and selection (the measurer must match the one used to lay out)
    HitResult hitTest(core::Point p, const ITextMeasurer& measurer) const;
    // Caret x of a position.
    double xForPosition(TextPosition pos, const ITextMeasurer& measurer) const;
    // Highlight rectangles (one per line fragment, merged per line) between two
    // positions in either order. Works across blocks.
    std::vector<core::Rect> selectionRects(TextPosition a, TextPosition b, const ITextMeasurer& measurer) const;
    // Plain text between two positions: soft wraps become ' ', lines '\n',
    // table cells '\t'.
    std::string selectedText(TextPosition a, TextPosition b) const;
    // Union of the source ranges of the runs touched by the selection.
    SourceRange selectedSource(TextPosition a, TextPosition b) const;
    // Best-effort source offset of a position.
    std::size_t sourceOffsetAt(TextPosition pos) const;
};

// Lays out `doc` for the given viewport width. `images` may be null (all
// images Unknown). `cache` may be null (a temporary one is used).
MarkdownLayout layoutMarkdown(const MarkdownDocument& doc, double viewportWidth, const ITextMeasurer& measurer,
                              const IImageSizeProvider* images = nullptr, const Typography& typography = {},
                              MeasureCache* cache = nullptr);

} // namespace rivet::markdown
