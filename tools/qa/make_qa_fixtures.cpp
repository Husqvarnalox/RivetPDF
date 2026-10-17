// SPDX-License-Identifier: MPL-2.0
//
// Writes the PDF fixtures of the scripted QA scenarios (tools/qa/scenarios)
// into the directory given as argv[1]. Reuses the deterministic generators of
// the test suite (tests/pdf/PdfFixtures.hpp); no PDF library involved.
//
//   c++ -std=c++2b -I tests/pdf tools/qa/make_qa_fixtures.cpp -o make_qa_fixtures
//   ./make_qa_fixtures build/qa-fixtures

#include "PdfFixtures.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using rivet::test::pdffix::buildPdf;
using rivet::test::pdffix::streamObject;

// One Helvetica line on a US Letter page (612x792): baseline origin + size.
struct Line {
    int x;
    int y;
    int size;
    std::string text;
};

std::string textPdf(const std::vector<std::vector<Line>>& pages) {
    std::vector<std::string> bodies;
    const std::size_t count = pages.size();
    // 1 catalog, 2 pages, 3 font, then (page, content) pairs.
    bodies.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (std::size_t i = 0; i < count; ++i) kids += std::to_string(4 + 2 * i) + " 0 R ";
    bodies.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(count) + " >>");
    bodies.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    for (std::size_t i = 0; i < count; ++i) {
        bodies.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " +
                         std::to_string(5 + 2 * i) + " 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
        std::string content;
        for (const Line& line : pages[i]) {
            content += "BT /F1 " + std::to_string(line.size) + " Tf " + std::to_string(line.x) + " " +
                       std::to_string(line.y) + " Td (" + line.text + ") Tj ET\n";
        }
        bodies.push_back(streamObject("", content));
    }
    return buildPdf(bodies);
}

bool write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_qa_fixtures <out-dir>\n");
        return 2;
    }
    const std::filesystem::path dir(argv[1]);
    std::filesystem::create_directories(dir);

    // sample.pdf: 4 pages with text at known user-space positions (the
    // scenario coordinates depend on them: see tools/qa/scenarios).
    std::vector<std::vector<Line>> sample;
    for (int page = 1; page <= 4; ++page) {
        sample.push_back({
            {72, 720, 28, "Rivet QA sample page " + std::to_string(page)},
            {72, 660, 14, "The quick brown fox jumps over the lazy dog."},
            {72, 630, 14, "SECRETWORD appears on this line for redaction."},
            {72, 600, 14, "Annotate this sentence with a highlight please."},
            {72, 560, 12, "Lorem ipsum dolor sit amet, consectetur adipiscing elit."},
            {72, 540, 12, "Sed do eiusmod tempor incididunt ut labore et dolore."},
        });
    }
    // compare-a/b: the same two pages, differing in a few words.
    const std::vector<std::vector<Line>> compareA{
        {{72, 720, 20, "Comparison document"},
         {72, 680, 14, "The quick brown fox jumps over the lazy dog."},
         {72, 650, 14, "Payment is due within thirty days of delivery."}},
        {{72, 720, 20, "Second page"}, {72, 680, 14, "Nothing changes on this line."}},
    };
    const std::vector<std::vector<Line>> compareB{
        {{72, 720, 20, "Comparison document"},
         {72, 680, 14, "The quick red fox leaps over the lazy dog."},
         {72, 650, 14, "Payment is due within sixty days of delivery."}},
        {{72, 720, 20, "Second page"}, {72, 680, 14, "Nothing changes on this line."}},
    };
    // scan-source: one big-type page that is rasterized into the OCR input.
    const std::vector<std::vector<Line>> scanSource{
        {{72, 700, 36, "Scanned invoice 2026"},
         {72, 640, 24, "Total amount due: 1234 EUR"},
         {72, 590, 24, "Recognize this text please"}},
    };

    bool ok = true;
    ok = write(dir / "sample.pdf", textPdf(sample)) && ok;
    ok = write(dir / "compare-a.pdf", textPdf(compareA)) && ok;
    ok = write(dir / "compare-b.pdf", textPdf(compareB)) && ok;
    ok = write(dir / "scan-source.pdf", textPdf(scanSource)) && ok;
    ok = write(dir / "form.pdf", rivet::test::pdffix::formPdf()) && ok;
    ok = write(dir / "rich.pdf", rivet::test::pdffix::richPdf()) && ok;
    return ok ? 0 : 1;
}
