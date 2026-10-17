// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "ui/TextElision.hpp"

#include <string>
#include <string_view>

namespace {

// One unit per code point (the ellipsis counts as one).
double measureCodePoints(std::string_view text) {
    double count = 0.0;
    for (const char byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) count += 1.0;
    }
    return count;
}

constexpr const char* kEllipsis = "\xE2\x80\xA6";

} // namespace

RIVET_TEST(elideMiddleReturnsFittingTextUnchanged) {
    CHECK(rivet::ui::elideMiddle("short", 5.0, measureCodePoints) == "short");
    CHECK(rivet::ui::elideMiddle("", 0.0, measureCodePoints).empty());
}

RIVET_TEST(elideMiddleKeepsHeadAndTailWithinTheBudget) {
    const std::string path = "/Users/someone/Documents/projects/rivet/specs";
    for (double width : {10.0, 15.0, 20.0, 30.0}) {
        const std::string out = rivet::ui::elideMiddle(path, width, measureCodePoints);
        CHECK(measureCodePoints(out) <= width);
        CHECK(out.find(kEllipsis) != std::string::npos);
        CHECK(out.front() == '/');          // head kept
        CHECK(out.back() == 's');           // tail kept
    }
    // Fills the budget exactly: 11 kept + ellipsis = 12.
    CHECK(measureCodePoints(rivet::ui::elideMiddle(path, 12.0, measureCodePoints)) == 12.0);
    // Wider budgets keep strictly more.
    CHECK(rivet::ui::elideMiddle(path, 30.0, measureCodePoints).size() >
          rivet::ui::elideMiddle(path, 15.0, measureCodePoints).size());
}

RIVET_TEST(elideMiddleNeverSplitsUtf8AndHandlesTinyBudgets) {
    const std::string text = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC";
    for (double width = 0.0; width <= 9.0; width += 1.0) {
        const std::string out = rivet::ui::elideMiddle(text, width, measureCodePoints);
        // Valid UTF-8: no sequence starts with a stray continuation byte.
        CHECK((static_cast<unsigned char>(out.front()) & 0xC0) != 0x80);
        if (width >= 1.0) CHECK(measureCodePoints(out) <= width);
    }
    // No room even for the ellipsis: just the ellipsis.
    CHECK(rivet::ui::elideMiddle(text, 0.0, measureCodePoints) == kEllipsis);
}
