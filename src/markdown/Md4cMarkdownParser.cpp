// SPDX-License-Identifier: MPL-2.0
#include "markdown/Md4cMarkdownParser.hpp"

#include "markdown/MarkdownPlainText.hpp"
#include "markdown/MarkdownSlug.hpp"
#include "markdown/Utf8.hpp"

#include <md4c.h> // the only place MD4C types are visible

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace rivet::markdown {

const HeadingEntry* MarkdownDocument::findHeadingBySlug(std::string_view anchor) const {
    if (!anchor.empty() && anchor.front() == '#') anchor.remove_prefix(1);
    for (const HeadingEntry& h : headings) {
        if (h.slug == anchor) return &h;
    }
    return nullptr;
}

std::unique_ptr<IMarkdownParser> makeMarkdownParser() { return std::make_unique<Md4cMarkdownParser>(); }

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// ------------------------------------------------------------ entities

struct NamedEntity {
    std::string_view name;
    char32_t cp;
};

constexpr std::array<NamedEntity, 40> kEntities{{
    {"amp", '&'},     {"lt", '<'},      {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", 0xA0},   {"copy", 0xA9},   {"reg", 0xAE},     {"trade", 0x2122}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},  {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7},
    {"deg", 0xB0},    {"plusmn", 0xB1}, {"times", 0xD7},   {"divide", 0xF7},  {"euro", 0x20AC},
    {"pound", 0xA3},  {"yen", 0xA5},    {"cent", 0xA2},    {"sect", 0xA7},    {"para", 0xB6},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191},  {"darr", 0x2193},  {"hearts", 0x2665},
    {"shy", 0xAD},    {"ensp", 0x2002}, {"emsp", 0x2003},  {"thinsp", 0x2009}, {"minus", 0x2212},
}};

// `e` is "&...;" as found in the source. Unknown entities are kept verbatim.
void decodeEntity(std::string& out, std::string_view e) {
    if (e.size() < 3 || e.front() != '&' || e.back() != ';') {
        out.append(e);
        return;
    }
    std::string_view body = e.substr(1, e.size() - 2);
    if (!body.empty() && body.front() == '#') {
        body.remove_prefix(1);
        int base = 10;
        if (!body.empty() && (body.front() == 'x' || body.front() == 'X')) {
            base = 16;
            body.remove_prefix(1);
        }
        if (body.empty() || body.size() > 8) {
            out.append(e);
            return;
        }
        std::uint64_t v = 0;
        for (const char c : body) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else {
                out.append(e);
                return;
            }
            v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        }
        utf8::append(out, (v == 0 || v > 0x10FFFF) ? utf8::kReplacement : static_cast<char32_t>(v));
        return;
    }
    for (const NamedEntity& n : kEntities) {
        if (n.name == body) {
            utf8::append(out, n.cp);
            return;
        }
    }
    out.append(e);
}

// ------------------------------------------------------------ extents

struct Extent {
    std::size_t lo = kNone;
    std::size_t hi = 0;
    bool has() const { return lo != kNone; }
    void add(std::size_t a, std::size_t b) {
        if (lo == kNone || a < lo) lo = a;
        if (b > hi) hi = b;
    }
};

enum class FK : std::uint8_t { Doc, Quote, List, Item, Leaf, Table, Section, Row, Cell };

struct Frame {
    FK kind = FK::Doc;
    MD_BLOCKTYPE type = MD_BLOCK_DOC;
    bool phantom = false;
    bool implicit = false;
    Block* block = nullptr;
    ListItem* item = nullptr;
    TableRow* row = nullptr;
    TableCell* cell = nullptr;
    std::vector<Block>* blocks = nullptr;   // Doc/Quote/Item (and phantom containers): child block sink
    std::vector<Inline>* inlines = nullptr; // Leaf/Cell: root inline sink
    Extent ext;
    // Fenced code.
    std::size_t fenceStart = kNone;
    char fenceChar = 0;
    // Table / Row bookkeeping.
    bool isHead = false;
    bool skipRows = false;
    std::size_t cellCount = 0;
    std::size_t colIndex = 0;
};

struct InlineFrame {
    MD_SPANTYPE type = MD_SPAN_EM;
    Inline* node = nullptr; // nullptr: phantom (flattened) span
    std::vector<Inline>* target = nullptr;
    Extent ext;
    bool autolink = false;
};

bool isEol(char c) { return c == '\n' || c == '\r'; }
bool isBlank(char c) { return c == ' ' || c == '\t'; }

class Builder {
public:
    Builder(std::string_view src, const ParseLimits& limits, MarkdownDocument& doc)
        : src_(src), n_(src.size()), limits_(limits), doc_(doc) {}

    bool aborted() const { return aborted_; }
    bool failed() const { return failed_; }

    // ------------------------------------------------ MD4C callbacks
    int enterBlock(MD_BLOCKTYPE type, void* detail) {
        closeImplicit();
        switch (type) {
        case MD_BLOCK_DOC: {
            Frame f;
            f.kind = FK::Doc;
            f.type = type;
            f.blocks = &doc_.blocks;
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            if (!countBlock()) return 1;
            Frame f;
            f.type = type;
            f.kind = type == MD_BLOCK_QUOTE ? FK::Quote : FK::List;
            if (depth_ >= limits_.maxNestingDepth) {
                f.phantom = true;
                f.blocks = containerBlocks();
                diagOnce(DiagnosticCode::NestingTooDeep, DiagnosticSeverity::Warning,
                         "container nesting too deep; inner containers were flattened");
            } else {
                Block& b = newBlock(type == MD_BLOCK_QUOTE ? BlockKind::Quote : BlockKind::List);
                f.block = &b;
                if (type == MD_BLOCK_QUOTE) {
                    f.blocks = &b.children;
                } else if (type == MD_BLOCK_UL) {
                    b.ordered = false;
                    b.tight = static_cast<const MD_BLOCK_UL_DETAIL*>(detail)->is_tight != 0;
                } else {
                    const auto* d = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
                    b.ordered = true;
                    b.start = d->start;
                    b.tight = d->is_tight != 0;
                }
                ++depth_;
            }
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_LI: {
            if (!countBlock()) return 1;
            Frame f;
            f.kind = FK::Item;
            f.type = type;
            Frame& list = stack_.back();
            if (list.phantom || list.block == nullptr) {
                f.phantom = true;
                f.blocks = containerBlocks();
            } else {
                ListItem& item = list.block->items.emplace_back();
                const auto* d = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
                if (d && d->is_task) {
                    item.isTask = true;
                    item.checked = d->task_mark == 'x' || d->task_mark == 'X';
                }
                f.item = &item;
                f.blocks = &item.children;
            }
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_HR: {
            if (!countBlock()) return 1;
            Block& b = newBlock(BlockKind::HorizontalRule);
            const auto [s, e] = nextNonBlankLine(cursor_);
            b.range = {s, e};
            finishedLineRange(b.range);
            return 0;
        }
        case MD_BLOCK_H:
        case MD_BLOCK_P:
        case MD_BLOCK_CODE:
        case MD_BLOCK_HTML: {
            if (!countBlock()) return 1;
            pushLeaf(type, detail, false);
            return 0;
        }
        case MD_BLOCK_TABLE: {
            if (!countBlock()) return 1;
            Block& b = newBlock(BlockKind::Table);
            const auto* d = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
            const std::size_t cols = std::min<std::size_t>(d ? d->col_count : 0, limits_.maxTableColumns);
            if (d && d->col_count > limits_.maxTableColumns) {
                diagOnce(DiagnosticCode::TableTooLarge, DiagnosticSeverity::Warning,
                         "table has more columns than the limit; extra columns were dropped");
            }
            b.alignments.assign(cols, ColumnAlign::Default);
            Frame f;
            f.kind = FK::Table;
            f.type = type;
            f.block = &b;
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY: {
            Frame f;
            f.kind = FK::Section;
            f.type = type;
            f.isHead = type == MD_BLOCK_THEAD;
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_TR: {
            if (!countBlock()) return 1;
            Frame* table = tableFrame();
            Frame f;
            f.kind = FK::Row;
            f.type = type;
            const bool head = stack_.back().isHead;
            if (table == nullptr || table->skipRows) {
                scratchRow_ = TableRow{};
                f.row = &scratchRow_;
            } else if (head) {
                f.row = &table->block->header;
            } else {
                f.row = &table->block->body.emplace_back();
            }
            f.skipRows = table == nullptr || table->skipRows; // row discarded
            stack_.push_back(f);
            return 0;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            Frame* table = tableFrame();
            Frame& row = stack_.back();
            Frame f;
            f.kind = FK::Cell;
            f.type = type;
            const std::size_t col = row.colIndex++;
            if (table != nullptr && table->block != nullptr && type == MD_BLOCK_TH && col < table->block->alignments.size()) {
                const auto* d = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
                table->block->alignments[col] = d ? toAlign(d->align) : ColumnAlign::Default;
            }
            bool keep = table != nullptr && !row.skipRows && col < limits_.maxTableColumns;
            if (keep) {
                if (++table->cellCount > limits_.maxTableCells) {
                    table->skipRows = true; // drop the rest of this row and every later row
                    row.skipRows = true;
                    keep = false;
                    diagOnce(DiagnosticCode::TableTooLarge, DiagnosticSeverity::Warning,
                             "table has more cells than the limit; remaining rows were dropped");
                    // The row already in progress is incomplete: discard it.
                    if (row.row != nullptr && row.row == &table->block->body.back()) table->block->body.pop_back();
                    scratchRow_ = TableRow{};
                    row.row = &scratchRow_;
                }
            }
            if (keep) {
                f.cell = &row.row->cells.emplace_back();
            } else {
                scratchCell_ = TableCell{};
                f.cell = &scratchCell_;
            }
            f.inlines = &f.cell->inlines;
            inlineCursor_ = cursor_;
            stack_.push_back(f);
            return 0;
        }
        default: {
            // Unsupported block (extensions are not enabled): pass through.
            Frame f;
            f.kind = FK::Quote;
            f.type = type;
            f.phantom = true;
            f.blocks = containerBlocks();
            stack_.push_back(f);
            return 0;
        }
        }
    }

    int leaveBlock(MD_BLOCKTYPE type) {
        closeImplicit();
        if (stack_.empty()) return 0;
        switch (type) {
        case MD_BLOCK_HR:
            return 0;
        default:
            popFrame();
            return 0;
        }
    }

    int enterSpan(MD_SPANTYPE type, void* detail) {
        Frame* leaf = ensureLeaf();
        if (leaf == nullptr) return 0;
        InlineFrame f;
        f.type = type;
        std::vector<Inline>* parent = currentInlineTarget();
        const bool inImage = insideImage();
        const bool supported = type == MD_SPAN_EM || type == MD_SPAN_STRONG || type == MD_SPAN_DEL ||
                               type == MD_SPAN_CODE || type == MD_SPAN_A || type == MD_SPAN_IMG;
        if (!supported || inImage || inline_.size() >= limits_.maxInlineDepth) {
            if (supported && !inImage) {
                diagOnce(DiagnosticCode::InlineTooDeep, DiagnosticSeverity::Warning,
                         "inline nesting too deep; inner spans were flattened");
            }
            f.target = parent;
            inline_.push_back(f);
            return 0;
        }
        Inline& in = parent->emplace_back();
        f.node = &in;
        f.target = &in.children;
        switch (type) {
        case MD_SPAN_EM: in.kind = InlineKind::Emphasis; break;
        case MD_SPAN_STRONG: in.kind = InlineKind::Strong; break;
        case MD_SPAN_DEL: in.kind = InlineKind::Strike; break;
        case MD_SPAN_CODE: in.kind = InlineKind::Code; break;
        case MD_SPAN_A: {
            in.kind = InlineKind::Link;
            const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
            in.url = attrToString(d->href);
            in.title = attrToString(d->title);
            f.autolink = d->is_autolink != 0;
            break;
        }
        default: {
            in.kind = InlineKind::Image;
            const auto* d = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
            in.url = attrToString(d->src);
            in.title = attrToString(d->title);
            break;
        }
        }
        lastTextOpen_ = false;
        inline_.push_back(f);
        return 0;
    }

    int leaveSpan(MD_SPANTYPE) {
        if (inline_.empty()) return 0;
        InlineFrame f = inline_.back();
        inline_.pop_back();
        Extent ext = f.ext;
        if (f.node != nullptr) {
            SourceRange r = spanRange(f, ext);
            f.node->range = r;
            ext = Extent{};
            ext.add(r.start, r.end);
        }
        if (ext.has()) {
            holder().add(ext.lo, ext.hi);
            inlineCursor_ = std::max(inlineCursor_ == kNone ? 0 : inlineCursor_, ext.hi);
        }
        lastTextOpen_ = false;
        return 0;
    }

    int text(MD_TEXTTYPE type, const char* p, std::size_t size) {
        if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
            if (ensureLeaf() == nullptr) return 0;
            addBreak(type == MD_TEXT_BR);
            return 0;
        }
        // Verbatim blocks collect text directly.
        if (!stack_.empty() && stack_.back().kind == FK::Leaf && stack_.back().block != nullptr &&
            (stack_.back().type == MD_BLOCK_CODE || stack_.back().type == MD_BLOCK_HTML)) {
            Frame& f = stack_.back();
            noteExtent(f.ext, p, size);
            appendLimited(f.block->text, type, p, size);
            return 0;
        }
        if (ensureLeaf() == nullptr) return 0;
        std::string decoded;
        appendDecoded(decoded, type, p, size);
        SourceRange r = chunkRange(p, size);
        Extent chunk;
        if (inSource(p, size)) chunk.add(r.start, r.end);
        if (chunk.has()) {
            holder().add(chunk.lo, chunk.hi);
            inlineCursor_ = std::max(inlineCursor_ == kNone ? 0 : inlineCursor_, chunk.hi);
        }
        if (!inline_.empty() && inline_.back().node != nullptr) {
            Inline& node = *inline_.back().node;
            if (node.kind == InlineKind::Image || node.kind == InlineKind::Code) {
                node.text += decoded;
                return 0;
            }
        }
        addText(decoded, r);
        return 0;
    }

    // ------------------------------------------------ finish
    void finish() {
        closeImplicit();
        while (!stack_.empty()) popFrame();
        doc_.blockCount = nextId_;
    }

    void diagOnce(DiagnosticCode code, DiagnosticSeverity sev, std::string message, SourceRange r = {}) {
        const auto bit = static_cast<unsigned>(code);
        if (diagSeen_ & (1u << bit)) return;
        diagSeen_ |= 1u << bit;
        doc_.diagnostics.push_back({sev, code, std::move(message), r});
    }

    void markFailed() { failed_ = true; }
    void markAborted() { aborted_ = true; }

private:
    // ------------------------------------------------ source scanning
    std::pair<std::size_t, std::size_t> lineBounds(std::size_t p) {
        if (p > n_) p = n_;
        if (lineLo_ != kNone && p >= lineLo_ && p <= lineHi_) return {lineLo_, lineHi_};
        std::size_t s = p;
        while (s > 0 && !isEol(src_[s - 1])) --s;
        std::size_t e = p;
        while (e < n_ && !isEol(src_[e])) ++e;
        lineLo_ = s;
        lineHi_ = e;
        return {s, e};
    }
    std::size_t afterEol(std::size_t e) const {
        if (e >= n_) return n_;
        if (src_[e] == '\r' && e + 1 < n_ && src_[e + 1] == '\n') return e + 2;
        return e + 1;
    }
    bool lineIsBlank(std::size_t s, std::size_t e) const {
        for (std::size_t i = s; i < e; ++i) {
            if (!isBlank(src_[i])) return false;
        }
        return true;
    }
    // Next non-blank line at or after `from` (a line start); empty range at EOF.
    std::pair<std::size_t, std::size_t> nextNonBlankLine(std::size_t from) {
        while (from < n_) {
            const auto [s, e] = lineBounds(from);
            if (!lineIsBlank(s, e)) return {s, e};
            from = afterEol(e);
        }
        return {n_, n_};
    }
    void finishedLineRange(SourceRange r) {
        stack_.back().ext.add(r.start, r.end);
        advanceCursor(r.end);
    }
    void advanceCursor(std::size_t end) {
        const std::size_t c = afterEol(std::min(end, n_));
        if (c > cursor_) cursor_ = c;
    }
    bool inSource(const char* p, std::size_t size) const {
        return p >= src_.data() && p + size <= src_.data() + n_;
    }
    SourceRange chunkRange(const char* p, std::size_t size) const {
        if (!inSource(p, size)) {
            const std::size_t at = inlineCursor_ == kNone ? cursor_ : inlineCursor_;
            return {std::min(at, n_), std::min(at, n_)};
        }
        std::size_t s = static_cast<std::size_t>(p - src_.data());
        std::size_t e = s + size;
        // A backslash escape: MD4C reports the escaped character only.
        if (size == 1 && s > 0 && src_[s - 1] == '\\' && !isEol(src_[s]) && std::ispunct(static_cast<unsigned char>(src_[s]))) --s;
        while (e > s && isEol(src_[e - 1])) --e;
        return {s, e};
    }
    void noteExtent(Extent& ext, const char* p, std::size_t size) {
        if (!inSource(p, size) || size == 0) return;
        const SourceRange r = chunkRange(p, size);
        ext.add(r.start, r.end);
    }

    // ------------------------------------------------ text helpers
    void appendSanitized(std::string& out, const char* p, std::size_t size) {
        bool ascii = true;
        for (std::size_t i = 0; i < size; ++i) {
            if (static_cast<unsigned char>(p[i]) >= 0x80) {
                ascii = false;
                break;
            }
        }
        if (ascii) {
            out.append(p, size);
            return;
        }
        bool changed = false;
        out += utf8::sanitize(std::string_view(p, size), &changed);
        if (changed) {
            diagOnce(DiagnosticCode::InvalidUtf8, DiagnosticSeverity::Info,
                     "invalid UTF-8 was replaced with U+FFFD in the rendered text");
        }
    }
    void appendDecoded(std::string& out, MD_TEXTTYPE type, const char* p, std::size_t size) {
        switch (type) {
        case MD_TEXT_NULLCHAR: utf8::append(out, utf8::kReplacement); break;
        case MD_TEXT_ENTITY: decodeEntity(out, std::string_view(p, size)); break;
        default: appendSanitized(out, p, size); break;
        }
    }
    void appendLimited(std::string& out, MD_TEXTTYPE type, const char* p, std::size_t size) {
        if (out.size() >= limits_.maxCodeBlockBytes) {
            diagOnce(DiagnosticCode::CodeBlockTruncated, DiagnosticSeverity::Warning,
                     "a code/HTML block exceeded the size limit and was cut");
            return;
        }
        std::string tmp;
        appendDecoded(tmp, type, p, size);
        const std::size_t room = limits_.maxCodeBlockBytes - out.size();
        if (tmp.size() > room) {
            std::size_t cut = utf8::snapToBoundary(tmp, room);
            tmp.resize(cut);
            diagOnce(DiagnosticCode::CodeBlockTruncated, DiagnosticSeverity::Warning,
                     "a code/HTML block exceeded the size limit and was cut");
        }
        out += tmp;
    }
    std::string attrToString(const MD_ATTRIBUTE& a) {
        std::string out;
        if (a.text == nullptr || a.size == 0) return out;
        if (a.substr_types == nullptr || a.substr_offsets == nullptr) {
            appendSanitized(out, a.text, a.size);
            return out;
        }
        for (std::size_t i = 0; a.substr_offsets[i] < a.size; ++i) {
            const std::size_t off = a.substr_offsets[i];
            const std::size_t end = std::min<std::size_t>(a.substr_offsets[i + 1], a.size);
            if (end <= off) break;
            appendDecoded(out, a.substr_types[i], a.text + off, end - off);
        }
        return out;
    }
    static ColumnAlign toAlign(MD_ALIGN a) {
        switch (a) {
        case MD_ALIGN_LEFT: return ColumnAlign::Left;
        case MD_ALIGN_CENTER: return ColumnAlign::Center;
        case MD_ALIGN_RIGHT: return ColumnAlign::Right;
        default: return ColumnAlign::Default;
        }
    }

    // ------------------------------------------------ blocks
    bool countBlock() {
        if (++blockTotal_ > limits_.maxBlocks) {
            diagOnce(DiagnosticCode::TooManyBlocks, DiagnosticSeverity::Error,
                     "block limit reached; the rest of the document was not parsed");
            aborted_ = true;
            return false;
        }
        return true;
    }
    std::vector<Block>* containerBlocks() {
        for (std::size_t i = stack_.size(); i-- > 0;) {
            if (stack_[i].blocks != nullptr) return stack_[i].blocks;
        }
        return &doc_.blocks;
    }
    Block& newBlock(BlockKind kind) {
        Block& b = containerBlocks()->emplace_back();
        b.id = nextId_++;
        b.kind = kind;
        return b;
    }
    Frame* tableFrame() {
        for (std::size_t i = stack_.size(); i-- > 0;) {
            if (stack_[i].kind == FK::Table) return &stack_[i];
        }
        return nullptr;
    }
    void pushLeaf(MD_BLOCKTYPE type, void* detail, bool implicit) {
        Frame f;
        f.kind = FK::Leaf;
        f.type = type;
        f.implicit = implicit;
        BlockKind kind = BlockKind::Paragraph;
        if (type == MD_BLOCK_H) kind = BlockKind::Heading;
        else if (type == MD_BLOCK_CODE) kind = BlockKind::CodeBlock;
        else if (type == MD_BLOCK_HTML) kind = BlockKind::HtmlBlock;
        Block& b = newBlock(kind);
        f.block = &b;
        if (type == MD_BLOCK_H) {
            b.level = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level);
            f.inlines = &b.inlines;
        } else if (type == MD_BLOCK_CODE) {
            const auto* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
            b.info = attrToString(d->info);
            b.fenced = d->fence_char != 0;
            f.fenceChar = d->fence_char;
            if (b.fenced) f.fenceStart = nextNonBlankLine(cursor_).first;
        } else if (type == MD_BLOCK_P) {
            f.inlines = &b.inlines;
        }
        inlineCursor_ = cursor_;
        lastTextOpen_ = false;
        stack_.push_back(f);
    }
    // Text outside any leaf (tight list items): open an implicit paragraph.
    Frame* ensureLeaf() {
        if (stack_.empty()) return nullptr;
        Frame& top = stack_.back();
        if (top.kind == FK::Leaf || top.kind == FK::Cell) return &top;
        if (top.kind == FK::Item || top.kind == FK::Quote || top.kind == FK::Doc) {
            if (!countBlock()) return nullptr;
            pushLeaf(MD_BLOCK_P, nullptr, true);
            return &stack_.back();
        }
        return nullptr;
    }
    void closeImplicit() {
        if (!stack_.empty() && stack_.back().kind == FK::Leaf && stack_.back().implicit) {
            inline_.clear();
            popFrame();
        }
    }
    Extent& holder() {
        if (!inline_.empty()) return inline_.back().ext;
        return stack_.back().ext;
    }
    std::vector<Inline>* currentInlineTarget() {
        if (!inline_.empty()) return inline_.back().target;
        return stack_.back().inlines;
    }
    bool insideImage() const {
        for (const InlineFrame& f : inline_) {
            if (f.type == MD_SPAN_IMG && f.node != nullptr) return true;
        }
        return false;
    }

    // ------------------------------------------------ inlines
    void addText(const std::string& decoded, SourceRange r) {
        if (decoded.empty()) return;
        std::vector<Inline>* target = currentInlineTarget();
        if (target == nullptr) return;
        if (!target->empty() && target->back().kind == InlineKind::Text && lastTextOpen_) {
            Inline& t = target->back();
            t.text += decoded;
            if (r.start < t.range.start) t.range.start = r.start;
            if (r.end > t.range.end) t.range.end = r.end;
            return;
        }
        Inline& t = target->emplace_back();
        t.kind = InlineKind::Text;
        t.text = decoded;
        t.range = r;
        lastTextOpen_ = true;
    }
    void addBreak(bool hard) {
        if (insideImage()) {
            if (!inline_.empty() && inline_.back().node != nullptr) inline_.back().node->text.push_back(' ');
            return;
        }
        std::vector<Inline>* target = currentInlineTarget();
        if (target == nullptr) return;
        const std::size_t at = std::min(inlineCursor_ == kNone ? cursor_ : inlineCursor_, n_);
        std::size_t e = at;
        while (e < n_ && !isEol(src_[e])) ++e;
        std::size_t end = e < n_ ? afterEol(e) : e;
        Inline& in = target->emplace_back();
        in.kind = hard ? InlineKind::HardBreak : InlineKind::SoftBreak;
        in.range = {at, end};
        holder().add(at, e);
        lastTextOpen_ = false;
    }

    std::size_t scanLinkEnd(std::size_t p) const {
        if (p >= n_) return p;
        if (src_[p] == '(') {
            int depth = 1;
            char quote = 0;
            const std::size_t limit = std::min(n_, p + 8192);
            for (std::size_t i = p + 1; i < limit; ++i) {
                const char c = src_[i];
                if (c == '\\') { ++i; continue; }
                if (quote) {
                    if (c == quote) quote = 0;
                    continue;
                }
                if ((c == '"' || c == '\'') && isBlank(src_[i - 1]) ) quote = c;
                else if (c == '(') ++depth;
                else if (c == ')' && --depth == 0) return i + 1;
            }
            return p;
        }
        if (src_[p] == '[') {
            const std::size_t limit = std::min(n_, p + 1024);
            for (std::size_t i = p + 1; i < limit; ++i) {
                if (src_[i] == '\\') { ++i; continue; }
                if (src_[i] == ']') return i + 1;
                if (src_[i] == '[') break;
            }
        }
        return p;
    }

    SourceRange spanRange(const InlineFrame& f, const Extent& ext) {
        std::size_t lo = ext.lo;
        std::size_t hi = ext.hi;
        const bool has = ext.has();
        const std::size_t cursor = inlineCursor_ == kNone ? cursor_ : inlineCursor_;
        switch (f.type) {
        case MD_SPAN_EM:
        case MD_SPAN_STRONG: {
            if (!has) return {std::min(cursor, n_), std::min(cursor, n_)};
            const std::size_t d = f.type == MD_SPAN_STRONG ? 2 : 1;
            if (lo >= d && hi + d <= n_) {
                const char c = src_[lo - 1];
                bool ok = c == '*' || c == '_';
                for (std::size_t k = 1; ok && k <= d; ++k) ok = src_[lo - k] == c && src_[hi + k - 1] == c;
                if (ok) { lo -= d; hi += d; }
            }
            break;
        }
        case MD_SPAN_DEL: {
            if (!has) return {std::min(cursor, n_), std::min(cursor, n_)};
            std::size_t before = 0;
            while (before < 2 && before < lo && src_[lo - before - 1] == '~') ++before;
            std::size_t after = 0;
            while (after < 2 && hi + after < n_ && src_[hi + after] == '~') ++after;
            const std::size_t d = std::min(before, after);
            lo -= d;
            hi += d;
            break;
        }
        case MD_SPAN_CODE: {
            if (!has) return {std::min(cursor, n_), std::min(cursor, n_)};
            std::size_t s = lo;
            std::size_t e = hi;
            if (s > 0 && src_[s - 1] == ' ') --s;
            if (e < n_ && src_[e] == ' ') ++e;
            std::size_t before = 0;
            while (before < s && src_[s - before - 1] == '`') ++before;
            std::size_t after = 0;
            while (e + after < n_ && src_[e + after] == '`') ++after;
            std::size_t d = std::min(before, after);
            if (d == 0 && s != lo) { // the space belonged to the content
                s = lo;
                e = hi;
                while (before < s && src_[s - before - 1] == '`') ++before;
                while (e + after < n_ && src_[e + after] == '`') ++after;
                d = std::min(before, after);
            }
            if (d > 0) { lo = s - d; hi = e + d; }
            break;
        }
        case MD_SPAN_A:
        case MD_SPAN_IMG: {
            const bool image = f.type == MD_SPAN_IMG;
            std::size_t open;
            std::size_t closeBracket;
            if (has) {
                const std::size_t need = image ? 2 : 1;
                if (lo < need || src_[lo - 1] != '[' || (image && src_[lo - 2] != '!')) {
                    if (f.autolink && lo > 0 && hi < n_ && src_[lo - 1] == '<' && src_[hi] == '>') { --lo; ++hi; }
                    break;
                }
                open = lo - need;
                closeBracket = hi;
            } else {
                // Empty text: find the opener after the cursor.
                std::size_t i = std::min(cursor, n_);
                const std::size_t limit = std::min(n_, i + 65536);
                std::size_t found = kNone;
                for (; i < limit; ++i) {
                    if (image ? (src_[i] == '!' && i + 1 < n_ && src_[i + 1] == '[') : src_[i] == '[') { found = i; break; }
                }
                if (found == kNone) return {std::min(cursor, n_), std::min(cursor, n_)};
                open = found;
                closeBracket = found + (image ? 2 : 1);
            }
            if (closeBracket >= n_ || src_[closeBracket] != ']') {
                lo = open;
                hi = has ? hi : closeBracket;
                break;
            }
            lo = open;
            const std::size_t after = closeBracket + 1;
            const std::size_t end = scanLinkEnd(after);
            hi = end > after ? end : after;
            break;
        }
        default:
            break;
        }
        if (hi > n_) hi = n_;
        if (lo > hi) lo = hi;
        return {lo, hi};
    }

    // ------------------------------------------------ finishing frames
    bool isSetextUnderline(std::size_t s, std::size_t e) const {
        std::size_t i = s;
        while (i < e && (isBlank(src_[i]) || src_[i] == '>')) ++i;
        if (i >= e || (src_[i] != '=' && src_[i] != '-')) return false;
        const char c = src_[i];
        while (i < e && src_[i] == c) ++i;
        while (i < e && isBlank(src_[i])) ++i;
        return i == e;
    }
    bool isClosingFence(std::size_t s, std::size_t e, char fence) const {
        std::size_t i = s;
        while (i < e && (isBlank(src_[i]) || src_[i] == '>')) ++i;
        std::size_t run = 0;
        while (i < e && src_[i] == fence) { ++i; ++run; }
        if (run < 3) return false;
        while (i < e && isBlank(src_[i])) ++i;
        return i == e;
    }

    SourceRange lineRangeOf(const Extent& ext) {
        if (!ext.has()) {
            const auto [s, e] = nextNonBlankLine(cursor_);
            return {s, e};
        }
        const auto s = lineBounds(ext.lo).first;
        const auto e = lineBounds(ext.hi > ext.lo ? ext.hi - 1 : ext.hi).second;
        // lineBounds(hi - 1) keeps the end on the line that holds the last byte.
        return {s, std::max(e, std::min(ext.hi, n_))};
    }

    void finishLeaf(Frame& f) {
        Block& b = *f.block;
        SourceRange r;
        if (b.kind == BlockKind::CodeBlock && b.fenced) {
            r.start = f.fenceStart != kNone ? f.fenceStart : (f.ext.has() ? lineBounds(f.ext.lo).first : cursor_);
            std::size_t e;
            if (f.ext.has()) e = lineRangeOf(f.ext).end;
            else e = lineBounds(std::min(r.start, n_)).second;
            std::size_t from = afterEol(e);
            while (from < n_ && e < n_) {
                const auto [s, le] = lineBounds(from);
                if (lineIsBlank(s, le)) { from = afterEol(le); continue; }
                if (isClosingFence(s, le, f.fenceChar)) e = le;
                break;
            }
            r.end = std::max(e, r.start);
        } else {
            r = lineRangeOf(f.ext);
            if (b.kind == BlockKind::Heading && f.ext.has()) {
                const auto [ls, le] = lineBounds(r.end);
                bool atx = false;
                for (std::size_t i = ls; i < f.ext.lo && i < n_; ++i) atx = atx || src_[i] == '#';
                if (!atx && le < n_) {
                    const auto [s2, e2] = lineBounds(afterEol(le));
                    if (isSetextUnderline(s2, e2)) r.end = e2;
                }
            }
        }
        b.range = clampRange(r);
        if (b.kind == BlockKind::Heading) {
            const std::string plain = inlinesToPlainText(b.inlines);
            b.slug = slugs_.allocate(plain);
            doc_.headings.push_back({b.id, b.level, b.slug, plain, b.range});
        } else if (b.kind == BlockKind::Paragraph) {
            promoteImageBlock(b);
        }
    }
    static void promoteImageBlock(Block& b) {
        std::size_t images = 0;
        for (const Inline& in : b.inlines) {
            if (in.kind == InlineKind::Image) {
                ++images;
            } else if (in.kind == InlineKind::SoftBreak) {
                continue;
            } else if (in.kind == InlineKind::Text) {
                for (const char c : in.text) {
                    if (!(c == ' ' || c == '\t' || c == '\n')) return;
                }
            } else {
                return;
            }
        }
        if (images != 1) return;
        std::vector<Inline> kept;
        for (Inline& in : b.inlines) {
            if (in.kind == InlineKind::Image) kept.push_back(std::move(in));
        }
        b.inlines = std::move(kept);
        b.kind = BlockKind::ImageBlock;
    }
    SourceRange clampRange(SourceRange r) const {
        r.start = std::min(r.start, n_);
        r.end = std::min(std::max(r.end, r.start), n_);
        return r;
    }

    void popFrame() {
        Frame f = stack_.back();
        stack_.pop_back();
        SourceRange range;
        bool lineLevel = true;
        switch (f.kind) {
        case FK::Doc:
            return;
        case FK::Leaf:
            if (f.block == nullptr) return;
            inline_.clear();
            finishLeaf(f);
            range = f.block->range;
            break;
        case FK::Quote:
        case FK::List:
            if (f.block != nullptr) {
                range = clampRange(lineRangeOf(f.ext));
                if (f.block->kind == BlockKind::List) {
                    // Item ranges are final; the list covers them.
                }
                f.block->range = range;
                --depth_;
            } else if (f.phantom && f.kind != FK::Doc) {
                if (!f.ext.has()) return;
                range = clampRange(lineRangeOf(f.ext));
            } else {
                return;
            }
            break;
        case FK::Item:
            range = clampRange(lineRangeOf(f.ext));
            if (f.item != nullptr) f.item->range = range;
            break;
        case FK::Table: {
            range = clampRange(lineRangeOf(f.ext));
            Block& t = *f.block;
            if (t.body.empty() && f.ext.has()) {
                // Header-only table: include the delimiter row.
                const auto [ls, le] = lineBounds(range.end);
                if (le < n_) {
                    const auto [s2, e2] = lineBounds(afterEol(le));
                    bool delim = e2 > s2;
                    for (std::size_t i = s2; i < e2 && delim; ++i) {
                        const char c = src_[i];
                        delim = c == '|' || c == '-' || c == ':' || isBlank(c);
                    }
                    if (delim) range.end = e2;
                }
                (void)ls;
            }
            t.range = range;
            break;
        }
        case FK::Section:
            if (!stack_.empty() && f.ext.has()) stack_.back().ext.add(f.ext.lo, f.ext.hi);
            return;
        case FK::Row: {
            if (f.row == nullptr) return;
            const bool discarded = f.skipRows;
            if (!discarded) {
                range = clampRange(lineRangeOf(f.ext));
                f.row->range = range;
                Frame* table = tableFrame();
                if (table != nullptr && table->block != nullptr) {
                    const std::size_t cols = table->block->alignments.size();
                    while (f.row->cells.size() < cols) {
                        TableCell& c = f.row->cells.emplace_back();
                        c.range = {range.end, range.end};
                    }
                }
            } else {
                return;
            }
            break;
        }
        case FK::Cell: {
            lineLevel = false;
            if (f.cell == nullptr) return;
            if (f.ext.has()) {
                range = clampRange({f.ext.lo, f.ext.hi});
            } else {
                const std::size_t at = std::min(inlineCursor_ == kNone ? cursor_ : inlineCursor_, n_);
                range = {at, at};
            }
            f.cell->range = range;
            inline_.clear();
            break;
        }
        }
        if (!stack_.empty()) {
            if (range.end > range.start || f.kind == FK::Cell) stack_.back().ext.add(range.start, range.end);
            else if (lineLevel) stack_.back().ext.add(range.start, range.end);
        }
        if (lineLevel) advanceCursor(range.end);
    }

    std::string_view src_;
    std::size_t n_;
    const ParseLimits& limits_;
    MarkdownDocument& doc_;
    std::vector<Frame> stack_;
    std::vector<InlineFrame> inline_;
    SlugAllocator slugs_;
    TableRow scratchRow_;
    TableCell scratchCell_;
    std::size_t cursor_ = 0;            // start of the next line not yet consumed by a block
    std::size_t inlineCursor_ = kNone;  // end of the last inline content seen in this leaf
    std::size_t lineLo_ = kNone;
    std::size_t lineHi_ = 0;
    std::size_t blockTotal_ = 0;
    std::size_t depth_ = 0;
    std::uint32_t nextId_ = 0;
    std::uint32_t diagSeen_ = 0;
    bool lastTextOpen_ = false;
    bool aborted_ = false;
    bool failed_ = false;
};

// ---------------------------------------------------------- C trampolines

template <typename F>
int guarded(void* user, F&& f) {
    auto* b = static_cast<Builder*>(user);
    try {
        return f(*b);
    } catch (const std::bad_alloc&) {
        b->markFailed();
        b->diagOnce(DiagnosticCode::OutOfMemory, DiagnosticSeverity::Error, "out of memory while parsing");
        return 1;
    } catch (...) {
        b->markFailed();
        b->diagOnce(DiagnosticCode::InternalError, DiagnosticSeverity::Error, "internal error while parsing");
        return 1;
    }
}

int onEnterBlock(MD_BLOCKTYPE t, void* d, void* u) { return guarded(u, [&](Builder& b) { return b.enterBlock(t, d); }); }
int onLeaveBlock(MD_BLOCKTYPE t, void*, void* u) { return guarded(u, [&](Builder& b) { return b.leaveBlock(t); }); }
int onEnterSpan(MD_SPANTYPE t, void* d, void* u) { return guarded(u, [&](Builder& b) { return b.enterSpan(t, d); }); }
int onLeaveSpan(MD_SPANTYPE t, void*, void* u) { return guarded(u, [&](Builder& b) { return b.leaveSpan(t); }); }
int onText(MD_TEXTTYPE t, const MD_CHAR* p, MD_SIZE n, void* u) {
    return guarded(u, [&](Builder& b) { return b.text(t, p, n); });
}

} // namespace

MarkdownDocument Md4cMarkdownParser::parse(std::string_view source, const ParseLimits& limits) const noexcept {
    MarkdownDocument doc;
    try {
        if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
            static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
            source.remove_prefix(3);
            doc.sourceOffsetBase = 3;
        }
        const std::size_t hardCap = static_cast<std::size_t>(std::numeric_limits<int>::max());
        const std::size_t cap = std::min(limits.maxSourceBytes, hardCap);
        if (source.size() > cap) {
            std::size_t cut = utf8::snapToBoundary(source, cap);
            doc.diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::SourceTruncated,
                                       "source exceeds the size limit and was truncated", {cut, source.size()}});
            source = source.substr(0, cut);
        }
        doc.sourceLength = source.size();

        Builder builder(source, limits, doc);
        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS;
        parser.enter_block = &onEnterBlock;
        parser.leave_block = &onLeaveBlock;
        parser.enter_span = &onEnterSpan;
        parser.leave_span = &onLeaveSpan;
        parser.text = &onText;
        parser.debug_log = nullptr;
        parser.syntax = nullptr;
        const int rc = md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, &builder);
        if (rc != 0 && !builder.aborted() && !builder.failed()) {
            builder.diagOnce(DiagnosticCode::InternalError, DiagnosticSeverity::Error,
                             "the Markdown parser reported a failure; the document may be partial");
        }
        builder.finish();
    } catch (const std::bad_alloc&) {
        doc.diagnostics.push_back({DiagnosticSeverity::Error, DiagnosticCode::OutOfMemory, "out of memory while parsing", {}});
    } catch (...) {
        doc.diagnostics.push_back({DiagnosticSeverity::Error, DiagnosticCode::InternalError, "internal error while parsing", {}});
    }
    return doc;
}

} // namespace rivet::markdown
