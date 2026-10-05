// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownLayout.hpp"

#include "markdown/Utf8.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace rivet::markdown {

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxCacheEntries = 400'000;
constexpr int kMaxInlineRecursion = 128;
constexpr int kMaxBlockRecursion = 256;

std::uint64_t styleKey(const TextStyle& s) {
    const auto size = std::bit_cast<std::uint32_t>(static_cast<float>(s.size));
    const std::uint64_t flags = (s.bold ? 1u : 0u) | (s.italic ? 2u : 0u) | (s.strike ? 4u : 0u) | (s.monospace ? 8u : 0u);
    return (static_cast<std::uint64_t>(s.kind) << 40) | (flags << 32) | size;
}

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

SourceRange unite(SourceRange a, SourceRange b) {
    if (a.empty() && a.start == 0 && a.end == 0) return b;
    return {std::min(a.start, b.start), std::max(a.end, b.end)};
}

} // namespace

// --------------------------------------------------------------- MeasureCache

double MeasureCache::width(const ITextMeasurer& m, std::string_view text, const TextStyle& style) {
    if (text.empty()) return 0.0;
    if (widths_.size() > kMaxCacheEntries) widths_.clear();
    std::string key;
    key.reserve(8 + text.size());
    const std::uint64_t k = styleKey(style);
    key.append(reinterpret_cast<const char*>(&k), sizeof k);
    key.append(text);
    const auto it = widths_.find(key);
    if (it != widths_.end()) return it->second;
    const double w = m.measure(text, style).width;
    widths_.emplace(std::move(key), w);
    return w;
}

StyleMetrics MeasureCache::styleMetrics(const ITextMeasurer& m, const TextStyle& style) {
    const std::uint64_t k = styleKey(style);
    const auto it = styles_.find(k);
    if (it != styles_.end()) return it->second;
    const TextMetrics sample = m.measure("Hg", style);
    StyleMetrics sm;
    sm.ascent = sample.ascent;
    sm.descent = sample.descent;
    sm.spaceWidth = m.measure(" ", style).width;
    styles_.emplace(k, sm);
    return sm;
}

void MeasureCache::clear() {
    widths_.clear();
    styles_.clear();
}

namespace {

// ------------------------------------------------------------------- engine

struct Token {
    enum class Type : std::uint8_t { Word, Space, HardBreak, Image };
    Type type = Type::Word;
    std::string_view text;
    TextStyle style;
    std::uint32_t link = 0;
    double width = 0.0;
    SourceRange src;
    bool exact = false; // src.size() == text.size() and maps 1:1
};

struct Ctx {
    double x = 0.0;
    double width = 0.0;
    bool quote = false;
    bool tight = false;
};

class Engine {
public:
    Engine(const MarkdownDocument& doc, double viewportWidth, const ITextMeasurer& m, const IImageSizeProvider* images,
           const Typography& t, MeasureCache& cache, MarkdownLayout& out)
        : doc_(doc), viewport_(viewportWidth), m_(m), images_(images), t_(t), cache_(cache), out_(out) {}

    void run() {
        out_.viewportWidth = viewport_;
        out_.blockIndexById.assign(doc_.blockCount, MarkdownLayout::kNoBlock);
        const double avail = std::max(t_.minContentWidth, viewport_ - 2.0 * t_.pageMarginX);
        Ctx ctx;
        ctx.x = t_.pageMarginX;
        ctx.width = avail;
        double y = t_.pageMarginY;
        layoutBlocks(doc_.blocks, ctx, y, 0);
        out_.contentHeight = y + t_.pageMarginY;
        out_.contentWidth = std::max(viewport_, maxRight_ + t_.pageMarginX);
    }

private:
    // ------------------------------------------------------------ styles
    TextStyle baseStyle(TextKind kind) const {
        TextStyle s;
        s.kind = kind;
        s.size = kind == TextKind::Table ? t_.tableSize : t_.bodySize;
        return s;
    }
    TextStyle headingStyle(int level) const {
        const int l = std::clamp(level, 1, 6);
        TextStyle s;
        s.kind = static_cast<TextKind>(static_cast<int>(TextKind::H1) + l - 1);
        s.bold = true;
        s.size = t_.headingSize[static_cast<std::size_t>(l - 1)];
        return s;
    }
    double lineHeightFor(double size, const StyleMetrics& sm) const {
        return std::max(size * t_.lineHeightFactor, sm.ascent + sm.descent);
    }

    // ------------------------------------------------------------ tokens
    std::uint32_t addLink(const std::string& url, const std::string& title) {
        out_.links.push_back({url, title});
        return static_cast<std::uint32_t>(out_.links.size());
    }
    void tokenizeText(const Inline& in, const TextStyle& style, std::uint32_t link, std::vector<Token>& toks) {
        const std::string_view t = in.text;
        const bool exact = in.range.size() == t.size();
        const StyleMetrics sm = cache_.styleMetrics(m_, style);
        std::size_t i = 0;
        while (i < t.size()) {
            const std::size_t a = i;
            const bool ws = isWs(t[i]);
            while (i < t.size() && isWs(t[i]) == ws) ++i;
            Token tok;
            tok.style = style;
            tok.link = link;
            tok.exact = exact;
            tok.src = exact ? SourceRange{in.range.start + a, in.range.start + i} : in.range;
            if (ws) {
                tok.type = Token::Type::Space;
                tok.text = " ";
                tok.width = sm.spaceWidth;
                tok.exact = exact && (i - a) == 1;
            } else {
                tok.type = Token::Type::Word;
                tok.text = t.substr(a, i - a);
                tok.width = cache_.width(m_, tok.text, style);
            }
            toks.push_back(tok);
        }
    }
    void tokenize(const std::vector<Inline>& inlines, const TextStyle& base, std::uint32_t link,
                  std::vector<Token>& toks, int depth) {
        if (depth > kMaxInlineRecursion) return;
        for (const Inline& in : inlines) {
            switch (in.kind) {
            case InlineKind::Text:
                tokenizeText(in, base, link, toks);
                break;
            case InlineKind::Emphasis: {
                TextStyle s = base;
                s.italic = true;
                tokenize(in.children, s, link, toks, depth + 1);
                break;
            }
            case InlineKind::Strong: {
                TextStyle s = base;
                s.bold = true;
                tokenize(in.children, s, link, toks, depth + 1);
                break;
            }
            case InlineKind::Strike: {
                TextStyle s = base;
                s.strike = true;
                tokenize(in.children, s, link, toks, depth + 1);
                break;
            }
            case InlineKind::Code: {
                TextStyle s = base;
                if (s.kind == TextKind::Body || s.kind == TextKind::Quote || s.kind == TextKind::Table) s.kind = TextKind::Code;
                s.monospace = true;
                s.size = base.size * (t_.codeSize / t_.bodySize);
                Inline tmp; // tokenizeText only needs text + range
                tmp.text = in.text;
                tmp.range = in.range;
                tokenizeText(tmp, s, link, toks);
                break;
            }
            case InlineKind::Link: {
                TextStyle s = base;
                s.kind = TextKind::Link;
                const std::uint32_t id = addLink(in.url, in.title);
                tokenize(in.children, s, id, toks, depth + 1);
                break;
            }
            case InlineKind::Image: {
                Token tok;
                tok.type = Token::Type::Image;
                tok.text = in.text.empty() ? std::string_view("image") : std::string_view(in.text);
                tok.style = base;
                tok.style.italic = true;
                tok.link = addLink(in.url, in.title);
                tok.width = cache_.width(m_, tok.text, tok.style);
                tok.src = in.range;
                toks.push_back(tok);
                break;
            }
            case InlineKind::SoftBreak: {
                Token tok;
                tok.type = Token::Type::Space;
                tok.text = " ";
                tok.style = base;
                tok.link = link;
                tok.width = cache_.styleMetrics(m_, base).spaceWidth;
                tok.src = in.range;
                toks.push_back(tok);
                break;
            }
            case InlineKind::HardBreak: {
                Token tok;
                tok.type = Token::Type::HardBreak;
                tok.style = base;
                tok.src = in.range;
                toks.push_back(tok);
                break;
            }
            }
        }
    }

    // -------------------------------------------------------------- flow
    struct FlowResult {
        double bottom = 0.0;
        double maxWidth = 0.0;
        bool produced = false;
    };

    // Places tokens into lines of at most `width` starting at (x, y).
    FlowResult flow(const std::vector<Token>& toks, double x, double y, double width, const TextStyle& base,
                    ColumnAlign align, LineEnd finalEnd) {
        FlowResult res;
        res.bottom = y;
        const double eps = 0.01;

        const auto runStartIdx = [&] { return static_cast<std::uint32_t>(out_.runs.size()); };
        std::uint32_t runStart = runStartIdx();
        double used = 0.0;
        double asc = 0.0;
        double desc = 0.0;
        double maxSize = 0.0;
        bool hasPending = false;
        Token pending;
        bool lineHasContent = false;

        const auto noteStyle = [&](const TextStyle& st) {
            const StyleMetrics sm = cache_.styleMetrics(m_, st);
            asc = std::max(asc, sm.ascent);
            desc = std::max(desc, sm.descent);
            maxSize = std::max(maxSize, st.size);
        };

        const auto place = [&](std::string_view text, const TextStyle& st, std::uint32_t link, bool image, double w,
                               SourceRange src) {
            if (out_.runs.size() > runStart) {
                LayoutRun& last = out_.runs.back();
                if (last.style == st && last.link == link && !last.isImage && !image) {
                    last.text.append(text);
                    last.width += w;
                    last.source = unite(last.source, src);
                    used += w;
                    noteStyle(st);
                    lineHasContent = true;
                    return;
                }
            }
            LayoutRun& r = out_.runs.emplace_back();
            r.text.assign(text);
            r.style = st;
            r.link = link;
            r.isImage = image;
            r.x = x + used;
            r.width = w;
            r.source = src;
            used += w;
            noteStyle(st);
            lineHasContent = true;
        };

        const auto finishLine = [&](LineEnd end, bool force) {
            if (!lineHasContent && !force) return;
            if (!lineHasContent) noteStyle(base);
            const StyleMetrics fallback = cache_.styleMetrics(m_, base);
            if (asc + desc <= 0.0) {
                asc = fallback.ascent;
                desc = fallback.descent;
                maxSize = base.size;
            }
            const double height = std::max(maxSize * t_.lineHeightFactor, asc + desc);
            const double baseline = y + (height - (asc + desc)) * 0.5 + asc;
            double shift = 0.0;
            if (align == ColumnAlign::Center) shift = std::max(0.0, (width - used) * 0.5);
            else if (align == ColumnAlign::Right) shift = std::max(0.0, width - used);
            const auto lineIndex = static_cast<std::uint32_t>(out_.lines.size());
            const std::uint32_t runEnd = runStartIdx();
            for (std::uint32_t i = runStart; i < runEnd; ++i) {
                LayoutRun& r = out_.runs[i];
                r.baseline = baseline;
                r.x += shift;
                r.line = lineIndex;
                r.exactSource = !r.source.empty() && r.source.size() == r.text.size();
            }
            LayoutLine& ln = out_.lines.emplace_back();
            ln.top = y;
            ln.height = height;
            ln.baseline = baseline;
            ln.x = x + shift;
            ln.width = used;
            ln.runBegin = runStart;
            ln.runEnd = runEnd;
            ln.end = end;
            res.maxWidth = std::max(res.maxWidth, used);
            res.produced = true;
            y += height;
            res.bottom = y;
            runStart = runEnd;
            used = 0.0;
            asc = desc = maxSize = 0.0;
            hasPending = false;
            lineHasContent = false;
        };

        const auto placeLongWord = [&](const Token& tok) {
            // The word is wider than a whole line: break it between code points.
            std::size_t fragStart = 0;
            double fragWidth = 0.0;
            const std::string_view text = tok.text;
            std::size_t i = 0;
            const auto flush = [&](std::size_t end) {
                if (end > fragStart) {
                    SourceRange src = tok.src;
                    if (tok.exact) src = {tok.src.start + fragStart, tok.src.start + end};
                    place(text.substr(fragStart, end - fragStart), tok.style, tok.link, tok.type == Token::Type::Image,
                          fragWidth, src);
                }
                fragStart = end;
                fragWidth = 0.0;
            };
            while (i < text.size()) {
                const std::size_t len = utf8::decode(text, i).length;
                const double cw = cache_.width(m_, text.substr(i, len), tok.style);
                if (used + fragWidth + cw > width + eps && (used + fragWidth) > 0.0) {
                    flush(i);
                    finishLine(LineEnd::Wrap, false);
                }
                fragWidth += cw;
                i += len;
            }
            flush(text.size());
        };

        for (const Token& tok : toks) {
            switch (tok.type) {
            case Token::Type::Space:
                if (lineHasContent) {
                    hasPending = true;
                    pending = tok;
                }
                break;
            case Token::Type::HardBreak:
                finishLine(LineEnd::HardBreak, true);
                break;
            case Token::Type::Word:
            case Token::Type::Image: {
                const bool image = tok.type == Token::Type::Image;
                const double gap = (lineHasContent && hasPending) ? pending.width : 0.0;
                if (lineHasContent && used + gap + tok.width > width + eps) finishLine(LineEnd::Wrap, false);
                if (!lineHasContent && tok.width > width + eps) {
                    placeLongWord(tok);
                    break;
                }
                if (lineHasContent && hasPending) {
                    place(pending.text, pending.style, pending.link, false, pending.width, pending.src);
                }
                hasPending = false;
                place(tok.text, tok.style, tok.link, image, tok.width, tok.src);
                break;
            }
            }
        }
        finishLine(finalEnd, false);
        return res;
    }

    // ------------------------------------------------------------ blocks
    double gapBetween(const Block* prev, const Block& cur, bool tight) const {
        if (prev == nullptr) return 0.0;
        if (cur.kind == BlockKind::HorizontalRule || prev->kind == BlockKind::HorizontalRule) return t_.ruleGap;
        if (cur.kind == BlockKind::Heading) {
            return t_.headingGapBeforeFactor * t_.headingSize[static_cast<std::size_t>(std::clamp(cur.level, 1, 6) - 1)];
        }
        if (prev->kind == BlockKind::Heading) return t_.headingGapAfter;
        return tight ? t_.tightGap : t_.paragraphGap;
    }

    void layoutBlocks(const std::vector<Block>& blocks, const Ctx& ctx, double& y, int depth) {
        if (depth > kMaxBlockRecursion) return;
        const Block* prev = nullptr;
        for (const Block& b : blocks) {
            const double top = y + gapBetween(prev, b, ctx.tight);
            double bottom = top;
            if (layoutBlock(b, ctx, top, bottom, depth)) {
                y = bottom;
                prev = &b;
            }
        }
    }

    LayoutBlock& beginLeaf(const Block& b, LayoutBlockKind kind) {
        LayoutBlock& lb = out_.blocks.emplace_back();
        lb.blockId = b.id;
        lb.kind = kind;
        lb.source = b.range;
        lb.lineBegin = lb.lineEnd = static_cast<std::uint32_t>(out_.lines.size());
        lb.cellBegin = lb.cellEnd = static_cast<std::uint32_t>(out_.tableCells.size());
        lb.decoBegin = lb.decoEnd = static_cast<std::uint32_t>(out_.decorations.size());
        if (b.id < out_.blockIndexById.size()) {
            out_.blockIndexById[b.id] = static_cast<std::uint32_t>(out_.blocks.size() - 1);
        }
        return lb;
    }
    void endLeaf(std::size_t index, core::Rect rect, double contentWidth) {
        LayoutBlock& lb = out_.blocks[index];
        lb.rect = rect;
        lb.contentWidth = contentWidth;
        lb.lineEnd = static_cast<std::uint32_t>(out_.lines.size());
        lb.cellEnd = static_cast<std::uint32_t>(out_.tableCells.size());
        lb.decoEnd = static_cast<std::uint32_t>(out_.decorations.size());
        maxRight_ = std::max(maxRight_, rect.origin.x + contentWidth);
    }

    // Returns true when the block produced output; `bottom` is then its bottom edge.
    bool layoutBlock(const Block& b, const Ctx& ctx, double top, double& bottom, int depth) {
        switch (b.kind) {
        case BlockKind::Paragraph:
        case BlockKind::Heading:
            return layoutText(b, ctx, top, bottom);
        case BlockKind::CodeBlock:
        case BlockKind::HtmlBlock:
            return layoutCode(b, ctx, top, bottom);
        case BlockKind::HorizontalRule:
            return layoutRule(b, ctx, top, bottom);
        case BlockKind::ImageBlock:
            return layoutImage(b, ctx, top, bottom);
        case BlockKind::Table:
            return layoutTable(b, ctx, top, bottom);
        case BlockKind::Quote:
            return layoutQuote(b, ctx, top, bottom, depth);
        case BlockKind::List:
            return layoutList(b, ctx, top, bottom, depth);
        }
        return false;
    }

    bool layoutText(const Block& b, const Ctx& ctx, double top, double& bottom) {
        const bool heading = b.kind == BlockKind::Heading;
        const TextStyle base = heading ? headingStyle(b.level) : baseStyle(ctx.quote ? TextKind::Quote : TextKind::Body);
        std::vector<Token> toks;
        tokenize(b.inlines, base, 0, toks, 0);
        if (toks.empty()) return false;
        const std::size_t idx = out_.blocks.size();
        LayoutBlock& lb = beginLeaf(b, heading ? LayoutBlockKind::Heading : LayoutBlockKind::Paragraph);
        lb.headingLevel = heading ? b.level : 0;
        const FlowResult r = flow(toks, ctx.x, top, ctx.width, base, ColumnAlign::Default, LineEnd::BlockEnd);
        if (!r.produced) {
            // Whitespace only: drop the block again.
            out_.blocks.pop_back();
            if (b.id < out_.blockIndexById.size()) out_.blockIndexById[b.id] = MarkdownLayout::kNoBlock;
            return false;
        }
        endLeaf(idx, core::Rect(ctx.x, top, ctx.width, r.bottom - top), std::max(ctx.width, r.maxWidth));
        bottom = r.bottom;
        return true;
    }

    // Splits code text into lines (tabs expanded) and lays them out without wrapping.
    bool layoutCode(const Block& b, const Ctx& ctx, double top, double& bottom) {
        const bool html = b.kind == BlockKind::HtmlBlock;
        TextStyle style;
        style.kind = TextKind::Code;
        style.monospace = true;
        style.size = t_.codeSize;
        const StyleMetrics sm = cache_.styleMetrics(m_, style);
        const double lineHeight = lineHeightFor(style.size, sm);
        const double pad = t_.codePadding;

        const std::size_t idx = out_.blocks.size();
        beginLeaf(b, html ? LayoutBlockKind::Html : LayoutBlockKind::Code);

        std::string_view text = b.text;
        if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);

        double y = top + pad;
        double maxWidth = 0.0;
        std::size_t pos = 0;
        std::string expanded;
        bool lastLine = false;
        while (!lastLine) {
            std::size_t nl = text.find('\n', pos);
            if (nl == std::string_view::npos) {
                nl = text.size();
                lastLine = true;
            }
            std::string_view line = text.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            pos = nl + 1;

            std::string_view shown = line;
            if (line.find('\t') != std::string_view::npos) {
                expanded.clear();
                std::size_t col = 0;
                const std::size_t tab = static_cast<std::size_t>(std::max(1, t_.tabSize));
                for (std::size_t i = 0; i < line.size();) {
                    if (line[i] == '\t') {
                        const std::size_t n = tab - (col % tab);
                        expanded.append(n, ' ');
                        col += n;
                        ++i;
                    } else {
                        const std::size_t len = utf8::decode(line, i).length;
                        expanded.append(line.substr(i, len));
                        ++col;
                        i += len;
                    }
                }
                shown = expanded;
            }
            const double w = shown.empty() ? 0.0 : m_.measure(shown, style).width;
            const auto lineIndex = static_cast<std::uint32_t>(out_.lines.size());
            const auto runIndex = static_cast<std::uint32_t>(out_.runs.size());
            if (!shown.empty()) {
                LayoutRun& r = out_.runs.emplace_back();
                r.text.assign(shown);
                r.style = style;
                r.x = ctx.x + pad;
                r.width = w;
                r.line = lineIndex;
                r.source = b.range;
            }
            const double baseline = y + (lineHeight - (sm.ascent + sm.descent)) * 0.5 + sm.ascent;
            if (!shown.empty()) out_.runs.back().baseline = baseline;
            LayoutLine& ln = out_.lines.emplace_back();
            ln.top = y;
            ln.height = lineHeight;
            ln.baseline = baseline;
            ln.x = ctx.x + pad;
            ln.width = w;
            ln.runBegin = runIndex;
            ln.runEnd = static_cast<std::uint32_t>(out_.runs.size());
            ln.end = LineEnd::CodeLine;
            maxWidth = std::max(maxWidth, w);
            y += lineHeight;
        }
        const double height = (y + pad) - top;
        const core::Rect rect(ctx.x, top, ctx.width, height);
        Decoration& d = out_.decorations.emplace_back();
        d.kind = DecorationKind::CodeBackground;
        d.rect = rect;
        d.blockId = b.id;
        endLeaf(idx, rect, std::max(ctx.width, maxWidth + 2.0 * pad));
        bottom = top + height;
        return true;
    }

    bool layoutRule(const Block& b, const Ctx& ctx, double top, double& bottom) {
        const std::size_t idx = out_.blocks.size();
        beginLeaf(b, LayoutBlockKind::Rule);
        const core::Rect rect(ctx.x, top, ctx.width, t_.ruleHeight);
        Decoration& d = out_.decorations.emplace_back();
        d.kind = DecorationKind::Rule;
        d.rect = rect;
        d.blockId = b.id;
        endLeaf(idx, rect, ctx.width);
        bottom = top + t_.ruleHeight;
        return true;
    }

    bool layoutImage(const Block& b, const Ctx& ctx, double top, double& bottom) {
        if (b.inlines.empty()) return false;
        const Inline& img = b.inlines.front();
        ImageInfo info;
        if (images_ != nullptr) info = images_->imageInfo(img.url);
        double w;
        double h;
        if (info.state == ImageState::Known && info.width > 0.0 && info.height > 0.0) {
            const double scale = std::min({1.0, ctx.width / info.width, t_.maxImageHeight / info.height});
            w = info.width * scale;
            h = info.height * scale;
        } else if (info.state == ImageState::Blocked) {
            w = std::min(ctx.width, t_.imageBlockedWidth);
            h = t_.imageBlockedHeight;
        } else {
            info.state = info.state == ImageState::Known ? ImageState::Unknown : info.state;
            w = std::min(ctx.width, t_.imagePlaceholderWidth);
            h = t_.imagePlaceholderHeight;
        }
        const std::size_t idx = out_.blocks.size();
        beginLeaf(b, LayoutBlockKind::Image);
        const core::Rect rect(ctx.x, top, w, h);
        Decoration& d = out_.decorations.emplace_back();
        d.kind = DecorationKind::ImagePlaceholder;
        d.rect = rect;
        d.text = img.text;
        d.url = img.url;
        d.imageState = info.state;
        d.blockId = b.id;
        endLeaf(idx, rect, w);
        bottom = top + h;
        return true;
    }

    struct CellTokens {
        std::vector<Token> toks;
        double natural = 0.0; // widest single-line extent
        double minimum = 0.0; // widest unbreakable word
    };

    bool layoutTable(const Block& b, const Ctx& ctx, double top, double& bottom) {
        const std::size_t cols = b.alignments.size();
        const std::size_t rowCount = 1 + b.body.size();
        if (cols == 0) return false;

        // Tokenise once; compute per-column natural and minimum widths.
        std::vector<CellTokens> cells(rowCount * cols);
        std::vector<double> natural(cols, 0.0);
        std::vector<double> minimum(cols, 0.0);
        const auto prepare = [&](const TableRow& row, std::size_t r, bool header) {
            TextStyle base = baseStyle(TextKind::Table);
            base.bold = header;
            for (std::size_t c = 0; c < cols && c < row.cells.size(); ++c) {
                CellTokens& ct = cells[r * cols + c];
                tokenize(row.cells[c].inlines, base, 0, ct.toks, 0);
                double line = 0.0;
                bool pendingSpace = false;
                double spaceW = 0.0;
                for (const Token& tok : ct.toks) {
                    if (tok.type == Token::Type::Space) {
                        pendingSpace = line > 0.0;
                        spaceW = tok.width;
                    } else if (tok.type == Token::Type::HardBreak) {
                        ct.natural = std::max(ct.natural, line);
                        line = 0.0;
                        pendingSpace = false;
                    } else {
                        line += (pendingSpace ? spaceW : 0.0) + tok.width;
                        pendingSpace = false;
                        ct.minimum = std::max(ct.minimum, tok.width);
                    }
                }
                ct.natural = std::max(ct.natural, line);
                ct.minimum = std::min(ct.minimum, t_.tableMaxColumnWidth);
                natural[c] = std::max(natural[c], ct.natural);
                minimum[c] = std::max(minimum[c], ct.minimum);
            }
        };
        prepare(b.header, 0, true);
        for (std::size_t r = 0; r < b.body.size(); ++r) prepare(b.body[r], r + 1, false);

        const double padX = t_.tableCellPaddingX;
        const double padY = t_.tableCellPaddingY;
        double totalNatural = 0.0;
        double totalMin = 0.0;
        for (std::size_t c = 0; c < cols; ++c) {
            minimum[c] = std::min(minimum[c], natural[c]);
            totalNatural += natural[c] + 2.0 * padX;
            totalMin += minimum[c] + 2.0 * padX;
        }
        std::vector<double> width(cols, 0.0);
        if (totalNatural <= ctx.width) {
            for (std::size_t c = 0; c < cols; ++c) width[c] = natural[c];
        } else if (totalMin >= ctx.width) {
            for (std::size_t c = 0; c < cols; ++c) width[c] = minimum[c];
        } else {
            const double extra = ctx.width - totalMin;
            double slack = 0.0;
            for (std::size_t c = 0; c < cols; ++c) slack += natural[c] - minimum[c];
            for (std::size_t c = 0; c < cols; ++c) {
                width[c] = minimum[c] + (slack > 0.0 ? extra * (natural[c] - minimum[c]) / slack : 0.0);
            }
        }
        double tableWidth = 0.0;
        std::vector<double> colX(cols, 0.0);
        for (std::size_t c = 0; c < cols; ++c) {
            colX[c] = ctx.x + tableWidth;
            tableWidth += width[c] + 2.0 * padX;
        }

        const std::size_t idx = out_.blocks.size();
        beginLeaf(b, LayoutBlockKind::Table);
        const StyleMetrics minMetrics = cache_.styleMetrics(m_, baseStyle(TextKind::Table));
        const double minRowHeight = lineHeightFor(t_.tableSize, minMetrics) + 2.0 * padY;

        double y = top;
        for (std::size_t r = 0; r < rowCount; ++r) {
            const TableRow& row = r == 0 ? b.header : b.body[r - 1];
            double rowHeight = minRowHeight;
            const std::size_t firstCell = out_.tableCells.size();
            for (std::size_t c = 0; c < cols; ++c) {
                TableCellLayout& cell = out_.tableCells.emplace_back();
                cell.row = static_cast<std::uint32_t>(r);
                cell.col = static_cast<std::uint32_t>(c);
                cell.header = r == 0;
                cell.align = b.alignments[c];
                if (c < row.cells.size()) cell.source = row.cells[c].range;
                cell.lineBegin = static_cast<std::uint32_t>(out_.lines.size());
                double contentH = 0.0;
                const CellTokens& ct = cells[r * cols + c];
                if (!ct.toks.empty()) {
                    TextStyle base = baseStyle(TextKind::Table);
                    base.bold = r == 0;
                    const LineEnd end = c + 1 < cols ? LineEnd::CellEnd : LineEnd::RowEnd;
                    const FlowResult fr = flow(ct.toks, colX[c] + padX, y + padY, width[c], base, cell.align, end);
                    contentH = fr.bottom - (y + padY);
                }
                cell.lineEnd = static_cast<std::uint32_t>(out_.lines.size());
                rowHeight = std::max(rowHeight, contentH + 2.0 * padY);
            }
            for (std::size_t c = 0; c < cols; ++c) {
                TableCellLayout& cell = out_.tableCells[firstCell + c];
                cell.rect = core::Rect(colX[c], y, width[c] + 2.0 * padX, rowHeight);
            }
            y += rowHeight;
        }
        const core::Rect rect(ctx.x, top, tableWidth, y - top);
        endLeaf(idx, rect, std::max(tableWidth, 0.0));
        bottom = y;
        return true;
    }

    bool layoutQuote(const Block& b, const Ctx& ctx, double top, double& bottom, int depth) {
        Ctx child;
        child.x = ctx.x + t_.quoteIndent;
        child.width = std::max(t_.minContentWidth, ctx.width - t_.quoteIndent);
        child.quote = true;
        child.tight = false;
        const std::size_t first = out_.blocks.size();
        double y = top;
        layoutBlocks(b.children, child, y, depth + 1);
        if (out_.blocks.size() == first) return false;
        noteContainer(b.id, first);
        Decoration& d = out_.decorations.emplace_back();
        d.kind = DecorationKind::QuoteBar;
        d.rect = core::Rect(ctx.x + t_.quoteBarInset, top, t_.quoteBarWidth, y - top);
        d.blockId = b.id;
        bottom = y;
        return true;
    }

    void noteContainer(std::uint32_t id, std::size_t firstLeaf) {
        if (id < out_.blockIndexById.size() && firstLeaf < out_.blocks.size()) {
            out_.blockIndexById[id] = static_cast<std::uint32_t>(firstLeaf);
        }
    }

    bool layoutList(const Block& b, const Ctx& ctx, double top, double& bottom, int depth) {
        if (b.items.empty()) return false;
        const TextStyle body = baseStyle(ctx.quote ? TextKind::Quote : TextKind::Body);
        double indent = t_.listIndent;
        if (b.ordered) {
            const std::string label = std::to_string(static_cast<unsigned long long>(b.start) + b.items.size() - 1) + ".";
            indent = std::max(indent, cache_.width(m_, label, body) + t_.listMarkerGap + 4.0);
        }
        indent = std::min(indent, std::max(0.0, ctx.width - t_.minContentWidth));
        Ctx child;
        child.x = ctx.x + indent;
        child.width = std::max(t_.minContentWidth, ctx.width - indent);
        child.quote = ctx.quote;
        child.tight = b.tight;

        const std::size_t listFirst = out_.blocks.size();
        double y = top;
        bool any = false;
        unsigned long long number = b.start;
        for (const ListItem& item : b.items) {
            if (any) y += b.tight ? t_.tightGap : t_.paragraphGap;
            const double itemTop = y;
            const std::size_t first = out_.blocks.size();
            layoutBlocks(item.children, child, y, depth + 1);
            const StyleMetrics sm = cache_.styleMetrics(m_, body);
            double baseline;
            double lineTop;
            double lineHeight;
            if (out_.blocks.size() > first) {
                const LayoutBlock& fb = out_.blocks[first];
                if (fb.lineEnd > fb.lineBegin) {
                    const LayoutLine& ln = out_.lines[fb.lineBegin];
                    baseline = ln.baseline;
                    lineTop = ln.top;
                    lineHeight = ln.height;
                } else {
                    lineTop = fb.rect.origin.y;
                    lineHeight = lineHeightFor(body.size, sm);
                    baseline = lineTop + (lineHeight - (sm.ascent + sm.descent)) * 0.5 + sm.ascent;
                }
            } else {
                // Empty item: reserve one line for the marker.
                lineTop = itemTop;
                lineHeight = lineHeightFor(body.size, sm);
                baseline = lineTop + (lineHeight - (sm.ascent + sm.descent)) * 0.5 + sm.ascent;
                y = itemTop + lineHeight;
            }
            const double markerRight = ctx.x + indent - t_.listMarkerGap;
            Decoration& d = out_.decorations.emplace_back();
            d.blockId = b.id;
            d.baseline = baseline;
            d.style = body;
            if (item.isTask) {
                d.kind = DecorationKind::TaskCheckbox;
                d.checked = item.checked;
                d.rect = core::Rect(markerRight - t_.checkboxSize, baseline - t_.checkboxSize * 0.85, t_.checkboxSize,
                                    t_.checkboxSize);
            } else if (b.ordered) {
                d.kind = DecorationKind::ListNumber;
                d.text = std::to_string(number) + ".";
                const double w = cache_.width(m_, d.text, body);
                d.rect = core::Rect(markerRight - w, lineTop, w, lineHeight);
            } else {
                d.kind = DecorationKind::ListBullet;
                d.rect = core::Rect(markerRight - t_.bulletSize - 3.0, baseline - body.size * 0.35 - t_.bulletSize * 0.5,
                                    t_.bulletSize, t_.bulletSize);
            }
            ++number;
            any = true;
        }
        if (out_.blocks.size() > listFirst) noteContainer(b.id, listFirst);
        bottom = y;
        return any;
    }

    const MarkdownDocument& doc_;
    double viewport_;
    const ITextMeasurer& m_;
    const IImageSizeProvider* images_;
    const Typography& t_;
    MeasureCache& cache_;
    MarkdownLayout& out_;
    double maxRight_ = 0.0;
};

} // namespace

MarkdownLayout layoutMarkdown(const MarkdownDocument& doc, double viewportWidth, const ITextMeasurer& measurer,
                              const IImageSizeProvider* images, const Typography& typography, MeasureCache* cache) {
    MarkdownLayout out;
    MeasureCache local;
    MeasureCache& c = cache != nullptr ? *cache : local;
    // Rough reservations: avoids repeated growth on large documents.
    out.blocks.reserve(doc.blockCount);
    Engine(doc, viewportWidth, measurer, images, typography, c, out).run();
    return out;
}

// ------------------------------------------------------------------ lookups

std::size_t MarkdownLayout::blockIndexAtY(double y) const {
    if (blocks.empty()) return kNpos;
    auto it = std::upper_bound(blocks.begin(), blocks.end(), y,
                               [](double v, const LayoutBlock& b) { return v < b.rect.origin.y; });
    if (it == blocks.begin()) return 0;
    return static_cast<std::size_t>(it - blocks.begin()) - 1;
}

std::pair<std::size_t, std::size_t> MarkdownLayout::blockRangeForY(double y0, double y1) const {
    const auto first = std::partition_point(blocks.begin(), blocks.end(),
                                            [&](const LayoutBlock& b) { return b.rect.maxY() <= y0; });
    const auto last = std::partition_point(first, blocks.end(), [&](const LayoutBlock& b) { return b.rect.origin.y < y1; });
    return {static_cast<std::size_t>(first - blocks.begin()), static_cast<std::size_t>(last - blocks.begin())};
}

std::vector<std::size_t> MarkdownLayout::decorationsInRange(double y0, double y1) const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < decorations.size(); ++i) {
        const core::Rect& r = decorations[i].rect;
        if (r.maxY() > y0 && r.origin.y < y1) out.push_back(i);
    }
    return out;
}

std::size_t MarkdownLayout::blockIndexAtSource(std::size_t offset) const {
    if (blocks.empty()) return kNpos;
    auto it = std::upper_bound(blocks.begin(), blocks.end(), offset,
                               [](std::size_t v, const LayoutBlock& b) { return v < b.source.start; });
    if (it == blocks.begin()) return 0;
    return static_cast<std::size_t>(it - blocks.begin()) - 1;
}

double MarkdownLayout::yForBlockId(std::uint32_t blockId) const {
    if (blockId >= blockIndexById.size() || blockIndexById[blockId] == kNoBlock) return -1.0;
    return blocks[blockIndexById[blockId]].rect.origin.y;
}

double MarkdownLayout::yForSourceOffset(std::size_t offset) const {
    const std::size_t i = blockIndexAtSource(offset);
    return i == kNpos ? -1.0 : blocks[i].rect.origin.y;
}

// ------------------------------------------------------- hit-test/selection

namespace {

double prefixWidth(const LayoutRun& r, std::size_t offset, const ITextMeasurer& m) {
    if (offset == 0) return 0.0;
    if (offset >= r.text.size()) return r.width;
    return std::min(r.width, m.measure(std::string_view(r.text).substr(0, offset), r.style).width);
}

} // namespace

double MarkdownLayout::xForPosition(TextPosition pos, const ITextMeasurer& measurer) const {
    if (pos.run >= runs.size()) return 0.0;
    const LayoutRun& r = runs[pos.run];
    return r.x + prefixWidth(r, pos.offset, measurer);
}

HitResult MarkdownLayout::hitTest(core::Point p, const ITextMeasurer& measurer) const {
    HitResult hit;
    if (blocks.empty() || runs.empty()) return hit;
    std::size_t bi = blockIndexAtY(p.y);
    if (bi == kNpos) return hit;
    // Between blocks: take the nearer neighbour.
    if (p.y > blocks[bi].rect.maxY() && bi + 1 < blocks.size()) {
        if (blocks[bi + 1].rect.origin.y - p.y < p.y - blocks[bi].rect.maxY()) ++bi;
    }
    // Find the closest block that has text.
    const auto hasText = [&](std::size_t i) { return blocks[i].lineEnd > blocks[i].lineBegin; };
    std::size_t chosen = kNpos;
    for (std::size_t d = 0; d < blocks.size(); ++d) {
        if (bi + d < blocks.size() && hasText(bi + d)) { chosen = bi + d; break; }
        if (d <= bi && hasText(bi - d)) { chosen = bi - d; break; }
    }
    if (chosen == kNpos) return hit;
    const LayoutBlock& blk = blocks[chosen];

    std::uint32_t lo = blk.lineBegin;
    std::uint32_t hi = blk.lineEnd;
    if (blk.kind == LayoutBlockKind::Table) {
        // Pick the cell under (or nearest to) the point, then its lines.
        double best = std::numeric_limits<double>::max();
        std::uint32_t bestCell = kNoBlock;
        for (std::uint32_t c = blk.cellBegin; c < blk.cellEnd; ++c) {
            const TableCellLayout& cell = tableCells[c];
            if (cell.lineEnd <= cell.lineBegin) continue;
            const double dx = std::max({cell.rect.minX() - p.x, 0.0, p.x - cell.rect.maxX()});
            const double dy = std::max({cell.rect.minY() - p.y, 0.0, p.y - cell.rect.maxY()});
            const double dist = dx * dx + dy * dy;
            if (dist < best) { best = dist; bestCell = c; }
        }
        if (bestCell == kNoBlock) return hit;
        lo = tableCells[bestCell].lineBegin;
        hi = tableCells[bestCell].lineEnd;
    }
    // Line: last line whose top <= y within [lo, hi).
    const auto lb = lines.begin() + lo;
    const auto le = lines.begin() + hi;
    auto it = std::upper_bound(lb, le, p.y, [](double v, const LayoutLine& l) { return v < l.top; });
    const std::size_t lineIdx = it == lb ? lo : static_cast<std::size_t>(it - lines.begin()) - 1;
    const LayoutLine& line = lines[lineIdx];
    if (line.runEnd <= line.runBegin) {
        // Empty code line: caret at the start of the next run, if any, else the previous.
        hit.valid = false;
        for (std::size_t l = lineIdx; l < hi; ++l) {
            if (lines[l].runEnd > lines[l].runBegin) {
                hit.valid = true; hit.block = chosen; hit.line = l;
                hit.position = {lines[l].runBegin, 0};
                hit.sourceOffset = sourceOffsetAt(hit.position);
                return hit;
            }
        }
        for (std::size_t l = lineIdx; l-- > lo;) {
            if (lines[l].runEnd > lines[l].runBegin) {
                const std::uint32_t r = lines[l].runEnd - 1;
                hit.valid = true; hit.block = chosen; hit.line = l;
                hit.position = {r, static_cast<std::uint32_t>(runs[r].text.size())};
                hit.sourceOffset = sourceOffsetAt(hit.position);
                return hit;
            }
        }
        return hit;
    }
    // Run: last run with x <= px.
    std::uint32_t runIdx = line.runBegin;
    for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) {
        if (runs[r].x <= p.x) runIdx = r; else break;
    }
    const LayoutRun& run = runs[runIdx];
    const double dx = p.x - run.x;
    std::size_t offset;
    if (dx <= 0.0) {
        offset = 0;
    } else if (dx >= run.width) {
        offset = run.text.size();
    } else {
        // Caret between code points: binary search over boundaries.
        std::vector<std::size_t> bounds;
        bounds.reserve(run.text.size() + 1);
        for (std::size_t i = 0; i < run.text.size(); i += utf8::decode(run.text, i).length) bounds.push_back(i);
        bounds.push_back(run.text.size());
        const auto xAt = [&](std::size_t k) { return prefixWidth(run, bounds[k], measurer); };
        std::size_t a = 0;
        std::size_t z = bounds.size() - 1;
        while (a < z) {
            const std::size_t mid = (a + z + 1) / 2;
            if (xAt(mid) <= dx) a = mid; else z = mid - 1;
        }
        if (a + 1 < bounds.size() && xAt(a + 1) - dx < dx - xAt(a)) ++a;
        offset = bounds[a];
    }
    hit.valid = true;
    hit.block = chosen;
    hit.line = lineIdx;
    hit.position = {runIdx, static_cast<std::uint32_t>(offset)};
    hit.onText = p.x >= run.x && p.x <= run.x + run.width && p.y >= line.top && p.y <= line.top + line.height;
    hit.link = run.link;
    hit.sourceOffset = sourceOffsetAt(hit.position);
    return hit;
}

std::vector<core::Rect> MarkdownLayout::selectionRects(TextPosition a, TextPosition b,
                                                       const ITextMeasurer& measurer) const {
    std::vector<core::Rect> rects;
    if (b < a) std::swap(a, b);
    if (a == b || a.run >= runs.size()) return rects;
    const std::uint32_t last = std::min<std::uint32_t>(b.run, static_cast<std::uint32_t>(runs.size() - 1));
    for (std::uint32_t r = a.run; r <= last; ++r) {
        const LayoutRun& run = runs[r];
        const std::size_t startOff = r == a.run ? a.offset : 0;
        const std::size_t endOff = r == b.run ? b.offset : run.text.size();
        const double x0 = run.x + prefixWidth(run, startOff, measurer);
        const double x1 = run.x + prefixWidth(run, endOff, measurer);
        const LayoutLine& line = lines[run.line];
        if (x1 <= x0) continue;
        if (!rects.empty()) {
            core::Rect& prev = rects.back();
            if (prev.origin.y == line.top && std::abs(prev.maxX() - x0) < 0.5) {
                prev.size.width = x1 - prev.origin.x;
                continue;
            }
        }
        rects.emplace_back(x0, line.top, x1 - x0, line.height);
    }
    return rects;
}

std::string MarkdownLayout::selectedText(TextPosition a, TextPosition b) const {
    std::string out;
    if (b < a) std::swap(a, b);
    if (a == b || a.run >= runs.size()) return out;
    const std::uint32_t last = std::min<std::uint32_t>(b.run, static_cast<std::uint32_t>(runs.size() - 1));
    for (std::uint32_t r = a.run; r <= last; ++r) {
        const LayoutRun& run = runs[r];
        const std::size_t s = r == a.run ? std::min<std::size_t>(a.offset, run.text.size()) : 0;
        const std::size_t e = r == b.run ? std::min<std::size_t>(b.offset, run.text.size()) : run.text.size();
        if (e > s) out.append(run.text, s, e - s);
        const LayoutLine& line = lines[run.line];
        if (r + 1 == line.runEnd && r != last) {
            switch (line.end) {
            case LineEnd::Wrap: out.push_back(' '); break;
            case LineEnd::CellEnd: out.push_back('\t'); break;
            default: out.push_back('\n'); break;
            }
        }
    }
    return out;
}

SourceRange MarkdownLayout::selectedSource(TextPosition a, TextPosition b) const {
    SourceRange r;
    if (b < a) std::swap(a, b);
    if (a == b || a.run >= runs.size()) return r;
    const std::uint32_t last = std::min<std::uint32_t>(b.run, static_cast<std::uint32_t>(runs.size() - 1));
    bool any = false;
    for (std::uint32_t i = a.run; i <= last; ++i) {
        if (i == a.run && a.offset >= runs[i].text.size() && a.run != b.run) continue;
        if (i == b.run && b.offset == 0 && a.run != b.run) continue;
        if (!any) { r = runs[i].source; any = true; }
        else { r.start = std::min(r.start, runs[i].source.start); r.end = std::max(r.end, runs[i].source.end); }
    }
    return r;
}

std::size_t MarkdownLayout::sourceOffsetAt(TextPosition pos) const {
    if (pos.run >= runs.size()) return 0;
    const LayoutRun& r = runs[pos.run];
    if (r.exactSource) return r.source.start + std::min<std::size_t>(pos.offset, r.text.size());
    if (pos.offset == 0 || r.text.empty()) return r.source.start;
    if (pos.offset >= r.text.size()) return r.source.end;
    return r.source.start + (r.source.size() * pos.offset) / r.text.size();
}

} // namespace rivet::markdown
