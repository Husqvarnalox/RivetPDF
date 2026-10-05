// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownPlainText.hpp"

#include <algorithm>

namespace rivet::markdown {

namespace {

struct Sink {
    PlainText& out;
    bool spans;

    void add(const std::string& s, SourceRange r) {
        if (s.empty()) return;
        const std::size_t begin = out.text.size();
        out.text += s;
        if (spans) out.spans.push_back({begin, out.text.size(), r});
    }
    void addChar(char c, SourceRange r) {
        const std::size_t begin = out.text.size();
        out.text.push_back(c);
        if (spans) out.spans.push_back({begin, begin + 1, r});
    }
    void separator(char c) { out.text.push_back(c); }
};

void walkInlines(const std::vector<Inline>& inlines, Sink& sink) {
    for (const Inline& in : inlines) {
        switch (in.kind) {
        case InlineKind::Text:
        case InlineKind::Code:
        case InlineKind::Image:
            sink.add(in.text, in.range);
            break;
        case InlineKind::SoftBreak:
            sink.addChar(' ', in.range);
            break;
        case InlineKind::HardBreak:
            sink.addChar('\n', in.range);
            break;
        default:
            walkInlines(in.children, sink);
            break;
        }
    }
}

void walkBlocks(const std::vector<Block>& blocks, Sink& sink, bool& first) {
    for (const Block& b : blocks) {
        if (!first) sink.separator('\n');
        first = false;
        switch (b.kind) {
        case BlockKind::Paragraph:
        case BlockKind::Heading:
        case BlockKind::ImageBlock:
            walkInlines(b.inlines, sink);
            break;
        case BlockKind::CodeBlock:
        case BlockKind::HtmlBlock:
            sink.add(b.text, b.range);
            break;
        case BlockKind::HorizontalRule:
            break;
        case BlockKind::Quote:
        {
            bool childFirst = true;
            walkBlocks(b.children, sink, childFirst);
            break;
        }
        case BlockKind::List: {
            bool firstItem = true;
            for (const ListItem& item : b.items) {
                if (!firstItem) sink.separator('\n');
                firstItem = false;
                bool firstChild = true;
                walkBlocks(item.children, sink, firstChild);
            }
            break;
        }
        case BlockKind::Table: {
            auto row = [&](const TableRow& r) {
                bool firstCell = true;
                for (const TableCell& c : r.cells) {
                    if (!firstCell) sink.separator('\t');
                    firstCell = false;
                    walkInlines(c.inlines, sink);
                }
            };
            row(b.header);
            for (const TableRow& r : b.body) {
                sink.separator('\n');
                row(r);
            }
            break;
        }
        }
    }
}

} // namespace

PlainText extractPlainText(const MarkdownDocument& doc, bool withSpans) {
    PlainText out;
    Sink sink{out, withSpans};
    bool first = true;
    walkBlocks(doc.blocks, sink, first);
    return out;
}

std::string inlinesToPlainText(const std::vector<Inline>& inlines) {
    PlainText out;
    Sink sink{out, false};
    walkInlines(inlines, sink);
    return std::move(out.text);
}

SourceRange sourceRangeForText(const PlainText& plain, std::size_t textStart, std::size_t textEnd) {
    SourceRange r;
    bool any = false;
    auto it = std::lower_bound(plain.spans.begin(), plain.spans.end(), textStart,
                               [](const PlainTextSpan& s, std::size_t v) { return s.textEnd <= v; });
    for (; it != plain.spans.end() && it->textStart < textEnd; ++it) {
        if (!any) {
            r = it->source;
            any = true;
        } else {
            r.start = std::min(r.start, it->source.start);
            r.end = std::max(r.end, it->source.end);
        }
    }
    return r;
}

} // namespace rivet::markdown
