// SPDX-License-Identifier: MPL-2.0
#include "editor/OcrLayer.hpp"

#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFonts.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::editor {

namespace {

// Where the word box's bottom edge sits relative to the baseline, as a
// fraction of the box height: descenders hang below the baseline.
constexpr double kBaselineLift = 0.2;
// Boxes smaller than this (display points) carry no usable text.
constexpr double kMinBoxSide = 0.5;

bool finite(const core::Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); }

bool printable(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return false;
    if (c == 0x2028 || c == 0x2029 || c == 0x00A0 || c == 0x200B || c == 0xFEFF) return false;
    return c != U' ';
}

} // namespace

OcrWord ocrWordFromBitmap(std::string text, const std::array<core::Point, 4>& bitmapQuad,
                          std::uint32_t bitmapWidth, std::uint32_t bitmapHeight,
                          const core::Size& displaySize) {
    OcrWord word;
    word.text = std::move(text);
    const double sx = bitmapWidth == 0 ? 0.0 : displaySize.width / static_cast<double>(bitmapWidth);
    const double sy = bitmapHeight == 0 ? 0.0 : displaySize.height / static_cast<double>(bitmapHeight);
    for (std::size_t i = 0; i < 4; ++i) {
        word.quad[i] = core::Point{bitmapQuad[i].x * sx, bitmapQuad[i].y * sy};
    }
    return word;
}

InvisibleTextPlan planInvisibleText(const pdf::PdfPageView& view, std::span<const OcrWord> words) {
    InvisibleTextPlan plan;
    const auto inverse = pdf::userToDisplayMatrix(view).inverted();
    if (!inverse.has_value()) {
        plan.skipped = words.size();
        return plan;
    }
    const pdf::BundledFontMetrics& metrics = pdf::bundledFontMetrics(pdf::PdfBundledFont::SansRegular);

    for (const OcrWord& word : words) {
        if (plan.runs.size() >= pdf::kMaxInvisibleRunsPerPage) {
            ++plan.skipped;
            continue;
        }
        const core::Point topLeft = word.quad[0];
        const core::Point bottomRight = word.quad[2];
        const core::Point bottomLeft = word.quad[3];
        const core::Point baseline = word.quad[2] - word.quad[3];
        const double width = std::hypot(baseline.x, baseline.y);
        const double height = std::hypot((topLeft - bottomLeft).x, (topLeft - bottomLeft).y);
        if (!finite(topLeft) || !finite(bottomRight) || !finite(bottomLeft) || !(width >= kMinBoxSide) ||
            !(height >= kMinBoxSide)) {
            ++plan.skipped;
            continue;
        }

        std::u32string kept;
        for (const char32_t c : pdf::decodeUtf8(word.text)) {
            if (printable(c) && pdf::bundledFontCovers(pdf::PdfBundledFont::SansRegular, c)) kept.push_back(c);
        }
        std::string text = pdf::encodeUtf8(kept);
        if (text.empty() || text.size() > pdf::kMaxInvisibleRunBytes) {
            ++plan.skipped;
            continue;
        }

        const double fontSize = std::clamp(height, pdf::kMinFontSize, pdf::kMaxFontSize);
        const double natural = metrics.textWidth(text, fontSize);
        if (!(natural > 0.0)) {
            ++plan.skipped;
            continue;
        }
        const double scale = width / natural;
        if (!(scale >= pdf::kMinInvisibleHorizontalScale) || !(scale <= pdf::kMaxInvisibleHorizontalScale)) {
            ++plan.skipped;
            continue;
        }

        // Frame (x along the baseline, y up) -> display, then -> user space.
        // `up` is the baseline direction turned a quarter turn towards the
        // top of the (y-down) display.
        const double cx = baseline.x / width;
        const double cy = baseline.y / width;
        const core::Point origin{bottomLeft.x + cy * kBaselineLift * height,
                                 bottomLeft.y - cx * kBaselineLift * height};
        const core::Matrix frameToDisplay{cx, cy, cy, -cx, origin.x, origin.y};

        pdf::PdfInvisibleTextRun run;
        run.text = std::move(text);
        run.placement = *inverse * frameToDisplay;
        run.fontSize = fontSize;
        run.horizontalScale = scale;
        plan.runs.push_back(std::move(run));
    }
    return plan;
}

} // namespace rivet::editor
