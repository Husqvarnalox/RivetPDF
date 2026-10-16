// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownEditing.hpp"

#include "markdown/MarkdownCaret.hpp"
#include "markdown/Utf8.hpp"

#include <algorithm>
#include <limits>

namespace rivet::markdown::editing {

namespace {

constexpr std::size_t kNpos = std::string_view::npos;

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isWordByte(char c) { return isAlnum(c) || static_cast<unsigned char>(c) >= 0x80; }

std::size_t lineStartOf(std::string_view s, std::size_t offset) {
    offset = std::min(offset, s.size());
    const std::size_t nl = offset == 0 ? kNpos : s.rfind('\n', offset - 1);
    return nl == kNpos ? 0 : nl + 1;
}

std::size_t lineEndOf(std::string_view s, std::size_t offset) {
    const std::size_t nl = s.find('\n', std::min(offset, s.size()));
    return nl == kNpos ? s.size() : nl;
}

std::string rstrip(std::string_view s) {
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return std::string(s);
}

// A line holding only quote markers and spaces (">", "> >").
bool isQuoteOnlyLine(std::string_view s, std::size_t lineStart) {
    for (std::size_t i = lineStart; i < s.size() && s[i] != '\n'; ++i) {
        if (!isSpace(s[i]) && s[i] != '>') return false;
    }
    return true;
}

bool emphasisLike(InlineKind k) { return k == InlineKind::Emphasis || k == InlineKind::Strong || k == InlineKind::Strike; }
bool isSpan(InlineKind k) { return emphasisLike(k) || k == InlineKind::Code || k == InlineKind::Link; }

Edit noChange(std::size_t selBegin, std::size_t selEnd) {
    Edit e;
    e.offset = selBegin;
    e.anchor = selBegin;
    e.caret = selEnd;
    return e;
}

Edit insertAt(std::size_t offset, std::string text) {
    Edit e;
    e.offset = offset;
    e.caret = e.anchor = offset + text.size();
    e.insert = std::move(text);
    return e;
}

// Where a backslash must go so that `rest`, now at the start of a line,
// stays literal text (kNpos: nothing to escape).
std::size_t lineStartEscapeOffset(std::string_view rest) {
    std::size_t i = 0;
    while (i < rest.size() && i < 3 && rest[i] == ' ') ++i;
    if (i >= rest.size()) return kNpos;
    const char c = rest[i];
    const auto followedByBlank = [&](std::size_t j) { return j >= rest.size() || isSpace(rest[j]) || rest[j] == '\n'; };
    const auto onlyCharAndSpaces = [&](char ch, std::size_t min) {
        std::size_t n = 0;
        for (std::size_t j = i; j < rest.size() && rest[j] != '\n'; ++j) {
            if (rest[j] == ch) ++n;
            else if (!isSpace(rest[j])) return false;
        }
        return n >= min;
    };
    switch (c) {
    case '#': {
        std::size_t j = i;
        while (j < rest.size() && rest[j] == '#') ++j;
        return (j - i <= 6 && followedByBlank(j)) ? i : kNpos;
    }
    case '>': return i;
    case '-':
    case '+':
    case '*':
        if (followedByBlank(i + 1)) return i;
        if (c != '+' && onlyCharAndSpaces(c, 3)) return i;
        return kNpos;
    case '=': return onlyCharAndSpaces('=', 1) ? i : kNpos;
    case '_': return onlyCharAndSpaces('_', 3) ? i : kNpos;
    case '`':
    case '~': {
        std::size_t j = i;
        while (j < rest.size() && rest[j] == c) ++j;
        return j - i >= 3 ? i : kNpos;
    }
    case '<':
        if (i + 1 < rest.size() && (isAlnum(rest[i + 1]) || rest[i + 1] == '/' || rest[i + 1] == '!' || rest[i + 1] == '?')) {
            return i;
        }
        return kNpos;
    default: break;
    }
    if (isDigit(c)) {
        std::size_t j = i;
        while (j < rest.size() && isDigit(rest[j]) && j - i < 10) ++j;
        if (j - i <= 9 && j < rest.size() && (rest[j] == '.' || rest[j] == ')') && followedByBlank(j + 1)) return j;
    }
    return kNpos;
}

// ------------------------------------------------------------ doc walking

bool containsInclusive(SourceRange r, std::size_t offset) { return offset >= r.start && offset <= r.end; }

void collectSpans(const std::vector<Inline>& inlines, std::size_t offset, Context& ctx, int depth) {
    if (depth > 64) return;
    for (const Inline& in : inlines) {
        if (!isSpan(in.kind)) continue;
        const SourceRange content = inlineContent(in);
        if (!containsInclusive(content, offset)) continue;
        if (offset == content.end && content.end == in.range.end && in.kind != InlineKind::Code) continue;
        ctx.spans.push_back(&in);
        if (in.kind == InlineKind::Code) {
            ctx.inCodeSpan = true;
            return;
        }
        collectSpans(in.children, offset, ctx, depth + 1);
        return;
    }
}

bool walkBlocks(const std::vector<Block>& blocks, std::size_t offset, Context& ctx, int depth) {
    if (depth > 64) return false;
    for (const Block& b : blocks) {
        if (!containsInclusive(b.range, offset)) continue;
        switch (b.kind) {
        case BlockKind::Quote:
            ctx.inQuote = true;
            walkBlocks(b.children, offset, ctx, depth + 1);
            return true;
        case BlockKind::List:
            for (std::size_t i = 0; i < b.items.size(); ++i) {
                const ListItem& item = b.items[i];
                if (!containsInclusive(item.range, offset)) continue;
                ctx.item = &item;
                ctx.list = &b;
                ctx.itemIndex = i;
                walkBlocks(item.children, offset, ctx, depth + 1);
                return true;
            }
            return true;
        default:
            ctx.leaf = &b;
            ctx.island = isReadOnlyIsland(b);
            if (b.kind == BlockKind::CodeBlock && !ctx.island && !b.textSpans.empty()) {
                ctx.editableCode = offset >= b.textSpans.front().source && offset <= b.textSpans.back().sourceEnd();
            }
            if (b.kind == BlockKind::Paragraph || b.kind == BlockKind::Heading) collectSpans(b.inlines, offset, ctx, 0);
            return true;
        }
    }
    return false;
}

struct SpanInfo {
    SourceRange range;
    SourceRange content;
    bool emphasis = false;
};

void gatherSpans(const std::vector<Inline>& inlines, SourceRange window, std::vector<SpanInfo>& out, int depth) {
    if (depth > 64) return;
    for (const Inline& in : inlines) {
        if (in.range.end < window.start || in.range.start > window.end) continue;
        if (isSpan(in.kind)) {
            out.push_back(SpanInfo{in.range, inlineContent(in), emphasisLike(in.kind)});
            if (in.kind != InlineKind::Code) gatherSpans(in.children, window, out, depth + 1);
        }
    }
}

void gatherBlockSpans(const std::vector<Block>& blocks, SourceRange window, std::vector<SpanInfo>& out, int depth) {
    if (depth > 64) return;
    for (const Block& b : blocks) {
        if (b.range.end < window.start || b.range.start > window.end) continue;
        switch (b.kind) {
        case BlockKind::Quote: gatherBlockSpans(b.children, window, out, depth + 1); break;
        case BlockKind::List:
            for (const ListItem& item : b.items) gatherBlockSpans(item.children, window, out, depth + 1);
            break;
        case BlockKind::Paragraph:
        case BlockKind::Heading: gatherSpans(b.inlines, window, out, 0); break;
        default: break;
        }
    }
}

// --------------------------------------------------------------- prefixes

// The quote prefix to repeat on a new line: the current line's, or (lazy
// continuation lines have none) the one of the block's first line.
std::string quotePrefixFor(std::string_view s, const LinePrefix& cur, const Context& ctx) {
    std::string q(s.substr(cur.lineStart, cur.quoteEnd - cur.lineStart));
    if (q.empty() && ctx.inQuote && ctx.leaf != nullptr) {
        const LinePrefix first = scanLinePrefix(s, ctx.leaf->range.start);
        q.assign(s.substr(first.lineStart, first.quoteEnd - first.lineStart));
    }
    if (!q.empty() && q.back() == '>') q.push_back(' ');
    return q;
}

// Prefix of a continuation line inside the current container: quote markers
// plus spaces up to the list item's content column.
std::string continuationPrefix(std::string_view s, const LinePrefix& cur, const Context& ctx) {
    std::string q = quotePrefixFor(s, cur, ctx);
    if (ctx.item != nullptr) {
        const LinePrefix first = scanLinePrefix(s, ctx.item->range.start);
        q.append(first.markerEnd - first.quoteEnd, ' ');
    }
    return q;
}

std::string newItemPrefix(std::string_view s, const ListItem& item) {
    const LinePrefix p = scanLinePrefix(s, item.range.start);
    std::string out(s.substr(p.lineStart, p.markerStart - p.lineStart));
    if (p.marker == LinePrefix::Marker::Ordered) {
        out += std::to_string(p.number + 1);
        out.push_back(p.delimiter);
    } else {
        out.push_back(p.marker == LinePrefix::Marker::Bullet ? p.bullet : '-');
    }
    out.push_back(' ');
    if (p.task) out += "[ ] ";
    return out;
}

// Leading whitespace of a code line up to `limit`, quote markers included.
std::string codeLineLead(std::string_view s, std::size_t lineStart, std::size_t limit) {
    std::size_t i = lineStart;
    while (i < limit && i < s.size() && (isSpace(s[i]) || s[i] == '>')) ++i;
    return std::string(s.substr(lineStart, i - lineStart));
}

// Inserts `breakText` at p, removing the whitespace that would start the new
// line and escaping the remainder when it would start with markup.
Edit splitAt(std::string_view s, std::size_t p, const std::string& breakText) {
    const std::size_t le = lineEndOf(s, p);
    std::size_t ws = p;
    while (ws < le && isSpace(s[ws])) ++ws;
    Edit e;
    e.offset = p;
    e.insert = breakText;
    const std::size_t esc = lineStartEscapeOffset(s.substr(ws, le - ws));
    if (esc == kNpos) {
        e.removeLength = ws - p;
    } else {
        e.removeLength = ws + esc - p;
        e.insert.append(s.substr(ws, esc));
        e.insert.push_back('\\');
    }
    e.caret = e.anchor = p + breakText.size();
    return e;
}

// Opens a blank line above the block whose first line starts at lineStart; the
// caret stays on it.
Edit openAbove(std::string_view s, const LinePrefix& first) {
    const std::string q(s.substr(first.lineStart, first.quoteEnd - first.lineStart));
    std::string qs = q;
    if (!qs.empty() && qs.back() == '>') qs.push_back(' ');
    Edit e;
    e.offset = first.lineStart;
    e.insert = qs + "\n" + rstrip(q) + "\n";
    e.caret = e.anchor = first.lineStart + qs.size();
    return e;
}

std::string normalizePlain(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if ((static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') || c == 0x7F) {
            continue;
        } else {
            out.push_back(c);
        }
    }
    return utf8::sanitize(out);
}

} // namespace

static Edit deleteRangeImpl(std::string_view source, const MarkdownDocument* doc, std::size_t begin, std::size_t end,
                     bool keepSpans);

namespace {

// The source with a selection removed, and where the caret ends up.
struct Prepared {
    std::string storage;
    std::string_view text;
    std::size_t at = 0;
    bool replaced = false;
};
Prepared prepare(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd,
                 bool keepSpans = false) {
    Prepared p;
    if (selBegin == selEnd) {
        p.text = source;
        p.at = selBegin;
        return p;
    }
    const Edit del = deleteRangeImpl(source, doc, selBegin, selEnd, keepSpans);
    p.storage = applied(source, del);
    p.text = p.storage;
    p.at = del.caret;
    p.replaced = true;
    return p;
}

Edit finish(std::string_view source, const Prepared& prep, const Edit& inner) {
    if (!prep.replaced) return inner;
    const std::string after = applied(prep.text, inner);
    return diffEdit(source, after, inner.anchor, inner.caret);
}

bool entityBefore(std::string_view before) {
    std::size_t i = before.size();
    std::size_t n = 0;
    while (i > 0 && isAlnum(before[i - 1]) && n < 32) {
        --i;
        ++n;
    }
    if (n == 0 || i == 0) return false;
    if (before[i - 1] == '#') --i;
    if (i == 0 || before[i - 1] != '&') return false;
    return i < 2 || before[i - 2] != '\\';
}

} // namespace

// ================================================================== basics

std::string applied(std::string_view source, const Edit& edit) {
    std::string out;
    const std::size_t offset = std::min(edit.offset, source.size());
    const std::size_t remove = std::min(edit.removeLength, source.size() - offset);
    out.reserve(source.size() - remove + edit.insert.size());
    out.append(source.substr(0, offset));
    out.append(edit.insert);
    out.append(source.substr(offset + remove));
    return out;
}

Edit diffEdit(std::string_view before, std::string_view after, std::size_t anchor, std::size_t caret) {
    std::size_t prefix = 0;
    const std::size_t common = std::min(before.size(), after.size());
    while (prefix < common && before[prefix] == after[prefix]) ++prefix;
    while (prefix > 0 && prefix < before.size() && (static_cast<unsigned char>(before[prefix]) & 0xC0) == 0x80) --prefix;
    std::size_t suffix = 0;
    while (suffix < common - prefix && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    while (suffix > 0 && (static_cast<unsigned char>(before[before.size() - suffix]) & 0xC0) == 0x80) --suffix;
    Edit e;
    e.offset = prefix;
    e.removeLength = before.size() - prefix - suffix;
    e.insert.assign(after.substr(prefix, after.size() - prefix - suffix));
    e.anchor = anchor;
    e.caret = caret;
    return e;
}

LinePrefix scanLinePrefix(std::string_view s, std::size_t offset) {
    LinePrefix p;
    p.lineStart = lineStartOf(s, offset);
    p.lineEnd = lineEndOf(s, p.lineStart);
    const std::size_t le = p.lineEnd;
    std::size_t i = p.lineStart;
    for (;;) {
        std::size_t j = i;
        std::size_t spaces = 0;
        while (j < le && s[j] == ' ' && spaces < 3) {
            ++j;
            ++spaces;
        }
        if (j < le && s[j] == '>') {
            ++j;
            if (j < le && isSpace(s[j])) ++j;
            i = j;
            continue;
        }
        break;
    }
    p.quoteEnd = i;
    std::size_t k = i;
    while (k < le && isSpace(s[k])) ++k;
    const auto blankAfter = [&](std::size_t j) { return j >= le || isSpace(s[j]); };
    std::size_t afterMarker = kNpos;
    if (k < le && (s[k] == '-' || s[k] == '+' || s[k] == '*') && blankAfter(k + 1)) {
        p.marker = LinePrefix::Marker::Bullet;
        p.bullet = s[k];
        afterMarker = k + 1;
    } else if (k < le && isDigit(s[k])) {
        std::size_t j = k;
        unsigned long n = 0;
        while (j < le && isDigit(s[j]) && j - k < 9) {
            n = n * 10 + static_cast<unsigned long>(s[j] - '0');
            ++j;
        }
        if (j < le && (s[j] == '.' || s[j] == ')') && blankAfter(j + 1) && (j >= le || !isDigit(s[j]))) {
            p.marker = LinePrefix::Marker::Ordered;
            p.number = n;
            p.delimiter = s[j];
            afterMarker = j + 1;
        }
    }
    if (afterMarker != kNpos) {
        p.markerStart = k;
        std::size_t j = afterMarker;
        std::size_t n = 0;
        while (j < le && isSpace(s[j]) && n < 4) {
            ++j;
            ++n;
        }
        p.markerEnd = j;
    } else {
        p.markerStart = p.markerEnd = i;
    }
    p.taskEnd = p.markerEnd;
    if (p.marker != LinePrefix::Marker::None && p.markerEnd + 3 <= le && s[p.markerEnd] == '[' &&
        s[p.markerEnd + 2] == ']' &&
        (s[p.markerEnd + 1] == ' ' || s[p.markerEnd + 1] == 'x' || s[p.markerEnd + 1] == 'X') &&
        (p.markerEnd + 3 == le || isSpace(s[p.markerEnd + 3]))) {
        p.task = true;
        p.taskChecked = s[p.markerEnd + 1] != ' ';
        p.taskEnd = p.markerEnd + 3;
        if (p.taskEnd < le && isSpace(s[p.taskEnd])) ++p.taskEnd;
    }
    p.headingEnd = p.taskEnd;
    {
        std::size_t j = p.taskEnd;
        std::size_t spaces = 0;
        while (j < le && s[j] == ' ' && spaces < 3) {
            ++j;
            ++spaces;
        }
        std::size_t h = j;
        while (h < le && s[h] == '#') ++h;
        if (h > j && h - j <= 6 && (h >= le || isSpace(s[h]))) {
            p.headingLevel = static_cast<int>(h - j);
            while (h < le && isSpace(s[h])) ++h;
            p.headingEnd = h;
        }
    }
    std::size_t c = p.headingEnd;
    while (c < le && isSpace(s[c])) ++c;
    p.contentStart = c;
    return p;
}

SourceRange inlineContent(const Inline& span) {
    if (span.kind == InlineKind::Code) {
        if (!span.spans.empty()) return {span.spans.front().source, span.spans.back().sourceEnd()};
        return span.range;
    }
    if (!span.children.empty()) return {span.children.front().range.start, span.children.back().range.end};
    return {span.range.start, span.range.start};
}

Context contextAt(const MarkdownDocument* doc, std::size_t offset) {
    Context ctx;
    if (doc != nullptr) walkBlocks(doc->blocks, offset, ctx, 0);
    return ctx;
}

bool atLineContentStart(std::string_view source, std::size_t offset) {
    const LinePrefix p = scanLinePrefix(source, offset);
    return offset <= p.contentStart;
}

// ================================================================== typing

std::string escapeTyped(std::string_view source, std::size_t offset, std::string_view typed, bool lineStart,
                        bool inHeading) {
    std::string out;
    out.reserve(typed.size() + 4);
    offset = std::min(offset, source.size());
    // Digits typed so far at the start of the line ("12" before a ".").
    long digits = -1;
    if (lineStart) {
        digits = 0;
    } else {
        const LinePrefix p = scanLinePrefix(source, offset);
        if (offset >= p.contentStart) {
            const std::string_view seg = source.substr(p.contentStart, offset - p.contentStart);
            if (!seg.empty() && seg.size() <= 9 && std::all_of(seg.begin(), seg.end(), isDigit)) {
                digits = static_cast<long>(seg.size());
            }
        }
    }
    const std::size_t lookback = offset > 48 ? offset - 48 : 0;
    std::string recent(source.substr(lookback, offset - lookback));
    for (std::size_t i = 0; i < typed.size();) {
        const utf8::Decoded d = utf8::decode(typed, i);
        const std::string_view ch = typed.substr(i, d.length);
        const bool first = i == 0;
        const bool ls = lineStart && first;
        i += d.length;
        if (d.length != 1) {
            out.append(ch);
            recent.append(ch);
            digits = -1;
            continue;
        }
        const char c = ch[0];
        char next = 0;
        if (i < typed.size()) next = typed[i];
        else if (offset < source.size()) next = source[offset];
        bool escape = false;
        std::string replacement;
        switch (c) {
        case '\\':
        case '`':
        case '*':
        case '_':
        case '[':
        case ']':
        case '<':
        case '~': escape = true; break;
        case '>':
        case '-':
        case '+':
        case '=': escape = ls; break;
        case '#': escape = ls || inHeading; break;
        case '&': escape = isAlnum(next) || next == '#'; break;
        case ';': escape = entityBefore(recent); break;
        case '.':
        case ')': escape = digits > 0 && digits <= 9; break;
        case ' ':
            if (ls) replacement = "&#32;";
            break;
        case '\t':
            if (ls) replacement = "&#9;";
            break;
        default: break;
        }
        if (!replacement.empty()) {
            out += replacement;
        } else {
            if (escape) out.push_back('\\');
            out.push_back(c);
        }
        recent.push_back(c);
        if (digits >= 0 && isDigit(c)) {
            ++digits;
        } else {
            digits = -1;
        }
    }
    return out;
}

Edit insertText(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd,
                std::string_view typed) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    selEnd = std::min(selEnd, source.size());
    selBegin = std::min(selBegin, selEnd);
    const std::string text = normalizePlain(typed);
    if (text.find('\n') != std::string::npos) return pastePlainText(source, doc, selBegin, selEnd, text);
    if (text.empty() && selBegin == selEnd) return noChange(selBegin, selEnd);
    const Context ctx = contextAt(doc, selBegin);
    if (ctx.island) return noChange(selBegin, selEnd);
    const Prepared prep = prepare(source, doc, selBegin, selEnd, true);
    std::size_t p = prep.at;
    Edit inner;
    if (ctx.editableCode || ctx.inCodeSpan) {
        inner = insertAt(p, text);
    } else {
        const bool ls = atLineContentStart(prep.text, p);
        const bool heading = ctx.leaf != nullptr && ctx.leaf->kind == BlockKind::Heading;
        // Whitespace typed at an emphasis edge goes outside the delimiters
        // ("**bold **" would no longer be emphasis).
        if (!prep.replaced && !text.empty() && !ls) {
            if (isSpace(text.front())) {
                std::size_t to = p;
                for (const Inline* span : ctx.spans) {
                    if (emphasisLike(span->kind) && inlineContent(*span).end == p) to = std::max(to, span->range.end);
                }
                p = to;
            } else if (isSpace(text.back())) {
                std::size_t to = p;
                for (const Inline* span : ctx.spans) {
                    if (emphasisLike(span->kind) && inlineContent(*span).start == p) {
                        to = std::min(to, span->range.start);
                    }
                }
                p = to;
            }
        }
        inner = insertAt(p, escapeTyped(prep.text, p, text, ls, heading));
    }
    return finish(source, prep, inner);
}

// ================================================================ deleting

Edit deleteRange(std::string_view source, const MarkdownDocument* doc, std::size_t begin, std::size_t end) {
    return deleteRangeImpl(source, doc, begin, end, false);
}

static Edit deleteRangeImpl(std::string_view source, const MarkdownDocument* doc, std::size_t begin, std::size_t end,
                     bool keepSpans) {
    if (begin > end) std::swap(begin, end);
    end = std::min(end, source.size());
    begin = std::min(begin, end);
    if (begin == end) return noChange(begin, end);
    std::vector<SpanInfo> spans;
    if (doc != nullptr) gatherBlockSpans(doc->blocks, SourceRange{begin, end}, spans, 0);

    // 1. A span whose whole content goes away takes its delimiters along
    //    (unless typing replaces it: the new text keeps the formatting).
    for (bool changed = !keepSpans; changed;) {
        changed = false;
        for (const SpanInfo& s : spans) {
            const bool contentGone = !s.content.empty() && s.content.start >= begin && s.content.end <= end;
            const bool rangeIn = s.range.start >= begin && s.range.end <= end;
            if (contentGone && !rangeIn) {
                begin = std::min(begin, s.range.start);
                end = std::max(end, s.range.end);
                changed = true;
            }
        }
    }
    // 2. Delimiters of partly covered spans stay.
    std::vector<SourceRange> kept;
    for (const SpanInfo& s : spans) {
        const bool rangeIn = s.range.start >= begin && s.range.end <= end;
        const bool overlaps = s.range.start < end && s.range.end > begin;
        if (rangeIn || !overlaps) continue;
        const SourceRange open{s.range.start, s.content.start};
        const SourceRange close{s.content.end, s.range.end};
        if (!open.empty() && open.start >= begin && open.end <= end) kept.push_back(open);
        if (!close.empty() && close.start >= begin && close.end <= end) kept.push_back(close);
    }
    std::sort(kept.begin(), kept.end(), [](SourceRange a, SourceRange b) { return a.start < b.start; });
    std::string replacement;
    for (const SourceRange r : kept) replacement.append(source.substr(r.start, r.size()));

    Edit e;
    e.offset = begin;
    e.removeLength = end - begin;
    e.insert = replacement;
    e.anchor = e.caret = begin;
    if (!replacement.empty()) return e;

    // 3. Whitespace must not end up just inside an emphasis delimiter.
    for (const SpanInfo& s : spans) {
        if (!s.emphasis) continue;
        if (s.content.end == end && begin > s.content.start && isSpace(source[begin - 1])) {
            std::size_t ws = begin;
            while (ws > s.content.start && isSpace(source[ws - 1])) --ws;
            if (ws == s.content.start) continue;
            Edit w;
            w.offset = ws;
            w.removeLength = s.range.end - ws;
            w.insert.assign(source.substr(end, s.range.end - end));
            w.insert.append(source.substr(ws, begin - ws));
            w.anchor = w.caret = ws + w.insert.size();
            return w;
        }
        if (s.content.start == begin && end < s.content.end && isSpace(source[end])) {
            std::size_t we = end;
            while (we < s.content.end && isSpace(source[we])) ++we;
            if (we == s.content.end) continue;
            Edit w;
            w.offset = s.range.start;
            w.removeLength = we - s.range.start;
            w.insert.assign(source.substr(end, we - end));
            w.insert.append(source.substr(s.range.start, begin - s.range.start));
            w.anchor = w.caret = s.range.start + (we - end);
            return w;
        }
    }
    // 4. A line must not start with structural markup it did not have.
    if (atLineContentStart(source, begin)) {
        const std::size_t le = lineEndOf(source, end);
        const std::size_t esc = lineStartEscapeOffset(source.substr(end, le - end));
        if (esc != kNpos && !atLineContentStart(source, end)) {
            e.removeLength = end + esc - begin;
            e.insert.assign(source.substr(end, esc));
            e.insert.push_back('\\');
        }
    }
    return e;
}

// ------------------------------------------------------ one character

namespace {

// Layout blocks whose source lies in [from, to): islands in a gap.
void islandsIn(const MarkdownLayout& layout, std::size_t from, std::size_t to, bool& blocked,
               std::optional<SourceRange>& image) {
    for (const LayoutBlock& b : layout.blocks) {
        if (b.source.start < from || b.source.end > to || b.source.empty()) continue; // only blocks in between
        if (b.kind == LayoutBlockKind::Image && !image) {
            image = b.source;
        } else {
            blocked = true;
        }
    }
}

bool isCodeBlock(const MarkdownLayout& layout, std::size_t index) {
    return index < layout.blocks.size() && layout.blocks[index].kind == LayoutBlockKind::Code;
}

} // namespace

DeleteResult deleteCharacter(std::string_view source, const MarkdownDocument* doc, const CaretMap& map,
                             std::size_t caret, bool forward) {
    DeleteResult result;
    result.edit = noChange(caret, caret);
    const MarkdownLayout* layout = map.layout();
    const std::optional<TextPosition> here = map.positionOf(caret);
    if (layout == nullptr || !here) {
        // Nothing rendered: plain character deletion in the source.
        if (forward && caret < source.size()) {
            std::size_t e = caret + 1;
            while (e < source.size() && (static_cast<unsigned char>(source[e]) & 0xC0) == 0x80) ++e;
            result.edit = deleteRange(source, doc, caret, e);
        } else if (!forward && caret > 0) {
            result.edit = deleteRange(source, doc, utf8::snapToBoundary(source, caret - 1), caret);
        }
        return result;
    }
    const std::size_t hereSource = map.sourceOf(*here);
    // A caret off the rendered text (a blank line opened by Enter, an empty
    // list item): delete the gap up to the neighbouring rendered position.
    if (hereSource != caret) {
        std::optional<TextPosition> other = *here;
        if (!forward && hereSource > caret) other = map.previous(*here);
        if (forward && hereSource < caret) other = map.next(*here);
        std::size_t from = forward ? caret : (other ? map.sourceOf(*other) : 0);
        std::size_t to = forward ? (other ? map.sourceOf(*other) : source.size()) : caret;
        if (from > to) std::swap(from, to);
        if (from == to) return result;
        bool blocked = false;
        islandsIn(*layout, from, to, blocked, result.selectIsland);
        if (blocked || result.selectIsland) {
            result.blocked = blocked && !result.selectIsland;
            return result;
        }
        result.edit = deleteRange(source, doc, from, to);
        result.edit.anchor = result.edit.caret = forward ? from : result.edit.caret;
        return result;
    }
    TextPosition target = *here;
    if (forward) {
        const std::optional<TextPosition> next = map.next(*here);
        if (!next) {
            result.blocked = true;
            return result;
        }
        target = *next;
    }
    if (const std::optional<SourceRange> ch = map.charBefore(target)) {
        result.edit = deleteRange(source, doc, ch->start, ch->end);
        return result;
    }
    // Block boundary: join with the previous block unless something
    // non-editable sits in between.
    const std::optional<TextPosition> prev = forward ? std::optional<TextPosition>(*here) : map.previous(*here);
    if (!prev) {
        result.blocked = true;
        return result;
    }
    const std::size_t from = map.sourceOf(*prev);
    const std::size_t to = map.sourceOf(target);
    bool blocked = false;
    islandsIn(*layout, from, to, blocked, result.selectIsland);
    const std::size_t pb = map.blockOf(*prev);
    const std::size_t tb = map.blockOf(target);
    if (isCodeBlock(*layout, pb) || isCodeBlock(*layout, tb)) blocked = true;
    if (result.selectIsland) return result;
    if (blocked || to <= from) {
        result.blocked = true;
        return result;
    }
    result.edit = deleteRange(source, doc, from, to);
    return result;
}

// ================================================================== breaks

Edit insertParagraphBreak(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin,
                          std::size_t selEnd) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    selEnd = std::min(selEnd, source.size());
    selBegin = std::min(selBegin, selEnd);
    const Context ctx = contextAt(doc, selBegin);
    if (ctx.island) return noChange(selBegin, selEnd);
    const Prepared prep = prepare(source, doc, selBegin, selEnd);
    const std::string_view s = prep.text;
    const std::size_t p = prep.at;
    const LinePrefix cur = scanLinePrefix(s, p);
    Edit inner = noChange(p, p);

    if (ctx.editableCode) {
        inner = insertAt(p, "\n" + codeLineLead(s, cur.lineStart, p));
    } else if (ctx.leaf == nullptr) {
        if (ctx.item != nullptr && cur.marker != LinePrefix::Marker::None && cur.blank()) {
            // Empty list item: leave the list (a blank line ends it).
            const std::string q = quotePrefixFor(s, cur, ctx);
            inner.offset = cur.lineStart;
            inner.removeLength = cur.lineEnd - cur.lineStart;
            inner.insert = rstrip(q) + "\n" + q;
            inner.anchor = inner.caret = cur.lineStart + inner.insert.size();
        } else if (cur.quoteEnd > cur.lineStart && cur.blank()) {
            // Empty quote line: leave the quote.
            const std::size_t prevStart = cur.lineStart > 0 ? lineStartOf(s, cur.lineStart - 1) : cur.lineStart;
            const bool separated = cur.lineStart == 0 || isQuoteOnlyLine(s, prevStart);
            const std::string q(s.substr(cur.lineStart, cur.quoteEnd - cur.lineStart));
            inner.offset = cur.lineStart;
            inner.removeLength = cur.lineEnd - cur.lineStart;
            inner.insert = separated ? std::string() : rstrip(q) + "\n";
            inner.anchor = inner.caret = cur.lineStart + inner.insert.size();
        }
    } else if (ctx.leaf->kind != BlockKind::Paragraph && ctx.leaf->kind != BlockKind::Heading) {
        // Fence lines, rules: nothing to split.
    } else {
        const Block& leaf = *ctx.leaf;
        const LinePrefix first = scanLinePrefix(s, leaf.range.start);
        const bool atBlockStart = cur.lineStart == first.lineStart && p <= cur.contentStart && !cur.blank();
        if (leaf.kind == BlockKind::Heading) {
            const bool setext = lineStartOf(s, std::min(leaf.range.end, s.size())) != first.lineStart;
            if (atBlockStart) {
                inner = openAbove(s, first);
            } else if (setext && p >= cur.lineEnd) {
                const std::size_t underlineEnd = lineEndOf(s, std::min(leaf.range.end, s.size()));
                inner = insertAt(underlineEnd, "\n\n" + continuationPrefix(s, cur, ctx));
            } else {
                inner = splitAt(s, p, "\n\n" + continuationPrefix(s, cur, ctx));
            }
        } else if (ctx.item != nullptr) {
            inner = splitAt(s, p, "\n" + newItemPrefix(s, *ctx.item));
        } else if (atBlockStart) {
            inner = openAbove(s, first);
        } else if (ctx.inQuote) {
            const std::string q = quotePrefixFor(s, cur, ctx);
            inner = splitAt(s, p, "\n" + rstrip(q) + "\n" + q);
        } else {
            inner = splitAt(s, p, "\n\n" + continuationPrefix(s, cur, ctx));
        }
    }
    if (!inner.changesText() && !prep.replaced) return noChange(selBegin, selEnd);
    return finish(source, prep, inner);
}

Edit insertLineBreak(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    const Context ctx = contextAt(doc, selBegin);
    if (ctx.editableCode || ctx.leaf == nullptr || ctx.leaf->kind != BlockKind::Paragraph) {
        return insertParagraphBreak(source, doc, selBegin, selEnd);
    }
    const Prepared prep = prepare(source, doc, selBegin, std::min(selEnd, source.size()));
    const LinePrefix cur = scanLinePrefix(prep.text, prep.at);
    const Edit inner = splitAt(prep.text, prep.at, "\\\n" + continuationPrefix(prep.text, cur, ctx));
    return finish(source, prep, inner);
}

Edit pastePlainText(std::string_view source, const MarkdownDocument* doc, std::size_t selBegin, std::size_t selEnd,
                    std::string_view text) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    selEnd = std::min(selEnd, source.size());
    selBegin = std::min(selBegin, selEnd);
    const std::string clean = normalizePlain(text);
    if (clean.find('\n') == std::string::npos) return insertText(source, doc, selBegin, selEnd, clean);
    const Context ctx = contextAt(doc, selBegin);
    if (ctx.island) return noChange(selBegin, selEnd);
    const Prepared prep = prepare(source, doc, selBegin, selEnd);
    const std::string_view s = prep.text;
    const std::size_t p = prep.at;
    const LinePrefix cur = scanLinePrefix(s, p);
    std::string ins;
    if (ctx.editableCode || ctx.inCodeSpan) {
        const std::string lead = ctx.editableCode ? codeLineLead(s, cur.lineStart, p) : std::string();
        for (const char c : clean) {
            if (c == '\n') {
                ins.push_back(ctx.editableCode ? '\n' : ' ');
                ins += lead;
            } else {
                ins.push_back(c);
            }
        }
    } else {
        std::string brk;
        if (ctx.leaf != nullptr && ctx.leaf->kind == BlockKind::Heading) {
            brk = "\n\n" + continuationPrefix(s, cur, ctx);
        } else if (ctx.item != nullptr) {
            brk = "\n" + newItemPrefix(s, *ctx.item);
        } else if (ctx.inQuote) {
            const std::string q = quotePrefixFor(s, cur, ctx);
            brk = "\n" + rstrip(q) + "\n" + q;
        } else {
            brk = "\n\n";
        }
        const bool heading = ctx.leaf != nullptr && ctx.leaf->kind == BlockKind::Heading;
        bool firstLine = true;
        std::size_t pos = 0;
        while (pos <= clean.size()) {
            std::size_t nl = clean.find('\n', pos);
            if (nl == std::string::npos) nl = clean.size();
            const std::string_view line = std::string_view(clean).substr(pos, nl - pos);
            pos = nl + 1;
            if (firstLine) {
                ins += escapeTyped(s, p, line, atLineContentStart(s, p), heading);
                firstLine = false;
                continue;
            }
            std::string_view trimmed = line;
            while (!trimmed.empty() && isSpace(trimmed.front())) trimmed.remove_prefix(1);
            if (trimmed.empty()) continue;
            ins += brk;
            ins += escapeTyped({}, 0, trimmed, true, false);
        }
    }
    return finish(source, prep, insertAt(p, std::move(ins)));
}

// ============================================================== formatting

namespace {

struct Segment {
    std::size_t start = 0; // content [start, end) in the source
    std::size_t end = 0;
};

std::size_t runBefore(std::string_view s, std::size_t at, char c) {
    std::size_t n = 0;
    while (at > n && s[at - 1 - n] == c) ++n;
    return n;
}
std::size_t runAfter(std::string_view s, std::size_t at, char c) {
    std::size_t n = 0;
    while (at + n < s.size() && s[at + n] == c) ++n;
    return n;
}

// One segment's replacement: source [regionStart, regionEnd) becomes `text`,
// whose content lies at [contentStart, contentEnd) of `text`.
struct Rewrite {
    std::size_t regionStart = 0;
    std::size_t regionEnd = 0;
    std::string text;
    std::size_t contentStart = 0;
    std::size_t contentEnd = 0;
};

Rewrite wrap(std::string_view s, Segment g, std::string_view open, std::string_view close) {
    Rewrite r;
    r.regionStart = g.start;
    r.regionEnd = g.end;
    r.text.assign(open);
    r.contentStart = r.text.size();
    r.text.append(s.substr(g.start, g.end - g.start));
    r.contentEnd = r.text.size();
    r.text.append(close);
    return r;
}

Rewrite unwrapOuter(std::string_view s, Segment g, std::size_t left, std::size_t right) {
    Rewrite r;
    r.regionStart = g.start - left;
    r.regionEnd = g.end + right;
    r.text.assign(s.substr(g.start, g.end - g.start));
    r.contentStart = 0;
    r.contentEnd = r.text.size();
    return r;
}

Rewrite unwrapInner(std::string_view s, Segment g, std::size_t left, std::size_t right) {
    Rewrite r;
    r.regionStart = g.start;
    r.regionEnd = g.end;
    r.text.assign(s.substr(g.start + left, g.end - g.start - left - right));
    r.contentStart = 0;
    r.contentEnd = r.text.size();
    return r;
}

// Delimiter characters at the segment's own edges belong to the markup, not
// to the content (a line selected from its start begins with "**").
Segment shrink(std::string_view s, Segment g, std::string_view chars) {
    while (g.end > g.start && chars.find(s[g.start]) != std::string_view::npos) ++g.start;
    while (g.end > g.start && chars.find(s[g.end - 1]) != std::string_view::npos && s[g.end - 2] != '\\') --g.end;
    return g;
}

Rewrite toggleEmphasis(std::string_view s, Segment g, std::size_t width) {
    if (const Segment inner = shrink(s, g, "*_"); inner.end > inner.start) g = inner;
    // Delimiter character: what surrounds the segment, '*' by default.
    char c = '*';
    if (g.start > 0 && (s[g.start - 1] == '_' || s[g.start - 1] == '*') && g.end < s.size() && s[g.end] == s[g.start - 1]) {
        c = s[g.start - 1];
    }
    const std::size_t n = std::min(runBefore(s, g.start, c), runAfter(s, g.end, c));
    const auto has = [&](std::size_t count) { return width == 2 ? count >= 2 : (count % 2) == 1; };
    if (has(n)) return unwrapOuter(s, g, width, width);
    const char ic = s[g.start];
    if ((ic == '*' || ic == '_') && s[g.end - 1] == ic) {
        const std::size_t len = g.end - g.start;
        const std::size_t m = std::min({runAfter(s, g.start, ic), runBefore(s, g.end, ic), len / 2});
        if (has(m) && len > 2 * width) return unwrapInner(s, g, width, width);
    }
    const std::string d(width, '*');
    return wrap(s, g, d, d);
}

Rewrite toggleStrike(std::string_view s, Segment g) {
    if (const Segment inner = shrink(s, g, "~"); inner.end > inner.start) g = inner;
    const std::size_t n = std::min(runBefore(s, g.start, '~'), runAfter(s, g.end, '~'));
    if (n >= 2) return unwrapOuter(s, g, 2, 2);
    const std::size_t len = g.end - g.start;
    if (len > 4 && runAfter(s, g.start, '~') >= 2 && runBefore(s, g.end, '~') >= 2) return unwrapInner(s, g, 2, 2);
    return wrap(s, g, "~~", "~~");
}

Rewrite toggleCode(std::string_view s, Segment g) {
    const std::size_t n = std::min(runBefore(s, g.start, '`'), runAfter(s, g.end, '`'));
    if (n >= 1) return unwrapOuter(s, g, n, n);
    if (g.start > 1 && g.end + 1 < s.size() && s[g.start - 1] == ' ' && s[g.end] == ' ') {
        const std::size_t l = runBefore(s, g.start - 1, '`');
        if (l >= 1 && l == runAfter(s, g.end + 1, '`')) return unwrapOuter(s, g, l + 1, l + 1);
    }
    const std::size_t len = g.end - g.start;
    const std::size_t m = std::min(runAfter(s, g.start, '`'), runBefore(s, g.end, '`'));
    if (m >= 1 && len > 2 * m) {
        std::size_t left = m;
        std::size_t right = m;
        if (len > 2 * m + 2 && s[g.start + m] == ' ' && s[g.end - m - 1] == ' ') {
            ++left;
            ++right;
        }
        return unwrapInner(s, g, left, right);
    }
    std::size_t longest = 0;
    for (std::size_t i = g.start; i < g.end;) {
        const std::size_t r = runAfter(s, i, '`');
        if (r > 0) {
            longest = std::max(longest, r);
            i += r;
        } else {
            ++i;
        }
    }
    std::string fence(longest + 1, '`');
    const bool pad = s[g.start] == '`' || s[g.end - 1] == '`';
    return wrap(s, g, pad ? fence + " " : fence, pad ? " " + fence : fence);
}

Rewrite toggleLink(std::string_view s, Segment g, std::string_view url) {
    if (g.start > 0 && s[g.start - 1] == '[' && s.substr(g.end, 2) == "](") {
        int depth = 1;
        for (std::size_t i = g.end + 2; i < s.size() && s[i] != '\n'; ++i) {
            if (s[i] == '\\') {
                ++i;
                continue;
            }
            if (s[i] == '(') ++depth;
            if (s[i] == ')' && --depth == 0) return unwrapOuter(s, g, 1, i + 1 - g.end);
        }
    }
    return wrap(s, g, "[", "](" + std::string(url.empty() ? std::string_view("https://") : url) + ")");
}

} // namespace

Edit toggleInline(std::string_view source, std::size_t selBegin, std::size_t selEnd, InlineFormat format,
                  std::string_view url) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    selEnd = std::min(selEnd, source.size());
    selBegin = std::min(selBegin, selEnd);
    std::size_t a = selBegin;
    std::size_t b = selEnd;
    if (a == b) {
        while (a > 0 && isWordByte(source[a - 1])) --a;
        while (b < source.size() && isWordByte(source[b])) ++b;
        if (a == b) {
            if (format != InlineFormat::Link) return noChange(selBegin, selEnd);
            const std::string target(url.empty() ? std::string_view("https://") : url);
            Edit e;
            e.offset = a;
            e.insert = "[" + target + "](" + target + ")";
            e.anchor = a + 1;
            e.caret = a + 1 + target.size();
            return e;
        }
    }
    // Segments: one per line, without the line's container prefix and
    // without surrounding whitespace.
    std::vector<Segment> segments;
    for (std::size_t pos = a; pos < b;) {
        const std::size_t le = lineEndOf(source, pos);
        std::size_t s = pos;
        if (pos != a) s = std::max(s, scanLinePrefix(source, pos).contentStart); // later lines: skip their prefix
        std::size_t e = std::min(le, b);
        while (s < e && isSpace(source[s])) ++s;
        while (e > s && isSpace(source[e - 1])) --e;
        // Where the line (not the selection) cuts the segment, delimiters of
        // this format belong to the markup.
        const std::string_view marks = format == InlineFormat::Code            ? "`"
                                       : format == InlineFormat::Strikethrough ? "~"
                                       : format == InlineFormat::Link          ? "["
                                                                               : "*_";
        if (pos != a) {
            while (s < e && marks.find(source[s]) != std::string_view::npos) ++s;
            if (format == InlineFormat::Code && s < e && source[s] == ' ') ++s;
        }
        if (le < b) {
            if (format == InlineFormat::Link && e > s && source[e - 1] == ')') {
                const std::size_t close = source.substr(s, e - s).rfind("](");
                if (close != std::string_view::npos) e = s + close;
            }
            while (e > s && marks.find(source[e - 1]) != std::string_view::npos) --e;
            if (format == InlineFormat::Code && e > s && source[e - 1] == ' ') --e;
        }
        if (e > s) segments.push_back({s, e});
        pos = le + 1;
    }
    if (segments.empty()) return noChange(selBegin, selEnd);
    std::vector<Rewrite> rewrites;
    for (const Segment g : segments) {
        switch (format) {
        case InlineFormat::Bold: rewrites.push_back(toggleEmphasis(source, g, 2)); break;
        case InlineFormat::Italic: rewrites.push_back(toggleEmphasis(source, g, 1)); break;
        case InlineFormat::Strikethrough: rewrites.push_back(toggleStrike(source, g)); break;
        case InlineFormat::Code: rewrites.push_back(toggleCode(source, g)); break;
        case InlineFormat::Link: rewrites.push_back(toggleLink(source, g, url)); break;
        }
    }
    const std::size_t regionStart = rewrites.front().regionStart;
    const std::size_t regionEnd = rewrites.back().regionEnd;
    Edit e;
    e.offset = regionStart;
    e.removeLength = regionEnd - regionStart;
    std::size_t at = regionStart;
    for (std::size_t i = 0; i < rewrites.size(); ++i) {
        const Rewrite& r = rewrites[i];
        e.insert.append(source.substr(at, r.regionStart - at));
        const std::size_t base = regionStart + e.insert.size();
        if (i == 0) e.anchor = base + r.contentStart;
        if (i + 1 == rewrites.size()) e.caret = base + r.contentEnd;
        e.insert.append(r.text);
        at = r.regionEnd;
    }
    return e;
}

namespace {

bool isSetextUnderline(std::string_view s, std::size_t lineStart) {
    const std::size_t le = lineEndOf(s, lineStart);
    std::size_t i = lineStart;
    std::size_t spaces = 0;
    while (i < le && s[i] == ' ' && spaces < 3) {
        ++i;
        ++spaces;
    }
    if (i >= le || (s[i] != '=' && s[i] != '-')) return false;
    const char c = s[i];
    while (i < le && s[i] == c) ++i;
    while (i < le && isSpace(s[i])) ++i;
    return i == le;
}

} // namespace

Edit applyBlockFormat(std::string_view source, std::size_t selBegin, std::size_t selEnd, BlockFormat format) {
    if (selBegin > selEnd) std::swap(selBegin, selEnd);
    selEnd = std::min(selEnd, source.size());
    selBegin = std::min(selBegin, selEnd);
    const std::size_t firstLine = lineStartOf(source, selBegin);
    std::size_t lastLine = lineStartOf(source, selEnd);
    if (selEnd > selBegin && selEnd == lastLine && lastLine > firstLine) lastLine = lineStartOf(source, selEnd - 1);

    std::vector<LinePrefix> lines;
    for (std::size_t ls = firstLine;;) {
        lines.push_back(scanLinePrefix(source, ls));
        const std::size_t le = lines.back().lineEnd;
        if (ls >= lastLine || le >= source.size()) break;
        ls = le + 1;
    }
    const std::size_t regionStart = firstLine;
    std::size_t regionEnd = lines.back().lineEnd;

    const auto headingTarget = [&]() -> int {
        switch (format) {
        case BlockFormat::Heading1: return 1;
        case BlockFormat::Heading2: return 2;
        case BlockFormat::Heading3: return 3;
        case BlockFormat::Heading4: return 4;
        case BlockFormat::Heading5: return 5;
        case BlockFormat::Heading6: return 6;
        default: return 0;
        }
    };
    const bool heading = format <= BlockFormat::Heading6;
    const bool list = format == BlockFormat::BulletList || format == BlockFormat::NumberedList ||
                      format == BlockFormat::TaskList;
    const auto hasFormat = [&](const LinePrefix& p) {
        if (heading) return p.headingLevel == headingTarget();
        switch (format) {
        case BlockFormat::BulletList: return p.marker == LinePrefix::Marker::Bullet && !p.task;
        case BlockFormat::NumberedList: return p.marker == LinePrefix::Marker::Ordered;
        case BlockFormat::TaskList: return p.task;
        case BlockFormat::Quote: return p.quoteEnd > p.lineStart;
        default: return false;
        }
    };
    bool all = true;
    bool anyNonBlank = false;
    for (const LinePrefix& p : lines) {
        if (p.blank()) continue;
        anyNonBlank = true;
        all = all && hasFormat(p);
    }
    if (!anyNonBlank && format != BlockFormat::Quote) return noChange(selBegin, selEnd);
    const int level = heading ? ((all && headingTarget() > 0) ? 0 : headingTarget()) : 0;

    // Rebuild the region line by line, remembering where each line's content went.
    struct Mapped {
        std::size_t oldContent = 0;
        std::size_t newContent = 0;
        std::size_t oldLineStart = 0;
        std::size_t oldLineEnd = 0;
    };
    std::vector<Mapped> mapped;
    std::string out;
    unsigned long number = 0;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const LinePrefix& p = lines[li];
        const std::string_view lineText = source.substr(p.lineStart, p.lineEnd - p.lineStart);
        if (li > 0) out.push_back('\n');
        Mapped m;
        m.oldLineStart = p.lineStart;
        m.oldLineEnd = p.lineEnd;
        if (heading) {
            if (li > 0 && isSetextUnderline(source, p.lineStart) && !lines[li - 1].blank()) {
                out.pop_back(); // drop the underline and its line break
                m.oldContent = p.lineStart;
                m.newContent = regionStart + out.size();
                mapped.push_back(m);
                continue;
            }
            if (p.blank()) {
                out.append(lineText);
                m.oldContent = p.lineStart;
                m.newContent = regionStart + out.size() - lineText.size();
            } else {
                out.append(source.substr(p.lineStart, p.taskEnd - p.lineStart));
                if (level > 0) {
                    out.append(static_cast<std::size_t>(level), '#');
                    out.push_back(' ');
                }
                m.oldContent = p.contentStart;
                m.newContent = regionStart + out.size();
                out.append(source.substr(p.contentStart, p.lineEnd - p.contentStart));
            }
        } else if (list) {
            if (p.blank()) {
                out.append(lineText);
                m.oldContent = p.lineStart;
                m.newContent = regionStart + out.size() - lineText.size();
            } else {
                const std::string q(source.substr(p.lineStart, p.quoteEnd - p.lineStart));
                if (all) {
                    out.append(q);
                } else {
                    out.append(source.substr(p.lineStart, p.markerStart - p.lineStart));
                    if (format == BlockFormat::NumberedList) {
                        out.append(std::to_string(++number) + ". ");
                    } else {
                        out.append(format == BlockFormat::TaskList ? "- [ ] " : "- ");
                    }
                }
                std::size_t content = p.taskEnd;
                while (content < p.lineEnd && isSpace(source[content])) ++content;
                m.oldContent = content;
                m.newContent = regionStart + out.size();
                out.append(source.substr(content, p.lineEnd - content));
            }
        } else { // quote
            if (all) {
                std::size_t i = p.lineStart;
                while (i < p.lineEnd && source[i] == ' ' && i - p.lineStart < 3) ++i;
                if (i < p.lineEnd && source[i] == '>') {
                    std::size_t after = i + 1;
                    if (after < p.lineEnd && isSpace(source[after])) ++after;
                    out.append(source.substr(p.lineStart, i - p.lineStart));
                    m.oldContent = after;
                    m.newContent = regionStart + out.size();
                    out.append(source.substr(after, p.lineEnd - after));
                } else {
                    m.oldContent = p.lineStart;
                    m.newContent = regionStart + out.size();
                    out.append(lineText);
                }
            } else {
                out.append(lineText.empty() ? ">" : "> ");
                m.oldContent = p.lineStart;
                m.newContent = regionStart + out.size();
                out.append(lineText);
            }
        }
        mapped.push_back(m);
    }
    // A setext underline right after the region goes too (the heading is ATX now).
    if (heading && !lines.back().blank() && regionEnd < source.size()) {
        const std::size_t next = regionEnd + 1;
        if (isSetextUnderline(source, next)) regionEnd = lineEndOf(source, next);
    }
    const auto map = [&](std::size_t offset) {
        for (const Mapped& m : mapped) {
            if (offset < m.oldLineStart || offset > m.oldLineEnd) continue;
            return offset >= m.oldContent ? m.newContent + (offset - m.oldContent) : m.newContent;
        }
        return regionStart + out.size();
    };
    Edit e;
    e.offset = regionStart;
    e.removeLength = regionEnd - regionStart;
    e.anchor = map(selBegin);
    e.caret = map(selEnd);
    e.insert = std::move(out);
    return e;
}

} // namespace rivet::markdown::editing
