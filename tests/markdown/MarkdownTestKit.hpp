// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared helpers for the Markdown tests: a fixed-width fake text measurer, a
// fake image size provider, fixture loading, a deterministic document
// generator and structural validators.

#include "markdown/Md4cMarkdownParser.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "markdown/MarkdownPlainText.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef RIVET_MD_FIXTURE_DIR
#define RIVET_MD_FIXTURE_DIR "tests/fixtures/markdown"
#endif

namespace rivet::markdown::testkit {

// Width = code points * size * 2/3 (10 per character at the 15pt body size).
class FixedMeasurer final : public ITextMeasurer {
public:
    mutable std::size_t calls = 0;
    TextMetrics measure(std::string_view utf8, const TextStyle& style) const override {
        ++calls;
        std::size_t cps = 0;
        for (const char c : utf8) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++cps;
        }
        TextMetrics m;
        m.width = static_cast<double>(cps) * style.size * 2.0 / 3.0;
        m.ascent = style.size * 0.8;
        m.descent = style.size * 0.2;
        return m;
    }
};

class FakeImages final : public IImageSizeProvider {
public:
    std::map<std::string, ImageInfo> known;
    ImageInfo imageInfo(std::string_view url) const override {
        const auto it = known.find(std::string(url));
        return it == known.end() ? ImageInfo{} : it->second;
    }
};

inline std::string readFixture(const std::string& name) {
    std::ifstream in(std::string(RIVET_MD_FIXTURE_DIR) + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline MarkdownDocument parse(std::string_view src, const ParseLimits& limits = {}) {
    return Md4cMarkdownParser().parse(src, limits);
}

// Zero page margins keep layout arithmetic in tests readable.
inline Typography plainTypography() {
    Typography t;
    t.pageMarginX = 0.0;
    t.pageMarginY = 0.0;
    return t;
}

// ----------------------------------------------------------- generator

// Deterministic pseudo-random document of roughly `targetBytes` bytes mixing
// headings, paragraphs with inline formatting, lists, quotes, code, tables
// and Cyrillic text. Never committed; generated at run time.
inline std::string generateMarkdown(std::size_t targetBytes, std::uint32_t seed = 12345) {
    std::uint32_t state = seed;
    const auto next = [&](std::uint32_t mod) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % mod;
    };
    static const char* const words[] = {"lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit",
                                        "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore",
                                        "magna", "aliqua", "привет", "мир", "документ", "строка", "текст"};
    const auto sentence = [&](std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ' ';
            const char* w = words[next(sizeof(words) / sizeof(words[0]))];
            switch (next(12)) {
            case 0: s += std::string("**") + w + "**"; break;
            case 1: s += std::string("*") + w + "*"; break;
            case 2: s += std::string("`") + w + "`"; break;
            case 3: s += std::string("[") + w + "](https://example.com/" + w + ")"; break;
            case 4: s += std::string("~~") + w + "~~"; break;
            default: s += w; break;
            }
        }
        return s;
    };
    std::string out;
    out.reserve(targetBytes + 1024);
    std::size_t section = 0;
    while (out.size() < targetBytes) {
        switch (next(8)) {
        case 0:
            out += std::string(1 + next(3), '#') + " Section " + std::to_string(section++) + " " + sentence(2) + "\n\n";
            break;
        case 1:
        case 2:
        case 3:
            out += sentence(10 + next(40)) + "\n\n";
            break;
        case 4: {
            const std::size_t n = 2 + next(5);
            for (std::size_t i = 0; i < n; ++i) {
                out += (next(4) == 0 ? "- [x] " : "- ") + sentence(3 + next(8)) + "\n";
                if (next(5) == 0) out += "  - " + sentence(3) + "\n";
            }
            out += "\n";
            break;
        }
        case 5:
            out += "> " + sentence(8 + next(12)) + "\n\n";
            break;
        case 6:
            out += "```\n";
            for (std::size_t i = 0, n = 2 + next(6); i < n; ++i) out += "int x" + std::to_string(i) + " = " + std::to_string(next(1000)) + ";\n";
            out += "```\n\n";
            break;
        default:
            out += "| a | b | c |\n|---|:-:|--:|\n";
            for (std::size_t i = 0, n = 1 + next(4); i < n; ++i) out += "| " + sentence(1) + " | " + sentence(2) + " | " + std::to_string(next(100)) + " |\n";
            out += "\n";
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------- validators

struct RangeCheck {
    std::size_t sourceSize = 0;
    std::size_t problems = 0;
    std::string firstProblem;

    void fail(const std::string& what) {
        if (problems++ == 0) firstProblem = what;
    }
    void range(const SourceRange& r, const char* what) {
        if (r.start > r.end || r.end > sourceSize) fail(std::string(what) + " range outside source or inverted");
    }
    void inlines(const std::vector<Inline>& v, const char* what) {
        std::size_t prevStart = 0;
        for (const Inline& in : v) {
            range(in.range, what);
            if (in.range.start < prevStart) fail(std::string(what) + ": inline ranges not ordered");
            prevStart = in.range.start;
            inlines(in.children, what);
        }
    }
    void blocks(const std::vector<Block>& v) {
        std::size_t prevStart = 0;
        for (const Block& b : v) {
            range(b.range, "block");
            if (b.range.start < prevStart) fail("block ranges not ordered");
            prevStart = b.range.start;
            inlines(b.inlines, "inline");
            blocks(b.children);
            for (const ListItem& it : b.items) {
                range(it.range, "item");
                blocks(it.children);
            }
            const auto row = [&](const TableRow& r) {
                range(r.range, "row");
                for (const TableCell& c : r.cells) {
                    range(c.range, "cell");
                    inlines(c.inlines, "cell inline");
                }
            };
            if (b.kind == BlockKind::Table) {
                row(b.header);
                for (const TableRow& r : b.body) row(r);
            }
        }
    }
};

inline RangeCheck checkRanges(const MarkdownDocument& doc) {
    RangeCheck c;
    c.sourceSize = doc.sourceLength;
    c.blocks(doc.blocks);
    return c;
}

inline std::size_t countBlocks(const std::vector<Block>& v) {
    std::size_t n = 0;
    for (const Block& b : v) {
        ++n;
        n += countBlocks(b.children);
        for (const ListItem& it : b.items) n += countBlocks(it.children);
    }
    return n;
}

inline std::string slice(std::string_view src, const SourceRange& r) {
    return std::string(src.substr(r.start, r.size()));
}

} // namespace rivet::markdown::testkit
