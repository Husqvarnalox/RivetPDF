// SPDX-License-Identifier: MPL-2.0
// Document comparison (ADR-0029): word tokens and page-pair diffs over
// synthetic text pages (portable), and the full driver over two generated
// PDFs (PDFium-ON bodies; early return without a backend).
#include "RivetTest.h"

#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentComparison.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/TextService.hpp"
#include "pdf/PdfBuild.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace rivet;
using editor::ChangeKind;
using editor::ComparisonChange;

namespace {

// Text page with one 10x10 box per character, 10 points apart; `lines` are
// laid out 20 points apart and separated by a generated zero-area '\n'.
pdf::PdfTextPage makePage(const std::vector<std::string>& lines) {
    std::vector<pdf::TextChar> chars;
    double y = 0.0;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const std::u32string line = pdf::decodeUtf8(lines[l]);
        double x = 0.0;
        for (const char32_t c : line) {
            pdf::TextChar ch;
            ch.unicode = c;
            ch.index = static_cast<std::uint32_t>(chars.size());
            ch.bounds = core::Rect{x, y, 10.0, 10.0};
            ch.fontSize = 10.0;
            chars.push_back(ch);
            x += 10.0;
        }
        if (l + 1 < lines.size()) {
            pdf::TextChar nl;
            nl.unicode = U'\n';
            nl.index = static_cast<std::uint32_t>(chars.size());
            chars.push_back(nl);
        }
        y += 20.0;
    }
    return pdf::PdfTextPage(std::move(chars));
}

} // namespace

RIVET_TEST(tokenizeWordsSplitsWordsPunctuationAndLines) {
    const pdf::PdfTextPage page = makePage({"Hello, wörld", "end.x"});
    const auto tokens = editor::tokenizeWords(page);
    // Hello , wörld | end . x
    CHECK_EQ(tokens.size(), std::size_t{6});
    CHECK_EQ(tokens[0].text, std::string("Hello"));
    CHECK_EQ(tokens[0].firstChar, 0u);
    CHECK_EQ(tokens[0].lastChar, 4u);
    CHECK_EQ(tokens[1].text, std::string(","));
    CHECK_EQ(tokens[2].text, std::string("w\xC3\xB6rld"));
    CHECK_EQ(tokens[2].line, 0u);
    CHECK_EQ(tokens[3].text, std::string("end"));
    CHECK_EQ(tokens[3].line, 1u);
    CHECK_EQ(tokens[4].text, std::string("."));
    CHECK_EQ(tokens[5].text, std::string("x"));
    CHECK(editor::tokenizeWords(pdf::PdfTextPage{}).empty());
}

RIVET_TEST(comparePageTextMapsHunksToCharBoxes) {
    // "one two three" vs "one 2 three four"
    const pdf::PdfTextPage left = makePage({"one two three"});
    const pdf::PdfTextPage right = makePage({"one 2 three four"});
    std::vector<ComparisonChange> changes;
    const editor::PageComparison page = editor::comparePageText(&left, &right, 4, changes);

    CHECK_EQ(changes.size(), std::size_t{2});
    CHECK(changes[0].kind == ChangeKind::Replaced);
    CHECK_EQ(changes[0].leftPage, std::size_t{4});
    CHECK_EQ(changes[0].leftSnippet, std::string("two"));
    CHECK_EQ(changes[0].rightSnippet, std::string("2"));
    // "two" = chars 4..6 -> x 40..70; "2" = char 4 -> x 40..50.
    CHECK_NEAR(changes[0].leftRect.minX(), 40.0, 0.01);
    CHECK_NEAR(changes[0].leftRect.maxX(), 70.0, 0.01);
    CHECK_NEAR(changes[0].rightRect.minX(), 40.0, 0.01);
    CHECK_NEAR(changes[0].rightRect.maxX(), 50.0, 0.01);
    CHECK(changes[1].kind == ChangeKind::Inserted);
    CHECK(changes[1].leftRect.isEmpty());
    CHECK_EQ(changes[1].rightSnippet, std::string("four"));

    CHECK_EQ(page.deletedRects.size(), std::size_t{1});
    CHECK_EQ(page.insertedRects.size(), std::size_t{2});
}

RIVET_TEST(comparePageTextIdenticalAndEmpty) {
    const pdf::PdfTextPage page = makePage({"same text", "second line"});
    std::vector<ComparisonChange> changes;
    const auto result = editor::comparePageText(&page, &page, 0, changes);
    CHECK(changes.empty());
    CHECK(result.deletedRects.empty());
    CHECK(result.insertedRects.empty());

    const auto bothNull = editor::comparePageText(nullptr, nullptr, 0, changes);
    CHECK(changes.empty());
    CHECK(bothNull.deletedRects.empty());

    // Text vs. no text: one deletion covering both lines (two rects).
    const auto deleted = editor::comparePageText(&page, nullptr, 0, changes);
    CHECK_EQ(changes.size(), std::size_t{1});
    CHECK(changes[0].kind == ChangeKind::Deleted);
    CHECK_EQ(deleted.deletedRects.size(), std::size_t{2});
}

RIVET_TEST(comparePageTextBoundFallbackStillHighlights) {
    // Limit the bound below the word count so the per-line fallback runs.
    const pdf::PdfTextPage left = makePage({"a b c", "d e f", "g h i"});
    const pdf::PdfTextPage right = makePage({"a b c", "d X f", "g h i"});
    editor::DiffLimits limits;
    limits.maxTokens = 4;
    std::vector<ComparisonChange> changes;
    const auto page = editor::comparePageText(&left, &right, 0, changes, limits);
    CHECK(page.granularity == editor::DiffGranularity::Line);
    CHECK_EQ(changes.size(), std::size_t{1});
    CHECK(changes[0].kind == ChangeKind::Replaced);
    CHECK_NEAR(changes[0].leftRect.minY(), 20.0, 0.01);
}

namespace {

pdf::PdfBuildPage textPage(const std::vector<std::string>& lines) {
    pdf::PdfBuildPage page;
    double y = 700.0;
    for (const std::string& line : lines) {
        pdf::PdfBuildText text;
        text.fontSize = 14.0;
        text.origin = pdf::PdfPoint{72.0, y};
        text.utf8 = line;
        page.items.push_back(text);
        y -= 40.0;
    }
    return page;
}

class StringSink final : public pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        bytes.append(static_cast<const char*>(data), size);
        return core::ok();
    }
    std::string bytes;
};

std::filesystem::path writePdf(pdf::PdfEngine& engine, const std::string& name,
                               const std::vector<std::vector<std::string>>& pages) {
    pdf::PdfBuildDocument document;
    for (const auto& lines : pages) document.pages.push_back(textPage(lines));
    StringSink sink;
    CHECK(engine.buildDocument(document, sink).has_value());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        std::fwrite(sink.bytes.data(), 1, sink.bytes.size(), file);
        std::fclose(file);
    }
    return path;
}

} // namespace

RIVET_TEST(compareDocumentsOverTwoGeneratedPdfs) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    // The first open initializes the library (building text needs it): do it
    // with a fixture so the test does not depend on test order.
    (void)engine->openDocument(std::filesystem::path(RIVET_EDITOR_PDF_FIXTURE_DIR) / "markers-5.pdf");

    const auto leftPath = writePdf(*engine, "rivet-compare-left.pdf",
                                   {{"The quick brown fox", "jumps over"}, {"unchanged page"}, {"only left"}});
    const auto rightPath = writePdf(*engine, "rivet-compare-right.pdf",
                                    {{"The quick red fox", "jumps over"}, {"unchanged page"}});

    core::TaskScheduler scheduler{2};
    auto left = editor::DocumentSession::create(*engine, scheduler, nullptr, leftPath);
    auto right = editor::DocumentSession::create(*engine, scheduler, nullptr, rightPath);
    CHECK(left.has_value());
    CHECK(right.has_value());
    if (!left.has_value() || !right.has_value()) return;

    const editor::ComparisonResult result = editor::compareDocuments(
        (*left)->pageSnapshot(), (*left)->textService(), (*right)->pageSnapshot(),
        (*right)->textService());
    CHECK(!result.cancelled);
    CHECK_EQ(result.leftPageCount, std::size_t{3});
    CHECK_EQ(result.rightPageCount, std::size_t{2});
    CHECK_EQ(result.extractionFailures, std::size_t{0});
    CHECK_EQ(result.pages.size(), std::size_t{2});
    CHECK_EQ(result.changes.size(), std::size_t{2});
    if (result.changes.size() != 2) return;

    const ComparisonChange& word = result.changes[0];
    CHECK(word.kind == ChangeKind::Replaced);
    CHECK_EQ(word.leftPage, std::size_t{0});
    CHECK_EQ(word.leftSnippet, std::string("brown"));
    CHECK_EQ(word.rightSnippet, std::string("red"));
    CHECK(!word.leftRect.isEmpty());
    CHECK(!word.rightRect.isEmpty());
    // The highlight lies on the first text line of the page (top area) and
    // inside the page.
    CHECK_GT(word.leftRect.minX(), 72.0);
    CHECK_LT(word.leftRect.maxY(), 200.0);
    // "brown" is wider than "red" in the same font.
    CHECK_GT(word.leftRect.size.width, word.rightRect.size.width);
    CHECK_EQ(result.pages[0].deletedRects.size(), std::size_t{1});
    CHECK_EQ(result.pages[0].insertedRects.size(), std::size_t{1});
    CHECK(result.pages[1].deletedRects.empty());

    const ComparisonChange& extra = result.changes[1];
    CHECK(extra.kind == ChangeKind::PageDeleted);
    CHECK_EQ(extra.leftPage, std::size_t{2});
    CHECK_EQ(extra.pageCount, std::size_t{1});

    // The same document against itself is identical.
    const editor::ComparisonResult same = editor::compareDocuments(
        (*left)->pageSnapshot(), (*left)->textService(), (*left)->pageSnapshot(),
        (*left)->textService());
    CHECK(same.identical());

    // Cancellation is honored.
    const editor::ComparisonResult cancelled = editor::compareDocuments(
        (*left)->pageSnapshot(), (*left)->textService(), (*right)->pageSnapshot(),
        (*right)->textService(), [] { return true; });
    CHECK(cancelled.cancelled);
}
