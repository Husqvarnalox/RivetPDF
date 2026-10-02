// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "editor/TextBlocks.hpp"
#include "pdf/PdfContent.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Text block reconstruction and capability classification on synthetic
// PdfPageContent (ADR-0014 table, ADR-0015 thresholds).

using namespace rivet;
using namespace rivet::editor;

namespace {

pdf::PdfContentObject makeText(std::uint32_t index, double x, double y, double size, const std::string& text,
                               double width, double angle = 0.0) {
    pdf::PdfContentObject o;
    o.index = index;
    o.origin.kind = pdf::PdfContentOrigin::Kind::Source;
    o.origin.sourceIndex = index;
    o.type = pdf::PdfContentObjectType::Text;
    o.matrix = core::Matrix::rotation(angle);
    o.matrix.tx = x;
    o.matrix.ty = y;
    o.text = text;
    o.fontSize = size;
    o.font.embedded = true;
    o.font.subset = false;
    o.fill = pdf::PdfColor{};
    const core::Point local[4] = {{0.0, -0.2 * size}, {width, -0.2 * size}, {width, 0.8 * size}, {0.0, 0.8 * size}};
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (std::size_t i = 0; i < 4; ++i) {
        const core::Point p = o.matrix.map(local[i]);
        o.quad[i] = pdf::PdfPoint{p.x, p.y};
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    o.bounds = pdf::PdfBox{minX, minY, maxX, maxY};
    return o;
}

pdf::PdfContentObject makeImage(std::uint32_t index) {
    pdf::PdfContentObject o;
    o.index = index;
    o.origin.sourceIndex = index;
    o.type = pdf::PdfContentObjectType::Image;
    o.matrix = core::Matrix{100.0, 0.0, 0.0, 100.0, 300.0, 300.0};
    o.bounds = pdf::PdfBox{300.0, 300.0, 400.0, 400.0};
    o.quad = {pdf::PdfPoint{300.0, 300.0}, pdf::PdfPoint{400.0, 300.0}, pdf::PdfPoint{400.0, 400.0},
              pdf::PdfPoint{300.0, 400.0}};
    o.pixelWidth = 10;
    o.pixelHeight = 10;
    return o;
}

std::vector<ReconstructedBlock> reconstruct(const pdf::PdfPageContent& content) {
    return reconstructTextBlocks(content, classifyObjects(content));
}

// A two-word line "Hello world" starting at (72, y): words 30 and 33 wide, gap 4.
void addLine(pdf::PdfPageContent& content, double y, double size = 12.0, double x = 72.0) {
    const auto at = static_cast<std::uint32_t>(content.objects.size());
    content.objects.push_back(makeText(at, x, y, size, "Hello", 30.0));
    content.objects.push_back(makeText(at + 1, x + 34.0, y, size, "world", 33.0));
}

} // namespace

RIVET_TEST(TextBlocks_multi_line_paragraph_becomes_one_block) {
    pdf::PdfPageContent content;
    addLine(content, 700.0);
    addLine(content, 685.6);
    addLine(content, 671.2);
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{1});
    const ReconstructedBlock& b = blocks[0];
    CHECK_EQ(b.lines.size(), std::size_t{3});
    CHECK_EQ(b.lines[0].members.size(), std::size_t{2});
    CHECK_EQ(b.lines[0].text, std::string("Hello world"));
    CHECK_EQ(b.text, std::string("Hello world\nHello world\nHello world"));
    CHECK_NEAR(b.lineAdvance, 14.4, 1e-9);
    CHECK_NEAR(b.fontSize, 12.0, 1e-12);
    CHECK_NEAR(b.angleRadians, 0.0, 1e-12);
    // Widest line = 34 + 33 = 67 plus a little slack.
    CHECK_GT(b.wrapWidth, 67.0);
    CHECK_LT(b.wrapWidth, 80.0);
    CHECK(b.capability == ContentCapability::FullyEditable);
    CHECK_EQ(b.tag, std::uint64_t{0});
    CHECK_EQ(b.firstMember(), std::uint32_t{0});
    CHECK_NEAR(b.lines[1].baselineStart.y, 685.6, 1e-9);
    CHECK_NEAR(b.lines[0].baselineEnd.x - b.lines[0].baselineStart.x, 67.0, 1e-9);
    CHECK_NEAR(b.bounds.left, 72.0, 1e-9);
    CHECK_NEAR(b.bounds.right, 72.0 + 67.0, 1e-9);
}

RIVET_TEST(TextBlocks_single_line_has_default_advance_and_no_wrap) {
    pdf::PdfPageContent content;
    addLine(content, 700.0, 10.0);
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK_EQ(blocks[0].lines.size(), std::size_t{1});
    CHECK_NEAR(blocks[0].lineAdvance, 12.0, 1e-9);
    CHECK_NEAR(blocks[0].wrapWidth, 0.0, 1e-12);
}

RIVET_TEST(TextBlocks_word_spacing_is_inferred_from_the_gap) {
    pdf::PdfPageContent content;
    content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "Hel", 20.0));
    content.objects.push_back(makeText(1, 92.0, 700.0, 12.0, "lo", 14.0));  // touching: no space
    content.objects.push_back(makeText(2, 110.0, 700.0, 12.0, "you", 20.0)); // 4 pt gap: space
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK_EQ(blocks[0].lines[0].text, std::string("Hello you"));
}

RIVET_TEST(TextBlocks_far_apart_objects_on_one_baseline_split_into_lines_and_blocks) {
    pdf::PdfPageContent content;
    content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "Left", 30.0));
    content.objects.push_back(makeText(1, 300.0, 700.0, 12.0, "Right", 30.0)); // gap 198 > 1.5 x size
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{2});
    CHECK_EQ(blocks[0].text, std::string("Left"));
    CHECK_EQ(blocks[1].text, std::string("Right"));
}

RIVET_TEST(TextBlocks_two_columns_are_not_merged) {
    {
        // Column after column.
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 685.6);
        addLine(content, 700.0, 12.0, 320.0);
        addLine(content, 685.6, 12.0, 320.0);
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{2});
        CHECK_EQ(blocks[0].lines.size(), std::size_t{2});
        CHECK_EQ(blocks[1].lines.size(), std::size_t{2});
        CHECK_NEAR(blocks[0].bounds.left, 72.0, 1e-9);
        CHECK_NEAR(blocks[1].bounds.left, 320.0, 1e-9);
    }
    {
        // Line by line across the columns.
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 700.0, 12.0, 320.0);
        addLine(content, 685.6);
        addLine(content, 685.6, 12.0, 320.0);
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{4});
        for (const ReconstructedBlock& block : blocks) CHECK_EQ(block.lines.size(), std::size_t{1});
    }
}

RIVET_TEST(TextBlocks_mixed_sizes_split) {
    {
        pdf::PdfPageContent content;
        addLine(content, 700.0, 12.0);
        addLine(content, 680.0, 18.0); // different size, next line
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{2});
    }
    {
        // Same baseline, sizes 12 and 18: two lines, two blocks.
        pdf::PdfPageContent content;
        content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "small", 30.0));
        content.objects.push_back(makeText(1, 104.0, 700.0, 18.0, "large", 45.0));
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{2});
    }
    {
        // Within 5 %: same line.
        pdf::PdfPageContent content;
        content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "a", 6.0));
        content.objects.push_back(makeText(1, 80.0, 700.0, 12.3, "b", 6.0));
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{1});
        CHECK_EQ(blocks[0].lines.size(), std::size_t{1});
        CHECK_EQ(blocks[0].lines[0].members.size(), std::size_t{2});
    }
}

RIVET_TEST(TextBlocks_baseline_step_limits) {
    {
        // 14.4, 14.4 then 30 (> 2.2 x size): the third line starts a new block.
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 685.6);
        addLine(content, 655.6);
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{2});
        CHECK_EQ(blocks[0].lines.size(), std::size_t{2});
        CHECK_EQ(blocks[1].lines.size(), std::size_t{1});
    }
    {
        // Inconsistent steps (14.4 then 20 is more than 25 % off).
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 685.6);
        addLine(content, 665.6);
        const auto blocks = reconstruct(content);
        CHECK_EQ(blocks.size(), std::size_t{2});
    }
    {
        // Too tight (< 0.8 x size).
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 693.0);
        CHECK_EQ(reconstruct(content).size(), std::size_t{2});
    }
    {
        // Not left aligned (indent of 20 > 1 x size).
        pdf::PdfPageContent content;
        addLine(content, 700.0);
        addLine(content, 685.6, 12.0, 92.0);
        CHECK_EQ(reconstruct(content).size(), std::size_t{2});
    }
}

RIVET_TEST(TextBlocks_rotated_text_forms_blocks_along_its_own_axes) {
    const double angle = std::numbers::pi / 2.0; // reads bottom to top
    pdf::PdfPageContent content;
    // Baselines run along +y; the next line is one step to the right (+x = down in the text frame).
    content.objects.push_back(makeText(0, 100.0, 100.0, 12.0, "Hello", 30.0, angle));
    content.objects.push_back(makeText(1, 100.0, 134.0, 12.0, "world", 33.0, angle));
    content.objects.push_back(makeText(2, 114.4, 100.0, 12.0, "Again", 30.0, angle));
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{1});
    const ReconstructedBlock& b = blocks[0];
    CHECK_EQ(b.lines.size(), std::size_t{2});
    CHECK_EQ(b.lines[0].text, std::string("Hello world"));
    CHECK_NEAR(b.angleRadians, angle, 1e-9);
    CHECK_NEAR(b.lineAdvance, 14.4, 1e-9);
    CHECK_NEAR(b.lines[0].baselineEnd.y - b.lines[0].baselineStart.y, 67.0, 1e-9);
    CHECK_NEAR(b.lines[0].baselineEnd.x, 100.0, 1e-9);
    // The oriented frame hugs the glyph boxes: 67 along the baseline, 14.4 + 12 across.
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const pdf::PdfPoint& p : b.frame) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    CHECK_NEAR(maxY - minY, 67.0, 1e-9);
    CHECK_NEAR(maxX - minX, 14.4 + 12.0, 1e-9);
}

RIVET_TEST(TextBlocks_small_rotation_differences_join_large_ones_do_not) {
    const double degrees = std::numbers::pi / 180.0;
    {
        pdf::PdfPageContent content;
        content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "a", 6.0, 0.0));
        content.objects.push_back(makeText(1, 80.0, 700.0, 12.0, "b", 6.0, 0.3 * degrees));
        CHECK_EQ(reconstruct(content)[0].lines[0].members.size(), std::size_t{2});
    }
    {
        pdf::PdfPageContent content;
        content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "a", 6.0, 0.0));
        content.objects.push_back(makeText(1, 80.0, 700.0, 12.0, "b", 6.0, 1.0 * degrees));
        CHECK_EQ(reconstruct(content).size(), std::size_t{2});
    }
}

RIVET_TEST(TextBlocks_tagged_objects_form_exactly_their_edit_block) {
    pdf::PdfPageContent content;
    content.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "Edited line one", 90.0));
    content.objects.push_back(makeText(1, 72.0, 500.0, 12.0, "Unrelated", 50.0));
    content.objects.push_back(makeText(2, 72.0, 650.0, 12.0, "Edited line two", 90.0)); // far below: still the block
    for (std::size_t i : {0u, 2u}) {
        content.objects[i].blockTag = 7;
        content.objects[i].origin.kind = pdf::PdfContentOrigin::Kind::Created;
        content.objects[i].origin.tag = 7;
    }
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{2});
    CHECK_EQ(blocks[0].tag, std::uint64_t{7});
    CHECK_EQ(blocks[0].lines.size(), std::size_t{2});
    CHECK_EQ(blocks[0].lines[0].members.size(), std::size_t{1});
    CHECK_EQ(blocks[0].firstMember(), std::uint32_t{0});
    CHECK_EQ(blocks[0].text, std::string("Edited line one\nEdited line two"));
    CHECK_NEAR(blocks[0].lineAdvance, 50.0, 1e-9);
    CHECK_EQ(blocks[1].tag, std::uint64_t{0});
    CHECK_EQ(blocks[1].text, std::string("Unrelated"));
}

RIVET_TEST(TextBlocks_tagged_objects_break_untagged_neighbours) {
    pdf::PdfPageContent content;
    addLine(content, 700.0);
    content.objects.push_back(makeText(2, 72.0, 685.6, 12.0, "tagged", 40.0));
    content.objects[2].blockTag = 3;
    addLine(content, 671.2);
    // The tagged object between two otherwise adjacent lines is not merged away.
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{3});
}

RIVET_TEST(TextBlocks_invisible_ocr_text_stays_single_object_read_only_blocks) {
    pdf::PdfPageContent content;
    for (std::uint32_t i = 0; i < 3; ++i) {
        content.objects.push_back(makeText(i, 72.0, 700.0 - 14.4 * i, 12.0, "ocr", 20.0));
        content.objects.back().renderMode = 3;
    }
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{3});
    for (const ReconstructedBlock& b : blocks) {
        CHECK_EQ(b.lines.size(), std::size_t{1});
        CHECK(b.capability == ContentCapability::ReadOnly);
        CHECK(!b.capabilityReason.empty());
    }
    // Visible text next to invisible text is not merged into it.
    pdf::PdfPageContent mixed;
    mixed.objects.push_back(makeText(0, 72.0, 700.0, 12.0, "seen", 30.0));
    mixed.objects.push_back(makeText(1, 106.0, 700.0, 12.0, "hidden", 30.0));
    mixed.objects.back().renderMode = 3;
    CHECK_EQ(reconstruct(mixed).size(), std::size_t{2});
}

RIVET_TEST(TextBlocks_non_text_objects_are_ignored_and_order_follows_content) {
    pdf::PdfPageContent content;
    content.objects.push_back(makeImage(0));
    addLine(content, 700.0); // 1, 2
    content.objects.push_back(makeImage(3));
    addLine(content, 685.6); // 4, 5
    content.objects.push_back(makeText(6, 72.0, 300.0, 12.0, "Later", 30.0));
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{2});
    CHECK_EQ(blocks[0].firstMember(), std::uint32_t{1});
    CHECK_EQ(blocks[0].lines.size(), std::size_t{2});
    CHECK_EQ(blocks[1].firstMember(), std::uint32_t{6});
}

RIVET_TEST(TextBlocks_reconstruction_is_deterministic) {
    pdf::PdfPageContent content;
    for (int i = 0; i < 6; ++i) addLine(content, 700.0 - 14.4 * i);
    const auto first = reconstruct(content);
    const auto second = reconstruct(content);
    CHECK_EQ(first.size(), second.size());
    CHECK_EQ(first[0].text, second[0].text);
    CHECK_EQ(first[0].lines.size(), std::size_t{6});
    CHECK_EQ(first[0].wrapWidth, second[0].wrapWidth);
    CHECK(reconstruct(pdf::PdfPageContent{}).empty());
}

RIVET_TEST(TextBlocks_capability_table_for_text) {
    const auto fresh = [] { return makeText(0, 72.0, 700.0, 12.0, "x", 6.0); };
    {
        auto o = fresh();
        const auto info = classifyObject(o);
        CHECK(info.capability == ContentCapability::FullyEditable); // embedded, not a subset
        CHECK(info.reason.empty());
    }
    {
        auto o = fresh();
        o.font.embedded = false;
        o.font.standard14 = true;
        CHECK(classifyObject(o).capability == ContentCapability::FullyEditable);
    }
    {
        auto o = fresh();
        o.font.subset = true;
        const auto info = classifyObject(o);
        CHECK(info.capability == ContentCapability::Replaceable);
        CHECK(!info.reason.empty());
    }
    {
        auto o = fresh();
        o.font.embedded = false;
        CHECK(classifyObject(o).capability == ContentCapability::Replaceable);
    }
    {
        auto o = fresh();
        o.fontSubstituted = true;
        CHECK(classifyObject(o).capability == ContentCapability::Replaceable);
    }
    {
        auto o = fresh();
        o.font.type3 = true;
        CHECK(classifyObject(o).capability == ContentCapability::MoveOnly);
    }
    {
        auto o = fresh();
        o.textUnmappable = true;
        CHECK(classifyObject(o).capability == ContentCapability::MoveOnly);
    }
    {
        auto o = fresh();
        o.hasClip = true;
        CHECK(classifyObject(o).capability == ContentCapability::MoveOnly);
    }
    {
        auto o = fresh();
        o.matrix.c = 0.4; // skewed
        CHECK(classifyObject(o).capability == ContentCapability::MoveOnly);
    }
    {
        auto o = fresh();
        o.matrix.d = -1.0; // mirrored
        CHECK(classifyObject(o).capability == ContentCapability::MoveOnly);
    }
    {
        auto o = fresh();
        o.renderMode = 3;
        CHECK(classifyObject(o).capability == ContentCapability::ReadOnly);
        o.renderMode = 7;
        CHECK(classifyObject(o).capability == ContentCapability::ReadOnly);
        o.renderMode = 1; // stroked text stays editable
        CHECK(classifyObject(o).capability == ContentCapability::FullyEditable);
    }
    {
        auto o = fresh();
        o.fontSize = std::nan("");
        CHECK(classifyObject(o).capability == ContentCapability::ReadOnly);
    }
}

RIVET_TEST(TextBlocks_capability_table_for_other_objects) {
    pdf::PdfContentObject o = makeImage(0);
    CHECK(classifyObject(o).capability == ContentCapability::FullyEditable);
    o.hasClip = true;
    CHECK(classifyObject(o).capability == ContentCapability::FullyEditable);
    for (auto type : {pdf::PdfContentObjectType::Path, pdf::PdfContentObjectType::Form,
                      pdf::PdfContentObjectType::Shading}) {
        o.type = type;
        const auto info = classifyObject(o);
        CHECK(info.capability == ContentCapability::MoveOnly);
        CHECK(!info.reason.empty());
    }
    o.type = pdf::PdfContentObjectType::Unknown;
    CHECK(classifyObject(o).capability == ContentCapability::ReadOnly);
}

RIVET_TEST(TextBlocks_page_wide_restrictions_make_everything_read_only) {
    pdf::PdfPageContent content;
    addLine(content, 700.0);
    content.objects.push_back(makeImage(2));
    CHECK(!pageReadOnlyReason(content).has_value());

    content.regenerationSafe = false;
    content.regenerationIssue = "uses an unsupported operator";
    const auto reason = pageReadOnlyReason(content);
    CHECK(reason.has_value());
    CHECK(reason->find("unsupported operator") != std::string::npos);
    const auto classes = classifyObjects(content);
    CHECK_EQ(classes.size(), content.objects.size());
    for (const ObjectCapabilityInfo& info : classes) {
        CHECK(info.capability == ContentCapability::ReadOnly);
        CHECK(!info.reason.empty());
    }
    // Blocks are still reconstructed (for display), but read-only.
    const auto blocks = reconstructTextBlocks(content, classes);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK(blocks[0].capability == ContentCapability::ReadOnly);

    pdf::PdfPageContent truncated;
    truncated.truncated = true;
    CHECK(pageReadOnlyReason(truncated).has_value());
}

RIVET_TEST(TextBlocks_block_capability_is_the_weakest_member) {
    pdf::PdfPageContent content;
    addLine(content, 700.0);
    content.objects[1].font.subset = true; // Replaceable
    const auto blocks = reconstruct(content);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK(blocks[0].capability == ContentCapability::Replaceable);
    CHECK(!blocks[0].capabilityReason.empty());
}
